#ifndef DRAM_CACHE_MANAGER_H
#define DRAM_CACHE_MANAGER_H

#include <deque>
#include <map>
#include <set>
#include <vector>

#include "memory_class.h"
#include "dram_controller.h"

// ============================================================================
// Configuration parameters
//
// Paper value -> ChampSim value -> exact/adapted -> reason
// (full table + gem5 cross-reference lives in docs/gem5_to_champsim_mapping.md)
// ============================================================================

// paper Table I: ORB = 128 entries -> exact
#define DCM_ORB_MAX_SIZE 128
// paper Table I: CRB = 32 entries -> exact, and enforced: add_rq()/add_wq()
// reject a conflicting request when CRB.size() >= this value
// (dram_cache_manager.cc:271,347), mirroring gem5's own CRB-full retry in
// recvTimingReq (policy_manager.cc:296-366). Also reported as the CRB
// capacity by get_occupancy()/get_size() (dram_cache_manager.cc:431).
#define DCM_CRB_MAX_SIZE 32
// paper Table I: WB Buffer = 64 entries -> exact (structural size; see
// DCM_WB_PRESSURE_THRESHOLD below for gem5's actual admission-throttle
// value, which is a different number derived from ORB size, not this one)
#define DCM_WB_MAX_SIZE 64
// gem5's ACTUAL admission-backpressure threshold (verified directly in
// policy_manager.cc:332, `recvTimingReq`):
//   if (pktFarMemWrite.size() >= (orbMaxSize / 2)) { ...retry... }
// i.e. orbMaxSize/2 = 128/2 = 64. This happens to equal DCM_WB_MAX_SIZE
// numerically, but they are conceptually different in gem5: the WB deque
// itself (pktFarMemWrite) has NO hard capacity check anywhere in gem5 --
// it is an unbounded std::deque. The "WB Buffer = 64 entries" in the
// paper's Table I describes the real hardware's structural WB buffer
// size; gem5's model approximates "the WB buffer is getting full" by
// throttling new ORB admissions once the backlog reaches orbMaxSize/2,
// rather than tracking real WB occupancy against a 64-entry cap directly.
// This port replicates gem5's actual mechanism (threshold derived from
// ORB size), not an independently-invented 64-entry cap on the WB deque.
#define DCM_WB_PRESSURE_THRESHOLD (DCM_ORB_MAX_SIZE / 2)
// paper: DRAM cache capacity = 128 MB -> exact
#define DCM_DRAM_CACHE_SIZE (128ULL * 1024ULL * 1024ULL)
// paper: 64B cache-line granularity -> reuse ChampSim's global BLOCK_SIZE (already 64)
#define DCM_BLOCK_SIZE ((uint64_t)BLOCK_SIZE)
// Number of logical DRAM-cache lines = capacity / line size
//   = 128 MiB / 64 B = 2,097,152.
// This is both the size of tagMetadataStore and the modulus of the
// direct-mapped index, so capacity and granularity are guaranteed by
// construction: DCM_NUM_LINES * DCM_BLOCK_SIZE == DCM_DRAM_CACHE_SIZE.
// See returnIndexDC()'s comment block in src/dram_cache_manager.cc for the
// full PACKET::address convention this depends on.
#define DCM_NUM_LINES (DCM_DRAM_CACHE_SIZE / DCM_BLOCK_SIZE)
// paper Table I: DRAM Cache Manager frontend/backend latency = 20ns
// round-trip -> gem5 does NOT model this as one combined 20ns constant.
// It has two SEPARATE SimObject params, `static_frontend_latency` and
// `static_backend_latency`, both defaulting to 10ns each
// (src/mem/PolicyManager.py, verified directly from the pulled gem5
// source), and applies them as `frontendLatency + backendLatency`
// (10+10=20ns, matching the paper's stated round-trip) to a normal
// response, but as `frontendLatency + backendLatency + backendLatency`
// (10+10+10=30ns) to the read-miss-then-far-fetch response specifically
// (`policy_manager.cc:734-735,853-854,959-960`, one site per policy, all
// three textually identical). Naively applying a single flat "20ns to
// every response" constant would NOT reproduce this -- the extra
// backendLatency on the far-miss path is a real, deliberate distinction
// in gem5's code, not an approximation. See
// DRAM_CACHE_MANAGER::frontendLatencyCycles/backendLatencyCycles and
// completeRequest()'s `responseLatencyCycles` parameter.
#define DCM_FRONTEND_LATENCY_NS 10.0
#define DCM_BACKEND_LATENCY_NS 10.0
#define DCM_FRONTEND_LATENCY_CYCLES ((uint64_t)((DCM_FRONTEND_LATENCY_NS * CPU_FREQ) / 1000.0))
#define DCM_BACKEND_LATENCY_CYCLES ((uint64_t)((DCM_BACKEND_LATENCY_NS * CPU_FREQ) / 1000.0))
// paper Case Study 3: far-memory link round-trip latency, tested at
// 100/500/1000 ns. The MECHANISM is implemented and applied: a nonzero
// linkLatencyCycles makes dispatchToFar() hold each far-bound packet in
// pendingFarDispatches until current_core_cycle >= targetCycle before it
// is offered to farMC (dram_cache_manager.cc:712-722, released by
// processPendingFarDispatches()); zero means dispatch immediately.
// main.cc's --dcm_link_latency_ns knob converts ns to cycles and calls
// setLinkLatency() (main.cc:782). The three named constants below are
// the paper's own Case Study 3 values, used directly by
// tests/test_dcm_link_latency.cc, test_dcm_controller_latency.cc and
// test_dcm_bypass.cc; they are reference values, not the only settable
// ones -- setLinkLatency() accepts any cycle count.
#define DCM_LINK_LATENCY_CYCLES_100NS ((uint64_t)((100.0 * CPU_FREQ) / 1000.0))
#define DCM_LINK_LATENCY_CYCLES_500NS ((uint64_t)((500.0 * CPU_FREQ) / 1000.0))
#define DCM_LINK_LATENCY_CYCLES_1000NS ((uint64_t)((1000.0 * CPU_FREQ) / 1000.0))

