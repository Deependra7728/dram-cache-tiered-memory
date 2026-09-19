#include "dram_cache_manager.h"

#include <iostream>

// ============================================================================
// DRAM_CACHE_MANAGER
//
// Complete request path for all three paper policies
// (CascadeLakeNoPartWrs / BEAR-Wr-Opt / RambusHypo-Oracle): real
// direct-mapped tag/metadata lookup, hit/miss/dirty classification,
// victim identification, dirty write-back, cache-line installation.
// The policies share this entire path and differ only in
// chooseInitialState(), which decides whether a request may skip the
// local tag-check read; all three are Table-II-verified (24/24 cells
// exact). See docs/gem5_to_champsim_mapping.md for the behavioral
// facts this is ported from and docs/feature_coverage.md for what is
// and isn't implemented.
// ============================================================================

// NOTE on a real precision bug found and fixed here: ChampSim's original
// DRAM_DBUS_RETURN_TIME formula (main.cc, pre-existing, and this file's
// own earlier version) computes
//   (BLOCK_SIZE / DRAM_CHANNEL_WIDTH) * (CPU_FREQ / DRAM_MTPS)
// using pure integer division for BOTH terms. At CPU_FREQ=4000 and MTPS
// values in the low thousands (3200 default, or 4000/2400 for
// HBM2/DDR4), `CPU_FREQ / DRAM_MTPS` truncates to 1 for EVERY MTPS in
// [2001, 4000] -- e.g. 4000/2400 (real 1.667) and 4000/4000 (real 1.0)
// BOTH truncate to integer 1, producing the SAME dbus=8 for near and far
// despite a real, intended 32 GB/s vs 19.2 GB/s difference. This was
// silently present in ChampSim's default single-DRAM configuration too
// (3200 MT/s -> truncates to dbus=8 instead of the mathematically
// correct 10), just never visible before because there was only one
// DRAM_MTPS value in the whole simulator to compare against nothing.
// Fixed here with proper floating-point computation + rounding, scoped
// to these two functions only (not touching set_timing()'s general
// contract or main.cc's --low_bandwidth adjustment, to avoid scope creep
// on unrelated pre-existing call sites -- see docs/limitations.md).
uint32_t computeDbusReturnTime(uint32_t mtps)
{
    double transfersPerBlock = (double)BLOCK_SIZE / (double)DRAM_CHANNEL_WIDTH;
    double cyclesPerTransfer = (double)CPU_FREQ / (double)mtps;
    return (uint32_t)(transfersPerBlock * cyclesPerTransfer + 0.5); // round to nearest
}

double dcmBandwidthUtilization(uint64_t opCount, uint32_t mtps, uint64_t elapsedCycles)
{
    // Degenerate inputs return 0.0 rather than dividing by zero. The
    // caller decides how to display "no data" -- see
    // docs/final_independent_audit.md HIGH-1 for the underflow this
    // replaces (the old expression could wrap to ~1.8e19 cycles and
    // silently report ~1e-16% utilization instead of failing loudly).
    if (elapsedCycles == 0 || mtps == 0)
        return 0.0;
    double peakBytesPerCycle = (double)(DRAM_CHANNEL_WIDTH * mtps) / (double)CPU_FREQ;
    double bytesMoved = (double)opCount * (double)BLOCK_SIZE;
    return bytesMoved / ((double)elapsedCycles * peakBytesPerCycle);
}

void configureNearAsHBM2(MEMORY_CONTROLLER &mc)
{
    uint32_t trp = (uint32_t)((1.0 * tRP_DRAM_NANOSECONDS * CPU_FREQ) / 1000);
    uint32_t trcd = (uint32_t)((1.0 * tRCD_DRAM_NANOSECONDS * CPU_FREQ) / 1000);
    uint32_t tcas = (uint32_t)((1.0 * tCAS_DRAM_NANOSECONDS * CPU_FREQ) / 1000);
    uint32_t mtps = DCM_NEAR_HBM2_MTPS;
    uint32_t dbus = computeDbusReturnTime(mtps);
    mc.set_timing(trp, trcd, tcas, mtps, dbus);
}

void configureFarAsDDR4(MEMORY_CONTROLLER &mc)
{
    uint32_t trp = (uint32_t)((1.0 * tRP_DRAM_NANOSECONDS * CPU_FREQ) / 1000);
    uint32_t trcd = (uint32_t)((1.0 * tRCD_DRAM_NANOSECONDS * CPU_FREQ) / 1000);
    uint32_t tcas = (uint32_t)((1.0 * tCAS_DRAM_NANOSECONDS * CPU_FREQ) / 1000);
    uint32_t mtps = DCM_FAR_DDR4_MTPS;
    uint32_t dbus = computeDbusReturnTime(mtps);
    mc.set_timing(trp, trcd, tcas, mtps, dbus);
}

DRAM_CACHE_MANAGER::DRAM_CACHE_MANAGER(string v1, MEMORY_CONTROLLER *near, MEMORY_CONTROLLER *far)
    : NAME(v1), nearMC(near), farMC(far), policy(DCM_POLICY_BASELINE_CASCADE_LAKE), linkLatencyCycles(0),
      frontendLatencyCycles(DCM_FRONTEND_LATENCY_CYCLES), backendLatencyCycles(DCM_BACKEND_LATENCY_CYCLES),
      bypassDcache(false), debugForceHit(false), debugForceDirty(false), debugPrint(false), nextRequestId(1)
{
    fill_level = FILL_DRAM;

    // One source of truth for capacity: the metadata store has exactly as
    // many entries as the direct-mapped index has values, so
    // DCM_NUM_LINES * DCM_BLOCK_SIZE == DCM_DRAM_CACHE_SIZE == 128 MiB.
    tagMetadataStore.resize(DCM_NUM_LINES);

    // Wire the near/far controllers' completion callbacks back to this
    // manager instead of directly to the LLC. This is the one place where
    // gem5's two-port structure (locReqPort/farReqPort, each with its own
    // recvTimingResp) is collapsed into ChampSim's single return_data()
    // entry point -- see "Deliberate adaptations" in
    // docs/gem5_to_champsim_mapping.md.
    for (uint32_t i = 0; i < NUM_CPUS; i++) {
        nearMC->upper_level_icache[i] = this;
        nearMC->upper_level_dcache[i] = this;
        farMC->upper_level_icache[i] = this;
        farMC->upper_level_dcache[i] = this;
    }

    for (uint32_t c = 0; c < DRAM_CHANNELS; c++) {
        nearMC->RQ[c].is_RQ = 1;
        nearMC->WQ[c].is_WQ = 1;
        farMC->RQ[c].is_RQ = 1;
        farMC->WQ[c].is_WQ = 1;
    }
}

DRAM_CACHE_MANAGER::~DRAM_CACHE_MANAGER()
{
    for (std::map<uint64_t, DCM_ORB_ENTRY *>::iterator it = ORB.begin(); it != ORB.end(); ++it) {
        delete it->second;
    }
    ORB.clear();
}

void DRAM_CACHE_MANAGER::resetROIStats()
{
    // Preserve the warmup-phase evidence counter; clear every ROI
    // counter. Architectural state (tagMetadataStore, ORB, CRB, WB,
    // pendingNear/FarDispatches, pendingResponses) is deliberately NOT
    // touched -- the DRAM cache must stay warmed across this boundary.
    uint64_t keepWarmupTagUpdates = stats.warmupTagUpdates;
    stats = DCM_STATS();
    stats.warmupTagUpdates = keepWarmupTagUpdates;
}

