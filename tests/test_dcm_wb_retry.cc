// Deterministic tests for the WB -> far-memory write acceptance/retry
// fix (docs/wb_retry_audit.md).
//
// BACKGROUND: the audit found that MEMORY_CONTROLLER::add_wq()/add_rq()
// (dram_controller.cc) have no bounds check at all -- if their
// destination queue is genuinely full, the packet is silently discarded
// with the SAME return value (-1) as success, with zero signal to the
// caller. DRAM_CACHE_MANAGER::drainWB()/dispatchToFar() were
// fire-and-forget and inherited full exposure to this. The fix
// (src/dram_cache_manager.cc): dispatchToFar() now checks
// farMC->get_occupancy() against farMC->get_size() BEFORE ever calling
// add_rq()/add_wq(), and returns false (nothing sent, nothing queued) if
// there is no room. drainWB() only pops WB.front() AFTER dispatchToFar()
// reports success; on failure the entry is retained and retried on a
// later operate() cycle. processPendingFarDispatches() applies the
// identical check at link-latency release time, for symmetry, and stops
// (does not skip ahead) on the first blocked entry, preserving FIFO
// order.
//
// gem5 correspondence (see file banner-length comment on gem5 vs this
// port near the bottom of this file, and docs/wb_retry_audit.md for the
// full source-level comparison): gem5's PolicyManager::pktFarMemWrite
// front entry is popped ONLY when farReqPort.sendTimingReq() returns
// true; on false, retryFarMemWrite is set and the SAME front entry is
// retried once gem5's async port framework calls
// farMemRecvReqRetry(). ChampSim has no equivalent async port-retry
// callback -- this port achieves the identical OUTCOME (retain until
// accepted, retry every subsequent cycle, preserve order) via
// synchronous per-cycle polling instead: drainWB()/
// processPendingFarDispatches() are already called once per operate()
// cycle regardless, so simply not popping on failure IS the retry --
// no flag, no callback, no dram_controller.cc change needed.
//
// Proves:
//   1) Far queue has capacity -> WB write accepted -> WB entry removed.
//   2) Far queue is full -> WB write NOT removed -> no write lost.
//   3) Queue becomes available -> WB write succeeds on a later cycle ->
//      WB entry removed exactly once (no duplicate dispatch).
//   4) Multiple WB entries -> no entry lost, no entry duplicated.
//   5) Sustained write pressure (stress test): enough dirty evictions to
//      exceed farMC's WQ capacity -- proves
//      "WB entries generated == far-memory writes accepted",
//      lost writes = 0, duplicated writes = 0. Repeats a controlled
//      version of the exact failure case documented in
//      docs/wb_retry_audit.md.
//   6) Mixed WB traffic and normal far reads/writes: far-read dispatches
//      (cold misses) and far-write dispatches (WB drains) share farMC
//      without one starving or corrupting the other.
//   7) Ordering is preserved: WB entries drain to farMC in the exact
//      order they were inserted, even when some are retried.
//   8) Parent/request accounting remains correct: the ORB entry for the
//      EVICTING request itself completes (LLC gets its response)
//      regardless of whether its victim's write-back is immediately
//      accepted or retried -- WB retry blocking must not stall unrelated
//      ORB entries.
//   9) WB occupancy statistics remain correct while entries are
//      retained/retried (wbMaxOccupancy, wbDispatchRetries, wbDrains,
//      wbInsertions all reconcile exactly).
//
// Build:
//   g++ -std=c++11 -Iinc -o /tmp/test_dcm_wb_retry \
//       tests/test_dcm_wb_retry.cc src/dram_cache_manager.cc \
//       src/dram_controller.cc src/block.cc

#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "champsim.h"
#include "dram_cache_manager.h"

uint64_t current_core_cycle[NUM_CPUS];
uint8_t all_warmup_complete;

class FAKE_LLC : public MEMORY {
  public:
    std::vector<uint64_t> responses;
    int add_rq(PACKET *packet) { return -1; }
    int add_wq(PACKET *packet) { return -1; }
    int add_pq(PACKET *packet) { return -1; }
    void operate() {}
    void increment_WQ_FULL(uint64_t address) {}
    uint32_t get_occupancy(uint8_t queue_type, uint64_t address) { return 0; }
    uint32_t get_size(uint8_t queue_type, uint64_t address) { return 1; }
    void return_data(PACKET *packet) { responses.push_back(packet->address); }
};

