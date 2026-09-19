// Deterministic tests for the manager<->far-memory link latency (paper
// Case Study 3: 100/500/1000 ns).
//
// Proves:
//   1) Increasing link latency produces a corresponding increase in
//      completion time for a request that touches far memory (a miss).
//   2) The increase closely matches the configured extra cycles (applied
//      exactly once per far-bound dispatch, not doubled/omitted).
//   3) A local-only hit's completion time is COMPLETELY unaffected by
//      link latency, at any configured value -- it never touches the far
//      dispatch path at all.
//   4) The link latency is also applied to the far-write path (dirty
//      victim write-back), not just the far-read fetch.
//
// Build:
//   g++ -std=c++11 -Iinc -o /tmp/test_dcm_link_latency \
//       tests/test_dcm_link_latency.cc src/dram_cache_manager.cc \
//       src/dram_controller.cc src/block.cc

#include <iostream>
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

// Runs a single cold-miss read to completion on a fresh DCM configured
// with the given link latency; returns total cycles to completion.
static uint64_t missCompletionCycles(uint64_t linkLatencyCycles, uint64_t addr)
{
    current_core_cycle[0] = 0;
    MEMORY_CONTROLLER near("LL_NEAR"), far("LL_FAR");
    standardTiming(near);
    standardTiming(far);
    DRAM_CACHE_MANAGER dcm("LL_DCM", &near, &far);
    dcm.setLinkLatency(linkLatencyCycles);
    FAKE_LLC llc;
    dcm.upper_level_icache[0] = &llc;
    dcm.upper_level_dcache[0] = &llc;

    PACKET p = makeReadPacket(addr);
    dcm.add_rq(&p);
    pumpUntil(dcm, [&]() { return !llc.responses.empty(); }, 2000000);
    return current_core_cycle[0];
}