void DRAM_CACHE_MANAGER::warmupTagUpdate(PACKET *packet, bool isWrite)
{
    uint64_t indexDC = returnIndexDC(packet->address);
    uint64_t tagDC = returnTagDC(packet->address);
    DCM_TAG_ENTRY &slot = tagMetadataStore[indexDC];

    bool isHit = slot.validLine && (slot.tagDC == tagDC);

    // Identical update rules to classifyAndInstall()'s "Updating Tag &
    // Metadata" block (gem5 handleRequestorPkt, policy_manager.cc:
    // 1426-1443) -- kept in lockstep deliberately so a line warmed here
    // is indistinguishable from one installed during the ROI.
    slot.tagDC = tagDC;
    slot.indexDC = indexDC;
    slot.validLine = true;
    if (!isWrite)
        slot.dirtyLine = isHit ? slot.dirtyLine : false;
    else
        slot.dirtyLine = true;
    slot.farMemAddr = packet->address;

    stats.warmupTagUpdates++;
}


// ============================================================================
// ChampSim PACKET::address convention and DCM normalization
//
// Established by direct source inspection of every layer (full write-up in
// docs/gem5_to_champsim_mapping.md, "ChampSim PACKET::address convention
// and DCM normalization"):
//
//   PACKET::full_addr = physical BYTE address
//   PACKET::address   = physical CACHE-LINE (block) address
//                     = full_addr >> LOG2_BLOCK_SIZE
//
// Evidence, layer by layer:
//   * CPU:    src/ooo_cpu.cc:1696,2253 -- `data_packet.address =
//             physical_address >> LOG2_BLOCK_SIZE;`
//             `data_packet.full_addr = physical_address;`
//   * CACHE:  src/cache.cc:1058 -- get_set() masks `address` DIRECTLY with
//             (NUM_SET-1), and get_way() compares the whole `address` as
//             the tag. Both only make sense if `address` is already a line
//             address.
//   * CACHE -> lower_level: the packet is forwarded UNMODIFIED
//             (src/cache.cc:661,672 add_rq; :115,445 add_wq), so whatever
//             the LLC held is exactly what the DCM receives.
//   * MEMORY_CONTROLLER: dram_get_channel() uses `shift = 0`
//             (src/dram_controller.cc:634) -- again a line address, with no
//             byte-offset bits to discard.
//
// The convention is uniform across the whole hierarchy, and this DCM was
// the ONLY component that disagreed with it: it divided the incoming
// address by DCM_BLOCK_SIZE a SECOND time, collapsing 64 consecutive
// distinct 64-byte lines onto one index (effective line size 4096 B,
// effective capacity 8 GB). Normalization is therefore applied HERE, at
// the DCM interface only -- PACKET::address keeps its global meaning
// untouched, because source inspection proves that meaning is already
// correct and consistent everywhere else.
//
//   incoming PACKET::address  (cache-line address, normalized upstream)
//     -> lineAddr = address                    (no shift, no divide)
//     -> indexDC  = lineAddr % DCM_NUM_LINES   (direct-mapped)
//     -> tagDC    = lineAddr / DCM_NUM_LINES
//
// DCM_NUM_LINES = DCM_DRAM_CACHE_SIZE / DCM_BLOCK_SIZE
//               = 128 MiB / 64 B = 2,097,152 lines,
// which is exactly tagMetadataStore's size, so the modelled cache is
// 2,097,152 x 64 B = 128 MiB with 64 B lines -- the paper's Table I
// configuration.
// ============================================================================
uint64_t DRAM_CACHE_MANAGER::returnIndexDC(uint64_t address)
{
    // `address` IS the cache-line address -- see the contract above.
    return address % DCM_NUM_LINES; // direct-mapped (paper baseline)
}

uint64_t DRAM_CACHE_MANAGER::returnTagDC(uint64_t address)
{
    return address / DCM_NUM_LINES;
}

// ----------------------------------------------------------------------------
// MEMORY interface
// ----------------------------------------------------------------------------

int DRAM_CACHE_MANAGER::add_rq(PACKET *packet)
{
    // Preserve ChampSim's existing pre-warmup bypass behavior exactly as
    // MEMORY_CONTROLLER::add_rq does it (dram_controller.cc:419-426), so
    // that inserting this manager does not change what happens during
    // ChampSim's warmup phase.
    if (all_warmup_complete < NUM_CPUS) {
        // Warm the DRAM cache's tag/metadata state so the ROI does not
        // begin against a completely cold cache (paper Section V's
        // methodology; docs/final_independent_audit.md HIGH-2). Only
        // metadata is updated -- no ORB/CRB/WB entry, no DRAM timing,
        // no ROI statistic -- which preserves ChampSim's existing
        // warmup contract of returning memory data immediately.
        // Bypass mode has no DRAM cache in the path, so nothing to warm.
        if (!bypassDcache)
            warmupTagUpdate(packet, /*isWrite=*/false);
        if (packet->instruction)
            upper_level_icache[packet->cpu]->return_data(packet);
        if (packet->is_data)
            upper_level_dcache[packet->cpu]->return_data(packet);
        return -1;
    }

    // bypassDcache: gem5's FIRST check in recvTimingReq
    // (policy_manager.cc:160-162), before anything else -- forward
    // straight to farMC via dispatchToFar() (reusing the far-link-latency
    // mechanism, since that models the physical link, not this
    // manager's own bookkeeping -- see docs/bypass_mode.md), with NO ORB
    // entry, NO CRB check, NO tag/metadata lookup, NO classification at
    // all. Tracked in bypassOutstandingReads so return_data() can route
    // the eventual response straight to the LLC.
    if (bypassDcache) {
        stats.bypassReads++;
        bypassOutstandingReads.insert(packet->address);
        // dispatchToFar()'s callers on the normal path (driveState's
        // DCM_FAR_MEM_READ/DCM_LOC_MEM_WRITE cases) always stamp
        // event_cycle to "now" immediately before dispatch -- required
        // because PACKET's own default constructor leaves it at
        // UINT64_MAX, which MEMORY_CONTROLLER's scheduling never picks
        // up. admitRequest() is what would normally do the equivalent
        // stamping for a DCM-admitted request, but bypass mode skips
        // admitRequest() entirely, so it must be done here instead.
        packet->event_cycle = current_core_cycle[packet->cpu];
        // Never silently drop a bypass read even if farMC is full right
        // now -- dispatchToFarGuaranteed() retries via pendingFarDispatches
        // until accepted (docs/memory_dispatch_audit.md), instead of the
        // old bare dispatchToFar() call whose false return went unchecked.
        dispatchToFarGuaranteed(*packet, /*isWrite=*/false);
        return -1;
    }

    // Admission order mirrors gem5 recvTimingReq exactly
    // (policy_manager.cc:296-362, fact #3 in
    // docs/gem5_to_champsim_mapping.md): conflict-by-index check (-> CRB,
    // retry if CRB full) -> WB-occupancy-pressure check (retry) ->
    // ORB-full check (retry) -> admit.
    uint64_t indexDC = returnIndexDC(packet->address);
    if (checkConflictInORB(indexDC)) {
        if (CRB.size() >= DCM_CRB_MAX_SIZE) {
            stats.crbFullRejects++;
            return -2;
        }
        DCM_CRB_ENTRY c;
        c.arrivalCycle = current_core_cycle[packet->cpu];
        c.pkt = *packet;
        c.isWriteReq = false;
        CRB.push_back(c);
        stats.crbInserts++;
        if (debugPrint)
            std::cout << "[DCM] CRB insert (conflict) READ addr=0x" << std::hex << packet->address
                       << std::dec << " index=" << indexDC << " crb_size=" << CRB.size() << std::endl;
        return -1;
    }

    if (WB.size() >= DCM_WB_PRESSURE_THRESHOLD) {
        // gem5: `if (pktFarMemWrite.size() >= (orbMaxSize / 2)) { ...retry... }`
        // -- applies to EVERY new admission, not just ones that would
        // themselves evict a dirty line (policy_manager.cc:332-345).
        stats.wbFullRejects++;
        if (debugPrint)
            std::cout << "[DCM] WBfull: addr=0x" << std::hex << packet->address << std::dec
                       << " wb_size=" << WB.size() << std::endl;
        return -2;
    }

    if (ORB.size() >= DCM_ORB_MAX_SIZE) {
        // NOTE: the LLC never actually inspects add_rq's return value on
        // this path -- it pre-checks get_occupancy()==get_size() before
        // calling add_rq at all (cache.cc:652). This return is defensive
        // documentation of intent, not a relied-upon contract.
        stats.orbFullRejects++;
        return -2;
    }

    admitRequest(packet, /*isWrite=*/false);
    return -1;
}

