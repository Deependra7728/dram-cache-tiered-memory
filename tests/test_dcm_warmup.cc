// Regression tests for DRAM-cache warmup
// (docs/final_independent_audit.md, HIGH-2).
//
// ROOT CAUSE. While `all_warmup_complete < NUM_CPUS`, the DCM's
// add_rq() returned the packet to the LLC immediately and add_wq()
// dropped it -- without creating an ORB entry, WITHOUT TOUCHING
// tagMetadataStore, and without recording anything. The DCM was
// therefore completely inert for ChampSim's entire warmup phase, so the
// DRAM cache began every measured region stone cold. Measured on the
// real 600M-instruction BASELINE run: COLD_MISSES 13106 of MISSES
// 13638 -- 96% of all misses in the ROI were compulsory. That directly
// contradicts the paper's stated methodology (Section V: "we made sure
// that the DRAM cache had been warmed-up, so cold misses are not
// contributing to the performance observed from the system").
//
// FIX. The warmup branches now call warmupTagUpdate(), which applies
// byte-for-byte the same tag/metadata update rules as
// classifyAndInstall() -- read hit keeps the dirty bit, read miss
// installs clean, any write dirties the line -- and nothing else. No
// ORB/CRB/WB entry, no DRAM timing, no ROI statistic. ChampSim's
// existing warmup contract (memory returns data immediately) is
// preserved exactly, so warmup stays fast. At the warmup->ROI boundary
// main.cc calls resetROIStats(), which clears ROI counters but
// deliberately leaves the tag store warmed.
//
// Build:
//   g++ -std=c++11 -Iinc -o /tmp/test_dcm_warmup \
//       tests/test_dcm_warmup.cc src/dram_cache_manager.cc \
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
    MEMORY_CONTROLLER near_mc, far_mc;
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

static uint64_t countValidLines(Harness &h)
{
    uint64_t n = 0;
    for (size_t i = 0; i < h.dcm.tagMetadataStore.size(); i++)
        if (h.dcm.tagMetadataStore[i].validLine)
            n++;
    return n;
}

// Index helper that defers to the DCM's OWN mapping rather than copying
// the formula. An earlier version duplicated it, which silently broke
// these assertions when the PACKET::address convention was corrected --
// see returnIndexDC()'s contract block in src/dram_cache_manager.cc.
static uint64_t idxOf(Harness &h, uint64_t a) { return h.dcm.returnIndexDC(a); }

static int failures = 0;
static void check(const char *what, bool ok)
{
    std::cout << "  " << what << ": " << (ok ? "OK" : "FAIL") << std::endl;
    if (!ok)
        failures++;
}