// Records the exact order (and full multiset) of addresses the DCM
// hands to add_wq()/add_rq(), WITHOUT altering their behavior at all
// (records BEFORE delegating to the real MEMORY_CONTROLLER logic).
// This lets tests directly observe dispatch order/count from OUTSIDE
// the DCM, rather than inferring it indirectly -- the cleanest way to
// verify "no loss, no duplication, FIFO order" against ground truth.
class ORDER_TRACKING_MC : public MEMORY_CONTROLLER {
  public:
    std::vector<uint64_t> wqArrivalOrder;
    std::vector<uint64_t> rqArrivalOrder;
    // When true, get_occupancy()/get_size() report the WQ as permanently
    // full (occupancy >= size), regardless of its real state -- used by
    // tests that need GUARANTEED, timing-independent blocking (no race
    // against real DRAM completion draining a natural pre-fill faster
    // than the test can observe the "still blocked" state).
    bool forceWqFull;
    explicit ORDER_TRACKING_MC(std::string name) : MEMORY_CONTROLLER(name), forceWqFull(false) {}
    int add_wq(PACKET *packet)
    {
        wqArrivalOrder.push_back(packet->address);
        return MEMORY_CONTROLLER::add_wq(packet);
    }
    int add_rq(PACKET *packet)
    {
        rqArrivalOrder.push_back(packet->address);
        return MEMORY_CONTROLLER::add_rq(packet);
    }
    uint32_t get_occupancy(uint8_t queue_type, uint64_t address)
    {
        if (forceWqFull && queue_type == 2)
            return 1;
        return MEMORY_CONTROLLER::get_occupancy(queue_type, address);
    }
    uint32_t get_size(uint8_t queue_type, uint64_t address)
    {
        if (forceWqFull && queue_type == 2)
            return 1;
        return MEMORY_CONTROLLER::get_size(queue_type, address);
    }
};

// NOTE: both helpers stamp event_cycle explicitly (a real bug found
// while writing this file's Test 7): PACKET's own default constructor
// leaves event_cycle at UINT64_MAX, and MEMORY_CONTROLLER::
// update_schedule_cycle() picks the UNSCHEDULED entry with the SMALLEST
// event_cycle to schedule next -- starting its comparison at
// min_cycle=UINT64_MAX, so an entry stuck at UINT64_MAX can NEVER win
// that comparison (UINT64_MAX < UINT64_MAX is false) and is therefore
// NEVER scheduled, EVER. This only matters for packets submitted
// DIRECTLY to a MEMORY_CONTROLLER (this file's farMC "filler" packets,
// bypassing the DCM entirely) -- packets that go through the DCM's own
// admission path always get event_cycle correctly re-stamped to "now"
// immediately before dispatch (driveState()'s `p.event_cycle = nowCycle`
// convention, and dispatchToFar()'s equivalent for the far side), so
// this was invisible until a test needed to inject packets straight
// into a MEMORY_CONTROLLER's queue.
static PACKET makeReadPacket(uint64_t addr)
{
    PACKET p;
    p.type = LOAD;
    p.address = addr;
    p.full_addr = addr;
    p.cpu = 0;
    p.instruction = 0;
    p.is_data = 1;
    p.event_cycle = current_core_cycle[0];
    return p;
}

static PACKET makeWritePacket(uint64_t addr)
{
    PACKET p;
    p.type = WRITEBACK;
    p.address = addr;
    p.full_addr = addr;
    p.cpu = 0;
    p.instruction = 0;
    p.is_data = 1;
    p.event_cycle = current_core_cycle[0];
    return p;
}

static void standardTiming(MEMORY_CONTROLLER &mc)
{
    uint32_t trp = (uint32_t)((1.0 * tRP_DRAM_NANOSECONDS * CPU_FREQ) / 1000);
    uint32_t trcd = (uint32_t)((1.0 * tRCD_DRAM_NANOSECONDS * CPU_FREQ) / 1000);
    uint32_t tcas = (uint32_t)((1.0 * tCAS_DRAM_NANOSECONDS * CPU_FREQ) / 1000);
    uint32_t mtps = DRAM_IO_FREQ;
    mc.set_timing(trp, trcd, tcas, mtps, computeDbusReturnTime(mtps));
}

struct Harness {
    ORDER_TRACKING_MC near_mc, far_mc;
    DRAM_CACHE_MANAGER dcm;
    FAKE_LLC llc;

    Harness(const std::string &name)
        : near_mc(name + "_N"), far_mc(name + "_F"), dcm(name + "_DCM", &near_mc, &far_mc)
    {
        standardTiming(near_mc);
        standardTiming(far_mc);
        dcm.upper_level_icache[0] = &llc;
        dcm.upper_level_dcache[0] = &llc;
    }
};

static void pump(Harness &h, int cycles)
{
    for (int i = 0; i < cycles; i++) {
        current_core_cycle[0]++;
        h.dcm.operate();
    }
}