int DRAM_CACHE_MANAGER::add_wq(PACKET *packet)
{
    // MEMORY_CONTROLLER::add_wq silently drops writes pre-warmup
    // (dram_controller.cc:495-496); mirrored here for the same reason.
    // The write's tag/metadata effect is still applied, so a line
    // written during warmup is correctly resident AND dirty when the
    // ROI begins (see add_rq's warmup branch for the full rationale).
    if (all_warmup_complete < NUM_CPUS) {
        if (!bypassDcache)
            warmupTagUpdate(packet, /*isWrite=*/true);
        return -1;
    }

    // bypassDcache: same forwarding as add_rq's bypass branch above, for
    // writes. No tracking entry needed on the write side: this port's
    // write contract never produces an LLC-visible callback for a write
    // in ANY mode (gem5_to_champsim_mapping.md fact #2), bypass included
    // -- MEMORY_CONTROLLER never calls return_data() for a WQ completion
    // (dram_controller.cc), so there is nothing to route back.
    if (bypassDcache) {
        stats.bypassWrites++;
        packet->event_cycle = current_core_cycle[packet->cpu]; // see add_rq's bypass branch for why
        // Never silently drop a bypass write even if farMC is full right
        // now -- same guaranteed-delivery mechanism as the read side
        // above (docs/memory_dispatch_audit.md). This closes the gap
        // documented in docs/bypass_mode.md's Limitations section (a
        // bypass write hitting a momentarily-full farMC used to be
        // silently dropped even after the WB-path fix, since bypass mode
        // has no WB-like holding structure of its own -- it now reuses
        // the same pendingFarDispatches retry queue instead).
        dispatchToFarGuaranteed(*packet, /*isWrite=*/true);
        return -1;
    }

    uint64_t indexDC = returnIndexDC(packet->address);
    if (checkConflictInORB(indexDC)) {
        if (CRB.size() >= DCM_CRB_MAX_SIZE) {
            stats.crbFullRejects++;
            return -2;
        }
        DCM_CRB_ENTRY c;
        c.arrivalCycle = current_core_cycle[packet->cpu];
        c.pkt = *packet;
        c.isWriteReq = true;
        CRB.push_back(c);
        stats.crbInserts++;
        if (debugPrint)
            std::cout << "[DCM] CRB insert (conflict) WRITE addr=0x" << std::hex << packet->address
                       << std::dec << " index=" << indexDC << " crb_size=" << CRB.size() << std::endl;
        return -1;
    }

    if (WB.size() >= DCM_WB_PRESSURE_THRESHOLD) {
        stats.wbFullRejects++;
        if (debugPrint)
            std::cout << "[DCM] WBfull: addr=0x" << std::hex << packet->address << std::dec
                       << " wb_size=" << WB.size() << std::endl;
        return -2;
    }

    if (ORB.size() >= DCM_ORB_MAX_SIZE) {
        stats.orbFullRejects++;
        return -2;
    }

    admitRequest(packet, /*isWrite=*/true);
    return -1;
}

int DRAM_CACHE_MANAGER::add_pq(PACKET *packet)
{
    // Mirrors MEMORY_CONTROLLER::add_pq, which is already a no-op in this
    // ChampSim tree (dram_controller.cc:534-537) -- prefetch traffic to the
    // lower level is dropped today regardless of this port. Not something
    // this port changes; see docs/limitations.md.
    return -1;
}

void DRAM_CACHE_MANAGER::increment_WQ_FULL(uint64_t address)
{
    stats.wqFullSignals++;
}

uint32_t DRAM_CACHE_MANAGER::get_occupancy(uint8_t queue_type, uint64_t address)
{
    // Types 1 (RQ) and 2 (WQ) both map onto the single shared ORB, matching
    // gem5's single ORB governing both reads and writes. The caller
    // (CACHE::handle_read/handle_writeback) checks
    // get_occupancy(type,addr) == get_size(type,addr) for THIS address
    // before calling add_rq/add_wq, so this must report the same
    // conflict-vs-ORB routing decision add_rq/add_wq will actually make
    // for that address, not just raw ORB size.
    // bypassDcache: none of this manager's own ORB/CRB/WB capacity
    // applies at all in bypass mode -- a bypass request is forwarded
    // straight to farMC, so flow control must be delegated to farMC's
    // OWN queue occupancy, exactly the way the request itself is
    // delegated (reusing the existing far MEMORY_CONTROLLER, not
    // inventing a parallel capacity model for bypass mode).
    if (bypassDcache)
        return farMC->get_occupancy(queue_type, address);

    if (queue_type == 1 || queue_type == 2) {
        uint64_t indexDC = returnIndexDC(address);
        if (checkConflictInORB(indexDC))
            return (uint32_t)CRB.size(); // this address would queue in the CRB
        if (WB.size() >= DCM_WB_PRESSURE_THRESHOLD)
            return DCM_WB_PRESSURE_THRESHOLD; // WB-pressure would block ANY address right now (report full)
        return (uint32_t)ORB.size();          // this address would go straight into the ORB
    }
    return 0; // type 3 (PQ): not modeled, see add_pq()
}

uint32_t DRAM_CACHE_MANAGER::get_size(uint8_t queue_type, uint64_t address)
{
    if (bypassDcache)
        return farMC->get_size(queue_type, address);

    if (queue_type == 1 || queue_type == 2) {
        uint64_t indexDC = returnIndexDC(address);
        if (checkConflictInORB(indexDC))
            return DCM_CRB_MAX_SIZE;
        if (WB.size() >= DCM_WB_PRESSURE_THRESHOLD)
            return DCM_WB_PRESSURE_THRESHOLD;
        return DCM_ORB_MAX_SIZE;
    }
    return 1; // type 3 (PQ): report always-available so PQ admission never blocks here
}

void DRAM_CACHE_MANAGER::operate()
{
    // Order: release anything whose link-latency hold has elapsed and
    // drain one WB entry BEFORE stepping the controllers, so a
    // just-dispatched packet gets a chance to be scheduled by farMC this
    // same cycle rather than one cycle late. processPendingNearDispatches()
    // gets the same treatment for nearMC, right before nearMC->operate()
    // (docs/memory_dispatch_audit.md).
    processPendingFarDispatches();
    drainWB();
    processPendingNearDispatches();

    nearMC->operate();
    farMC->operate();

    // Controller-latency-delayed responses are released independently of
    // the near/far controllers stepping -- they are pure bookkeeping
    // delays on packets already fully resolved (ORB already retired),
    // not something waiting on any device. Order relative to the two
    // operate() calls above does not matter for correctness; placed here
    // for readability (grouped with the other per-cycle release/drain
    // steps).
    processPendingResponses();

    recordOccupancySamples();
}

