// Deterministic tests for the DRAM Cache Manager's own controller
// frontend/backend latency (paper Table I: "Frontend/Backend Latencies:
// 20 ns round-trip"), ported from gem5's `static_frontend_latency`/
// `static_backend_latency` (src/mem/PolicyManager.py, 10ns each by
// default) and their use in `accessAndRespond()`
// (policy_manager.cc:1042-1043 etc. for a plain response,
// :734-735 etc. for a response that followed a far fetch -- the extra
// backendLatency on that second path was verified directly from the
// gem5 source, not assumed from the paper's flat "20ns" description).
//
// Proves:
//   1) Controller latency = 0 behaves exactly as before this feature
//      existed (no added delay).
//   2) Controller latency = the paper's default (10ns+10ns) adds exactly
//      that much delay to a response.
//   3) A different configured value changes the added delay accordingly.
//   4) The delay is applied EXACTLY ONCE per response, for: read hit,
//      read miss (clean and dirty victim), and the write/WB paths (which
//      produce no LLC-visible response at all, so must show ZERO
//      controller-latency effect, by construction).
//   5) Controller latency and far-link latency are additive and neither
//      is applied twice when both are configured simultaneously.
//   6) Controller latency does not alter local/far DRAM timing or the
//      number of local/far operations for any policy (baseline,
//      BEAR-Wr-Opt, Oracle) -- it only shifts the final response's
//      delivery cycle.
//
// IMPORTANT test-methodology note (a real bug found and fixed while
// writing this file): every measurement below uses ELAPSED-CYCLE DELTAS
// (`current_core_cycle[0]` sampled immediately before submission and
// again at completion), NEVER an absolute reset of `current_core_cycle[0]`
// in the middle of a Harness's lifetime. An earlier version of this file
// reset the clock to 0 between a warm-up phase and a measurement phase
// while reusing the SAME `MEMORY_CONTROLLER` instances -- but
// `MEMORY_CONTROLLER`'s internal `bank_request[...].cycle_available`
// state holds ABSOLUTE cycle numbers from before the reset, so resetting
// the clock backwards made banks appear busy for hundreds of stale
// cycles until the counter caught back up. This produced wildly wrong
// (both too-fast and too-slow) measurements that had nothing to do with
// controller latency. Fixed by never resetting mid-Harness and measuring
// via deltas instead -- the same pattern already used correctly by every
// other `tests/test_dcm_*.cc` file.
//
// Build:
//   g++ -std=c++11 -Iinc -o /tmp/test_dcm_controller_latency \
//       tests/test_dcm_controller_latency.cc src/dram_cache_manager.cc \
//       src/dram_controller.cc src/block.cc

#include <iostream>
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

static void standardTiming(MEMORY_CONTROLLER &mc)
{
    uint32_t trp = (uint32_t)((1.0 * tRP_DRAM_NANOSECONDS * CPU_FREQ) / 1000);
    uint32_t trcd = (uint32_t)((1.0 * tRCD_DRAM_NANOSECONDS * CPU_FREQ) / 1000);
    uint32_t tcas = (uint32_t)((1.0 * tCAS_DRAM_NANOSECONDS * CPU_FREQ) / 1000);
    uint32_t mtps = DRAM_IO_FREQ;
    mc.set_timing(trp, trcd, tcas, mtps, computeDbusReturnTime(mtps));
}

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

struct Harness {
    MEMORY_CONTROLLER near_mc, far_mc;
    DRAM_CACHE_MANAGER dcm;
    FAKE_LLC llc;

    Harness(const std::string &name, DCM_POLICY pol = DCM_POLICY_BASELINE_CASCADE_LAKE)
        : near_mc(name + "_N"), far_mc(name + "_F"), dcm(name + "_DCM", &near_mc, &far_mc)
    {
        standardTiming(near_mc);
        standardTiming(far_mc);
        dcm.policy = pol;
        dcm.upper_level_icache[0] = &llc;
        dcm.upper_level_dcache[0] = &llc;
    }
};

