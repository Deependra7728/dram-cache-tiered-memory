// Regression tests for the bandwidth-utilization statistic
// (docs/final_independent_audit.md, HIGH-1).
//
// ROOT CAUSE. print_dcm_stats() computed the ROI duration as
//     ooo_cpu[0].finish_sim_cycle - ooo_cpu[0].begin_sim_cycle
// but finish_sim_cycle is ALREADY the ROI duration -- src/main.cc
// assigns it as (current_core_cycle - begin_sim_cycle) at simulation
// completion. Subtracting begin_sim_cycle a second time double-counts
// the warmup. Worse, when the ROI is shorter in cycles than the warmup
// the unsigned subtraction WRAPS to ~1.8e19, and the `elapsedCycles > 0`
// guard does not catch it -- the run silently reported utilisations of
// ~1e-16% instead of failing. Observed directly with
// `-warmup_instructions 1000000 -simulation_instructions 100000`:
//     Warmup complete ... cycles: 477272
//     Finished CPU 0   ... cycles: 83829
//     DCM LOCAL_BW_UTILIZATION: 1.73472e-16%
//
// FIX. main.cc now passes finish_sim_cycle directly, and the arithmetic
// itself was factored into dcmBandwidthUtilization() so it is unit
// testable -- which is what this file does. The helper returns 0.0 for
// degenerate inputs rather than dividing by zero.
//
// Build:
//   g++ -std=c++11 -Iinc -o /tmp/test_dcm_bandwidth_stats \
//       tests/test_dcm_bandwidth_stats.cc src/dram_cache_manager.cc \
//       src/dram_controller.cc src/block.cc

#include <cmath>
#include <iostream>

#include "champsim.h"
#include "dram_cache_manager.h"

uint64_t current_core_cycle[NUM_CPUS];
uint8_t all_warmup_complete;

static int failures = 0;

static void expectNear(const char *what, double got, double want, double tol)
{
    bool ok = std::fabs(got - want) <= tol;
    std::cout << "  " << what << ": got=" << got << " want=" << want << (ok ? "  OK" : "  FAIL") << std::endl;
    if (!ok)
        failures++;
}

static void expectTrue(const char *what, bool ok)
{
    std::cout << "  " << what << ": " << (ok ? "OK" : "FAIL") << std::endl;
    if (!ok)
        failures++;
}