// ----------------------------------------------------------------------------
// Baseline (CascadeLakeNoPartWrs) request handling
//
// Both reads and writes are admitted the same way and both start with a
// local tag-check read (DCM_LOC_MEM_READ), matching the real baseline
// policy's `start -> locMemRead` step for both read and write
// (policy_manager.cc:686-711) -- this tag-check is NEVER skipped in the
// baseline (that optimization is what distinguishes BEAR-Wr-Opt/Oracle,
// neither of which is implemented here). Real hit/miss/dirty/victim
// classification happens once, eagerly, at admission time
// (classifyAndInstall(), ported from gem5 handleRequestorPkt's inline
// logic) -- BEFORE the tag-check read is even dispatched, matching gem5's
// ordering exactly (the logical tag store is updated immediately,
// decoupled from the physical DRAM timing simulated afterward).
//
// What happens after the tag-check read completes (return_data(),
// DCM_WAITING_LOC_MEM_READ_RESP branch):
//
//   1. If a dirty victim was identified at admission (e->handleDirtyLine),
//      push its write-back now -- this is gem5's exact trigger point
//      (policy_manager.cc:521-538): the tag-check read is what "sources"
//      the victim's data in the real hardware being modeled (tag+data
//      co-located in ECC bits), so the write-back is queued right when
//      that read completes, before anything else happens.
//   2. WRITE: goes straight to a local write and completes with no
//      callback to the LLC, matching both the real baseline's structure
//      (tag-check then local write, unconditional on hit/miss) and
//      ChampSim's own pre-existing no-callback contract for writes (see
//      gem5_to_champsim_mapping.md, fact #2).
//   3. READ HIT: completes immediately (pure local path, no far touch).
//   4. READ MISS: goes to far memory and, on far completion, responds to
//      the LLC immediately before issuing a background local fill write,
//      matching the "respond-before-fill" behavior recorded in
//      gem5_to_champsim_mapping.md fact #1.
// ----------------------------------------------------------------------------

bool DRAM_CACHE_MANAGER::checkConflictInORB(uint64_t indexDC)
{
    for (std::map<uint64_t, DCM_ORB_ENTRY *>::iterator it = ORB.begin(); it != ORB.end(); ++it) {
        if (it->second->validEntry && it->second->indexDC == indexDC)
            return true;
    }
    return false;
}

void DRAM_CACHE_MANAGER::promoteFromCRB(uint64_t indexDC)
{
    for (std::vector<DCM_CRB_ENTRY>::iterator it = CRB.begin(); it != CRB.end(); ++it) {
        if (returnIndexDC(it->pkt.address) == indexDC) {
            PACKET pkt = it->pkt;
            bool isWrite = it->isWriteReq;
            uint64_t originalArrival = it->arrivalCycle;
            CRB.erase(it);

            stats.crbPromotions++;
            if (debugPrint)
                std::cout << "[DCM] CRB promote addr=0x" << std::hex << pkt.address << std::dec
                           << " index=" << indexDC << " waited_cycles="
                           << (current_core_cycle[pkt.cpu] - originalArrival) << " crb_size=" << CRB.size()
                           << std::endl;

            admitRequest(&pkt, isWrite, originalArrival);

            // Cosmetic/bookkeeping parity with gem5's resumeConflictingReq,
            // which calls checkConflictInCRB() right after promotion to
            // flag whether more CRB entries are still queued behind this
            // one for the same index (policy_manager.cc:1658, 1698-1710).
            // Not read by any dispatch logic in gem5's baseline either --
            // informational only.
            std::map<uint64_t, DCM_ORB_ENTRY *>::iterator promoted = ORB.find(pkt.address);
            if (promoted != ORB.end()) {
                for (std::vector<DCM_CRB_ENTRY>::iterator rest = CRB.begin(); rest != CRB.end(); ++rest) {
                    if (returnIndexDC(rest->pkt.address) == indexDC) {
                        promoted->second->conflict = true;
                        break;
                    }
                }
            }
            break; // only one promotion per freed index; others (if any) stay queued
        }
    }
}

void DRAM_CACHE_MANAGER::classifyAndInstall(DCM_ORB_ENTRY *e)
{
    DCM_TAG_ENTRY &slot = tagMetadataStore[e->indexDC];

    // Snapshot the OLD resident line (the potential victim) BEFORE any
    // mutation -- gem5 reads tagMetadataStore.at(indexDC) inline for
    // exactly this purpose in handleRequestorPkt.
    bool oldValid = slot.validLine;
    bool oldDirty = slot.dirtyLine;
    uint64_t oldFarAddr = slot.farMemAddr;

    e->victimWasValid = oldValid;
    e->victimWasDirty = oldDirty;
    e->victimFarAddr = oldFarAddr;

    // checkHitOrMiss (policy_manager.cc:1463-1528).
    e->isHit = oldValid && (e->tagDC == slot.tagDC);

    if (e->isHit) {
        stats.numTotHits++;
        if (e->isWriteReq)
            stats.numWrHit++;
        else
            stats.numRdHit++;
    } else {
        stats.numTotMisses++;
        // "Insert-on-miss": every miss results in a cache-line install
        // (see cacheFills). This is policy-INDEPENDENT -- BEAR-Wr-Opt and
        // Oracle skip the tag-check READ for some cases but still install
        // on every miss, so no per-policy condition is needed here.
        // Verified: CACHE_FILLS == MISSES under all three policies.
        stats.cacheFills++;
        if (oldValid)
            stats.numHotMisses++;
        else
            stats.numColdMisses++;

        if (e->isWriteReq) {
            if (oldValid && oldDirty)
                stats.numWrMissDirty++;
            else
                stats.numWrMissClean++;
        } else {
            if (oldValid && oldDirty)
                stats.numRdMissDirty++;
            else
                stats.numRdMissClean++;
        }
    }

    // checkDirty(addr) && !isHit (policy_manager.cc:1421-1424): a hit
    // never evicts anything, so the victim only needs a write-back on a
    // miss where the old line was valid and dirty.
    if (oldValid && oldDirty && !e->isHit) {
        e->dirtyLineAddr = oldFarAddr;
        e->handleDirtyLine = true;
    }

    // "Updating Tag & Metadata" (policy_manager.cc:1426-1443) -- eager
    // install, decoupled from the physical DRAM timing simulated later.
    slot.tagDC = e->tagDC;
    slot.indexDC = e->indexDC;
    slot.validLine = true;
    if (!e->isWriteReq) {
        // read: a hit leaves the dirty bit unchanged; a miss installs a
        // freshly-fetched, clean line.
        slot.dirtyLine = e->isHit ? slot.dirtyLine : false;
    } else {
        // a write always dirties the line, whether it was a hit or a miss.
        slot.dirtyLine = true;
    }
    slot.farMemAddr = e->pkt.address;

    if (debugPrint) {
        std::cout << "[DCM] req#" << e->requestId << " classify " << (e->isHit ? "HIT" : "MISS")
                   << " victim_valid=" << e->victimWasValid << " victim_dirty=" << e->victimWasDirty
                   << " victim_addr=0x" << std::hex << e->victimFarAddr << std::dec
                   << " needs_writeback=" << e->handleDirtyLine << std::endl;
    }
}

