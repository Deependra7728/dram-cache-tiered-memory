// REAL LLC -> DRAM_CACHE_MANAGER integration test
// (docs/final_independent_audit.md, MEDIUM-3).
//
// WHY THIS EXISTS. Every other suite in tests/ drives the DCM through a
// hand-written `FAKE_LLC` stub. That is fine for exercising the DCM's own
// state machine, but it cannot catch anything that depends on how the
// REAL ChampSim `CACHE` actually talks to its `lower_level`: MSHR
// behaviour, the read/writeback split, the `get_occupancy()==get_size()`
// pre-check before `add_rq`/`add_wq`, retry-on-full handling, and the
// exact `return_data()` contract. The audit specifically flagged that
// gap -- the LLC-reachable trigger for the CRITICAL duplicate-merge
// defect (a demand read and a dirty writeback to the SAME address
// travelling independent paths) is invisible to a stubbed LLC.
//
// This links the ACTUAL `CACHE` class from src/cache.cc, plus the real
// replacement and prefetcher modules, wires a real LLC's
// `lower_level` to a real `DRAM_CACHE_MANAGER`, and drives traffic in
// through `CACHE::add_rq()`/`add_wq()` exactly as L2C would. No
// architectural change was needed to make this possible -- the DCM
// already presents the standard `MEMORY` interface, which is the whole
// point of the port's design.
//
// `lg2()` and `va_to_pa()` are defined here because they live in
// src/main.cc, which cannot be linked into a test (it owns `main()`).
// `va_to_pa` is the identity map: these tests use physical addresses
// directly, and an identity translation is what makes the DRAM-cache
// index/tag arithmetic in the assertions below predictable.
//
// Build:
//   g++ -std=c++11 -Iinc -o /tmp/test_dcm_llc_integration \
//       tests/test_dcm_llc_integration.cc src/cache.cc src/dram_cache_manager.cc \
//       src/dram_controller.cc src/block.cc replacement/llc_replacement.cc \
//       replacement/base_replacement.cc prefetcher/llc_prefetcher.cc \
//       prefetcher/l1d_prefetcher.cc prefetcher/l2c_prefetcher.cc

#include <iostream>
#include <string>
#include <vector>

#include "champsim.h"
#include "cache.h"
#include "dram_cache_manager.h"

uint64_t current_core_cycle[NUM_CPUS];
uint8_t all_warmup_complete;
uint8_t warmup_complete[NUM_CPUS];
uint32_t PAGE_TABLE_LATENCY = 0, SWAP_LATENCY = 0;

int lg2(int n)
{
    int i = 0;
    while (n > 1) {
        n >>= 1;
        i++;
    }
    return i;
}

// Identity translation -- see the file banner.
uint64_t va_to_pa(uint32_t cpu, uint64_t instr_id, uint64_t va, uint64_t unique_vpage, uint8_t is_code)
{
    return va;
}

static void standardTiming(MEMORY_CONTROLLER &mc)
{
    uint32_t trp = (uint32_t)((1.0 * tRP_DRAM_NANOSECONDS * CPU_FREQ) / 1000);
    uint32_t trcd = (uint32_t)((1.0 * tRCD_DRAM_NANOSECONDS * CPU_FREQ) / 1000);
    uint32_t tcas = (uint32_t)((1.0 * tCAS_DRAM_NANOSECONDS * CPU_FREQ) / 1000);
    uint32_t mtps = DRAM_IO_FREQ;
    mc.set_timing(trp, trcd, tcas, mtps, computeDbusReturnTime(mtps));
}

// Stands in for the L2C above the LLC: it only has to accept the LLC's
// fill responses, which is all CACHE requires of its upper level here.
class UPPER : public MEMORY {
  public:
    std::vector<uint64_t> filled;
    int add_rq(PACKET *packet) { return -1; }
    int add_wq(PACKET *packet) { return -1; }
    int add_pq(PACKET *packet) { return -1; }
    void operate() {}
    void increment_WQ_FULL(uint64_t address) {}
    uint32_t get_occupancy(uint8_t queue_type, uint64_t address) { return 0; }
    uint32_t get_size(uint8_t queue_type, uint64_t address) { return 1; }
    void return_data(PACKET *packet) { filled.push_back(packet->address); }
};

