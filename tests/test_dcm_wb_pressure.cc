// Deterministic tests for WB (Write-Back buffer) occupancy admission
// backpressure -- gem5's `pktFarMemWrite.size() >= orbMaxSize/2` retry in
// `recvTimingReq` (policy_manager.cc:332-345), ported as
// DCM_WB_PRESSURE_THRESHOLD = DCM_ORB_MAX_SIZE/2 = 64.
//
// Proves:
//   1) Below threshold: admission proceeds normally.
//   2) At threshold: new admissions (read AND write) are rejected via
//      get_occupancy()==get_size(), matching the contract CACHE checks
//      before calling add_rq/add_wq.
//   3) Above threshold (can't happen via legitimate use, but verified
//      defensively): still rejects.
//   4) A rejected request can be retried successfully once the WB drains
//      back below threshold.
//   5) WB pressure and ORB/CRB interact correctly: a conflicting request
//      still queues in the CRB (conflict check runs BEFORE the WB check,
//      per gem5's exact ordering) even while WB is under pressure; a
//      non-conflicting request is blocked by WB pressure even though the
//      ORB itself has room.
//
// Build:
//   g++ -std=c++11 -Iinc -o /tmp/test_dcm_wb_pressure \
//       tests/test_dcm_wb_pressure.cc src/dram_cache_manager.cc \
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

