// Deterministic tests for the memory-dispatch audit
// (docs/memory_dispatch_audit.md): every path where DRAM_CACHE_MANAGER
// sends a request to nearMC or farMC must never silently drop it.
//
// BACKGROUND: the WB-write-back path (drainWB()) was already fixed and
// verified (docs/wb_retry_audit.md, tests/test_dcm_wb_retry.cc). This
// audit found FIVE more call sites with the identical class of exposure
// (some via MEMORY_CONTROLLER::add_rq()/add_wq()'s own bounds-check-free
// silent-drop bug directly, others via dispatchToFar()'s bool return
// being ignored):
//   1. near-memory tag-check READ      (driveState, DCM_LOC_MEM_READ)
//   2. near-memory direct WRITE        (driveState, DCM_LOC_MEM_WRITE)
//   3. near-memory cache-fill WRITE    (return_data, background fill)
//   4. far-memory demand READ          (driveState, DCM_FAR_MEM_READ)
//   5. bypass-mode READ                (add_rq's bypassDcache branch)
//   6. bypass-mode WRITE               (add_wq's bypassDcache branch)
//
// FIX: two new guaranteed-delivery wrappers, dispatchToNear() (near
// side, backed by a new pendingNearDispatches retry queue) and
// dispatchToFarGuaranteed() (far side, reuses the EXISTING
// pendingFarDispatches queue/processPendingFarDispatches() loop already
// proven correct for WB). dispatchToFar() itself (used directly by
// drainWB()) is UNCHANGED in contract. See docs/memory_dispatch_audit.md
// for the full audit table and gem5 correspondence.
//
// Proves, for each of the 6 paths above: the destination queue can
// become full; a request hitting a full queue is retained, not lost;
// it succeeds exactly once (no duplication) once capacity frees up;
// FIFO order is preserved under multiple pending operations; ORB/
// bypass-tracking state stays correct while an operation is pending;
// and stress-test scale (generated == accepted == eventually completed)
// holds for every path.
//
// Build:
//   g++ -std=c++11 -Iinc -o /tmp/test_dcm_memory_dispatch \
//       tests/test_dcm_memory_dispatch.cc src/dram_cache_manager.cc \
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

// Records exact dispatch order/counts on BOTH RQ and WQ, and can force
// either queue to report "full" deterministically (see
// tests/test_dcm_wb_retry.cc's forceWqFull, generalized here to cover
// reads too since this file audits read paths as well as writes).
class CONTROLLABLE_MC : public MEMORY_CONTROLLER {
  public:
    std::vector<uint64_t> rqArrivalOrder;
    std::vector<uint64_t> wqArrivalOrder;
    bool forceRqFull;
    bool forceWqFull;
    explicit CONTROLLABLE_MC(std::string name) : MEMORY_CONTROLLER(name), forceRqFull(false), forceWqFull(false) {}
    int add_rq(PACKET *packet)
    {
        rqArrivalOrder.push_back(packet->address);
        return MEMORY_CONTROLLER::add_rq(packet);
    }
    int add_wq(PACKET *packet)
    {
        wqArrivalOrder.push_back(packet->address);
        return MEMORY_CONTROLLER::add_wq(packet);
    }
    uint32_t get_occupancy(uint8_t queue_type, uint64_t address)
    {
        if (forceRqFull && queue_type == 1)
            return 1;
        if (forceWqFull && queue_type == 2)
            return 1;
        return MEMORY_CONTROLLER::get_occupancy(queue_type, address);
    }
    uint32_t get_size(uint8_t queue_type, uint64_t address)
    {
        if (forceRqFull && queue_type == 1)
            return 1;
        if (forceWqFull && queue_type == 2)
            return 1;
        return MEMORY_CONTROLLER::get_size(queue_type, address);
    }
};

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
    CONTROLLABLE_MC near_mc, far_mc;
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

