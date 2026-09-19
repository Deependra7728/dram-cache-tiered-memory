# Implementation Status

## Stage 1: Integration skeleton — DONE
## Stage 2: per-instance timing + ORB/CRB conflict handling — DONE
## Stage 3: real tag/metadata + complete baseline behavior — DONE

See prior versions of this file (or `gem5_to_champsim_mapping.md`'s
per-fact citations) for full detail; not repeated here.

## Stage 4: WB backpressure, near/far config, link latency, statistics — DONE, verified

### 1. WB occupancy / admission backpressure

Ported gem5's exact mechanism (`policy_manager.cc:332`,
`pktFarMemWrite.size() >= orbMaxSize/2`) as
`DCM_WB_PRESSURE_THRESHOLD = DCM_ORB_MAX_SIZE/2 = 64`. Admission order in
`add_rq`/`add_wq` is now exactly gem5's: conflict-by-index check (→ CRB)
→ WB-pressure check (→ retry) → ORB-full check (→ retry) → admit.
`get_occupancy`/`get_size` updated to report the same decision. Draining
(`drainWB()`) is now bounded to one entry per `operate()` cycle (was:
drained synchronously and immediately inside `pushDirtyWriteBack()`, so
the WB deque could never actually hold a backlog) — this is what makes
the threshold meaningful and testable.

**Exact threshold**: 64 (`DCM_ORB_MAX_SIZE/2`). Verified this is gem5's
*actual* mechanism by re-reading `recvTimingReq` directly
(`policy_manager.cc:296-362`), not assumed from the paper's separately-
stated "WB Buffer = 64 entries" (Table I) — the two numbers coincide, but
for different reasons (see the header comment in
`inc/dram_cache_manager.h` and `gem5_to_champsim_mapping.md`).

### 2. Near/far memory configuration

`configureNearAsHBM2()`/`configureFarAsDDR4()` (free functions,
`dram_cache_manager.{h,cc}`) apply the paper's own declared peak
bandwidths: near = 4000 MT/s (32 GB/s), far = 2400 MT/s (19.2 GB/s) —
both exact, derived from Table I via ChampSim's
`BW(GB/s) = DRAM_CHANNEL_WIDTH(8B) * MTPS / 1000`. Wired into `main.cc`
(replacing the previous "both identical" placeholder), with the
pre-existing `--low_bandwidth` knob's effect (quarter the rate) preserved
on top. tRP/tRCD/tCAS are NOT differentiated — see "Discrepancies /
limitations" below; this is a documented, explained gap, not an
oversight.

**A real ChampSim bug was found and fixed**: `DRAM_DBUS_RETURN_TIME`'s
original formula truncates `CPU_FREQ/DRAM_MTPS` via integer division,
which collapses every MTPS in [2001,4000] to the same result at
`CPU_FREQ=4000` — meaning near (4000) and far (2400) would have produced
*identical* dbus timing (8 cycles) despite the intended ~1.67x bandwidth
difference, and even ChampSim's own pre-existing single-DRAM default
(3200 MT/s) was already silently ~20% off (8 instead of the correct 10).
Fixed via `computeDbusReturnTime()` (floating point + round-to-nearest),
applied in the two new configure functions and in `main.cc`'s
`--low_bandwidth` adjustment. Verified: near=8, far=13 cycles
post-fix — genuinely different, as intended.

### 3. Link latency (Case Study 3)