int main()
{
    current_core_cycle[0] = 0;
    all_warmup_complete = NUM_CPUS;
    int failures = 0;

    // ---- Test 1/2: increasing link latency increases miss completion time ----
    {
        uint64_t addr = 0xD00000;
        uint64_t base = missCompletionCycles(0, addr);
        uint64_t c100 = missCompletionCycles(DCM_LINK_LATENCY_CYCLES_100NS, addr + DCM_DRAM_CACHE_SIZE);
        uint64_t c500 = missCompletionCycles(DCM_LINK_LATENCY_CYCLES_500NS, addr + 2 * DCM_DRAM_CACHE_SIZE);
        uint64_t c1000 = missCompletionCycles(DCM_LINK_LATENCY_CYCLES_1000NS, addr + 3 * DCM_DRAM_CACHE_SIZE);

        std::cout << "[TEST 1/2: link latency scaling] base(0ns)=" << base << " 100ns=" << c100
                   << " 500ns=" << c500 << " 1000ns=" << c1000 << " (100ns="
                   << DCM_LINK_LATENCY_CYCLES_100NS << "cyc 500ns=" << DCM_LINK_LATENCY_CYCLES_500NS
                   << "cyc 1000ns=" << DCM_LINK_LATENCY_CYCLES_1000NS << "cyc)" << std::endl;

        // Monotonically increasing, and each step's increase should be
        // close to the configured extra latency (applied exactly once,
        // on the far-read dispatch only -- not doubled for
        // request+response, since the link-latency hold happens before
        // dispatch and the response path is the normal MEMORY_CONTROLLER
        // callback, unmodified).
        bool monotonic = (c100 > base) && (c500 > c100) && (c1000 > c500);
        uint64_t delta100 = c100 - base;
        uint64_t delta500minus100 = c500 - c100;
        uint64_t delta1000minus500 = c1000 - c500;
        uint64_t expected400ns = DCM_LINK_LATENCY_CYCLES_500NS - DCM_LINK_LATENCY_CYCLES_100NS; // 500-100=400ns step
        uint64_t expected500ns = DCM_LINK_LATENCY_CYCLES_1000NS - DCM_LINK_LATENCY_CYCLES_500NS; // 1000-500=500ns step

        // Allow generous tolerance (+-20%) for DRAM scheduling jitter
        // around the exact added delay -- the point is "applied exactly
        // once, correctly", not cycle-perfect equality with a
        // hand-derived number that ignores scheduling effects.
        bool delta100Ok = delta100 >= (DCM_LINK_LATENCY_CYCLES_100NS * 8 / 10) &&
                           delta100 <= (DCM_LINK_LATENCY_CYCLES_100NS * 12 / 10 + 50);
        bool delta500Ok = delta500minus100 >= (expected400ns * 8 / 10) && delta500minus100 <= (expected400ns * 12 / 10 + 50);
        bool delta1000Ok =
            delta1000minus500 >= (expected500ns * 8 / 10) && delta1000minus500 <= (expected500ns * 12 / 10 + 50);

        std::cout << "  delta100=" << delta100 << " (expect ~" << DCM_LINK_LATENCY_CYCLES_100NS << ")"
                   << " delta500-100=" << delta500minus100 << " (expect ~" << expected400ns << ")"
                   << " delta1000-500=" << delta1000minus500 << " (expect ~" << expected500ns << ")"
                   << std::endl;

        if (!monotonic || !delta100Ok || !delta500Ok || !delta1000Ok) {
            std::cerr << "  FAIL (each link-latency increase must add approximately the configured extra "
                         "cycles, applied exactly once)"
                      << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ---- Test 3: local-only hits are completely unaffected by link latency ----
    {
        current_core_cycle[0] = 0;
        MEMORY_CONTROLLER near("LL_NEAR3"), far("LL_FAR3");
        standardTiming(near);
        standardTiming(far);
        DRAM_CACHE_MANAGER dcm("LL_DCM3", &near, &far);
        // This test isolates LINK latency specifically -- controller
        // (frontend/backend) latency is a separate, independently-tested
        // feature (tests/test_dcm_controller_latency.cc) that, by design,
        // adds a fixed delay to EVERY read response including hits. Since
        // DRAM_CACHE_MANAGER now defaults to the paper's nonzero
        // controller latency (20ns round-trip), leaving it at that
        // default here would conflate the two latency sources and this
        // test's absolute-cycle-count assertion below would no longer
        // isolate link latency's effect specifically. Disabling it here
        // keeps this test's original, narrower claim intact: a local hit
        // is unaffected by link latency, full stop.
        dcm.setControllerLatency(0, 0);
        FAKE_LLC llc;
        dcm.upper_level_icache[0] = &llc;
        dcm.upper_level_dcache[0] = &llc;

        uint64_t addr = 0xE00000;
        // Warm up with NO link latency (fast, deterministic).
        PACKET w = makeReadPacket(addr);
        dcm.add_rq(&w);
        pumpUntil(dcm, [&]() { return !llc.responses.empty(); }, 20000);
        pumpUntil(dcm, [&]() { return near.get_occupancy(2, addr) == 0; }, 20000);

        // NOW turn on a large link latency and do a pure hit.
        dcm.setLinkLatency(DCM_LINK_LATENCY_CYCLES_1000NS);
        uint64_t start = current_core_cycle[0];
        PACKET hit = makeReadPacket(addr);
        dcm.add_rq(&hit);
        pumpUntil(dcm, [&]() { return llc.responses.size() == 2; }, 20000);
        uint64_t hitCycles = current_core_cycle[0] - start;

        std::cout << "[TEST 3: hit unaffected by link latency] hit_cycles_with_1000ns_link=" << hitCycles
                   << " sentToFar=" << dcm.stats.sentToFar << " pending_far_dispatches="
                   << dcm.pendingFarDispatches.size() << std::endl;

        // A hit should complete in a handful of cycles regardless of
        // link latency (it never reaches dispatchToFar at all), and
        // sentToFar must still be exactly 1 (only the warm-up miss).
        if (hitCycles > 100 || dcm.stats.sentToFar != 1 || !dcm.pendingFarDispatches.empty()) {
            std::cerr << "  FAIL (a local hit must never be delayed by far-link latency)" << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ---- Test 4: link latency also applies to the dirty write-back path ----
    {
        current_core_cycle[0] = 0;
        MEMORY_CONTROLLER near("LL_NEAR4"), far("LL_FAR4");
        standardTiming(near);
        standardTiming(far);
        DRAM_CACHE_MANAGER dcm("LL_DCM4", &near, &far);
        dcm.setLinkLatency(DCM_LINK_LATENCY_CYCLES_500NS);
        FAKE_LLC llc;
        dcm.upper_level_icache[0] = &llc;
        dcm.upper_level_dcache[0] = &llc;

        uint64_t addrA = 0xF00000;
        uint64_t addrB = addrA + DCM_DRAM_CACHE_SIZE; // same index

        // Install A, dirty it, then evict it with B -> triggers a
        // dirty write-back, which is far-bound and must go through the
        // same link-latency hold.
        PACKET ra = makeReadPacket(addrA);
        dcm.add_rq(&ra);
        pumpUntil(dcm, [&]() { return llc.responses.size() == 1; }, 2000000);
        PACKET wa = makeWritePacket(addrA);
        dcm.add_wq(&wa);
        for (int i = 0; i < 2000; i++) {
            current_core_cycle[0]++;
            dcm.operate();
        }

        PACKET rb = makeReadPacket(addrB);
        dcm.add_rq(&rb);
        // The write-back is pushed only once B's tag-check read
        // PHYSICALLY completes (matching gem5's exact trigger point, see
        // gem5_to_champsim_mapping.md fact #7) -- not synchronously at
        // admission. Pump forward enough cycles for that tag-check read
        // to finish (standardTiming's tRP+tRCD+tCAS is small, well under
        // 500ns-worth of cycles) but well short of the full link-latency
        // + far-processing time, then check the write-back is being held.
        pumpUntil(dcm, [&]() { return !dcm.pendingFarDispatches.empty(); }, 300);
        bool wasHeld = !dcm.pendingFarDispatches.empty();

        pumpUntil(dcm, [&]() { return llc.responses.size() == 2; }, 2000000);

        std::cout << "[TEST 4: link latency applies to write-back] was_held=" << wasHeld
                   << " numWrBacks=" << dcm.stats.numWrBacks << " farWrites=" << dcm.stats.farWrites
                   << std::endl;

        if (!wasHeld || dcm.stats.numWrBacks != 1) {
            std::cerr << "  FAIL (the dirty write-back must also be held for link latency before reaching "
                         "farMC)"
                      << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    if (failures == 0) {
        std::cout << "ALL LINK LATENCY TESTS PASSED" << std::endl;
        return 0;
    } else {
        std::cout << failures << " LINK LATENCY TEST(S) FAILED" << std::endl;
        return 1;
    }
}