int main()
{
    current_core_cycle[0] = 0;
    all_warmup_complete = NUM_CPUS;
    int failures = 0;

    const uint64_t STRIDE = DCM_DRAM_CACHE_SIZE; // same index, different tag

    // ---- Test 1/2/3: fill the WB deque directly to below/at/above threshold ----
    // Directly manipulate WB (it's public) to get deterministic, instant
    // occupancy levels without needing DCM_WB_PRESSURE_THRESHOLD (64)
    // separate dirty evictions to actually happen first (which would work
    // too, just slower and more code -- this is equivalent and faster).
    {
        MEMORY_CONTROLLER near("WB_NEAR"), far("WB_FAR");
        standardTiming(near);
        standardTiming(far);
        DRAM_CACHE_MANAGER dcm("WB_DCM", &near, &far);
        FAKE_LLC llc;
        dcm.upper_level_icache[0] = &llc;
        dcm.upper_level_dcache[0] = &llc;

        uint64_t probeAddr = 0x900000;

        // Below threshold (0 entries): must NOT report full.
        bool belowOk = (dcm.get_occupancy(1, probeAddr) != dcm.get_size(1, probeAddr));

        // Fill WB to exactly DCM_WB_PRESSURE_THRESHOLD - 1 (still below).
        for (uint32_t i = 0; i < DCM_WB_PRESSURE_THRESHOLD - 1; i++) {
            DCM_WB_ENTRY w;
            w.pkt = makeWritePacket(0x1000 + i * 64);
            dcm.WB.push_back(w);
        }
        bool stillBelowOk = (dcm.get_occupancy(1, probeAddr) != dcm.get_size(1, probeAddr));

        // One more -> exactly at threshold -> must report full.
        {
            DCM_WB_ENTRY w;
            w.pkt = makeWritePacket(0x2000000);
            dcm.WB.push_back(w);
        }
        bool atThresholdBlocksRead = (dcm.get_occupancy(1, probeAddr) == dcm.get_size(1, probeAddr));
        bool atThresholdBlocksWrite = (dcm.get_occupancy(2, probeAddr) == dcm.get_size(2, probeAddr));

        // add_rq/add_wq must actually refuse to admit while at threshold.
        PACKET p = makeReadPacket(probeAddr);
        int rc = dcm.add_rq(&p);
        bool wasRejected = (dcm.ORB.find(probeAddr) == dcm.ORB.end());
        (void)rc; // return value is defensive-only, see add_rq's own comment

        // Above threshold (push one more): still blocks.
        {
            DCM_WB_ENTRY w;
            w.pkt = makeWritePacket(0x3000000);
            dcm.WB.push_back(w);
        }
        bool aboveThresholdBlocks = (dcm.get_occupancy(1, probeAddr) == dcm.get_size(1, probeAddr));

        std::cout << "[TEST 1-3: WB threshold levels] below_ok=" << belowOk
                   << " still_below_ok=" << stillBelowOk << " at_threshold_blocks_read=" << atThresholdBlocksRead
                   << " at_threshold_blocks_write=" << atThresholdBlocksWrite
                   << " admission_rejected_at_threshold=" << wasRejected
                   << " above_threshold_blocks=" << aboveThresholdBlocks
                   << " wbFullRejects=" << dcm.stats.wbFullRejects << std::endl;

        if (!belowOk || !stillBelowOk || !atThresholdBlocksRead || !atThresholdBlocksWrite || !wasRejected ||
            !aboveThresholdBlocks || dcm.stats.wbFullRejects != 1) {
            std::cerr << "  FAIL" << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ---- Test 4: retry succeeds once WB drains back below threshold ----
    {
        current_core_cycle[0] = 0;
        MEMORY_CONTROLLER near("WB_NEAR4"), far("WB_FAR4");
        standardTiming(near);
        standardTiming(far);
        DRAM_CACHE_MANAGER dcm("WB_DCM4", &near, &far);
        FAKE_LLC llc;
        dcm.upper_level_icache[0] = &llc;
        dcm.upper_level_dcache[0] = &llc;

        uint64_t addr = 0x950000;
        for (uint32_t i = 0; i < DCM_WB_PRESSURE_THRESHOLD; i++) {
            DCM_WB_ENTRY w;
            w.pkt = makeWritePacket(0x4000000 + (uint64_t)i * 64);
            dcm.WB.push_back(w);
        }

        bool blockedFirst = (dcm.get_occupancy(1, addr) == dcm.get_size(1, addr));
        PACKET p1 = makeReadPacket(addr);
        dcm.add_rq(&p1);
        bool rejectedFirst = (dcm.ORB.find(addr) == dcm.ORB.end());

        // Drain the WB deque via operate() (drainWB() pops at most one per
        // cycle) until it's back below threshold.
        for (int i = 0; i < 200 && dcm.WB.size() >= DCM_WB_PRESSURE_THRESHOLD; i++) {
            current_core_cycle[0]++;
            dcm.operate();
        }
        bool nowBelow = (dcm.get_occupancy(1, addr) != dcm.get_size(1, addr));

        PACKET p2 = makeReadPacket(addr);
        dcm.add_rq(&p2);
        bool admittedSecond = (dcm.ORB.find(addr) != dcm.ORB.end());

        std::cout << "[TEST 4: retry after WB drains] blocked_first=" << blockedFirst
                   << " rejected_first=" << rejectedFirst << " now_below=" << nowBelow
                   << " admitted_second=" << admittedSecond << " wb_size=" << dcm.WB.size() << std::endl;

        if (!blockedFirst || !rejectedFirst || !nowBelow || !admittedSecond) {
            std::cerr << "  FAIL" << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ---- Test 5a: conflict check runs BEFORE WB check (CRB still works under WB pressure) ----
    {
        current_core_cycle[0] = 0;
        MEMORY_CONTROLLER near("WB_NEAR5A"), far("WB_FAR5A");
        standardTiming(near);
        standardTiming(far);
        DRAM_CACHE_MANAGER dcm("WB_DCM5A", &near, &far);
        FAKE_LLC llc;
        dcm.upper_level_icache[0] = &llc;
        dcm.upper_level_dcache[0] = &llc;

        uint64_t addrA = 0x960000;
        uint64_t addrB = addrA + STRIDE; // same index as addrA

        PACKET pa = makeReadPacket(addrA);
        dcm.add_rq(&pa); // occupies the ORB for this index

        // Now push WB to threshold.
        for (uint32_t i = 0; i < DCM_WB_PRESSURE_THRESHOLD; i++) {
            DCM_WB_ENTRY w;
            w.pkt = makeWritePacket(0x5000000 + (uint64_t)i * 64);
            dcm.WB.push_back(w);
        }

        // addrB conflicts with addrA's index -> must STILL go to the CRB
        // (conflict check precedes the WB check in gem5's ordering),
        // despite WB being at/above threshold.
        PACKET pb = makeReadPacket(addrB);
        dcm.add_rq(&pb);
        bool wentToCRB = (dcm.CRB.size() == 1 && dcm.CRB[0].pkt.address == addrB);

        std::cout << "[TEST 5a: conflict beats WB pressure] went_to_crb=" << wentToCRB
                   << " crb_size=" << dcm.CRB.size() << " wb_size=" << dcm.WB.size() << std::endl;

        if (!wentToCRB) {
            std::cerr << "  FAIL (a conflicting request must queue in the CRB regardless of WB pressure)"
                      << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ---- Test 5b: WB pressure blocks a NON-conflicting request even with ORB room ----
    {
        current_core_cycle[0] = 0;
        MEMORY_CONTROLLER near("WB_NEAR5B"), far("WB_FAR5B");
        standardTiming(near);
        standardTiming(far);
        DRAM_CACHE_MANAGER dcm("WB_DCM5B", &near, &far);
        FAKE_LLC llc;
        dcm.upper_level_icache[0] = &llc;
        dcm.upper_level_dcache[0] = &llc;

        for (uint32_t i = 0; i < DCM_WB_PRESSURE_THRESHOLD; i++) {
            DCM_WB_ENTRY w;
            w.pkt = makeWritePacket(0x6000000 + (uint64_t)i * 64);
            dcm.WB.push_back(w);
        }

        uint64_t freshAddr = 0x970000; // does not conflict with anything; ORB is empty (plenty of room)
        PACKET p = makeReadPacket(freshAddr);
        dcm.add_rq(&p);
        bool orbHasRoom = (dcm.ORB.size() < DCM_ORB_MAX_SIZE);
        bool wasBlocked = (dcm.ORB.find(freshAddr) == dcm.ORB.end());

        std::cout << "[TEST 5b: WB pressure blocks despite ORB room] orb_has_room=" << orbHasRoom
                   << " was_blocked=" << wasBlocked << " orb_size=" << dcm.ORB.size() << std::endl;

        if (!orbHasRoom || !wasBlocked) {
            std::cerr << "  FAIL (WB pressure must block admission even when the ORB itself has room)"
                      << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    if (failures == 0) {
        std::cout << "ALL WB PRESSURE TESTS PASSED" << std::endl;
        return 0;
    } else {
        std::cout << failures << " WB PRESSURE TEST(S) FAILED" << std::endl;
        return 1;
    }
}
