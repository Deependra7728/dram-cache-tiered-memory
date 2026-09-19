// Deterministic tests for the DRAM_CACHE_MANAGER's COMPLETE BASELINE
// (CascadeLakeNoPartWrs) request path: real tag/metadata lookup,
// hit/miss/dirty classification, victim identification, dirty
// write-back, and cache-line installation. BEAR-Wr-Opt and Oracle are
// NOT covered here (not implemented).
//
// This is the paper's own Table II (arXiv:2303.13029, Section V) used as
// an automated correctness oracle, derived here directly from gem5's
// actual state machine (policy_manager.cc) rather than assumed:
//
//   A. READ  HIT            : local_read=1 local_write=0 far_read=0 far_write=0
//   B. WRITE HIT             : local_read=1 local_write=1 far_read=0 far_write=0
//   C. READ  MISS + COLD     : local_read=1 local_write=1 far_read=1 far_write=0
//   D. READ  MISS + CLEAN    : local_read=1 local_write=1 far_read=1 far_write=0
//   E. READ  MISS + DIRTY    : local_read=1 local_write=1 far_read=1 far_write=1
//   F. WRITE MISS + COLD     : local_read=1 local_write=1 far_read=0 far_write=0
//   G. WRITE MISS + CLEAN    : local_read=1 local_write=1 far_read=0 far_write=0
//   H. WRITE MISS + DIRTY    : local_read=1 local_write=1 far_read=0 far_write=1
//
// (Totals per case -- 1,2,3,3,4,2,2,3 respectively when write/read hit
// dirty/clean are collapsed -- match the paper's Table II "Tot. Baseline"
// row "1 1 4 3 2 2 3 2" exactly, for [RdHitDirty, RdHitClean, RdMissDirty,
// RdMissClean, WrHitDirty, WrHitClean, WrMissDirty, WrMissClean].)
//
// Build:
//   g++ -std=c++11 -Iinc -o /tmp/test_dcm_baseline \
//       tests/test_dcm_baseline.cc src/dram_cache_manager.cc \
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
    uint32_t dbus = (BLOCK_SIZE / DRAM_CHANNEL_WIDTH) * (CPU_FREQ / mtps);
    mc.set_timing(trp, trcd, tcas, mtps, dbus);
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

// Bundles one DCM + its two memory controllers + a FAKE_LLC. One Harness
// per test case keeps tagMetadataStore state isolated between cases.
struct Harness {
    MEMORY_CONTROLLER near_mc, far_mc;
    DRAM_CACHE_MANAGER dcm;
    FAKE_LLC llc;

    Harness(const std::string &name) : near_mc(name + "_N"), far_mc(name + "_F"), dcm(name + "_DCM", &near_mc, &far_mc)
    {
        standardTiming(near_mc);
        standardTiming(far_mc);
        dcm.upper_level_icache[0] = &llc;
        dcm.upper_level_dcache[0] = &llc;
    }
};

// Issues a read and pumps until it completes (plus a short settle window
// to drain any background fill write). Returns true if it completed.
static bool doRead(Harness &h, uint64_t addr, uint64_t budget = 20000)
{
    size_t before = h.llc.responses.size();
    PACKET p = makeReadPacket(addr);
    h.dcm.add_rq(&p);
    bool ok = pumpUntil(h.dcm, [&]() { return h.llc.responses.size() > before; }, budget);
    for (int i = 0; i < 300; i++) { // drain background fill / write-back
        current_core_cycle[0]++;
        h.dcm.operate();
    }
    return ok;
}