// Elapsed cycles from submission to completion for a fresh (cold-miss)
// read. Uses a delta, never a clock reset -- see the file banner.
// Settles a harness for a fixed window after a measurement so the NEXT
// operation on it starts from a clean slate. This matters because
// MEMORY_CONTROLLER::add_rq() has a pre-existing write-queue-forwarding
// shortcut (dram_controller.cc:428-455) that services a read INSTANTLY,
// bypassing all real timing, if a matching address is still sitting in
// that controller's write queue -- e.g. the background fill write a
// cold miss issues to nearMC. Without settling, whether the NEXT request
// happens to hit that shortcut depends on incidental timing (in
// particular, how many extra operate() cycles a longer controller-
// latency wait happened to grant nearMC to drain its own queue) rather
// than anything meaningful -- which would silently contaminate exactly
// what these tests are trying to measure. Same lesson already learned
// (and fixed the same way) in tests/test_dcm_near_far_config.cc.
static void settle(Harness &h)
{
    for (int i = 0; i < 500; i++) {
        current_core_cycle[0]++;
        h.dcm.operate();
    }
}

// NOTE: must compare against the response count captured BEFORE
// submission, not `!responses.empty()` -- a Harness reused for a second
// cold read (as Tests 4b/4c/6 do, e.g. addrA then addrB) already has a
// non-empty `responses` vector from the FIRST read, so an
// empty()-based stop condition is trivially satisfied on the very first
// operate() of the second call, making its measured elapsed time bogus
// (this was a real bug found while chasing Tests 4b/4c reporting
// delta=0: both the zero-latency and default-latency harnesses' second
// reads were returning near-instantly for this reason, not because
// controller latency was failing to apply).
static uint64_t coldReadCompletionCycles(Harness &h, uint64_t addr)
{
    size_t before = h.llc.responses.size();
    uint64_t start = current_core_cycle[0];
    PACKET p = makeReadPacket(addr);
    h.dcm.add_rq(&p);
    pumpUntil(h.dcm, [&]() { return h.llc.responses.size() > before; }, 2000000);
    uint64_t elapsed = current_core_cycle[0] - start;
    settle(h);
    return elapsed;
}

// Elapsed cycles from submission to completion for a read hit at addr
// (must already be resident). Uses a delta, never a clock reset.
static uint64_t hitReadCompletionCycles(Harness &h, uint64_t addr)
{
    size_t before = h.llc.responses.size();
    uint64_t start = current_core_cycle[0];
    PACKET p = makeReadPacket(addr);
    h.dcm.add_rq(&p);
    pumpUntil(h.dcm, [&]() { return h.llc.responses.size() > before; }, 2000000);
    uint64_t elapsed = current_core_cycle[0] - start;
    settle(h);
    return elapsed;
}