void DRAM_CACHE_MANAGER::pushDirtyWriteBack(DCM_ORB_ENTRY *e)
{
    PACKET wb;
    wb.type = WRITEBACK;
    wb.address = e->dirtyLineAddr;
    wb.full_addr = e->dirtyLineAddr;
    wb.cpu = e->pkt.cpu;
    wb.instruction = 0;
    wb.is_data = 1;
    wb.event_cycle = current_core_cycle[e->pkt.cpu];

    DCM_WB_ENTRY w;
    w.arrivalCycle = current_core_cycle[e->pkt.cpu];
    w.pkt = wb;
    WB.push_back(w);
    stats.wbInsertions++;
    if (WB.size() > stats.wbMaxOccupancy)
        stats.wbMaxOccupancy = WB.size();

    if (debugPrint)
        std::cout << "[DCM] req#" << e->requestId << " WB insert victim addr=0x" << std::hex
                   << e->dirtyLineAddr << std::dec << " wb_size=" << WB.size() << std::endl;

    // NOTE: draining happens in drainWB(), called once per operate()
    // cycle -- NOT here. Draining at most one entry per cycle (rather
    // than immediately, synchronously, right here) is what lets the WB
    // deque genuinely hold a backlog when dirty evictions arrive faster
    // than they drain, which is what makes DCM_WB_PRESSURE_THRESHOLD
    // admission backpressure meaningful and testable.
}

bool DRAM_CACHE_MANAGER::trySend(MEMORY_CONTROLLER *mc, PACKET pkt, bool isWrite)
{
    // queue_type 2 = WQ (write), 1 = RQ (read) -- the same convention
    // get_occupancy()/get_size() use everywhere else in this port.
    uint8_t queueType = isWrite ? 2 : 1;
    if (mc->get_occupancy(queueType, pkt.address) >= mc->get_size(queueType, pkt.address))
        return false; // NOT sent -- caller retains ownership, nothing touched
    pkt.event_cycle = current_core_cycle[pkt.cpu];

    if (isWrite) {
        // A WQ duplicate-address merge coalesces this write into an
        // identically-addressed entry already queued. That is legitimate
        // real write-queue coalescing, produces no response either way,
        // and cannot lose anything in a model that carries no data --
        // so a merged write counts as genuinely sent.
        mc->add_wq(&pkt);
        return true;
    }

    // READS are different, and this is the CRITICAL-1 fix
    // (docs/final_independent_audit.md). MEMORY_CONTROLLER::add_rq()
    // returns a non-negative index -- and enqueues NOTHING -- when an
    // entry for the same address is already resident in the read queue
    // (dram_controller.cc, "check for duplicates in the read queue").
    // Every other add_rq() path (warmup shortcut, write-queue-forward
    // service, normal insert) returns -1, so `rc >= 0` identifies a
    // merge precisely.
    //
    // In stock ChampSim a merge is safe because the CALLER is a CACHE
    // that merges via its own MSHR and expects no per-dispatch response.
    // The DCM has no such merging: it needs exactly one return_data()
    // per dispatched read to advance the owning ORB entry. A merged read
    // therefore yields no completion at all, stranding that ORB entry
    // forever and -- because a live ORB entry permanently owns its
    // DRAM-cache index -- blocking every later request to that index.
    //
    // Reachable whenever promoteFromCRB() runs inside return_data(),
    // because MEMORY_CONTROLLER::process() only removes the completing
    // entry AFTER return_data() returns; a promoted request for that
    // same address then collides with it. Treating the merge as "not
    // accepted" hands the packet back to the caller's existing
    // retain-and-retry path, which succeeds on the next operate() cycle
    // once the completing entry has been removed.
    int rc = mc->add_rq(&pkt);
    if (rc >= 0) {
        stats.dispatchMergeRetries++;
        return false; // merged, NOT enqueued -- caller must retain and retry
    }
    return true;
}

bool DRAM_CACHE_MANAGER::dispatchToFar(const PACKET &pkt, bool isWrite)
{
    if (linkLatencyCycles == 0) {
        // Check farMC's REAL capacity before ever calling add_rq()/
        // add_wq() -- those functions have no bounds check of their own
        // and will silently discard the packet with no signal at all if
        // their queue is genuinely full (verified directly,
        // docs/wb_retry_audit.md).
        return trySend(farMC, pkt, isWrite);
    }

    DCM_PENDING_FAR_DISPATCH d;
    d.targetCycle = current_core_cycle[pkt.cpu] + linkLatencyCycles;
    d.pkt = pkt;
    d.isWrite = isWrite;
    pendingFarDispatches.push_back(d);

    if (debugPrint)
        std::cout << "[DCM] far dispatch held for link latency: addr=0x" << std::hex << pkt.address
                   << std::dec << (isWrite ? " WRITE" : " READ") << " target_cycle=" << d.targetCycle
                   << std::endl;
    return true; // safely queued; the real capacity check happens at release time below
}

void DRAM_CACHE_MANAGER::dispatchToFarGuaranteed(const PACKET &pkt, bool isWrite)
{
    if (dispatchToFar(pkt, isWrite))
        return; // sent now, or safely queued for the link-latency hold already

    // Immediate attempt failed (farMC has no room right now, and
    // linkLatencyCycles == 0 so dispatchToFar()'s own queuing branch
    // didn't run) -- queue for retry via the existing far-dispatch
    // processing loop, ready immediately (retried starting next cycle),
    // exactly like a link-latency-delayed entry that has just become
    // ready. This is what guarantees the caller (driveState()'s
    // DCM_FAR_MEM_READ case, bypass mode's add_rq()/add_wq()) never
    // silently loses the packet (docs/memory_dispatch_audit.md).
    stats.farDispatchRetries++;
    DCM_PENDING_FAR_DISPATCH d;
    d.targetCycle = current_core_cycle[pkt.cpu];
    d.pkt = pkt;
    d.isWrite = isWrite;
    pendingFarDispatches.push_back(d);

    if (debugPrint)
        std::cout << "[DCM] far dispatch retry-queued (farMC full) addr=0x" << std::hex << pkt.address
                   << std::dec << (isWrite ? " WRITE" : " READ") << std::endl;
}

void DRAM_CACHE_MANAGER::processPendingFarDispatches()
{
    // pendingFarDispatches is FIFO by construction (link latency is a
    // fixed, uniform delay, so entries become ready in the order they
    // were queued) -- pop everything ready from the front, EXCEPT that a
    // ready entry whose destination has no room right now is left in
    // place and processing stops for this cycle (docs/wb_retry_audit.md):
    // popping it first and discovering failure only afterward is exactly
    // how the underlying silent-drop bug gets triggered, and skipping
    // past it to a later entry would violate FIFO ordering.
    while (!pendingFarDispatches.empty() &&
           pendingFarDispatches.front().targetCycle <= current_core_cycle[pendingFarDispatches.front().pkt.cpu]) {
        const DCM_PENDING_FAR_DISPATCH &front = pendingFarDispatches.front();

        // Release through trySend() -- exactly as the near-side release
        // path does. This previously called farMC->add_rq()/add_wq()
        // DIRECTLY, which skipped the duplicate-address merge check and
        // left the far/bypass release path still able to lose a read
        // (docs/final_independent_audit.md, CRITICAL-2). trySend() also
        // re-stamps event_cycle to the actual dispatch cycle, which this
        // path needs because the packet may have been sitting here for
        // the whole link-latency hold.
        if (!trySend(farMC, front.pkt, front.isWrite)) {
            if (front.isWrite)
                stats.wbDispatchRetries++;
            else
                stats.farDispatchRetries++;
            if (debugPrint)
                std::cout << "[DCM] far dispatch BLOCKED (farMC queue full or merge) addr=0x" << std::hex
                           << front.pkt.address << std::dec << (front.isWrite ? " WRITE" : " READ")
                           << " retained" << std::endl;
            break;
        }

        if (debugPrint)
            std::cout << "[DCM] far dispatch released addr=0x" << std::hex << front.pkt.address << std::dec
                       << (front.isWrite ? " WRITE" : " READ") << " cycle=" << current_core_cycle[front.pkt.cpu]
                       << std::endl;
        pendingFarDispatches.pop_front();
    }
}

