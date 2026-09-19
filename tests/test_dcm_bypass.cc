// Deterministic tests for bypassDcache / the paper's "No-DRAM-Cache"
// comparison configuration, ported from gem5 PolicyManager's own
// `bypassDcache` SimObject param (verified directly from the pulled
// gem5 source, dram_cache_disaggregated branch, before writing any of
// this):
//
//   recvTimingReq (policy_manager.cc:157-162): the VERY FIRST check,
//     before ANY ORB/CRB/classification logic runs --
//       if (bypassDcache) { return farReqPort.sendTimingReq(pkt); }
//   farMemRecvTimingResp (policy_manager.cc:560-566):
//       if (bypassDcache) { port.schedTimingResp(pkt, curTick()); return true; }
//     i.e. the response is scheduled IMMEDIATELY (curTick(), zero added
//     ticks) -- gem5 does NOT apply controller frontend/backend latency
//     in bypass mode, because accessAndRespond() (which is what applies
//     that latency on the normal path) is never called here at all.
//   farMemRecvReqRetry (policy_manager.cc:644-647):
//       if (bypassDcache) { port.sendRetryReq(); return; }
//     (no ChampSim equivalent needed -- this port has no retry-on-NACK
//     concept on the far side, see docs/project_status.md item on WB
//     retry-on-nack, a SEPARATE unimplemented feature.)
//
// Ported behavior (see docs/bypass_mode.md for the full architectural
// writeup): DRAM_CACHE_MANAGER::bypassDcache (default false), toggled
// via setBypassDcache(). When true, add_rq()/add_wq() skip ALL of this
// manager's own bookkeeping -- no ORB entry, no CRB entry, no
// tag/metadata lookup, no cache fill, no dirty eviction, no BEAR/Oracle
// decision -- and forward straight to farMC via the EXISTING
// dispatchToFar() helper (the same one the normal far-fetch/write-back
// paths use), so the configured far-LINK latency (Case Study 3) still
// applies -- that latency models the physical wire between the manager
// and far memory, which is still crossed in bypass mode (gem5's bypass
// branch still goes out farReqPort, the same port the link sits on).
// Controller frontend/backend latency, by contrast, is skipped entirely
// in bypass mode, matching gem5's schedTimingResp(pkt, curTick()) above.
//
// Proves (numbered per the task's required minimum coverage):
//   1)  bypass read completes and reaches the LLC
//   2)  bypass write completes (reaches farMC; no LLC callback, matching
//       this port's write contract in EVERY mode)
//   3)  multiple reads all complete independently
//   4)  multiple writes all complete independently
//   5)  mixed read/write traffic
//   6)  repeated access to the same address (no DCM-side merging/caching
//       at all -- every access is a fresh trip to farMC)
//   7)  addresses that would map to the SAME DRAM-cache index in normal
//       mode do NOT conflict in bypass mode (no CRB, no ORB, no index
//       concept exists in bypass mode at all)
//   8)  dirty/write behavior: writing then reading the same address in
//       bypass mode never touches DCM dirty-eviction machinery
//   9)  response/completion correctness (right address, right cpu, right
//       count)
//   10) no DCM local (near-memory) operation occurs
//   11) no DCM metadata (tagMetadataStore) is modified
//   12) no ORB/CRB entry is ever created
//   13) no WB entry is ever created
//   14) far memory receives the request exactly as expected (RQ/WQ
//       ACCESS and ROW_BUFFER_HIT+MISS counts on farMC)
//
// Plus: a normal-vs-bypass comparison for the SAME workload, showing
// bypass mode produces zero DCM-side operations across the board; a
// timing check proving far-link latency composes with bypass mode while
// controller latency does NOT; and a statistics-isolation check proving
// bypass mode does not corrupt the existing Baseline/BEAR/Oracle
// counters when disabled.
//
// Build:
//   g++ -std=c++11 -Iinc -o /tmp/test_dcm_bypass \
//       tests/test_dcm_bypass.cc src/dram_cache_manager.cc \
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
    std::vector<uint64_t> readResponses;
    int add_rq(PACKET *packet) { return -1; }
    int add_wq(PACKET *packet) { return -1; }
    int add_pq(PACKET *packet) { return -1; }
    void operate() {}
    void increment_WQ_FULL(uint64_t address) {}
    uint32_t get_occupancy(uint8_t queue_type, uint64_t address) { return 0; }
    uint32_t get_size(uint8_t queue_type, uint64_t address) { return 1; }
    void return_data(PACKET *packet) { readResponses.push_back(packet->address); }
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

    Harness(const std::string &name)
        : near_mc(name + "_N"), far_mc(name + "_F"), dcm(name + "_DCM", &near_mc, &far_mc)
    {
        standardTiming(near_mc);
        standardTiming(far_mc);
        dcm.upper_level_icache[0] = &llc;
        dcm.upper_level_dcache[0] = &llc;
    }
};

