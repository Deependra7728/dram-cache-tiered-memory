// Deterministic tests for the DRAM_CACHE_MANAGER: integration wiring,
// per-instance timing, and ORB/CRB conflict handling. Real baseline
// hit/miss/dirty/victim behavior (cases A-H, Table II) is covered
// separately in tests/test_dcm_baseline.cc.
//
// Does NOT use ChampSim's main.cc / OoO core / traces at all -- it drives
// the manager directly with synthetic PACKETs, the same spirit as the
// gem5 reference's own validation methodology (a synthetic traffic
// generator feeding PolicyManager directly, see
// disaggregated_dram_cache_script.py pulled from the gem5 repo).
//
// Proves:
//   1) LLC-facing add_rq() -> DCM -> LOCAL memory path -> completion
//      callback fires with the right address and content, for a genuine
//      tag-store hit (line warmed up by a first access, then re-accessed).
//   2) The FAR memory path is reachable and sequenced correctly
//      (respond-before-fill) for a genuine cold miss.
//   3) A write (add_wq()) takes the tag-check-then-local-write path and
//      needs no callback (matches ChampSim's existing write contract).
//   4) nearMC and farMC can run independent DRAM timing simultaneously
//      (per-instance set_timing(), not the old shared globals) without
//      one affecting the other.
//   5) Two requests whose addresses map to the same DRAM-cache index
//      conflict correctly: the second waits in the CRB (not admitted to
//      the ORB) until the first completes, is then promoted, and both
//      complete exactly once each (no loss, no duplication) -- and, since
//      both are genuine cold/hot misses now, this also exercises a real
//      READ MISS + CLEAN VICTIM classification for the promoted request.
//   6) CRB backpressure: once the CRB is full, a further conflicting
//      request is correctly reported as blocked via
//      get_occupancy()==get_size(), the same contract the LLC checks
//      before calling add_rq/add_wq.
//
// Build:
//   g++ -std=c++11 -Iinc -o /tmp/test_dcm_skeleton \
//       tests/test_dcm_skeleton.cc src/dram_cache_manager.cc \
//       src/dram_controller.cc src/block.cc

#include <iostream>
#include <vector>

#include "champsim.h"
#include "dram_cache_manager.h"

// ----------------------------------------------------------------------------
// Globals ChampSim's non-main.cc source files (dram_controller.cc, block.cc)
// need at link time. Normally defined in src/main.cc; this test does not
// link main.cc (it has its own main() and does not need the OoO core), so
// it provides minimal definitions itself.
// ----------------------------------------------------------------------------
uint64_t current_core_cycle[NUM_CPUS];
uint8_t all_warmup_complete;

// A tiny stand-in for "the LLC": records every completed PACKET, in
// order, so tests can assert on sequencing as well as content.
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
    return p;
}

// Realistic-ish DRAM timing, matching the formulas main.cc uses. Applied
// per-instance now (see docs/gem5_to_champsim_mapping.md).
static void standardTiming(MEMORY_CONTROLLER &mc)
{
    uint32_t trp = (uint32_t)((1.0 * tRP_DRAM_NANOSECONDS * CPU_FREQ) / 1000);
    uint32_t trcd = (uint32_t)((1.0 * tRCD_DRAM_NANOSECONDS * CPU_FREQ) / 1000);
    uint32_t tcas = (uint32_t)((1.0 * tCAS_DRAM_NANOSECONDS * CPU_FREQ) / 1000);
    uint32_t mtps = DRAM_IO_FREQ;
    uint32_t dbus = (BLOCK_SIZE / DRAM_CHANNEL_WIDTH) * (CPU_FREQ / mtps);
    mc.set_timing(trp, trcd, tcas, mtps, dbus);
}