void DRAM_CACHE_MANAGER::dispatchToNear(const PACKET &pkt, bool isWrite)
{
    // If pendingNearDispatches already has entries, a fresh attempt MUST
    // still queue behind them rather than trying to sneak ahead via a
    // direct trySend() -- otherwise a later-arriving request could be
    // dispatched to nearMC before an earlier one still waiting for
    // capacity, violating FIFO order. Only attempt an immediate send
    // when the retry queue is empty.
    if (pendingNearDispatches.empty() && trySend(nearMC, pkt, isWrite))
        return;

    stats.nearDispatchRetries++;
    DCM_PENDING_NEAR_DISPATCH d;
    d.pkt = pkt;
    d.isWrite = isWrite;
    pendingNearDispatches.push_back(d);

    if (debugPrint)
        std::cout << "[DCM] near dispatch retry-queued (nearMC full) addr=0x" << std::hex << pkt.address
                   << std::dec << (isWrite ? " WRITE" : " READ") << std::endl;
}

void DRAM_CACHE_MANAGER::processPendingNearDispatches()
{
    // FIFO: pop everything acceptable from the front, stopping at the
    // first entry nearMC cannot yet accept (mirrors
    // processPendingFarDispatches()'s exact discipline -- see its
    // comment for why stopping, not skipping, is required for order).
    while (!pendingNearDispatches.empty()) {
        const DCM_PENDING_NEAR_DISPATCH &front = pendingNearDispatches.front();
        if (!trySend(nearMC, front.pkt, front.isWrite)) {
            if (debugPrint)
                std::cout << "[DCM] near dispatch BLOCKED (nearMC queue full) addr=0x" << std::hex
                           << front.pkt.address << std::dec << (front.isWrite ? " WRITE" : " READ")
                           << " retained" << std::endl;
            break;
        }
        if (debugPrint)
            std::cout << "[DCM] near dispatch released addr=0x" << std::hex << front.pkt.address << std::dec
                       << (front.isWrite ? " WRITE" : " READ") << " cycle=" << current_core_cycle[front.pkt.cpu]
                       << std::endl;
        pendingNearDispatches.pop_front();
    }
}

void DRAM_CACHE_MANAGER::drainWB()
{
    if (WB.empty())
        return;

    // Copy (not a reference) so it stays valid across the eventual
    // pop_front() below -- WB.front() is only actually popped AFTER
    // dispatchToFar() confirms the write was handed off.
    DCM_WB_ENTRY front = WB.front();

    if (!dispatchToFar(front.pkt, /*isWrite=*/true)) {
        // farMC has no room right now -- the entry was NEVER sent
        // (dispatchToFar() guarantees this: no partial dispatch, no
        // internal state changed on a false return). Leave it at the
        // front of WB; the next operate() cycle's drainWB() call will
        // retry the SAME entry, exactly mirroring gem5's invariant that
        // pktFarMemWrite's front is only popped once
        // farReqPort.sendTimingReq() returns true
        // (docs/wb_retry_audit.md).
        stats.wbDispatchRetries++;
        if (debugPrint)
            std::cout << "[DCM] WB drain BLOCKED (farMC WQ full) addr=0x" << std::hex << front.pkt.address
                       << std::dec << " retained, wb_size=" << WB.size() << std::endl;
        return;
    }

    // Only pop AFTER confirmed successful hand-off (dispatched now, or
    // safely queued in pendingFarDispatches under link latency) -- never
    // before. This is what makes loss and duplication both impossible:
    // the entry is removed exactly once, exactly when (and only when)
    // dispatchToFar() reports success.
    WB.pop_front();
    stats.wbDrains++;
    stats.farWrites++;
    stats.numWrBacks++;

    if (debugPrint)
        std::cout << "[DCM] WB drain -> FAR_MEM_WRITE (dirty victim writeback) addr=0x" << std::hex
                   << front.pkt.address << std::dec << " remaining_wb_size=" << WB.size() << std::endl;
}

void DRAM_CACHE_MANAGER::recordOccupancySamples()
{
    uint64_t orbSize = ORB.size();
    uint64_t crbSize = CRB.size();
    uint64_t wbSize = WB.size();

    if (orbSize > stats.orbMaxOccupancy)
        stats.orbMaxOccupancy = orbSize;
    if (crbSize > stats.crbMaxOccupancy)
        stats.crbMaxOccupancy = crbSize;
    if (wbSize > stats.wbMaxOccupancy)
        stats.wbMaxOccupancy = wbSize;

    stats.orbOccupancySum += orbSize;
    stats.crbOccupancySum += crbSize;
    stats.wbOccupancySum += wbSize;
    stats.occupancySamples++;
}

void DRAM_CACHE_MANAGER::admitRequest(PACKET *packet, bool isWrite, uint64_t admittedArrivalCycle)
{
    DCM_ORB_ENTRY *e = new DCM_ORB_ENTRY();
    e->requestId = nextRequestId++;
    e->validEntry = true;
    e->arrivalCycle = (admittedArrivalCycle == (uint64_t)-1) ? current_core_cycle[packet->cpu] : admittedArrivalCycle;
    e->pkt = *packet;
    e->isWriteReq = isWrite;
    e->indexDC = returnIndexDC(packet->address);
    e->tagDC = returnTagDC(packet->address);
    e->pol = policy;
    e->state = DCM_START;

    ORB[packet->address] = e;

    stats.totalRequests++;
    if (isWrite)
        stats.writeRequests++;
    else
        stats.readRequests++;

    if (debugPrint) {
        std::cout << "[DCM] req#" << e->requestId << (isWrite ? " WRITE " : " READ ")
                   << "addr=0x" << std::hex << packet->address << std::dec
                   << " index=" << e->indexDC << " tag=" << e->tagDC
                   << " arrival_cycle=" << e->arrivalCycle << std::endl;
    }

    // Real hit/miss/dirty/victim classification + eager metadata install,
    // BEFORE the tag-check read is dispatched (matches gem5's ordering --
    // see the banner comment above this section).
    classifyAndInstall(e);

    chooseInitialState(e);
    driveState(e);
}

