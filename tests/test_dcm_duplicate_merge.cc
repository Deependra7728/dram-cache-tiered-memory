// Regression tests for the duplicate-address merge defect
// (docs/final_independent_audit.md, CRITICAL-1 and CRITICAL-2).
//
// ROOT CAUSE. MEMORY_CONTROLLER::add_rq() (src/dram_controller.cc)
// checks for an entry with the SAME address already resident in the
// destination read queue and, if it finds one, returns that entry's
// index and enqueues NOTHING ("check for duplicates in the read
// queue"). In stock ChampSim that is safe, because the caller is a
// CACHE which merges via its own MSHR and expects no per-dispatch
// response. The DCM is different: it requires exactly one return_data()
// per dispatched read in order to advance the owning ORB entry. A
// merged read therefore produces NO completion, the ORB entry is
// stranded in DCM_WAITING_LOC_MEM_READ_RESP forever, and -- because a
// live ORB entry permanently owns its DRAM-cache index -- every later
// request mapping to that index is blocked for the rest of the run.
//
// REACHABILITY. promoteFromCRB() runs INSIDE return_data(), and
// MEMORY_CONTROLLER::process() only removes the completing queue entry
// AFTER return_data() returns. So a request promoted out of the CRB for
// the SAME address as the completing one collides with it and is merged
// away. Reachable from a real LLC: its MSHR prevents two concurrent
// READS to one address, but a read (add_rq) and a dirty writeback
// (add_wq) for the same address travel independent paths and can be
// concurrent -- that is Test 2 below.
//
// FIX. trySend() now inspects add_rq()'s return value. A non-negative
// return means "merged, not enqueued" (every other add_rq() path --
// warmup shortcut, write-queue-forward service, normal insert --
// returns -1), so the packet is handed back to the caller's existing
// retain-and-retry path and succeeds on a later cycle once the
// colliding entry has drained. WRITE merges are deliberately still
// treated as success: coalescing two writes to one address is
// legitimate write-queue behavior, produces no response either way, and
// cannot lose anything in a model that carries no data.
// processPendingFarDispatches() was additionally changed to release
// through trySend() rather than calling add_rq()/add_wq() directly, so
// the far/bypass release path is covered by the same check.
//
// Build:
//   g++ -std=c++11 -Iinc -o /tmp/test_dcm_duplicate_merge \
//       tests/test_dcm_duplicate_merge.cc src/dram_cache_manager.cc \
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

// Counts every address handed to each controller queue, so "lost" and
// "duplicated" can be distinguished from each other and from "merged".
class COUNTING_MC : public MEMORY_CONTROLLER {
  public:
    std::vector<uint64_t> rqOrder, wqOrder;
    explicit COUNTING_MC(std::string name) : MEMORY_CONTROLLER(name) {}
    int add_rq(PACKET *packet)
    {
        rqOrder.push_back(packet->address);
        return MEMORY_CONTROLLER::add_rq(packet);
    }
    int add_wq(PACKET *packet)
    {
        wqOrder.push_back(packet->address);
        return MEMORY_CONTROLLER::add_wq(packet);
    }
};