// ----------------------------------------------------------------------------
// Near/far memory technology configuration (paper Table I, Section III).
//
// ChampSim's MEMORY_CONTROLLER bandwidth model:
//   BW(GB/s) = DRAM_CHANNEL_WIDTH(bytes, fixed at 8 globally -- see
//              limitations.md) * DRAM_MTPS / 1000
// so MTPS = BW * 1000 / 8.
//
//   near (DRAM cache) = HBM2, paper-declared peak bandwidth = 32 GB/s
//     -> MTPS = 32 * 1000 / 8 = 4000
//   far (backing store) = DDR4, paper-declared peak bandwidth = 19.2 GB/s
//     -> MTPS = 19.2 * 1000 / 8 = 2400 (matches the real DDR4-2400 JEDEC
//        speed grade -- and gem5's own DDR4_2400_16x4 SimObject name --
//        exactly)
//
// tRP/tRCD/tCAS are NOT differentiated between near and far: gem5's real
// per-technology access latencies live inside its DRAMInterface
// SimObjects (HBM_2000_4H_1x64, DDR4_2400_16x4), which are not given as
// explicit nanosecond numbers anywhere in the paper's own text. Rather
// than invent plausible-sounding numbers, both keep ChampSim's existing
// generic default (12.5 ns each). This is a documented, deliberate
// approximation -- see docs/gem5_to_champsim_mapping.md and
// docs/limitations.md.
// ----------------------------------------------------------------------------
#define DCM_NEAR_HBM2_MTPS 4000
#define DCM_FAR_DDR4_MTPS 2400

// Applies the paper's declared HBM2/DDR4 peak-bandwidth configuration to
// a MEMORY_CONTROLLER instance. Free functions (not DRAM_CACHE_MANAGER
// methods) since they only need a MEMORY_CONTROLLER reference -- usable
// directly on uncore.DRAM_CACHE_DEVICE/uncore.DRAM from main.cc, or on
// any test's own controller instances.
void configureNearAsHBM2(MEMORY_CONTROLLER &mc);
void configureFarAsDDR4(MEMORY_CONTROLLER &mc);

// Correctly-rounded DRAM_DBUS_RETURN_TIME for a given MT/s rate. See the
// large comment above this function's definition in
// dram_cache_manager.cc for the integer-truncation bug this fixes
// (ChampSim's original formula collapses every MTPS in [2001,4000] to
// the same value at CPU_FREQ=4000).
uint32_t computeDbusReturnTime(uint32_t mtps);

// Fraction (0..1) of a controller's theoretical peak bandwidth actually
// used, given how many BLOCK_SIZE-sized operations it performed over
// `elapsedCycles` cycles. Peak is DRAM_CHANNEL_WIDTH bytes per transfer
// at `mtps` MT/s, converted to bytes/cycle via CPU_FREQ -- the same
// derivation used to SET DRAM_MTPS from the paper's declared peak GB/s
// (see the near/far configuration comment above).
//
// Factored out of main.cc's print_dcm_stats() specifically so the
// arithmetic is unit-testable, including the degenerate inputs that
// previously produced a silent unsigned underflow
// (docs/final_independent_audit.md, HIGH-1). Returns 0.0 for
// elapsedCycles == 0 or mtps == 0 rather than dividing by zero; the
// caller is responsible for deciding how to display "no data".
double dcmBandwidthUtilization(uint64_t opCount, uint32_t mtps, uint64_t elapsedCycles);

// ============================================================================
// Policies discussed in the paper (Section V), mirroring gem5's
// enums::Policy (CascadeLakeNoPartWrs / BearWriteOpt / RambusHypo).
//
// All three are implemented and independently selectable at run time via
// main.cc's --dcm_policy flag; see DRAM_CACHE_MANAGER::chooseInitialState()
// for the per-policy state-machine entry points. Each policy's full
// per-operation behavior is verified against the paper's Table II
// (8 cells x 3 policies = 24, all exact) by tests/test_dcm_baseline.cc,
// tests/test_dcm_bear.cc and tests/test_dcm_oracle.cc.
//
// They differ only in which requests may skip the local tag-check read:
//   BASELINE_CASCADE_LAKE  never skips it -- every request tag-checks.
//   BEAR_WR_OPT            skips it on write hits.
//   ORACLE (RambusHypo)    skips it on write hits AND on clean misses,
//                          deciding hit/dirty before the request starts.
// See chooseInitialState() for why the Oracle port follows the paper's
// Table II rather than gem5's literal code, which contradicts it.
// ============================================================================
enum DCM_POLICY {
    DCM_POLICY_BASELINE_CASCADE_LAKE = 0,
    DCM_POLICY_BEAR_WR_OPT           = 1,
    DCM_POLICY_ORACLE                = 2
};

// ============================================================================
// Request states, mirroring gem5 PolicyManager::reqState.
// ============================================================================
enum DCM_REQ_STATE {
    DCM_START = 0,
    DCM_LOC_MEM_READ,
    DCM_WAITING_LOC_MEM_READ_RESP,
    DCM_LOC_MEM_WRITE,
    DCM_WAITING_LOC_MEM_WRITE_RESP,
    DCM_FAR_MEM_READ,
    DCM_WAITING_FAR_MEM_READ_RESP,
    DCM_FAR_MEM_WRITE,
    DCM_WAITING_FAR_MEM_WRITE_RESP,
    DCM_DONE
};

// DRAM-cache tag/metadata store entry (gem5 tagMetaStoreEntry). This is
// the real, authoritative record of what's resident at each direct-mapped
// index -- consulted and updated on every request (see
// DRAM_CACHE_MANAGER::classifyAndInstall, ported from gem5
// handleRequestorPkt's inline hit/miss + metadata-update logic).
struct DCM_TAG_ENTRY {
    uint64_t tagDC;
    uint64_t indexDC;
    bool validLine;
    bool dirtyLine;
    uint64_t farMemAddr;

    DCM_TAG_ENTRY() : tagDC(0), indexDC(0), validLine(false), dirtyLine(false), farMemAddr(0) {}
};

// One entry of the Outstanding Requests Buffer (ORB) -- gem5 reqBufferEntry.
class DCM_ORB_ENTRY {
  public:
    uint64_t requestId;
    bool validEntry;
    uint64_t arrivalCycle;

    uint64_t tagDC;
    uint64_t indexDC;

    PACKET pkt; // owned copy of the requestor's packet
    bool isWriteReq; // true if admitted via add_wq, false if via add_rq

    DCM_POLICY pol;
    DCM_REQ_STATE state;

    bool issued;
    bool isHit;   // derived from tagMetadataStore in classifyAndInstall()
    bool isDirty; // true if the OLD resident line at this index was dirty (only meaningful pre-classification/for logging)
    bool conflict;