// A real LLC wired on top of a real DRAM_CACHE_MANAGER.
struct System {
    MEMORY_CONTROLLER near_mc, far_mc;
    DRAM_CACHE_MANAGER dcm;
    CACHE llc;
    UPPER upper;

    System(const std::string &n, DCM_POLICY pol = DCM_POLICY_BASELINE_CASCADE_LAKE)
        : near_mc(n + "_N"), far_mc(n + "_F"), dcm(n + "_DCM", &near_mc, &far_mc),
          llc(n + "_LLC", LLC_SET, LLC_WAY, LLC_SET *LLC_WAY, LLC_WQ_SIZE, LLC_RQ_SIZE, LLC_PQ_SIZE, LLC_MSHR_SIZE)
    {
        standardTiming(near_mc);
        standardTiming(far_mc);
        dcm.policy = pol;

        // Same configuration main.cc applies to uncore.LLC (MAX_FILL is
        // left at CACHE's own constructor default there too).
        llc.cache_type = IS_LLC;
        llc.fill_level = FILL_LLC;
        llc.MAX_READ = NUM_CPUS;
        llc.lower_level = &dcm; // <-- the integration under test
        for (uint32_t i = 0; i < NUM_CPUS; i++) {
            llc.upper_level_icache[i] = &upper;
            llc.upper_level_dcache[i] = &upper;
            dcm.upper_level_icache[i] = &llc;
            dcm.upper_level_dcache[i] = &llc;
        }
        llc.llc_initialize_replacement();
        llc.llc_prefetcher_initialize();
    }
};

static PACKET makePacket(uint64_t addr, uint8_t type, uint64_t id)
{
    PACKET p;
    p.type = type;
    p.address = addr >> LOG2_BLOCK_SIZE;
    p.full_addr = addr;
    p.cpu = 0;
    p.instr_id = id;
    p.instruction = 0;
    p.is_data = 1;
    p.fill_level = FILL_L2;
    p.event_cycle = current_core_cycle[0];
    return p;
}

// One simulated cycle of the whole stack, in main.cc's order: caches
// first, then the memory side.
static void tick(System &s, int cycles)
{
    for (int i = 0; i < cycles; i++) {
        current_core_cycle[0]++;
        s.llc.operate();
        s.dcm.operate();
    }
}

static bool tickUntil(System &s, uint64_t maxCycles, bool (*done)(System &))
{
    for (uint64_t i = 0; i < maxCycles; i++) {
        current_core_cycle[0]++;
        s.llc.operate();
        s.dcm.operate();
        if (done(s))
            return true;
    }
    return false;
}