// Runs until the WB deque, farMC's WQ, AND the ORB are all empty
// (nothing left in flight anywhere), or the cycle budget runs out. Used
// before taking final measurements, so counts are never a race against
// something still in transit.
//
// IMPORTANT (a real test-methodology bug found and fixed while writing
// this file, same class of issue documented in
// tests/test_dcm_controller_latency.cc's banner comment): checking only
// `WB.empty() && farMC.WQ.occupancy==0` is trivially satisfied the
// INSTANT this function is first called, before the just-issued
// eviction's own tag-check read has even been dispatched, let alone
// completed -- at that point nothing has been pushed to WB or farMC's
// WQ yet, so both conditions read as "done" when in fact the request is
// still sitting in the ORB, not even started. This caused every WB
// stat/dispatch check to observe a stale, pre-work snapshot. Fixed by
// ALSO requiring `dcm.ORB.empty()`, which is never trivially true right
// after issuing a request (admitRequest() adds it to the ORB
// synchronously, before this function's very first operate() call), and
// only becomes true again once every outstanding request has genuinely
// completed.
static bool pumpUntilQuiescent(Harness &h, uint64_t maxCycles)
{
    for (uint64_t i = 0; i < maxCycles; i++) {
        current_core_cycle[0]++;
        h.dcm.operate();
        if (h.dcm.WB.empty() && h.far_mc.WQ[0].occupancy == 0 && h.dcm.ORB.empty())
            return true;
    }
    return false;
}

// Installs a dirty valid line at addr's DRAM-cache index via a write,
// then pumps until that write's ORB entry completes.
static void seedDirtyLine(Harness &h, uint64_t addr)
{
    PACKET w = makeWritePacket(addr);
    h.dcm.add_wq(&w);
    for (int i = 0; i < 500 && !h.dcm.ORB.empty(); i++) {
        current_core_cycle[0]++;
        h.dcm.operate();
    }
}

// Evicts whatever currently occupies addr's DRAM-cache index (a read to
// a different tag, same index) -- if that resident line is dirty, this
// triggers a WB insertion. Does NOT wait for the eviction's own
// completion -- callers pump separately so multiple evictions can be
// issued back-to-back to create real WB backlog pressure.
static void issueEvictingRead(Harness &h, uint64_t evictingAddr)
{
    PACKET r = makeReadPacket(evictingAddr);
    h.dcm.add_rq(&r);
}

// Submits an eviction for every address in evictAddrs, RETRYING any that
// are rejected at admission time (return code -2: CRB full, WB-pressure
// full, or ORB full -- all legitimate, pre-existing, unrelated
// backpressure mechanisms, not the WB-retry bug this file tests). A
// real test bug found while writing the stress test: a single-shot
// issueEvictingRead() loop over many addresses silently treats a
// legitimately-REJECTED admission (which has NO side effects at all --
// no ORB/CRB entry created) as if the eviction had been issued, making
// it vanish from the test's own bookkeeping and produce a false
// "lost write" report that was actually just an admission this test
// never retried, not a write the implementation ever lost. Retries
// (like `dispatchToFar`'s retry) are spaced by real operate() cycles so
// backpressure has a chance to clear, mirroring how a real LLC retries
// admission after checking get_occupancy()==get_size().
//
// SECOND real test bug found while writing the stress test: admitting
// many requests in one uninterrupted burst (zero operate() cycles
// between admissions) causes every one of their tag-check reads to be
// dispatched to nearMC->add_rq() essentially simultaneously
// (driveState()'s DCM_LOC_MEM_READ case calls it directly, with NO
// bounds protection of its own -- the exact same underlying
// MEMORY_CONTROLLER silent-drop bug this file's fix addresses for the
// FAR/write side, but on nearMC's RQ instead, an entirely separate,
// out-of-scope call site per docs/wb_retry_audit.md's note that the
// far-READ path has this identical, separately-tracked exposure -- the
// near tag-check-read path turns out to share it too). Once more than
// DRAM_RQ_SIZE (64) requests are concurrently in flight this way,
// nearMC's RQ silently drops the overflow, which has nothing to do with
// farMC's WQ retry logic this file actually tests. Paced admission
// (checking nearMC's RQ occupancy and waiting for room before each new
// admission) avoids ever exercising that separate bug, keeping this
// file's tests focused on their actual target -- and is also simply
// realistic: a real LLC checks occupancy before calling add_rq too.
static void admitAllEvictions(Harness &h, std::vector<uint64_t> evictAddrs, uint64_t maxCycles)
{
    const uint32_t nearRqSafeLimit = (uint32_t)DRAM_RQ_SIZE / 2;
    uint64_t cycles = 0;
    while (!evictAddrs.empty() && cycles < maxCycles) {
        std::vector<uint64_t> stillPending;
        for (size_t i = 0; i < evictAddrs.size(); i++) {
            if (h.near_mc.RQ[0].occupancy >= nearRqSafeLimit) {
                stillPending.push_back(evictAddrs[i]); // paced -- retry next cycle
                continue;
            }
            PACKET r = makeReadPacket(evictAddrs[i]);
            int rc = h.dcm.add_rq(&r);
            if (rc == -2)
                stillPending.push_back(evictAddrs[i]);
        }
        evictAddrs = stillPending;
        if (!evictAddrs.empty()) {
            current_core_cycle[0]++;
            h.dcm.operate();
            cycles++;
        }
    }
}