void DRAM_CACHE_MANAGER::chooseInitialState(DCM_ORB_ENTRY *e)
{
    bool isWriteHit = e->isWriteReq && e->isHit;
    // gem5's own condition is simply `!isDirty` on the OLD resident line
    // (policy_manager.cc:787,795,805 -- `isDirty` there is `checkDirty(addr)`,
    // i.e. `validLine && dirtyLine` of whatever occupied the index BEFORE
    // this request). That is exactly `!(victimWasValid && victimWasDirty)`
    // here, which is true for both a genuinely clean valid victim AND a
    // cold/invalid line -- gem5 does not distinguish the two for this
    // purpose either.
    bool isDirtyVictimMiss = !e->isHit && e->victimWasValid && e->victimWasDirty;
    bool isCleanMiss = !e->isHit && !isDirtyVictimMiss;

    if (isWriteHit) {
        stats.writeHitOptOpportunities++;
        if (e->pol != DCM_POLICY_BEAR_WR_OPT && e->pol != DCM_POLICY_ORACLE)
            stats.writeHitOptNotApplicable++;
    }
    if (isCleanMiss) {
        stats.cleanMissOptOpportunities++;
        if (e->pol != DCM_POLICY_ORACLE)
            stats.cleanMissOptNotApplicable++;
    }

    if (e->pol == DCM_POLICY_BEAR_WR_OPT && isWriteHit) {
        // BEAR-Wr-Opt: the ONLY case that skips the local tag-check read
        // (policy_manager.cc:897-911). Every other case -- read hit, read
        // miss, write miss of any kind -- falls through to the identical
        // baseline behavior below.
        stats.writeHitOptApplied++;
        stats.localTagCheckReadsAvoided++;
        e->state = DCM_LOC_MEM_WRITE;
        if (debugPrint)
            std::cout << "[DCM] req#" << e->requestId
                       << " BEAR-Wr-Opt: write HIT -> skip tag-check read, go straight to LOC_MEM_WRITE"
                       << std::endl;
        return;
    }

    if (e->pol == DCM_POLICY_ORACLE) {
        if (isWriteHit) {
            // Oracle exempts write hits exactly like BEAR-Wr-Opt
            // (policy_manager.cc:804-809, `(!isRead && isHit) -> locMemWrite`).
            stats.oracleWriteHits++;
            stats.writeHitOptApplied++;
            stats.localTagCheckReadsAvoided++;
            e->state = DCM_LOC_MEM_WRITE;
            if (debugPrint)
                std::cout << "[DCM] req#" << e->requestId
                           << " Oracle: write HIT -> skip tag-check read, go straight to LOC_MEM_WRITE"
                           << std::endl;
            return;
        }
        if (isCleanMiss) {
            // Oracle's ADDITIONAL exemption over BEAR-Wr-Opt: any miss
            // (read or write) whose victim was not dirty. Read miss clean
            // goes straight to the far fetch (policy_manager.cc:795-800,
            // `(isRead && !isHit && !isDirty) -> farMemRead`); write miss
            // clean goes straight to the local write
            // (policy_manager.cc:804-809, `(!isRead && !isHit && !isDirty)
            // -> locMemWrite`) -- there is nothing to fetch from far for a
            // write, and nothing dirty to write back, so the incoming
            // write data is simply installed.
            stats.oracleCleanMisses++;
            stats.cleanMissOptApplied++;
            stats.localTagCheckReadsAvoided++;
            e->state = e->isWriteReq ? DCM_LOC_MEM_WRITE : DCM_FAR_MEM_READ;
            if (debugPrint)
                std::cout << "[DCM] req#" << e->requestId << " Oracle: " << (e->isWriteReq ? "WRITE" : "READ")
                           << " CLEAN MISS -> skip tag-check read, go straight to "
                           << (e->isWriteReq ? "LOC_MEM_WRITE" : "FAR_MEM_READ") << std::endl;
            return;
        }
        // Read hit (needs the local read to fetch actual data, not just
        // check the tag) and dirty-victim miss of either kind (needs the
        // local read to SOURCE the dirty victim's data for write-back)
        // both fall through to the identical baseline path below --
        // Oracle's zero-latency SRAM tag store only ever eliminates a
        // TAG CHECK, never a genuine data read.
    }

    // Baseline (and BEAR-Wr-Opt/Oracle for every non-exempted case): the
    // tag-check read is never skipped (policy_manager.cc:686-691 baseline;
    // :897-903 BEAR-Wr-Opt's own fallback; :784-792 Oracle's own fallback).
    e->state = DCM_LOC_MEM_READ;
}

void DRAM_CACHE_MANAGER::driveState(DCM_ORB_ENTRY *e)
{
    // Every sub-request handed to nearMC/farMC needs its own event_cycle
    // stamped to "now": MEMORY_CONTROLLER::update_schedule_cycle() only
    // ever picks entries whose event_cycle is less than the current
    // minimum (dram_controller.cc:558), and PACKET's default constructor
    // leaves event_cycle at UINT64_MAX. Real CACHE traffic gets a real
    // event_cycle stamped on it upstream before reaching add_rq/add_wq;
    // our synthetically-generated sub-requests must do the same here.
    uint64_t nowCycle = current_core_cycle[e->pkt.cpu];

    switch (e->state) {
    case DCM_LOC_MEM_READ: {
        PACKET p = e->pkt;
        p.type = LOAD; // tag-check read, always a read regardless of the original request type
        p.event_cycle = nowCycle;
        e->state = DCM_WAITING_LOC_MEM_READ_RESP;
        e->issued = true;
        stats.sentToNear++;
        stats.localReads++;
        if (debugPrint)
            std::cout << "[DCM] req#" << e->requestId << " -> LOC_MEM_READ (tag check)" << std::endl;
        dispatchToNear(p, /*isWrite=*/false);
        break;
    }
    case DCM_FAR_MEM_READ: {
        PACKET p = e->pkt;
        p.type = LOAD;
        p.event_cycle = nowCycle;
        e->state = DCM_WAITING_FAR_MEM_READ_RESP;
        e->issued = true;
        stats.sentToFar++;
        stats.farReads++;
        if (debugPrint)
            std::cout << "[DCM] req#" << e->requestId << " -> FAR_MEM_READ (miss fetch)" << std::endl;
        dispatchToFarGuaranteed(p, /*isWrite=*/false);
        break;
    }
    case DCM_LOC_MEM_WRITE: {
        PACKET p = e->pkt;
        p.type = WRITEBACK;
        p.event_cycle = nowCycle;
        e->state = DCM_WAITING_LOC_MEM_WRITE_RESP;
        e->issued = true;
        stats.sentToNear++;
        stats.localWrites++;
        if (debugPrint)
            std::cout << "[DCM] req#" << e->requestId << " -> LOC_MEM_WRITE" << std::endl;
        dispatchToNear(p, /*isWrite=*/true);
        // MEMORY_CONTROLLER::process() never calls return_data for the
        // write-queue branch (dram_controller.cc:302-320), so no callback
        // will ever arrive for this. Complete synchronously here instead
        // of waiting for one -- matches ChampSim's existing fire-and-forget
        // write contract (see gem5_to_champsim_mapping.md fact #2).
        completeRequest(e);
        break;
    }
    default:
        break;
    }
}

void DRAM_CACHE_MANAGER::completeRequest(DCM_ORB_ENTRY *e, uint64_t responseLatencyCycles)
{
    e->state = DCM_DONE;

    if (debugPrint) {
        std::cout << "[DCM] req#" << e->requestId << " DONE addr=0x" << std::hex << e->pkt.address
                   << std::dec << " total_cycles=" << (current_core_cycle[e->pkt.cpu] - e->arrivalCycle)
                   << std::endl;
    }

    if (!e->isWriteReq) {
        stats.completedReadsToLLC++;
        PACKET resp = e->pkt;
        // Controller latency (frontendLatencyCycles+backendLatencyCycles,
        // or +2*backendLatencyCycles for a far-fetch response -- see the
        // header comment on frontendLatencyCycles) delays ONLY this
        // delivery, exactly mirroring gem5's decoupling between
        // ORB.erase()/resumeConflictingReq() (synchronous, see below,
        // unaffected) and port.schedTimingResp() (scheduled
        // responseLatencyCycles later). scheduleResponse() delivers
        // immediately if responseLatencyCycles == 0.
        scheduleResponse(resp, responseLatencyCycles);
    } else {
        stats.completedWrites++;
    }

    uint64_t freedIndex = e->indexDC;
    ORB.erase(e->pkt.address);
    delete e;

    // The DRAM-cache index this entry occupied is now free -- see if
    // anything queued in the CRB was waiting on it (gem5
    // resumeConflictingReq, called at the same point in
    // handleNextState's "done" transitions). This happens IMMEDIATELY,
    // regardless of controller latency -- gem5 does the equivalent
    // (`resumeConflictingReq(orbEntry)`) synchronously too, right after
    // scheduling (not waiting for) the delayed response.
    promoteFromCRB(freedIndex);
}