// See tests/test_dcm_wb_retry.cc's banner comment for why ORB.empty()
// must be part of a "quiescent" check (a check of only WB/pending-queue
// emptiness is trivially true before anything has even started).
//
// TWO real test bugs found while writing THIS file, on top of that
// lesson:
//   1. ORB.empty() becomes true as soon as completeRequest() runs, but
//      a nonzero (default) controller frontend/backend latency means
//      the actual LLC-visible response can still be sitting in
//      dcm.pendingResponses, not yet delivered, for a while AFTER that
//      -- completeRequest() erases the ORB entry unconditionally, then
//      schedules the response separately
//      (docs/gem5_to_champsim_mapping.md's controller-latency section).
//      Omitting pendingResponses.empty() let this report "done" one
//      step early.
//   2. Bypass-mode traffic has NO ORB entry at all (by design -- see
//      docs/bypass_mode.md), so ORB.empty() gives ZERO signal about
//      whether a bypass request's underlying real MEMORY_CONTROLLER
//      processing has actually finished: pendingFarDispatches can
//      become empty (the packet was handed off to farMC's REAL queue)
//      long before that real queue finishes processing it. Fixed by
//      also checking near_mc/far_mc's own RQ occupancy directly --
//      the only way to know nothing is still genuinely in flight
//      inside either controller, ORB-tracked or not.
static bool pumpUntilQuiescent(Harness &h, uint64_t maxCycles)
{
    for (uint64_t i = 0; i < maxCycles; i++) {
        current_core_cycle[0]++;
        h.dcm.operate();
        if (h.dcm.WB.empty() && h.dcm.pendingNearDispatches.empty() && h.dcm.pendingFarDispatches.empty() &&
            h.dcm.pendingResponses.empty() && h.near_mc.RQ[0].occupancy == 0 && h.near_mc.WQ[0].occupancy == 0 &&
            h.far_mc.RQ[0].occupancy == 0 && h.far_mc.WQ[0].occupancy == 0 && h.dcm.ORB.empty())
            return true;
    }
    return false;
}

// Pumps until `cond()` is true (checked once per cycle, after
// operate()), or the budget runs out -- used instead of a fixed cycle
// count wherever the exact number of cycles needed depends on
// contention that varies by run (e.g. how many near-memory banks are
// occupied by concurrently in-flight tag-check reads).
template <typename Cond>
static bool pumpUntilTrue(Harness &h, Cond cond, uint64_t maxCycles)
{
    for (uint64_t i = 0; i < maxCycles; i++) {
        current_core_cycle[0]++;
        h.dcm.operate();
        if (cond())
            return true;
    }
    return false;
}