`DRAM_CACHE_MANAGER::linkLatencyCycles` (default 0) + `setLinkLatency()`.
Applied via a new `dispatchToFar(pkt, isWrite)` helper used by BOTH the
far-read fetch (`driveState`'s `DCM_FAR_MEM_READ` case) and the dirty
write-back (`drainWB()`) — both physically cross the same link in the
paper's model. When `linkLatencyCycles > 0`, the packet is held in a new
`pendingFarDispatches` deque until `current_core_cycle + linkLatencyCycles`,
released by `processPendingFarDispatches()` (called once per `operate()`,
before `nearMC`/`farMC` step). When `linkLatencyCycles == 0` (the default
— unset in `main.cc`, so the full binary's behavior is unaffected by this
feature's mere existence), dispatch is immediate, byte-for-byte the same
as before this feature existed.

**Exact values**: `DCM_LINK_LATENCY_CYCLES_100NS/500NS/1000NS`, the
paper's three evaluated values, converted to cycles at `CPU_FREQ`.

**Verification precision**: measured completion-time deltas for
100→500→1000ns matched hand-derived expected deltas (400, 1600, 2000
cycles) **exactly**, not just "within tolerance" — the mechanism applies
the latency exactly once, with no double-counting or omission.

### 4. Statistics

Added: `localReads/localWrites/farReads/farWrites` (already present from
stage 3, confirmed complete against this stage's checklist);
`wbDrains`, `wbFullRejects`, `cacheFills`; occupancy tracking
(`orbMaxOccupancy/crbMaxOccupancy/wbMaxOccupancy`,
`orbOccupancySum/crbOccupancySum/wbOccupancySum` + `occupancySamples`,
sampled once per `operate()` cycle via `recordOccupancySamples()`);
`DCM_STATS::accessAmplification()`,
`avgOrbOccupancy()`/`avgCrbOccupancy()`/`avgWbOccupancy()` (computed
methods, not stored fields). Full checklist cross-reference in
`feature_coverage.md`'s "Statistics coverage" table — every item from
this stage's instructions has a named field and a verifying test.

Stats independence (not accidentally cumulative across scenarios)
verified explicitly: `tests/test_dcm_stats.cc` Test 2 constructs two
independent `DRAM_CACHE_MANAGER` instances and confirms zero leakage
(each `stats` is a plain per-instance member, no static/global state
anywhere in `DRAM_CACHE_MANAGER` or `DCM_STATS`).

### Files modified

- `inc/dram_cache_manager.h` — `DCM_WB_PRESSURE_THRESHOLD`,
  `DCM_NEAR_HBM2_MTPS`/`DCM_FAR_DDR4_MTPS`, `DCM_PENDING_FAR_DISPATCH`
  struct, `linkLatencyCycles`/`setLinkLatency()`, new stats fields +
  `accessAmplification()`/`avg*Occupancy()` methods,
  `configureNearAsHBM2`/`configureFarAsDDR4`/`computeDbusReturnTime` free
  function declarations, new private methods (`dispatchToFar`,
  `processPendingFarDispatches`, `drainWB`, `recordOccupancySamples`).
- `src/dram_cache_manager.cc` — WB-pressure check in `add_rq`/`add_wq`;
  `get_occupancy`/`get_size` WB branch; `operate()` now calls
  `processPendingFarDispatches()`/`drainWB()`/`recordOccupancySamples()`;
  `pushDirtyWriteBack()` no longer drains synchronously; new
  `dispatchToFar`/`processPendingFarDispatches`/`drainWB`/
  `recordOccupancySamples`/`configureNearAsHBM2`/`configureFarAsDDR4`/
  `computeDbusReturnTime` implementations; `driveState`'s
  `DCM_FAR_MEM_READ` case now calls `dispatchToFar` instead of
  `farMC->add_rq` directly; `classifyAndInstall` increments `cacheFills`.
- `src/main.cc` — timing setup now calls `configureNearAsHBM2`/
  `configureFarAsDDR4` instead of computing one set of identical values;
  `--low_bandwidth` adjustment uses `computeDbusReturnTime()`; DRAM-info
  print messages split into near/far lines.
- `tests/test_dcm_wb_pressure.cc`, `tests/test_dcm_near_far_config.cc`,
  `tests/test_dcm_link_latency.cc`, `tests/test_dcm_stats.cc` — **new
  files**, 5+3+4+3 = 15 new test blocks.
- `docs/gem5_to_champsim_mapping.md`, `docs/feature_coverage.md`,
  `docs/validation.md`, `docs/limitations.md` — updated (see those files).

### Build result

`make clean && make` succeeds, zero new warnings.

### Tests performed (this stage)

All 15 new test blocks across 4 new files pass:
`test_dcm_wb_pressure.cc` (5/5), `test_dcm_near_far_config.cc` (3/3),
`test_dcm_link_latency.cc` (4/4), `test_dcm_stats.cc` (3/3). Two real
bugs were found and fixed while writing these tests (both documented
above and in `limitations.md`): the `DRAM_DBUS_RETURN_TIME`
integer-truncation precision bug, and a test-measurement race with
ChampSim's pre-existing write-queue-forwarding shortcut. Neither was a
bug in the DCM's own logic.

### Task 5: baseline regression — full pass

Re-ran the complete existing suite after all changes: `test_dcm_skeleton.cc`
(6/6), `test_dcm_baseline.cc` (10/10, all 8 Table-II cases + repeated
access + replacement chain still pass unchanged), full binary on two
real traces (no crash/hang). Baseline state-machine behavior itself was
NOT touched this stage — all changes were additive (new checks inserted
at admission-order boundaries already established in stage 2/3, new
helper functions) or confined to files this port owns exclusively.

### Discrepancies with gem5 found this stage

None in the ported mechanisms themselves (WB threshold, link latency
application point, admission ordering) — all matched gem5's actual code
on inspection. Two real *ChampSim* (not gem5-vs-port) issues were found
and are documented in full in `limitations.md`:
1. `DRAM_DBUS_RETURN_TIME` integer-truncation bug (pre-existing in
   ChampSim, fixed in this port's new configuration functions).
2. Write-queue-forwarding shortcut interacting with timing measurements
   taken too soon after a fill (pre-existing ChampSim behavior, not a
   bug — but flagged since it can silently distort timing measurements
   for anyone, DCM-related or not).

### Remaining differences from the paper (explicitly not silently approximated)

- tRP/tRCD/tCAS are not differentiated between HBM2 near and DDR4 far.
  **Why not implementable faithfully right now**: gem5's real
  per-technology latencies live inside `DRAMInterface` SimObjects
  (`HBM_2000_4H_1x64`, `DDR4_2400_16x4`) not disclosed as explicit
  nanosecond values in the paper's text. Only the two peak-bandwidth
  numbers (32 GB/s, 19.2 GB/s) are given explicitly, and those ARE
  reproduced exactly. See `limitations.md` for the full reasoning.
- `DRAM_CHANNEL_WIDTH` (bus width) is a single global macro in this
  ChampSim tree, not per-controller — near/far can only differ in
  transfer rate, not width, in this port's bandwidth model.
- An NVM far-memory profile (the paper's Case Study 3 varies far
  technology as well as link latency) is not implemented — not requested
  this stage.

### Remaining unimplemented functionality (see next_task.md)

- ~~BEAR-Wr-Opt~~ — DONE, see Stage 5 below.
- Oracle policy.
- Controller frontend/backend latency (`DCM_CTRL_LATENCY_CYCLES`,
  defined, unapplied).
- WB→far dispatch retry-on-nack modeling (currently fire-and-forget).

## Stage 5: BEAR-Wr-Opt policy — DONE, verified

### What was implemented

Re-read the actual gem5 `enums::BearWriteOpt` branches directly from the
pulled `policy_manager.cc` (`setNextState`: lines 897-1007;
`handleNextState`: lines 1234-1341) before writing any code, per this
stage's explicit instruction to determine "the exact metadata behavior
from the actual gem5 BearWriteOpt implementation" rather than from the
papers' prose alone. Key finding: gem5's BEAR does NOT implement a
separate LLC-side metadata mechanism (despite the paper's prose saying
"BEAR determines write hit accesses using the metadata stored in the
last level cache") — the actual code reuses `orbEntry->isHit`, the SAME
value computed by the SAME `checkHitOrMiss()` every policy shares at
admission. The only place BEAR's code differs from baseline's is the
`start`-state decision in `setNextState`: `if (isWrite() && isHit) ->
locMemWrite` directly, else `-> locMemRead` (identical to baseline's
unconditional `-> locMemRead`). `handleNextState` for BEAR
(lines 1234-1341) is a byte-for-byte textual copy of baseline's.

Ported as:
- **`DRAM_CACHE_MANAGER::chooseInitialState(DCM_ORB_ENTRY *e)`** (new):
  called once per admission, immediately after `classifyAndInstall()`
  (unchanged, policy-independent, shared by every policy exactly as in
  gem5). If `e->pol == DCM_POLICY_BEAR_WR_OPT && e->isWriteReq &&
  e->isHit`, sets `e->state = DCM_LOC_MEM_WRITE` directly (skipping
  `DCM_LOC_MEM_READ` and its wait entirely). Every other combination
  falls through to `e->state = DCM_LOC_MEM_READ`, identical to baseline.
- No changes to `driveState()` or `return_data()` — since this port
  already has ONE shared state-transition implementation (not gem5's
  per-policy-duplicated `handleNextState`), reusing it for every
  post-`start` state was already correct by construction; no new
  branches were needed there. This is a deliberate, documented
  structural simplification vs. gem5's textual duplication — verified
  behaviorally equivalent, not just assumed, via a baseline-vs-BEAR
  request-flow diff test (Test 14).
- `chooseInitialState()` also updates the new BEAR-specific stats
  (`writeHitOptOpportunities`/`writeHitOptApplied`/
  `writeHitOptNotApplicable`/`localTagCheckReadsAvoided`), tracked
  regardless of active policy so a baseline run's
  opportunities-vs-applied gap is itself evidence baseline never applies
  the optimization.

**Baseline behavior is provably unchanged**: `chooseInitialState()` is
the only new code in the admission path, and for
`DCM_POLICY_BASELINE_CASCADE_LAKE` it always sets `DCM_LOC_MEM_READ` —
identical to the unconditional assignment it replaced. Verified: the
full binary produces byte-identical cycle counts on both test traces
before and after this stage (26174 / 9503 cycles, unchanged), and all 31
pre-existing test blocks still pass unmodified.

### Files modified

- `inc/dram_cache_manager.h` — `DCM_POLICY` enum comment updated;
  `chooseInitialState()` declared (with a detailed banner explaining
  exactly what it does and does not change, referencing the specific
  gem5 line ranges); four new `DCM_STATS` fields.
- `src/dram_cache_manager.cc` — `chooseInitialState()` implemented;
  `admitRequest()` now calls it instead of unconditionally assigning
  `DCM_LOC_MEM_READ`.
- `tests/test_dcm_bear.cc` — **new file**, 14 test blocks.
- `docs/gem5_to_champsim_mapping.md`, `docs/feature_coverage.md`,
  `docs/validation.md`, `docs/request_flow.md` — updated (see those
  files; `docs/validation.md` carries the full gem5-comparison table
  this stage's instructions required).

### Build result

`make clean && make` succeeds, zero new warnings.

### Tests performed

`tests/test_dcm_bear.cc`: **14/14 PASS** — write hit (Table-II-defining
case: local_read=0), write miss × {cold, clean victim, dirty victim}
(read NOT skipped in any of these), read hit, read miss × {clean, dirty
victim} (both unchanged from baseline), metadata after fill/replacement/
dirty-eviction, a CRB-promoted request resolving into a genuine BEAR
write hit, ORB/CRB admission ordering unaffected by policy, WB
interaction (a hit never inserts into WB; WB pressure still blocks
admission before the optimization ever runs), and a direct baseline-vs-
BEAR request-flow diff over an identical scenario (differs by exactly
one `local_read`, nothing else).

Full regression: all 6 prior test binaries re-run, **31/31 still pass**;
full binary on 2 real traces, byte-identical cycle counts to before this
stage.

One test-design bug was found and fixed while writing Test 11 (not a DCM
bug): see `validation.md` for the full explanation (a BEAR write hit
completes synchronously, so an initial test design assumed a
"still-outstanding" window that cannot exist; fixed by testing a request
that becomes a write hit via CRB promotion instead).

### Table II validation

All 6 applicable BEAR-Wr-Opt cases (Table II's `Tot. BEAR-Wr-Opt` row
`1 1 4 3 1 1 3 2`) verified with exact per-operation-type counts — see
`validation.md`. The two write-hit columns are the only ones that differ
from baseline (2→1); all others match baseline exactly, as asserted
explicitly in Tests 2-7.

### gem5 comparison

Full scenario-by-scenario comparison table in `validation.md`. No
behavioral discrepancy found between gem5's `BearWriteOpt` and this port.

### Exact behavior difference from baseline

**Exactly one case differs**: a WRITE request that `classifyAndInstall()`
classifies as a HIT skips the local tag-check read (`DCM_LOC_MEM_READ`)
entirely, going straight to the local write. Every other case — read hit,
read miss (clean or dirty victim), write miss (clean or dirty victim) —
is byte-for-byte identical to baseline, verified both by exact Table-II
operation counts and by a direct request-flow-log diff.

### Remaining unimplemented functionality (as of Stage 5)

- Oracle policy (`DCM_POLICY_ORACLE` exists as an enum value only,
  currently falls through to baseline behavior). **Implemented in Stage
  6, below.**
- Controller frontend/backend latency, WB retry-on-nack modeling (both
  carried over from before this stage, unrelated to BEAR).

## Stage 6: Oracle policy — DONE, verified

### What was implemented

Re-read the actual gem5 `enums::RambusHypo` branches directly from
`policy_manager.cc` (`setNextState`: lines 786-893) before writing any
code, per this stage's explicit instruction to determine behavior from
the actual implementation rather than inferring it from the policy's
name. Key finding, confirmed by the code rather than assumed: Oracle's
"zero-latency SRAM tag store" does not eliminate the local read for
*every* miss or for read hits — it eliminates it for exactly two things:
(1) write hits (same exemption as BEAR-Wr-Opt) and (2) any miss, read or
write, whose OLD resident (victim) line was NOT dirty — the paper's rule
does not distinguish a genuinely clean-valid victim from a cold/invalid
line; both get the exemption. (NOTE: gem5's literal `isDirty` reads
POST-update metadata and does NOT implement this rule — see
`inc/dram_cache_manager.h`'s `chooseInitialState()` comment. This port
follows the paper, verified against Table II.) A
READ HIT is never exempted (the read fetches actual data, not just a
tag) and a DIRTY miss is never exempted (the read is what sources the
victim's data for its write-back — Oracle's SRAM tag store cannot
substitute for reading real data).

Ported as an extension to the same `chooseInitialState(DCM_ORB_ENTRY *e)`
introduced for BEAR-Wr-Opt (Stage 5) — no new function was needed:
- `e->pol == DCM_POLICY_ORACLE && isWriteHit` → `DCM_LOC_MEM_WRITE`
  directly (identical mechanism to BEAR-Wr-Opt's write-hit exemption).
- `e->pol == DCM_POLICY_ORACLE && isCleanMiss` (where `isCleanMiss =
  !e->isHit && !(e->victimWasValid && e->victimWasDirty)`, i.e. exactly
  the paper's victim-clean rule) → `e->isWriteReq ? DCM_LOC_MEM_WRITE :
  DCM_FAR_MEM_READ` directly, skipping `DCM_LOC_MEM_READ` entirely.
- Every other case (read hit, dirty miss of either kind) falls through
  to the same `e->state = DCM_LOC_MEM_READ` baseline/BEAR-Wr-Opt already
  use.

**No changes were needed to `driveState()` or `return_data()`** — the
same finding as Stage 5 applies again and was re-verified rather than
assumed: gem5's `handleNextState` block for `RambusHypo` is textually
identical in shape to baseline's for every post-`start` state
(`waitingLocMemReadResp`, `waitingFarMemReadResp`, `waitingLocMemWriteResp`),
and this port's already-shared implementation of those states already
generalizes correctly across all three policies, since it dispatches
purely on `{isWriteReq, isHit}` rather than on any policy-specific
constant.

**Baseline and BEAR-Wr-Opt are provably unchanged**: `chooseInitialState()`'s
Oracle branch is only reached when `e->pol == DCM_POLICY_ORACLE`, which
nothing in `main.cc` ever sets (defaults to
`DCM_POLICY_BASELINE_CASCADE_LAKE`). Verified: the full binary produces
byte-identical cycle counts on both test traces before and after this
stage (26174 / 9503 cycles, unchanged from Stage 5), and all 45
pre-existing test blocks (Stages 1-5) still pass unmodified.

### Files modified

- `inc/dram_cache_manager.h` — `chooseInitialState()`'s banner comment
  extended with the Oracle derivation; 5 new `DCM_STATS` fields
  (`cleanMissOptOpportunities/Applied/NotApplicable`, `oracleWriteHits`,
  `oracleCleanMisses`); constructor initializer list updated.
- `src/dram_cache_manager.cc` — `chooseInitialState()` extended with the
  Oracle branch (write-hit exemption reusing the same code path as BEAR;
  new clean-miss exemption).
- `tests/test_dcm_oracle.cc` — **new file**, 17 test blocks (16 required
  + the baseline/BEAR/Oracle comparison table).
- `docs/gem5_to_champsim_mapping.md` — not modified this stage (no new
  gem5 behavioral facts beyond what Stage 5's fact #7 and the BEAR
  comparison table already establish; Oracle's own comparison lives in
  `validation.md` following the same pattern).
- `docs/feature_coverage.md`, `docs/validation.md`, `docs/request_flow.md`,
  `docs/limitations.md` — updated (see those files).

### Build result

`make clean && make` succeeds, zero new warnings.

### Tests performed

`tests/test_dcm_oracle.cc`: **17/17 PASS** — read hit (NOT exempted),
write hit (exempted), read/write × {cold miss, clean-valid-victim miss}
(both exempted), read/write × dirty miss (NOT exempted), replacement,
dirty eviction, repeated access, ORB conflict, a CRB-promoted request
resolving into a real Oracle write hit (using a read-cold-miss blocker,
since Oracle's own write-hit/clean-miss-to-write skips complete
synchronously and offer no observation window — the same finding from
Stage 5's BEAR test 11, re-confirmed and worked around the same way),
WB interaction (hit never touches WB; WB pressure still gates admission
before `chooseInitialState()` runs), metadata correctness after a
skipped read, and a full baseline/BEAR/Oracle comparison table (6
scenarios × 3 policies, all 18 cells exact).

Full regression: all 7 prior test binaries re-run, **45/45 still pass**;
full binary on 2 real traces, byte-identical cycle counts to Stage 5.

No new bugs were found this stage (Stage 5's two test-design lessons —
synchronous-completion windows and stat-delta-vs-cumulative — were
already known and applied correctly on the first attempt here).

### Table II validation

All 6 applicable Oracle cases (Table II's `Tot. Oracle` row `1 1 4 2 1 1
3 1`) verified with exact per-operation-type counts — see
`validation.md`. Two columns change from baseline (write hit 2→1, same
as BEAR) and two more change beyond BEAR (clean miss: read 3→2, write
2→1); dirty-miss and read-hit columns are unchanged, matching the paper
exactly.

### Baseline vs. BEAR vs. Oracle comparison

Full 6-scenario × 3-policy table in `validation.md` (`tests/test_dcm_oracle.cc`
Test 17's actual program output) — every cell matches the paper's Table
II differences exactly; no unexpected deviation in either direction.

### gem5 comparison

Full scenario-by-scenario comparison table in `validation.md`, with the
A/B/C reproducibility classification this stage's instructions
requested. No behavioral discrepancy found between gem5's `RambusHypo`
and this port; every applicable scenario is class C (directly
reproducible) — Oracle's entire behavior lives inside the DRAM cache
manager's own logic, none of it depends on anything ChampSim's
trace-based architecture cannot represent.

### Remaining unimplemented functionality (as of Stage 6)

All three paper policies (baseline, BEAR-Wr-Opt, Oracle) are now
implemented. Remaining at the time:
- Controller frontend/backend latency (`DCM_CTRL_LATENCY_CYCLES`,
  defined, unapplied). **Done in Stage 7, below.**
- WB→far dispatch retry-on-nack modeling (currently fire-and-forget).
- NVM far-memory profile (Case Study 3's other half).
- Actually diverging near/far timing beyond bandwidth (tRP/tRCD/tCAS) —
  documented gap, not a "not yet" item (paper/gem5 don't give the
  numbers in accessible form).

## Stage 7: DCM controller frontend/backend latency — DONE, verified

A final no-code-changes audit (`final_paper_coverage_audit.md`) found
two gaps against gem5: controller frontend/backend latency, and
`bypass_dcache`. This stage implements only the first.

### What was implemented

Re-read `policy_manager.cc`/`PolicyManager.py` directly before coding
(per explicit instruction not to assume the paper's flat "20ns" implies
a single constant). Confirmed gem5 declares two separate 10ns SimObject
params (`static_frontend_latency`, `static_backend_latency`), applied as
`frontend+backend` for a plain response but `frontend+backend+backend`
(backend counted twice) for a response that follows a far-memory fetch.
Full derivation and exact call-site mapping in
`gem5_to_champsim_mapping.md` ("RESOLVED: DCM controller frontend/backend
latency").

`DRAM_CACHE_MANAGER` gained:
- `frontendLatencyCycles`/`backendLatencyCycles` fields, defaulting to
  `DCM_FRONTEND_LATENCY_CYCLES`/`DCM_BACKEND_LATENCY_CYCLES` (10ns each
  @ `CPU_FREQ`, the paper's 20ns round-trip split as gem5 splits it).
- `setControllerLatency(frontendCycles, backendCycles)` for deterministic
  test configuration.
- `DCM_PENDING_RESPONSE{targetCycle, pkt}` struct and a `pendingResponses`
  deque, drained each `operate()` cycle by the new
  `processPendingResponses()` — mirrors the `pendingFarDispatches`
  pattern used for link latency (Stage 4) rather than inventing a new
  mechanism.
- `scheduleResponse(pkt, delayCycles)`: delivers immediately (unchanged
  behavior) if `delayCycles==0`, otherwise enqueues for later delivery.
- `completeRequest(e, responseLatencyCycles=0)`: now takes the response
  delay as a parameter; ORB/CRB bookkeeping stays synchronous regardless
  (matches gem5's decoupling of response scheduling from request-tracking
  bookkeeping). Called with `frontendLatencyCycles+backendLatencyCycles`
  at the local-hit response site and with
  `frontendLatencyCycles+2*backendLatencyCycles` at the far-fetch
  response site. The write path's call (which never produces an
  LLC-visible response) keeps the default 0, which is inert by
  construction, not a special case.

Applies uniformly to baseline, BEAR-Wr-Opt, and Oracle since all three
share the same `completeRequest()` call sites — no policy-specific code
was touched.

### Files modified

- `inc/dram_cache_manager.h`: replaced the old unused
  `DCM_CTRL_LATENCY_CYCLES` macro with
  `DCM_FRONTEND_LATENCY_CYCLES`/`DCM_BACKEND_LATENCY_CYCLES`; added
  `DCM_PENDING_RESPONSE`, the new fields/setter, `pendingResponses`, and
  the two new private method declarations; changed `completeRequest`'s
  signature.
- `src/dram_cache_manager.cc`: constructor initializes the two new
  latency fields; `operate()` calls `processPendingResponses()`;
  `completeRequest()` rewritten to route through `scheduleResponse()`;
  new `scheduleResponse()`/`processPendingResponses()`; the two read
  completion call sites updated to pass the correct single/double
  latency value.
- `tests/test_dcm_link_latency.cc`: Test 3 explicitly sets
  `setControllerLatency(0, 0)` (previously implicit since the feature
  didn't exist) to preserve its original, narrower claim — link-latency
  isolation — now that a nonzero controller-latency default exists.
  Documented inline as an intentional, narrow-scope change, not a
  loosened assertion.
- `tests/test_dcm_controller_latency.cc` (new): 9 deterministic
  assertions, see "Tests performed" below.

### Build result

`make clean && make -j4` — clean build, `bin/champsim` links
successfully with the DCM (as wired in prior stages) using the new
default controller latency. Full-binary smoke test on
`dpc3_traces/403.gcc-16B.champsimtrace.xz` (100K warmup / 200K sim
instructions) completes without error or crash.

### Tests performed

New `tests/test_dcm_controller_latency.cc`, 9/9 assertions passing:
1. Controller latency = 0 behaves exactly as before the feature existed.
2. Controller latency = paper default (10ns+10ns=80 cycles) adds exactly
   that delay to a read-hit response.
3. A custom value (40+60=100 cycles) changes the delay accordingly.
4. Applied exactly once: read hit (single RT, 80 cycles), read miss with
   clean victim (double RT, 120 cycles), read miss with dirty victim
   (double RT, 120 cycles), write hit/miss/WB (zero LLC-visible response,
   so zero effect by construction).
5. Controller latency and far-link latency are additive with no
   double-application (`withBoth == withNeither + ctrlContribution +
   linkContribution`, exactly).
6. Controller latency does not change local/far DRAM operation counts
   for baseline, BEAR-Wr-Opt, or Oracle.

Two test-methodology bugs were found and fixed while writing this file
(both in the new test file itself, not in `DRAM_CACHE_MANAGER`):
resetting `current_core_cycle[0]` mid-Harness corrupted `MEMORY_
CONTROLLER`'s absolute bank-availability state (fixed by measuring via
elapsed-cycle deltas, the pattern already used by every other test
file); and a stop condition of `!responses.empty()` was trivially
already-true on a Harness's second cold-read call, making the measured
delta always 0 for Tests 4b/4c specifically (fixed by comparing against
a `before` snapshot of the response count, matching the pattern already
used correctly by `hitReadCompletionCycles`). Both are documented inline
in the test file.

Full regression: all 8 pre-existing `tests/test_dcm_*.cc` suites
(`skeleton`, `baseline`, `wb_pressure`, `near_far_config`,
`link_latency`, `stats`, `bear`, `oracle`) still pass, with the one
explicitly-documented `test_dcm_link_latency.cc` Test 3 change above.

### gem5 comparison

Exact match to gem5's single-vs-double `backendLatency` distinction
(`accessAndRespond` call sites), including the derived numeric round
trips (80 cycles for single, 120 for double, at the default 10ns+10ns
split and this port's `CPU_FREQ`). See `gem5_to_champsim_mapping.md` for
the full derivation.

### Remaining unimplemented functionality (as of Stage 7)

- `bypass_dcache`-equivalent / no-DRAM-cache comparison toggle
  (final-audit Section 1.15). **Done in Stage 8, below.**
- WB→far dispatch retry-on-nack modeling (currently fire-and-forget).
- NVM far-memory profile (Case Study 3's other half).
- Full Case Study 1/2/3 reproduction runs.
- Actually diverging near/far timing beyond bandwidth (tRP/tRCD/tCAS) —
  documented gap, not a "not yet" item (paper/gem5 don't give the
  numbers in accessible form).

## Stage 8: bypassDcache / "No-DRAM-Cache" comparison mode — DONE, verified

Full architectural writeup (gem5 call sites re-verified directly from
the pulled source, request-path diagrams, exact implementation, timing
derivation) in `docs/bypass_mode.md` — not duplicated here in full.
Summary:

### What was implemented

`DRAM_CACHE_MANAGER::bypassDcache` (bool, default `false`,
`setBypassDcache()`), ported from gem5 `PolicyManager::bypassDcache`
(verified: exactly 3 call sites in `policy_manager.cc`, re-pulled from
`darchr/dram-cache-model`'s `dram_cache_disaggregated` branch before
writing any code). `add_rq()`/`add_wq()` check `bypassDcache`
immediately before all ORB/CRB/WB/admission logic (mirroring gem5's
`recvTimingReq` bypass check being its first statement) and, if set,
forward straight to `farMC` via the EXISTING `dispatchToFar()` helper
(reused from Stage 4's far-link latency, not duplicated) — no ORB entry,
no CRB entry, no `classifyAndInstall()` call, no policy decision, no
cache fill, no dirty eviction. `return_data()` recognizes a bypass
response via a new `bypassOutstandingReads` tracking set (a `multiset`,
since bypass mode performs no conflict tracking and multiple concurrent
same-address bypass reads are valid) and delivers it to the LLC
immediately, with NO controller frontend/backend latency — verified
directly from gem5's `farMemRecvTimingResp` bypass branch
(`port.schedTimingResp(pkt, curTick())`, zero added ticks) BEFORE
assuming this, per the task's explicit "check gem5 before deciding"
instruction. Far-link latency, by contrast, DOES still apply in bypass
mode (verified: no "link" concept exists inside `policy_manager.cc` at
all — the link is external, on the physical port connection, still
crossed by bypass's `farReqPort.sendTimingReq`). `get_occupancy()`/
`get_size()` delegate to `farMC`'s own queue capacity in bypass mode,
reusing the existing far `MEMORY_CONTROLLER` rather than inventing a
parallel capacity model.

### A real ChampSim-specific bug found and fixed

`dispatchToFar()`'s existing callers always stamp `event_cycle` to "now"
before dispatch; `PACKET`'s default constructor leaves it at
`UINT64_MAX`, which `MEMORY_CONTROLLER` never schedules. Bypass mode
skips the normal-mode code path that would have done this stamping
(`admitRequest()`/`driveState()`), so it must do so itself. Without the
fix, a bypass request was silently accepted into `farMC`'s queue but
never completed at all — caught by `tests/test_dcm_bypass.cc` Test 1
failing outright (a real functional bug, not a test-methodology
artifact). Fixed in both `add_rq`'s and `add_wq`'s bypass branches.

### Files created

- `tests/test_dcm_bypass.cc` (new): 15 test blocks.
- `docs/bypass_mode.md` (new): full architectural writeup.

### Files modified

- `inc/dram_cache_manager.h`: `bypassDcache` field + `setBypassDcache()`,
  `bypassOutstandingReads` (`std::multiset<uint64_t>`), 3 new `DCM_STATS`
  counters (`bypassReads`/`bypassWrites`/`bypassCompletedReads`), `<set>`
  include.
- `src/dram_cache_manager.cc`: constructor initializes `bypassDcache(false)`;
  `add_rq()`/`add_wq()` gained bypass branches (with the `event_cycle`
  stamping fix); `get_occupancy()`/`get_size()` delegate to `farMC` in
  bypass mode; `return_data()` checks `bypassOutstandingReads` before the
  ORB lookup.

### Build result

`make clean && make -j4` — clean build. Full-binary smoke test
(`dpc3_traces/403.gcc-16B.champsimtrace.xz`, 100K warmup / 200K sim
instructions) produces output **byte-identical** to the pre-Stage-8
build (`diff` confirms zero difference), proving normal mode
(`bypassDcache` defaulting to `false`) is completely unaffected.

### Tests performed

New `tests/test_dcm_bypass.cc`, 15/15 test blocks passing: the 14
scenarios explicitly required by this stage's task (bypass read, bypass
write, multiple reads, multiple writes, mixed read/write, repeated
access, same-DRAM-cache-index addresses, dirty/write behavior,
response/completion correctness, no local operation, no metadata
modification, no ORB/CRB entry, no WB entry, far memory receiving
requests exactly as expected) plus a normal-vs-bypass comparison for the
identical workload, a controller/link-latency timing test, and a
statistics-isolation test.

Full regression: all 9 pre-existing `tests/test_dcm_*.cc` suites pass
**unchanged, with zero expected-result modifications anywhere** — unlike
Stage 7 (controller latency), `bypassDcache` defaults to `false` and
every pre-existing code path is byte-for-byte untouched when it is.

### gem5 comparison

Full scenario table in `gem5_to_champsim_mapping.md` ("RESOLVED:
bypassDcache / 'No-DRAM-Cache' comparison mode"). Every observable gem5
`bypassDcache` behavior classified A (directly reproducible) except the
far-side retry-on-nack protocol, classified B (reproducible with
adaptation) since it is a pre-existing, already-tracked gap affecting
normal-mode far dispatch identically, not something bypass mode
introduces. No C-classified (not reproducible) differences found.

### Paper connection

Implements the mechanism the paper's Case Study 1/2 "No-DRAM-Cache"
comparison configuration needs (final-audit Section 1.15). Does NOT run
the actual Case Study 1/2 experiments — that remains a separate,
explicitly out-of-scope item (`next_task.md`), requiring real workload
traces.

### Remaining unimplemented functionality (as of Stage 8)

- WB→far dispatch retry-on-nack modeling (currently fire-and-forget;
  affects both normal and bypass modes identically). **Audited (found a
  real silent-write-loss bug, classified MUST IMPLEMENT) and FIXED in
  Stage 9, below** (normal-mode WB path only; bypass-mode writes remain
  unaddressed — see Stage 9's "Remaining unimplemented functionality").
- NVM far-memory profile (Case Study 3's other half). **Audited,
  decision: DO NOT IMPLEMENT** — see `docs/nvm_support_audit.md`.
- Full Case Study 1/2/3 reproduction runs.
- Actually diverging near/far timing beyond bandwidth (tRP/tRCD/tCAS) —
  documented gap, not a "not yet" item.

## Stage 9: WB → far-memory write acceptance/retry fix — DONE, verified

A dedicated audit (`docs/wb_retry_audit.md`, prior stage) found that
ChampSim's `MEMORY_CONTROLLER::add_wq()`/`add_rq()` have no bounds check
at all — a packet is silently discarded with zero signal to the caller
when the destination queue is genuinely full — and that this port's
original fire-and-forget `drainWB()`/`dispatchToFar()` inherited full
exposure to it. Classified **A — MUST IMPLEMENT** (before write-heavy
Case Study reproduction). This stage implements the fix.

### What was implemented

`DRAM_CACHE_MANAGER::dispatchToFar()`'s signature changed from `void` to
`bool`. On the immediate-dispatch path (`linkLatencyCycles == 0`), it
now checks `farMC->get_occupancy(queueType, pkt.address) >=
farMC->get_size(queueType, pkt.address)` BEFORE ever calling
`add_wq()`/`add_rq()`, returning `false` (nothing sent, nothing queued,
caller retains full ownership) if there is no room. `drainWB()` copies
`WB.front()`, attempts `dispatchToFar()` on the copy, and pops the REAL
`WB.front()` only on a `true` return — on `false`, it increments the new
`stats.wbDispatchRetries` counter and returns, leaving the entry for the
next `operate()` cycle's `drainWB()` call to retry (ChampSim's
already-existing per-cycle polling loop IS the retry mechanism — no new
flag, event, or callback needed). `processPendingFarDispatches()`
(link-latency release path) applies the identical check before popping
its own front entry, and `break`s its drain loop on the first blocked
entry rather than skipping ahead, preserving FIFO order under retry.

No `dram_controller.cc`/`MEMORY_CONTROLLER` changes were made —
`get_occupancy()`/`get_size()` already existed and were already the
established pre-check pattern this codebase uses elsewhere (the LLC's
own admission contract), consistent with the audit's explicit
instruction to prefer fixing the DCM WB dispatch path over globally
changing the old ChampSim controller.

### Files modified

- `inc/dram_cache_manager.h`: `dispatchToFar()` now returns `bool`;
  `wbDispatchRetries` counter added to `DCM_STATS`; updated comments on
  `dispatchToFar()`, `drainWB()`, `processPendingFarDispatches()`.
- `src/dram_cache_manager.cc`: the three functions above rewritten per
  "What was implemented."
- `docs/wb_retry_audit.md`: "RESOLUTION" section appended (fix, gem5
  correspondence, test/regression results, remaining gaps) — the
  original audit content is preserved unmodified as historical record.

### Files created

- `tests/test_dcm_wb_retry.cc`: 9 required tests (capacity available;
  farMC full retains the entry; capacity frees up and it drains exactly
  once; multiple entries, none lost/duplicated; sustained pressure
  stress test; mixed WB/normal far traffic; ordering preserved under
  retry; parent request accounting unaffected by a blocked victim
  write-back; WB occupancy statistics correct while blocked and after
  draining) plus a dedicated stress test.

### Build result

`make clean && make -j4` — clean build. Full-binary smoke test
(`dpc3_traces/403.gcc-16B.champsimtrace.xz`, 100K warmup / 200K sim
instructions) produces output **byte-identical** to the pre-Stage-9
build.

### Tests performed

`tests/test_dcm_wb_retry.cc`, all passing, including a 3-wave stress
test (50 dirty evictions per wave × 3 waves = 150 total, each wave
fully retained/blocked via a deterministic test-only `forceWqFull`
override before release): `total_generated=150 total_accepted=150
lost=0 duplicated=0`, with `wbDispatchRetries` in the thousands
confirming capacity was genuinely, repeatedly exceeded. Four real
test-methodology bugs were found and fixed while writing this file (a
premature-quiescence check, a test-address index collision, an
unstamped `event_cycle` on directly-injected packets, and an
initial single-wave design that conflicted with `DCM_WB_PRESSURE_
THRESHOLD`'s own pre-existing admission-time cap) — full detail in
`docs/wb_retry_audit.md`'s "RESOLUTION" section, none of them
implementation bugs.

Full regression: all 10 pre-existing `tests/test_dcm_*.cc` suites pass
**unchanged, with zero expected-result modifications anywhere**
(`test_dcm_skeleton`, `test_dcm_baseline`, `test_dcm_wb_pressure`,
`test_dcm_near_far_config`, `test_dcm_link_latency`, `test_dcm_stats`,
`test_dcm_bear`, `test_dcm_oracle`, `test_dcm_controller_latency`,
`test_dcm_bypass`).

### gem5 comparison

Exact correspondence in outcome (retain-until-accepted, retry every
opportunity, preserve FIFO order) via a different mechanism (gem5:
asynchronous port-retry callback; this port: synchronous per-cycle
polling, which achieves the same outcome more simply since ChampSim has
no async accept/decline protocol to replicate). Full comparison table in
`docs/wb_retry_audit.md`'s "RESOLUTION" section.

### Remaining unimplemented functionality (as of Stage 9)

- The identical `MEMORY_CONTROLLER` silent-drop exposure was separately
  reachable via `nearMC`'s direct, unprotected tag-check-read dispatch
  and background-fill-write dispatch (`driveState()`/`return_data()`),
  and via `dispatchToFar()`'s ignored `bool` return on the far-read and
  bypass-mode call sites. **All fixed in Stage 10, below.**
- NVM far-memory profile — audited, DO NOT IMPLEMENT
  (`docs/nvm_support_audit.md`).
- Full Case Study 1/2/3 reproduction runs — now unblocked for the
  write-heavy (NPB) case by Stage 9's fix, and further hardened by
  Stage 10's fix to every other DCM→memory dispatch path.

## Stage 10: audit and fix ALL remaining memory dispatch paths — DONE, verified

A dedicated audit (`docs/memory_dispatch_audit.md`) examined every
remaining path where `DRAM_CACHE_MANAGER` sends a request to `nearMC`
or `farMC`, following Stage 9's WB→far-write fix. Found FIVE more call
sites with the same class of exposure as the audited-and-fixed WB path:
near-memory tag-check READ, near-memory direct WRITE, near-memory
cache-fill WRITE (all three bare, unprotected `nearMC->add_rq()`/
`add_wq()` calls), far-memory demand READ, and both bypass-mode READ
and WRITE (these last three all called `dispatchToFar()`, which already
had a correct occupancy pre-check, but their `bool` return was
**ignored**, so a rejected dispatch simply vanished with no retry).

### What was implemented

Two new guaranteed-delivery wrapper functions, keeping
`dispatchToFar()`'s existing `bool`-returning, caller-decides contract
UNCHANGED (still used correctly, and only, by `drainWB()`):

- **`dispatchToNear(pkt, isWrite)`** (new): for ALL near-memory dispatch
  (no existing near-side caller manages its own retry, unlike `drainWB()`
  for WB). Tries an immediate send via the new shared primitive
  `trySend()`; on failure, queues into a new `pendingNearDispatches`
  deque, drained every `operate()` cycle by new
  `processPendingNearDispatches()` (FIFO, stops rather than skips on a
  blocked entry — the same discipline `processPendingFarDispatches()`
  already used correctly for WB).
- **`dispatchToFarGuaranteed(pkt, isWrite)`** (new): wraps
  `dispatchToFar()` for the far-read and bypass call sites that had no
  retry logic of their own. On an immediate-attempt failure, it queues
  the packet into the SAME `pendingFarDispatches` queue
  `processPendingFarDispatches()` already drains for WB and
  link-latency-delayed traffic, with its target cycle set to "now" (no
  new queue needed on the far side at all).

`trySend(MEMORY_CONTROLLER *mc, PACKET pkt, bool isWrite)` (new): the
single shared low-level primitive both the above and `dispatchToFar()`'s
own immediate branch are built on — checks real occupancy/size, stamps
`event_cycle`, and calls `add_wq()`/`add_rq()` if there is room.

Six call sites updated: `driveState()`'s `DCM_LOC_MEM_READ` case (→
`dispatchToNear`), `DCM_LOC_MEM_WRITE` case (→ `dispatchToNear`),
`DCM_FAR_MEM_READ` case (→ `dispatchToFarGuaranteed`); `return_data()`'s
background fill write (→ `dispatchToNear`); `add_rq()`/`add_wq()`'s
bypass branches (→ `dispatchToFarGuaranteed`, both). `drainWB()` and
`dispatchToFar()`'s own internals are otherwise unchanged (the latter's
immediate branch was refactored to call the new `trySend()`, a pure
refactor with identical behavior, verified by full regression).

Two new `DCM_STATS` counters: `nearDispatchRetries` (all near-side
retries) and `farDispatchRetries` (far-read and bypass retries,
kept separate from the existing WB-specific `wbDispatchRetries`).

### Files created

- `tests/test_dcm_memory_dispatch.cc`: 12 required tests (near read
  queue full; near fill-write queue full; far read queue full; far
  write queue full [confirming the already-fixed path]; bypass read
  queue full; bypass write queue full; queue becomes available later;
  multiple pending operations; ordering preservation; no duplicates; no
  lost operations; request/parent-ID correctness) plus 3 stress tests
  (near read ×40, far read ×40, near fill write ×20 — each proving
  generated == accepted == eventually completed, or generated ==
  accepted and every fill reaches its destination).

### Files modified

- `inc/dram_cache_manager.h`: new `DCM_PENDING_NEAR_DISPATCH` struct;
  new `pendingNearDispatches` deque; new `nearDispatchRetries`/
  `farDispatchRetries` counters; new `trySend()`,
  `dispatchToFarGuaranteed()`, `dispatchToNear()`,
  `processPendingNearDispatches()` declarations.
- `src/dram_cache_manager.cc`: `trySend()` (new); `dispatchToFar()`
  refactored to call it (behavior unchanged); `dispatchToFarGuaranteed()`,
  `dispatchToNear()`, `processPendingNearDispatches()` (new); `operate()`
  calls `processPendingNearDispatches()`; the 6 call sites listed above
  updated.
- `docs/memory_dispatch_audit.md` (new): full audit table, gem5
  correspondence, and design rationale.

### Build result

`make clean && make -j4` — clean build. Full-binary smoke test
(`dpc3_traces/403.gcc-16B.champsimtrace.xz`, 100K warmup / 200K sim
instructions) produces output **byte-identical** to the pre-Stage-10
build (ignoring the wall-clock "Simulation time" line, which is not a
simulation result).

### Tests performed

`tests/test_dcm_memory_dispatch.cc`, all 15 blocks (12 required + 3
stress) passing. Two real test-methodology bugs were found and fixed
while writing this file (both in the "quiescent" helper, the same
general category already documented in `test_dcm_controller_latency.cc`'s
and `test_dcm_wb_retry.cc`'s banner comments): (1) the quiescent check
omitted `pendingResponses.empty()`, so it reported "done" one step
early whenever the default nonzero controller latency delayed a
response after its ORB entry had already been erased; (2) bypass-mode
traffic has no ORB entry at all, so `ORB.empty()` gave zero signal
about whether a bypass request's real `MEMORY_CONTROLLER` processing
had actually finished — fixed by also checking `near_mc`/`far_mc`'s own
RQ occupancy directly.

Full regression: all 11 pre-existing `tests/test_dcm_*.cc` suites pass
**unchanged, with zero expected-result modifications anywhere**
(`test_dcm_skeleton`, `test_dcm_baseline`, `test_dcm_wb_pressure`,
`test_dcm_near_far_config`, `test_dcm_link_latency`, `test_dcm_stats`,
`test_dcm_bear`, `test_dcm_oracle`, `test_dcm_controller_latency`,
`test_dcm_bypass`, `test_dcm_wb_retry`).

### gem5 comparison

Every audited path corresponds to a gem5 `sendTimingReq()`/
`recvReqRetry()` pair that retains-and-retries on rejection
(`locMemRead`/`retryLocMemRead`, `locMemWrite`/`retryLocMemWrite`,
`farMemRead`/`retryFarMemRead`, and bypass mode's identical use of the
far port's retry protocol) — full table in
`docs/memory_dispatch_audit.md`. This port continues to achieve the
same outcome via synchronous per-cycle polling rather than gem5's async
callback, exactly as established for the WB path in Stage 9.

### Remaining unimplemented functionality (as of Stage 10)

- NVM far-memory profile — audited, DO NOT IMPLEMENT
  (`docs/nvm_support_audit.md`).
- Full Case Study 1/2/3 reproduction runs — now unblocked for both
  write-heavy (NPB) and read-heavy/high-concurrency workloads: every
  DCM→memory dispatch path (near read, near write, near fill, far read,
  far write-back, bypass read, bypass write) now guarantees eventual
  delivery. **Experiment infrastructure built in Stage 11, below; the
  actual long runs remain unlaunched, per explicit instruction.**
- `add_pq()` (prefetch traffic to the lower level) remains a
  pre-existing, unaudited no-op — out of scope (it generates no DCM-side
  dispatch to audit at all; see `docs/limitations.md`).

## Stage 11: Case Study experiment harness — DONE, verified (no long runs launched)

Built the infrastructure needed to reproduce Case Studies 1–3 as
closely as trace-based ChampSim allows, without editing source code
between runs. Full detail, trace investigation, and per-Case-Study
classification in `docs/case_study_reproduction_plan.md`.

### What was implemented

- **`src/main.cc` CLI knobs** (new, purely additive — default behavior
  verified byte-identical to the pre-Stage-11 binary): `--dcm_policy=
  baseline|bear|oracle` (maps to `uncore.DCM.policy`), `--dcm_bypass`
  (maps to `uncore.DCM.setBypassDcache(true)`), `--dcm_link_latency_ns=N`
  (maps to `uncore.DCM.setLinkLatency()`, converting ns→cycles via the
  same `CPU_FREQ`-based formula already used for
  `DCM_LINK_LATENCY_CYCLES_*NS`). An unrecognized `--dcm_policy` value
  now exits cleanly with a clear error instead of silently
  misconfiguring or `abort()`-ing (the pre-existing `getopt_long_only`
  loop's `default: abort();` is only reached for genuinely unrecognized
  FLAGS, not values — a bad value is validated explicitly and reported).
- **`print_dcm_stats()`** (new function in `src/main.cc`, called once
  per run alongside the existing `print_dram_stats()`): surfaces every
  `DRAM_CACHE_MANAGER::stats` field relevant to the paper's own reported
  metrics (requests, hits/misses incl. clean/dirty breakdown, local/far
  reads/writes, WB operations, dispatch retries, CRB/ORB conflicts,
  access amplification, ORB/CRB/WB occupancy, and a newly-computed
  local/far bandwidth utilization matching the paper's Figure 9 metric)
  — `DRAM_CACHE_MANAGER::stats` was never printed anywhere in ChampSim's
  output before this stage. In bypass mode, only the 3 bypass-specific
  counters are printed (normal-mode fields would read as all-zero and
  could be misread as "the cache did nothing" rather than "the cache
  was not in the path").
- **`run_case_studies.sh`** (new, repo root, executable): runs the full
  7-configuration × N-trace matrix from the command line. Supports
  `--dry-run` (prints every command without executing), `--configs`,
  `--traces`, `--warmup`, `--sim`, `--binary`. Defaults to all 7
  configurations and all 6 valid (non-corrupt) traces.
- **`docs/case_study_reproduction_plan.md`** (new): trace investigation
  (format, size-based instruction-count estimate, one corrupt file
  found — `619.lbm_s-3766B.champsimtrace.xz`, truncated download), the 7
  configuration definitions, the full metrics list, sanity-run results
  for all 7 configurations, and the per-Case-Study
  EXACT/ADAPTED/NOT-REPRODUCIBLE classification.

### Files created

- `run_case_studies.sh`
- `docs/case_study_reproduction_plan.md`

### Files modified

- `src/main.cc`: 3 new CLI knobs, `print_dcm_stats()` (new function),
  its call site alongside `print_dram_stats()`.

### Build result

`make clean && make -j4` — clean build (only pre-existing, unrelated
warnings). Full-binary smoke test with NO new flags produces output
that is a strict superset of the pre-Stage-11 output — every
pre-existing line unchanged, only new lines appended (verified via
`diff`).

### Tests performed

Full regression: all 12 pre-existing `tests/test_dcm_*.cc` suites pass
unchanged (these test the DCM library directly, independent of
`main.cc`, so are unaffected by this stage's CLI-only changes — run to
confirm no incidental regression anyway). Tiny sanity runs (≤200K
instructions total) for all 7 configurations on `403.gcc-16B...xz`:
every configuration loaded correctly, selected the correct policy,
bypass genuinely bypassed the DCM (only 3 bypass counters printed, all
normal-mode fields absent), BEAR/Oracle each differed from baseline
only in the expected way (BEAR: identical in a write-hit-free window;
Oracle: local reads reduced by exactly the clean-miss count), and link
latency changed only cycles/IPC while local/far operation counts stayed
identical across all three latencies. Full results table in
`docs/case_study_reproduction_plan.md` Section 6.

### gem5 comparison / paper connection

Not applicable in the usual sense (this stage builds tooling, not a new
behavioral port) — see `docs/case_study_reproduction_plan.md` Section 7
for the per-Case-Study paper-experiment-to-ChampSim-configuration
mapping and classification.

### Remaining gaps

- The actual long (100M-warmup / 500M-sim, 42-run) Case Study matrix
  has NOT been launched — explicitly out of scope for this stage. Exact
  command to launch it is in `docs/case_study_reproduction_plan.md`
  Section 9.
- No GAPBS/NPB traces exist in this environment; SPEC CPU2006 (ChampSim's
  own DPC-3 trace set) is the substitute, with the workload-mismatch
  caveat documented in `docs/case_study_reproduction_plan.md` Section 3.
- `619.lbm_s-3766B.champsimtrace.xz` is corrupt (truncated download) —
  excluded by `run_case_studies.sh`'s default trace list; usable only if
  re-downloaded.
- NVM half of Case Study 3 remains NOT REPRODUCIBLE
  (`docs/nvm_support_audit.md`).

## Stage 12: pre-flight verification + one confirmed 600M-instruction run — DONE

Before authorizing the full 42-run Case Study matrix (Stage 11's
`run_case_studies.sh`), this stage verified every exact command the
script would issue for all 7 configurations (policy/bypass/link-latency
flags, trace paths, instruction counts, output-filename uniqueness —
the last verified PROGRAMMATICALLY via `sort | uniq -d` on the full
42-line dry-run output, not just by inspection) and confirmed DCM
statistics are inherently reset per run (each configuration/trace
combination is launched as a separate OS process; `uncore` is a single
global default-constructed once per process — `src/uncore.cc:4`). Ran
exactly ONE real 600M-instruction confirmation
(`BASELINE`/`403.gcc-16B`, no other configuration or trace) to validate
output completeness/parseability and check for any request-loss/retry
anomaly at real scale. Full detail, the exact command, and the
resulting statistics are in
`docs/case_study_reproduction_plan.md` Section 10 and
`docs/validation.md`.

**Result**: completed successfully in 18 min 58 sec (exit 0,
well-formed 148-line output). Every DCM-internal count invariant
(reads+writes==total, far_reads==miss-count, wb_insertions==wb_drains,
etc.) holds exactly except one 3-in-3.25-million (0.00009%) discrepancy
in the background-fill-write count, fully explained as an ordinary
in-flight-at-cutoff artifact of an asynchronous, off-critical-path
operation (the far reads that trigger those fills are ALL accounted for
exactly — nothing was lost). All 6 retry/reject counters
(`WB_DISPATCH_RETRIES`, `NEAR_DISPATCH_RETRIES`, `FAR_DISPATCH_RETRIES`,
`WB_FULL_REJECTS`, `CRB_FULL_REJECTS`, `ORB_FULL_REJECTS`) are exactly
zero. **No request-loss or retry anomaly found.** Measured throughput
(600M instructions / 18:58) replaces the prior stage's untested
"typically hours" guess for the full 42-run matrix with a real
projection (~13 hours serial, well under 2 hours with modest
parallelism).

### Remaining gaps

- The full 42-run matrix (all 7 configurations × 6 traces) is still not
  launched — only the one `BASELINE`/`403.gcc-16B` confirmation run was
  authorized and performed this stage.

## Stage 13: fix CRITICAL/HIGH findings from the independent audit — DONE, verified

The independent audit (`docs/final_independent_audit.md`) found four
blocking defects. All four are fixed; full root-cause/fix/test detail is
in that document's **§15 Remediation**. Summary:

### What was implemented

- **CRITICAL-1/-2 — duplicate-address merge loses a request.**
  `MEMORY_CONTROLLER::add_rq()` returns a non-negative index and enqueues
  *nothing* when an entry for the same address is already in the read
  queue. `trySend()` ignored that, so a request promoted out of the CRB
  for an address still resident in the near queue was silently dropped,
  stranding its ORB entry and permanently blocking that DRAM-cache index.
  `trySend()` now treats `rc >= 0` as "merged, not enqueued" and returns
  `false`, routing the packet into the existing retain-and-retry path.
  A second instance of the same hole —
  `processPendingFarDispatches()` releasing via raw `add_rq()`/`add_wq()`
  instead of `trySend()` — was found while verifying and also fixed, so
  both release paths are now symmetric. Write merges are deliberately
  still accepted (legitimate write coalescing, no response, no data).
- **HIGH-1 — bandwidth denominator.** `print_dcm_stats()` subtracted
  `begin_sim_cycle` from `finish_sim_cycle`, which is already the ROI
  delta; the result unsigned-wrapped whenever warmup exceeded the ROI in
  cycles. Now uses `finish_sim_cycle` directly, via a new pure,
  unit-testable helper `dcmBandwidthUtilization()`.
- **HIGH-2 — DRAM cache never warmed.** New
  `DRAM_CACHE_MANAGER::warmupTagUpdate()` applies the same tag/metadata
  rules as `classifyAndInstall()` during ChampSim's warmup phase —
  metadata only, no ORB/CRB/WB, no timing, no ROI statistic — so the ROI
  no longer starts against a cold cache. New
  `DRAM_CACHE_MANAGER::resetROIStats()` is called at the warmup→ROI
  boundary beside the existing `reset_cache_stats()` calls: it clears ROI
  counters but leaves the tag store (and all in-flight state) intact.
- **MEDIUM-1 — run-validity instrumentation.** `print_dcm_stats()` now
  reports the four completion counters, `WARMUP_TAG_UPDATES`,
  `DISPATCH_MERGE_RETRIES`, and a residual ORB/CRB/WB/pending line
  flagged `[OK: no request left in flight]` or a loud warning.

### Files modified

- `inc/dram_cache_manager.h` — `dispatchMergeRetries`/`warmupTagUpdates`
  counters; `resetROIStats()`, `warmupTagUpdate()`;
  `dcmBandwidthUtilization()` declaration.
- `src/dram_cache_manager.cc` — `trySend()` merge detection;
  `processPendingFarDispatches()` released via `trySend()`;
  `warmupTagUpdate()`, `resetROIStats()`, `dcmBandwidthUtilization()`;
  warmup branches of `add_rq()`/`add_wq()`.
- `src/main.cc` — corrected bandwidth denominator; `resetROIStats()` at
  the warmup boundary; extended statistics output.

### Files created

- `tests/test_dcm_duplicate_merge.cc` (6 blocks)
- `tests/test_dcm_bandwidth_stats.cc` (6 blocks)
- `tests/test_dcm_warmup.cc` (8 blocks)

### Verification

Table II re-measured after every change: **24/24 exact** for all three
policies — no policy behaviour changed. Full regression: **15/15 suites
pass** (12 pre-existing, none weakened, plus the 3 new). All three
original audit reproductions now report no loss. See
`docs/validation.md` for the 600M pre-flight results.

### Remaining (documented, not fixed here)

HIGH-3 (a false in-code claim about gem5's `isDirty`; behaviour already
correct), MEDIUM-2 (harness failure detection), MEDIUM-3 (no real-LLC
integration test), LOW-1/LOW-2, COSMETIC-1.

## Stage 14: final pre-experiment hardening (HIGH-3, MEDIUM-2, MEDIUM-3) — DONE

Scope was limited to the three named issues; no DRAM-cache architecture,
policy, ORB/CRB/WB semantics, timing, bypass behaviour or Table-II
expectation was changed (Table II re-measured: **24/24 exact**).

- **HIGH-3 (comments/docs only).** The `chooseInitialState()` comment
  claimed gem5's `isDirty` reads the OLD resident line. It does not —
  `handleRequestorPkt()` eagerly overwrites the metadata at
  `recvTimingReq` line 366, before `setNextState()` at line 376, so
  `checkDirty()` reads POST-update state. Corrected to state that this
  port implements the **paper** (Section V + Table II) and deliberately
  diverges from gem5's literal code, with the consequences of gem5's
  ordering spelled out. The same false claim was corrected in
  `feature_coverage.md`, `implementation_status.md`, `request_flow.md`
  and `validation.md`.
- **MEDIUM-2.** `run_case_studies.sh` gained `validate_run()`: a run is
  accepted only if the process exited zero, output exists and is
  complete (`ChampSim completed all CPUs`), the required statistics are
  present *for that mode*, the requested instruction count was retired,
  no crash signature appears, and the DCM does not report itself jammed.
  The script tallies valid/invalid/skipped, lists every bad run and
  **exits non-zero**. Verified against 9 injected failure modes plus two
  valid controls; one false positive (bypass runs print a short-form
  stats block) was found and fixed by making the validator mode-aware.
- **MEDIUM-3.** New `tests/test_dcm_llc_integration.cc` links the actual
  `CACHE` class and drives real LLC traffic into the real DCM — no
  architectural change needed, since the DCM already presents the
  standard `MEMORY` interface. 7 blocks, all passing, including the
  audit's LLC-reachable same-address read+writeback scenario.

### NEW CRITICAL DEFECT FOUND (documented, NOT fixed)

The integration test immediately found what a stubbed LLC never could:
the real `CACHE` passes **block** addresses while `returnIndexDC()`
divides by `DCM_BLOCK_SIZE` again, so 64 distinct 64-byte lines alias
onto one DRAM-cache index — effective line size 4096 B and capacity 8 GB
instead of 64 B / 128 MB, with hit rates systematically overstated in
every full-binary run. Out of this pass's scope and left unfixed by
design; see `docs/final_independent_audit.md` §16 and
`docs/limitations.md`. **This now gates the Case Study matrix.**

### Regression

**16/16 suites pass**; full binary rebuilds clean; 7-configuration
sanity experiment through the hardened harness reports
`valid: 7  INVALID: 0` with exit code 0.

## Stage 15: fix the DRAM-cache address-granularity defect — DONE, verified

The real-LLC integration test added in stage 14 immediately exposed a
CRITICAL address-convention defect (`docs/final_independent_audit.md`
§16). Fixed this stage.

**Root cause.** `returnIndexDC()` computed
`(address / DCM_BLOCK_SIZE) % numLines`, but `PACKET::address` is
*already* a cache-line address in every ChampSim layer — so it divided by
the line size a second time and 64 distinct 64-byte lines aliased onto a
single DRAM-cache index (effective 4096 B lines, 8 GB capacity).

**Fix.** A localized normalization at the DCM interface only.
`returnIndexDC()` is now `address % DCM_NUM_LINES`, `returnTagDC()` is
`address / DCM_NUM_LINES`, both preceded by a ~45-line contract comment
recording the convention and its source evidence. `DCM_NUM_LINES`
(= `DCM_DRAM_CACHE_SIZE / DCM_BLOCK_SIZE` = 2,097,152) was added as the
single source of truth for both the index modulus and
`tagMetadataStore.resize()`. `PACKET::address`'s global meaning was **not**
changed; no other component was touched.

**Unit tests corrected, not weakened.** Eighteen sites across
`test_dcm_baseline.cc` / `test_dcm_bear.cc` / `test_dcm_oracle.cc`, plus
`idxOf` in `test_dcm_warmup.cc`, had hand-rolled the old formula to locate
metadata slots; they now call `dcm.returnIndexDC()`. **No expected value
changed, and Table II re-measures 24/24 exact.**

**Tests.** `test_dcm_llc_integration.cc` Test 7 is no longer a
characterisation test asserting the aliasing — it asserts the correct
behaviour. Tests 8–10 added: same-index/different-tag conflict with dirty
replacement and writeback; direct-injection vs real-LLC entry-path
equivalence; capacity/granularity invariants.

**Verification.** 16/16 suites pass. Table II 24/24 exact. Short real
trace A/B on the same trace: LLC misses 422 in both; DCM hits
395 → **0**, DCM misses 27 → **422**, hit rate 93.6% → **0.0%** — the
pre-fix "hits" were 422 distinct lines collapsing into 27 4-KiB
super-lines. Identical LLC misses confirms the blast radius was
DCM-internal only.

## Stage 16: post-fix 600M production pre-flight — DONE

One 600M-instruction run only (no matrix). `BASELINE`
(CascadeLakeNoPartWrs), no bypass / no BEAR / no Oracle, 100M warmup +
500M ROI, on `462.libquantum-1343B`.

**Trace chosen by measurement**, not assumption: `619.lbm_s` re-confirmed
corrupt (`xz -t`); the remaining six were ranked by a short 5M/20M probe
and `462.libquantum-1343B` leads on LLC MPKI at 22.66, roughly twice the
next candidate.

**Pre-launch configuration verified** against the freshly rebuilt binary
via a compiled-constant probe: `BLOCK_SIZE == DCM_BLOCK_SIZE == 64 B`;
`DCM_DRAM_CACHE_SIZE == 134,217,728 B == 128 MiB`;
`DCM_NUM_LINES == 2,097,152` with `NUM_LINES x BLOCK == DCM_DRAM_CACHE_SIZE`;
line *L*/*L+1* → distinct indices, *L*/*L+NUM_LINES* → same index with
tag+1; default policy `baseline`, default `bypassDcache == false`;
frontend/backend latency 10 ns (40 cycles) each; ORB/CRB/WB 128/32/64.

**Results**: IPC 0.275061 over 1,817,776,339 ROI cycles; 12,736,699 LLC
misses; 17,763,568 DCM requests; 14,098,299 hits / 3,665,269 misses of
which only **27 cold**; **79.37% hit rate**; access amplification 1.77506;
local BW 11.63% / far BW 3.74%; ORB avg 1.29 max 10; CRB 0; WB max 1.

**Conservation: 18/18 invariants exact**; every retry, full-reject and
merge counter is 0; residual `[OK: fully drained]`. Passes the harness's
own `validate_run()`. One +1 boundary artifact between LLC ROI misses and
DCM ROI reads, explained by the LLC and DCM stat resets sitting on either
side of a single straddling request (`src/main.cc:347` vs `:357`).

**All pre-fix full-binary results are invalidated**, including the
stage-12 `403.gcc-16B` 600M run and its 99.87% hit rate — that figure was
the aliasing defect. Details: `docs/case_study_reproduction_plan.md` §11,
`docs/validation.md` "Stage 16".

**Stale comments corrected (stage 17, comment-only)**: seven pre-Stage-5/6 comments in
`inc/dram_cache_manager.h` and `src/dram_cache_manager.cc` described a state of the code that
had not been true since stages 5-9. All are now accurate:

| location | was | now |
|---|---|---|
| `h` DCM_POLICY enum | "DCM_POLICY_ORACLE is NOT implemented yet" | all three selectable via `--dcm_policy`, Table-II-verified, with the per-policy skip rules |
| `h` class banner | "BEAR-Wr-Opt and Oracle are NOT implemented" | all three transition tables wired; they differ only in `chooseInitialState()` |
| `h:21` CRB | "declared; not yet enforced" | enforced in `add_rq()`/`add_wq()` at `dram_cache_manager.cc:271,347` |
| `h:74` link latency | "Defined but NOT YET applied" | applied via `dispatchToFar()`/`pendingFarDispatches`; `--dcm_link_latency_ns` -> `setLinkLatency()`; the three named constants are the paper's Case Study 3 values used by the tests |
| `h:578` tagMetadataStore | "allocated, not yet consulted" | read/written every request by `classifyAndInstall()` and `warmupTagUpdate()`; sized `DCM_NUM_LINES` |
| `cc:11` file banner | "BEAR-Wr-Opt and Oracle are NOT implemented" | complete path for all three policies |
| `cc:576` cacheFills | "Under BEAR/Oracle (not implemented) this would need its own condition" | insert-on-miss is policy-INDEPENDENT; `CACHE_FILLS == MISSES` verified under all three |

No functional code touched, and proven so: the comment-stripped translation units are
byte-identical before and after; 16/16 suites pass; Table II assertions re-verified; and full
simulator output on a real trace is identical to the pre-cleanup binary under all three policies.

## ChampSim PACKET::address convention and DCM normalization

**What `PACKET::address` means in each layer** (established by source
inspection, not assumption): it is the physical **cache-line (block)
address** everywhere in the hierarchy, i.e.
`address == full_addr >> LOG2_BLOCK_SIZE`, while `full_addr` is the
physical **byte** address.

- CPU → L1: `src/ooo_cpu.cc:1696,2253` set `address = physical_address >>
  LOG2_BLOCK_SIZE` and `full_addr = physical_address`.
- CACHE, all levels: `src/cache.cc:1058` — `get_set()` masks `address`
  *directly* with `(NUM_SET-1)`; `get_way()` compares the whole `address`
  as the tag. Neither is correct for a byte address.
- CACHE → `lower_level`: the packet is forwarded **unmodified**
  (`src/cache.cc:661,672` for reads, `:115,445` for writebacks);
  writebacks carry `block[set][way].address` (`src/cache.cc:107`), itself
  a line address.
- MEMORY_CONTROLLER: `src/dram_controller.cc:634` — `dram_get_channel()`
  uses `shift = 0`, i.e. no byte-offset bits to discard.
- The **only** other convention is the TLB path
  (`src/ooo_cpu.cc:1459,1542`), where `address` is a page number. It never
  reaches the DCM.

**What the DCM expects, and where normalization occurs.** The DCM
consumes the same convention, so normalization is a no-op *by design* and
is localized entirely to `returnIndexDC()`/`returnTagDC()` in
`src/dram_cache_manager.cc` — no shift, no divide:

```
incoming PACKET::address  (cache-line address, normalized upstream)
  -> lineAddr = address
  -> indexDC  = lineAddr % DCM_NUM_LINES   (direct-mapped, paper baseline)
  -> tagDC    = lineAddr / DCM_NUM_LINES
```

`PACKET::address`'s global meaning is **not** changed: source inspection
proved it is already correct and consistent in every other component, so
the correct architectural fix was to make the DCM agree with the existing
contract rather than redefine the contract.

**How 64-byte granularity is guaranteed.** `indexDC` advances by exactly
1 per consecutive cache-line address, so adjacent 64-byte lines *L* and
*L+1* always occupy different indices. Asserted through the real LLC in
`tests/test_dcm_llc_integration.cc` Test 7 (`0x40000000` / `0x40000040` →
indices 0 and 1, two misses, both resident simultaneously).

**How 128 MiB capacity is guaranteed.**
`DCM_NUM_LINES = DCM_DRAM_CACHE_SIZE / DCM_BLOCK_SIZE = 128 MiB / 64 B =
2,097,152` is a single source of truth: it is both the index modulus and
the size of `tagMetadataStore` (`tagMetadataStore.resize(DCM_NUM_LINES)`
in the constructor). Hence `DCM_NUM_LINES * DCM_BLOCK_SIZE ==
DCM_DRAM_CACHE_SIZE` holds by construction, and the index can never fall
outside the store. `tagMetadataStore` is the same compact per-line
metadata vector as before — no larger structure is allocated or iterated.
Asserted in Test 10.

**Before / after.** `returnIndexDC()` previously computed
`(address / DCM_BLOCK_SIZE) % numLines`, dividing by the line size a
second time:

| | before (defective) | after (correct) |
|---|---|---|
| index formula | `(address / 64) % 2097152` | `address % 2097152` |
| effective line size | 4096 B | **64 B** |
| effective capacity | 8 GB | **128 MiB** |
| `0x40000000` vs `0x40000040` | same index → false HIT | different indices → two misses |

Full analysis: `docs/final_independent_audit.md` §16 and
`docs/gem5_to_champsim_mapping.md`.


## Production warmup methodology — fixed 1B / 500M

**This is the current production methodology. It is fixed and
deterministic. There is no adaptive warmup.**

```
Warmup = 1,000,000,000 instructions
ROI    =   500,000,000 instructions
```

Every configuration in the 42-run matrix uses these same two numbers —
`BASELINE`, `BEAR_WR_OPT`, `ORACLE`, `NO_DRAM_CACHE`, `BASELINE_100NS`,
`BASELINE_500NS`, `BASELINE_1000NS` — so all seven measure the same
instruction window by construction. `NO_DRAM_CACHE` receives exactly the
same fixed 1B warmup as every other configuration; no boundary is
propagated between configurations.

Explicitly absent: no cache-state-based warmup termination, no dynamic
extension of warmup, no warmup cap, no warmup criterion, no
`--dcm_warmup_max_instructions`, no `--warmup-cap`, no ROI-boundary
transfer, and no `WARMUP_LINES_FILLED` / `WARMUP_CRITERION` reporting.

### Why 1B — stated accurately

**The paper does not use 1B instructions, and this is not a conversion of
the paper's number.** The paper warms for **100 ms of simulated wall-clock
time** in full-system gem5, takes a checkpoint, and restores every
configuration from it (§III-C, Fig. 3). ChampSim is trace-based, has no
full-system clock and no checkpoint mechanism, so **instruction-count
warmup is the available adaptation** — that adaptation predates this
change and is a long-standing documented limitation.

1B was chosen as an **experimental methodology decision for the ChampSim
adaptation**: the independent audit measured the previous 100M warmup as
insufficient on several traces (cold misses reached 32–90% of ROI misses;
47.1% of all DCM accesses on 434.zeusmp). 1B is an order of magnitude
longer while remaining a fixed, reproducible instruction count.

### Cold misses are measured, not targeted

`COLD_MISSES` remains a genuine measured statistic. Nothing forces it
toward zero and no cold-miss threshold is required or asserted anywhere.
The 1B warmup is expected to substantially reduce cold-start
contamination relative to 100M; the actual ratio is whatever each run
reports and must be read from the run's own `COLD_MISSES` / `MISSES`
fields.

### Determinism

Warmup ends at a fixed retired-instruction count, so the boundary is a
property of the trace and the configured number alone. Repeated runs of
the same configuration are bit-identical (verified previously across
three configurations and two machines).

### Historical note — the adaptive warmup experiment (REJECTED)

A cache-state-based adaptive warmup criterion was prototyped (gem5's
`cache_warmup_ratio = 0.7` distinct-line rule plus a footprint-saturation
stop and an instruction cap). **It was evaluated and rejected, and is NOT
the production methodology.** It has been removed from the simulator and
the harness. It is recorded here only to explain why the fixed methodology
is stated so explicitly: the adaptive approach produced
configuration-dependent warmup boundaries, which required a separate
ROI-alignment mechanism to keep `NO_DRAM_CACHE` comparable to
`BASELINE` — complexity the fixed 1B methodology avoids entirely, because
every configuration stops at the same instruction count by construction.