    // Victim bookkeeping: state of whatever occupied this DRAM-cache index
    // BEFORE this request's classification overwrote it (gem5's implicit
    // "old tagMetadataStore.at(indexDC)" reads in handleRequestorPkt,
    // captured explicitly here for testability/logging).
    bool victimWasValid;
    bool victimWasDirty;
    uint64_t victimFarAddr;

    // Set when the old resident line must be written back to far memory
    // (gem5 orbEntry->dirtyLineAddr / handleDirtyLine): true only when the
    // old line was valid+dirty AND this request is a miss (a hit means no
    // eviction happens at all).
    bool handleDirtyLine;
    uint64_t dirtyLineAddr;

    DCM_ORB_ENTRY()
        : requestId(0), validEntry(false), arrivalCycle(0), tagDC(0), indexDC(0),
          isWriteReq(false), pol(DCM_POLICY_BASELINE_CASCADE_LAKE), state(DCM_START),
          issued(false), isHit(false), isDirty(false), conflict(false), victimWasValid(false),
          victimWasDirty(false), victimFarAddr(0), handleDirtyLine(false), dirtyLineAddr(0) {}
};

// Conflicting Requests Buffer (CRB) entry -- gem5 timeReqPair.
// A request lands here instead of the ORB when it maps to a DRAM-cache
// index (not necessarily the same address) that some other ORB entry is
// already occupying. It waits here until that blocking ORB entry
// completes, then is promoted into the ORB -- see
// DRAM_CACHE_MANAGER::promoteFromCRB / checkConflictInORB.
struct DCM_CRB_ENTRY {
    uint64_t arrivalCycle;
    PACKET pkt;
    bool isWriteReq;
};

// Write-Back Buffer (WB) entry -- gem5 pktFarMemWrite. Populated by
// pushDirtyWriteBack() (gem5 handleDirtyCacheLine) when a dirty victim is
// evicted. Drained at most one entry per operate() cycle (see
// DRAM_CACHE_MANAGER::drainWB), so it genuinely holds a backlog when
// dirty evictions arrive faster than they can be drained -- this is what
// makes DCM_WB_PRESSURE_THRESHOLD admission backpressure meaningful.
struct DCM_WB_ENTRY {
    uint64_t arrivalCycle;
    PACKET pkt;
};

// A far-memory operation (read fetch or write-back) that has been
// decided but is being held for `linkLatencyCycles` before actually
// being dispatched to farMC -- models the paper's Case Study 3
// configurable link latency between the DRAM Cache Manager and the
// remote/far backing store. Empty (unused) when linkLatencyCycles == 0,
// in which case dispatch is immediate, exactly as before this feature
// was added.
struct DCM_PENDING_FAR_DISPATCH {
    uint64_t targetCycle;
    PACKET pkt;
    bool isWrite;
};

// A near-memory operation (tag-check read, direct write, or background
// fill write) whose immediate dispatch attempt found nearMC's
// destination queue full and must be retried (docs/memory_dispatch_audit.md).
// Near-memory dispatch has no Case-Study-3 link-latency concept (that
// models the physical near<->far manager link only) -- so unlike
// DCM_PENDING_FAR_DISPATCH, there is no targetCycle delay field: an
// entry here is always "ready right now," just blocked on capacity, and
// processPendingNearDispatches() retries the front of this deque every
// operate() cycle until nearMC has room.
struct DCM_PENDING_NEAR_DISPATCH {
    PACKET pkt;
    bool isWrite;
};

// A READ response that has been decided (the ORB entry is already
// retired and CRB promotion already checked -- see completeRequest())
// but whose delivery to the LLC is held for the controller's
// frontend+backend latency, mirroring gem5's decoupling between
// `ORB.erase()`/`resumeConflictingReq()` (synchronous) and the packet
// actually leaving the port via `port.schedTimingResp()` (scheduled
// `static_latency` ticks later, policy_manager.cc:1541-1579). Empty
// (unused) when both frontendLatencyCycles and backendLatencyCycles are
// 0, in which case delivery is immediate, exactly as before this
// feature was added. WRITEs never produce a response at all in this
// port (see gem5_to_champsim_mapping.md fact #2), so they never appear
// here.
struct DCM_PENDING_RESPONSE {
    uint64_t targetCycle;
    PACKET pkt;
};

struct DCM_STATS {
    uint64_t totalRequests;
    uint64_t readRequests;
    uint64_t writeRequests;
    uint64_t sentToNear;
    uint64_t sentToFar;
    uint64_t completedFromNear;
    uint64_t completedFromFar;
    uint64_t completedReadsToLLC;
    uint64_t completedWrites;
    uint64_t orbFullRejects;
    uint64_t wqFullSignals;
    uint64_t crbInserts;
    uint64_t crbPromotions;
    uint64_t crbFullRejects;

    // Granular per-Table-II-category operation counts (gem5
    // PolicyManagerStats). sentToNear == localReads + localWrites and
    // sentToFar == farReads + farWrites always hold; these are kept
    // separately because Table II categorizes by operation type, not just
    // "which memory".
    uint64_t localReads;
    uint64_t localWrites;
    uint64_t farReads;
    uint64_t farWrites;

