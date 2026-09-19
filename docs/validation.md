# Validation

## Table II oracle (paper Section V, `Tot. Baseline` row)

The paper's Table II gives per-case operation counts. The
"Tot. Baseline" row is `1 1 4 3 2 2 3 2` for columns
`[RdHitDirty, RdHitClean, RdMissDirty, RdMissClean, WrHitDirty,
WrHitClean, WrMissDirty, WrMissClean]`. Rather than trust the OCR'd
per-operation checkmark grid (ambiguous in the extracted PDF text), the
per-operation breakdown below was derived independently from gem5's
actual state machine (`policy_manager.cc`, read directly, not from
memory) and then cross-checked against these totals — all eight match
exactly, which is itself a validation that the derivation is correct:

| Case | local_read | local_write | far_read | far_write | total | Table II total |
|---|---|---|---|---|---|---|
| A. Read Hit (dirty or clean) | 1 | 0 | 0 | 0 | 1 | 1, 1 |
| C/D. Read Miss Clean (cold or clean victim) | 1 | 1 | 1 | 0 | 3 | 3 |
| E. Read Miss Dirty | 1 | 1 | 1 | 1 | 4 | 4 |
| B. Write Hit (dirty or clean) | 1 | 1 | 0 | 0 | 2 | 2, 2 |
| H. Write Miss Dirty | 1 | 1 | 0 | 1 | 3 | 3 |
| F/G. Write Miss Clean (cold or clean victim) | 1 | 1 | 0 | 0 | 2 | 2 |

All six rows implemented and asserted exactly (not just totals — the
per-operation-type breakdown) in `tests/test_dcm_baseline.cc`, cases
A–H. Result: **8/8 PASS** (A, B, C, D, E, F, G, H each assert exact
`local_read`/`local_write`/`far_read`/`far_write` counts via delta
snapshots around the case under test).

## Deterministic test inventory

### `tests/test_dcm_skeleton.cc` — integration, timing, ORB/CRB (6 tests)

Build: `g++ -std=c++11 -Iinc -o /tmp/test_dcm_skeleton tests/test_dcm_skeleton.cc src/dram_cache_manager.cc src/dram_controller.cc src/block.cc`

| # | Test | Verifies | Result |
|---|---|---|---|
| 1 | Local-hit path (real, post-warm-up) | Genuine tag-store hit after a real cold miss installs the line: exactly 1 more near op, 0 far ops | PASS |
| 2 | Far-miss path | Genuine cold miss reaches far memory, responds correctly | PASS |
| 3 | Write path | No LLC callback for writes; matches ChampSim's pre-existing write contract | PASS |
| 4 | Near/far timing independence | Deliberately different `set_timing()` per controller produces genuinely different completion latency | PASS |
| 5 | ORB/CRB conflict + real classification | Conflict → CRB → promotion → **real** READ MISS + CLEAN VICTIM classification of the promoted request (not hand-set) | PASS |
| 6 | CRB backpressure | `DCM_CRB_MAX_SIZE` enforced via `get_occupancy`/`get_size` | PASS |

### `tests/test_dcm_baseline.cc` — complete baseline behavior (10 tests)

Build: `g++ -std=c++11 -Iinc -o /tmp/test_dcm_baseline tests/test_dcm_baseline.cc src/dram_cache_manager.cc src/dram_controller.cc src/block.cc`

| Case | Verifies | Result |
|---|---|---|
| A. READ HIT | op counts, metadata unchanged (valid, clean, correct far addr) | PASS |
| B. WRITE HIT | op counts, metadata now dirty | PASS |
| C. READ MISS + COLD | op counts, victim correctly seen as invalid, metadata installs clean, classification stats (`numColdMisses`, `numRdMissClean`), zero write-backs | PASS |
| D. READ MISS + CLEAN VICTIM | op counts, victim correctly seen as valid+clean, **no** write-back, classification stats (delta-based, see below) | PASS |
| E. READ MISS + DIRTY VICTIM | op counts (4, matches Table II), victim correctly seen as valid+dirty, exactly one write-back, new line installs clean | PASS |
| F. WRITE MISS + COLD | op counts, metadata installs dirty | PASS |
| G. WRITE MISS + CLEAN VICTIM | op counts, no write-back | PASS |
| H. WRITE MISS + DIRTY VICTIM | op counts (3, matches Table II), exactly one write-back, **no** far-read ever (writes never fetch) | PASS |
| Repeated access | 5 consecutive hits to the same line stay pure local hits (no re-miss, no drift) | PASS |
| Replacement chain | 4 addresses round-robin one index, each dirtied before the next evicts it → exactly N-1=3 write-backs, 1 cold miss, 3 hot misses | PASS |

One test bug was found and fixed during this validation (not an
implementation bug): Case D's initial setup read (installing the first
resident line) is *itself* a cold `RdMissClean`, so asserting the
harness-cumulative `numRdMissClean == 1` after the case-under-test read
was wrong (it's legitimately 2 — one from setup, one from the case).
Rather than relax the expected value, the implementation was checked
first: the code was verified against gem5's `checkHitOrMiss` (cold misses are
classified as clean misses, `policy_manager.cc:1506-1519`) and confirmed
correct; the test was fixed to use before/after deltas around the
case-under-test operation instead of a cumulative count.

### `tests/test_dcm_wb_pressure.cc` — WB occupancy admission backpressure (5 tests)

Build: `g++ -std=c++11 -Iinc -o /tmp/test_dcm_wb_pressure tests/test_dcm_wb_pressure.cc src/dram_cache_manager.cc src/dram_controller.cc src/block.cc`