// Issues a write and settles it (writes get no callback -- see
// gem5_to_champsim_mapping.md fact #2).
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

    const uint64_t STRIDE = DCM_DRAM_CACHE_SIZE; // adding this to an address keeps the same index, different tag

    // ==================================================================
    // Case A: READ HIT
    // ==================================================================
    {
        Harness h("A");
        uint64_t addr = 0x100000;
        doRead(h, addr); // cold miss, installs clean line (not the case under test)
        Counts before = snapshot(h.dcm);
        bool ok = doRead(h, addr); // genuine hit
        Counts d = delta(snapshot(h.dcm), before);
        printCounts("[A: READ HIT]", d);
        uint64_t idx = h.dcm.returnIndexDC(addr);
        bool metaOk = h.dcm.tagMetadataStore[idx].validLine && !h.dcm.tagMetadataStore[idx].dirtyLine &&
                      h.dcm.tagMetadataStore[idx].farMemAddr == addr;
        if (!ok || !checkCounts(d, 1, 0, 0, 0) || !metaOk) {
            std::cerr << "  FAIL" << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Case B: WRITE HIT
    // ==================================================================
    {
        Harness h("B");
        uint64_t addr = 0x200000;
        doRead(h, addr); // install the line first (clean)
        Counts before = snapshot(h.dcm);
        doWrite(h, addr); // write to the SAME address -> hit
        Counts d = delta(snapshot(h.dcm), before);
        printCounts("[B: WRITE HIT]", d);
        uint64_t idx = h.dcm.returnIndexDC(addr);
        bool metaOk = h.dcm.tagMetadataStore[idx].validLine && h.dcm.tagMetadataStore[idx].dirtyLine &&
                      h.dcm.tagMetadataStore[idx].farMemAddr == addr;
        if (!checkCounts(d, 1, 1, 0, 0) || !metaOk) {
            std::cerr << "  FAIL (expect dirty=true after a write hit)" << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Case C: READ MISS + INVALID/COLD
    // ==================================================================
    { 
        Harness h("C");
        uint64_t addr = 0x300000;
        uint64_t idx = h.dcm.returnIndexDC(addr);
        bool coldBefore = !h.dcm.tagMetadataStore[idx].validLine;

        Counts before = snapshot(h.dcm);
        std::cout << "---- request-flow log: READ MISS + COLD ----" << std::endl;
        h.dcm.debugPrint = true;
        bool ok = doRead(h, addr);
        h.dcm.debugPrint = false;
        std::cout << "---- end request-flow log ----" << std::endl;
        Counts d = delta(snapshot(h.dcm), before);
        printCounts("[C: READ MISS + COLD]", d);

        bool metaAfter = h.dcm.tagMetadataStore[idx].validLine && !h.dcm.tagMetadataStore[idx].dirtyLine &&
                          h.dcm.tagMetadataStore[idx].farMemAddr == addr;

        if (!ok || !coldBefore || !checkCounts(d, 1, 1, 1, 0) || !metaAfter || h.dcm.stats.numColdMisses != 1 ||
            h.dcm.stats.numRdMissClean != 1 || h.dcm.stats.numWrBacks != 0) {
            std::cerr << "  FAIL" << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Case D: READ MISS + CLEAN VICTIM
    // ==================================================================
    {
        Harness h("D");
        uint64_t addrA = 0x400000;
        uint64_t addrB = addrA + STRIDE; // same index, different tag
        doRead(h, addrA); // install A, clean (itself a cold RdMissClean --
                           // captured in "before" below so it isn't double-counted)

        Counts before = snapshot(h.dcm);
        uint64_t hotBefore = h.dcm.stats.numHotMisses;
        uint64_t rdMissCleanBefore = h.dcm.stats.numRdMissClean;
        uint64_t wrBacksBefore = h.dcm.stats.numWrBacks;

        bool ok = doRead(h, addrB); // evicts A (clean) -> no write-back
        Counts d = delta(snapshot(h.dcm), before);
        printCounts("[D: READ MISS + CLEAN VICTIM]", d);

        uint64_t idx = h.dcm.returnIndexDC(addrA);
        bool metaAfter = h.dcm.tagMetadataStore[idx].validLine && !h.dcm.tagMetadataStore[idx].dirtyLine &&
                          h.dcm.tagMetadataStore[idx].farMemAddr == addrB;

        if (!ok || !checkCounts(d, 1, 1, 1, 0) || !metaAfter ||
            (h.dcm.stats.numHotMisses - hotBefore) != 1 ||
            (h.dcm.stats.numRdMissClean - rdMissCleanBefore) != 1 ||
            (h.dcm.stats.numWrBacks - wrBacksBefore) != 0) {
            std::cerr << "  FAIL (clean victim must NOT trigger a write-back)" << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Case E: READ MISS + DIRTY VICTIM
    // ==================================================================
    {
        Harness h("E");
        uint64_t addrA = 0x500000;
        uint64_t addrB = addrA + STRIDE;
        doRead(h, addrA);   // install A, clean
        doWrite(h, addrA);  // dirty A (write hit)

        uint64_t idx = h.dcm.returnIndexDC(addrA);
        bool dirtyBefore = h.dcm.tagMetadataStore[idx].validLine && h.dcm.tagMetadataStore[idx].dirtyLine;

        Counts before = snapshot(h.dcm);
        std::cout << "---- request-flow log: READ MISS + DIRTY VICTIM ----" << std::endl;
        h.dcm.debugPrint = true;
        bool ok = doRead(h, addrB); // evicts dirty A -> write-back required
        h.dcm.debugPrint = false;
        std::cout << "---- end request-flow log ----" << std::endl;
        Counts d = delta(snapshot(h.dcm), before);
        printCounts("[E: READ MISS + DIRTY VICTIM]", d);

        bool metaAfter = h.dcm.tagMetadataStore[idx].validLine && !h.dcm.tagMetadataStore[idx].dirtyLine &&
                          h.dcm.tagMetadataStore[idx].farMemAddr == addrB;

        if (!ok || !dirtyBefore || !checkCounts(d, 1, 1, 1, 1) || !metaAfter || h.dcm.stats.numRdMissDirty != 1 ||
            h.dcm.stats.numWrBacks != 1) {
            std::cerr << "  FAIL (dirty victim MUST trigger exactly one write-back, and the new line "
                         "must install clean)"
                      << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Case F: WRITE MISS + INVALID/COLD
    // ==================================================================
    {
        Harness h("F");
        uint64_t addr = 0x600000;
        uint64_t idx = h.dcm.returnIndexDC(addr);
        bool coldBefore = !h.dcm.tagMetadataStore[idx].validLine;

        Counts before = snapshot(h.dcm);
        doWrite(h, addr);
        Counts d = delta(snapshot(h.dcm), before);
        printCounts("[F: WRITE MISS + COLD]", d);

        bool metaAfter = h.dcm.tagMetadataStore[idx].validLine && h.dcm.tagMetadataStore[idx].dirtyLine &&
                          h.dcm.tagMetadataStore[idx].farMemAddr == addr;

        if (!coldBefore || !checkCounts(d, 1, 1, 0, 0) || !metaAfter || h.dcm.stats.numWrMissClean != 1 ||
            h.dcm.stats.numWrBacks != 0) {
            std::cerr << "  FAIL" << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Case G: WRITE MISS + CLEAN VICTIM
    // ==================================================================
    {
        Harness h("G");
        uint64_t addrA = 0x700000;
        uint64_t addrB = addrA + STRIDE;
        doRead(h, addrA); // install A, clean

        Counts before = snapshot(h.dcm);
        doWrite(h, addrB); // evicts A (clean) -> no write-back
        Counts d = delta(snapshot(h.dcm), before);
        printCounts("[G: WRITE MISS + CLEAN VICTIM]", d);

        uint64_t idx = h.dcm.returnIndexDC(addrA);
        bool metaAfter = h.dcm.tagMetadataStore[idx].validLine && h.dcm.tagMetadataStore[idx].dirtyLine &&
                          h.dcm.tagMetadataStore[idx].farMemAddr == addrB;

        if (!checkCounts(d, 1, 1, 0, 0) || !metaAfter || h.dcm.stats.numWrMissClean != 1 ||
            h.dcm.stats.numWrBacks != 0) {
            std::cerr << "  FAIL (clean victim must NOT trigger a write-back)" << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Case H: WRITE MISS + DIRTY VICTIM
    // ==================================================================
    {
        Harness h("H");
        uint64_t addrA = 0x800000;
        uint64_t addrB = addrA + STRIDE;
        doRead(h, addrA);  // install A, clean
        doWrite(h, addrA); // dirty A

        Counts before = snapshot(h.dcm);
        std::cout << "---- request-flow log: WRITE MISS + DIRTY VICTIM ----" << std::endl;
        h.dcm.debugPrint = true;
        doWrite(h, addrB); // evicts dirty A -> write-back required
        h.dcm.debugPrint = false;
        std::cout << "---- end request-flow log ----" << std::endl;
        Counts d = delta(snapshot(h.dcm), before);
        printCounts("[H: WRITE MISS + DIRTY VICTIM]", d);

        uint64_t idx = h.dcm.returnIndexDC(addrA);
        bool metaAfter = h.dcm.tagMetadataStore[idx].validLine && h.dcm.tagMetadataStore[idx].dirtyLine &&
                          h.dcm.tagMetadataStore[idx].farMemAddr == addrB;

        if (!checkCounts(d, 1, 1, 0, 1) || !metaAfter || h.dcm.stats.numWrMissDirty != 1 ||
            h.dcm.stats.numWrBacks != 1) {
            std::cerr << "  FAIL (dirty victim MUST trigger exactly one write-back; write-miss must NEVER "
                         "touch far-read)"
                      << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Extra: repeated access to the same line (hit stays a hit, N times)
    // ==================================================================
    {
        Harness h("REPEAT");
        uint64_t addr = 0x900000;
        doRead(h, addr); // install
        Counts before = snapshot(h.dcm);
        bool allOk = true;
        for (int i = 0; i < 5; i++) {
            if (!doRead(h, addr))
                allOk = false;
        }
        Counts d = delta(snapshot(h.dcm), before);
        printCounts("[REPEAT: 5x read hit]", d);
        if (!allOk || !checkCounts(d, 5, 0, 0, 0) || h.llc.responses.size() != 6) {
            std::cerr << "  FAIL (every repeated access to a resident line must be a pure local hit)"
                      << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Extra: replacement chain -- 4 addresses round-robin through one
    // index, each write-dirtied before being evicted by the next.
    // ==================================================================
    {
        Harness h("CHAIN");
        uint64_t base = 0xA00000;
        int N = 4;
        for (int i = 0; i < N; i++) {
            uint64_t addr = base + (uint64_t)i * STRIDE;
            doRead(h, addr);   // install (miss; evicts previous dirty resident for i>0)
            doWrite(h, addr);  // dirty it, so the NEXT installation must write it back
        }
        // stats.numWrBacks should be N-1: every install after the first
        // evicts a dirty predecessor (the very first install is cold, no
        // victim to write back).
        std::cout << "[CHAIN: 4 addresses round-robin one index] numWrBacks=" << h.dcm.stats.numWrBacks
                   << " numColdMisses=" << h.dcm.stats.numColdMisses
                   << " numHotMisses=" << h.dcm.stats.numHotMisses << std::endl;
        if (h.dcm.stats.numWrBacks != (uint64_t)(N - 1) || h.dcm.stats.numColdMisses != 1 ||
            h.dcm.stats.numHotMisses != (uint64_t)(N - 1)) {
            std::cerr << "  FAIL (expected exactly N-1 dirty write-backs in an N-deep replacement chain)"
                      << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    if (failures == 0) {
        std::cout << "ALL BASELINE TESTS PASSED" << std::endl;
        return 0;
    } else {
        std::cout << failures << " BASELINE TEST(S) FAILED" << std::endl;
        return 1;
    }
}