    // Hit/miss/dirty classification (gem5 PolicyManagerStats::numRdHit
    // etc., policy_manager.cc:1463-1528).
    uint64_t numTotHits;
    uint64_t numTotMisses;
    uint64_t numColdMisses; // miss where the old line was invalid
    uint64_t numHotMisses;  // miss where the old line was valid but tag differed
    uint64_t numRdHit;
    uint64_t numWrHit;
    uint64_t numRdMissClean;
    uint64_t numRdMissDirty;
    uint64_t numWrMissClean;
    uint64_t numWrMissDirty;
    uint64_t numWrBacks;    // dirty victims written back (gem5 numWrBacks)
    uint64_t wbInsertions;  // WB deque insert events (same as numWrBacks, kept separately for clarity in tests)
    uint64_t wbDrains;      // WB deque -> farMC dispatch events (should equal wbInsertions once drained)
    uint64_t wbFullRejects; // admissions rejected because WB.size() >= DCM_WB_PRESSURE_THRESHOLD
    // Count of operate() cycles where a WB drain (or a link-latency-held
    // far dispatch) was BLOCKED because farMC's destination queue had no
    // room right now, and the entry was retained rather than lost
    // (docs/wb_retry_audit.md fix). Zero in every scenario that never
    // drives farMC's queue to capacity -- i.e. every pre-existing test
    // this stage; nonzero is the direct, positive signal that the retry
    // path was actually exercised, used by tests/test_dcm_wb_retry.cc.
    uint64_t wbDispatchRetries;
    // Analogous retry counters for the other dispatch paths audited in
    // docs/memory_dispatch_audit.md -- each counts an operate() cycle
    // where an attempted dispatch found its destination full and was
    // retained/retried rather than lost. nearDispatchRetries covers ALL
    // near-side traffic (tag-check reads, direct writes, background fill
    // writes) since they share one retry queue (pendingNearDispatches);
    // farDispatchRetries covers far DEMAND READS and bypass-mode
    // reads/writes specifically (kept separate from wbDispatchRetries,
    // which remains WB-write-back-specific and already tested).
    uint64_t nearDispatchRetries;
    uint64_t farDispatchRetries;
    // Times a dispatch was refused because MEMORY_CONTROLLER::add_rq()
    // would have MERGED it into an identically-addressed entry already
    // resident in the destination read queue instead of enqueuing it
    // (docs/final_independent_audit.md, CRITICAL-1). A merge returns the
    // same "-1-like" success to the caller but enqueues nothing and
    // yields no second completion, so a merged DCM read is a silently
    // lost request. These are retried, never dropped.
    uint64_t dispatchMergeRetries;
    // Tag/metadata installs performed during ChampSim's warmup phase
    // (see warmupTagUpdate()). Deliberately NOT cleared by
    // resetROIStats() -- it is a warmup-phase metric and is the direct
    // evidence that the DRAM cache was actually warmed before the ROI.
    uint64_t warmupTagUpdates;
    uint64_t cacheFills;    // new cache-line installs (== numTotMisses in baseline: every miss installs)

    // Occupancy tracking for ORB/CRB/WB, sampled once per operate() call
    // (drainWB()/recordOccupancySamples()): running max and a sum/count
    // pair so callers can compute the average themselves
    // (sum/samples). Not gem5 stats (gem5 uses statistics::Average, a
    // gem5-internal type) -- a straightforward equivalent for this port.
    uint64_t orbMaxOccupancy, crbMaxOccupancy, wbMaxOccupancy;
    uint64_t orbOccupancySum, crbOccupancySum, wbOccupancySum;
    uint64_t occupancySamples;

    // BEAR-Wr-Opt / Oracle local-tag-check-read-elimination bookkeeping.
    // "Opportunity" = classifyAndInstall() found a write hit (the case
    // BOTH BEAR-Wr-Opt and Oracle exempt); "Applied" = the active policy
    // actually skipped the read for that opportunity; "NotApplicable" =
    // an opportunity existed but the active policy (e.g. baseline) does
    // not exempt it. Tracked regardless of which policy is active, so a
    // baseline run's opportunities-vs-applied gap is itself a check that
    // baseline truly never applies the optimization.
    uint64_t writeHitOptOpportunities;
    uint64_t writeHitOptApplied;
    uint64_t writeHitOptNotApplicable;

    // Oracle-only additional exemption: ANY miss (read or write) whose
    // OLD RESIDENT (victim) line was clean -- or invalid/cold, which the
    // paper's rule does not distinguish from clean. Derived from the
    // paper's Section V wording and Table II, NOT from gem5's literal
    // `isDirty` check, which reads post-update metadata and does not
    // implement this rule (see the full explanation on
    // chooseInitialState() below, and gem5_to_champsim_mapping.md).
    // Same Opportunity/Applied/NotApplicable pattern as write-hit above,
    // tracked independently since it is an Oracle-only exemption
    // (BEAR-Wr-Opt does not get it).
    uint64_t cleanMissOptOpportunities;
    uint64_t cleanMissOptApplied;
    uint64_t cleanMissOptNotApplicable;

    // Oracle-specific named counters (this stage's explicit ask), subsets
    // of the two *Applied counters above, counted only while
    // e->pol == DCM_POLICY_ORACLE.
    uint64_t oracleWriteHits;
    uint64_t oracleCleanMisses;

    uint64_t localTagCheckReadsAvoided; // == writeHitOptApplied + cleanMissOptApplied

    // bypassDcache / "No-DRAM-Cache" comparison mode (paper's Case Study
    // 1/2 alternate configuration; gem5 PolicyManager::bypassDcache).
    // Counted independently of every counter above -- a bypass request
    // never touches ORB/CRB/WB/tag-metadata/near-memory at all, so it
    // must never increment localReads/localWrites/farReads/farWrites/
    // numTotHits/numTotMisses/cacheFills/etc, only these three. See
    // docs/bypass_mode.md.
    uint64_t bypassReads;
    uint64_t bypassWrites;
    uint64_t bypassCompletedReads;

    DCM_STATS()
        : totalRequests(0), readRequests(0), writeRequests(0), sentToNear(0), sentToFar(0),
          completedFromNear(0), completedFromFar(0), completedReadsToLLC(0), completedWrites(0),
          orbFullRejects(0), wqFullSignals(0), crbInserts(0), crbPromotions(0), crbFullRejects(0),
          localReads(0), localWrites(0), farReads(0), farWrites(0), numTotHits(0), numTotMisses(0),
          numColdMisses(0), numHotMisses(0), numRdHit(0), numWrHit(0), numRdMissClean(0),
          numRdMissDirty(0), numWrMissClean(0), numWrMissDirty(0), numWrBacks(0), wbInsertions(0),
          wbDrains(0), wbFullRejects(0), wbDispatchRetries(0), nearDispatchRetries(0), farDispatchRetries(0),
          dispatchMergeRetries(0), warmupTagUpdates(0),
          cacheFills(0), orbMaxOccupancy(0), crbMaxOccupancy(0),
          wbMaxOccupancy(0), orbOccupancySum(0), crbOccupancySum(0), wbOccupancySum(0),
          occupancySamples(0), writeHitOptOpportunities(0), writeHitOptApplied(0),
          writeHitOptNotApplicable(0), cleanMissOptOpportunities(0), cleanMissOptApplied(0),
          cleanMissOptNotApplicable(0), oracleWriteHits(0), oracleCleanMisses(0),
          localTagCheckReadsAvoided(0), bypassReads(0), bypassWrites(0), bypassCompletedReads(0) {}

    // Average sub-operations (local+far, read+write) per outer request --
    // the paper's "access amplification" concept. 0 if no requests yet.
    double accessAmplification() const
    {
        if (totalRequests == 0)
            return 0.0;
        return (double)(localReads + localWrites + farReads + farWrites) / (double)totalRequests;
    }
    double avgOrbOccupancy() const { return occupancySamples ? (double)orbOccupancySum / occupancySamples : 0.0; }
    double avgCrbOccupancy() const { return occupancySamples ? (double)crbOccupancySum / occupancySamples : 0.0; }
    double avgWbOccupancy() const { return occupancySamples ? (double)wbOccupancySum / occupancySamples : 0.0; }
};