int main()
{
    current_core_cycle[0] = 0;
    const uint64_t STRIDE = DCM_DRAM_CACHE_SIZE;

    // ==================================================================
    // Test 1: the cache is genuinely cold before any warmup traffic.
    // ==================================================================
    {
        all_warmup_complete = 0;
        Harness h("W1");
        std::cout << "[TEST 1: cold before warmup]" << std::endl;
        check("no valid lines in a fresh DCM", countValidLines(h) == 0);
        check("no warmup tag updates yet", h.dcm.stats.warmupTagUpdates == 0);
    }

    // ==================================================================
    // Test 2: warmup accesses install lines, and do so WITHOUT creating
    // any request state or touching any ROI statistic.
    // ==================================================================
    {
        all_warmup_complete = 0; // warmup phase
        Harness h("W2");
        const int N = 5;
        uint64_t base = 0x8000000;
        for (int i = 0; i < N; i++) {
            // DCM_BLOCK_SIZE spacing gives N DISTINCT indices. (STRIDE,
            // i.e. the cache size, would give the SAME index with a
            // different tag -- that is what STRIDE is for elsewhere.)
            PACKET p = makeReadPacket(base + (uint64_t)i * DCM_BLOCK_SIZE);
            h.dcm.add_rq(&p);
        }
        pump(h, 200);

        std::cout << "[TEST 2: warmup installs lines, no ROI side effects]" << std::endl;
        check("N lines now valid", countValidLines(h) == (uint64_t)N);
        check("warmupTagUpdates == N", h.dcm.stats.warmupTagUpdates == (uint64_t)N);
        check("no ORB entries created", h.dcm.ORB.empty());
        check("no CRB entries created", h.dcm.CRB.empty());
        check("no WB entries created", h.dcm.WB.empty());
        check("no pending dispatches", h.dcm.pendingNearDispatches.empty() && h.dcm.pendingFarDispatches.empty());
        check("ROI totalRequests untouched", h.dcm.stats.totalRequests == 0);
        check("ROI localReads untouched", h.dcm.stats.localReads == 0);
        check("ROI hit/miss counters untouched",
              h.dcm.stats.numTotHits == 0 && h.dcm.stats.numTotMisses == 0);
        check("near/far controllers never engaged",
              h.near_mc.RQ[0].occupancy == 0 && h.far_mc.RQ[0].occupancy == 0);
    }

    // ==================================================================
    // Test 3: an ROI access to a warmed line HITS (the whole point).
    // ==================================================================
    {
        all_warmup_complete = 0;
        Harness h("W3");
        uint64_t A = 0x8100000;
        {
            PACKET p = makeReadPacket(A);
            h.dcm.add_rq(&p); // warm it
        }
        pump(h, 100);
        bool warmed = h.dcm.tagMetadataStore[idxOf(h, A)].validLine;

        all_warmup_complete = NUM_CPUS; // ROI begins
        h.dcm.resetROIStats();
        {
            PACKET p = makeReadPacket(A);
            h.dcm.add_rq(&p);
        }
        pump(h, 3000);

        std::cout << "[TEST 3: ROI access to a warmed line hits]" << std::endl;
        check("line was warmed", warmed);
        check("ROI read is a HIT", h.dcm.stats.numRdHit == 1);
        check("no miss recorded", h.dcm.stats.numTotMisses == 0);
        check("no cold miss recorded", h.dcm.stats.numColdMisses == 0);
        check("no far traffic (hit needs none)", h.dcm.stats.farReads == 0);
        check("LLC got its response", h.llc.responses.size() >= 1);
    }

    // ==================================================================
    // Test 4: resetROIStats() clears ROI counters but must NOT clear the
    // warmed tag state, and must preserve the warmup evidence counter.
    // ==================================================================
    {
        all_warmup_complete = 0;
        Harness h("W4");
        const int N = 4;
        uint64_t base = 0x8200000;
        for (int i = 0; i < N; i++) {
            PACKET p = makeReadPacket(base + (uint64_t)i * DCM_BLOCK_SIZE); // distinct indices
            h.dcm.add_rq(&p);
        }
        pump(h, 200);

        all_warmup_complete = NUM_CPUS;
        // Generate some ROI traffic, then reset as main.cc would.
        {
            PACKET p = makeReadPacket(base);
            h.dcm.add_rq(&p);
        }
        pump(h, 3000);
        bool hadRoiTraffic = h.dcm.stats.totalRequests > 0;

        uint64_t validBefore = countValidLines(h);
        h.dcm.resetROIStats();

        std::cout << "[TEST 4: ROI reset keeps cache warm]" << std::endl;
        check("there was ROI traffic to clear", hadRoiTraffic);
        check("ROI counters cleared", h.dcm.stats.totalRequests == 0 && h.dcm.stats.localReads == 0 &&
                                          h.dcm.stats.numTotHits == 0 && h.dcm.stats.numTotMisses == 0);
        check("warmupTagUpdates PRESERVED", h.dcm.stats.warmupTagUpdates == (uint64_t)N);
        check("tag store NOT cleared", countValidLines(h) == validBefore && validBefore == (uint64_t)N);
    }

    // ==================================================================
    // Test 5: dirty metadata written during warmup survives into the
    // ROI and correctly drives a write-back on eviction.
    // ==================================================================
    {
        all_warmup_complete = 0;
        Harness h("W5");
        uint64_t A = 0x8300000, B = A + STRIDE; // same index, different tag
        {
            PACKET p = makeWritePacket(A);
            h.dcm.add_wq(&p); // warmup WRITE -> line must be dirty
        }
        pump(h, 100);
        bool warmDirty = h.dcm.tagMetadataStore[idxOf(h, A)].validLine && h.dcm.tagMetadataStore[idxOf(h, A)].dirtyLine;

        all_warmup_complete = NUM_CPUS;
        h.dcm.resetROIStats();
        {
            PACKET p = makeReadPacket(B); // evicts A, which is dirty
            h.dcm.add_rq(&p);
        }
        pump(h, 20000);

        std::cout << "[TEST 5: warmup dirty state survives into ROI]" << std::endl;
        check("warmup write marked the line dirty", warmDirty);
        check("ROI miss classified as DIRTY miss", h.dcm.stats.numRdMissDirty == 1);
        check("write-back generated for the warmed dirty victim", h.dcm.stats.numWrBacks == 1);
        check("write-back reached far memory", h.dcm.stats.farWrites == 1);
        check("no request stranded", h.dcm.ORB.empty() && h.dcm.WB.empty());
    }

    // ==================================================================
    // Test 6: all three policies warm identically -- warming is a
    // metadata-only operation below the policy layer.
    // ==================================================================
    {
        DCM_POLICY pols[3] = {DCM_POLICY_BASELINE_CASCADE_LAKE, DCM_POLICY_BEAR_WR_OPT, DCM_POLICY_ORACLE};
        const char *names[3] = {"BASELINE", "BEAR", "ORACLE"};
        uint64_t validCount[3], warmUpdates[3];
        bool dirtyBit[3], validBit[3];
        uint64_t A = 0x8400000, C = 0x8400000 + 7 * DCM_BLOCK_SIZE; // C: different index

        for (int i = 0; i < 3; i++) {
            all_warmup_complete = 0;
            Harness h(std::string("W6_") + names[i], pols[i]);
            {
                PACKET p = makeWritePacket(A);
                h.dcm.add_wq(&p);
            }
            {
                PACKET p = makeReadPacket(C);
                h.dcm.add_rq(&p);
            }
            pump(h, 200);
            validCount[i] = countValidLines(h);
            warmUpdates[i] = h.dcm.stats.warmupTagUpdates;
            validBit[i] = h.dcm.tagMetadataStore[idxOf(h, A)].validLine;
            dirtyBit[i] = h.dcm.tagMetadataStore[idxOf(h, A)].dirtyLine;
        }

        std::cout << "[TEST 6: warmup is policy-independent]" << std::endl;
        check("same number of warmed lines", validCount[0] == validCount[1] && validCount[1] == validCount[2]);
        check("same warmup update count", warmUpdates[0] == warmUpdates[1] && warmUpdates[1] == warmUpdates[2]);
        check("same valid state", validBit[0] && validBit[1] && validBit[2]);
        check("same dirty state", dirtyBit[0] && dirtyBit[1] && dirtyBit[2]);
    }

    // ==================================================================
    // Test 7: bypass mode has no DRAM cache in the path, so warming it
    // would be meaningless -- nothing must be installed.
    // ==================================================================
    {
        all_warmup_complete = 0;
        Harness h("W7");
        h.dcm.setBypassDcache(true);
        for (int i = 0; i < 4; i++) {
            PACKET p = makeReadPacket(0x8500000 + (uint64_t)i * STRIDE);
            h.dcm.add_rq(&p);
            PACKET w = makeWritePacket(0x8600000 + (uint64_t)i * STRIDE);
            h.dcm.add_wq(&w);
        }
        pump(h, 200);

        std::cout << "[TEST 7: bypass stays bypassed during warmup]" << std::endl;
        check("no lines installed", countValidLines(h) == 0);
        check("no warmup tag updates", h.dcm.stats.warmupTagUpdates == 0);
        check("no ORB/CRB/WB state", h.dcm.ORB.empty() && h.dcm.CRB.empty() && h.dcm.WB.empty());
    }

    // ==================================================================
    // Test 8: warmup neither loses nor duplicates requests. Reads must
    // get exactly one LLC response each (ChampSim's warmup contract);
    // writes get none, in warmup exactly as in the ROI.
    // ==================================================================
    {
        all_warmup_complete = 0;
        Harness h("W8");
        const int NR = 6, NW = 3;
        for (int i = 0; i < NR; i++) {
            PACKET p = makeReadPacket(0x8700000 + (uint64_t)i * DCM_BLOCK_SIZE);
            h.dcm.add_rq(&p);
        }
        size_t afterReads = h.llc.responses.size();
        for (int i = 0; i < NW; i++) {
            PACKET p = makeWritePacket(0x8800000 + (uint64_t)i * DCM_BLOCK_SIZE);
            h.dcm.add_wq(&p);
        }
        pump(h, 200);

        std::cout << "[TEST 8: no loss or duplication during warmup]" << std::endl;
        check("exactly one response per warmup read", afterReads == (size_t)NR);
        check("writes add no responses", h.llc.responses.size() == (size_t)NR);
        check("all reads and writes left tag state", h.dcm.stats.warmupTagUpdates == (uint64_t)(NR + NW));
        check("nothing left in flight", h.dcm.ORB.empty() && h.dcm.CRB.empty() && h.dcm.WB.empty() &&
                                            h.dcm.pendingNearDispatches.empty() &&
                                            h.dcm.pendingFarDispatches.empty());
    }

    all_warmup_complete = NUM_CPUS; // restore
    if (failures == 0) {
        std::cout << "ALL WARMUP TESTS PASSED" << std::endl;
        return 0;
    }
    std::cout << failures << " WARMUP TEST(S) FAILED" << std::endl;
    return 1;
}