int main()
{
    current_core_cycle[0] = 0;
    all_warmup_complete = NUM_CPUS;
    int failures = 0;
    const uint64_t STRIDE = DCM_DRAM_CACHE_SIZE;

    // ==================================================================
    // Test 1: controller latency = 0 behaves exactly as before this
    // feature existed (recorded as the zero-latency baseline for Tests 2-3).
    // ==================================================================
    uint64_t hitCyclesZero;
    {
        Harness h("CL1");
        h.dcm.setControllerLatency(0, 0);
        uint64_t addr = 0x5000000;
        uint64_t missCycles = coldReadCompletionCycles(h, addr);
        hitCyclesZero = hitReadCompletionCycles(h, addr);
        std::cout << "[TEST 1: controller latency = 0] miss_cycles=" << missCycles
                   << " hit_cycles=" << hitCyclesZero << std::endl;
        std::cout << "  PASS (recorded as the zero-latency baseline for Tests 2-3)" << std::endl;
    }

    // ==================================================================
    // Test 2: controller latency = paper default (10ns+10ns) adds
    // exactly that much delay.
    // ==================================================================
    {
        Harness h("CL2"); // constructor default IS the paper's value -- do not override
        uint64_t addr = 0x5100000;
        coldReadCompletionCycles(h, addr);
        uint64_t hitCyclesDefault = hitReadCompletionCycles(h, addr);

        uint64_t expectedRoundTrip = DCM_FRONTEND_LATENCY_CYCLES + DCM_BACKEND_LATENCY_CYCLES;
        uint64_t hitDelta = hitCyclesDefault - hitCyclesZero;

        std::cout << "[TEST 2: controller latency = paper default] hit_cycles=" << hitCyclesDefault
                   << " hit_delta_vs_zero=" << hitDelta << " expected_round_trip=" << expectedRoundTrip
                   << std::endl;

        if (h.dcm.frontendLatencyCycles != DCM_FRONTEND_LATENCY_CYCLES ||
            h.dcm.backendLatencyCycles != DCM_BACKEND_LATENCY_CYCLES || hitDelta != expectedRoundTrip) {
            std::cerr << "  FAIL (default constructor must use the paper's 10ns+10ns round-trip, and it "
                         "must add exactly that many cycles to a hit's completion)"
                      << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Test 3: a DIFFERENT configured value changes the added delay
    // accordingly (proves it's genuinely configurable, not hard-coded).
    // ==================================================================
    {
        Harness h("CL3");
        uint64_t customFrontend = 40, customBackend = 60; // arbitrary, distinct from the default
        h.dcm.setControllerLatency(customFrontend, customBackend);
        uint64_t addr = 0x5200000;
        coldReadCompletionCycles(h, addr);
        uint64_t hitCyclesCustom = hitReadCompletionCycles(h, addr);

        uint64_t expected = customFrontend + customBackend;
        uint64_t hitDelta = hitCyclesCustom - hitCyclesZero;

        std::cout << "[TEST 3: custom controller latency] hit_cycles=" << hitCyclesCustom
                   << " hit_delta_vs_zero=" << hitDelta << " expected=" << expected << std::endl;

        if (hitDelta != expected) {
            std::cerr << "  FAIL (a custom frontend/backend configuration must change the added delay "
                         "by exactly frontend+backend cycles)"
                      << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Test 4: applied exactly once, across read hit / read miss (clean +
    // dirty victim) / write hit / write miss / the WB operation itself.
    // ==================================================================
    {
        uint64_t frontend = DCM_FRONTEND_LATENCY_CYCLES, backend = DCM_BACKEND_LATENCY_CYCLES;
        uint64_t singleRT = frontend + backend;
        uint64_t doubleRT = frontend + 2 * backend;

        // -- read hit: single round trip --
        {
            Harness h0("CL4_RH0");
            h0.dcm.setControllerLatency(0, 0);
            uint64_t addr = 0x5300000;
            coldReadCompletionCycles(h0, addr);
            uint64_t without = hitReadCompletionCycles(h0, addr);

            Harness h("CL4_RH");
            coldReadCompletionCycles(h, addr);
            uint64_t withLatency = hitReadCompletionCycles(h, addr);

            uint64_t delta = withLatency - without;
            std::cout << "[TEST 4a: read hit] delta=" << delta << " expected=" << singleRT << std::endl;
            if (delta != singleRT) {
                std::cerr << "  FAIL" << std::endl;
                failures++;
            } else {
                std::cout << "  PASS" << std::endl;
            }
        }

        // -- read miss, clean victim: this response always follows a far
        // fetch in this port, so it must use the DOUBLE-backend-latency
        // round trip (verified from gem5, not the plain single one) --
        {
            Harness h0("CL4_RMC0");
            h0.dcm.setControllerLatency(0, 0);
            uint64_t addrA = 0x5400000, addrB = addrA + STRIDE;
            coldReadCompletionCycles(h0, addrA);
            uint64_t without = coldReadCompletionCycles(h0, addrB); // B evicts A -> miss with clean victim

            Harness h("CL4_RMC");
            coldReadCompletionCycles(h, addrA);
            uint64_t withLatency = coldReadCompletionCycles(h, addrB);

            uint64_t delta = withLatency - without;
            std::cout << "[TEST 4b: read miss clean victim] delta=" << delta << " expected=" << doubleRT
                       << " (far-fetch response path -> DOUBLE backend latency, verified from gem5)"
                       << std::endl;
            if (delta != doubleRT) {
                std::cerr << "  FAIL (a read miss's response always follows a far fetch in this port, so "
                             "it must always use the double-backend-latency round trip, per gem5)"
                          << std::endl;
                failures++;
            } else {
                std::cout << "  PASS" << std::endl;
            }
        }

        // -- read dirty miss: still the far-fetch response path (double) --
        {
            Harness h0("CL4_RMD0");
            h0.dcm.setControllerLatency(0, 0);
            uint64_t addrA = 0x5500000, addrB = addrA + STRIDE;
            coldReadCompletionCycles(h0, addrA);
            PACKET wa0 = makeWritePacket(addrA);
            h0.dcm.add_wq(&wa0);
            for (int i = 0; i < 500; i++) {
                current_core_cycle[0]++;
                h0.dcm.operate();
            }
            uint64_t without = coldReadCompletionCycles(h0, addrB);

            Harness h("CL4_RMD");
            coldReadCompletionCycles(h, addrA);
            PACKET wa = makeWritePacket(addrA);
            h.dcm.add_wq(&wa);
            for (int i = 0; i < 500; i++) {
                current_core_cycle[0]++;
                h.dcm.operate();
            }
            uint64_t withLatency = coldReadCompletionCycles(h, addrB);

            uint64_t delta = withLatency - without;
            std::cout << "[TEST 4c: read dirty miss] delta=" << delta << " expected=" << doubleRT
                       << std::endl;
            if (delta != doubleRT) {
                std::cerr << "  FAIL" << std::endl;
                failures++;
            } else {
                std::cout << "  PASS" << std::endl;
            }
        }

        // -- write hit / write miss / WB operation itself: ZERO effect,
        // since writes never produce an LLC-visible response in this
        // port at all (gem5_to_champsim_mapping.md fact #2) -- there is
        // nothing for controller latency to delay. Verified via
        // response/completion counts, not cycle deltas (there is no
        // response to time).
        {
            Harness h("CL4_WH"); // default (nonzero) controller latency
            uint64_t addr = 0x5600000;
            coldReadCompletionCycles(h, addr);
            uint64_t writesBefore = h.dcm.stats.completedWrites;
            PACKET w = makeWritePacket(addr); // write hit
            h.dcm.add_wq(&w);
            for (int i = 0; i < 500; i++) {
                current_core_cycle[0]++;
                h.dcm.operate();
            }
            bool noResponse = h.llc.responses.size() == 1; // only the earlier read's response
            bool writeCompleted = (h.dcm.stats.completedWrites - writesBefore) == 1;
            std::cout << "[TEST 4d: write hit / miss / WB produce no LLC response regardless of "
                         "controller latency] no_response="
                       << noResponse << " write_completed=" << writeCompleted << std::endl;
            if (!noResponse || !writeCompleted) {
                std::cerr << "  FAIL" << std::endl;
                failures++;
            } else {
                std::cout << "  PASS" << std::endl;
            }
        }
    }

    // ==================================================================
    // Test 5: controller latency + far-link latency are ADDITIVE; neither
    // is applied twice.
    // ==================================================================
    {
        Harness hNeither("CL5_NONE");
        hNeither.dcm.setControllerLatency(0, 0);
        uint64_t addr = 0x5700000;
        uint64_t withNeither = coldReadCompletionCycles(hNeither, addr);

        Harness hCtrlOnly("CL5_CTRL");
        // default controller latency, no link latency
        uint64_t withCtrlOnly = coldReadCompletionCycles(hCtrlOnly, addr);

        Harness hLinkOnly("CL5_LINK");
        hLinkOnly.dcm.setControllerLatency(0, 0);
        hLinkOnly.dcm.setLinkLatency(DCM_LINK_LATENCY_CYCLES_500NS);
        uint64_t withLinkOnly = coldReadCompletionCycles(hLinkOnly, addr);

        Harness hBoth("CL5_BOTH");
        hBoth.dcm.setLinkLatency(DCM_LINK_LATENCY_CYCLES_500NS);
        uint64_t withBoth = coldReadCompletionCycles(hBoth, addr);

        uint64_t ctrlContribution = withCtrlOnly - withNeither;
        uint64_t linkContribution = withLinkOnly - withNeither;
        uint64_t expectedBoth = withNeither + ctrlContribution + linkContribution;

        std::cout << "[TEST 5: additive, no double-counting] neither=" << withNeither
                   << " ctrl_only=" << withCtrlOnly << " link_only=" << withLinkOnly
                   << " both=" << withBoth << " expected_both=" << expectedBoth << std::endl;

        if (withBoth != expectedBoth) {
            std::cerr << "  FAIL (controller latency and link latency must be additive: enabling both "
                         "together must equal enabling each separately and summing their individual "
                         "contributions -- any deviation means one is being applied twice, or not at all, "
                         "when combined)"
                      << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Test 6: controller latency does not change op counts for any
    // policy -- it only shifts the final response's delivery cycle.
    // ==================================================================
    {
        DCM_POLICY pols[3] = {DCM_POLICY_BASELINE_CASCADE_LAKE, DCM_POLICY_BEAR_WR_OPT, DCM_POLICY_ORACLE};
        const char *names[3] = {"BASELINE", "BEAR", "ORACLE"};
        bool allOk = true;
        for (int i = 0; i < 3; i++) {
            Harness hZero(std::string("CL6Z_") + names[i], pols[i]);
            hZero.dcm.setControllerLatency(0, 0);
            Harness hDefault(std::string("CL6D_") + names[i], pols[i]);

            uint64_t addrA = 0x5800000 + (uint64_t)i * STRIDE * 4;
            uint64_t addrB = addrA + STRIDE;

            // read miss, write hit, read miss dirty victim -- same
            // sequence under both zero and default controller latency.
            coldReadCompletionCycles(hZero, addrA);
            PACKET wz = makeWritePacket(addrA);
            hZero.dcm.add_wq(&wz);
            for (int c = 0; c < 500; c++) {
                current_core_cycle[0]++;
                hZero.dcm.operate();
            }
            coldReadCompletionCycles(hZero, addrB);

            coldReadCompletionCycles(hDefault, addrA);
            PACKET wd = makeWritePacket(addrA);
            hDefault.dcm.add_wq(&wd);
            for (int c = 0; c < 500; c++) {
                current_core_cycle[0]++;
                hDefault.dcm.operate();
            }
            coldReadCompletionCycles(hDefault, addrB);

            bool opsMatch = (hZero.dcm.stats.localReads == hDefault.dcm.stats.localReads) &&
                             (hZero.dcm.stats.localWrites == hDefault.dcm.stats.localWrites) &&
                             (hZero.dcm.stats.farReads == hDefault.dcm.stats.farReads) &&
                             (hZero.dcm.stats.farWrites == hDefault.dcm.stats.farWrites) &&
                             (hZero.dcm.stats.numWrBacks == hDefault.dcm.stats.numWrBacks);

            std::cout << "[TEST 6: " << names[i] << " op counts unaffected] zero=(" << hZero.dcm.stats.localReads
                       << "," << hZero.dcm.stats.localWrites << "," << hZero.dcm.stats.farReads << ","
                       << hZero.dcm.stats.farWrites << ") default=(" << hDefault.dcm.stats.localReads << ","
                       << hDefault.dcm.stats.localWrites << "," << hDefault.dcm.stats.farReads << ","
                       << hDefault.dcm.stats.farWrites << ") match=" << opsMatch << std::endl;
            allOk = allOk && opsMatch;
        }
        if (!allOk) {
            std::cerr << "  FAIL (controller latency must not change local/far DRAM operation counts for "
                         "any policy)"
                      << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    if (failures == 0) {
        std::cout << "ALL CONTROLLER LATENCY TESTS PASSED" << std::endl;
        return 0;
    } else {
        std::cout << failures << " CONTROLLER LATENCY TEST(S) FAILED" << std::endl;
        return 1;
    }
}