// ============================================================================
// DRAM_CACHE_MANAGER
//
// Sits where the memory controller used to sit (LLC's lower_level).
// Owns two independent MEMORY_CONTROLLER instances (near = DRAM cache
// device, far = backing store) and forwards read/write completions from
// either back up to whoever called add_rq/add_wq on this object.
//
// Implements the COMPLETE request path for ALL THREE paper policies
// (CascadeLakeNoPartWrs / BEAR-Wr-Opt / RambusHypo-Oracle): real
// direct-mapped tag/metadata lookup, hit/miss/dirty classification,
// victim identification, dirty write-back, and cache-line installation.
// `policy` selects the transition table, and all three are wired and
// Table-II-verified (24/24 cells exact) -- see the DCM_POLICY enum above
// and chooseInitialState(), which is the only function that differs
// between them. See docs/feature_coverage.md for exactly what is and is
// not implemented.
// ============================================================================
class DRAM_CACHE_MANAGER : public MEMORY {
  public:
    const string NAME;
    int fill_level; // not part of the shared MEMORY base class; each MEMORY
                     // subclass (CACHE, MEMORY_CONTROLLER) declares its own

    MEMORY_CONTROLLER *nearMC; // local/near memory controller (DRAM cache device)
    MEMORY_CONTROLLER *farMC;  // far/backing memory controller

    DCM_POLICY policy;

    // Case Study 3: extra round-trip latency between this manager and
    // farMC, applied on every far-bound dispatch (fetch AND write-back --
    // both cross the same link). 0 (default) means "no link modeled",
    // dispatch is immediate, exactly as before this feature existed.
    // Paper values: 100/500/1000 ns -> use
    // DCM_LINK_LATENCY_CYCLES_{100,500,1000}NS, or setLinkLatency() with
    // any custom value.
    uint64_t linkLatencyCycles;
    void setLinkLatency(uint64_t cycles) { linkLatencyCycles = cycles; }

    // DRAM Cache Manager's own controller frontend/backend latency
    // (paper Table I: "Frontend/Backend Latencies: 20 ns round-trip";
    // gem5: separate `static_frontend_latency`/`static_backend_latency`
    // SimObject params, 10ns each by default -- see the DCM_FRONTEND_/
    // BACKEND_LATENCY_* macros for the exact derivation and why they are
    // NOT combined into one flat constant). Defaults to the paper's
    // configuration (10ns + 10ns) in the constructor; override with
    // setControllerLatency() for deterministic tests (e.g. 0 to disable
    // entirely, or any other value). This is DISTINCT from:
    //   - near-memory DRAM latency: nearMC's own tRP/tRCD/tCAS (set_timing()),
    //   - far-memory DRAM latency: farMC's own tRP/tRCD/tCAS (set_timing()),
    //   - far-link latency: linkLatencyCycles above (Case Study 3) --
    //     applied on the far-bound REQUEST path (dispatchToFar()), not the
    //     response path this controls.
    // Controller latency applies ONLY to the final RESPONSE to the LLC
    // (completeRequest()'s callback), for reads only -- writes never
    // produce a callback in this port at all (gem5_to_champsim_mapping.md
    // fact #2), so there is nothing for controller latency to delay on
    // the write path, matching gem5 exactly (gem5's write-path
    // `accessAndRespond` call schedules a response on a port ChampSim's
    // write contract has no equivalent of).
    uint64_t frontendLatencyCycles;
    uint64_t backendLatencyCycles;
    void setControllerLatency(uint64_t frontendCycles, uint64_t backendCycles)
    {
        frontendLatencyCycles = frontendCycles;
        backendLatencyCycles = backendCycles;
    }

    // bypassDcache: paper's "No-DRAM-Cache" comparison configuration
    // (Case Study 1/2), ported from gem5 PolicyManager's own
    // `bypassDcache` SimObject param (verified directly from
    // policy_manager.cc: it is gem5's FIRST check in recvTimingReq,
    // before any ORB/CRB/classification logic runs at all). Default
    // false (normal DRAM-cache mode, unchanged from every prior stage).
    // When true, add_rq()/add_wq() skip this manager's own bookkeeping
    // entirely -- no ORB entry, no CRB entry, no tag/metadata lookup, no
    // cache fill, no dirty eviction, no BEAR/Oracle optimization
    // decision -- and forward straight to farMC via the SAME
    // dispatchToFar() helper the far-fetch/write-back paths already use,
    // so the configured far-link latency (Case Study 3) still applies
    // (gem5's bypass path crosses the same physical farReqPort/link as
    // the normal path -- only PolicyManager's OWN internal bookkeeping is
    // skipped, not the external link). Controller frontend/backend
    // latency, by contrast, is verified from gem5's `farMemRecvTimingResp`
    // bypass branch (`port.schedTimingResp(pkt, curTick())`, i.e. ZERO
    // added ticks) to NOT apply in bypass mode -- see
    // docs/bypass_mode.md and docs/gem5_to_champsim_mapping.md for the
    // full derivation. Use setBypassDcache() to toggle for deterministic
    // tests.
    bool bypassDcache;
    void setBypassDcache(bool enable) { bypassDcache = enable; }

    // Tracks addresses of bypass-mode reads currently outstanding at
    // farMC, so return_data() can tell a bypass passthrough response
    // (which has no ORB entry at all -- see above) apart from a normal
    // DCM-tracked one, and deliver it straight to the LLC instead of
    // attempting an ORB lookup. A multiset (not a set) because bypass
    // mode deliberately performs NO conflict tracking of its own -- two
    // outstanding bypass reads to the same address at once are valid and
    // each must be accounted for and delivered independently, matching
    // gem5 (bypass skips checkConflictInDramCache entirely, so nothing
    // there prevents it either). Writes never need an entry here: this
    // port's write contract never produces an LLC-visible callback for a
    // write in ANY mode (gem5_to_champsim_mapping.md fact #2), bypass
    // included.
    std::multiset<uint64_t> bypassOutstandingReads;