int main()
{
    current_core_cycle[0] = 0;
    all_warmup_complete = NUM_CPUS;
    int failures = 0;
    const uint64_t STRIDE = DCM_DRAM_CACHE_SIZE; // two addresses this far apart share a DRAM-cache index

    // ==================================================================
    // Test 1: bypass read completes and reaches the LLC.
    // ==================================================================
    {
        Harness h("BP1");
        h.dcm.setBypassDcache(true);
        uint64_t addr = 0x9000000;
        PACKET p = makeReadPacket(addr);
        h.dcm.add_rq(&p);
        bool got = pumpUntil(h.dcm, [&]() { return !h.llc.readResponses.empty(); }, 2000000);

        std::cout << "[TEST 1: bypass read] completed=" << got
                   << " addr_correct=" << (got && h.llc.readResponses[0] == addr) << std::endl;
        if (!got || h.llc.readResponses[0] != addr) {
            std::cerr << "  FAIL" << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Test 2: bypass write completes (reaches farMC's WQ; no LLC
    // callback -- writes never produce one in this port, in any mode).
    // ==================================================================
    {
        Harness h("BP2");
        h.dcm.setBypassDcache(true);
        uint64_t addr = 0x9100000;
        PACKET p = makeWritePacket(addr);
        h.dcm.add_wq(&p);
        for (int i = 0; i < 500; i++) {
            current_core_cycle[0]++;
            h.dcm.operate();
        }
        uint64_t drained = h.far_mc.WQ[0].ROW_BUFFER_HIT + h.far_mc.WQ[0].ROW_BUFFER_MISS;
        bool noLlcResponse = h.llc.readResponses.empty();

        std::cout << "[TEST 2: bypass write] far_wq_drained=" << drained
                   << " no_llc_response=" << noLlcResponse << std::endl;
        if (drained != 1 || !noLlcResponse) {
            std::cerr << "  FAIL" << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Test 3: multiple reads all complete independently.
    // ==================================================================
    {
        Harness h("BP3");
        h.dcm.setBypassDcache(true);
        uint64_t addrs[4] = {0x9200000, 0x9200040, 0x9200080, 0x92000C0};
        for (int i = 0; i < 4; i++) {
            PACKET p = makeReadPacket(addrs[i]);
            h.dcm.add_rq(&p);
        }
        pumpUntil(h.dcm, [&]() { return h.llc.readResponses.size() >= 4; }, 2000000);

        std::cout << "[TEST 3: multiple reads] completed=" << h.llc.readResponses.size() << " expected=4"
                   << std::endl;
        if (h.llc.readResponses.size() != 4) {
            std::cerr << "  FAIL" << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Test 4: multiple writes all complete independently.
    // ==================================================================
    {
        Harness h("BP4");
        h.dcm.setBypassDcache(true);
        uint64_t addrs[4] = {0x9300000, 0x9300040, 0x9300080, 0x93000C0};
        for (int i = 0; i < 4; i++) {
            PACKET p = makeWritePacket(addrs[i]);
            h.dcm.add_wq(&p);
        }
        for (int i = 0; i < 2000; i++) {
            current_core_cycle[0]++;
            h.dcm.operate();
        }
        uint64_t drained = h.far_mc.WQ[0].ROW_BUFFER_HIT + h.far_mc.WQ[0].ROW_BUFFER_MISS;

        std::cout << "[TEST 4: multiple writes] far_wq_drained=" << drained << " expected=4" << std::endl;
        if (drained != 4) {
            std::cerr << "  FAIL" << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Test 5: mixed read/write traffic.
    // ==================================================================
    {
        Harness h("BP5");
        h.dcm.setBypassDcache(true);
        PACKET r1 = makeReadPacket(0x9400000);
        PACKET w1 = makeWritePacket(0x9400040);
        PACKET r2 = makeReadPacket(0x9400080);
        PACKET w2 = makeWritePacket(0x94000C0);
        h.dcm.add_rq(&r1);
        h.dcm.add_wq(&w1);
        h.dcm.add_rq(&r2);
        h.dcm.add_wq(&w2);
        for (int i = 0; i < 2000; i++) {
            current_core_cycle[0]++;
            h.dcm.operate();
        }
        uint64_t wDrained = h.far_mc.WQ[0].ROW_BUFFER_HIT + h.far_mc.WQ[0].ROW_BUFFER_MISS;

        std::cout << "[TEST 5: mixed read/write] reads=" << h.llc.readResponses.size()
                   << " writes_drained=" << wDrained << std::endl;
        if (h.llc.readResponses.size() != 2 || wDrained != 2) {
            std::cerr << "  FAIL" << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Test 6: repeated access to the same address -- every access is an
    // independent trip to farMC (no DCM-side caching/merging at all in
    // bypass mode).
    // ==================================================================
    {
        Harness h("BP6");
        h.dcm.setBypassDcache(true);
        uint64_t addr = 0x9500000;
        for (int i = 0; i < 3; i++) {
            PACKET p = makeReadPacket(addr);
            h.dcm.add_rq(&p);
            pumpUntil(h.dcm, [&]() { return h.llc.readResponses.size() >= (size_t)(i + 1); }, 2000000);
        }

        std::cout << "[TEST 6: repeated access] completed=" << h.llc.readResponses.size() << " expected=3"
                   << std::endl;
        if (h.llc.readResponses.size() != 3) {
            std::cerr << "  FAIL" << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Test 7: addresses that would map to the SAME DRAM-cache index in
    // normal mode do NOT conflict in bypass mode -- both admit and
    // complete immediately, with no CRB queueing at all (bypass mode has
    // no index concept to conflict on in the first place).
    // ==================================================================
    {
        Harness h("BP7");
        h.dcm.setBypassDcache(true);
        uint64_t addrA = 0x9600000, addrB = addrA + STRIDE; // same DRAM-cache index in normal mode
        PACKET pa = makeReadPacket(addrA);
        PACKET pb = makeReadPacket(addrB);
        int rcA = h.dcm.add_rq(&pa);
        int rcB = h.dcm.add_rq(&pb);
        pumpUntil(h.dcm, [&]() { return h.llc.readResponses.size() >= 2; }, 2000000);

        bool bothAdmitted = (rcA == -1) && (rcB == -1); // neither routed to CRB (would still be -1 here,
                                                         // so instead check CRB stayed empty directly)
        bool crbUntouched = h.dcm.CRB.empty();
        bool orbUntouched = h.dcm.ORB.empty();

        std::cout << "[TEST 7: same-index addresses] completed=" << h.llc.readResponses.size()
                   << " crb_empty=" << crbUntouched << " orb_empty=" << orbUntouched << std::endl;
        if (h.llc.readResponses.size() != 2 || !crbUntouched || !orbUntouched || !bothAdmitted) {
            std::cerr << "  FAIL (bypass mode must not apply DRAM-cache-index conflict detection at all)"
                      << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Test 8: dirty/write behavior -- write then read the same address
    // in bypass mode; must never touch DCM dirty-eviction machinery
    // (WB stays empty, numWrBacks stays 0) since there is no DRAM-cache
    // line to ever become dirty in the first place.
    // ==================================================================
    {
        Harness h("BP8");
        h.dcm.setBypassDcache(true);
        uint64_t addr = 0x9700000;
        PACKET w = makeWritePacket(addr);
        h.dcm.add_wq(&w);
        for (int i = 0; i < 500; i++) {
            current_core_cycle[0]++;
            h.dcm.operate();
        }
        PACKET r = makeReadPacket(addr);
        h.dcm.add_rq(&r);
        pumpUntil(h.dcm, [&]() { return !h.llc.readResponses.empty(); }, 2000000);

        bool wbEmpty = h.dcm.WB.empty();
        bool noWrBacks = h.dcm.stats.numWrBacks == 0;

        std::cout << "[TEST 8: dirty/write behavior] read_completed=" << !h.llc.readResponses.empty()
                   << " wb_empty=" << wbEmpty << " no_wrbacks=" << noWrBacks << std::endl;
        if (h.llc.readResponses.empty() || !wbEmpty || !noWrBacks) {
            std::cerr << "  FAIL" << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Test 9: response/completion correctness -- right address, right
    // cpu, right count, for a small batch of distinct reads.
    // ==================================================================
    {
        Harness h("BP9");
        h.dcm.setBypassDcache(true);
        uint64_t addrs[3] = {0x9800000, 0x9800100, 0x9800200};
        for (int i = 0; i < 3; i++) {
            PACKET p = makeReadPacket(addrs[i]);
            p.cpu = 0;
            h.dcm.add_rq(&p);
        }
        pumpUntil(h.dcm, [&]() { return h.llc.readResponses.size() >= 3; }, 2000000);

        bool allAddrsMatch = true;
        for (int i = 0; i < 3; i++) {
            bool found = false;
            for (size_t j = 0; j < h.llc.readResponses.size(); j++)
                if (h.llc.readResponses[j] == addrs[i])
                    found = true;
            allAddrsMatch = allAddrsMatch && found;
        }

        std::cout << "[TEST 9: response correctness] count=" << h.llc.readResponses.size()
                   << " all_addrs_match=" << allAddrsMatch << std::endl;
        if (h.llc.readResponses.size() != 3 || !allAddrsMatch) {
            std::cerr << "  FAIL" << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Test 10-13: no DCM local operation, no metadata modification, no
    // ORB/CRB entry, no WB entry -- checked together across a mixed
    // read/write/repeated-access workload run entirely in bypass mode.
    // ==================================================================
    {
        Harness h("BP10");
        h.dcm.setBypassDcache(true);

        // Snapshot tag metadata before -- every entry must remain exactly
        // as constructed (all-invalid, untouched) since bypass mode never
        // calls classifyAndInstall() at all.
        bool metadataUntouchedBefore = true;
        for (size_t i = 0; i < h.dcm.tagMetadataStore.size() && i < 1000; i++) {
            if (h.dcm.tagMetadataStore[i].validLine || h.dcm.tagMetadataStore[i].dirtyLine)
                metadataUntouchedBefore = false;
        }

        uint64_t addrA = 0x9900000, addrB = addrA + STRIDE; // same index in normal mode
        PACKET r1 = makeReadPacket(addrA);
        PACKET w1 = makeWritePacket(addrA);
        PACKET r2 = makeReadPacket(addrB);
        h.dcm.add_rq(&r1);
        h.dcm.add_wq(&w1);
        h.dcm.add_rq(&r2);
        PACKET r3 = makeReadPacket(addrA); // repeated access
        h.dcm.add_rq(&r3);

        for (int i = 0; i < 2000; i++) {
            current_core_cycle[0]++;
            h.dcm.operate();
        }

        bool metadataUntouchedAfter = true;
        for (size_t i = 0; i < h.dcm.tagMetadataStore.size() && i < 1000; i++) {
            if (h.dcm.tagMetadataStore[i].validLine || h.dcm.tagMetadataStore[i].dirtyLine)
                metadataUntouchedAfter = false;
        }

        bool noLocalOps = (h.dcm.stats.localReads == 0) && (h.dcm.stats.localWrites == 0) &&
                           (h.dcm.stats.sentToNear == 0) && (h.dcm.stats.completedFromNear == 0);
        bool noOrbCrb = h.dcm.ORB.empty() && h.dcm.CRB.empty();
        bool noWb = h.dcm.WB.empty() && (h.dcm.stats.numWrBacks == 0) && (h.dcm.stats.wbInsertions == 0);
        bool noClassification = (h.dcm.stats.numTotHits == 0) && (h.dcm.stats.numTotMisses == 0) &&
                                 (h.dcm.stats.cacheFills == 0);
        bool nearControllerIdle = (h.near_mc.RQ[0].ACCESS == 0);

        std::cout << "[TEST 10-13: no DCM-side operation at all] metadata_untouched=("
                   << metadataUntouchedBefore << "," << metadataUntouchedAfter << ") no_local_ops=" << noLocalOps
                   << " no_orb_crb=" << noOrbCrb << " no_wb=" << noWb << " no_classification=" << noClassification
                   << " near_controller_idle=" << nearControllerIdle << std::endl;
        if (!metadataUntouchedBefore || !metadataUntouchedAfter || !noLocalOps || !noOrbCrb || !noWb ||
            !noClassification || !nearControllerIdle) {
            std::cerr << "  FAIL (bypass mode must not touch ANY DCM-internal state -- ORB, CRB, WB, "
                         "tag metadata, or the near-memory controller)"
                      << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Test 14: far memory receives the request exactly as expected --
    // RQ ACCESS count for reads, WQ drain count for writes, on farMC
    // specifically (not nearMC, which must see nothing at all).
    // ==================================================================
    {
        Harness h("BP14");
        h.dcm.setBypassDcache(true);
        for (int i = 0; i < 5; i++) {
            PACKET p = makeReadPacket(0x9A00000 + (uint64_t)i * DCM_BLOCK_SIZE);
            h.dcm.add_rq(&p);
        }
        for (int i = 0; i < 3; i++) {
            PACKET p = makeWritePacket(0x9B00000 + (uint64_t)i * DCM_BLOCK_SIZE);
            h.dcm.add_wq(&p);
        }
        for (int i = 0; i < 5000; i++) {
            current_core_cycle[0]++;
            h.dcm.operate();
        }

        // NOTE: PACKET_QUEUE::ACCESS on the RQ side is only incremented by
        // MEMORY_CONTROLLER::add_rq's write-queue-forwarding shortcut
        // (dram_controller.cc:448, a pre-existing artifact
        // ran into before -- see test_dcm_controller_latency.cc's banner
        // comment), NOT on normal admission -- so it is not a valid
        // "was this request received" signal here. ROW_BUFFER_HIT+MISS,
        // incremented on every real completion for BOTH RQ and WQ
        // (dram_controller.cc's per-bank completion logic), is used
        // instead -- it is the same completion-count signal already used
        // for writes in Tests 2/4/5 above.
        uint64_t farRqCompleted = h.far_mc.RQ[0].ROW_BUFFER_HIT + h.far_mc.RQ[0].ROW_BUFFER_MISS;
        uint64_t farWqDrained = h.far_mc.WQ[0].ROW_BUFFER_HIT + h.far_mc.WQ[0].ROW_BUFFER_MISS;
        uint64_t nearRqCompleted = h.near_mc.RQ[0].ROW_BUFFER_HIT + h.near_mc.RQ[0].ROW_BUFFER_MISS;

        std::cout << "[TEST 14: far memory receives requests exactly] far_rq_completed=" << farRqCompleted
                   << " (expected 5) far_wq_drained=" << farWqDrained << " (expected 3) near_rq_completed="
                   << nearRqCompleted << " (expected 0)" << std::endl;
        if (farRqCompleted != 5 || farWqDrained != 3 || nearRqCompleted != 0) {
            std::cerr << "  FAIL" << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Normal vs. bypass comparison: SAME workload run once with
    // bypassDcache=false and once with bypassDcache=true. Bypass must
    // show zero DCM-cache-side operations across the board.
    // ==================================================================
    {
        Harness hNormal("CMP_NORMAL");
        Harness hBypass("CMP_BYPASS");
        hBypass.dcm.setBypassDcache(true);

        uint64_t addrA = 0x9C00000, addrB = addrA + STRIDE;
        for (Harness *h : {&hNormal, &hBypass}) {
            PACKET r1 = makeReadPacket(addrA);
            h->dcm.add_rq(&r1);
            for (int i = 0; i < 500; i++) {
                current_core_cycle[0]++;
                h->dcm.operate();
            }
            PACKET w1 = makeWritePacket(addrA);
            h->dcm.add_wq(&w1);
            for (int i = 0; i < 500; i++) {
                current_core_cycle[0]++;
                h->dcm.operate();
            }
            PACKET r2 = makeReadPacket(addrB); // evicts addrA's line in normal mode
            h->dcm.add_rq(&r2);
            for (int i = 0; i < 2000; i++) {
                current_core_cycle[0]++;
                h->dcm.operate();
            }
        }

        DCM_STATS &n = hNormal.dcm.stats;
        DCM_STATS &b = hBypass.dcm.stats;

        std::cout << "[COMPARISON: normal vs. bypass, same workload]" << std::endl;
        std::cout << "  NORMAL: localReads=" << n.localReads << " localWrites=" << n.localWrites
                   << " farReads=" << n.farReads << " farWrites=" << n.farWrites
                   << " cacheFills=" << n.cacheFills << " numWrBacks=" << n.numWrBacks
                   << " wbInsertions=" << n.wbInsertions << " orbInserts(approx via crbInserts)="
                   << n.crbInserts << " completedReadsToLLC=" << n.completedReadsToLLC << std::endl;
        std::cout << "  BYPASS: localReads=" << b.localReads << " localWrites=" << b.localWrites
                   << " farReads=" << b.farReads << " farWrites=" << b.farWrites
                   << " cacheFills=" << b.cacheFills << " numWrBacks=" << b.numWrBacks
                   << " wbInsertions=" << b.wbInsertions << " crbInserts=" << b.crbInserts
                   << " bypassReads=" << b.bypassReads << " bypassWrites=" << b.bypassWrites
                   << " bypassCompletedReads=" << b.bypassCompletedReads << std::endl;

        bool normalDidRealWork = (n.localReads > 0) && (n.cacheFills > 0);
        bool bypassDidNoDcmWork = (b.localReads == 0) && (b.localWrites == 0) && (b.farReads == 0) &&
                                   (b.farWrites == 0) && (b.cacheFills == 0) && (b.numWrBacks == 0) &&
                                   (b.wbInsertions == 0) && (b.crbInserts == 0);
        bool bypassStatsCorrect = (b.bypassReads == 2) && (b.bypassWrites == 1) && (b.bypassCompletedReads == 2);

        if (!normalDidRealWork || !bypassDidNoDcmWork || !bypassStatsCorrect) {
            std::cerr << "  FAIL (bypass mode must show zero DRAM-cache-side activity for the identical "
                         "workload that produces real activity in normal mode)"
                      << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Timing: far-link latency (Case Study 3) DOES compose with bypass
    // mode (same physical link is crossed); controller frontend/backend
    // latency does NOT apply in bypass mode at all (verified from gem5's
    // farMemRecvTimingResp bypass branch -- see file banner). Uses
    // elapsed-cycle deltas, never a clock reset, per this port's
    // established test-methodology lesson (see
    // tests/test_dcm_controller_latency.cc's banner comment for the two
    // real bugs that pattern was adopted to avoid).
    // ==================================================================
    {
        Harness hPlain("TMG_PLAIN");
        hPlain.dcm.setBypassDcache(true);
        hPlain.dcm.setControllerLatency(0, 0);
        uint64_t addr1 = 0x9D00000;
        uint64_t start1 = current_core_cycle[0];
        PACKET p1 = makeReadPacket(addr1);
        hPlain.dcm.add_rq(&p1);
        pumpUntil(hPlain.dcm, [&]() { return !hPlain.llc.readResponses.empty(); }, 2000000);
        uint64_t plainCycles = current_core_cycle[0] - start1;

        Harness hLink("TMG_LINK");
        hLink.dcm.setBypassDcache(true);
        hLink.dcm.setControllerLatency(0, 0);
        hLink.dcm.setLinkLatency(DCM_LINK_LATENCY_CYCLES_500NS);
        uint64_t addr2 = 0x9E00000;
        uint64_t start2 = current_core_cycle[0];
        PACKET p2 = makeReadPacket(addr2);
        hLink.dcm.add_rq(&p2);
        pumpUntil(hLink.dcm, [&]() { return !hLink.llc.readResponses.empty(); }, 2000000);
        uint64_t linkCycles = current_core_cycle[0] - start2;

        Harness hCtrl("TMG_CTRL");
        hCtrl.dcm.setBypassDcache(true);
        // default (nonzero) controller latency left in place deliberately
        uint64_t addr3 = 0x9F00000;
        uint64_t start3 = current_core_cycle[0];
        PACKET p3 = makeReadPacket(addr3);
        hCtrl.dcm.add_rq(&p3);
        pumpUntil(hCtrl.dcm, [&]() { return !hCtrl.llc.readResponses.empty(); }, 2000000);
        uint64_t ctrlCycles = current_core_cycle[0] - start3;

        uint64_t linkDelta = linkCycles - plainCycles;
        uint64_t ctrlDelta = ctrlCycles - plainCycles;

        // Small tolerance on the link-latency delta, matching the
        // established convention in tests/test_dcm_link_latency.cc
        // (+-20% band for DRAM-scheduling jitter around the configured
        // delay -- that file's own Test 1/2 do not require bit-exact
        // equality either). The controller-latency delta, by contrast,
        // is an exact 0-vs-nonzero claim ("does gem5 apply it here at
        // all"), not a scaling comparison, so it IS checked exactly.
        uint64_t linkTolerance = DCM_LINK_LATENCY_CYCLES_500NS / 5 + 5; // +-20% + slack
        bool linkOk = linkDelta + linkTolerance >= DCM_LINK_LATENCY_CYCLES_500NS &&
                      linkDelta <= DCM_LINK_LATENCY_CYCLES_500NS + linkTolerance;

        std::cout << "[TIMING: link latency composes, controller latency does not] plain=" << plainCycles
                   << " with_link=" << linkCycles << " link_delta=" << linkDelta
                   << " expected_link_delta=~" << DCM_LINK_LATENCY_CYCLES_500NS << " with_default_ctrl="
                   << ctrlCycles << " ctrl_delta=" << ctrlDelta << " expected_ctrl_delta=0" << std::endl;

        if (!linkOk || ctrlDelta != 0) {
            std::cerr << "  FAIL (bypass mode must still respect far-link latency -- the physical link is "
                         "still crossed -- but must NOT apply DCM controller frontend/backend latency, "
                         "verified absent from gem5's bypass response path)"
                      << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    // ==================================================================
    // Statistics isolation: enabling bypass mode on one instance must
    // never corrupt another (normal-mode) instance's Baseline/BEAR/Oracle
    // statistics -- and a normal-mode instance that never enables bypass
    // must show bypassReads/bypassWrites/bypassCompletedReads == 0.
    // ==================================================================
    {
        Harness hNormal("STAT_NORMAL");
        uint64_t addr = 0xA000000;
        PACKET p = makeReadPacket(addr);
        hNormal.dcm.add_rq(&p);
        pumpUntil(hNormal.dcm, [&]() { return !hNormal.llc.readResponses.empty(); }, 2000000);

        bool bypassStatsZero = (hNormal.dcm.stats.bypassReads == 0) && (hNormal.dcm.stats.bypassWrites == 0) &&
                                (hNormal.dcm.stats.bypassCompletedReads == 0);
        bool normalStatsIntact = (hNormal.dcm.stats.localReads > 0) && (hNormal.dcm.stats.cacheFills > 0) &&
                                  (hNormal.dcm.stats.completedReadsToLLC == 1);

        std::cout << "[STATS: bypass disabled by default, does not corrupt existing counters] "
                      "bypass_stats_zero="
                   << bypassStatsZero << " normal_stats_intact=" << normalStatsIntact << std::endl;
        if (!bypassStatsZero || !normalStatsIntact) {
            std::cerr << "  FAIL" << std::endl;
            failures++;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    if (failures == 0) {
        std::cout << "ALL BYPASS TESTS PASSED" << std::endl;
        return 0;
    } else {
        std::cout << failures << " BYPASS TEST(S) FAILED" << std::endl;
        return 1;
    }
}