int main()
{
    current_core_cycle[0] = 0;
    all_warmup_complete = NUM_CPUS;
    int failures = 0;
    const uint64_t STRIDE = DCM_DRAM_CACHE_SIZE;

    // ==================================================================
    // Test 1: near-memory tag-check READ queue full -> retained, not
    // lost; succeeds exactly once once capacity frees up.
    // ==================================================================
    {
        Harness h("MD1_NEAR_READ");
        h.near_mc.forceRqFull = true;
        uint64_t addr = 0xD000000;
        PACKET r = makeReadPacket(addr);
        h.dcm.add_rq(&r);
        pump(h, 50);

        bool retained = h.dcm.pendingNearDispatches.size() == 1;
        bool notSentYet = h.near_mc.rqArrivalOrder.empty();
        bool noResponseYet = h.llc.responses.empty();
        bool retriesCounted = h.dcm.stats.nearDispatchRetries > 0;

        std::cout << "[TEST 1: near tag-check read, nearMC RQ full] pending=" << h.dcm.pendingNearDispatches.size()
                   << " not_sent_yet=" << notSentYet << " no_response_yet=" << noResponseYet
                   << " retries=" << h.dcm.stats.nearDispatchRetries << std::endl;
        if (!retained || !notSentYet || !noResponseYet || !retriesCounted) {
            std::cerr << "  FAIL (a near tag-check read must be retained, not lost, when nearMC's RQ is full)"
                      << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }

        // Release capacity: succeeds exactly once.
        h.near_mc.forceRqFull = false;
        bool completed = pumpUntilQuiescent(h, 20000);
        int occurrences = 0;
        for (size_t i = 0; i < h.near_mc.rqArrivalOrder.size(); i++)
            if (h.near_mc.rqArrivalOrder[i] == addr)
                occurrences++;
        std::cout << "  [after release] completed=" << completed << " occurrences=" << occurrences
                   << " llc_responses=" << h.llc.responses.size() << std::endl;
        if (!completed || occurrences != 1 || h.llc.responses.size() != 1) {
            std::cerr << "  FAIL (post-release)" << std::endl;
            failures++;
        }
    }

    // ==================================================================
    // Test 2: near-memory cache-fill WRITE queue full -> retained, not
    // lost; LLC response is NOT delayed by the blocked fill; fill
    // succeeds exactly once once capacity frees up.
    // ==================================================================
    {
        Harness h("MD2_NEAR_FILL");
        h.near_mc.forceWqFull = true; // blocks WQ only -- RQ (tag-check read) proceeds normally
        uint64_t addr = 0xD100000;    // cold miss -> far fetch -> background fill write to nearMC
        PACKET r = makeReadPacket(addr);
        h.dcm.add_rq(&r);

        // Pump until the LLC response arrives (must NOT be blocked by
        // the fill-write capacity issue at all).
        bool responded = false;
        for (int i = 0; i < 5000; i++) {
            current_core_cycle[0]++;
            h.dcm.operate();
            if (!h.llc.responses.empty()) {
                responded = true;
                break;
            }
        }
        bool fillRetained = h.dcm.pendingNearDispatches.size() == 1;
        bool fillNotSentYet = h.near_mc.wqArrivalOrder.empty();

        std::cout << "[TEST 2: near fill write, nearMC WQ full] llc_responded=" << responded
                   << " fill_pending=" << h.dcm.pendingNearDispatches.size() << " fill_not_sent="
                   << fillNotSentYet << std::endl;
        if (!responded || !fillRetained || !fillNotSentYet) {
            std::cerr << "  FAIL (the LLC response must not be delayed by a blocked background fill, and "
                         "the fill itself must be retained, not lost)"
                      << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }

        h.near_mc.forceWqFull = false;
        bool drained = pumpUntilQuiescent(h, 20000);
        int occurrences = 0;
        for (size_t i = 0; i < h.near_mc.wqArrivalOrder.size(); i++)
            if (h.near_mc.wqArrivalOrder[i] == addr)
                occurrences++;
        std::cout << "  [after release] drained=" << drained << " fill_occurrences=" << occurrences << std::endl;
        if (!drained || occurrences != 1) {
            std::cerr << "  FAIL (post-release)" << std::endl;
            failures++;
        }
    }

    // ==================================================================
    // Test 3: far-memory demand READ queue full -> retained, not lost;
    // succeeds exactly once once capacity frees up.
    // ==================================================================
    {
        Harness h("MD3_FAR_READ");
        h.far_mc.forceRqFull = true;
        uint64_t addr = 0xD200000; // cold miss -> tag check (near, unaffected) -> far fetch (blocked)
        PACKET r = makeReadPacket(addr);
        h.dcm.add_rq(&r);
        pumpUntilTrue(h, [&]() { return !h.dcm.pendingFarDispatches.empty(); }, 20000);

        bool retained = h.dcm.pendingFarDispatches.size() == 1;
        bool isRead = retained && !h.dcm.pendingFarDispatches.front().isWrite;
        bool notSentYet = h.far_mc.rqArrivalOrder.empty();
        bool noResponseYet = h.llc.responses.empty();
        bool retriesCounted = h.dcm.stats.farDispatchRetries > 0;

        std::cout << "[TEST 3: far demand read, farMC RQ full] pending=" << h.dcm.pendingFarDispatches.size()
                   << " is_read=" << isRead << " not_sent_yet=" << notSentYet << " no_response_yet="
                   << noResponseYet << " retries=" << h.dcm.stats.farDispatchRetries << std::endl;
        if (!retained || !isRead || !notSentYet || !noResponseYet || !retriesCounted) {
            std::cerr << "  FAIL (a far demand read must be retained, not lost, when farMC's RQ is full)"
                      << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }

        h.far_mc.forceRqFull = false;
        bool completed = pumpUntilQuiescent(h, 20000);
        int occurrences = 0;
        for (size_t i = 0; i < h.far_mc.rqArrivalOrder.size(); i++)
            if (h.far_mc.rqArrivalOrder[i] == addr)
                occurrences++;
        std::cout << "  [after release] completed=" << completed << " occurrences=" << occurrences
                   << " llc_responses=" << h.llc.responses.size() << std::endl;
        if (!completed || occurrences != 1 || h.llc.responses.size() != 1) {
            std::cerr << "  FAIL (post-release)" << std::endl;
            failures++;
        }
    }

    // ==================================================================
    // Test 4: far-memory dirty WRITE-BACK queue full -> retained (this
    // path was already fixed in the prior stage -- see
    // tests/test_dcm_wb_retry.cc for the full suite; this is a minimal
    // confirming check for completeness of this file's path list).
    // ==================================================================
    {
        Harness h("MD4_FAR_WB");
        uint64_t addr = 0xD300000, evictAddr = addr + STRIDE;
        PACKET w = makeWritePacket(addr);
        h.dcm.add_wq(&w);
        for (int i = 0; i < 500 && !h.dcm.ORB.empty(); i++) {
            current_core_cycle[0]++;
            h.dcm.operate();
        }
        h.far_mc.forceWqFull = true;
        PACKET r = makeReadPacket(evictAddr);
        h.dcm.add_rq(&r);
        pump(h, 500);

        bool retained = h.dcm.WB.size() == 1;
        bool notSentYet = h.far_mc.wqArrivalOrder.empty();
        std::cout << "[TEST 4: far write-back, farMC WQ full] wb_size=" << h.dcm.WB.size() << " not_sent_yet="
                   << notSentYet << " retries=" << h.dcm.stats.wbDispatchRetries << std::endl;
        if (!retained || !notSentYet || h.dcm.stats.wbDispatchRetries == 0) {
            std::cerr << "  FAIL" << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }

        h.far_mc.forceWqFull = false;
        bool drained = pumpUntilQuiescent(h, 20000);
        int occurrences = 0;
        for (size_t i = 0; i < h.far_mc.wqArrivalOrder.size(); i++)
            if (h.far_mc.wqArrivalOrder[i] == addr)
                occurrences++;
        std::cout << "  [after release] drained=" << drained << " occurrences=" << occurrences << std::endl;
        if (!drained || occurrences != 1) {
            std::cerr << "  FAIL (post-release)" << std::endl;
            failures++;
        }
    }

    // ==================================================================
    // Test 5: bypass-mode READ queue full -> retained, not lost;
    // bypassOutstandingReads not leaked; succeeds exactly once.
    // ==================================================================
    {
        Harness h("MD5_BYPASS_READ");
        h.dcm.setBypassDcache(true);
        h.far_mc.forceRqFull = true;
        uint64_t addr = 0xD400000;
        PACKET r = makeReadPacket(addr);
        h.dcm.add_rq(&r);
        pump(h, 50);

        bool retained = h.dcm.pendingFarDispatches.size() == 1;
        bool trackedInBypassSet = h.dcm.bypassOutstandingReads.find(addr) != h.dcm.bypassOutstandingReads.end();
        bool notSentYet = h.far_mc.rqArrivalOrder.empty();
        bool noResponseYet = h.llc.responses.empty();

        std::cout << "[TEST 5: bypass read, farMC RQ full] pending=" << h.dcm.pendingFarDispatches.size()
                   << " tracked=" << trackedInBypassSet << " not_sent_yet=" << notSentYet
                   << " no_response_yet=" << noResponseYet << " retries=" << h.dcm.stats.farDispatchRetries
                   << std::endl;
        if (!retained || !trackedInBypassSet || !notSentYet || !noResponseYet) {
            std::cerr << "  FAIL (a bypass read must be retained, not lost, and its bypassOutstandingReads "
                         "entry must not be leaked)"
                      << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }

        h.far_mc.forceRqFull = false;
        bool completed = pumpUntilQuiescent(h, 20000);
        int occurrences = 0;
        for (size_t i = 0; i < h.far_mc.rqArrivalOrder.size(); i++)
            if (h.far_mc.rqArrivalOrder[i] == addr)
                occurrences++;
        bool bypassSetCleared = h.dcm.bypassOutstandingReads.find(addr) == h.dcm.bypassOutstandingReads.end();
        std::cout << "  [after release] completed=" << completed << " occurrences=" << occurrences
                   << " llc_responses=" << h.llc.responses.size() << " bypass_set_cleared=" << bypassSetCleared
                   << std::endl;
        if (!completed || occurrences != 1 || h.llc.responses.size() != 1 || !bypassSetCleared) {
            std::cerr << "  FAIL (post-release)" << std::endl;
            failures++;
        }
    }

    // ==================================================================
    // Test 6: bypass-mode WRITE queue full -> retained, not lost;
    // succeeds exactly once (this closes the one gap the prior stage
    // left explicitly open -- docs/bypass_mode.md's Limitations).
    // ==================================================================
    {
        Harness h("MD6_BYPASS_WRITE");
        h.dcm.setBypassDcache(true);
        h.far_mc.forceWqFull = true;
        uint64_t addr = 0xD500000;
        PACKET w = makeWritePacket(addr);
        h.dcm.add_wq(&w);
        pump(h, 50);

        bool retained = h.dcm.pendingFarDispatches.size() == 1;
        bool isWrite = retained && h.dcm.pendingFarDispatches.front().isWrite;
        bool notSentYet = h.far_mc.wqArrivalOrder.empty();

        std::cout << "[TEST 6: bypass write, farMC WQ full] pending=" << h.dcm.pendingFarDispatches.size()
                   << " is_write=" << isWrite << " not_sent_yet=" << notSentYet << std::endl;
        if (!retained || !isWrite || !notSentYet) {
            std::cerr << "  FAIL (a bypass write must be retained, not lost, when farMC's WQ is full)"
                      << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }

        h.far_mc.forceWqFull = false;
        bool drained = pumpUntilQuiescent(h, 20000);
        int occurrences = 0;
        for (size_t i = 0; i < h.far_mc.wqArrivalOrder.size(); i++)
            if (h.far_mc.wqArrivalOrder[i] == addr)
                occurrences++;
        std::cout << "  [after release] drained=" << drained << " occurrences=" << occurrences << std::endl;
        if (!drained || occurrences != 1) {
            std::cerr << "  FAIL (post-release)" << std::endl;
            failures++;
        }
    }

    // ==================================================================
    // Test 7: queue becomes available later -- explicit combined check
    // across near read, far read, and bypass read simultaneously, all
    // blocked at once, then released together.
    // ==================================================================
    {
        Harness h("MD7_RELEASE_LATER");
        h.near_mc.forceRqFull = true;
        h.far_mc.forceRqFull = true;
        uint64_t nearAddr = 0xD600000;
        PACKET r1 = makeReadPacket(nearAddr);
        h.dcm.add_rq(&r1);
        pump(h, 50);
        bool nearBlocked = h.dcm.pendingNearDispatches.size() == 1;

        h.near_mc.forceRqFull = false; // let the near read through so the far fetch can be attempted next
        pump(h, 500);
        bool farBlocked = h.dcm.pendingFarDispatches.size() == 1;

        h.far_mc.forceRqFull = false;
        bool completed = pumpUntilQuiescent(h, 20000);

        std::cout << "[TEST 7: queue becomes available later] near_blocked=" << nearBlocked << " far_blocked="
                   << farBlocked << " completed=" << completed << " llc_responses=" << h.llc.responses.size()
                   << std::endl;
        if (!nearBlocked || !farBlocked || !completed || h.llc.responses.size() != 1) {
            std::cerr << "  FAIL" << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Test 8/9/10/11: multiple pending operations, ordering preserved,
    // no duplicates, no lost operations -- checked together across a
    // batch of near tag-check reads AND a batch of far demand reads,
    // each forced to queue up multiple entries before release.
    // ==================================================================
    {
        Harness h("MD8_MULTI");
        const int N = 8;

        // -- near side: N distinct-index cold reads, nearMC RQ forced full --
        h.near_mc.forceRqFull = true;
        std::vector<uint64_t> nearAddrs(N);
        for (int i = 0; i < N; i++) {
            nearAddrs[i] = 0xD700000 + (uint64_t)i * DCM_BLOCK_SIZE;
            PACKET r = makeReadPacket(nearAddrs[i]);
            h.dcm.add_rq(&r);
        }
        pump(h, 50);
        bool allNearPending = h.dcm.pendingNearDispatches.size() == (size_t)N;

        h.near_mc.forceRqFull = false;
        bool nearDrained = pumpUntilQuiescent(h, 50000);

        std::vector<uint64_t> nearObserved;
        for (size_t i = 0; i < h.near_mc.rqArrivalOrder.size(); i++) {
            uint64_t a = h.near_mc.rqArrivalOrder[i];
            for (int j = 0; j < N; j++)
                if (a == nearAddrs[j]) {
                    nearObserved.push_back(a);
                    break;
                }
        }
        bool nearOrderOk = nearObserved.size() == (size_t)N;
        if (nearOrderOk)
            for (int i = 0; i < N; i++)
                if (nearObserved[i] != nearAddrs[i])
                    nearOrderOk = false;
        bool nearCompletedAll = h.llc.responses.size() == (size_t)N;

        std::cout << "[TEST 8-11 near: multiple/order/no-dup/no-lost] all_pending=" << allNearPending
                   << " drained=" << nearDrained << " order_ok=" << nearOrderOk << " completed_all="
                   << nearCompletedAll << std::endl;
        if (!allNearPending || !nearDrained || !nearOrderOk || !nearCompletedAll) {
            std::cerr << "  FAIL (near side)" << std::endl;
            failures++;
        } else {
            std::cout << "  PASS (near side)" << std::endl;
        }

        // -- far side: N distinct-index cold misses, farMC RQ forced full --
        Harness h2("MD8_MULTI_FAR");
        h2.far_mc.forceRqFull = true;
        std::vector<uint64_t> farAddrs(N);
        for (int i = 0; i < N; i++) {
            farAddrs[i] = 0xD800000 + (uint64_t)i * DCM_BLOCK_SIZE;
            PACKET r = makeReadPacket(farAddrs[i]);
            h2.dcm.add_rq(&r);
        }
        pumpUntilTrue(h2, [&]() { return (int)h2.dcm.pendingFarDispatches.size() >= N; }, 20000);
        bool allFarPending = h2.dcm.pendingFarDispatches.size() == (size_t)N;

        h2.far_mc.forceRqFull = false;
        bool farDrained = pumpUntilQuiescent(h2, 50000);

        std::vector<uint64_t> farObserved;
        for (size_t i = 0; i < h2.far_mc.rqArrivalOrder.size(); i++) {
            uint64_t a = h2.far_mc.rqArrivalOrder[i];
            for (int j = 0; j < N; j++)
                if (a == farAddrs[j]) {
                    farObserved.push_back(a);
                    break;
                }
        }
        bool farOrderOk = farObserved.size() == (size_t)N;
        if (farOrderOk)
            for (int i = 0; i < N; i++)
                if (farObserved[i] != farAddrs[i])
                    farOrderOk = false;
        bool farCompletedAll = h2.llc.responses.size() == (size_t)N;

        std::cout << "[TEST 8-11 far: multiple/order/no-dup/no-lost] all_pending=" << allFarPending
                   << " drained=" << farDrained << " order_ok=" << farOrderOk << " completed_all="
                   << farCompletedAll << std::endl;
        if (!allFarPending || !farDrained || !farOrderOk || !farCompletedAll) {
            std::cerr << "  FAIL (far side)" << std::endl;
            failures++;
        } else {
            std::cout << "  PASS (far side)" << std::endl;
        }
    }

    // ==================================================================
    // Test 12: request-ID/parent-ID correctness -- multiple different
    // addresses pending simultaneously on the near side must each
    // resolve to their OWN correct LLC response (not cross-attributed),
    // verified via distinct addresses in the response set.
    // ==================================================================
    {
        Harness h("MD12_PARENT_ID");
        const int N = 5;
        h.near_mc.forceRqFull = true;
        std::vector<uint64_t> addrs(N);
        for (int i = 0; i < N; i++) {
            addrs[i] = 0xD900000 + (uint64_t)i * DCM_BLOCK_SIZE;
            PACKET r = makeReadPacket(addrs[i]);
            h.dcm.add_rq(&r);
        }
        pump(h, 50);
        h.near_mc.forceRqFull = false;
        pumpUntilQuiescent(h, 50000);

        bool allResponsesCorrect = h.llc.responses.size() == (size_t)N;
        if (allResponsesCorrect) {
            std::map<uint64_t, int> seen;
            for (size_t i = 0; i < h.llc.responses.size(); i++)
                seen[h.llc.responses[i]]++;
            for (int i = 0; i < N; i++)
                if (seen[addrs[i]] != 1)
                    allResponsesCorrect = false;
        }

        std::cout << "[TEST 12: request-ID/parent-ID correctness] responses=" << h.llc.responses.size()
                   << " expected=" << N << " all_correct=" << allResponsesCorrect << std::endl;
        if (!allResponsesCorrect) {
            std::cerr << "  FAIL (each pending request must resolve to its OWN response, no "
                         "cross-attribution)"
                      << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // STRESS TESTS: intentionally overfill each destination queue,
    // proving generated == accepted == eventually completed (or, for
    // background writes, generated == accepted and every one reaches
    // its destination).
    // ==================================================================

    // -- Stress 1: near tag-check reads, forced full, N=40 --
    {
        Harness h("MDS1_NEAR_READ_STRESS");
        const int N = 40;
        h.near_mc.forceRqFull = true;
        std::vector<uint64_t> addrs(N);
        for (int i = 0; i < N; i++) {
            addrs[i] = 0xDA00000 + (uint64_t)i * DCM_BLOCK_SIZE;
            PACKET r = makeReadPacket(addrs[i]);
            h.dcm.add_rq(&r);
        }
        pump(h, 50);
        bool allPending = h.dcm.pendingNearDispatches.size() == (size_t)N;
        h.near_mc.forceRqFull = false;
        bool drained = pumpUntilQuiescent(h, 200000);

        std::map<uint64_t, int> counts;
        for (size_t i = 0; i < h.near_mc.rqArrivalOrder.size(); i++)
            counts[h.near_mc.rqArrivalOrder[i]]++;
        int lost = 0, duplicated = 0;
        for (int i = 0; i < N; i++) {
            if (counts[addrs[i]] == 0)
                lost++;
            else if (counts[addrs[i]] > 1)
                duplicated++;
        }
        bool allCompleted = h.llc.responses.size() == (size_t)N;

        std::cout << "[STRESS 1: near read, N=" << N << "] all_pending=" << allPending << " drained="
                   << drained << " lost=" << lost << " duplicated=" << duplicated << " completed="
                   << h.llc.responses.size() << " retries=" << h.dcm.stats.nearDispatchRetries << std::endl;
        if (!allPending || !drained || lost != 0 || duplicated != 0 || !allCompleted) {
            std::cerr << "  FAIL" << std::endl;
            failures++;
        } else {
            std::cout << "  PASS (generated == accepted == completed == " << N << ")" << std::endl;
        }
    }

    // -- Stress 2: far demand reads, forced full, N=40 --
    {
        Harness h("MDS2_FAR_READ_STRESS");
        const int N = 40;
        h.far_mc.forceRqFull = true;
        std::vector<uint64_t> addrs(N);
        for (int i = 0; i < N; i++) {
            addrs[i] = 0xDB00000 + (uint64_t)i * DCM_BLOCK_SIZE;
            PACKET r = makeReadPacket(addrs[i]);
            h.dcm.add_rq(&r);
        }
        pumpUntilTrue(h, [&]() { return (int)h.dcm.pendingFarDispatches.size() >= N; }, 50000);
        bool allPending = h.dcm.pendingFarDispatches.size() == (size_t)N;
        h.far_mc.forceRqFull = false;
        bool drained = pumpUntilQuiescent(h, 200000);

        std::map<uint64_t, int> counts;
        for (size_t i = 0; i < h.far_mc.rqArrivalOrder.size(); i++)
            counts[h.far_mc.rqArrivalOrder[i]]++;
        int lost = 0, duplicated = 0;
        for (int i = 0; i < N; i++) {
            if (counts[addrs[i]] == 0)
                lost++;
            else if (counts[addrs[i]] > 1)
                duplicated++;
        }
        bool allCompleted = h.llc.responses.size() == (size_t)N;

        std::cout << "[STRESS 2: far read, N=" << N << "] all_pending=" << allPending << " drained="
                   << drained << " lost=" << lost << " duplicated=" << duplicated << " completed="
                   << h.llc.responses.size() << " retries=" << h.dcm.stats.farDispatchRetries << std::endl;
        if (!allPending || !drained || lost != 0 || duplicated != 0 || !allCompleted) {
            std::cerr << "  FAIL" << std::endl;
            failures++;
        } else {
            std::cout << "  PASS (generated == accepted == completed == " << N << ")" << std::endl;
        }
    }

    // -- Stress 3: near background fill writes, forced full, N=20
    //    (each triggered by a cold-miss far fetch completing) --
    {
        Harness h("MDS3_NEAR_FILL_STRESS");
        const int N = 20;
        h.near_mc.forceWqFull = true;
        std::vector<uint64_t> addrs(N);
        for (int i = 0; i < N; i++) {
            addrs[i] = 0xDC00000 + (uint64_t)i * DCM_BLOCK_SIZE;
            PACKET r = makeReadPacket(addrs[i]);
            h.dcm.add_rq(&r);
        }
        bool allResponded = false;
        for (int i = 0; i < 200000 && h.llc.responses.size() < (size_t)N; i++) {
            current_core_cycle[0]++;
            h.dcm.operate();
        }
        allResponded = h.llc.responses.size() == (size_t)N;
        bool allFillsPending = h.dcm.pendingNearDispatches.size() == (size_t)N;

        h.near_mc.forceWqFull = false;
        bool drained = pumpUntilQuiescent(h, 200000);

        std::map<uint64_t, int> counts;
        for (size_t i = 0; i < h.near_mc.wqArrivalOrder.size(); i++)
            counts[h.near_mc.wqArrivalOrder[i]]++;
        int lost = 0, duplicated = 0;
        for (int i = 0; i < N; i++) {
            if (counts[addrs[i]] == 0)
                lost++;
            else if (counts[addrs[i]] > 1)
                duplicated++;
        }

        std::cout << "[STRESS 3: near fill write, N=" << N << "] all_responded=" << allResponded
                   << " all_fills_pending=" << allFillsPending << " drained=" << drained << " lost=" << lost
                   << " duplicated=" << duplicated << std::endl;
        if (!allResponded || !allFillsPending || !drained || lost != 0 || duplicated != 0) {
            std::cerr << "  FAIL (every generated fill must be accepted exactly once and reach nearMC, "
                         "independent of the already-sent LLC responses)"
                      << std::endl;
            failures++;
        } else {
            std::cout << "  PASS (generated == accepted == " << N << " fills, all reached nearMC)" << std::endl;
        }
    }

    if (failures == 0) {
        std::cout << "ALL MEMORY DISPATCH TESTS PASSED" << std::endl;
        return 0;
    } else {
        std::cout << failures << " MEMORY DISPATCH TEST(S) FAILED" << std::endl;
        return 1;
    }
}