| # | Test | Verifies | Result |
|---|---|---|---|
| 1-3 | Below/at/above `DCM_WB_PRESSURE_THRESHOLD` (=64) | Admission allowed below, blocked at and above (both read and write), via `get_occupancy`==`get_size`, and `add_rq` actually refuses admission | PASS |
| 4 | Retry after drain | A request blocked by WB pressure succeeds once `drainWB()` brings the WB deque back below threshold | PASS |
| 5a | Conflict beats WB pressure | A conflicting request still queues in the CRB even while WB is at threshold (gem5's exact check ordering: conflict check precedes WB check) | PASS |
| 5b | WB pressure blocks despite ORB room | A non-conflicting request is blocked by WB pressure even when the ORB itself is empty | PASS |

### `tests/test_dcm_near_far_config.cc` — near/far configuration (3 tests)

Build: `g++ -std=c++11 -Iinc -o /tmp/test_dcm_near_far_config tests/test_dcm_near_far_config.cc src/dram_cache_manager.cc src/dram_controller.cc src/block.cc`

| # | Test | Verifies | Result |
|---|---|---|---|
| 1 | Paper-derived MT/s + dbus differentiation | `configureNearAsHBM2`/`configureFarAsDDR4` set exactly 4000/2400 MT/s; resulting `DRAM_DBUS_RETURN_TIME` differs (8 vs 13 cycles) | PASS |
| 2 | Near timing isolation | Slowing ONLY near's timing (400 vs 4 cycles for tRP/tRCD/tCAS) more than doubles a pure hit's latency, with far timing held identical in both runs | PASS |
| 3 | Queue independence | Occupying near's RQ does not change far's RQ occupancy | PASS |

A real test-design bug was found and fixed while writing Test 2 (not a
DCM bug): ChampSim's own pre-existing write-queue-forwarding shortcut
(`MEMORY_CONTROLLER::add_rq`, services a read instantly if a matching
address is still in that controller's write queue) meant a "hit"
measurement taken too soon after a fill could be serviced by that
shortcut instead of real read timing — and did so more often for the
*slower* near controller (whose fill write took longer to drain),
initially making the slow controller look faster. Fixed by waiting for
the write queue to actually drain (`get_occupancy(2,...) == 0`) before
measuring. See `limitations.md` for the general note (this ChampSim
behavior predates this port and could affect other timing measurements).

### `tests/test_dcm_link_latency.cc` — Case Study 3 link latency (4 tests)

Build: `g++ -std=c++11 -Iinc -o /tmp/test_dcm_link_latency tests/test_dcm_link_latency.cc src/dram_cache_manager.cc src/dram_controller.cc src/block.cc`

| # | Test | Verifies | Result |
|---|---|---|---|
| 1/2 | Latency scaling | 0/100/500/1000ns produce monotonically increasing, closely-matching-expected completion times for a cold-miss read | PASS — measured deltas (400, 1600, 2000 cycles) matched hand-derived expected deltas **exactly**, no tolerance needed |
| 3 | Hit isolation | A pure local hit's completion time is identical regardless of a configured 1000ns link latency; `pendingFarDispatches` stays empty | PASS — **updated in stage 7**: now explicitly calls `setControllerLatency(0, 0)` right after construction, since stage 7 gave `DRAM_CACHE_MANAGER` a nonzero-by-default controller latency and this test's `hitCycles > 100` threshold assumed zero controller latency (a real regression, caught by the stage-7 full-regression run). Setting it to 0 here preserves the test's original, narrower claim — link-latency isolation — without conflating it with the separately-tested controller-latency feature. Documented inline in the test file; the test's actual assertion and expected values are otherwise unchanged. |
| 4 | Write-back path | A dirty victim's write-back is also held in `pendingFarDispatches` for the configured link latency before reaching `farMC` | PASS |

### `tests/test_dcm_controller_latency.cc` — DCM controller frontend/backend latency (9 assertions, stage 7)

Build: `g++ -std=c++11 -Iinc -o /tmp/test_dcm_controller_latency tests/test_dcm_controller_latency.cc src/dram_cache_manager.cc src/dram_controller.cc src/block.cc`

| # | Test | Verifies | Result |
|---|---|---|---|
| 1 | Zero latency | `setControllerLatency(0,0)` behaves exactly as before the feature existed (recorded as the baseline for Tests 2–3) | PASS |
| 2 | Paper default | Default constructor latency (10ns+10ns=80 cycles @ `CPU_FREQ`) adds exactly 80 cycles to a read-hit's completion vs. Test 1's baseline | PASS — `hit_delta_vs_zero=80`, `expected_round_trip=80`, exact match |
| 3 | Custom value | `setControllerLatency(40, 60)` adds exactly 100 cycles vs. Test 1's baseline | PASS — `hit_delta_vs_zero=100`, `expected=100`, exact match |
| 4a | Read hit, exactly once | Single round trip (frontend+backend=80 cycles) | PASS — `delta=80` |
| 4b | Read miss, clean victim, exactly once | Double round trip (frontend+2×backend=120 cycles), since a miss response always follows a far fetch in this port | PASS — `delta=120`, after fixing a test-harness bug (see below) |
| 4c | Read miss, dirty victim, exactly once | Same double round trip, with an intervening dirty write on the victim address | PASS — `delta=120` |
| 4d | Write hit/miss/WB, exactly once (zero) | Writes never produce an LLC-visible response in this port, so controller latency has zero effect on them by construction | PASS — `no_response=1`, `write_completed=1` |
| 5 | Additive with link latency | `withBoth == withNeither + ctrlContribution + linkContribution` exactly, proving neither mechanism double-applies when both are configured | PASS — `neither=301 ctrl_only=421 link_only=2301 both=2421 expected_both=2421` |
| 6 | Op-count invariance | Controller latency changes only the final response's delivery cycle, never `localReads`/`localWrites`/`farReads`/`farWrites`/`numWrBacks`, for baseline, BEAR-Wr-Opt, and Oracle alike | PASS for all 3 policies |

**Two test-methodology bugs found and fixed while writing this file**
(both in the test file, not in `DRAM_CACHE_MANAGER` — the same category
of issue already documented for other test files, e.g.
`test_dcm_near_far_config.cc`'s write-forwarding note):
1. An earlier version of the cold-miss/hit timing helpers reset
   `current_core_cycle[0]` to 0 between a warm-up phase and a measurement
   phase while reusing the same `MEMORY_CONTROLLER` instances.
   `MEMORY_CONTROLLER`'s `bank_request[...].cycle_available` holds
   ABSOLUTE cycle numbers, so resetting backward made banks appear busy
   for hundreds of stale cycles. Fixed by measuring via elapsed-cycle
   deltas and never resetting mid-`Harness`, the pattern already used
   correctly by every other `tests/test_dcm_*.cc` file.
2. `coldReadCompletionCycles`'s stop condition,
   `!h.llc.responses.empty()`, is trivially already-true on a Harness's
   SECOND cold-read call (Tests 4b/4c/6 issue two cold reads per
   Harness), so the second call returned almost instantly regardless of
   configured controller latency — producing `delta=0` for Tests 4b/4c
   in both the zero-latency and default-latency harnesses identically.
   Fixed by comparing against a `before` snapshot of the response count,
   the pattern already used correctly by `hitReadCompletionCycles` in the
   same file. Confirmed via a standalone instrumented trace
   (`dcm.debugPrint=true`) showing the correct 120-cycle difference
   between the two harnesses' second read once the fix was applied.

Full regression: all 8 pre-existing `tests/test_dcm_*.cc` suites pass
unchanged (modulo the one documented `test_dcm_link_latency.cc` Test 3
change above). Full-binary build (`make clean && make -j4`) succeeds,
and a smoke-test run (`dpc3_traces/403.gcc-16B.champsimtrace.xz`, 100K
warmup / 200K sim instructions) completes without error.

### `tests/test_dcm_stats.cc` — statistics completeness and independence (3 tests)

Build: `g++ -std=c++11 -Iinc -o /tmp/test_dcm_stats tests/test_dcm_stats.cc src/dram_cache_manager.cc src/dram_controller.cc src/block.cc`

| # | Test | Verifies | Result |
|---|---|---|---|
| 1 | Full stats coverage | Every required statistic (this stage's checklist) increments to the exact hand-derived value for a 5-request scenario (cold miss, hit, write-hit, miss-with-dirty-eviction, cold write-miss) | PASS |
| 2 | Stats independence | Two independent `DRAM_CACHE_MANAGER` instances never share/leak counters | PASS |
| 3 | Occupancy tracking | `orbMaxOccupancy`/`avgOrbOccupancy()` update correctly after an `operate()` cycle with one outstanding request | PASS |

## Full-binary trace smoke tests (not performance validation — see limitations.md)

Ran to completion, no crash/hang, after this stage's changes (near=HBM2
4000 MT/s, far=DDR4 2400 MT/s, WB pressure infra, link latency infra
present but `linkLatencyCycles=0` by default so unused in the full
binary):

- `401.bzip2-277B`: 5003 instrs, 26174 cycles (was 26154 before this
  stage's real bandwidth differentiation — the small change is the
  expected, real effect of near/far now having genuinely different,
  paper-derived timing instead of identical placeholder values), LLC avg
  miss latency 249.09 cycles.
- `462.libquantum-1343B`: 5000 instrs, 9503 cycles, LLC avg miss latency
  142.649 cycles (unchanged from the previous stage — this trace's access
  pattern happens to not be sensitive to the bandwidth change at this
  instruction count).

These numbers are **structural sanity checks**, not claims of gem5
equivalence (see `limitations.md`).

## Re-check against the reference documents (this stage's explicit ask)

- `docs/paper_to_champsim_spec.md`: functional requirements table updated
  — "insert-on-miss, write-back" and "tag/metadata co-located" rows move
  from "structurally mirrored, not consulted" to fully wired; "miss
  handling requires multiple accesses" row moves from "not yet
  policy-accurate" to verified via Table II.
- `docs/gem5_to_champsim_mapping.md`: needs a new fact/section for the
  eager-metadata-update ordering and the tag-check-completion write-back
  trigger point — added (see project_status.md item to keep this doc's
  "Confirmed gem5 behavioral facts" numbering consistent going forward).
- `docs/feature_coverage.md`: updated (see that file directly).
- Actual gem5 implementation: re-read `handleRequestorPkt`
  (`policy_manager.cc:1345-1444`) and `locMemRecvTimingResp`
  (`policy_manager.cc:521-545`) directly from the pulled source (not from
  memory/notes) to confirm the exact metadata-update ordering and the
  dirty-write-back trigger point before writing any code. Both are
  ported exactly, not approximated — see `request_flow.md` for the
  request-flow evidence.

## BEAR-Wr-Opt

### Table II oracle, BEAR-Wr-Opt row

Paper's Table II "Tot. BEAR-Wr-Opt" row: `1 1 4 3 1 1 3 2` — identical to
baseline's `1 1 4 3 2 2 3 2` except the two write-hit columns (5th and
6th: WrHitDirty, WrHitClean) drop from 2 to 1. Every other column is
unchanged. This is exactly what "BEAR eliminates the local tag-check read
for write hits only" predicts, and is used here as the oracle:

| Case | local_read | local_write | far_read | far_write | total | Table II total | Changed from baseline? |
|---|---|---|---|---|---|---|---|
| Write Hit (dirty or clean) | 0 | 1 | 0 | 0 | 1 | 1, 1 | **Yes** (baseline: 2) |
| Read Hit | 1 | 0 | 0 | 0 | 1 | 1, 1 | No |
| Read Miss Clean (cold/clean victim) | 1 | 1 | 1 | 0 | 3 | 3 | No |
| Read Miss Dirty | 1 | 1 | 1 | 1 | 4 | 4 | No |
| Write Miss Dirty | 1 | 1 | 0 | 1 | 3 | 3 | No |
| Write Miss Clean (cold/clean victim) | 1 | 1 | 0 | 0 | 2 | 2 | No |

All six rows asserted exactly (per-operation-type, not just totals) in
`tests/test_dcm_bear.cc`. Result: **all applicable BEAR cases PASS**,
including explicit assertions that the write-miss and read cases are
byte-for-byte unchanged from baseline (Tests 2, 3, 4, 5, 6, 7).

### `tests/test_dcm_bear.cc` — BEAR-Wr-Opt policy (14 tests)

Build: `g++ -std=c++11 -Iinc -o /tmp/test_dcm_bear tests/test_dcm_bear.cc src/dram_cache_manager.cc src/dram_controller.cc src/block.cc`

| # | Test | Verifies | Result |
|---|---|---|---|
| 1 | BEAR write hit | Local tag-check read count is exactly 0 (not 1); `writeHitOptApplied`/`writeHitOptOpportunities` both +1; metadata correct | PASS |
| 2 | BEAR write miss + cold | Read NOT skipped for a miss; `writeHitOptApplied` unchanged | PASS |
| 3 | BEAR write miss + clean victim | Read NOT skipped; no write-back | PASS |
| 4 | BEAR write miss + dirty victim | Read NOT skipped; exactly one write-back; matches Table II total=3 | PASS |
| 5 | BEAR read hit | Read NOT skipped for a read (BEAR never touches reads); `writeHitOptOpportunities` unchanged | PASS |
| 6 | BEAR read miss + clean victim | Unchanged from baseline (total=3) | PASS |
| 7 | BEAR read miss + dirty victim | Unchanged from baseline (total=4, matches Table II) | PASS |
| 8 | Metadata update after fill | valid=true, dirty=false, correct far address after a cold-miss install | PASS |
| 9 | Metadata update after replacement | New tag/address correctly overwrite the old ones at the shared index | PASS |
| 10 | Metadata after dirty eviction | Old line's dirty bit correctly triggers exactly one write-back; new line installs clean | PASS |
| 11 | Conflict → CRB promotion → BEAR write hit | A request that only becomes a write hit *because* of eager tag installation by the ORB entry it conflicted with is correctly detected as a hit and correctly gets the read eliminated | PASS |
| 12 | ORB/CRB admission ordering | Unaffected by active policy (conflict check still runs identically) | PASS |
| 13a | WB interaction: write hit | A write hit never inserts into WB (a hit never evicts) | PASS |
| 13b | WB interaction: pressure | WB pressure still blocks admission under BEAR exactly like baseline — the optimization cannot bypass admission control (checked *before* classification/`chooseInitialState` ever runs) | PASS |
| 14 | Baseline vs. BEAR diff | Running an identical 3-step scenario under both policies: request-flow logs are byte-for-byte identical except the one write-hit step; aggregate stats differ by exactly one fewer `local_read` for BEAR | PASS |

One test-design issue was found and fixed while writing Test 11 (not a
DCM bug): a BEAR write hit completes *synchronously*, within the same
`add_wq()` call that admitted it (it skips the only asynchronous step),
so there is no window in a single-threaded synchronous test in which
another request could observe it as "still outstanding" — an initial
version of Test 11 assumed such a window existed and found the ORB
already empty. Fixed by redesigning the scenario around a request that
*becomes* a write hit as a result of CRB promotion instead (see
`request_flow.md` for the full trace and reasoning).

### gem5 comparison table (this stage's explicit ask)

Direct comparison against `policy_manager.cc`'s `enums::BearWriteOpt`
branches in `setNextState` (:897-1007) and `handleNextState`
(:1234-1341), read from the previously-pulled gem5 source before writing
any code (not from memory or the papers' prose alone):

| Scenario | gem5 behavior | ChampSim behavior | Match? | Difference | Reason |
|---|---|---|---|---|---|
| Write hit | `start` state: `if (isWrite() && isHit) -> locMemWrite` directly (skips `locMemRead`) | `chooseInitialState()`: `if (isWriteReq && isHit) -> DCM_LOC_MEM_WRITE` directly | **Match** | None | Direct port |
| Write miss (any) | `start` state: `!(isWrite() && isHit) -> locMemRead` (unconditional read for every other case) | `chooseInitialState()`: falls through to `DCM_LOC_MEM_READ` for every non-write-hit case | **Match** | None | Direct port |
| Read hit/miss | Same `locMemRead` entry as baseline; BEAR's read-side transitions (`waitingLocMemReadResp`+read+hit→done, `waitingLocMemReadResp`+read+miss→`farMemRead`) are textually identical to baseline's | Reads are entirely unaffected by `chooseInitialState()` — same `DCM_LOC_MEM_READ` entry, same `return_data()` dispatch as baseline | **Match** | None | Baseline's `driveState()`/`return_data()` machinery is reused unchanged for all non-`start` states, exactly mirroring gem5's `handleNextState`'s BEAR block being textually identical to baseline's |
| `handleNextState` (post-`start` transitions) | BEAR's `handleNextState` block (:1234-1341) is a byte-for-byte copy of baseline's, only guarded by `pol == BearWriteOpt` instead of `pol == CascadeLakeNoPartWrs` | This port has ONE shared `driveState()`/`return_data()`, not a per-policy copy — `chooseInitialState()` is the only policy-conditional code path | **Behaviorally match, structurally adapted** | gem5 literally duplicates the identical code per policy (a consequence of its `if (pol == X)` block style); this port factors the shared logic into one implementation | Deliberate, explained simplification — avoids maintaining N textually-identical copies of the same state machine; verified behaviorally identical via Test 14's diff, not assumed |
| Dirty-victim write-back trigger point | `handleDirtyCacheLine` called from `locMemRecvTimingResp` when the physical tag-check read completes, guarded by `pol == CascadeLakeNoPartWrs \|\| RambusHypo \|\| BearWriteOpt` (i.e. BEAR does NOT get its own write-back trigger — it reuses the exact same one) | `pushDirtyWriteBack()` called from `return_data()`'s tag-check-completion branch, unconditional on policy (reached only when a physical tag-check read actually happens, which for BEAR only occurs on misses — write hits never reach it, matching gem5's own `assert(!orbEntry->isHit)` guard inside `handleDirtyCacheLine`'s call site) | **Match** | None | Direct port; also confirms why a BEAR write hit structurally cannot trigger a write-back (no eviction on a hit, in either implementation) |
| Metadata source for the write-hit decision | `orbEntry->isHit`, computed by the SAME `checkHitOrMiss()` shared by every policy, at admission (`handleRequestorPkt`) — despite the paper's prose saying "BEAR determines write hits using metadata stored in the LLC," the actual gem5 *model* does not implement separate LLC-side metadata; it reuses the DRAM cache manager's own eagerly-computed classification | `e->isHit`, computed by the SAME `classifyAndInstall()` shared by every policy, at admission — no separate "LLC metadata" mechanism, matching gem5's actual (not prose-described) implementation | **Match** | None (matches gem5's *code*, which is the paper's own reference implementation and therefore the authoritative behavior to reproduce) | Confirmed by directly reading `policy_manager.cc` rather than relying on the paper's prose, per this stage's explicit instruction to determine "the exact metadata behavior from the actual gem5 BearWriteOpt implementation" |

No behavioral discrepancy was found between gem5's BearWriteOpt and this
port. The one structural difference (shared vs. duplicated
`handleNextState`) is a deliberate, documented simplification with no
behavioral consequence, verified by the baseline-vs-BEAR diff test.

## Oracle (`enums::RambusHypo`)

### Table II oracle, Oracle row

Paper's Table II "Tot. Oracle" row: `1 1 4 2 1 1 3 1`. Relative to
baseline (`1 1 4 3 2 2 3 2`): the two write-hit columns drop 2→1 (same as
BEAR-Wr-Opt) AND the two clean-miss columns (RdMissClean, WrMissClean)
additionally drop 3→2 and 2→1. The two dirty-miss columns (RdMissDirty,
WrMissDirty) and the two read-hit columns are unchanged from baseline.

| Case | local_read | local_write | far_read | far_write | total | Table II total | Changed from baseline? |
|---|---|---|---|---|---|---|---|
| Read Hit | 1 | 0 | 0 | 0 | 1 | 1, 1 | No |
| Write Hit | 0 | 1 | 0 | 0 | 1 | 1, 1 | **Yes** (baseline: 2) |
| Read Miss Clean (cold/clean victim) | 0 | 1 | 1 | 0 | 2 | 2 | **Yes** (baseline: 3) |
| Write Miss Clean (cold/clean victim) | 0 | 1 | 0 | 0 | 1 | 1 | **Yes** (baseline: 2) |
| Read Miss Dirty | 1 | 1 | 1 | 1 | 4 | 4 | No |
| Write Miss Dirty | 1 | 1 | 0 | 1 | 3 | 3 | No |

All six rows asserted exactly (per-operation-type) in
`tests/test_dcm_oracle.cc` Tests 1-8 (read hit, write hit, read/write ×
cold miss, read/write × clean-valid-victim miss, read/write × dirty
miss). Result: **all 6 applicable Oracle cases PASS**.

### `tests/test_dcm_oracle.cc` — Oracle policy (17 tests)

Build: `g++ -std=c++11 -Iinc -o /tmp/test_dcm_oracle tests/test_dcm_oracle.cc src/dram_cache_manager.cc src/dram_controller.cc src/block.cc`

| # | Test | Verifies | Result |
|---|---|---|---|
| 1 | Read hit | Read NOT eliminated (Oracle fetches data on a hit, not just a tag) | PASS |
| 2 | Write hit | Read eliminated; `oracleWriteHits`+1 | PASS |
| 3 | Read cold miss | Read eliminated; `oracleCleanMisses`+1; correct far-fetch + fill | PASS |
| 4 | Write cold miss | Read eliminated; correct local install | PASS |
| 5 | Read clean miss (valid, not cold) | Read eliminated; no write-back | PASS |
| 6 | Write clean miss (valid, not cold) | Read eliminated; no write-back | PASS |
| 7 | Read dirty miss | Read NOT eliminated (sources victim data); matches Table II total=4 | PASS |
| 8 | Write dirty miss | Read NOT eliminated; matches Table II total=3 | PASS |
| 9 | Replacement | New tag/address correctly overwrite old at shared index | PASS |
| 10 | Dirty eviction | Exactly one write-back; new line installs clean | PASS |
| 11 | Repeated access | 5 consecutive hits stay pure local hits | PASS |
| 12 | ORB conflict | Admission ordering unaffected by policy | PASS |
| 13 | CRB promotion → Oracle write hit | A read-cold-miss blocker (genuinely async, since Oracle's read-clean-miss path still takes real far-fetch time) lets a same-address conflicting write queue in the CRB; once promoted it resolves to a real hit and Oracle correctly eliminates its read too | PASS |
| 14 | WB interaction: write hit | A write hit never inserts into WB | PASS |
| 15 | WB pressure | WB pressure still blocks admission before `chooseInitialState()` ever runs | PASS |
| 16 | Metadata after read-clean-miss skip | Tag/valid/dirty/address correctly installed even though the physical read never happens for this path | PASS |
| 17 | Baseline vs. BEAR vs. Oracle comparison table | 6 scenarios × 3 policies, all 18 cells match Table-II-derived expectations exactly (printed table included) | PASS |

### Baseline vs. BEAR-Wr-Opt vs. Oracle comparison table (this stage's explicit ask)

Generated and verified by `tests/test_dcm_oracle.cc` Test 17 (actual
program output):

```
scenario                  | policy      | local_rd | local_wr | far_rd | far_wr
read hit                  | BASELINE    |        1 |        0 |      0 |      0
read hit                  | BEAR-Wr-Opt |        1 |        0 |      0 |      0
read hit                  | ORACLE      |        1 |        0 |      0 |      0
write hit                 | BASELINE    |        1 |        1 |      0 |      0
write hit                 | BEAR-Wr-Opt |        0 |        1 |      0 |      0
write hit                 | ORACLE      |        0 |        1 |      0 |      0
read miss clean           | BASELINE    |        1 |        1 |      1 |      0
read miss clean           | BEAR-Wr-Opt |        1 |        1 |      1 |      0
read miss clean           | ORACLE      |        0 |        1 |      1 |      0
write miss clean          | BASELINE    |        1 |        1 |      0 |      0
write miss clean          | BEAR-Wr-Opt |        1 |        1 |      0 |      0
write miss clean          | ORACLE      |        0 |        1 |      0 |      0
read miss dirty           | BASELINE    |        1 |        1 |      1 |      1
read miss dirty           | BEAR-Wr-Opt |        1 |        1 |      1 |      1
read miss dirty           | ORACLE      |        1 |        1 |      1 |      1
write miss dirty          | BASELINE    |        1 |        1 |      0 |      1
write miss dirty          | BEAR-Wr-Opt |        1 |        1 |      0 |      1
write miss dirty          | ORACLE      |        1 |        1 |      0 |      1
```

Exactly matches the paper's Table II differences: BEAR-Wr-Opt differs
from baseline ONLY on "write hit" (local_read 1→0); Oracle differs from
baseline on "write hit" (same as BEAR) AND on both clean-miss rows
(local_read 1→0, everything else unchanged); dirty-miss rows and the
read-hit row are identical across all three policies. No cell deviates
from what the paper's Table II predicts.

### gem5 comparison table (this stage's explicit ask, with A/B/C classification)

Direct comparison against `policy_manager.cc`'s `enums::RambusHypo`
branches (`setNextState`: :786-893; `handleNextState`: :1128-1231, read
directly and confirmed textually identical in shape to baseline's, same
pattern as BEAR-Wr-Opt), read from the previously-pulled gem5 source
before writing any code. Classification: **C = directly reproducible**,
**B = reproducible with adaptation**, **A = not reproducible** (trace-
based/ChampSim architectural limits) — see master instructions.

| Scenario | gem5 Oracle behavior | ChampSim Oracle behavior | Match? | Difference | Reason | Class |
|---|---|---|---|---|---|---|
| Write hit | `start`: `(!isRead && isHit) -> locMemWrite` (skip `locMemRead`) | `chooseInitialState()`: `isWriteHit -> DCM_LOC_MEM_WRITE` | **Match** | None | Direct port | C |
| Read hit | `start`: `(isRead && isHit) -> locMemRead` (read NOT skipped) | Falls through to `DCM_LOC_MEM_READ` | **Match** | None | Direct port — confirms Oracle only ever eliminates a *tag check*, never a data fetch | C |
| Read/write clean or cold miss | `start`: `(isRead && !isHit && !isDirty) -> farMemRead`; `(!isRead && !isHit && !isDirty) -> locMemWrite` (both skip `locMemRead`) | `chooseInitialState()`: `isCleanMiss -> (isWriteReq ? DCM_LOC_MEM_WRITE : DCM_FAR_MEM_READ)` | **Matches the PAPER; deliberately differs from gem5's literal code** | gem5's `isDirty` is read AFTER `handleRequestorPkt()` has eagerly overwritten the metadata (call order: `recvTimingReq` line 366 then 376; `isDirty` computed at line 683), so post-update a read miss is always "clean" and a write always "dirty". gem5's WrMissClean therefore costs 2 accesses, not Table II's 1 | This port uses the OLD RESIDENT (victim) dirty bit, per the paper's Section V wording and Table II. Reproducing gem5 literally would break Table II conformance. **Corrected claim** — an earlier version of this row asserted gem5 read the victim's bit; it does not | B |
| Read/write dirty miss | `start`: `(isRead&&!isHit&&isDirty) \|\| (!isRead&&!isHit&&isDirty) -> locMemRead` (read NOT skipped — sources the victim's data) | Falls through to `DCM_LOC_MEM_READ` | **Matches the PAPER**; gem5's read-miss-dirty branch is in fact unreachable (post-update `isDirty` is false for any read miss), so gem5 costs 2 accesses where Table II says 4, and never writes the dirty victim back | gem5's eager-update ordering (see row above) | This port implements the paper: RdMissDirty = 4 accesses including the write-back, verified | B |
| Post-`start` transitions (`waitingLocMemReadResp`, `waitingFarMemReadResp`, `waitingLocMemWriteResp`) | RambusHypo's `handleNextState` block is textually the same shape as baseline's (just `pol == RambusHypo` instead) | Reused, unmodified `driveState()`/`return_data()` — same as BEAR-Wr-Opt's port | **Behaviorally match, structurally adapted** | gem5 duplicates identical code per policy; this port shares it | Deliberate, explained simplification, same as BEAR-Wr-Opt — verified via the comparison table (Test 17), not assumed | C |
| Dirty-victim write-back trigger | `handleDirtyCacheLine` from `locMemRecvTimingResp`, guarded by `pol == CascadeLakeNoPartWrs \|\| RambusHypo \|\| BearWriteOpt` — same single trigger for all three policies | `pushDirtyWriteBack()` from `return_data()`, unconditional on policy | **Match** | None | Direct port; also structurally guaranteed correct for Oracle since dirty-miss cases are exactly the ones that still visit `DCM_LOC_MEM_READ` | C |
| "Zero-latency SRAM" modeling | gem5 does not actually add or remove any latency for Oracle's tag store beyond skipping the physical `locMemRead` dispatch — there is no separate "SRAM access time" charged anywhere in the code | Same: `chooseInitialState()` charges no additional latency; skipping `DCM_LOC_MEM_READ` is the entire modeled effect | **Match** | None | Confirmed by reading the code, not assuming from the paper's "zero-latency SRAM" description — gem5's implementation is simpler than the prose suggests (it doesn't model an SRAM access at all, zero-latency or otherwise; it just never issues the DRAM read) | C |

No behavioral discrepancy was found between gem5's `RambusHypo` and this
port. Every applicable scenario is class **C** (directly reproducible) —
Oracle's entire behavioral surface is contained within the DRAM cache
manager's own request-classification and dispatch logic, none of it
depends on anything ChampSim's trace-based architecture cannot represent
(unlike, say, the paper's Linux-boot-based warmup methodology, which is
class A and already documented in `limitations.md`).

## bypassDcache / "No-DRAM-Cache" comparison mode (stage 8)

Full architectural writeup, gem5 call sites, and request-flow diagrams
in `docs/bypass_mode.md`; gem5-comparison classification table in
`gem5_to_champsim_mapping.md`. Test results below.

### `tests/test_dcm_bypass.cc` — bypassDcache (15 test blocks)

Build: `g++ -std=c++11 -Iinc -o /tmp/test_dcm_bypass tests/test_dcm_bypass.cc src/dram_cache_manager.cc src/dram_controller.cc src/block.cc`

| # | Test | Verifies | Result |
|---|---|---|---|
| 1 | Bypass read | Completes and reaches the LLC with the correct address | PASS |
| 2 | Bypass write | Reaches `farMC`'s WQ (drained exactly once); no LLC callback (matches this port's write contract in every mode) | PASS |
| 3 | Multiple reads | 4 independent reads all complete | PASS |
| 4 | Multiple writes | 4 independent writes all drain to `farMC` | PASS |
| 5 | Mixed read/write | 2 reads complete, 2 writes drain, independently | PASS |
| 6 | Repeated access | 3 successive reads to the SAME address each independently complete (no DCM-side caching at all in bypass mode) | PASS |
| 7 | Same-DRAM-cache-index addresses | Two addresses that would conflict via the CRB in normal mode both admit and complete immediately in bypass mode; `CRB`/`ORB` stay empty throughout | PASS |
| 8 | Dirty/write behavior | Write then read the same address never populates `WB` or increments `numWrBacks` | PASS |
| 9 | Response/completion correctness | All 3 distinct addresses in a batch are delivered exactly once each | PASS |
| 10–13 | No DCM-side operation at all | `tagMetadataStore` unmodified before/after; `localReads`/`localWrites`/`sentToNear`/`completedFromNear` all 0; `ORB`/`CRB` stay empty; `WB`/`numWrBacks`/`wbInsertions` all 0/empty; `numTotHits`/`numTotMisses`/`cacheFills` all 0; `nearMC.RQ[0].ACCESS` stays 0 | PASS |
| 14 | Far memory receives requests exactly | `farMC`'s RQ/WQ completion counts (`ROW_BUFFER_HIT+MISS`) match exactly (5 reads, 3 writes); `nearMC` sees nothing | PASS — after fixing a test-only measurement bug (see below) |
| — | Normal vs. bypass comparison | Identical workload: normal mode shows real `localReads`/`cacheFills`/etc. activity; bypass mode shows exactly 0 for every pre-existing counter, with `bypassReads`/`bypassWrites`/`bypassCompletedReads` correctly reflecting the bypass-side activity instead | PASS |
| — | Timing: link latency composes, controller latency does not | A configured far-link latency still adds its (approximate, per the established `test_dcm_link_latency.cc` tolerance convention) delay in bypass mode; a configured (nonzero, default) controller latency adds EXACTLY 0 — proving it genuinely does not apply, not merely untested | PASS |
| — | Statistics isolation | A normal-mode instance that never enables bypass shows `bypassReads`/`bypassWrites`/`bypassCompletedReads` all 0, and its normal counters are unaffected by the feature existing | PASS |

**A real functional bug in the implementation was found and fixed while
writing this test file** (not a test-methodology artifact): bypass
requests were silently accepted into `farMC`'s queue but never
scheduled or completed, because `dispatchToFar()`'s callers on the
normal path always stamp `event_cycle` to "now" before dispatch, and
bypass mode's new code path skipped that stamping (it bypasses
`admitRequest()`/`driveState()`, which is what would otherwise have done
it). `PACKET`'s default constructor leaves `event_cycle` at
`UINT64_MAX`, which `MEMORY_CONTROLLER` never schedules. Fixed by
stamping `packet->event_cycle = current_core_cycle[packet->cpu]` in both
`add_rq`'s and `add_wq`'s bypass branches immediately before calling
`dispatchToFar()`. Caught by Test 1 failing outright (0 responses within
a 2,000,000-cycle timeout) before the fix.

**One test-only measurement bug was also found and fixed**: Test 14
initially checked `farMC.RQ[0].ACCESS` as a "was this read received"
signal, but that counter is only incremented by
`MEMORY_CONTROLLER::add_rq`'s pre-existing write-queue-forwarding
shortcut (`dram_controller.cc:448`, the same artifact documented in
`tests/test_dcm_controller_latency.cc`'s banner comment), not on normal
admission. Fixed by using `ROW_BUFFER_HIT + ROW_BUFFER_MISS` (the actual
per-completion counters, incremented for both RQ and WQ) instead — the
same completion-count signal already used correctly for writes in Tests
2/4/5.

Full regression: all 9 pre-existing `tests/test_dcm_*.cc` suites pass
**unchanged** — zero expected-result modifications needed anywhere this
stage (unlike stage 7's one documented `test_dcm_link_latency.cc`
change), since `bypassDcache` defaults to `false` and every pre-existing
code path is untouched when it is. Full-binary build succeeds and a
smoke-test run produces output **byte-identical** to the pre-stage-8
build.

## WB → far-memory write acceptance/retry (stage 9)

Fixes the silent-write-loss bug found by the dedicated audit
(`docs/wb_retry_audit.md`). Full architectural writeup, exact fix, and
gem5 correspondence table in `docs/wb_retry_audit.md`'s "RESOLUTION"
section; results summarized here.

### `tests/test_dcm_wb_retry.cc` — WB retry/acceptance (9 tests + stress test)

Build: `g++ -std=c++11 -Iinc -o /tmp/test_dcm_wb_retry tests/test_dcm_wb_retry.cc src/dram_cache_manager.cc src/dram_controller.cc src/block.cc`

| # | Test | Verifies | Result |
|---|---|---|---|
| 1 | Capacity available | A WB write with room at `farMC` is accepted and its WB entry removed | PASS |
| 2 | Capacity full | A WB write with NO room at `farMC` is NOT removed from WB, and NOT lost | PASS |
| 3 | Capacity frees up | A retained WB entry succeeds on a later cycle once `farMC` has room, removed exactly once (no duplicate dispatch) | PASS |
| 4 | Multiple entries | 20 WB entries, none lost, none duplicated | PASS |
| 5 / stress | Sustained pressure | 3 waves × 50 dirty evictions (150 total), each wave fully retained/blocked (via a deterministic test-only `forceWqFull` override) before release: `total_generated=150 total_accepted=150 lost=0 duplicated=0`, `wbDispatchRetries` in the thousands confirming capacity was genuinely, repeatedly exceeded | PASS |
| 6 | Mixed traffic | Far-read dispatches (cold misses) and far-write dispatches (WB drains) share `farMC` correctly, admission-retried via a helper that mirrors real LLC backpressure handling | PASS |
| 7 | Ordering preserved | WB entries drain to `farMC` in their exact original insertion order, even when several are blocked/retried | PASS |
| 8 | Parent accounting | The evicting request's own ORB entry completes independently of whether its victim's write-back is retained/retried (isolated via `forceWqFull`, which blocks only the WQ, not the real `write_mode` arbitration that would otherwise also throttle the parent's own far-read) | PASS |
| 9 | WB statistics | `wbMaxOccupancy`/`wbInsertions`/`wbDrains`/`wbDispatchRetries` all reconcile exactly, both while entries are blocked and after they drain | PASS |

**Four real test-methodology bugs were found and fixed while writing
this file** (all in the test's own methodology, none in the
implementation — the same general category as bugs already documented
in `test_dcm_controller_latency.cc`'s and `test_dcm_bypass.cc`'s banner
comments):

1. `pumpUntilQuiescent`'s original condition
   (`WB.empty() && farMC.WQ.occupancy==0`) was trivially satisfied
   instantly, before the just-issued request had even been dispatched —
   fixed by also requiring `dcm.ORB.empty()`.
2. Test addresses generated as `base + i*STRIDE` (where
   `STRIDE = DCM_DRAM_CACHE_SIZE`) all collided on the SAME DRAM-cache
   index instead of being distinct — fixed by generating distinct
   addresses as `base + i*DCM_BLOCK_SIZE`, reserving `+STRIDE` only for
   pairing an address with the eviction targeting its specific index.
3. `makeReadPacket`/`makeWritePacket` never stamped `event_cycle`
   (defaults to `UINT64_MAX`), fatal for packets injected DIRECTLY into
   a `MEMORY_CONTROLLER` (this file's "filler" packets) since
   `update_schedule_cycle()` can never schedule an entry stuck at
   `UINT64_MAX` — fixed by stamping `p.event_cycle = current_core_cycle[0]`.
4. An initial single-wave stress-test design (100 simultaneously-blocked
   WB entries) conflicted with `DCM_WB_PRESSURE_THRESHOLD`'s own
   pre-existing, correct admission-time backpressure (caps WB at 64
   entries by design) — redesigned as 3 successive waves of 50, proving
   sustained pressure across repeated cycles instead.

Full regression: all 10 pre-existing `tests/test_dcm_*.cc` suites pass
**unchanged, with zero expected-result modifications anywhere**.
Full-binary build succeeds and a smoke-test run produces output
**byte-identical** to the pre-stage-9 build. The original audit's
direct-to-`MEMORY_CONTROLLER` diagnostic (bypassing the DCM) still
reproduces its original result unchanged (36 of 100 writes lost) —
expected, since that primitive itself was deliberately not modified
(the fix ensures the DCM's own dispatch code never calls into it in a
way that could trigger the bug, per the audit's explicit "prefer fixing
the DCM WB dispatch path" instruction).

## All remaining DCM → memory dispatch paths (stage 10)

Follow-up audit and fix for the 5 other exposed dispatch call sites
(near tag-check read, near direct write, near cache-fill write, far
demand read, bypass read, bypass write). Full architectural writeup,
audit table, and gem5 correspondence in `docs/memory_dispatch_audit.md`;
results summarized here.

### `tests/test_dcm_memory_dispatch.cc` — all remaining dispatch paths (12 tests + 3 stress tests)

Build: `g++ -std=c++11 -Iinc -o /tmp/test_dcm_memory_dispatch tests/test_dcm_memory_dispatch.cc src/dram_cache_manager.cc src/dram_controller.cc src/block.cc`

| # | Test | Verifies | Result |
|---|---|---|---|
| 1 | Near tag-check read, RQ full | Retained (not lost) while `nearMC`'s RQ is full; succeeds exactly once once capacity frees up | PASS |
| 2 | Near cache-fill write, WQ full | The LLC response is NOT delayed by a blocked fill; the fill itself is retained and succeeds exactly once | PASS |
| 3 | Far demand read, RQ full | Retained (not lost) while `farMC`'s RQ is full; succeeds exactly once | PASS |
| 4 | Far write-back, WQ full | Confirms the already-fixed (stage 9) path still works, for this file's completeness | PASS |
| 5 | Bypass read, RQ full | Retained; `bypassOutstandingReads` entry not leaked; succeeds exactly once, tracking set correctly cleared | PASS |
| 6 | Bypass write, WQ full | Retained (closing the one gap stage 9 left explicitly open); succeeds exactly once | PASS |
| 7 | Queue becomes available later | Combined near+far+release check | PASS |
| 8–11 | Multiple pending / ordering / no duplicates / no lost | 8 concurrent near reads AND 8 concurrent far reads, each forced to queue simultaneously, released together: exact original insertion order preserved, all complete, none lost or duplicated | PASS (both near and far) |
| 12 | Request/parent-ID correctness | 5 concurrent near reads to distinct addresses, all pending at once, each resolves to its OWN correct LLC response (no cross-attribution) | PASS |
| Stress 1 | Near read ×40 | `generated == accepted == completed == 40`, `lost=0 duplicated=0` | PASS |
| Stress 2 | Far read ×40 | Same, far side | PASS |
| Stress 3 | Near fill write ×20 | `generated == accepted == 20` fills, all reach `nearMC`, independent of already-sent LLC responses | PASS |

**Two real test-methodology bugs were found and fixed while writing
this file** (both in the shared "quiescent" helper, the same general
category of issue already documented in the other test
files' banner comments):
1. The quiescent check omitted `pendingResponses.empty()`: `ORB.empty()`
   becomes true as soon as `completeRequest()` runs, but a nonzero
   (default) controller frontend/backend latency means the actual
   LLC-visible response can still be queued in `dcm.pendingResponses`
   for a while afterward — the check reported "done" one step early.
2. Bypass-mode traffic has NO ORB entry at all, so `ORB.empty()` gave
   zero signal about whether a bypass request's real
   `MEMORY_CONTROLLER` processing had actually finished — fixed by also
   checking `near_mc`/`far_mc`'s own RQ occupancy directly, the only way
   to know nothing is genuinely still in flight, ORB-tracked or not.

Full regression: all 11 pre-existing `tests/test_dcm_*.cc` suites pass
**unchanged, with zero expected-result modifications anywhere**
(including `test_dcm_wb_retry.cc`, confirming `dispatchToFar()`'s
refactor to use the new shared `trySend()` primitive is behavior-
identical). Full-binary build succeeds and a smoke-test run produces
output byte-identical to the pre-stage-10 build (ignoring the wall-clock
"Simulation time" line).

## Case Study experiment harness (stage 11)

Full trace investigation, configuration definitions, metrics list, and
per-Case-Study classification in `docs/case_study_reproduction_plan.md`.
Validation summary:

- **Default-behavior regression**: `./bin/champsim` with no new flags
  produces output that is a strict superset of the pre-stage-11 build's
  output — `diff` confirms every pre-existing line unchanged, only new
  lines (`DRAM Cache Manager policy: ...` and the `print_dcm_stats()`
  block) appended.
- **All 12 pre-existing `tests/test_dcm_*.cc` suites** pass unchanged
  (these exercise the DCM library directly, independent of the
  `main.cc` CLI layer this stage adds; confirmed unaffected by rerunning
  the full suite anyway).
- **7/7 configurations sanity-tested** at ≤200K-instruction scale on
  `403.gcc-16B.champsimtrace.xz` (`docs/case_study_reproduction_plan.md`
  Section 6): `NO_DRAM_CACHE`, `BASELINE`, `BEAR_WR_OPT`, `ORACLE`,
  `BASELINE_100NS`, `BASELINE_500NS`, `BASELINE_1000NS` — all exit 0,
  all print the correct active configuration, and all show the expected
  qualitative behavior (Oracle beats baseline; BEAR matches baseline
  exactly in a write-hit-free window; link latency increases cycles
  monotonically with no change to local/far operation counts; bypass
  mode shows ONLY its 3 dedicated counters, confirming the DCM's own
  ORB/CRB/tag-metadata machinery is genuinely not exercised).
- **`run_case_studies.sh --dry-run`**: verified to print the exact
  command, flags, and output path for every configuration/trace
  combination without executing anything (demonstrated in this stage's
  own working session, not merely claimed).
- **No long simulation was launched** — the exact command for a real
  reproduction run is documented (`docs/case_study_reproduction_plan.md`
  Section 9) but not executed, per explicit instruction.

## Pre-flight verification + one confirmed 600M-instruction run

> **SUPERSEDED — the results in this section are INVALID.** They were
> produced before the stage-15 DRAM-cache address-granularity fix
> (`docs/final_independent_audit.md` §16), so every hit/miss number below
> is distorted by 64-line aliasing. In particular the 99.87% hit rate is
> an artifact of the defect, not a workload property. The section is kept
> only as a record of the infrastructure check it performed. The current
> baseline of record is the stage-16 pre-flight
> (`BASELINE` / `462.libquantum-1343B`, 79.37% hit rate) —
> see `docs/case_study_reproduction_plan.md` §11 and "Stage 16" below.

Full detail in `docs/case_study_reproduction_plan.md` Section 10.
Summary:

- **All 7 configurations' exact commands verified** via
  `./run_case_studies.sh --dry-run --warmup 100000000 --sim 500000000`
  (the real reproduction scale) — every policy/bypass/link-latency flag,
  trace path, and instruction count matched Section 4's configuration
  table exactly, across all 42 configuration×trace combinations.
- **Output-filename uniqueness verified programmatically**: the 42
  dry-run output paths piped through `sort | uniq -d` produced zero
  duplicates.
- **DCM statistics reset per run confirmed by source inspection**:
  `UNCORE uncore;` (`src/uncore.cc:4`) is a single global, default-
  constructed once per OS process; each `run_case_studies.sh` invocation
  is a separate process, so there is no code path for cross-run
  contamination.
- **One real 600M-instruction run performed** (`BASELINE`,
  `403.gcc-16B.champsimtrace.xz`, no other configuration or trace):
  completed successfully in 18 min 58 sec, exit 0, well-formed 148-line
  output.
- **Request-loss/retry anomaly check, done programmatically**: 10 of 11
  DCM-internal count invariants (reads+writes==total, hits+misses==total,
  far_reads==miss-count, wb_insertions==wb_drains, etc.) hold EXACTLY;
  the 11th (`local_writes == writes + far_reads`) is short by exactly 3
  out of 3,252,843 (0.00009%), fully explained as 3 background fill
  writes still in flight at the exact simulation-instruction cutoff (the
  far reads that trigger them are ALL accounted for exactly — nothing
  was lost, only an asynchronous, off-critical-path write hadn't yet
  been dispatched at the snapshot instant). All 6 retry/reject counters
  (`WB_DISPATCH_RETRIES`, `NEAR_DISPATCH_RETRIES`, `FAR_DISPATCH_RETRIES`,
  `WB_FULL_REJECTS`, `CRB_FULL_REJECTS`, `ORB_FULL_REJECTS`) are exactly
  zero — this trace/scale never exhausted any queue's capacity, so the
  guaranteed-delivery mechanisms from the prior two stages were not
  exercised here (they remain verified by their own dedicated unit/
  stress tests instead). **Conclusion: no request-loss or retry anomaly
  found.**
- **Throughput measured**: 600,000,002 total instructions in 18:58
  wall-clock — used to replace the earlier untested "typically hours"
  estimate for the full 42-run matrix with a real projection (~13 hours
  serial, well under 2 hours with modest parallelism).

## Stage 13 — CRITICAL/HIGH audit fixes: verification

### Reproduction-then-fix evidence

Each defect was independently re-reproduced *before* fixing, using the
probes written during the audit, and then re-run after:

| Defect | Before fix | After fix |
|---|---|---|
| CRITICAL-1 (read+read via CRB promotion) | `responses=1 (expect 2)`, `ORB residual=1`, `localReads=3` but `completedFromNear=2` | `responses=2`, `ORB residual=0`, `localReads=3 == completedFromNear=3` |
| CRITICAL-1 (read+writeback, LLC-reachable) | `ORB residual=1`, `completedFromNear=2` | `ORB residual=0`, `completedFromNear=3` |
| CRITICAL-2 (bypass duplicate read) | `bypassCompletedReads=1`, `LLCresponses=1`, `outstandingLeak=1` | `bypassCompletedReads=2`, `LLCresponses=2`, `outstandingLeak=0` |
| HIGH-1 (warmup cycles > ROI cycles) | `LOCAL_BW_UTILIZATION: 1.73472e-16%` | `LOCAL_BW_UTILIZATION: 0.0286947%  (over 83639 ROI cycles)` |
| HIGH-2 (cold cache) | `WARMUP_TAG_UPDATES` did not exist; DCM inert during warmup | `WARMUP_TAG_UPDATES: 27351` on the 600M run; ROI hits on warmed lines |

**Table II was re-measured after every change: 24/24 cells exact for all
three policies.** No policy behaviour, architectural semantic, or
pre-existing test expectation changed.

### New test suites

- `tests/test_dcm_duplicate_merge.cc` — 6 blocks: two concurrent reads
  via CRB promotion; the LLC-reachable read+writeback pairing; index not
  poisoned by the collision; **no duplication** (three `add_rq` attempts
  but exactly two serviced reads — one refused and retried); bypass
  duplicate read; and the same collision under BEAR and Oracle.
- `tests/test_dcm_bandwidth_stats.cc` — 6 blocks: hand-derived exact
  values; warmup-longer-than-ROI (also demonstrating the old expression
  wrapping to 18446744073709158173 cycles → 4.3e-14%); very short and
  oversubscribed ROI; degenerate inputs producing no NaN/inf; purity;
  and the paper's near/far peaks.
- `tests/test_dcm_warmup.cc` — 8 blocks: cold before warmup; warmup
  installs lines with zero ROI side effects; ROI access to a warmed line
  HITS; `resetROIStats()` clears counters but keeps the cache warm;
  warmup dirty state survives and drives a write-back; warming identical
  under all three policies; bypass excluded; no loss/duplication during
  warmup.

Three test-authoring bugs were found and fixed while writing these (all
in the tests, none in the implementation): a stale row-buffer baseline in
the no-duplication check, and two cases of using `STRIDE`
(= cache size, which maps to the SAME index) where DISTINCT indices were
intended — the same address-arithmetic trap already documented in
`test_dcm_wb_retry.cc`.

### Full regression

**15/15 suites pass** — the 12 pre-existing suites unchanged and
unweakened, plus the 3 new ones.

### 600M pre-flight re-run (BASELINE, `403.gcc-16B`)

Command:
```
./bin/champsim -warmup_instructions 100000000 -simulation_instructions 500000000 \
    -traces /home/dpsingh/Desktop/ChampSim-master/dpc3_traces/403.gcc-16B.champsimtrace.xz
```
Completed in 18 min 48 sec, exit 0.

| Check | Result |
|---|---|
| Warmup actually modifies DCM state | **Yes** — `WARMUP_TAG_UPDATES: 27351` (was structurally impossible before) |
| Warmup timing unchanged | **Yes** — warmup completed at cycle 46,674,846, byte-identical to the pre-fix run |
| Bandwidth statistics valid | **Yes** — `LOCAL_BW_UTILIZATION: 6.49259%  FAR_BW_UTILIZATION: 0.00991609%  (over 1693677975 ROI cycles)`; denominator now printed and equals the reported ROI cycle count |
| Merge defect triggered? | **No** — `DISPATCH_MERGE_RETRIES: 0`; also `NEAR/FAR/WB_DISPATCH_RETRIES: 0`, `CRB_FULL_REJECTS: 0`, `ORB_FULL_REJECTS: 0` |
| Reads conserved | **Exact** — `COMPLETED_READS_TO_LLC 7253888 == READS 7253888` |
| Far reads conserved | **Exact** — `COMPLETED_FROM_FAR 12382 == FAR_READS 12382` |
| Write-backs conserved | **Exact** — `WB_INSERTIONS 214 == WB_DRAINS 214 == FAR_WRITES 214` |
| ROI statistics separated | **Yes** — DCM counters reset at the boundary; `WARMUP_TAG_UPDATES` deliberately preserved |

**Request-conservation invariants — all exact.** Every count reconciles:
`reads+writes == total`, `hits+misses == total`, `cold+hot == misses`,
per-case misses == misses, `localReads == total` (baseline always
tag-checks), `farReads == read misses`, `cacheFills == misses`, and
crucially
`localWrites (3251972) == completedWrites (3239590) + completedFromFar (12382)` — **exact**.

**Residual: ORB 3, CRB 0, WB 0**, reported as
`[OK: in flight at cutoff -- ChampSim stops without draining]`. This is
*not* a leak, and the new instrumentation is what makes that provable:
the 3 residual entries are write requests whose tag-check read was
outstanding at the instruction cutoff, which explains the write shortfall
(3), the near-read shortfall (3) and the `localWrites` identity above
simultaneously and exactly. `DISPATCH_MERGE_RETRIES: 0` independently
confirms the merge defect never fired. The earlier pre-fix run showed a
similar 3-request discrepancy that was *unverifiable* at the time — that
ambiguity is now resolved by direct measurement.

### Cold-miss ratio — honest characterisation

Warming demonstrably works: misses fell from 13,638 → 12,383 and cold
misses from 13,106 → 11,851 (−9.6%) for the identical workload.

However, cold misses remain ~96% of all ROI misses. **This is now a
workload property, not a simulator defect.** The LLC absorbs nearly
everything on this trace: only 27,351 DCM requests occur across the
entire 100M-instruction warmup, so barely 1.3% of the 128 MB DRAM
cache's 2,097,152 lines are ever warmed, and the ROI then first-touches
addresses warmup never saw. Those misses are genuinely compulsory.

The implication for the Case Studies stands and should be stated plainly
in any write-up: **`403.gcc` does not exercise the DRAM cache
meaningfully** (99.9% hit rate, ~12K misses over 500M instructions).
Selecting memory-intensive traces — and/or a longer warmup — matters far
more for Case Study validity than the warmup mechanism itself, which is
now correct.

## Stage 14 — final pre-experiment hardening (HIGH-3, MEDIUM-2, MEDIUM-3)

### HIGH-3 — inaccurate gem5 `isDirty` claim corrected (comments/docs only)

No behaviour changed. The `chooseInitialState()` comment in
`inc/dram_cache_manager.h` previously asserted that gem5's `isDirty`
reads the OLD resident line. It does not: `recvTimingReq` calls
`handleRequestorPkt()` (which eagerly overwrites the metadata) at line
366 and only then `setNextState()` at line 376, so `checkDirty()` at line
683 reads POST-update state. The comment now states the truth — that this
port implements the **paper** (Section V + Table II), deliberately
diverging from gem5's literal code, which contradicts its own Table II
in two cells (RdMissDirty 2 vs 4, WrMissClean 2 vs 1) and never writes
back a dirty victim evicted by a RambusHypo read miss. The same false
claim was corrected in `docs/feature_coverage.md`,
`docs/implementation_status.md`, `docs/request_flow.md` and this file's
gem5 comparison table (two rows re-classified from "Match / Direct port"
to "Matches the PAPER; deliberately differs from gem5's literal code").

**Table II re-measured after the change: 24/24 exact.**

### MEDIUM-2 — harness failure detection

`run_case_studies.sh` gained a `validate_run()` gate. A run is accepted
only if every check passes; the script now tracks valid/invalid/skipped
counts, lists every bad run, and **exits non-zero** so a broken matrix
cannot be mistaken for a good one.

Verified by injecting each failure mode into a real result file:

| Injected condition | Detected |
|---|---|
| valid normal run (control) | **accepted** (no false positive) |
| valid BYPASS run (control) | **accepted** (no false positive) |
| simulator exit code non-zero | rejected |
| output file missing | rejected |
| output file empty | rejected |
| output truncated (`ChampSim completed all CPUs` absent) | rejected |
| required statistic missing (normal mode) | rejected |
| required statistic missing (bypass mode) | rejected |
| instruction count not reached | rejected |
| assertion / segfault / abort signature | rejected |
| DCM reports itself jammed | rejected |

One false positive was found and fixed during that verification: bypass
runs legitimately print a short-form statistics block (only the three
`BYPASS_*` counters), so the validator is now **mode-aware** and checks
the appropriate field set. Without that, every valid `NO_DRAM_CACHE` run
would have been rejected.

**Sanity experiment** — all 7 configurations, `403.gcc-16B`,
200 K warmup / 300 K sim: `valid: 7  INVALID: 0  skipped: 0`, harness
exit code 0.

### MEDIUM-3 — real LLC integration test

`tests/test_dcm_llc_integration.cc` links the **actual** ChampSim
`CACHE` class (plus the real replacement and prefetcher modules), wires a
real LLC's `lower_level` to a real `DRAM_CACHE_MANAGER`, and drives
traffic through `CACHE::add_rq()`/`add_wq()`. No architectural change was
required — the DCM already presents the standard `MEMORY` interface.
`lg2()`/`va_to_pa()` are provided test-locally because they live in
`src/main.cc`, which owns `main()` and cannot be linked into a test.

| # | Test | Result |
|---|---|---|
| 1 | Real LLC read miss → DCM → response, full wiring | PASS |
| 2 | LLC hit is absorbed by the LLC and never reaches the DCM | PASS |
| 3 | **The audit's LLC-reachable scenario**: same-address demand read + dirty writeback (independent paths — the MSHR merges only reads); no deadlock, nothing stranded, index not poisoned, follow-up traffic still works | PASS |
| 4 | Sustained mixed traffic; request conservation holds exactly | PASS |
| 5 | Bypass mode through the real LLC; DRAM cache never engaged | PASS |
| 6 | All three policies end-to-end | PASS |
| 7 | **DIAGNOSTIC**: address granularity across the LLC boundary | PASS (characterisation — see below) |

Test 6 recorded a genuine property of the real hierarchy: BEAR shows no
advantage here because its optimisation targets DRAM-cache **write
hits**, and a writeback whose block is resident in the LLC is absorbed by
the LLC and never forwarded downward. That is correct behaviour, not a
policy defect, and it is only visible with a real LLC in the path. The
assertion was written to match (BEAR ≤ baseline; Oracle < baseline via
its clean-miss exemption).

**Test 7 discovered a NEW CRITICAL defect** — the real LLC passes BLOCK
addresses while `returnIndexDC()` divides by `DCM_BLOCK_SIZE` again, so
64 distinct 64-byte lines alias onto one DRAM-cache index (effective line
size 4096 B, effective capacity 8 GB). It is **documented and NOT fixed**
(out of this pass's scope); see `docs/final_independent_audit.md` §16.
Test 7 is a characterisation test: it asserts the current, defective
behaviour and prints a loud warning, so the defect cannot be forgotten,
and its assertion must be inverted when the defect is fixed.

### Regression

**16/16 suites pass** — the 15 from before plus the new real-LLC
integration suite. Table II: **24/24 exact**. Full binary rebuilds clean.

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

### Stage 15 verification evidence (address granularity)

**Tests added / changed**

| Test | What it asserts |
|---|---|
| `test_dcm_llc_integration.cc` Test 7 | Two adjacent 64 B lines (`0x40000000`, `0x40000040`) driven through the **real LLC** land on indices 0 and 1, produce `misses == 2, hits == 0`, and are resident simultaneously. Previously a *characterisation* test asserting the aliasing; now a correctness assertion. |
| Test 8 (new) | Same-index / different-tag conflict: line *L* and line *L + DCM_NUM_LINES* collide; the dirty victim is replaced and produces a far-memory writeback. |
| Test 9 (new) | Entry-path equivalence: for the same logical byte addresses, direct packet injection and real-LLC-generated packets yield identical line address, index, tag, hit/miss classification, resulting metadata, and local/far operation counts. |
| Test 10 (new) | Capacity/granularity invariants: `DCM_NUM_LINES * DCM_BLOCK_SIZE == DCM_DRAM_CACHE_SIZE == 128 MiB`; line *L* and *L + DCM_NUM_LINES* share an index and differ in tag by exactly 1. |
| `test_dcm_baseline.cc`, `test_dcm_bear.cc`, `test_dcm_oracle.cc` (18 sites), `test_dcm_warmup.cc` (`idxOf`) | Hand-rolled copies of the *old* index formula, used to locate metadata slots, now call `dcm.returnIndexDC()`. **No expected value changed.** |

**Regression result**: 16/16 suites pass. **Table II: 24/24 exact**
(per-operation breakdown across all three policies), re-measured after
the fix — expected values were not altered to compensate.

**Short real trace through the actual binary**, A/B on the same trace
with only the index formula differing:

| metric | before (defective) | after (correct) |
|---|---|---|
| LLC misses | 422 | 422 |
| DCM hits | 395 | **0** |
| DCM misses | 27 | **422** |
| cache fills | 27 | **422** |
| far reads | 27 | **422** |
| local writes | 27 | **422** |
| DCM hit rate | 93.6% | **0.0%** |

Identical LLC miss counts on both sides prove the change is DCM-internal:
nothing upstream was perturbed. The 395 pre-fix "hits" were pure
aliasing — 422 distinct 64-byte lines collapsing into 27 4-KiB
super-lines (~15.6 lines each), exactly the ratio the defect predicts.
The post-fix 0% hit rate is the correct result for a short trace with no
reuse at 64 B granularity. Source restored to the fixed version and
confirmed byte-identical (`diff -q`) before the final regression run.


## Stage 16 verification evidence (post-fix 600M production pre-flight)

One 600M-instruction run (100M warmup / 500M ROI), `BASELINE`
(CascadeLakeNoPartWrs), no bypass, on `462.libquantum-1343B` — the most
memory-intensive **valid** trace, selected by measurement (LLC MPKI 22.66,
~2x the next candidate; `619.lbm_s` re-confirmed corrupt via `xz -t`).
Full configuration verification table, results, and sanity analysis:
`docs/case_study_reproduction_plan.md` §11.

**Headline**: IPC 0.275061 over 1,817,776,339 ROI cycles; 12,736,699 LLC
misses; 17,763,568 DCM requests; **79.37% DCM hit rate** (read 71.37%,
write 99.63%); 3,665,269 misses of which only **27 are cold**; access
amplification 1.77506; local BW 11.63%, far BW 3.74%.

**Conservation: 18/18 invariants exact**, including
`FAR_READS == RD_MISS_CLEAN + RD_MISS_DIRTY`,
`RD_MISS_DIRTY + WR_MISS_DIRTY == WB_INSERTIONS == WB_DRAINS == FAR_WRITES`,
`LOCAL_WRITES == CACHE_FILLS + WR_HIT`, and every `COMPLETED_*` counter
matching its source. All dispatch, merge, and full-reject counters are
**0**, and the residual line reads `ORB 0 CRB 0 WB 0 pendingNear 0
pendingFar 0 pendingResp 0 [OK: fully drained]` — nothing stranded,
nothing duplicated, no unexplained background operations at the cutoff.
Passes the harness's own `validate_run()`.

**Granularity behaviour confirmed independently of the hit rate**:
adjacent 64 B lines map to consecutive distinct indices (integration
Test 7 + compiled-constant probe); repeated access to one line hits
(baseline "5x read hit"; warmup Test 3); same-index/different-tag
addresses conflict and evict dirty (Test 8 — and in the run itself,
3,665,242 hot misses with 1,447,883 dirty evictions); warmup populates
the cache (`WARMUP_TAG_UPDATES: 3,502,390`, leaving only 27 cold misses
across the whole ROI, versus 96% cold misses on the pre-fix run).

**Do not compare this against the pre-fix 99.87%** — that figure is the
aliasing defect. On the same short probe methodology, `403.gcc-16B` (the
trace behind it) now shows a 2.12% DCM hit rate.


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