    // LEGACY test hooks: no longer read by the live request path. Real
    // hit/miss/dirty is now derived from tagMetadataStore
    // (classifyAndInstall()). Left declared, inert, exactly the way gem5
    // keeps `alwaysHit`/`alwaysDirty` as commented-out debug knobs rather
    // than deleting them (policy_manager.hh:1473,1537) -- kept in case a
    // future test wants to force a path without needing real tag setup.
    bool debugForceHit;
    bool debugForceDirty;
    bool debugPrint;

    uint64_t nextRequestId;

    // The DRAM cache's tag/metadata array: one DCM_TAG_ENTRY per cache
    // line, sized DCM_NUM_LINES in the constructor so that
    // DCM_NUM_LINES * DCM_BLOCK_SIZE == DCM_DRAM_CACHE_SIZE exactly.
    // Read and written on every request: classifyAndInstall() consults
    // tagMetadataStore[indexDC] for hit/miss/dirty classification and
    // victim identification (dram_cache_manager.cc:551), and
    // warmupTagUpdate() maintains the same state during warmup with no
    // timing or ROI-statistic side effects (dram_cache_manager.cc:132).
    // Metadata only -- no data is modelled, matching gem5.
    std::vector<DCM_TAG_ENTRY> tagMetadataStore;

    std::map<uint64_t, DCM_ORB_ENTRY *> ORB;
    std::vector<DCM_CRB_ENTRY> CRB; // conflict queue, keyed implicitly by DRAM-cache index -- see below
    std::deque<DCM_WB_ENTRY> WB;    // dirty write-backs awaiting drain to farMC (see drainWB())
    std::deque<DCM_PENDING_FAR_DISPATCH> pendingFarDispatches; // link-latency holding area (see dispatchToFar())
    std::deque<DCM_PENDING_NEAR_DISPATCH> pendingNearDispatches; // near-capacity retry queue (see dispatchToNear())
    std::deque<DCM_PENDING_RESPONSE> pendingResponses; // controller-latency holding area (see completeRequest())

    DCM_STATS stats;

    DRAM_CACHE_MANAGER(string v1, MEMORY_CONTROLLER *near, MEMORY_CONTROLLER *far);
    ~DRAM_CACHE_MANAGER();

    // Clears every ROI statistic while leaving ARCHITECTURAL state
    // (tagMetadataStore, ORB, CRB, WB, pending dispatch queues) fully
    // intact. Called once by main.cc at the warmup->ROI boundary, in the
    // same place ChampSim already calls reset_cache_stats() for the
    // caches, so DCM statistics measure the ROI only while the DRAM
    // cache stays warmed (docs/final_independent_audit.md, HIGH-2).
    // stats.warmupTagUpdates is deliberately preserved -- it describes
    // the warmup phase, not the ROI, and is the evidence that warming
    // actually happened.
    void resetROIStats();

    // Installs/updates ONLY the tag+metadata state for a request seen
    // during ChampSim's warmup phase, using byte-for-byte the same
    // update rules as classifyAndInstall() (read hit keeps the dirty
    // bit, read miss installs clean, any write dirties the line). No
    // ORB/CRB/WB entry is created, no DRAM timing is simulated, and no
    // ROI statistic is touched -- ChampSim's warmup contract returns
    // memory data immediately and this preserves that exactly, while
    // still giving the ROI a warmed DRAM cache as the paper's
    // methodology requires (paper Section V: "we made sure that the
    // DRAM cache had been warmed-up, so cold misses are not
    // contributing to the performance observed").
    void warmupTagUpdate(PACKET *packet, bool isWrite);


    // MEMORY interface (pure virtuals in memory_class.h)
    int add_rq(PACKET *packet);
    int add_wq(PACKET *packet);
    int add_pq(PACKET *packet);
    void return_data(PACKET *packet);
    void operate();
    void increment_WQ_FULL(uint64_t address);
    uint32_t get_occupancy(uint8_t queue_type, uint64_t address);
    uint32_t get_size(uint8_t queue_type, uint64_t address);

    // The direct-mapped mapping, exposed so tests can assert index/tag
    // for a given address without duplicating the formula (in particular
    // the direct-injection vs. real-LLC equivalence test). Input is a
    // ChampSim cache-line address -- see the contract block above
    // returnIndexDC()'s definition in src/dram_cache_manager.cc.
    uint64_t returnIndexDC(uint64_t address);
    uint64_t returnTagDC(uint64_t address);

  private:

    // True if some entry already in the ORB occupies DRAM-cache index
    // indexDC (gem5 checkConflictInDramCache -- conflict is checked only
    // against the ORB, since the ORB entry is the one actually "holding"
    // that direct-mapped cache location; anything else waiting for the
    // same index is already queued in the CRB behind it).
    bool checkConflictInORB(uint64_t indexDC);

    // admittedArrivalCycle: pass the CRB entry's original arrival cycle
    // when promoting out of the CRB, so wait time is measured from when
    // the request actually showed up, not from the promotion cycle
    // (mirrors gem5 resumeConflictingReq's
    // `ORB.at(confAddr)->arrivalTick = entry.first;`). Pass UINT64_MAX
    // (the default) for a fresh admission to use "now".
    void admitRequest(PACKET *packet, bool isWrite, uint64_t admittedArrivalCycle = (uint64_t)-1);

    // Real hit/miss/dirty/victim classification + eager metadata install,
    // ported from gem5 handleRequestorPkt's inline logic (checkHitOrMiss +
    // the dirty-victim capture + the "Updating Tag & Metadata" block,
    // policy_manager.cc:1419-1443). Called once per admission, BEFORE the
    // tag-check read is even dispatched -- gem5 updates the logical tag
    // store eagerly at admission time, decoupled from the physical DRAM
    // timing simulated later. See docs/gem5_to_champsim_mapping.md.
    void classifyAndInstall(DCM_ORB_ENTRY *e);