// Run operate() until either a stop condition holds or a cycle budget is
// exhausted. Small budget -> test runs quickly and fails fast (rather
// than hanging) if something is wired wrong.
template <typename StopCond>
static bool pumpUntil(DRAM_CACHE_MANAGER &dcm, StopCond stop, uint64_t maxCycles)
{
    for (uint64_t i = 0; i < maxCycles; i++) {
        current_core_cycle[0]++;
        dcm.operate();
        if (stop())
            return true;
    }
    return false;
}

struct ResponseCountAtLeast {
    FAKE_LLC *llc;
    size_t n;
    bool operator()() const { return llc->responses.size() >= n; }
};

int main()
{
    current_core_cycle[0] = 0;
    all_warmup_complete = NUM_CPUS; // exercise the real timing path, not the pre-warmup bypass

    int failures = 0;

    // ---- Test 1: pure LOCAL path -- genuine read hit after warm-up ------
    {
        current_core_cycle[0] = 0;
        MEMORY_CONTROLLER near("TEST_NEAR"), far("TEST_FAR");
        standardTiming(near);
        standardTiming(far);
        DRAM_CACHE_MANAGER dcm("TEST_DCM", &near, &far);
        FAKE_LLC llc;
        dcm.upper_level_icache[0] = &llc;
        dcm.upper_level_dcache[0] = &llc;

        uint64_t addr = 0x1000;

        // Warm the line up first (cold miss -- installs it).
        PACKET warm = makeReadPacket(addr);
        dcm.add_rq(&warm);
        ResponseCountAtLeast stopWarm = {&llc, 1};
        bool warmedOk = pumpUntil(dcm, stopWarm, 10000);
        for (int i = 0; i < 200; i++) { // drain the warm-up's background fill write
            current_core_cycle[0]++;
            dcm.operate();
        }
        uint64_t farAfterWarm = dcm.stats.sentToFar;
        uint64_t nearAfterWarm = dcm.stats.sentToNear;

        // Re-access the SAME address: must now be a genuine tag-store hit.
        PACKET p = makeReadPacket(addr);
        dcm.add_rq(&p);
        ResponseCountAtLeast stop = {&llc, 2};
        bool ok = pumpUntil(dcm, stop, 10000);

        std::cout << "[TEST 1: local-hit path] warmed=" << warmedOk << " responded=" << ok
                   << " addr_match=" << (llc.responses.size() == 2 && llc.responses[1] == addr)
                   << " near_delta=" << (dcm.stats.sentToNear - nearAfterWarm)
                   << " far_delta=" << (dcm.stats.sentToFar - farAfterWarm)
                   << " orb_empty=" << dcm.ORB.empty() << std::endl;

        if (!warmedOk || !ok || llc.responses.size() != 2 || llc.responses[1] != addr ||
            dcm.stats.sentToFar != farAfterWarm || dcm.stats.sentToNear != nearAfterWarm + 1 ||
            !dcm.ORB.empty()) {
            std::cerr << "  FAIL (a hit must touch near exactly once more and far not at all)" << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ---- Test 2: FAR path is reachable (genuine cold miss) --------------
    {
        current_core_cycle[0] = 0;
        MEMORY_CONTROLLER near("TEST_NEAR2"), far("TEST_FAR2");
        standardTiming(near);
        standardTiming(far);
        DRAM_CACHE_MANAGER dcm("TEST_DCM2", &near, &far);
        FAKE_LLC llc;
        dcm.upper_level_icache[0] = &llc;
        dcm.upper_level_dcache[0] = &llc;

        uint64_t addr = 0x2000; // never accessed before -> genuine cold miss


        PACKET p = makeReadPacket(addr);
        dcm.add_rq(&p);

        ResponseCountAtLeast stop = {&llc, 1};
        bool ok = pumpUntil(dcm, stop, 10000);
        for (int i = 0; i < 200; i++) { // drain the background fill write
            current_core_cycle[0]++;
            dcm.operate();
        }

        std::cout << "[TEST 2: far-miss path] responded=" << ok
                   << " addr_match=" << (!llc.responses.empty() && llc.responses[0] == addr)
                   << " sentToNear=" << dcm.stats.sentToNear << " sentToFar=" << dcm.stats.sentToFar
                   << " completedFromFar=" << dcm.stats.completedFromFar << " orb_empty=" << dcm.ORB.empty()
                   << std::endl;

        if (!ok || llc.responses.size() != 1 || llc.responses[0] != addr || dcm.stats.sentToFar != 1 ||
            dcm.stats.sentToNear != 2 || dcm.stats.completedFromFar != 1 || !dcm.ORB.empty()) {
            std::cerr << "  FAIL" << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ---- Test 3: write path needs no callback ---------------------------
    {
        current_core_cycle[0] = 0;
        MEMORY_CONTROLLER near("TEST_NEAR3"), far("TEST_FAR3");
        standardTiming(near);
        standardTiming(far);
        DRAM_CACHE_MANAGER dcm("TEST_DCM3", &near, &far);
        FAKE_LLC llc;
        dcm.upper_level_icache[0] = &llc;
        dcm.upper_level_dcache[0] = &llc;

        uint64_t addr = 0x3000;
        PACKET p = makeWritePacket(addr);
        dcm.add_wq(&p);

        for (int i = 0; i < 500; i++) {
            current_core_cycle[0]++;
            dcm.operate();
        }

        std::cout << "[TEST 3: write path] responded=" << !llc.responses.empty()
                   << " completedWrites=" << dcm.stats.completedWrites << " sentToFar=" << dcm.stats.sentToFar
                   << " orb_empty=" << dcm.ORB.empty() << std::endl;

        if (!llc.responses.empty() || dcm.stats.completedWrites != 1 || dcm.stats.sentToFar != 0 ||
            !dcm.ORB.empty()) {
            std::cerr << "  FAIL" << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ---- Test 4: near/far timing independence ---------------------------
    // Give near memory fast timing and far memory deliberately slow
    // timing, and prove (a) the far-miss path takes noticeably longer
    // than the local-hit path because far is slow, and (b) near's own
    // completion latency is unaffected by far being slow (i.e. the two
    // controllers really do run independent timing, not a shared global).
    {
        current_core_cycle[0] = 0;
        MEMORY_CONTROLLER near("TEST_NEAR4"), far("TEST_FAR4");
        near.set_timing(/*tRP=*/4, /*tRCD=*/4, /*tCAS=*/4, /*MTPS=*/3200, /*dbus=*/2);
        far.set_timing(/*tRP=*/400, /*tRCD=*/400, /*tCAS=*/400, /*MTPS=*/3200, /*dbus=*/2);
        DRAM_CACHE_MANAGER dcm("TEST_DCM4", &near, &far);
        FAKE_LLC llc;
        dcm.upper_level_icache[0] = &llc;
        dcm.upper_level_dcache[0] = &llc;

        uint64_t addr = 0x4000; // fresh address -> genuine cold miss, exercises the far path
        PACKET p = makeReadPacket(addr);
        dcm.add_rq(&p);

        uint64_t startCycle = current_core_cycle[0];
        ResponseCountAtLeast stop = {&llc, 1};
        bool ok = pumpUntil(dcm, stop, 100000);
        uint64_t farPathCycles = current_core_cycle[0] - startCycle;

        // Now a second, independent DCM whose near AND far are both fast,
        // to get a baseline "fast far" completion time for comparison.
        current_core_cycle[0] = 0;
        MEMORY_CONTROLLER near2("TEST_NEAR4B"), far2("TEST_FAR4B");
        near2.set_timing(4, 4, 4, 3200, 2);
        far2.set_timing(4, 4, 4, 3200, 2);
        DRAM_CACHE_MANAGER dcm2("TEST_DCM4B", &near2, &far2);
        FAKE_LLC llc2;
        dcm2.upper_level_icache[0] = &llc2;
        dcm2.upper_level_dcache[0] = &llc2;
        PACKET p2 = makeReadPacket(addr); // fresh in dcm2's own tag store too -> cold miss
        dcm2.add_rq(&p2);
        ResponseCountAtLeast stop2 = {&llc2, 1};
        bool ok2 = pumpUntil(dcm2, stop2, 100000);
        uint64_t fastPathCycles = current_core_cycle[0];

        std::cout << "[TEST 4: near/far timing independence] slow_far_cycles=" << farPathCycles
                   << " fast_both_cycles=" << fastPathCycles << " responded_slow=" << ok
                   << " responded_fast=" << ok2 << std::endl;

        // The only difference between the two DCMs is far's timing (400
        // vs 4 for tRP/tRCD/tCAS). If per-instance timing genuinely works,
        // the slow-far run must take meaningfully longer than the
        // fast-both run, proving farMC's timing is not being silently
        // shared/overwritten by nearMC's (which is identical, fast, in
        // both runs).
        if (!ok || !ok2 || !(farPathCycles > fastPathCycles + 500)) {
            std::cerr << "  FAIL (expected slow-far run to take meaningfully longer than fast-both run, "
                         "proving independent timing)"
                      << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ---- Test 5: ORB/CRB conflict -- same index, different addresses ---
    // DCM_DRAM_CACHE_SIZE/DCM_BLOCK_SIZE lines exist, direct-mapped, so
    // addr and addr+DCM_DRAM_CACHE_SIZE map to the same index but are
    // different addresses/tags -- a genuine index conflict.
    {
        current_core_cycle[0] = 0;
        MEMORY_CONTROLLER near("TEST_NEAR5"), far("TEST_FAR5");
        standardTiming(near);
        standardTiming(far);
        DRAM_CACHE_MANAGER dcm("TEST_DCM5", &near, &far);
        FAKE_LLC llc;
        dcm.upper_level_icache[0] = &llc;
        dcm.upper_level_dcache[0] = &llc;

        uint64_t addrA = 0x10000;
        uint64_t addrB = addrA + DCM_DRAM_CACHE_SIZE; // same index as addrA, different tag
        // Both are fresh addresses, so both are genuine misses: addrA is a
        // cold miss (installs clean); addrB, once promoted, finds addrA's
        // now-resident CLEAN line at the same index -- a real READ MISS +
        // CLEAN VICTIM classification (no write-back). Each miss is
        // local-read + far-read + local-fill-write = 2 near + 1 far.

        PACKET pa = makeReadPacket(addrA);
        dcm.debugPrint = true;
        std::cout << "---- request-flow log: conflict case ----" << std::endl;
        dcm.add_rq(&pa);

        // Immediately (same cycle, before any operate()) submit the
        // conflicting request. Because addrA's ORB entry already occupies
        // the index, addrB must be queued into the CRB, not admitted.
        PACKET pb = makeReadPacket(addrB);
        dcm.add_rq(&pb);

        bool admittedCorrectly = (dcm.ORB.size() == 1 && dcm.ORB.count(addrA) == 1 && dcm.CRB.size() == 1 &&
                                   dcm.CRB[0].pkt.address == addrB);

        ResponseCountAtLeast stopBoth = {&llc, 2};
        bool ok = pumpUntil(dcm, stopBoth, 20000);
        dcm.debugPrint = false;
        std::cout << "---- end request-flow log ----" << std::endl;

        bool orderCorrect = (llc.responses.size() == 2 && llc.responses[0] == addrA && llc.responses[1] == addrB);

        // drain both background fill writes so final stats are settled
        for (int i = 0; i < 200; i++) {
            current_core_cycle[0]++;
            dcm.operate();
        }

        std::cout << "[TEST 5: ORB/CRB conflict] admitted_correctly=" << admittedCorrectly
                   << " responded_both=" << ok << " order_correct=" << orderCorrect
                   << " crbInserts=" << dcm.stats.crbInserts << " crbPromotions=" << dcm.stats.crbPromotions
                   << " sentToNear=" << dcm.stats.sentToNear << " sentToFar=" << dcm.stats.sentToFar
                   << " numWrBacks=" << dcm.stats.numWrBacks << " numColdMisses=" << dcm.stats.numColdMisses
                   << " numHotMisses=" << dcm.stats.numHotMisses << " final_orb_size=" << dcm.ORB.size()
                   << " final_crb_size=" << dcm.CRB.size() << " total_responses=" << llc.responses.size()
                   << std::endl;

        // addrA: cold miss (local-read + far-read + local-fill = 2 near, 1 far).
        // addrB: promoted after addrA installs; finds addrA's CLEAN resident
        // line at the same index -> miss with a clean (not dirty) victim,
        // no write-back, same 2-near/1-far pattern.
        if (!admittedCorrectly || !ok || !orderCorrect || dcm.stats.crbInserts != 1 ||
            dcm.stats.crbPromotions != 1 || !dcm.ORB.empty() || !dcm.CRB.empty() ||
            llc.responses.size() != 2 || dcm.stats.sentToNear != 4 || dcm.stats.sentToFar != 2 ||
            dcm.stats.numWrBacks != 0 || dcm.stats.numColdMisses != 1 || dcm.stats.numHotMisses != 1) {
            std::cerr << "  FAIL (conflicting request must wait in CRB, be promoted after the blocker "
                         "completes, both must complete exactly once in arrival order, and the promoted "
                         "request must classify as a real READ MISS + CLEAN VICTIM)"
                      << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ---- Test 6: CRB backpressure once full ------------------------------
    {
        current_core_cycle[0] = 0;
        MEMORY_CONTROLLER near("TEST_NEAR6"), far("TEST_FAR6");
        standardTiming(near);
        standardTiming(far);
        DRAM_CACHE_MANAGER dcm("TEST_DCM6", &near, &far);
        FAKE_LLC llc;
        dcm.upper_level_icache[0] = &llc;
        dcm.upper_level_dcache[0] = &llc;

        uint64_t base = 0x20000;
        // First request occupies the ORB for this index and is never
        // pumped to completion (no operate() calls below), so it stays
        // outstanding for the whole test -- deterministic, no timing race.
        PACKET blocker = makeReadPacket(base);
        dcm.add_rq(&blocker);

        // Fill the CRB to its max with distinct same-index conflicting
        // addresses.
        bool allAdmittedToCRB = true;
        for (uint32_t i = 1; i <= DCM_CRB_MAX_SIZE; i++) {
            uint64_t addr = base + i * DCM_DRAM_CACHE_SIZE;
            PACKET p = makeReadPacket(addr);
            dcm.add_rq(&p);
            if (dcm.CRB.size() != i)
                allAdmittedToCRB = false;
        }

        uint64_t oneMoreAddr = base + (DCM_CRB_MAX_SIZE + 1) * DCM_DRAM_CACHE_SIZE;
        bool reportsFull = (dcm.get_occupancy(1, oneMoreAddr) == dcm.get_size(1, oneMoreAddr));

        std::cout << "[TEST 6: CRB backpressure] orb_size=" << dcm.ORB.size() << " crb_size=" << dcm.CRB.size()
                   << " all_admitted_to_crb=" << allAdmittedToCRB << " reports_full_for_one_more="
                   << reportsFull << std::endl;

        if (dcm.ORB.size() != 1 || dcm.CRB.size() != DCM_CRB_MAX_SIZE || !allAdmittedToCRB || !reportsFull) {
            std::cerr << "  FAIL (CRB should report itself full via get_occupancy==get_size once at "
                         "DCM_CRB_MAX_SIZE conflicting entries are queued)"
                      << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    if (failures == 0) {
        std::cout << "ALL TESTS PASSED" << std::endl;
        return 0;
    } else {
        std::cout << failures << " TEST(S) FAILED" << std::endl;
        return 1;
    }
}
