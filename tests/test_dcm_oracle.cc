// Deterministic tests for the Oracle policy (DCM_POLICY_ORACLE), ported
// from gem5's enums::RambusHypo (policy_manager.cc:786-893 setNextState
// -- handleNextState for RambusHypo, :1128-1231, was verified textually
// identical in shape to baseline's, so no separate handleNextState port
// was needed here either, exactly as for BEAR-Wr-Opt).
//
// gem5's actual condition is simply `!isDirty` on the OLD resident line
// (`isDirty = checkDirty(addr)`, i.e. validLine && dirtyLine of whatever
// occupied the index before this request) -- NOT "!isHit" and NOT
// "clean XOR cold" as two different cases. This test file verifies the
// real behavior derived from that condition, not an assumption from the
// policy's name:
//   - WRITE HIT: read eliminated (same as BEAR-Wr-Opt).
//   - READ HIT: read NOT eliminated (gem5 still visits locMemRead for
//     isRead&&isHit -- the read fetches DATA, not just a tag).
//   - MISS (read or write) with a CLEAN or COLD/INVALID old resident:
//     read eliminated (Oracle's one exemption beyond BEAR-Wr-Opt).
//   - MISS (read or write) with a DIRTY old resident: read NOT
//     eliminated (it sources the dirty victim's data for write-back).
//
// Build:
//   g++ -std=c++11 -Iinc -o /tmp/test_dcm_oracle \
//       tests/test_dcm_oracle.cc src/dram_cache_manager.cc \
//       src/dram_controller.cc src/block.cc

#include <cstdio>
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

    Harness(const std::string &name, DCM_POLICY pol)
        : near_mc(name + "_N"), far_mc(name + "_F"), dcm(name + "_DCM", &near_mc, &far_mc)
    {
        standardTiming(near_mc);
        standardTiming(far_mc);
        dcm.policy = pol;
        dcm.upper_level_icache[0] = &llc;
        dcm.upper_level_dcache[0] = &llc;
    }
};

static bool doRead(Harness &h, uint64_t addr, uint64_t budget = 20000)
{
    size_t before = h.llc.responses.size();
    PACKET p = makeReadPacket(addr);
    h.dcm.add_rq(&p);
    bool ok = pumpUntil(h.dcm, [&]() { return h.llc.responses.size() > before; }, budget);
    for (int i = 0; i < 300; i++) {
        current_core_cycle[0]++;
        h.dcm.operate();
    }
    return ok;
}

static void doWrite(Harness &h, uint64_t addr, uint64_t settleCycles = 500)
{
    PACKET p = makeWritePacket(addr);
    h.dcm.add_wq(&p);
    for (uint64_t i = 0; i < settleCycles; i++) {
        current_core_cycle[0]++;
        h.dcm.operate();
    }
}

struct Counts {
    uint64_t localReads, localWrites, farReads, farWrites;
};

static Counts snapshot(const DRAM_CACHE_MANAGER &dcm)
{
    Counts c;
    c.localReads = dcm.stats.localReads;
    c.localWrites = dcm.stats.localWrites;
    c.farReads = dcm.stats.farReads;
    c.farWrites = dcm.stats.farWrites;
    return c;
}

static Counts delta(const Counts &after, const Counts &before)
{
    Counts c;
    c.localReads = after.localReads - before.localReads;
    c.localWrites = after.localWrites - before.localWrites;
    c.farReads = after.farReads - before.farReads;
    c.farWrites = after.farWrites - before.farWrites;
    return c;
}

static bool checkCounts(const Counts &got, uint64_t lr, uint64_t lw, uint64_t fr, uint64_t fw)
{
    return got.localReads == lr && got.localWrites == lw && got.farReads == fr && got.farWrites == fw;
}

static void printCounts(const char *label, const Counts &c)
{
    std::cout << label << " local_read=" << c.localReads << " local_write=" << c.localWrites
               << " far_read=" << c.farReads << " far_write=" << c.farWrites << std::endl;
}