struct Harness {
    COUNTING_MC near_mc, far_mc;
    DRAM_CACHE_MANAGER dcm;
    FAKE_LLC llc;
    Harness(const std::string &n, DCM_POLICY pol = DCM_POLICY_BASELINE_CASCADE_LAKE)
        : near_mc(n + "_N"), far_mc(n + "_F"), dcm(n + "_DCM", &near_mc, &far_mc)
    {
        standardTiming(near_mc);
        standardTiming(far_mc);
        dcm.policy = pol;
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

// Fully quiescent: nothing left anywhere in the DCM or either controller.
static bool quiesce(Harness &h, uint64_t maxCycles)
{
    for (uint64_t i = 0; i < maxCycles; i++) {
        current_core_cycle[0]++;
        h.dcm.operate();
        if (h.dcm.ORB.empty() && h.dcm.CRB.empty() && h.dcm.WB.empty() &&
            h.dcm.pendingNearDispatches.empty() && h.dcm.pendingFarDispatches.empty() &&
            h.dcm.pendingResponses.empty() && h.near_mc.RQ[0].occupancy == 0 &&
            h.near_mc.WQ[0].occupancy == 0 && h.far_mc.RQ[0].occupancy == 0 && h.far_mc.WQ[0].occupancy == 0)
            return true;
    }
    return false;
}

static int countOf(const std::vector<uint64_t> &v, uint64_t a)
{
    int n = 0;
    for (size_t i = 0; i < v.size(); i++)
        if (v[i] == a)
            n++;
    return n;
}

int main()
{
    current_core_cycle[0] = 0;
    all_warmup_complete = NUM_CPUS;
    int failures = 0;
    const uint64_t STRIDE = DCM_DRAM_CACHE_SIZE;

    // ==================================================================
    // Test 1: two concurrent READS to the SAME address.
    // req1 occupies the ORB; req2 must land in the CRB; when req1
    // completes, req2 is promoted from INSIDE return_data() while req1's
    // entry is still resident in nearMC's RQ. Before the fix req2's
    // tag-check read was merged away and req2 never completed.
    // ==================================================================
    {
        Harness h("DM1");
        uint64_t A = 0x7000000;
        // Warm A so both requests are HITS -- keeps nearMC's WQ empty so
        // the write-queue-forwarding shortcut cannot mask the defect.
        {
            PACKET p = makeReadPacket(A);
            h.dcm.add_rq(&p);
        }
        quiesce(h, 20000);
        size_t base = h.llc.responses.size();
        uint64_t rdBase = h.dcm.stats.localReads, cfnBase = h.dcm.stats.completedFromNear;

        PACKET p1 = makeReadPacket(A);
        h.dcm.add_rq(&p1);
        PACKET p2 = makeReadPacket(A);
        h.dcm.add_rq(&p2);
        bool wentToCRB = (h.dcm.ORB.size() == 1 && h.dcm.CRB.size() == 1);

        bool q = quiesce(h, 200000);
        size_t delivered = h.llc.responses.size() - base;
        uint64_t dispatched = h.dcm.stats.localReads - rdBase;
        uint64_t completed = h.dcm.stats.completedFromNear - cfnBase;

        std::cout << "[TEST 1: two concurrent reads, same address] crb_used=" << wentToCRB
                   << " quiesced=" << q << " responses=" << delivered << " (expect 2)"
                   << " localReads_dispatched=" << dispatched << " completedFromNear=" << completed
                   << " ORB_residual=" << h.dcm.ORB.size() << " mergeRetries="
                   << h.dcm.stats.dispatchMergeRetries << std::endl;

        if (!wentToCRB || !q || delivered != 2 || dispatched != completed || !h.dcm.ORB.empty()) {
            std::cerr << "  FAIL (both requests must complete; every dispatched local read must "
                         "produce exactly one completion; no ORB entry may be stranded)"
                      << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Test 2: the LLC-REACHABLE variant -- a READ and a dirty WRITEBACK
    // to the same address. The LLC's MSHR merges concurrent reads, but
    // add_rq (read) and add_wq (writeback) are independent paths, so
    // this pairing genuinely occurs in real runs.
    // ==================================================================
    {
        Harness h("DM2");
        uint64_t A = 0x7100000;
        {
            PACKET p = makeReadPacket(A);
            h.dcm.add_rq(&p);
        }
        quiesce(h, 20000);
        uint64_t rdBase = h.dcm.stats.localReads, cfnBase = h.dcm.stats.completedFromNear;
        uint64_t wrBase = h.dcm.stats.completedWrites;

        PACKET p1 = makeReadPacket(A);
        h.dcm.add_rq(&p1);
        PACKET p2 = makeWritePacket(A); // writeback to the SAME address
        h.dcm.add_wq(&p2);
        bool wentToCRB = (h.dcm.ORB.size() == 1 && h.dcm.CRB.size() == 1);

        bool q = quiesce(h, 200000);
        uint64_t dispatched = h.dcm.stats.localReads - rdBase;
        uint64_t completed = h.dcm.stats.completedFromNear - cfnBase;
        uint64_t writesDone = h.dcm.stats.completedWrites - wrBase;

        std::cout << "[TEST 2: read + writeback, same address] crb_used=" << wentToCRB << " quiesced=" << q
                   << " localReads_dispatched=" << dispatched << " completedFromNear=" << completed
                   << " writes_completed=" << writesDone << " (expect 1)"
                   << " ORB_residual=" << h.dcm.ORB.size() << std::endl;

        if (!wentToCRB || !q || dispatched != completed || writesDone != 1 || !h.dcm.ORB.empty()) {
            std::cerr << "  FAIL (the promoted writeback must complete; no ORB entry may be stranded)"
                      << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Test 3: the index must NOT be permanently poisoned. After the
    // same-address collision resolves, ordinary traffic to that same
    // DRAM-cache index must still be admitted and complete. Before the
    // fix the stranded ORB entry made checkConflictInORB() true forever
    // for this index, so every later request queued in the CRB until it
    // filled and was then rejected outright.
    // ==================================================================
    {
        Harness h("DM3");
        uint64_t A = 0x7200000;
        {
            PACKET p = makeReadPacket(A);
            h.dcm.add_rq(&p);
        }
        quiesce(h, 20000);

        PACKET p1 = makeReadPacket(A);
        h.dcm.add_rq(&p1);
        PACKET p2 = makeReadPacket(A);
        h.dcm.add_rq(&p2);
        quiesce(h, 200000);

        // Now drive ordinary follow-up traffic to the SAME index.
        size_t base = h.llc.responses.size();
        const int N = 6;
        int admitted = 0;
        for (int i = 0; i < N; i++) {
            uint64_t addr = A + (uint64_t)(i + 1) * STRIDE; // same index, distinct tags
            PACKET p = makeReadPacket(addr);
            if (h.dcm.add_rq(&p) != -2)
                admitted++;
            quiesce(h, 200000);
        }
        size_t delivered = h.llc.responses.size() - base;

        std::cout << "[TEST 3: index not poisoned] admitted=" << admitted << "/" << N
                   << " delivered=" << delivered << "/" << N << " ORB_residual=" << h.dcm.ORB.size()
                   << " CRB_residual=" << h.dcm.CRB.size() << std::endl;

        if (admitted != N || delivered != (size_t)N || !h.dcm.ORB.empty() || !h.dcm.CRB.empty()) {
            std::cerr << "  FAIL (the DRAM-cache index must remain usable after a same-address collision)"
                      << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Test 4: no DUPLICATION. Each logical request must reach the
    // controller exactly once -- the fix must not turn a merge into a
    // double send.
    // ==================================================================
    {
        Harness h("DM4");
        uint64_t A = 0x7300000;
        {
            PACKET p = makeReadPacket(A);
            h.dcm.add_rq(&p);
        }
        quiesce(h, 20000);
        h.near_mc.rqOrder.clear();
        // Snapshot the completion counters too -- they accumulate from
        // the warm-up read above and are NOT reset by clearing rqOrder.
        uint64_t completionsBase = h.near_mc.RQ[0].ROW_BUFFER_HIT + h.near_mc.RQ[0].ROW_BUFFER_MISS;

        PACKET p1 = makeReadPacket(A);
        h.dcm.add_rq(&p1);
        PACKET p2 = makeReadPacket(A);
        h.dcm.add_rq(&p2);
        quiesce(h, 200000);

        // Two logical requests -> exactly two SERVICED tag-check reads.
        // A retry re-attempts add_rq, so raw call attempts may exceed 2;
        // what must equal 2 is the number of reads nearMC actually
        // serviced (the merge is refused, never double-enqueued).
        uint64_t completions =
            (h.near_mc.RQ[0].ROW_BUFFER_HIT + h.near_mc.RQ[0].ROW_BUFFER_MISS) - completionsBase;
        std::cout << "[TEST 4: no duplication] nearMC read completions=" << completions
                   << " (expect exactly 2)  add_rq_attempts_for_A=" << countOf(h.near_mc.rqOrder, A)
                   << std::endl;

        if (completions != 2) {
            std::cerr << "  FAIL (each logical request must be serviced exactly once)" << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Test 5: bypass mode, two concurrent reads to the same address
    // (CRITICAL-2). Bypass performs no conflict tracking by design, so
    // nothing else guards this. Both must complete and the
    // bypassOutstandingReads tracking set must not leak.
    // ==================================================================
    {
        Harness h("DM5");
        h.dcm.setBypassDcache(true);
        uint64_t A = 0x7400000;
        PACKET p1 = makeReadPacket(A);
        h.dcm.add_rq(&p1);
        PACKET p2 = makeReadPacket(A);
        h.dcm.add_rq(&p2);
        bool q = quiesce(h, 200000);

        std::cout << "[TEST 5: bypass, two concurrent reads] quiesced=" << q
                   << " bypassReads=" << h.dcm.stats.bypassReads
                   << " bypassCompletedReads=" << h.dcm.stats.bypassCompletedReads
                   << " responses=" << h.llc.responses.size() << " (expect 2)"
                   << " trackingLeak=" << h.dcm.bypassOutstandingReads.size() << " (expect 0)" << std::endl;

        if (!q || h.dcm.stats.bypassReads != 2 || h.dcm.stats.bypassCompletedReads != 2 ||
            h.llc.responses.size() != 2 || !h.dcm.bypassOutstandingReads.empty()) {
            std::cerr << "  FAIL (both bypass reads must complete and the tracking set must not leak)"
                      << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Test 6: same collision under BEAR and Oracle, to prove the fix is
    // policy-independent (it lives in the dispatch layer, below policy).
    // ==================================================================
    {
        DCM_POLICY pols[2] = {DCM_POLICY_BEAR_WR_OPT, DCM_POLICY_ORACLE};
        const char *names[2] = {"BEAR", "ORACLE"};
        bool allOk = true;
        for (int i = 0; i < 2; i++) {
            Harness h(std::string("DM6_") + names[i], pols[i]);
            uint64_t A = 0x7500000 + (uint64_t)i * 0x10000;
            {
                PACKET p = makeReadPacket(A);
                h.dcm.add_rq(&p);
            }
            quiesce(h, 20000);
            size_t base = h.llc.responses.size();

            PACKET p1 = makeReadPacket(A);
            h.dcm.add_rq(&p1);
            PACKET p2 = makeReadPacket(A);
            h.dcm.add_rq(&p2);
            bool q = quiesce(h, 200000);
            size_t delivered = h.llc.responses.size() - base;

            std::cout << "[TEST 6: " << names[i] << " same-address collision] quiesced=" << q
                       << " responses=" << delivered << " (expect 2) ORB_residual=" << h.dcm.ORB.size()
                       << std::endl;
            if (!q || delivered != 2 || !h.dcm.ORB.empty())
                allOk = false;
        }
        if (!allOk) {
            std::cerr << "  FAIL (BEAR/Oracle must behave identically -- the fix is below the policy layer)"
                      << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    if (failures == 0) {
        std::cout << "ALL DUPLICATE-MERGE TESTS PASSED" << std::endl;
        return 0;
    }
    std::cout << failures << " DUPLICATE-MERGE TEST(S) FAILED" << std::endl;
    return 1;
}