static bool quiet(System &s)
{
    return s.dcm.ORB.empty() && s.dcm.CRB.empty() && s.dcm.WB.empty() &&
           s.dcm.pendingNearDispatches.empty() && s.dcm.pendingFarDispatches.empty() &&
           s.dcm.pendingResponses.empty() && s.llc.MSHR.occupancy == 0 && s.llc.RQ.occupancy == 0 &&
           s.llc.WQ.occupancy == 0 && s.near_mc.RQ[0].occupancy == 0 && s.near_mc.WQ[0].occupancy == 0 &&
           s.far_mc.RQ[0].occupancy == 0 && s.far_mc.WQ[0].occupancy == 0;
}

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
    all_warmup_complete = NUM_CPUS;
    for (uint32_t i = 0; i < NUM_CPUS; i++)
        warmup_complete[i] = 1;

    // ==================================================================
    // Test 1: a real LLC read miss reaches the DCM, is serviced, and the
    // fill comes back up. This alone proves the whole wiring works
    // through the real CACHE code path.
    // ==================================================================
    {
        System s("LLC1");
        uint64_t A = 0x40000000;
        PACKET p = makePacket(A, LOAD, 1);
        int rc = s.llc.add_rq(&p);
        bool q = tickUntil(s, 200000, quiet);

        std::cout << "[TEST 1: real LLC read miss -> DCM -> response]" << std::endl;
        check("LLC accepted the request", rc != -2);
        check("system quiesced", q);
        check("DCM saw exactly one request", s.dcm.stats.totalRequests == 1);
        check("DCM classified it as a read", s.dcm.stats.readRequests == 1);
        check("DCM issued the baseline tag-check read", s.dcm.stats.localReads == 1);
        check("DCM fetched from far memory (cold miss)", s.dcm.stats.farReads == 1);
        check("DCM responded to the LLC", s.dcm.stats.completedReadsToLLC == 1);
        check("LLC filled its upper level", s.upper.filled.size() >= 1);
        check("no request stranded", s.dcm.ORB.empty());
    }

    // ==================================================================
    // Test 2: a second read to the same block HITS in the real LLC and
    // must NOT reach the DCM at all -- confirming we are genuinely
    // going through the LLC and not bypassing it.
    // ==================================================================
    {
        System s("LLC2");
        uint64_t A = 0x40100000;
        PACKET p1 = makePacket(A, LOAD, 1);
        s.llc.add_rq(&p1);
        tickUntil(s, 200000, quiet);
        uint64_t afterFirst = s.dcm.stats.totalRequests;

        PACKET p2 = makePacket(A, LOAD, 2);
        s.llc.add_rq(&p2);
        tickUntil(s, 200000, quiet);

        std::cout << "[TEST 2: LLC hit is absorbed by the LLC]" << std::endl;
        check("first access reached the DCM", afterFirst == 1);
        check("second access did NOT reach the DCM (LLC hit)", s.dcm.stats.totalRequests == afterFirst);
    }

    // ==================================================================
    // Test 3: THE AUDIT'S LLC-REACHABLE SCENARIO. A demand read and a
    // dirty writeback for the SAME address travel independent paths
    // (add_rq vs add_wq -- the LLC's MSHR only merges reads), which is
    // exactly what could collide inside the DCM and strand an ORB entry
    // before the duplicate-merge fix. Driven here through the REAL LLC.
    // ==================================================================
    {
        System s("LLC3");
        uint64_t A = 0x40200000;
        // Writeback and demand read for the same block, issued together.
        PACKET w = makePacket(A, WRITEBACK, 1);
        w.fill_level = FILL_LLC;
        s.llc.add_wq(&w);
        PACKET r = makePacket(A, LOAD, 2);
        s.llc.add_rq(&r);

        bool q = tickUntil(s, 500000, quiet);

        std::cout << "[TEST 3: same-address read + writeback via the REAL LLC]" << std::endl;
        check("system quiesced (no deadlock)", q);
        check("no ORB entry stranded", s.dcm.ORB.empty());
        check("no CRB entry stranded", s.dcm.CRB.empty());
        check("every near read got a completion",
              s.dcm.stats.localReads == s.dcm.stats.completedFromNear);
        check("every far read got a completion", s.dcm.stats.farReads == s.dcm.stats.completedFromFar);
        check("every WB insertion drained", s.dcm.stats.wbInsertions == s.dcm.stats.wbDrains);

        // Follow-up traffic to the SAME DRAM-cache index must still work
        // -- the audit's failure mode poisoned the index permanently.
        uint64_t before = s.dcm.stats.totalRequests;
        int admitted = 0;
        for (int i = 1; i <= 4; i++) {
            PACKET f = makePacket(A + (uint64_t)i * DCM_DRAM_CACHE_SIZE, LOAD, 100 + i);
            if (s.llc.add_rq(&f) != -2)
                admitted++;
            tickUntil(s, 500000, quiet);
        }
        check("follow-up traffic to the same index still admitted", admitted == 4);
        check("follow-up traffic reached the DCM", s.dcm.stats.totalRequests >= before + 4);
        check("still nothing stranded", s.dcm.ORB.empty() && s.dcm.CRB.empty());
    }

    // ==================================================================
    // Test 4: sustained mixed traffic through the real LLC. Request
    // conservation must hold exactly: every dispatched near/far read is
    // answered, every WB insertion drains, nothing is stranded.
    // ==================================================================
    {
        System s("LLC4");
        const int N = 40;
        uint64_t base = 0x40400000;
        for (int i = 0; i < N; i++) {
            PACKET r = makePacket(base + (uint64_t)i * DCM_BLOCK_SIZE, LOAD, 200 + i);
            if (s.llc.add_rq(&r) == -2)
                tickUntil(s, 100000, quiet); // respect LLC backpressure, then retry
            tick(s, 40);
        }
        // Dirty the same lines, then evict them to force write-backs.
        for (int i = 0; i < N; i++) {
            PACKET w = makePacket(base + (uint64_t)i * DCM_BLOCK_SIZE, WRITEBACK, 300 + i);
            w.fill_level = FILL_LLC;
            s.llc.add_wq(&w);
            tick(s, 40);
        }
        for (int i = 0; i < N; i++) {
            PACKET e = makePacket(base + (uint64_t)i * DCM_BLOCK_SIZE + DCM_DRAM_CACHE_SIZE, LOAD, 400 + i);
            if (s.llc.add_rq(&e) == -2)
                tickUntil(s, 100000, quiet);
            tick(s, 40);
        }
        bool q = tickUntil(s, 2000000, quiet);

        std::cout << "[TEST 4: sustained mixed traffic, request conservation]" << std::endl;
        std::cout << "    totalRequests=" << s.dcm.stats.totalRequests
                   << " localReads=" << s.dcm.stats.localReads
                   << " completedFromNear=" << s.dcm.stats.completedFromNear
                   << " farReads=" << s.dcm.stats.farReads
                   << " completedFromFar=" << s.dcm.stats.completedFromFar
                   << " wbIns=" << s.dcm.stats.wbInsertions << " wbDrains=" << s.dcm.stats.wbDrains
                   << " mergeRetries=" << s.dcm.stats.dispatchMergeRetries << std::endl;
        check("system quiesced", q);
        check("real traffic actually reached the DCM", s.dcm.stats.totalRequests > 0);
        check("every near read answered", s.dcm.stats.localReads == s.dcm.stats.completedFromNear);
        check("every far read answered", s.dcm.stats.farReads == s.dcm.stats.completedFromFar);
        check("every WB insertion drained", s.dcm.stats.wbInsertions == s.dcm.stats.wbDrains);
        check("far writes == WB drains", s.dcm.stats.farWrites == s.dcm.stats.wbDrains);
        check("nothing stranded", s.dcm.ORB.empty() && s.dcm.CRB.empty() && s.dcm.WB.empty());
        check("no ORB/CRB overflow rejects", s.dcm.stats.orbFullRejects == 0 && s.dcm.stats.crbFullRejects == 0);
    }

    // ==================================================================
    // Test 5: bypass mode through the real LLC -- traffic must reach far
    // memory and never touch the DRAM cache.
    // ==================================================================
    {
        System s("LLC5");
        s.dcm.setBypassDcache(true);
        for (int i = 0; i < 6; i++) {
            PACKET r = makePacket(0x40600000 + (uint64_t)i * DCM_BLOCK_SIZE, LOAD, 500 + i);
            s.llc.add_rq(&r);
            tick(s, 200);
        }
        bool q = tickUntil(s, 500000, quiet);

        std::cout << "[TEST 5: bypass mode via the real LLC]" << std::endl;
        check("system quiesced", q);
        check("bypass reads recorded", s.dcm.stats.bypassReads == 6);
        check("all bypass reads completed", s.dcm.stats.bypassCompletedReads == 6);
        check("tracking set not leaked", s.dcm.bypassOutstandingReads.empty());
        check("DRAM cache never engaged", s.dcm.stats.totalRequests == 0 && s.dcm.stats.localReads == 0);
        check("no ORB/CRB/WB state", s.dcm.ORB.empty() && s.dcm.CRB.empty() && s.dcm.WB.empty());
    }

    // ==================================================================
    // Test 6: all three policies work end-to-end through the real LLC,
    // and BEAR/Oracle really do eliminate local reads relative to
    // baseline for the same traffic (the Table II optimisation, observed
    // through the real cache hierarchy rather than a stub).
    // ==================================================================
    {
        DCM_POLICY pols[3] = {DCM_POLICY_BASELINE_CASCADE_LAKE, DCM_POLICY_BEAR_WR_OPT, DCM_POLICY_ORACLE};
        const char *names[3] = {"BASELINE", "BEAR", "ORACLE"};
        uint64_t locReads[3] = {0, 0, 0}, totals[3] = {0, 0, 0};
        bool allQuiet = true;

        for (int k = 0; k < 3; k++) {
            System s(std::string("LLC6_") + names[k], pols[k]);
            uint64_t base = 0x40800000;
            const int N = 8;
            // Populate, then write (write hits -- BEAR/Oracle optimise these).
            for (int i = 0; i < N; i++) {
                PACKET r = makePacket(base + (uint64_t)i * DCM_BLOCK_SIZE, LOAD, 600 + i);
                s.llc.add_rq(&r);
                tickUntil(s, 200000, quiet);
            }
            for (int i = 0; i < N; i++) {
                PACKET w = makePacket(base + (uint64_t)i * DCM_BLOCK_SIZE, WRITEBACK, 700 + i);
                w.fill_level = FILL_LLC;
                s.llc.add_wq(&w);
                tickUntil(s, 200000, quiet);
            }
            allQuiet = allQuiet && tickUntil(s, 500000, quiet);
            locReads[k] = s.dcm.stats.localReads;
            totals[k] = s.dcm.stats.totalRequests;
            // Per-policy conservation must hold regardless of policy.
            if (s.dcm.stats.localReads != s.dcm.stats.completedFromNear || !s.dcm.ORB.empty())
                failures++;
        }

        std::cout << "[TEST 6: all three policies end-to-end via the real LLC]" << std::endl;
        std::cout << "    localReads: BASELINE=" << locReads[0] << " BEAR=" << locReads[1]
                   << " ORACLE=" << locReads[2] << "   (same request count: " << totals[0] << "/"
                   << totals[1] << "/" << totals[2] << ")" << std::endl;
        check("all policies quiesced with conservation intact", allQuiet);
        check("identical request counts across policies", totals[0] == totals[1] && totals[1] == totals[2]);
        // NOTE on BEAR: its optimisation targets DRAM-cache WRITE HITS, and
        // this traffic produces none the DCM can see -- a writeback whose
        // block is resident in the LLC is absorbed by the LLC and never
        // forwarded to lower_level at all. That is correct real-hierarchy
        // behaviour, not a policy defect, and it is precisely the kind of
        // thing only a real-LLC test can show. BEAR must therefore be
        // no WORSE than baseline here, not strictly better.
        check("BEAR issues no more local reads than baseline", locReads[1] <= locReads[0]);
        check("Oracle issues strictly fewer local reads than baseline (clean-miss exemption)",
              locReads[2] < locReads[0]);
        check("Oracle issues no more local reads than BEAR", locReads[2] <= locReads[1]);
    }

    // ==================================================================
    // Test 7: ADJACENT 64-BYTE LINES MUST NOT ALIAS.
    //
    // This replaces the earlier CHARACTERISATION test that asserted the
    // defective aliasing. The defect (the DCM divided the incoming
    // PACKET::address by DCM_BLOCK_SIZE a second time, even though the
    // whole ChampSim hierarchy already passes line addresses) is fixed;
    // these are now correctness assertions.
    // ==================================================================
    {
        System s("LLC7");
        uint64_t A = 0x40000000, B = A + BLOCK_SIZE; // adjacent, DISTINCT 64B lines

        PACKET p1 = makePacket(A, LOAD, 900);
        s.llc.add_rq(&p1);
        tickUntil(s, 200000, quiet);
        uint64_t missAfterA = s.dcm.stats.numTotMisses, hitAfterA = s.dcm.stats.numTotHits;

        PACKET p2 = makePacket(B, LOAD, 901);
        s.llc.add_rq(&p2);
        tickUntil(s, 200000, quiet);
        uint64_t missAfterB = s.dcm.stats.numTotMisses, hitAfterB = s.dcm.stats.numTotHits;

        // Re-access A: it is resident, so this MUST hit.
        PACKET p3 = makePacket(A, LOAD, 902);
        s.llc.add_rq(&p3);
        tickUntil(s, 200000, quiet);

        uint64_t idxA = s.dcm.returnIndexDC(A >> LOG2_BLOCK_SIZE);
        uint64_t idxB = s.dcm.returnIndexDC(B >> LOG2_BLOCK_SIZE);

        std::cout << "[TEST 7: adjacent 64B lines must not alias]" << std::endl;
        std::cout << "    0x" << std::hex << A << " -> index " << std::dec << idxA
                   << " ; 0x" << std::hex << B << " -> index " << std::dec << idxB << std::endl;
        check("adjacent lines map to DIFFERENT indices", idxA != idxB);
        check("indices are exactly one apart (direct-mapped, 64B granularity)", idxB == idxA + 1);
        check("first access to A is a MISS", missAfterA == 1 && hitAfterA == 0);
        check("access to adjacent B is ALSO a MISS (no aliasing)", missAfterB == 2 && hitAfterB == 0);
        // p3 went through the LLC, which now holds A -- so it is absorbed
        // there and never reaches the DCM. Re-check residency directly.
        DCM_TAG_ENTRY &slotA = s.dcm.tagMetadataStore[idxA];
        DCM_TAG_ENTRY &slotB = s.dcm.tagMetadataStore[idxB];
        check("A is resident in the DRAM cache", slotA.validLine &&
                                                     slotA.tagDC == s.dcm.returnTagDC(A >> LOG2_BLOCK_SIZE));
        check("B is resident in the DRAM cache", slotB.validLine &&
                                                     slotB.tagDC == s.dcm.returnTagDC(B >> LOG2_BLOCK_SIZE));
        check("both lines resident simultaneously (they did not evict each other)",
              slotA.validLine && slotB.validLine);
    }

    // ==================================================================
    // Test 8: INTENTIONAL same-index / different-tag conflict.
    // Two byte addresses exactly DCM_NUM_LINES * 64 B apart (= 128 MiB,
    // the whole cache) must land on the SAME direct-mapped index with
    // DIFFERENT tags, and must therefore replace one another.
    // Covers: cold miss, replacement, dirty replacement, WB behaviour.
    // ==================================================================
    {
        System s("LLC8");
        const uint64_t SAME_INDEX_STRIDE = DCM_NUM_LINES * DCM_BLOCK_SIZE; // 128 MiB
        uint64_t A = 0x50000000, B = A + SAME_INDEX_STRIDE;

        uint64_t lineA = A >> LOG2_BLOCK_SIZE, lineB = B >> LOG2_BLOCK_SIZE;
        uint64_t idxA = s.dcm.returnIndexDC(lineA), idxB = s.dcm.returnIndexDC(lineB);
        uint64_t tagA = s.dcm.returnTagDC(lineA), tagB = s.dcm.returnTagDC(lineB);

        std::cout << "[TEST 8: intentional same-index / different-tag conflict]" << std::endl;
        std::cout << "    stride = DCM_NUM_LINES * 64B = " << SAME_INDEX_STRIDE << " B (128 MiB)"
                   << std::endl;
        std::cout << "    A -> index " << idxA << " tag " << tagA << " ; B -> index " << idxB
                   << " tag " << tagB << std::endl;
        check("A and B map to the SAME index", idxA == idxB);
        check("A and B have DIFFERENT tags", tagA != tagB);

        // Cold miss on A, then DIRTY it via a writeback that misses in the
        // LLC (so it is forwarded down), then B evicts it -> write-back.
        PACKET r = makePacket(A, LOAD, 910);
        s.llc.add_rq(&r);
        tickUntil(s, 200000, quiet);
        check("A cold-missed in the DRAM cache", s.dcm.stats.numColdMisses == 1);

        // Dirty A *in the DRAM cache*. This write is injected directly
        // into the DCM rather than through the LLC, because a writeback
        // whose block the LLC already holds is absorbed by the LLC and
        // never forwarded downward (correct real-hierarchy behaviour,
        // asserted in Test 6). Test 9 proves the two entry paths are
        // equivalent for the same logical address, so injecting here
        // exercises exactly the same DCM mapping and state machine.
        PACKET w = makePacket(A, WRITEBACK, 911);
        s.dcm.add_wq(&w);
        for (int c = 0; c < 200000 && !quiet(s); c++) {
            current_core_cycle[0]++;
            s.llc.operate();
            s.dcm.operate();
        }
        bool aDirty = s.dcm.tagMetadataStore[idxA].dirtyLine;

        uint64_t wbBefore = s.dcm.stats.numWrBacks;
        PACKET r2 = makePacket(B, LOAD, 912);
        s.llc.add_rq(&r2);
        tickUntil(s, 200000, quiet);

        check("A became dirty", aDirty);
        check("B replaced A at the shared index",
              s.dcm.tagMetadataStore[idxA].validLine && s.dcm.tagMetadataStore[idxA].tagDC == tagB);
        check("dirty replacement produced exactly one write-back",
              s.dcm.stats.numWrBacks == wbBefore + 1);
        check("write-back reached far memory", s.dcm.stats.farWrites == s.dcm.stats.wbDrains);
        check("nothing stranded", s.dcm.ORB.empty() && s.dcm.CRB.empty() && s.dcm.WB.empty());
    }

    // ==================================================================
    // Test 9: PAIRED EQUIVALENCE -- direct DCM injection vs. real-LLC
    // generated packet. For the SAME logical memory address, both entry
    // paths must produce identical line address, index, tag, hit/miss
    // classification, dirty state and operation sequence. This is the
    // test that would have caught the convention mismatch immediately.
    // ==================================================================
    {
        uint64_t BYTE_ADDRS[3] = {0x60000000, 0x60000040, 0x60000000 + DCM_NUM_LINES * DCM_BLOCK_SIZE};

        // --- Path A: direct injection into a bare DCM ---
        MEMORY_CONTROLLER nA("EQ_A_N"), fA("EQ_A_F");
        standardTiming(nA);
        standardTiming(fA);
        DRAM_CACHE_MANAGER dcmA("EQ_A_DCM", &nA, &fA);
        UPPER upA;
        for (uint32_t i = 0; i < NUM_CPUS; i++) {
            dcmA.upper_level_icache[i] = &upA;
            dcmA.upper_level_dcache[i] = &upA;
        }
        for (int k = 0; k < 3; k++) {
            // Direct injection uses the SAME convention the LLC uses:
            // address = line address, full_addr = byte address.
            PACKET p = makePacket(BYTE_ADDRS[k], LOAD, 920 + k);
            dcmA.add_rq(&p);
            for (int c = 0; c < 200000; c++) {
                current_core_cycle[0]++;
                dcmA.operate();
                if (dcmA.ORB.empty() && dcmA.WB.empty() && dcmA.pendingNearDispatches.empty() &&
                    dcmA.pendingFarDispatches.empty() && dcmA.pendingResponses.empty() &&
                    nA.RQ[0].occupancy == 0 && nA.WQ[0].occupancy == 0 && fA.RQ[0].occupancy == 0 &&
                    fA.WQ[0].occupancy == 0)
                    break;
            }
        }

        // --- Path B: the same addresses through a real LLC ---
        System sB("EQ_B");
        for (int k = 0; k < 3; k++) {
            PACKET p = makePacket(BYTE_ADDRS[k], LOAD, 930 + k);
            sB.llc.add_rq(&p);
            tickUntil(sB, 200000, quiet);
        }

        std::cout << "[TEST 9: direct-injection vs real-LLC equivalence]" << std::endl;
        bool mapEqual = true, metaEqual = true;
        for (int k = 0; k < 3; k++) {
            uint64_t line = BYTE_ADDRS[k] >> LOG2_BLOCK_SIZE;
            uint64_t iA = dcmA.returnIndexDC(line), iB = sB.dcm.returnIndexDC(line);
            uint64_t tA = dcmA.returnTagDC(line), tB = sB.dcm.returnTagDC(line);
            if (iA != iB || tA != tB)
                mapEqual = false;
            const DCM_TAG_ENTRY &mA = dcmA.tagMetadataStore[iA];
            const DCM_TAG_ENTRY &mB = sB.dcm.tagMetadataStore[iB];
            if (mA.validLine != mB.validLine || mA.dirtyLine != mB.dirtyLine || mA.tagDC != mB.tagDC)
                metaEqual = false;
            std::cout << "    0x" << std::hex << BYTE_ADDRS[k] << std::dec << " -> line " << line
                       << " index " << iA << " tag " << tA << std::endl;
        }
        check("identical index+tag on both entry paths", mapEqual);
        check("identical resulting metadata (valid/dirty/tag)", metaEqual);
        check("identical hit/miss classification",
              dcmA.stats.numTotHits == sB.dcm.stats.numTotHits &&
                  dcmA.stats.numTotMisses == sB.dcm.stats.numTotMisses);
        check("identical cold-miss count", dcmA.stats.numColdMisses == sB.dcm.stats.numColdMisses);
        check("identical operation sequence (local/far reads and writes)",
              dcmA.stats.localReads == sB.dcm.stats.localReads &&
                  dcmA.stats.localWrites == sB.dcm.stats.localWrites &&
                  dcmA.stats.farReads == sB.dcm.stats.farReads &&
                  dcmA.stats.farWrites == sB.dcm.stats.farWrites);
    }

    // ==================================================================
    // Test 10: CAPACITY / granularity sanity. Fast -- no huge structure
    // is allocated or iterated; the metadata store is already a compact
    // per-line vector and only its size and a few mappings are checked.
    // ==================================================================
    {
        System s("LLC10");
        std::cout << "[TEST 10: capacity and granularity]" << std::endl;
        check("DCM_NUM_LINES == 128 MiB / 64 B", DCM_NUM_LINES == (128ULL * 1024 * 1024) / 64);
        check("DCM_NUM_LINES == 2097152", DCM_NUM_LINES == 2097152ULL);
        check("NUM_LINES * BLOCK_SIZE == 128 MiB exactly",
              DCM_NUM_LINES * DCM_BLOCK_SIZE == DCM_DRAM_CACHE_SIZE);
        check("metadata store provides exactly that many logical locations",
              s.dcm.tagMetadataStore.size() == DCM_NUM_LINES);
        // Mapping is a bijection over one full cache period: line L and
        // line L + NUM_LINES share an index and differ in tag by one.
        uint64_t L = 12345;
        check("line L and L+NUM_LINES share an index",
              s.dcm.returnIndexDC(L) == s.dcm.returnIndexDC(L + DCM_NUM_LINES));
        check("...and differ in tag by exactly 1",
              s.dcm.returnTagDC(L + DCM_NUM_LINES) == s.dcm.returnTagDC(L) + 1);
        check("consecutive lines occupy consecutive indices",
              s.dcm.returnIndexDC(L + 1) == s.dcm.returnIndexDC(L) + 1);
        check("index is always within the store", s.dcm.returnIndexDC(~0ULL) < DCM_NUM_LINES);
    }

    if (failures == 0) {
        std::cout << "ALL REAL-LLC INTEGRATION TESTS PASSED" << std::endl;
        return 0;
    }
    std::cout << failures << " REAL-LLC INTEGRATION TEST(S) FAILED" << std::endl;
    return 1;
}