    // Ports gem5 setNextState's per-policy `start` transition
    // (policy_manager.cc:686-711 baseline, :897-911 BEAR-Wr-Opt). Called
    // once per admission, right after classifyAndInstall() has already
    // determined e->isHit -- this function ONLY decides which state to
    // enter FIRST; every state AFTER that (waitingLocMemReadResp,
    // farMemRead, locMemWrite, waitingLocMemWriteResp, ...) is handled by
    // the shared driveState()/return_data() machinery, identical for
    // every policy (gem5's handleNextState is likewise byte-for-byte
    // identical across CascadeLakeNoPartWrs/BearWriteOpt for every state
    // except this one `start` decision -- verified by direct comparison
    // of policy_manager.cc's baseline vs BearWriteOpt blocks).
    //
    //   - Baseline: ALWAYS DCM_LOC_MEM_READ (the tag-check read is never
    //     skipped -- this is the whole point of "baseline").
    //   - BEAR-Wr-Opt: DCM_LOC_MEM_WRITE directly, SKIPPING
    //     DCM_LOC_MEM_READ, if and only if this request is a WRITE that
    //     classifyAndInstall() already determined to be a HIT. Every
    //     other case (read hit, read miss, write miss of any kind)
    //     enters DCM_LOC_MEM_READ exactly like baseline -- the paper is
    //     explicit that BEAR-Wr-Opt exempts write hits ONLY, never reads
    //     and never misses, and gem5's own condition
    //     (`!(isWrite() && isHit)` -> locMemRead) encodes exactly that.
    //   - Oracle (paper Section V's "Oracle"; gem5's nearest analogue is
    //     enums::RambusHypo, policy_manager.cc:786-810): exempts write
    //     hits (same as BEAR-Wr-Opt) AND any miss -- read or write --
    //     whose OLD RESIDENT (victim) line was NOT dirty, i.e.
    //     clean-valid OR invalid/cold (the paper does not distinguish
    //     those two either). A READ hit is NOT exempted: the read
    //     fetches the actual DATA, not just the tag, and Oracle's
    //     zero-latency SRAM only eliminates the *tag check*. A DIRTY
    //     miss (read or write) is NOT exempted either: there the local
    //     read is what *sources the dirty victim's data* for its
    //     write-back, which an SRAM tag store cannot substitute for.
    //
    //     SOURCE OF TRUTH -- IMPORTANT, AND DELIBERATELY NOT gem5's
    //     LITERAL CODE. This implementation follows the PAPER, and is
    //     verified cell-by-cell against the paper's published Table II
    //     (24/24 exact across all three policies -- see
    //     docs/validation.md). The paper states the rule explicitly in
    //     Section V: Oracle avoids the tag-check read "not only for
    //     write hit demands, but also if the demand access (either read
    //     or write) will miss on DRAM cache and the cache line is
    //     clean", and Table II's "Local Read" row confirms Oracle omits
    //     the local read for exactly RdMissClean, WrHit(dirty+clean)
    //     and WrMissClean.
    //
    //     gem5's RambusHypo block does NOT actually implement that rule,
    //     and an earlier version of this comment wrongly claimed it did.
    //     gem5 computes `isDirty = checkDirty(owPkt->getAddr())` at the
    //     TOP of setNextState (policy_manager.cc:683), but
    //     recvTimingReq calls handleRequestorPkt() at line 366 -- which
    //     EAGERLY OVERWRITES the tag metadata at lines 1426-1443 -- and
    //     only then calls setNextState() at line 376. So gem5's
    //     `isDirty` reads POST-update state, not the victim's state:
    //     after the update a read miss always has dirtyLine==false and
    //     any write always has dirtyLine==true. Consequences inside
    //     gem5: two of its own labelled branches
    //     (`isRead && !isHit && isDirty`, `!isRead && !isHit &&
    //     !isDirty`) are unreachable; its RdMissDirty costs 2 accesses
    //     instead of Table II's 4; its WrMissClean costs 2 instead of 1;
    //     and a dirty victim evicted by a RambusHypo read miss is never
    //     written back at all, because handleDirtyCacheLine() is only
    //     ever called from locMemRecvTimingResp (line 536) and that path
    //     is skipped. Reproducing gem5 literally would therefore BREAK
    //     Table II conformance, so this port intentionally implements
    //     the paper instead. Do not "fix" this to match gem5.
    //
    //     Every downstream transition after chooseInitialState()
    //     (waitingLocMemReadResp, waitingFarMemReadResp,
    //     waitingLocMemWriteResp) is IDENTICAL for Oracle to
    //     baseline/BEAR-Wr-Opt, so this port's already-shared
    //     driveState()/return_data() need NO Oracle-specific branches at
    //     all; only this one function does.
    void chooseInitialState(DCM_ORB_ENTRY *e);

    // Pushes the old (evicted) dirty line to the WB deque and drains it to
    // farMC. Ported from gem5 handleDirtyCacheLine, called at the same
    // point gem5 calls it: right when the LOCAL tag-check read completes
    // (policy_manager.cc:521-538), not at admission time -- the tag-check
    // read is what "sources" the victim's data in the real hardware this
    // models (tag+data co-located in ECC bits).
    void pushDirtyWriteBack(DCM_ORB_ENTRY *e);

    // Sends pkt to farMC (add_rq if !isWrite, add_wq if isWrite).
    //
    // Returns true if the packet was HANDED OFF successfully -- either
    // dispatched to farMC right now (linkLatencyCycles == 0 and farMC had
    // room), or safely enqueued into pendingFarDispatches to await the
    // link-latency hold (linkLatencyCycles > 0; the real capacity check
    // for THIS case happens later, at release time, in
    // processPendingFarDispatches() -- see its own comment). Returns
    // false if linkLatencyCycles == 0 and farMC's destination queue
    // (RQ for a read, WQ for a write) has NO room right now -- in this
    // case NOTHING is sent and NOTHING is queued; the packet was never
    // touched, so the caller retains full ownership and MUST NOT assume
    // any effect happened.
    //
    // This return value exists specifically so drainWB() can tell
    // "dispatched" apart from "rejected, retry me" (docs/wb_retry_audit.md):
    // MEMORY_CONTROLLER::add_rq()/add_wq() (dram_controller.cc) have NO
    // bounds check of their own and will SILENTLY DISCARD a packet with
    // no signal at all if their queue is genuinely full (verified
    // directly, not assumed) -- checking farMC->get_occupancy() against
    // farMC->get_size() BEFORE ever calling add_rq()/add_wq() is what
    // avoids ever reaching that buggy code path in the first place.
    // drainWB() is the ONLY caller that uses this bool return directly
    // (it manages its own front-of-WB-deque retry). Every OTHER far
    // dispatch call site (driveState()'s DCM_FAR_MEM_READ case, bypass
    // mode's add_rq()/add_wq()) goes through dispatchToFarGuaranteed()
    // instead (docs/memory_dispatch_audit.md), which wraps this function
    // and queues a failed immediate attempt into pendingFarDispatches
    // itself, so those call sites need no retry logic of their own.
    bool dispatchToFar(const PACKET &pkt, bool isWrite);