void DRAM_CACHE_MANAGER::scheduleResponse(const PACKET &pkt, uint64_t delayCycles)
{
    if (delayCycles == 0) {
        PACKET resp = pkt;
        if (resp.instruction)
            upper_level_icache[resp.cpu]->return_data(&resp);
        if (resp.is_data)
            upper_level_dcache[resp.cpu]->return_data(&resp);
        return;
    }

    DCM_PENDING_RESPONSE r;
    r.targetCycle = current_core_cycle[pkt.cpu] + delayCycles;
    r.pkt = pkt;
    pendingResponses.push_back(r);

    if (debugPrint)
        std::cout << "[DCM] response held for controller latency: addr=0x" << std::hex << pkt.address
                   << std::dec << " target_cycle=" << r.targetCycle << std::endl;
}

void DRAM_CACHE_MANAGER::processPendingResponses()
{
    // FIFO by construction (controller latency is a fixed, uniform delay
    // applied at completion time, so entries become ready in the order
    // they were queued).
    while (!pendingResponses.empty() &&
           pendingResponses.front().targetCycle <= current_core_cycle[pendingResponses.front().pkt.cpu]) {
        DCM_PENDING_RESPONSE r = pendingResponses.front();
        pendingResponses.pop_front();
        if (r.pkt.instruction)
            upper_level_icache[r.pkt.cpu]->return_data(&r.pkt);
        if (r.pkt.is_data)
            upper_level_dcache[r.pkt.cpu]->return_data(&r.pkt);
        if (debugPrint)
            std::cout << "[DCM] response released addr=0x" << std::hex << r.pkt.address << std::dec
                       << " cycle=" << current_core_cycle[r.pkt.cpu] << std::endl;
    }
}

void DRAM_CACHE_MANAGER::return_data(PACKET *packet)
{
    // bypassDcache: a bypass-mode read completing at farMC has no ORB
    // entry (bypass never creates one), so it must be recognized and
    // delivered to the LLC BEFORE falling into the ORB lookup below
    // (which would otherwise silently drop it as "untracked" -- exactly
    // the pre-existing defensive branch a few lines down). Checked via
    // bypassOutstandingReads rather than the current bypassDcache flag
    // value, so a response for a request that was admitted while bypass
    // was enabled is still routed correctly even if bypass is toggled
    // off before that response arrives (no reliance on the flag being
    // held constant for a request's whole lifetime). No controller
    // frontend/backend latency and no ORB/CRB/promoteFromCRB bookkeeping
    // apply here at all -- verified from gem5's own bypass response path
    // (`farMemRecvTimingResp`: `port.schedTimingResp(pkt, curTick())`,
    // i.e. immediate, zero added ticks -- see docs/bypass_mode.md).
    std::multiset<uint64_t>::iterator bypassIt = bypassOutstandingReads.find(packet->address);
    if (bypassIt != bypassOutstandingReads.end()) {
        bypassOutstandingReads.erase(bypassIt);
        stats.bypassCompletedReads++;
        if (debugPrint)
            std::cout << "[DCM] bypass response addr=0x" << std::hex << packet->address << std::dec
                       << std::endl;
        if (packet->instruction)
            upper_level_icache[packet->cpu]->return_data(packet);
        if (packet->is_data)
            upper_level_dcache[packet->cpu]->return_data(packet);
        return;
    }

    std::map<uint64_t, DCM_ORB_ENTRY *>::iterator it = ORB.find(packet->address);
    if (it == ORB.end()) {
        // Defensive: should not happen in the skeleton's
        // single-outstanding-per-address model. Documented in
        // docs/limitations.md as a case that needs the real CRB before a
        // second request to the same index can be handled at all.
        if (debugPrint)
            std::cerr << "[DCM][WARN] return_data for untracked addr 0x" << std::hex << packet->address
                       << std::dec << std::endl;
        return;
    }

    DCM_ORB_ENTRY *e = it->second;

    if (e->state == DCM_WAITING_LOC_MEM_READ_RESP) {
        stats.completedFromNear++;

        // The tag-check read that just completed is what "sources" a
        // dirty victim's data (tag+data co-located, per the baseline's
        // design) -- push its write-back now, before anything else, no
        // matter what happens next. Matches gem5's exact trigger point
        // (policy_manager.cc:521-538, see the banner comment above).
        if (e->handleDirtyLine)
            pushDirtyWriteBack(e);

        if (e->isWriteReq) {
            // Tag-check read done for a write -> proceed to the actual
            // local write (unconditional on hit/miss -- baseline always
            // installs the write's data locally).
            e->state = DCM_LOC_MEM_WRITE;
            driveState(e);
        } else if (e->isHit) {
            // Real tag-store hit (classifyAndInstall(), run at admission).
            // gem5: `accessAndRespond(orbEntry->owPkt, frontendLatency +
            // backendLatency);` (policy_manager.cc:1042-1043 and the two
            // textually-identical BEAR/Oracle sites) -- a single backend
            // latency, no far fetch was involved.
            completeRequest(e, frontendLatencyCycles + backendLatencyCycles);
        } else {
            e->state = DCM_FAR_MEM_READ;
            driveState(e);
        }
    } else if (e->state == DCM_WAITING_FAR_MEM_READ_RESP) {
        stats.completedFromFar++;

        // Respond to the LLC now, before the fill write -- matches gem5's
        // respond-before-fill behavior (gem5_to_champsim_mapping.md fact
        // #1). Capture what the fill write needs before completeRequest()
        // deletes e.
        PACKET fillPkt = e->pkt;
        fillPkt.type = WRITEBACK;
        fillPkt.event_cycle = current_core_cycle[e->pkt.cpu];

        // gem5: `accessAndRespond(orbEntry->owPkt, frontendLatency +
        // backendLatency + backendLatency);` (policy_manager.cc:734-735
        // and the two textually-identical BEAR/Oracle sites) -- an EXTRA
        // backendLatency versus the plain-hit case above, because this
        // response followed a far fetch. Verified directly from the gem5
        // source, not assumed from the paper's flat "20ns" description --
        // see the DCM_FRONTEND_/BACKEND_LATENCY_* macro comment.
        completeRequest(e, frontendLatencyCycles + backendLatencyCycles + backendLatencyCycles);

        stats.sentToNear++;
        stats.localWrites++;
        if (debugPrint)
            std::cout << "[DCM] background fill write addr=0x" << std::hex << fillPkt.address << std::dec
                       << std::endl;
        // Deliberately off the critical response path (the LLC response
        // above already happened) -- but the fill itself must never be
        // lost, so it goes through the same guaranteed near-dispatch
        // mechanism as every other near-side operation
        // (docs/memory_dispatch_audit.md), not a bare add_wq() call.
        dispatchToNear(fillPkt, /*isWrite=*/true);
    }
    // DCM_WAITING_LOC_MEM_WRITE_RESP is never observed here: near-controller
    // writes never invoke return_data (see driveState()'s DCM_LOC_MEM_WRITE
    // case), so that branch of the state machine completes synchronously
    // instead of via this callback.
}