int main()
{
    current_core_cycle[0] = 0;
    all_warmup_complete = NUM_CPUS;

    // Peak bytes/cycle = DRAM_CHANNEL_WIDTH(8) * mtps / CPU_FREQ.
    // With mtps == CPU_FREQ (4000) that is exactly 8 bytes/cycle, so a
    // 64-byte block takes exactly 8 cycles of bus time. These constants
    // make every expectation below hand-checkable.
    const uint32_t mtps = (uint32_t)CPU_FREQ; // 8 B/cycle
    const uint64_t cyclesPerOp = (uint64_t)BLOCK_SIZE / (uint64_t)DRAM_CHANNEL_WIDTH; // 8

    // ==================================================================
    // Test 1: normal warmup/ROI relationship -- exact, hand-derived.
    // ==================================================================
    {
        std::cout << "[TEST 1: normal ROI, exact arithmetic]" << std::endl;
        // 1000 ops * 8 cycles/op = 8000 cycles of bus time.
        expectNear("100% utilisation", dcmBandwidthUtilization(1000, mtps, 1000 * cyclesPerOp), 1.0, 1e-12);
        expectNear("50% utilisation", dcmBandwidthUtilization(1000, mtps, 2000 * cyclesPerOp), 0.5, 1e-12);
        expectNear("25% utilisation", dcmBandwidthUtilization(500, mtps, 2000 * cyclesPerOp), 0.25, 1e-12);
        expectNear("0 ops -> 0", dcmBandwidthUtilization(0, mtps, 100000), 0.0, 1e-12);
    }

    // ==================================================================
    // Test 2: warmup cycles > ROI cycles. This is the exact scenario
    // that made the OLD expression wrap. The helper takes the ROI
    // duration directly, so a short ROI is simply a short ROI -- there
    // is no subtraction left that could underflow.
    // ==================================================================
    {
        std::cout << "[TEST 2: warmup longer than ROI]" << std::endl;
        // Mirrors the real observed run: warmup 477272 cycles, ROI 83829.
        uint64_t warmupEndCycle = 477272, roiCycles = 83829;
        double u = dcmBandwidthUtilization(1000, mtps, roiCycles);
        expectTrue("finite and positive", std::isfinite(u) && u > 0.0);
        expectTrue("not the ~1e-16 underflow artifact", u > 1e-6);
        // Demonstrate the defect the fix removes: the OLD denominator.
        uint64_t oldDenominator = roiCycles - warmupEndCycle; // unsigned wrap
        expectTrue("old expression really did wrap", oldDenominator > roiCycles);
        double uOld = dcmBandwidthUtilization(1000, mtps, oldDenominator);
        expectTrue("old expression yields absurd ~0 utilisation", uOld < 1e-12);
        std::cout << "    (old denominator = " << oldDenominator << " cycles -> " << (100.0 * uOld)
                   << "%, new = " << roiCycles << " cycles -> " << (100.0 * u) << "%)" << std::endl;
    }

    // ==================================================================
    // Test 3: very short ROI stays sane.
    // ==================================================================
    {
        std::cout << "[TEST 3: very short ROI]" << std::endl;
        double u = dcmBandwidthUtilization(1, mtps, cyclesPerOp); // exactly saturated
        expectNear("single op filling its own bus time = 100%", u, 1.0, 1e-12);
        double u2 = dcmBandwidthUtilization(1, mtps, 1); // 1 cycle, 1 op -> oversubscribed
        expectTrue("oversubscribed reports >100% rather than wrapping", std::isfinite(u2) && u2 > 1.0);
    }

    // ==================================================================
    // Test 4: degenerate inputs never divide by zero or produce NaN/inf.
    // ==================================================================
    {
        std::cout << "[TEST 4: degenerate inputs]" << std::endl;
        expectNear("zero elapsed cycles -> 0.0", dcmBandwidthUtilization(1000, mtps, 0), 0.0, 0.0);
        expectNear("zero mtps -> 0.0", dcmBandwidthUtilization(1000, 0, 1000), 0.0, 0.0);
        expectNear("all zero -> 0.0", dcmBandwidthUtilization(0, 0, 0), 0.0, 0.0);
        expectTrue("no NaN for zero elapsed", std::isfinite(dcmBandwidthUtilization(1000, mtps, 0)));
        expectTrue("no NaN for zero mtps", std::isfinite(dcmBandwidthUtilization(1000, 0, 1000)));
    }

    // ==================================================================
    // Test 5: independence between runs -- the helper is pure, so two
    // separate "runs" with identical inputs give identical results and
    // no state is carried between them.
    // ==================================================================
    {
        std::cout << "[TEST 5: run independence]" << std::endl;
        double a = dcmBandwidthUtilization(12345, mtps, 999999);
        double b = dcmBandwidthUtilization(777, mtps, 4242);
        double aAgain = dcmBandwidthUtilization(12345, mtps, 999999);
        expectTrue("pure function, repeatable", a == aAgain);
        expectTrue("distinct inputs give distinct results", a != b);
    }

    // ==================================================================
    // Test 6: the near/far configurations actually used by the paper
    // give plausible, differing peak capacities (HBM2 4000 MT/s vs
    // DDR4 2400 MT/s), so the same op count yields higher utilisation
    // on the slower far channel.
    // ==================================================================
    {
        std::cout << "[TEST 6: paper's near/far peaks]" << std::endl;
        uint64_t ops = 10000, cycles = 1000000;
        double nearU = dcmBandwidthUtilization(ops, DCM_NEAR_HBM2_MTPS, cycles);
        double farU = dcmBandwidthUtilization(ops, DCM_FAR_DDR4_MTPS, cycles);
        expectTrue("same ops -> higher utilisation on the slower far channel", farU > nearU);
        expectNear("far/near ratio == near_mtps/far_mtps", farU / nearU,
                    (double)DCM_NEAR_HBM2_MTPS / (double)DCM_FAR_DDR4_MTPS, 1e-9);
    }

    if (failures == 0) {
        std::cout << "ALL BANDWIDTH STATS TESTS PASSED" << std::endl;
        return 0;
    }
    std::cout << failures << " BANDWIDTH STATS TEST(S) FAILED" << std::endl;
    return 1;
}
