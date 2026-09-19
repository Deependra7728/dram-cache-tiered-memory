// Deterministic tests for the BEAR-Wr-Opt policy
// (DCM_POLICY_BEAR_WR_OPT), ported from gem5's enums::BearWriteOpt
// (policy_manager.cc:897-911 setNextState, :1234-1341 handleNextState --
// which is otherwise byte-for-byte identical to baseline's
// handleNextState for every state).
//
// The paper (Section V-a) and gem5 agree on exactly one behavioral
// difference from baseline: a WRITE that HITS in the DRAM cache skips
// the local tag-check read entirely and goes straight to the local
// write. Every other case -- read hit, read miss (clean or dirty
// victim), write miss (clean or dirty victim) -- is byte-for-byte
// identical to baseline. This is verified here both by exact Table II
// operation-count assertions AND by diffing BEAR's request-flow log
// against baseline's for equivalent non-write-hit scenarios.
//
// Build:
//   g++ -std=c++11 -Iinc -o /tmp/test_dcm_bear \
//       tests/test_dcm_bear.cc src/dram_cache_manager.cc \
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
    // Test 1: BEAR WRITE HIT -- the one case that differs from baseline.
    // Table II: BEAR-Wr-Opt Write-Hit total = 1 (baseline = 2).
    // ==================================================================
    {
        Harness h("BEAR1", DCM_POLICY_BEAR_WR_OPT);
        uint64_t addr = 0x1000000;
        doRead(h, addr); // install, clean (not the case under test)

        Counts before = snapshot(h.dcm);
        uint64_t oppBefore = h.dcm.stats.writeHitOptOpportunities;
        uint64_t appliedBefore = h.dcm.stats.writeHitOptApplied;

        std::cout << "---- request-flow log: BEAR WRITE HIT ----" << std::endl;
        h.dcm.debugPrint = true;
        doWrite(h, addr);
        h.dcm.debugPrint = false;
        std::cout << "---- end request-flow log ----" << std::endl;

        Counts d = delta(snapshot(h.dcm), before);
        printCounts("[1: BEAR WRITE HIT]", d);

        uint64_t idx = h.dcm.returnIndexDC(addr);
        bool metaOk = h.dcm.tagMetadataStore[idx].validLine && h.dcm.tagMetadataStore[idx].dirtyLine &&
                      h.dcm.tagMetadataStore[idx].farMemAddr == addr;

        // The Table-II-defining assertion: NO local read at all (0, not 1).
        if (!checkCounts(d, 0, 1, 0, 0) || !metaOk ||
            (h.dcm.stats.writeHitOptOpportunities - oppBefore) != 1 ||
            (h.dcm.stats.writeHitOptApplied - appliedBefore) != 1) {
            std::cerr << "  FAIL (BEAR write hit MUST eliminate the local tag-check read: expected "
                         "local_read=0, got "
                      << d.localReads << ")" << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Test 2: BEAR WRITE MISS + COLD/INVALID -- must NOT skip the read.
    // ==================================================================
    {
        Harness h("BEAR2", DCM_POLICY_BEAR_WR_OPT);
        uint64_t addr = 0x1100000;
        uint64_t idx = h.dcm.returnIndexDC(addr);
        bool coldBefore = !h.dcm.tagMetadataStore[idx].validLine;

        Counts before = snapshot(h.dcm);
        uint64_t appliedBefore = h.dcm.stats.writeHitOptApplied;
        doWrite(h, addr);
        Counts d = delta(snapshot(h.dcm), before);
        printCounts("[2: BEAR WRITE MISS + COLD]", d);

        if (!coldBefore || !checkCounts(d, 1, 1, 0, 0) ||
            h.dcm.stats.writeHitOptApplied != appliedBefore) {
            std::cerr << "  FAIL (a write MISS must never skip the tag-check read, even under BEAR)"
                      << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Test 3: BEAR WRITE MISS + CLEAN VICTIM
    // ==================================================================
    {
        Harness h("BEAR3", DCM_POLICY_BEAR_WR_OPT);
        uint64_t addrA = 0x1200000;
        uint64_t addrB = addrA + STRIDE;
        doRead(h, addrA); // install A, clean

        Counts before = snapshot(h.dcm);
        uint64_t appliedBefore = h.dcm.stats.writeHitOptApplied;
        doWrite(h, addrB); // evicts A (clean) -> no write-back, still needs tag-check read
        Counts d = delta(snapshot(h.dcm), before);
        printCounts("[3: BEAR WRITE MISS + CLEAN VICTIM]", d);

        if (!checkCounts(d, 1, 1, 0, 0) || h.dcm.stats.writeHitOptApplied != appliedBefore ||
            h.dcm.stats.numWrBacks != 0) {
            std::cerr << "  FAIL" << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Test 4: BEAR WRITE MISS + DIRTY VICTIM
    // ==================================================================
    {
        Harness h("BEAR4", DCM_POLICY_BEAR_WR_OPT);
        uint64_t addrA = 0x1300000;
        uint64_t addrB = addrA + STRIDE;
        doRead(h, addrA);
        doWrite(h, addrA); // dirty A (this itself is a BEAR write-hit, local_read=0)

        Counts before = snapshot(h.dcm);
        uint64_t appliedBefore = h.dcm.stats.writeHitOptApplied;
        std::cout << "---- request-flow log: BEAR WRITE MISS + DIRTY VICTIM ----" << std::endl;
        h.dcm.debugPrint = true;
        doWrite(h, addrB); // evicts dirty A -> write-back required, needs tag-check read
        h.dcm.debugPrint = false;
        std::cout << "---- end request-flow log ----" << std::endl;
        Counts d = delta(snapshot(h.dcm), before);
        printCounts("[4: BEAR WRITE MISS + DIRTY VICTIM]", d);

        if (!checkCounts(d, 1, 1, 0, 1) || h.dcm.stats.writeHitOptApplied != appliedBefore) {
            std::cerr << "  FAIL (write MISS with a dirty victim must still do the tag-check read and "
                         "the write-back, exactly like baseline)"
                      << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Test 5: BEAR READ HIT -- must be IDENTICAL to baseline (not skipped).
    // ==================================================================
    {
        Harness h("BEAR5", DCM_POLICY_BEAR_WR_OPT);
        uint64_t addr = 0x1400000;
        doRead(h, addr);

        Counts before = snapshot(h.dcm);
        uint64_t oppBefore = h.dcm.stats.writeHitOptOpportunities;
        bool ok = doRead(h, addr); // genuine read hit
        Counts d = delta(snapshot(h.dcm), before);
        printCounts("[5: BEAR READ HIT]", d);

        if (!ok || !checkCounts(d, 1, 0, 0, 0) || h.dcm.stats.writeHitOptOpportunities != oppBefore) {
            std::cerr << "  FAIL (BEAR must NEVER eliminate the tag-check read for a READ, hit or miss)"
                      << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Test 6: BEAR READ MISS + CLEAN VICTIM
    // ==================================================================
    {
        Harness h("BEAR6", DCM_POLICY_BEAR_WR_OPT);
        uint64_t addrA = 0x1500000;
        uint64_t addrB = addrA + STRIDE;
        doRead(h, addrA);

        Counts before = snapshot(h.dcm);
        bool ok = doRead(h, addrB);
        Counts d = delta(snapshot(h.dcm), before);
        printCounts("[6: BEAR READ MISS + CLEAN VICTIM]", d);

        if (!ok || !checkCounts(d, 1, 1, 1, 0) || h.dcm.stats.numWrBacks != 0) {
            std::cerr << "  FAIL" << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Test 7: BEAR READ MISS + DIRTY VICTIM
    // ==================================================================
    {
        Harness h("BEAR7", DCM_POLICY_BEAR_WR_OPT);
        uint64_t addrA = 0x1600000;
        uint64_t addrB = addrA + STRIDE;
        doRead(h, addrA);
        doWrite(h, addrA); // dirty A (BEAR write-hit, local_read=0 for this step)

        Counts before = snapshot(h.dcm);
        bool ok = doRead(h, addrB); // evicts dirty A
        Counts d = delta(snapshot(h.dcm), before);
        printCounts("[7: BEAR READ MISS + DIRTY VICTIM]", d);

        if (!ok || !checkCounts(d, 1, 1, 1, 1)) {
            std::cerr << "  FAIL (matches Table II Read-Miss-Dirty = 4 total, unchanged from baseline)"
                      << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Test 8: metadata update after fill (miss -> new line installed)
    // ==================================================================
    {
        Harness h("BEAR8", DCM_POLICY_BEAR_WR_OPT);
        uint64_t addr = 0x1700000;
        uint64_t idx = h.dcm.returnIndexDC(addr);
        doRead(h, addr); // cold miss -> fill

        bool metaOk = h.dcm.tagMetadataStore[idx].validLine && !h.dcm.tagMetadataStore[idx].dirtyLine;
        bool addrOk = h.dcm.tagMetadataStore[idx].farMemAddr == addr;

        std::cout << "[8: metadata after fill] valid=" << h.dcm.tagMetadataStore[idx].validLine
                   << " dirty=" << h.dcm.tagMetadataStore[idx].dirtyLine << " addr_match=" << addrOk
                   << std::endl;

        if (!metaOk || !addrOk) {
            std::cerr << "  FAIL" << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Test 9: metadata update after replacement (miss evicts, installs new tag)
    // ==================================================================
    {
        Harness h("BEAR9", DCM_POLICY_BEAR_WR_OPT);
        uint64_t addrA = 0x1800000;
        uint64_t addrB = addrA + STRIDE;
        uint64_t idx = h.dcm.returnIndexDC(addrA);

        doRead(h, addrA);
        uint64_t installedTagA = h.dcm.tagMetadataStore[idx].tagDC;

        doRead(h, addrB); // replacement: B evicts A at the same index
        uint64_t installedTagB = h.dcm.tagMetadataStore[idx].tagDC;

        std::cout << "[9: metadata after replacement] tagA=" << installedTagA
                   << " tagB=" << installedTagB << " addr_now=" << h.dcm.tagMetadataStore[idx].farMemAddr
                   << std::endl;

        if (installedTagA == installedTagB || h.dcm.tagMetadataStore[idx].farMemAddr != addrB ||
            !h.dcm.tagMetadataStore[idx].validLine) {
            std::cerr << "  FAIL (replacement must install B's tag/address, distinct from A's)"
                      << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Test 10: metadata after dirty eviction (dirty bit cleared for the new line)
    // ==================================================================
    {
        Harness h("BEAR10", DCM_POLICY_BEAR_WR_OPT);
        uint64_t addrA = 0x1900000;
        uint64_t addrB = addrA + STRIDE;
        uint64_t idx = h.dcm.returnIndexDC(addrA);

        doRead(h, addrA);
        doWrite(h, addrA); // dirty A
        bool dirtyBefore = h.dcm.tagMetadataStore[idx].dirtyLine;

        uint64_t wbBefore = h.dcm.stats.numWrBacks;
        doRead(h, addrB); // evicts dirty A -> write-back, installs B clean
        uint64_t wbAfter = h.dcm.stats.numWrBacks;

        bool newLineClean = !h.dcm.tagMetadataStore[idx].dirtyLine;

        std::cout << "[10: metadata after dirty eviction] dirty_before=" << dirtyBefore
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
    // Test 11: a CRB-promoted request resolves into a BEAR write hit.
    //
    // A genuine "BEAR write-hit blocked mid-flight by a conflict" cannot
    // be constructed directly: chooseInitialState() makes a write hit
    // skip the only asynchronous step (the tag-check read), so it
    // completes SYNCHRONOUSLY, within the very add_wq() call that
    // admitted it -- there is no window in which anything else could
    // observe it as "outstanding" (confirmed empirically: an earlier
    // version of this test tried exactly that and found the ORB already
    // empty by the time the "conflicting" request was submitted).
    //
    // Instead: X = a write MISS to a fresh address A (genuinely
    // outstanding for the tag-check-read's real latency). While X is
    // still in the ORB, Y = ANOTHER write to the SAME address A is
    // submitted -- same address means same index, so Y correctly
    // conflicts with X and queues in the CRB. classifyAndInstall()
    // installs X's tag EAGERLY at X's OWN admission (before X's tag-check
    // read even completes), so by the time X retires and Y is promoted,
    // the resident tag already equals A's tag -- Y, being the same
    // address, resolves into a genuine HIT, and since Y is a write, BEAR
    // applies the read-elimination optimization to a request that only
    // existed because of CRB promotion.
    // ==================================================================
    {
        Harness h("BEAR11", DCM_POLICY_BEAR_WR_OPT);
        uint64_t addrA = 0x1A00000; // fresh -- X is a cold miss

        std::cout << "---- request-flow log: BEAR write-hit via CRB promotion ----" << std::endl;
        h.dcm.debugPrint = true;

        PACKET x = makeWritePacket(addrA);
        h.dcm.add_wq(&x); // X: cold write miss, real outstanding time
        bool xInORB = (h.dcm.ORB.find(addrA) != h.dcm.ORB.end());
        Counts afterXAdmit = snapshot(h.dcm);
        uint64_t appliedAfterXAdmit = h.dcm.stats.writeHitOptApplied;

        PACKET y = makeWritePacket(addrA); // Y: SAME address -> conflicts with X
        h.dcm.add_wq(&y);
        bool yInCRB = (h.dcm.CRB.size() == 1 && h.dcm.CRB[0].pkt.address == addrA);

        bool drained = pumpUntil(h.dcm, [&]() { return h.dcm.ORB.empty() && h.dcm.CRB.empty(); }, 20000);
        h.dcm.debugPrint = false;
        std::cout << "---- end request-flow log ----" << std::endl;

        Counts d = delta(snapshot(h.dcm), afterXAdmit);
        printCounts("[11: BEAR write-hit via CRB promotion]", d);
        uint64_t appliedDelta = h.dcm.stats.writeHitOptApplied - appliedAfterXAdmit;

        std::cout << "  x_in_orb=" << xInORB << " y_in_crb=" << yInCRB << " drained=" << drained
                   << " crbPromotions=" << h.dcm.stats.crbPromotions << " writeHitOptApplied_for_Y="
                   << appliedDelta << std::endl;

        // From the point X is admitted (its own tag-check read already
        // dispatched and counted) to full drain: X still needs its local
        // write (localWrites+=1); Y, promoted as a genuine hit, skips its
        // read entirely and only needs its local write (localWrites+=1,
        // localReads+=0). Total: localReads+=0, localWrites+=2.
        if (!xInORB || !yInCRB || !drained || h.dcm.stats.crbPromotions != 1 || appliedDelta != 1 ||
            !checkCounts(d, 0, 2, 0, 0)) {
            std::cerr << "  FAIL (a request promoted from the CRB must be re-classified fresh, and if it "
                         "resolves to a write hit, BEAR must apply the same read-elimination optimization "
                         "to it as to any other write hit)"
                      << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Test 12: ORB/CRB interaction -- admission ordering unaffected by policy
    // ==================================================================
    {
        Harness h("BEAR12", DCM_POLICY_BEAR_WR_OPT);
        uint64_t addrA = 0x1B00000;
        uint64_t addrB = addrA + STRIDE;

        PACKET ra = makeReadPacket(addrA); // cold miss, occupies ORB for a while
        h.dcm.add_rq(&ra);

        PACKET wb = makeWritePacket(addrB); // conflicts with A's index
        h.dcm.add_wq(&wb);

        bool immediatelyQueued = (h.dcm.ORB.size() == 1 && h.dcm.CRB.size() == 1);

        std::cout << "[12: ORB/CRB interaction] orb_size=" << h.dcm.ORB.size()
                   << " crb_size=" << h.dcm.CRB.size() << " immediately_queued=" << immediatelyQueued
                   << std::endl;

        if (!immediatelyQueued) {
            std::cerr << "  FAIL (admission ordering -- conflict check before ORB-full check -- must be "
                         "identical regardless of active policy)"
                      << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Test 13a: WB interaction -- a write HIT never touches WB
    // ==================================================================
    {
        Harness h("BEAR13A", DCM_POLICY_BEAR_WR_OPT);
        uint64_t addr = 0x1C00000;
        doRead(h, addr);
        uint64_t wbBefore = h.dcm.stats.wbInsertions;
        doWrite(h, addr); // write hit -- must never touch WB (a hit never evicts)
        std::cout << "[13a: WB interaction, write hit] wb_delta=" << (h.dcm.stats.wbInsertions - wbBefore)
                   << std::endl;
        if (h.dcm.stats.wbInsertions != wbBefore) {
            std::cerr << "  FAIL (a write hit must never insert into WB)" << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Test 13b: WB interaction -- WB pressure still blocks admission
    // under BEAR exactly like baseline (the optimization must not bypass
    // admission control).
    // ==================================================================
    {
        Harness h("BEAR13B", DCM_POLICY_BEAR_WR_OPT);
        for (uint32_t i = 0; i < DCM_WB_PRESSURE_THRESHOLD; i++) {
            DCM_WB_ENTRY w;
            w.pkt = makeWritePacket(0x1D00000 + (uint64_t)i * 64);
            h.dcm.WB.push_back(w);
        }
        // Even a would-be write-hit-eligible address must be blocked --
        // WB pressure is checked at admission, before classification/
        // chooseInitialState ever runs.
        uint64_t addr = 0x1E00000;
        bool blocked = (h.dcm.get_occupancy(1, addr) == h.dcm.get_size(1, addr));
        PACKET p = makeReadPacket(addr);
        h.dcm.add_rq(&p);
        bool wasRejected = (h.dcm.ORB.find(addr) == h.dcm.ORB.end());

        std::cout << "[13b: WB interaction, pressure] blocked=" << blocked
                   << " was_rejected=" << wasRejected << std::endl;

        if (!blocked || !wasRejected) {
            std::cerr << "  FAIL (WB pressure must block admission under BEAR exactly like baseline -- "
                         "the optimization must not bypass admission control)"
                      << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Test 14: baseline-vs-BEAR request-flow diff for a NON-write-hit
    // scenario -- must be byte-for-byte identical (only the request id
    // prefix and DCM instance names differ).
    // ==================================================================
    {
        std::cout << "---- request-flow log: BASELINE, read-miss-dirty-victim ----" << std::endl;
        Harness base("DIFF_BASE", DCM_POLICY_BASELINE_CASCADE_LAKE);
        uint64_t addrA = 0x1F00000;
        uint64_t addrB = addrA + STRIDE;
        doRead(base, addrA);
        doWrite(base, addrA);
        base.dcm.debugPrint = true;
        doRead(base, addrB);
        base.dcm.debugPrint = false;
        std::cout << "---- end ----" << std::endl;

        std::cout << "---- request-flow log: BEAR-Wr-Opt, SAME scenario (read-miss-dirty-victim) ----"
                   << std::endl;
        Harness bear("DIFF_BEAR", DCM_POLICY_BEAR_WR_OPT);
        doRead(bear, addrA);
        doWrite(bear, addrA); // this step DOES differ (write hit, skips read) -- expected
        bear.dcm.debugPrint = true;
        doRead(bear, addrB); // this step must be IDENTICAL to baseline's
        bear.dcm.debugPrint = false;
        std::cout << "---- end ----" << std::endl;

        Counts baseCounts, bearCounts;
        baseCounts.localReads = base.dcm.stats.localReads;
        baseCounts.localWrites = base.dcm.stats.localWrites;
        baseCounts.farReads = base.dcm.stats.farReads;
        baseCounts.farWrites = base.dcm.stats.farWrites;
        bearCounts.localReads = bear.dcm.stats.localReads;
        bearCounts.localWrites = bear.dcm.stats.localWrites;
        bearCounts.farReads = bear.dcm.stats.farReads;
        bearCounts.farWrites = bear.dcm.stats.farWrites;

        // Over the whole 3-step scenario (cold read, write-hit, read-miss-
        // dirty-victim): baseline's write-hit costs 1 extra local_read
        // that BEAR's doesn't. Everything else identical. So:
        //   baseline: localReads = bear.localReads + 1, all else equal.
        std::cout << "[14: baseline vs BEAR diff] base=(" << baseCounts.localReads << ","
                   << baseCounts.localWrites << "," << baseCounts.farReads << "," << baseCounts.farWrites
                   << ") bear=(" << bearCounts.localReads << "," << bearCounts.localWrites << ","
                   << bearCounts.farReads << "," << bearCounts.farWrites << ")" << std::endl;

        if (baseCounts.localReads != bearCounts.localReads + 1 ||
            baseCounts.localWrites != bearCounts.localWrites || baseCounts.farReads != bearCounts.farReads ||
            baseCounts.farWrites != bearCounts.farWrites) {
            std::cerr << "  FAIL (the ONLY difference between baseline and BEAR over this scenario must "
                         "be exactly one fewer local_read for BEAR -- from the write-hit step)"
                      << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    if (failures == 0) {
        std::cout << "ALL BEAR-WR-OPT TESTS PASSED" << std::endl;
        return 0;
    } else {
        std::cout << failures << " BEAR-WR-OPT TEST(S) FAILED" << std::endl;
        return 1;
    }
}