    // Wraps dispatchToFar() for call sites with no retry loop of their
    // own (driveState()'s DCM_FAR_MEM_READ case; bypass mode's
    // add_rq()/add_wq()) -- GUARANTEES eventual delivery: if the
    // immediate attempt fails (farMC has no room and linkLatencyCycles
    // == 0, so dispatchToFar()'s own link-latency queuing branch didn't
    // run), the packet is pushed onto the SAME pendingFarDispatches
    // queue processPendingFarDispatches() already drains every cycle,
    // with its target cycle set to "now" so it is retried starting next
    // cycle, exactly like a link-latency-delayed entry that has just
    // become ready (docs/memory_dispatch_audit.md). Unlike
    // dispatchToFar(), this has no bool return: by design, from this
    // function's caller's perspective the packet is ALWAYS eventually
    // sent, never lost -- there is nothing left for the caller to do.
    void dispatchToFarGuaranteed(const PACKET &pkt, bool isWrite);

    // Called once per operate() cycle: dispatches any pendingFarDispatches
    // entries whose link-latency hold has elapsed. Peeks the front entry
    // and checks farMC's REAL capacity (get_occupancy() vs get_size())
    // BEFORE popping it -- if farMC has no room, the entry is left at the
    // front and the loop stops immediately (docs/wb_retry_audit.md),
    // preserving FIFO order: a later-queued entry must never be released
    // ahead of an earlier one that is merely capacity-blocked.
    void processPendingFarDispatches();

    // Sends pkt to nearMC (add_rq if !isWrite, add_wq if isWrite).
    // Near-memory dispatch has no Case-Study-3 link-latency concept (that
    // is a far-only, physical-link idea) and no existing caller manages
    // its own near-side retry the way drainWB() does for WB/far -- so
    // this function GUARANTEES eventual delivery directly, the same way
    // dispatchToFarGuaranteed() does for its call sites: on an immediate
    // capacity failure, the packet is queued into pendingNearDispatches
    // for processPendingNearDispatches() to keep retrying every cycle
    // until nearMC has room. Used by ALL near-memory dispatch call sites
    // (driveState()'s DCM_LOC_MEM_READ/DCM_LOC_MEM_WRITE cases,
    // return_data()'s background fill write) -- see
    // docs/memory_dispatch_audit.md.
    void dispatchToNear(const PACKET &pkt, bool isWrite);

    // Called once per operate() cycle: dispatches any pendingNearDispatches
    // entries. Peeks the front entry and checks nearMC's REAL capacity
    // (get_occupancy() vs get_size()) BEFORE popping it -- if nearMC has
    // no room, the entry is left at the front and the loop stops
    // immediately, preserving FIFO order, mirroring
    // processPendingFarDispatches()'s exact discipline.
    void processPendingNearDispatches();

    // Shared low-level primitive underlying dispatchToFar()'s immediate
    // branch and dispatchToNear(): checks mc's real capacity
    // (get_occupancy() vs get_size()) for the given queue (WQ if
    // isWrite, else RQ), and if room exists, stamps pkt's event_cycle to
    // "now" (MEMORY_CONTROLLER::update_schedule_cycle() requires a
    // fresh, non-stale value -- PACKET's default constructor leaves it
    // at UINT64_MAX, which can never be scheduled) and calls
    // add_wq()/add_rq(). Returns whether the send happened. Takes pkt by
    // value (not reference) since it is mutated (the event_cycle stamp)
    // before being handed to mc.
    bool trySend(MEMORY_CONTROLLER *mc, PACKET pkt, bool isWrite);

    // Drains at most one WB entry per operate() cycle to farMC via
    // dispatchToFar() (gem5's one-send-per-scheduled-event pattern,
    // policy_manager.cc:482-519 processFarMemWriteEvent). Draining at a
    // bounded rate (not "all at once") is what allows the WB deque to
    // genuinely hold a backlog -- see DCM_WB_PRESSURE_THRESHOLD.
    //
    // The WB.front() entry is popped ONLY if dispatchToFar() reports it
    // was actually handed off (docs/wb_retry_audit.md) -- if farMC has no
    // room right now, the entry is left in WB and this function simply
    // returns without popping anything, to be retried on a later
    // operate() cycle. This is the fix for the audit's confirmed
    // silent-write-loss bug: WB entries are never removed until their
    // far-memory write is confirmed accepted, mirroring gem5's own
    // invariant (`pktFarMemWrite` is only popped on
    // `farReqPort.sendTimingReq()` returning true) via ChampSim's
    // synchronous per-cycle polling instead of gem5's async port-retry
    // callback -- see the .cc implementation for the full comparison.
    void drainWB();

    // Samples current ORB/CRB/WB occupancy into the running max/sum
    // stats. Called once per operate() cycle.
    void recordOccupancySamples();

    // Holds pkt for (frontendLatencyCycles + backendLatencyCycles) before
    // delivering it to the LLC via upper_level_icache/dcache[cpu]->return_data().
    // If both are 0, delivers immediately (pre-existing behavior,
    // unchanged). Mirrors gem5's accessAndRespond()/port.schedTimingResp()
    // decoupling: the CALLER (completeRequest()) has already retired the
    // ORB entry and checked CRB promotion by the time this is invoked --
    // only the LLC-visible delivery is delayed, nothing else.
    void scheduleResponse(const PACKET &pkt, uint64_t delayCycles);

    // Called once per operate() cycle: delivers any pendingResponses
    // entries whose controller-latency hold has elapsed.
    void processPendingResponses();

    void driveState(DCM_ORB_ENTRY *e);

    // responseLatencyCycles is ONLY used when e is a read (writes never
    // produce a callback, see gem5_to_champsim_mapping.md fact #2) --
    // pass frontendLatencyCycles+backendLatencyCycles for a response
    // that did not need a far fetch, or
    // frontendLatencyCycles+2*backendLatencyCycles for one that did
    // (gem5's exact distinction, policy_manager.cc:734-735 vs :1042-1043
    // -- see the DCM_FRONTEND_/BACKEND_LATENCY_* macro comment in the
    // header for why these are NOT the same value).
    void completeRequest(DCM_ORB_ENTRY *e, uint64_t responseLatencyCycles = 0);

    // Called when an ORB entry for DRAM-cache index indexDC has just been
    // freed. Finds the oldest CRB entry (if any) waiting on that same
    // index, removes it from the CRB, and admits it into the ORB (gem5
    // resumeConflictingReq). At most one entry is promoted per call --
    // if more are queued behind it for the same index, they stay in the
    // CRB until this newly-promoted entry itself completes.
    void promoteFromCRB(uint64_t indexDC);
};

#endif