int main()
{
    current_core_cycle[0] = 0;
    all_warmup_complete = NUM_CPUS;
    int failures = 0;
    const uint64_t STRIDE = DCM_DRAM_CACHE_SIZE;

    // ==================================================================
    // Test 1: ORACLE READ HIT -- must NOT skip the read (Table II: 1).
    // ==================================================================
    {
        Harness h("OR1", DCM_POLICY_ORACLE);
        uint64_t addr = 0x2000000;
        doRead(h, addr); // install

        Counts before = snapshot(h.dcm);
        std::cout << "---- request-flow log: ORACLE READ HIT ----" << std::endl;
        h.dcm.debugPrint = true;
        bool ok = doRead(h, addr);
        h.dcm.debugPrint = false;
        std::cout << "---- end request-flow log ----" << std::endl;
        Counts d = delta(snapshot(h.dcm), before);
        printCounts("[1: ORACLE READ HIT]", d);

        if (!ok || !checkCounts(d, 1, 0, 0, 0)) {
            std::cerr << "  FAIL (Oracle must NOT eliminate the read for a read hit -- it fetches data, "
                         "not just a tag)"
                      << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Test 2: ORACLE WRITE HIT -- eliminated (Table II: 1, was 2 in baseline).
    // ==================================================================
    {
        Harness h("OR2", DCM_POLICY_ORACLE);
        uint64_t addr = 0x2100000;
        doRead(h, addr);

        Counts before = snapshot(h.dcm);
        uint64_t oracleWHBefore = h.dcm.stats.oracleWriteHits;
        std::cout << "---- request-flow log: ORACLE WRITE HIT ----" << std::endl;
        h.dcm.debugPrint = true;
        doWrite(h, addr);
        h.dcm.debugPrint = false;
        std::cout << "---- end request-flow log ----" << std::endl;
        Counts d = delta(snapshot(h.dcm), before);
        printCounts("[2: ORACLE WRITE HIT]", d);

        if (!checkCounts(d, 0, 1, 0, 0) || (h.dcm.stats.oracleWriteHits - oracleWHBefore) != 1) {
            std::cerr << "  FAIL" << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Test 3: ORACLE READ COLD MISS -- eliminated (Table II: 2, was 3).
    // ==================================================================
    {
        Harness h("OR3", DCM_POLICY_ORACLE);
        uint64_t addr = 0x2200000;
        uint64_t idx = h.dcm.returnIndexDC(addr);
        bool coldBefore = !h.dcm.tagMetadataStore[idx].validLine;

        Counts before = snapshot(h.dcm);
        uint64_t cleanMissBefore = h.dcm.stats.oracleCleanMisses;
        std::cout << "---- request-flow log: ORACLE READ COLD MISS ----" << std::endl;
        h.dcm.debugPrint = true;
        bool ok = doRead(h, addr);
        h.dcm.debugPrint = false;
        std::cout << "---- end request-flow log ----" << std::endl;
        Counts d = delta(snapshot(h.dcm), before);
        printCounts("[3: ORACLE READ COLD MISS]", d);

        if (!ok || !coldBefore || !checkCounts(d, 0, 1, 1, 0) ||
            (h.dcm.stats.oracleCleanMisses - cleanMissBefore) != 1) {
            std::cerr << "  FAIL (Oracle must eliminate the read for a read miss with no dirty victim, "
                         "including cold/invalid)"
                      << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Test 4: ORACLE WRITE COLD MISS -- eliminated (Table II: 1, was 2).
    // ==================================================================
    {
        Harness h("OR4", DCM_POLICY_ORACLE);
        uint64_t addr = 0x2300000;
        uint64_t idx = h.dcm.returnIndexDC(addr);
        bool coldBefore = !h.dcm.tagMetadataStore[idx].validLine;

        Counts before = snapshot(h.dcm);
        doWrite(h, addr);
        Counts d = delta(snapshot(h.dcm), before);
        printCounts("[4: ORACLE WRITE COLD MISS]", d);

        if (!coldBefore || !checkCounts(d, 0, 1, 0, 0)) {
            std::cerr << "  FAIL" << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Test 5: ORACLE READ CLEAN MISS (valid, not cold) -- eliminated.
    // ==================================================================
    {
        Harness h("OR5", DCM_POLICY_ORACLE);
        uint64_t addrA = 0x2400000;
        uint64_t addrB = addrA + STRIDE;
        doRead(h, addrA); // install A, clean, valid (not cold anymore for B's index check)

        Counts before = snapshot(h.dcm);
        bool ok = doRead(h, addrB); // B evicts A (A is valid+clean, not cold)
        Counts d = delta(snapshot(h.dcm), before);
        printCounts("[5: ORACLE READ CLEAN MISS (valid victim)]", d);

        if (!ok || !checkCounts(d, 0, 1, 1, 0) || h.dcm.stats.numWrBacks != 0) {
            std::cerr << "  FAIL" << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Test 6: ORACLE WRITE CLEAN MISS (valid, not cold) -- eliminated.
    // ==================================================================
    {
        Harness h("OR6", DCM_POLICY_ORACLE);
        uint64_t addrA = 0x2500000;
        uint64_t addrB = addrA + STRIDE;
        doRead(h, addrA);

        Counts before = snapshot(h.dcm);
        doWrite(h, addrB);
        Counts d = delta(snapshot(h.dcm), before);
        printCounts("[6: ORACLE WRITE CLEAN MISS (valid victim)]", d);

        if (!checkCounts(d, 0, 1, 0, 0) || h.dcm.stats.numWrBacks != 0) {
            std::cerr << "  FAIL" << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Test 7: ORACLE READ DIRTY MISS -- NOT eliminated (Table II: 4, unchanged).
    // ==================================================================
    {
        Harness h("OR7", DCM_POLICY_ORACLE);
        uint64_t addrA = 0x2600000;
        uint64_t addrB = addrA + STRIDE;
        doRead(h, addrA);
        doWrite(h, addrA); // dirty A (Oracle write hit, skips read)

        Counts before = snapshot(h.dcm);
        std::cout << "---- request-flow log: ORACLE READ DIRTY MISS ----" << std::endl;
        h.dcm.debugPrint = true;
        bool ok = doRead(h, addrB); // evicts dirty A
        h.dcm.debugPrint = false;
        std::cout << "---- end request-flow log ----" << std::endl;
        Counts d = delta(snapshot(h.dcm), before);
        printCounts("[7: ORACLE READ DIRTY MISS]", d);

        if (!ok || !checkCounts(d, 1, 1, 1, 1)) {
            std::cerr << "  FAIL (a dirty victim MUST still be read locally to source its write-back, "
                         "even under Oracle)"
                      << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Test 8: ORACLE WRITE DIRTY MISS -- NOT eliminated (Table II: 3, unchanged).
    // ==================================================================
    {
        Harness h("OR8", DCM_POLICY_ORACLE);
        uint64_t addrA = 0x2700000;
        uint64_t addrB = addrA + STRIDE;
        doRead(h, addrA);
        doWrite(h, addrA); // dirty A

        Counts before = snapshot(h.dcm);
        std::cout << "---- request-flow log: ORACLE WRITE DIRTY MISS ----" << std::endl;
        h.dcm.debugPrint = true;
        doWrite(h, addrB); // evicts dirty A
        h.dcm.debugPrint = false;
        std::cout << "---- end request-flow log ----" << std::endl;
        Counts d = delta(snapshot(h.dcm), before);
        printCounts("[8: ORACLE WRITE DIRTY MISS]", d);

        if (!checkCounts(d, 1, 1, 0, 1)) {
            std::cerr << "  FAIL" << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Test 9: replacement -- new tag/address correctly overwrite old.
    // ==================================================================
    {
        Harness h("OR9", DCM_POLICY_ORACLE);
        uint64_t addrA = 0x2800000;
        uint64_t addrB = addrA + STRIDE;
        uint64_t idx = h.dcm.returnIndexDC(addrA);

        doRead(h, addrA);
        uint64_t tagA = h.dcm.tagMetadataStore[idx].tagDC;
        doRead(h, addrB);
        uint64_t tagB = h.dcm.tagMetadataStore[idx].tagDC;

        std::cout << "[9: replacement] tagA=" << tagA << " tagB=" << tagB
                   << " addr_now=" << h.dcm.tagMetadataStore[idx].farMemAddr << std::endl;

        if (tagA == tagB || h.dcm.tagMetadataStore[idx].farMemAddr != addrB ||
            !h.dcm.tagMetadataStore[idx].validLine) {
            std::cerr << "  FAIL" << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Test 10: dirty eviction -- exactly one write-back, new line clean.
    // ==================================================================
    {
        Harness h("OR10", DCM_POLICY_ORACLE);
        uint64_t addrA = 0x2900000;
        uint64_t addrB = addrA + STRIDE;
        uint64_t idx = h.dcm.returnIndexDC(addrA);

        doRead(h, addrA);
        doWrite(h, addrA);
        bool dirtyBefore = h.dcm.tagMetadataStore[idx].dirtyLine;

        uint64_t wbBefore = h.dcm.stats.numWrBacks;
        doRead(h, addrB);
        uint64_t wbAfter = h.dcm.stats.numWrBacks;
        bool newLineClean = !h.dcm.tagMetadataStore[idx].dirtyLine;

        std::cout << "[10: dirty eviction] dirty_before=" << dirtyBefore
                   << " wb_delta=" << (wbAfter - wbBefore) << " new_line_clean=" << newLineClean
                   << std::endl;

        if (!dirtyBefore || (wbAfter - wbBefore) != 1 || !newLineClean) {
            std::cerr << "  FAIL" << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Test 11: repeated access -- N consecutive hits stay pure local hits.
    // ==================================================================
    {
        Harness h("OR11", DCM_POLICY_ORACLE);
        uint64_t addr = 0x2A00000;
        doRead(h, addr);
        Counts before = snapshot(h.dcm);
        bool allOk = true;
        for (int i = 0; i < 5; i++) {
            if (!doRead(h, addr))
                allOk = false;
        }
        Counts d = delta(snapshot(h.dcm), before);
        printCounts("[11: repeated access, 5x read hit]", d);
        if (!allOk || !checkCounts(d, 5, 0, 0, 0) || h.llc.responses.size() != 6) {
            std::cerr << "  FAIL" << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Test 12: ORB conflict -- admission ordering unaffected by policy.
    // ==================================================================
    {
        Harness h("OR12", DCM_POLICY_ORACLE);
        uint64_t addrA = 0x2B00000;
        uint64_t addrB = addrA + STRIDE;

        PACKET ra = makeReadPacket(addrA); // read miss-clean under Oracle: skips read, goes to far (async)
        h.dcm.add_rq(&ra);
        PACKET wb = makeWritePacket(addrB); // conflicts with A's index
        h.dcm.add_wq(&wb);

        bool immediatelyQueued = (h.dcm.ORB.size() == 1 && h.dcm.CRB.size() == 1);
        std::cout << "[12: ORB conflict] orb_size=" << h.dcm.ORB.size() << " crb_size=" << h.dcm.CRB.size()
                   << " immediately_queued=" << immediatelyQueued << std::endl;

        if (!immediatelyQueued) {
            std::cerr << "  FAIL" << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Test 13: CRB promotion resolves into an Oracle write hit.
    //
    // X = a READ to a fresh address A. Under Oracle, a read COLD miss
    // skips the local read and goes straight to FAR_MEM_READ -- which,
    // unlike a direct-to-LOC_MEM_WRITE skip, genuinely takes real
    // asynchronous time (the far fetch), giving Y a real window to
    // conflict. classifyAndInstall() eagerly installs A's tag at X's own
    // admission (before X's far fetch even completes), so once X retires
    // and Y (a WRITE to the SAME address A) is promoted, Y sees A's tag
    // already resident and classifies as a genuine HIT -- Oracle then
    // eliminates Y's read too.
    // ==================================================================
    {
        Harness h("OR13", DCM_POLICY_ORACLE);
        uint64_t addrA = 0x2C00000;

        std::cout << "---- request-flow log: ORACLE write-hit via CRB promotion ----" << std::endl;
        h.dcm.debugPrint = true;

        PACKET x = makeReadPacket(addrA); // X: cold read miss -> Oracle skips the read, real async far fetch
        h.dcm.add_rq(&x);
        bool xInORB = (h.dcm.ORB.find(addrA) != h.dcm.ORB.end());
        Counts afterXAdmit = snapshot(h.dcm);
        uint64_t oracleWHBefore = h.dcm.stats.oracleWriteHits;

        PACKET y = makeWritePacket(addrA); // Y: SAME address -> conflicts with X, queues in CRB
        h.dcm.add_wq(&y);
        bool yInCRB = (h.dcm.CRB.size() == 1 && h.dcm.CRB[0].pkt.address == addrA);

        bool drained = pumpUntil(h.dcm, [&]() { return h.dcm.ORB.empty() && h.dcm.CRB.empty(); }, 20000);
        h.dcm.debugPrint = false;
        std::cout << "---- end request-flow log ----" << std::endl;

        Counts d = delta(snapshot(h.dcm), afterXAdmit);
        printCounts("[13: ORACLE write-hit via CRB promotion]", d);
        uint64_t oracleWHDelta = h.dcm.stats.oracleWriteHits - oracleWHBefore;

        std::cout << "  x_in_orb=" << xInORB << " y_in_crb=" << yInCRB << " drained=" << drained
                   << " crbPromotions=" << h.dcm.stats.crbPromotions << " oracleWriteHits_for_Y="
                   << oracleWHDelta << std::endl;

        // From X's admission (its far fetch already dispatched) to full
        // drain: X still needs its background fill (localWrites+=1); Y,
        // promoted as a genuine hit, is a write hit -> Oracle eliminates
        // its read entirely, needing only its local write (localWrites+=1).
        // Total: localReads+=0, localWrites+=2, farReads+=0 (X's far read
        // was already dispatched/counted before the snapshot), farWrites+=0.
        if (!xInORB || !yInCRB || !drained || h.dcm.stats.crbPromotions != 1 || oracleWHDelta != 1 ||
            !checkCounts(d, 0, 2, 0, 0)) {
            std::cerr << "  FAIL (a request promoted from the CRB must be re-classified fresh under "
                         "Oracle too, and a resulting write hit must have its read eliminated)"
                      << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Test 14: WB interaction -- a write hit (Oracle-optimized) never
    // touches WB (a hit never evicts anything).
    // ==================================================================
    {
        Harness h("OR14", DCM_POLICY_ORACLE);
        uint64_t addr = 0x2D00000;
        doRead(h, addr);
        uint64_t wbBefore = h.dcm.stats.wbInsertions;
        doWrite(h, addr); // write hit, read eliminated
        std::cout << "[14: WB interaction, write hit] wb_delta=" << (h.dcm.stats.wbInsertions - wbBefore)
                   << std::endl;
        if (h.dcm.stats.wbInsertions != wbBefore) {
            std::cerr << "  FAIL (a write hit must never insert into WB, Oracle included)" << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Test 15: WB pressure -- still blocks admission under Oracle exactly
    // like baseline/BEAR; the optimization must not bypass admission
    // control (checked before classification/chooseInitialState runs).
    // ==================================================================
    {
        Harness h("OR15", DCM_POLICY_ORACLE);
        for (uint32_t i = 0; i < DCM_WB_PRESSURE_THRESHOLD; i++) {
            DCM_WB_ENTRY w;
            w.pkt = makeWritePacket(0x2E00000 + (uint64_t)i * 64);
            h.dcm.WB.push_back(w);
        }
        uint64_t addr = 0x2F00000; // would-be Oracle-optimizable cold miss
        bool blocked = (h.dcm.get_occupancy(1, addr) == h.dcm.get_size(1, addr));
        PACKET p = makeReadPacket(addr);
        h.dcm.add_rq(&p);
        bool wasRejected = (h.dcm.ORB.find(addr) == h.dcm.ORB.end());

        std::cout << "[15: WB pressure] blocked=" << blocked << " was_rejected=" << wasRejected
                   << std::endl;

        if (!blocked || !wasRejected) {
            std::cerr << "  FAIL" << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Test 16: metadata correctness after a read-clean-miss SKIP -- the
    // eager classifyAndInstall() metadata update must be correct even
    // though the physical local read never happens for this path.
    // ==================================================================
    {
        Harness h("OR16", DCM_POLICY_ORACLE);
        uint64_t addr = 0x3000000;
        uint64_t idx = h.dcm.returnIndexDC(addr);

        doRead(h, addr); // cold miss, read skipped entirely (far fetch + fill only)

        bool metaOk = h.dcm.tagMetadataStore[idx].validLine && !h.dcm.tagMetadataStore[idx].dirtyLine &&
                      h.dcm.tagMetadataStore[idx].farMemAddr == addr;

        std::cout << "[16: metadata after read-clean-miss skip] valid=" << h.dcm.tagMetadataStore[idx].validLine
                   << " dirty=" << h.dcm.tagMetadataStore[idx].dirtyLine
                   << " addr_match=" << (h.dcm.tagMetadataStore[idx].farMemAddr == addr) << std::endl;

        if (!metaOk) {
            std::cerr << "  FAIL (metadata must be correctly installed even when the physical tag-check "
                         "read is skipped)"
                      << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Test 17: Baseline vs. BEAR-Wr-Opt vs. Oracle comparison table.
    //
    // Runs each of the paper's core scenarios under all three policies
    // and prints/verifies local_read/local_write/far_read/far_write for
    // each. The DIFFERENCES must correspond exactly to Table II.
    // ==================================================================
    {
        struct Scenario {
            const char *name;
            // returns the op-count delta for just the case-under-test step
            Counts (*run)(DCM_POLICY pol, const char *tag);
        };

        std::cout << "\n==================== POLICY COMPARISON TABLE ====================" << std::endl;
        std::cout << "scenario                  | policy | local_rd | local_wr | far_rd | far_wr"
                   << std::endl;

        const char *policyNames[3] = {"BASELINE", "BEAR-Wr-Opt", "ORACLE"};
        DCM_POLICY policies[3] = {DCM_POLICY_BASELINE_CASCADE_LAKE, DCM_POLICY_BEAR_WR_OPT,
                                   DCM_POLICY_ORACLE};

        // Expected [scenario][policy] = {lr, lw, fr, fw}
        struct Expected {
            const char *scenario;
            Counts expect[3];
        };
        Expected table[] = {
            {"read hit", {{1, 0, 0, 0}, {1, 0, 0, 0}, {1, 0, 0, 0}}},
            {"write hit", {{1, 1, 0, 0}, {0, 1, 0, 0}, {0, 1, 0, 0}}},
            {"read miss clean", {{1, 1, 1, 0}, {1, 1, 1, 0}, {0, 1, 1, 0}}},
            {"write miss clean", {{1, 1, 0, 0}, {1, 1, 0, 0}, {0, 1, 0, 0}}},
            {"read miss dirty", {{1, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 1}}},
            {"write miss dirty", {{1, 1, 0, 1}, {1, 1, 0, 1}, {1, 1, 0, 1}}},
        };

        bool allMatch = true;
        for (int s = 0; s < 6; s++) {
            for (int p = 0; p < 3; p++) {
                std::string name = std::string("CMP_") + table[s].scenario + "_" + policyNames[p];
                for (char &c : name)
                    if (c == ' ')
                        c = '_';
                Harness h(name, policies[p]);
                uint64_t addrA = 0x4000000 + (uint64_t)(s * 3 + p) * STRIDE * 4;
                uint64_t addrB = addrA + STRIDE;
                Counts got;

                std::string scen = table[s].scenario;
                if (scen == "read hit") {
                    doRead(h, addrA);
                    Counts before = snapshot(h.dcm);
                    doRead(h, addrA);
                    got = delta(snapshot(h.dcm), before);
                } else if (scen == "write hit") {
                    doRead(h, addrA);
                    Counts before = snapshot(h.dcm);
                    doWrite(h, addrA);
                    got = delta(snapshot(h.dcm), before);
                } else if (scen == "read miss clean") {
                    doRead(h, addrA);
                    Counts before = snapshot(h.dcm);
                    doRead(h, addrB);
                    got = delta(snapshot(h.dcm), before);
                } else if (scen == "write miss clean") {
                    doRead(h, addrA);
                    Counts before = snapshot(h.dcm);
                    doWrite(h, addrB);
                    got = delta(snapshot(h.dcm), before);
                } else if (scen == "read miss dirty") {
                    doRead(h, addrA);
                    doWrite(h, addrA);
                    Counts before = snapshot(h.dcm);
                    doRead(h, addrB);
                    got = delta(snapshot(h.dcm), before);
                } else { // write miss dirty
                    doRead(h, addrA);
                    doWrite(h, addrA);
                    Counts before = snapshot(h.dcm);
                    doWrite(h, addrB);
                    got = delta(snapshot(h.dcm), before);
                }

                Counts exp = table[s].expect[p];
                bool ok = checkCounts(got, exp.localReads, exp.localWrites, exp.farReads, exp.farWrites);
                allMatch = allMatch && ok;

                printf("%-25s | %-11s | %8lu | %8lu | %6lu | %6lu %s\n", table[s].scenario, policyNames[p],
                       (unsigned long)got.localReads, (unsigned long)got.localWrites,
                       (unsigned long)got.farReads, (unsigned long)got.farWrites, ok ? "" : "<-- MISMATCH");
            }
        }
        std::cout << "===================================================================\n" << std::endl;

        std::cout << "[17: baseline vs BEAR vs Oracle comparison table]" << std::endl;
        if (!allMatch) {
            std::cerr << "  FAIL (comparison table did not match the expected Table-II-derived values)"
                      << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    if (failures == 0) {
        std::cout << "ALL ORACLE TESTS PASSED" << std::endl;
        return 0;
    } else {
        std::cout << failures << " ORACLE TEST(S) FAILED" << std::endl;
        return 1;
    }
}