int main()
{
    current_core_cycle[0] = 0;
    all_warmup_complete = NUM_CPUS;
    int failures = 0;
    const uint64_t STRIDE = DCM_DRAM_CACHE_SIZE;

    // ==================================================================
    // Test 1: far queue has capacity -> WB write accepted -> WB entry
    // removed.
    // ==================================================================
    {
        Harness h("WBR1");
        uint64_t addr = 0xB000000, evictAddr = addr + STRIDE;
        seedDirtyLine(h, addr);
        issueEvictingRead(h, evictAddr);
        pumpUntilQuiescent(h, 5000);

        bool wbEmpty = h.dcm.WB.empty();
        bool oneDrain = h.dcm.stats.wbDrains == 1;
        bool dispatchedOnce = h.far_mc.wqArrivalOrder.size() == 1 && h.far_mc.wqArrivalOrder[0] == addr;
        bool noRetries = h.dcm.stats.wbDispatchRetries == 0;

        std::cout << "[TEST 1: capacity available] wb_empty=" << wbEmpty << " wbDrains=" << h.dcm.stats.wbDrains
                   << " dispatched_once=" << dispatchedOnce << " retries=" << h.dcm.stats.wbDispatchRetries
                   << std::endl;
        if (!wbEmpty || !oneDrain || !dispatchedOnce || !noRetries) {
            std::cerr << "  FAIL" << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Test 2: far queue is full -> WB write NOT removed -> no write
    // lost.
    // ==================================================================
    {
        Harness h("WBR2");
        uint64_t addr = 0xB100000, evictAddr = addr + STRIDE;
        seedDirtyLine(h, addr);
        issueEvictingRead(h, evictAddr);

        // Pump cycle-by-cycle until the eviction's dirty victim is
        // actually pushed to WB (stats.wbInsertions becomes 1), instead
        // of guessing a fixed cycle count. This is what lets the
        // pre-fill below be fully deterministic and race-free:
        // DRAM_CACHE_MANAGER::operate() calls drainWB() BEFORE
        // nearMC->operate() every cycle, so the cycle where wbInsertions
        // first becomes nonzero is guaranteed to be one where drainWB()
        // has NOT yet had a chance to attempt (let alone succeed at)
        // dispatching this brand-new entry -- there is no possible
        // window for it to have already been sent before we fill farMC
        // immediately below, with zero operate() cycles in between.
        bool wbEntryCreated = false;
        for (int i = 0; i < 2000; i++) {
            current_core_cycle[0]++;
            h.dcm.operate();
            if (h.dcm.stats.wbInsertions >= 1) {
                wbEntryCreated = true;
                break;
            }
        }
        if (!wbEntryCreated) {
            std::cerr << "[TEST 2 SETUP FAIL] WB entry was never created" << std::endl;
            failures++;
        }

        // NOW fill farMC's WQ to capacity -- direct calls, no operate()
        // cycles consumed, so drainWB() cannot run again before this
        // finishes.
        for (int i = 0; i < DRAM_WQ_SIZE; i++) {
            PACKET filler = makeWritePacket(0xC000000 + (uint64_t)i * DCM_BLOCK_SIZE);
            h.far_mc.add_wq(&filler);
        }
        if (h.far_mc.WQ[0].occupancy != (uint32_t)DRAM_WQ_SIZE) {
            std::cerr << "[TEST 2 SETUP FAIL] could not fill farMC's WQ to capacity" << std::endl;
            failures++;
        }
        h.far_mc.wqArrivalOrder.clear(); // ignore the filler additions themselves in order tracking

        // Pump a few cycles: drainWB() will now find farMC full and must
        // retain the entry, not drop it.
        pump(h, 20);

        bool wbHasOne = h.dcm.WB.size() == 1;
        bool notDispatched = h.far_mc.wqArrivalOrder.empty();
        bool retryCounted = h.dcm.stats.wbDispatchRetries > 0;
        bool zeroDrainsForThis = h.dcm.stats.wbDrains == 0;

        std::cout << "[TEST 2: farMC full] wb_size=" << h.dcm.WB.size() << " not_dispatched=" << notDispatched
                   << " retries=" << h.dcm.stats.wbDispatchRetries << " wbDrains=" << h.dcm.stats.wbDrains
                   << std::endl;
        if (!wbHasOne || !notDispatched || !retryCounted || !zeroDrainsForThis) {
            std::cerr << "  FAIL (a WB entry must be retained, not dropped, when farMC has no room)"
                      << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }

        // ==============================================================
        // Test 3 (continues from Test 2's exact state): queue becomes
        // available -> WB write succeeds on a later cycle -> WB entry
        // removed exactly once.
        // ==============================================================
        // Drain farMC's pre-filled WQ via real DRAM completion timing.
        bool quiescentReachedForFiller = false;
        for (int i = 0; i < 20000; i++) {
            current_core_cycle[0]++;
            h.dcm.operate();
            if (h.far_mc.WQ[0].occupancy < (uint32_t)DRAM_WQ_SIZE) {
                quiescentReachedForFiller = true;
                break;
            }
        }
        // Now that room exists, keep pumping until the retained WB entry
        // is actually drained.
        pumpUntilQuiescent(h, 20000);

        bool wbNowEmpty = h.dcm.WB.empty();
        bool exactlyOneDrainNow = h.dcm.stats.wbDrains == 1;
        int occurrences = 0;
        for (size_t i = 0; i < h.far_mc.wqArrivalOrder.size(); i++)
            if (h.far_mc.wqArrivalOrder[i] == addr)
                occurrences++;
        bool dispatchedExactlyOnce = occurrences == 1;

        std::cout << "[TEST 3: capacity frees up] freed=" << quiescentReachedForFiller << " wb_empty="
                   << wbNowEmpty << " wbDrains=" << h.dcm.stats.wbDrains << " occurrences_of_addr="
                   << occurrences << std::endl;
        if (!quiescentReachedForFiller || !wbNowEmpty || !exactlyOneDrainNow || !dispatchedExactlyOnce) {
            std::cerr << "  FAIL (a retried WB entry must succeed exactly once, no duplicate dispatch)"
                      << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Test 4: multiple WB entries -> no entry lost, no entry duplicated.
    // ==================================================================
    {
        Harness h("WBR4");
        const int N = 20;
        std::vector<uint64_t> addrs(N);
        for (int i = 0; i < N; i++) {
            addrs[i] = 0xB200000 + (uint64_t)i * DCM_BLOCK_SIZE;
            seedDirtyLine(h, addrs[i]);
        }
        for (int i = 0; i < N; i++) {
            uint64_t evictAddr = addrs[i] + STRIDE; // shares addrs[i]'s index, evicts it
            issueEvictingRead(h, evictAddr);
        }
        pumpUntilQuiescent(h, 200000);

        std::map<uint64_t, int> counts;
        for (size_t i = 0; i < h.far_mc.wqArrivalOrder.size(); i++)
            counts[h.far_mc.wqArrivalOrder[i]]++;

        bool allExactlyOnce = true;
        for (int i = 0; i < N; i++) {
            if (counts[addrs[i]] != 1)
                allExactlyOnce = false;
        }
        bool countsMatch = (h.dcm.stats.wbInsertions == (uint64_t)N) && (h.dcm.stats.wbDrains == (uint64_t)N);

        std::cout << "[TEST 4: multiple entries] wbInsertions=" << h.dcm.stats.wbInsertions
                   << " wbDrains=" << h.dcm.stats.wbDrains << " expected=" << N
                   << " all_exactly_once=" << allExactlyOnce << std::endl;
        if (!countsMatch || !allExactlyOnce) {
            std::cerr << "  FAIL" << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Test 5 / STRESS TEST: sustained write pressure across MULTIPLE
    // successive waves, reproducing a controlled version of the exact
    // failure case documented in docs/wb_retry_audit.md. Proves, summed
    // across all waves:
    //   WB entries generated == far-memory writes accepted
    //   lost writes = 0
    //   duplicated writes = 0
    //
    // A FOURTH real test bug found while tuning this test: an initial
    // design tried to build up a SINGLE wave of 100 simultaneously
    // BLOCKED WB entries (more than DRAM_WQ_SIZE=64) to "obviously"
    // exceed farMC's WQ capacity. But DCM_WB_PRESSURE_THRESHOLD (=64,
    // gem5's own real, pre-existing, ALREADY-CORRECT admission-time
    // backpressure -- see inc/dram_cache_manager.h) rejects any NEW
    // admission once WB.size() >= 64, specifically to prevent the WB
    // deque from ever growing past that in the first place. Under
    // forceWqFull (nothing ever drains), WB genuinely reaches exactly 64
    // and then STAYS there -- the 65th eviction's admission is
    // legitimately, correctly rejected by this pre-existing mechanism,
    // not lost by the fix under test. A single-wave test design that
    // ignores this cannot ever observe more than 64 blocked entries
    // through normal admission, because the system is DESIGNED not to
    // allow that -- so it isn't a valid way to "stress" this feature.
    // The correct stress shape is instead SUSTAINED PRESSURE ACROSS
    // MULTIPLE WAVES: each wave fills WB up to (just under) its
    // admission-enforced ceiling, gets fully drained via the retry
    // mechanism, and the NEXT wave repeats -- proving the mechanism
    // holds up not just once but repeatedly, which is what "sustained"
    // actually means for a system that already caps single-wave
    // backlog by design.
    //
    // Uses forceWqFull (the same deterministic-blocking technique
    // Tests 8/9 use) to GUARANTEE every wave's write-backs are retained/
    // retried, and fully serializes each eviction's admission (waits for
    // its own ORB entry to retire before issuing the next) to avoid a
    // THIRD real test bug found while tuning this test: concurrently
    // admitting many evictions also drives nearMC's own
    // background-fill-write dispatch (return_data()'s
    // WAITING_FAR_MEM_READ_RESP branch calls nearMC->add_wq() directly,
    // with the same lack of bounds protection as the far/write path this
    // file fixes) close enough to ITS OWN capacity to introduce timing
    // artifacts unrelated to farMC's WQ, the actual thing this test
    // targets -- an out-of-scope, separate gap, not something this
    // stage's fix is meant to (or should) also resolve.
    // ==================================================================
    {
        Harness h("WBR5_STRESS");
        const int PER_WAVE = 50; // comfortably under DCM_WB_PRESSURE_THRESHOLD (64)
        const int WAVES = 3;     // total throughput (150) well exceeds a single farMC WQ capacity (64)
        uint64_t totalGenerated = 0, totalAccepted = 0, totalLost = 0, totalDuplicated = 0;
        bool everyWaveFullyBlocked = true;

        for (int wave = 0; wave < WAVES; wave++) {
            std::vector<uint64_t> addrs(PER_WAVE);
            for (int i = 0; i < PER_WAVE; i++) {
                addrs[i] = 0xB400000 + (uint64_t)(wave * PER_WAVE + i) * DCM_BLOCK_SIZE;
                seedDirtyLine(h, addrs[i]);
            }

            h.far_mc.forceWqFull = true;
            uint64_t insertionsBefore = h.dcm.stats.wbInsertions;
            uint64_t drainsBefore = h.dcm.stats.wbDrains;
            h.far_mc.wqArrivalOrder.clear();

            // Fully serialized admission -- see banner comment above.
            for (int i = 0; i < PER_WAVE; i++) {
                PACKET r = makeReadPacket(addrs[i] + STRIDE);
                h.dcm.add_rq(&r);
                for (int c = 0; c < 2000 && !h.dcm.ORB.empty(); c++) {
                    current_core_cycle[0]++;
                    h.dcm.operate();
                }
            }

            bool waveFullyBlocked = (h.dcm.stats.wbInsertions - insertionsBefore == (uint64_t)PER_WAVE) &&
                                     (h.dcm.stats.wbDrains == drainsBefore);
            everyWaveFullyBlocked = everyWaveFullyBlocked && waveFullyBlocked;

            h.far_mc.forceWqFull = false;
            pumpUntilQuiescent(h, 1000000);

            std::map<uint64_t, int> counts;
            for (size_t i = 0; i < h.far_mc.wqArrivalOrder.size(); i++)
                counts[h.far_mc.wqArrivalOrder[i]]++;
            for (int i = 0; i < PER_WAVE; i++) {
                int c = counts[addrs[i]];
                if (c == 0)
                    totalLost++;
                else if (c > 1)
                    totalDuplicated++;
            }
            totalGenerated += (h.dcm.stats.wbInsertions - insertionsBefore);
            totalAccepted += (h.dcm.stats.wbDrains - drainsBefore);
        }

        bool generatedEqualsAccepted = totalGenerated == totalAccepted && totalGenerated == (uint64_t)(PER_WAVE * WAVES);
        bool retriesHappened = h.dcm.stats.wbDispatchRetries > 0;

        std::cout << "[TEST 5 / STRESS: " << WAVES << " waves x " << PER_WAVE
                   << " sustained pressure] every_wave_fully_blocked=" << everyWaveFullyBlocked
                   << " total_generated=" << totalGenerated << " total_accepted=" << totalAccepted
                   << " retries_observed=" << h.dcm.stats.wbDispatchRetries << " lost=" << totalLost
                   << " duplicated=" << totalDuplicated << std::endl;
        if (!everyWaveFullyBlocked || !generatedEqualsAccepted || totalLost != 0 || totalDuplicated != 0 ||
            !retriesHappened) {
            std::cerr << "  FAIL (WB entries generated must exactly equal far-memory writes accepted, "
                         "with zero lost and zero duplicated, across every wave of sustained pressure)"
                      << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Test 6: mixed WB traffic and normal far reads/writes -- cold-miss
    // far reads must proceed normally, unaffected by WB retry activity
    // on the far-write side.
    // ==================================================================
    {
        Harness h("WBR6_MIXED");
        const int N = 30;
        std::vector<uint64_t> dirtyAddrs(N);
        for (int i = 0; i < N; i++) {
            dirtyAddrs[i] = 0xB600000 + (uint64_t)i * DCM_BLOCK_SIZE;
            seedDirtyLine(h, dirtyAddrs[i]);
        }
        // Evict all N (far writes / WB pressure) AND issue fresh cold
        // misses to unrelated addresses (far reads), interleaved in the
        // submission order -- retrying admission on rejection (see
        // admitAllEvictions()'s comment), since a single-shot loop here
        // would suffer the exact same "rejected admission silently
        // vanishes from the test's own bookkeeping" bug found in the
        // stress test.
        std::vector<uint64_t> toAdmit(2 * N);
        for (int i = 0; i < N; i++) {
            toAdmit[2 * i] = dirtyAddrs[i] + STRIDE;
            toAdmit[2 * i + 1] = 0xB700000 + (uint64_t)i * DCM_BLOCK_SIZE;
        }
        admitAllEvictions(h, toAdmit, 5000);
        pumpUntilQuiescent(h, 500000);

        std::map<uint64_t, int> wqCounts;
        for (size_t i = 0; i < h.far_mc.wqArrivalOrder.size(); i++)
            wqCounts[h.far_mc.wqArrivalOrder[i]]++;
        bool allWritesExactlyOnce = true;
        for (int i = 0; i < N; i++)
            if (wqCounts[dirtyAddrs[i]] != 1)
                allWritesExactlyOnce = false;

        // The N cold reads are far reads too (miss -> far fetch); plus
        // the N eviction reads are ALSO far reads (miss -> far fetch for
        // the new tag). Expect 2N total far-read dispatches, all
        // eventually completing (responses at the LLC).
        bool allColdReadsCompleted = h.llc.responses.size() >= (size_t)(2 * N);

        std::cout << "[TEST 6: mixed traffic] wbDrains=" << h.dcm.stats.wbDrains << " expected=" << N
                   << " all_writes_once=" << allWritesExactlyOnce << " llc_responses=" << h.llc.responses.size()
                   << " expected_min=" << (2 * N) << std::endl;
        if (h.dcm.stats.wbDrains != (uint64_t)N || !allWritesExactlyOnce || !allColdReadsCompleted) {
            std::cerr << "  FAIL" << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Test 7: ordering is preserved -- WB entries drain to farMC in the
    // exact order they were inserted, even when some are retried.
    // ==================================================================
    {
        Harness h("WBR7_ORDER");
        // Pre-fill farMC's WQ so the FIRST few evictions are guaranteed
        // to be retried (blocked) before capacity frees up, forcing the
        // retry path to actually reorder-risk a few entries.
        for (int i = 0; i < DRAM_WQ_SIZE; i++) {
            PACKET filler = makeWritePacket(0xB800000 + (uint64_t)i * DCM_BLOCK_SIZE);
            h.far_mc.add_wq(&filler);
        }
        h.far_mc.wqArrivalOrder.clear();

        const int N = 10;
        std::vector<uint64_t> addrs(N);
        for (int i = 0; i < N; i++) {
            addrs[i] = 0xB900000 + (uint64_t)i * DCM_BLOCK_SIZE;
            seedDirtyLine(h, addrs[i]);
        }
        for (int i = 0; i < N; i++)
            issueEvictingRead(h, addrs[i] + STRIDE);

        pumpUntilQuiescent(h, 1000000);

        // wqArrivalOrder may also contain the filler's own eventual
        // re-additions -- it does not, since fillers are never
        // re-submitted; only actual WB drains are retried. Extract just
        // the addresses that belong to this test's N entries, in the
        // order farMC received them.
        std::vector<uint64_t> observedOrder;
        for (size_t i = 0; i < h.far_mc.wqArrivalOrder.size(); i++) {
            uint64_t a = h.far_mc.wqArrivalOrder[i];
            for (int j = 0; j < N; j++) {
                if (a == addrs[j]) {
                    observedOrder.push_back(a);
                    break;
                }
            }
        }

        bool sameLength = observedOrder.size() == addrs.size();
        bool sameOrder = sameLength;
        if (sameLength) {
            for (int i = 0; i < N; i++)
                if (observedOrder[i] != addrs[i])
                    sameOrder = false;
        }

        std::cout << "[TEST 7: ordering preserved] observed_count=" << observedOrder.size() << " expected="
                   << N << " same_order=" << sameOrder << " retries_observed=" << h.dcm.stats.wbDispatchRetries
                   << std::endl;
        if (!sameLength || !sameOrder) {
            std::cerr << "  FAIL (WB entries must drain to farMC in their original insertion order, even "
                         "under retry)"
                      << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Test 8: parent/request accounting remains correct -- the evicting
    // request's own ORB entry completes (LLC gets its response)
    // regardless of whether its victim's write-back is immediately
    // accepted or retried.
    // ==================================================================
    {
        Harness h("WBR8_PARENT");
        // Force farMC's WQ to report permanently full (see
        // ORDER_TRACKING_MC::forceWqFull) -- deterministic, timing-free
        // blocking of the write-back specifically. Deliberately NOT
        // using a real pre-filled WQ here (as earlier tests do): a real
        // fill also flips farMC's write_mode arbitration
        // (dram_controller.cc), which would ALSO throttle the parent's
        // own far-READ (RQ is only serviced while write_mode==0) --
        // exactly the confound this test must avoid, since it needs to
        // prove the parent completes on its OWN, undelayed schedule
        // while ONLY its victim's write-back is blocked.
        h.far_mc.forceWqFull = true;

        uint64_t addr = 0xBB00000, evictAddr = addr + STRIDE;
        seedDirtyLine(h, addr);
        PACKET r = makeReadPacket(evictAddr);
        h.dcm.add_rq(&r);

        bool parentCompletedWhileBlocked = false;
        for (int i = 0; i < 2000; i++) {
            current_core_cycle[0]++;
            h.dcm.operate();
            if (!h.llc.responses.empty()) {
                parentCompletedWhileBlocked = true;
                break;
            }
        }
        // WB is guaranteed to still be blocked here regardless of how
        // long the loop above took, since forceWqFull never lets the
        // write-back succeed until explicitly turned off.
        bool wbStillBlocked = !h.dcm.WB.empty();

        std::cout << "[TEST 8: parent accounting] parent_completed_while_wb_blocked="
                   << parentCompletedWhileBlocked << " wb_still_blocked=" << wbStillBlocked << std::endl;
        if (!parentCompletedWhileBlocked || !wbStillBlocked) {
            std::cerr << "  FAIL (the evicting request must complete independently of whether its "
                         "victim's write-back is retained/retried)"
                      << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Test 9: WB occupancy statistics remain correct while entries are
    // retained/retried.
    // ==================================================================
    {
        Harness h("WBR9_STATS");
        const int N = 5;
        h.far_mc.forceWqFull = true; // deterministic, timing-free blocking -- see Test 8
        std::vector<uint64_t> addrs(N);
        for (int i = 0; i < N; i++) {
            addrs[i] = 0xBD00000 + (uint64_t)i * DCM_BLOCK_SIZE;
            seedDirtyLine(h, addrs[i]);
        }
        for (int i = 0; i < N; i++)
            issueEvictingRead(h, addrs[i] + STRIDE);
        // Pump cycle-by-cycle until all N have been identified as dirty
        // and pushed to WB, rather than guessing a fixed cycle count.
        for (int i = 0; i < 5000 && h.dcm.stats.wbInsertions < (uint64_t)N; i++) {
            current_core_cycle[0]++;
            h.dcm.operate();
        }

        bool wbSizeMatchesRetained = h.dcm.WB.size() == (size_t)N;
        bool maxOccupancyCorrect = h.dcm.stats.wbMaxOccupancy >= (uint64_t)N;
        bool retriesNonzero = h.dcm.stats.wbDispatchRetries > 0;
        bool insertionsCorrect = h.dcm.stats.wbInsertions == (uint64_t)N;
        bool zeroDrainsYet = h.dcm.stats.wbDrains == 0;

        std::cout << "[TEST 9: WB stats while blocked] wb_size=" << h.dcm.WB.size() << " expected=" << N
                   << " wbMaxOccupancy=" << h.dcm.stats.wbMaxOccupancy << " wbInsertions="
                   << h.dcm.stats.wbInsertions << " wbDrains=" << h.dcm.stats.wbDrains << " retries="
                   << h.dcm.stats.wbDispatchRetries << std::endl;
        if (!wbSizeMatchesRetained || !maxOccupancyCorrect || !retriesNonzero || !insertionsCorrect ||
            !zeroDrainsYet) {
            std::cerr << "  FAIL" << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;

            // Now free capacity (turn off the forced-full override) and
            // confirm stats reconcile once drained.
            h.far_mc.forceWqFull = false;
            pumpUntilQuiescent(h, 200000);
            bool finalDrainsCorrect = h.dcm.stats.wbDrains == (uint64_t)N;
            bool finalWbEmpty = h.dcm.WB.empty();
            std::cout << "  [after drain] wbDrains=" << h.dcm.stats.wbDrains << " expected=" << N
                       << " wb_empty=" << finalWbEmpty << std::endl;
            if (!finalDrainsCorrect || !finalWbEmpty) {
                std::cerr << "  FAIL (post-drain)" << std::endl;
                failures++;
            }
        }
    }

    if (failures == 0) {
        std::cout << "ALL WB RETRY TESTS PASSED" << std::endl;
        return 0;
    } else {
        std::cout << failures << " WB RETRY TEST(S) FAILED" << std::endl;
        return 1;
    }
}
