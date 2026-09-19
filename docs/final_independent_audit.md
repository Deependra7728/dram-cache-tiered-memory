# Final Independent Research-Grade Audit

**Scope**: independent correctness audit of the ChampSim port of
"Enabling Design Space Exploration of DRAM Caches in Emerging Memory
Systems" (arXiv 2303.13029 / ISPASS'23), against the paper and against
the gem5 reference (`darchr/dram-cache-model`, branch
`dram_cache_disaggregated`).

**Method**: every conclusion below was derived by reading the actual
source (paper text layer, gem5 source re-fetched this session, current
ChampSim tree) and, where behavioral, by executing purpose-written
standalone probes. **No prior report, documentation, code comment, or
test name was accepted as evidence.** Where a prior claim was checked
and found false, that is stated explicitly.

**Code was NOT modified during this audit.** No long experiment matrix
was run.

---

## 1. Executive verdict

The **core DRAM-cache model is substantially correct and faithful to the
paper**. I independently re-derived the paper's Table II and measured
all 24 cells (8 access cases × 3 policies) against the actual
implementation: **24/24 match exactly**, including the per-operation
breakdown (local read / local write / far read / far write), not merely
the totals. The baseline, BEAR-Wr-Opt and Oracle state machines, the
ORB/CRB/WB structures, the eager metadata update, the dirty-victim
write-back trigger point, and the bypass isolation all behave as the
paper specifies.

**However, the implementation is NOT currently safe for long research
experiments.** I found and reproduced a **CRITICAL deadlock-class
defect**: when a request is promoted out of the CRB for an address that
still has an in-flight entry in the near-memory read queue, the
underlying ChampSim `MEMORY_CONTROLLER::add_rq()` silently *merges*
(discards) it, and the DCM treats the merge as a successful send. The
promoted request's ORB entry is then stuck forever, and — because the
ORB entry permanently occupies its DRAM-cache index — **every future
request mapping to that index is blocked for the remainder of the
simulation**. This is reachable from the real LLC and it progressively
leaks cache indices.

Two further defects materially affect experimental validity: the
bandwidth-utilization statistic is computed from a double-subtracted
cycle count that **unsigned-underflows** under ordinary parameter
choices, and the DRAM cache is **never warmed during ChampSim's warmup
phase**, so every ROI begins with a completely cold DRAM cache —
directly contradicting the paper's stated methodology.

**Verdict: DO NOT run the long experiment matrix until the CRITICAL
finding and HIGH findings 1–2 are fixed.** The policy/state-machine
core itself does not need rework.

---

> ## STATUS UPDATE — CRITICAL-1, CRITICAL-2, HIGH-1 and HIGH-2 are now FIXED
>
> All four blocking findings have since been fixed and verified; see
> **§15 Remediation** at the end of this document for root causes, exact
> fixes, new tests, regression results and the post-fix 600M pre-flight.
> HIGH-3 (a false in-code claim about gem5, behaviour already correct)
> and the MEDIUM/LOW items are addressed or explicitly deferred there.
> Everything above this box is preserved verbatim as the original
> pre-fix audit record.

---

## 2. Complete paper feature matrix

Extracted section-by-section from both paper versions (the ISPASS and
arXiv versions are the same work; Section/figure numbering below follows
the arXiv PDF whose text layer I extracted).

| # | Paper feature | Paper description | Expected behavior | ChampSim implementation | Verdict |
|---|---|---|---|---|---|
| P1 | DRAM cache manager as memory-controller replacement | §II-B, Fig.1: "drop-in replacement for the memory controller", not in coherence domain | Sits at LLC's `lower_level` | `uncore.DCM`, `LLC.lower_level` | **Correct** |
| P2 | Two independent controllers (near/far) | §II-B: "does not require DRAM cache and backing store to share a data bus" | Separate `MEMORY_CONTROLLER` instances, independent queues/timing | `nearMC`, `farMC` | **Correct** (verified independent: bypass probe shows near untouched) |
| P3 | ORB (Outstanding Requests Buffer) | Fig.1, Table I: 128 entries | Admission, capacity, retirement | `ORB` map, `DCM_ORB_MAX_SIZE=128` | **Correct** |
| P4 | CRB (Conflicting Requests Buffer) | Fig.1, Table I: 32 entries; "two requests conflict if they map to same location in DRAM cache" | Conflict by **index**, not address | `checkConflictInORB(indexDC)`, `DCM_CRB_MAX_SIZE=32` | **Correct** |
| P5 | WB (Write Back) Buffer | Fig.1, Table I: 64 entries | Dirty victims queued, drained to far memory | `WB` deque, `DCM_WB_PRESSURE_THRESHOLD=64` | **Correct** (threshold derivation matches gem5's actual `orbMaxSize/2`) |
| P6 | Direct-mapped, 64B granularity | §II-B: "direct-mapped (with caching granularity of 64 bytes), inserts-on-miss, writes back dirty lines upon eviction" | index = blockaddr % numLines | `returnIndexDC/returnTagDC` | **Correct** |
| P7 | Tag+metadata in ECC bits alongside data | §V: "stores the tag and metadata in ECC bits in the cache line, along with the data" | Tag check requires reading the **whole line** | Modeled as a full local read (`DCM_LOC_MEM_READ`) | **Correct (adapted)** — no physical ECC storage, timing-equivalent |
| P8 | Baseline = Cascade-Lake-inspired | §II-B, §IV | Always tag-check read first | `DCM_POLICY_BASELINE_CASCADE_LAKE` | **Correct** |
| P9 | BEAR-Wr-Opt | §V: avoid tag-check read for **write hits only** | Write hit → local write directly | `chooseInitialState` write-hit branch | **Correct** (Table II verified) |
| P10 | Oracle | §V: avoid tag-check read for write hits **and** any miss "if ... the cache line is clean"; zero-latency SRAM tag store | Write hit, and clean-victim miss (read or write) skip the local read | `chooseInitialState` clean-miss branch | **Correct per paper** — see §5 and Finding H3 (deviates from gem5's *literal* code, which contradicts its own paper) |
| P11 | Respond-before-fill on read miss | Fig.1 steps ⑦⑧ | LLC response sent, then background fill | `completeRequest()` then `dispatchToNear(fill)` | **Correct** |
| P12 | Table II access amplification | Table II, 8 cases × 3 designs | Exact operation counts | Measured | **24/24 exact** (§5) |
| P13 | Frontend/backend latency 20 ns round-trip | Table I | 10ns+10ns; double backend on far-fetch response | `frontendLatencyCycles`/`backendLatencyCycles` | **Correct** (matches gem5's single-vs-double distinction) |
| P14 | HBM2 near, 32 GB/s | Table I | Peak BW modeled | `DCM_NEAR_HBM2_MTPS=4000` | **Correct (bandwidth); latency adapted** (documented) |
| P15 | DDR4 far, 19.2 GB/s | Table I | Peak BW modeled | `DCM_FAR_DDR4_MTPS=2400` | **Correct (bandwidth); latency adapted** |
| P16 | Case Study 1: DRAM cache vs none | §IV, Fig.5 | Bypass configuration | `--dcm_bypass` | **Correct mechanism** |
| P17 | Case Study 2: baseline vs BEAR vs Oracle | §V, Fig.10/11 | Policy selection | `--dcm_policy` | **Correct mechanism** |
| P18 | Case Study 3: link latency 100/500/1000 ns | §VI, Fig.12/13 | Configurable far-link delay | `--dcm_link_latency_ns` | **Correct mechanism** |
| P19 | Case Study 3: NVM far memory | §VI | Asymmetric read/write latency + limited write buffer | Not implemented | **Correctly NOT implemented** (§13) |
| P20 | 8-core target | Table I | 8 cores sharing one DRAM-cache channel | `NUM_CPUS=1` | **Not reproduced** (documented) |
| P21 | GAPBS + NPB workloads, full-system | §III-B/C | Real graph/HPC workloads | SPEC CPU2006 traces | **Not reproduced** (documented) |
| P22 | **Warmed-up DRAM cache before measurement** | §III-C + §V: "we made sure that the DRAM cache had been warmed-up, so cold misses are not contributing to the performance observed" | DRAM cache non-cold at ROI start | **DCM entirely bypassed during warmup** | **VIOLATED — Finding H2** |
| P23 | Validation vs traffic generator | §III, Fig.4 | Effective-bandwidth validation sweep | Not reproduced | **Not reproduced** (no traffic generator; not required for case studies) |
| P24 | `cache_warmup_ratio` stat reset | `PolicyManager.py` param (0.7) | Reset stats once cache 70% warm | Not implemented | **Correctly not implemented** — the gem5 code that would use it is commented out (`policy_manager.cc:1522-1527`), i.e. inert in the reference too |

---

## 3. Complete gem5 → ChampSim matrix

Derived from the re-fetched `dram_cache_disaggregated` sources
(`policy_manager.cc` 2118 lines, `.hh` 489, `PolicyManager.py` 69,
`disaggregated_dram_cache_script.py` 138). I confirmed branch identity
first: this branch uniquely defines `loc_req_port`/`far_req_port`; the
other DRAM-cache branch has no `PolicyManager.py` at all, so there is no
risk of having audited the wrong reference.

| gem5 behavior | Location | ChampSim equivalent | Class |
|---|---|---|---|
| Admission order: conflict→CRB, then FMWB-pressure, then ORB-full | `recvTimingReq` 298–362 | Same order in `add_rq`/`add_wq` | **Exact** |
| Conflict = same `indexDC` among **ORB entries only** | `checkConflictInDramCache` 1447–1460 | `checkConflictInORB` | **Exact** |
| WB pressure threshold = `orbMaxSize/2` | 332 | `DCM_WB_PRESSURE_THRESHOLD = DCM_ORB_MAX_SIZE/2` | **Exact** |
| Eager metadata update at admission | `handleRequestorPkt` 1426–1443 | `classifyAndInstall()` | **Exact** |
| `handleDirtyLine` decided from **pre-update** dirty bit | 1421 (before 1426) | `victimWasValid && victimWasDirty` | **Exact** |
| Write responds immediately at admission (`frontend+backend`) | 1389 | Writes get **no** LLC callback | **Adapted** (ChampSim write contract has no write response; documented) |
| Dirty write-back triggered **only** at local-read completion | `locMemRecvTimingResp` 530–537 | `pushDirtyWriteBack()` at tag-read completion | **Exact** |
| Read-miss response = `frontend+backend+backend` | 734/854/960 | `completeRequest(e, f+2b)` | **Exact** |
| Hit response = `frontend+backend` | 1042 etc. | `completeRequest(e, f+b)` | **Exact** |
| ORB erased + **one** CRB entry promoted | `resumeConflictingReq` 1624–1690 (`break`) | `promoteFromCRB` (`break`) | **Exact** |
| Promoted entry keeps original arrival tick | 1652 | `admitRequest(..., originalArrival)` | **Exact** |
| `alwaysHit` / `alwaysDirty` params | `PolicyManager.py` 59–60 | `debugForceHit`/`debugForceDirty` | **Exact** — both are **inert in gem5 too** (`checkHitOrMiss` 1473 and `checkDirty` 1537 are commented out). The port's claim that these are inert legacy hooks is **verified true**. |
| `bypass_dcache`: first check in `recvTimingReq` | 160–162 | First check in `add_rq`/`add_wq` | **Exact** |
| Bypass response scheduled at `curTick()` (no controller latency) | 563–566 | No controller latency on bypass path | **Exact** |
| `sendTimingReq` false → retain + retry via `recvReqRetry` | 490–499, 641–674 | Occupancy pre-check + `pendingNear/FarDispatches` retry | **Adapted** (sync polling vs async callback; same outcome — but see CRITICAL-1, the pre-check does not cover the *merge* failure mode) |
| Oracle `isDirty` = `checkDirty()` read **after** eager update | `setNextState` 683, called at 376 **after** `handleRequestorPkt` at 366 | Port uses **pre-update** victim dirty bit | **Deliberate deviation — matches the paper, not gem5.** See §5 / Finding H3 |
| Oracle read-miss-dirty never writes back victim (gem5 skips `locMemRead`, so `handleDirtyCacheLine` never fires) | consequence of above | Port **does** write back | **Deviation — port is correct per Table II** |
| `cache_warmup_ratio` stat reset | 1522–1527 **commented out** | Not implemented | **Exact** (inert in both) |

---

## 4. Code-level correctness findings

### CRITICAL-1 — Promoted CRB request silently lost; DRAM-cache index permanently deadlocked

- **File / function**: `src/dram_cache_manager.cc` → `trySend()` (and every caller: `dispatchToNear`, `dispatchToFar`), interacting with `src/dram_controller.cc` → `MEMORY_CONTROLLER::add_rq()`.
- **Exact behavior**: `add_rq()` first checks `check_dram_queue(&RQ[channel], packet)`; on an address match it `return index;` **without inserting the packet**. `trySend()` calls `add_rq(&pkt)` and **ignores the return value**, then returns `true`. The DCM therefore records `stats.localReads++`, sets the ORB entry to `DCM_WAITING_LOC_MEM_READ_RESP`, and waits for a response that will never arrive.
- **Why it is wrong**: In stock ChampSim the merge is safe because the *caller* (a `CACHE`) merges via its own MSHR and does not expect a per-dispatch response. The DCM has no such merging: it expects exactly one `return_data()` per dispatched read. A merge is therefore an unacknowledged drop.
- **Trigger**: `completeRequest()` → `promoteFromCRB()` runs **inside** `return_data()`, which is itself called from `MEMORY_CONTROLLER::process()` **before** that queue entry is removed (`process()` calls `return_data(...)` and only afterwards `queue->remove_queue(...)`). A promoted request for the **same address** therefore re-enters `add_rq()` while the completing entry is still resident, and is merged away.
- **Evidence (reproduced)**: `/tmp/audit/probe_merge.cc` (two reads to address A) and `/tmp/audit/probe_merge2.cc` (read then writeback to A):
  ```
  req1 rc=-1 ORB=1
  req2 rc=-1 ORB=1 CRB=1
  --- FINAL ---
  new responses delivered = 1   (EXPECT 2)
  ORB residual = 1              (EXPECT 0)
  stats: localReads=3 completedFromNear=2
  *** STUCK/LOST REQUEST REPRODUCED ***
  ```
  `localReads=3` but `completedFromNear=2`: the third dispatch was absorbed.
- **Consequence beyond one request**: the stuck ORB entry never retires, so `checkConflictInORB()` returns true for its index **forever**. All later requests to that index go to the CRB; once the 32-entry CRB fills, `add_rq`/`add_wq` return `-2` and `get_occupancy()==get_size()` for those addresses, stalling the LLC on them permanently. Indices leak monotonically over a long run.
- **LLC reachability**: ChampSim's LLC MSHR prevents two concurrent *reads* to one address, but a **read (`add_rq`) and a dirty writeback (`add_wq`) for the same address are independent paths** and can be concurrent — the `probe_merge2` variant. So this is reachable in real trace runs, not merely synthetic.
- **Expected behavior**: a promoted request must either be genuinely enqueued, or be retained and retried (as the WB path already does).
- **Proposed fix** (not applied): make `trySend()` treat a merge as a non-send. `MEMORY_CONTROLLER::add_rq()` returns `-1` on true insertion (and on WQ-forward) but returns `index >= 0` on an RQ merge, so `trySend()` can distinguish them without touching `dram_controller.cc`:
  ```cpp
  int rc = isWrite ? mc->add_wq(&pkt) : mc->add_rq(&pkt);
  if (rc >= 0) return false;   // merged, NOT enqueued -> caller must retain/retry
  return true;
  ```
  Both `dispatchToNear()` and `dispatchToFarGuaranteed()` already queue-and-retry on `false`, so this alone closes the hole. (A merged read will keep retrying and will succeed on a later cycle once the duplicate entry has drained.)
- **Test required**: concurrent same-address requests — (a) read+read via CRB promotion, (b) read+writeback via CRB promotion — asserting `ORB.empty()` and full response delivery at quiescence. **No existing test covers this** (§7).

### CRITICAL-2 — Bypass mode loses duplicate concurrent reads and leaks its tracking set

- **File / function**: `src/dram_cache_manager.cc` → `add_rq()` bypass branch → `dispatchToFarGuaranteed()` → `trySend()`.
- **Behavior**: same root cause as CRITICAL-1. Both requests insert into `bypassOutstandingReads`, but the second `farMC->add_rq()` is merged away. Only one completion arrives, so one LLC response is never delivered and one multiset entry leaks permanently.
- **Evidence (reproduced)**: `/tmp/audit/probe_bypass.cc`:
  ```
  bypassReads=2 bypassCompletedReads=1 LLCresponses=1 outstandingLeak=1
  *** BYPASS DUPLICATE-READ LOSS REPRODUCED ***
  ```
- **Reachability**: lower than CRITICAL-1 (the LLC MSHR normally prevents duplicate concurrent reads), but bypass mode performs **no** conflict tracking of its own by design, so nothing else guards it.
- **Fix**: identical to CRITICAL-1 (shared `trySend()`).
- **Same probe also positively verified bypass isolation**: `nearMC RQ ACCESS=0`, `nearWQ occ=0`, `ORB=0`, `CRB=0`, `WB=0` — bypass genuinely touches no DRAM-cache machinery.

### HIGH-1 — Bandwidth-utilization statistic double-subtracts, and unsigned-underflows

- **File / function**: `src/main.cc` → `print_dcm_stats()`, line 219.
- **Behavior**: `uint64_t elapsedCycles = ooo_cpu[0].finish_sim_cycle - ooo_cpu[0].begin_sim_cycle;` — but `finish_sim_cycle` is **already an ROI delta**: `src/main.cc:1053` assigns `finish_sim_cycle = current_core_cycle[i] - begin_sim_cycle`. Subtracting `begin_sim_cycle` again is wrong.
- **Magnitude on the real 600M run**: warmup ended at cycle 46,674,846; ROI = 1,693,827,177 cycles. Denominator used = 1,647,152,331 → utilization **overstated by 2.8%**.
- **Worse — silent underflow, reproduced**: whenever ROI cycles < warmup-end cycle the subtraction wraps. With `-warmup_instructions 1000000 -simulation_instructions 100000`:
  ```
  Warmup complete ... cycles: 477272
  Finished CPU 0 ... cycles: 83829
  DCM LOCAL_BW_UTILIZATION: 1.73472e-16%  FAR_BW_UTILIZATION: 1.4456e-16%
  ```
  The `if (elapsedCycles > 0)` guard does not catch it because the wrapped value is huge, not zero.
- **Fix**: `uint64_t elapsedCycles = ooo_cpu[0].finish_sim_cycle;` (already the ROI delta). Add a guard that reports `-` if zero.
- **Test required**: assert utilization is finite and within (0,100] for a short run where warmup cycles exceed ROI cycles.

### HIGH-2 — The DRAM cache is never warmed; every ROI starts cold (violates paper methodology)

- **File / function**: `src/dram_cache_manager.cc` → `add_rq()` (lines 125–131) and `add_wq()` (lines 212–213).
- **Behavior**: while `all_warmup_complete < NUM_CPUS`, `add_rq()` returns the packet immediately to the LLC and `add_wq()` drops it — **without creating an ORB entry, without touching `tagMetadataStore`, and without recording any statistic**. The DCM is therefore inert for the entire ChampSim warmup phase.
- **Consequences**: (a) *good* — DCM statistics are cleanly ROI-only, consistent with the LLC statistics printed beside them (I verified `reset_cache_stats` is applied to L1I/L1D/L2C/LLC but never to `uncore.DCM.stats`; this is harmless precisely *because* nothing is counted during warmup). (b) *bad* — the DRAM cache's tag store is empty at ROI start, so the measured region is dominated by compulsory misses.
- **Evidence from the real 600M-instruction BASELINE run**: `MISSES: 13638`, `COLD_MISSES: 13106` — **96% of all DRAM-cache misses in the measured region are cold misses.**
- **Why it is wrong**: the paper explicitly states the opposite methodology (§V): *"we made sure that the DRAM cache had been warmed-up, so cold misses are not contributing to the performance observed from the system"*. Case Study 1 and 2 conclusions are miss-ratio-driven; a cold cache biases every configuration, and biases them *unequally* (bypass mode has no cache to warm).
- **Fix options** (design decision, not a one-liner): let the DCM process requests during warmup for metadata purposes while preserving ChampSim's zero-latency warmup contract; or add an explicit DCM-warmup phase; or, at minimum, **report cold-miss fraction prominently and treat any run with a high cold-miss fraction as not comparable to the paper**.
- **Test required**: assert non-zero `tagMetadataStore` valid-line count at ROI start under a chosen policy.

### HIGH-3 — Oracle `isDirty` semantics: implementation is right, its stated justification is wrong

- **File**: `inc/dram_cache_manager.h`, `chooseInitialState()` doc comment.
- **The comment asserts**: *"gem5's own condition is simply `!isDirty` on the OLD resident line (policy_manager.cc:787,795,805 — `isDirty` there is `checkDirty(addr)`, i.e. `validLine && dirtyLine` of whatever occupied the index BEFORE this request)."*
- **This claim is factually false.** In gem5, `recvTimingReq` calls `handleRequestorPkt(pkt)` at line 366 — which **eagerly overwrites** the metadata at lines 1426–1443 — and only then calls `setNextState(...)` at line 376. `setNextState` computes `isDirty = checkDirty(owPkt->getAddr())` at line 683, which reads the **already-updated** entry. Post-update, a read miss has `dirtyLine=false` and any write has `dirtyLine=true`, unconditionally.
- **Consequences inside gem5 itself**: two of its own labelled branches become dead code (`(isRead && !isHit && isDirty)` and `(!isRead && !isHit && !isDirty)`), and gem5's actual RambusHypo counts diverge from its own paper's Table II in exactly two cells — RdMissDirty (gem5 = 2, paper = 4) and WrMissClean (gem5 = 2, paper = 1). gem5 also never writes back a dirty victim evicted by a RambusHypo read miss, because the local read that triggers `handleDirtyCacheLine` is skipped.
- **The ChampSim port uses the pre-update victim dirty bit** (`victimWasValid && victimWasDirty`), which reproduces the **paper's** Table II exactly (verified, §5) and matches the paper's prose: *"if the demand access (either read or write) will miss on DRAM cache **and the cache line is clean**"*.
- **Assessment**: the *behavior* is the correct choice and should be kept. Only the *justification* must be corrected — the port implements the paper, deliberately diverging from a defect in the reference implementation. Left as-is, the comment will mislead any future reader who checks it against gem5.
- **Severity**: HIGH as a documentation/traceability defect (it is the single most load-bearing fidelity claim in the port), **not** a behavioral defect.

### MEDIUM-1 — Statistics cannot detect the stuck-request condition; a prior "benign" conclusion is unsubstantiated

- **File**: `src/main.cc` → `print_dcm_stats()`.
- **Behavior**: prints ORB/CRB/WB *occupancy averages and maxima* but **not** final `ORB.size()`, `CRB.size()`, `pendingNearDispatches.size()`, `completedFromNear`, `completedFromFar`, `completedReadsToLLC`, or `completedWrites`. There is therefore no way to observe a stuck ORB entry from a result file.
- **Direct consequence**: the previously recorded conclusion that the 600M run's local-write shortfall was "3 background fill writes still in flight at the cutoff — nothing was lost" is **not established by the available evidence**. The observed shortfall (`LOCAL_WRITES 3252840` vs. expected `WRITES 3239593 + FAR_READS 13250 = 3252843`) is equally consistent with **3 requests permanently stuck by CRITICAL-1**, which is now known to be a real, reachable defect. Both hypotheses fit every printed number. This must be treated as **unresolved** until ORB residual is instrumented.
- **Fix**: print final `ORB.size()`, `CRB.size()`, both pending-dispatch queue sizes, and the four completion counters; treat non-zero ORB residual at end of simulation as a run-invalidating error.

### MEDIUM-2 — Experiment harness has no failure or incomplete-run detection

- **File**: `run_case_studies.sh`.
- **Behavior**: uses `set -u` but not `set -e`; after `eval "$cmd" > "$out_file" 2>&1` it only *prints* `exit=$?` and a line count. A crashed, killed, or truncated run produces a result file that looks like any other. There is no check for the terminal `ChampSim completed all CPUs` marker.
- **Also**: re-running the matrix silently overwrites prior results (no timestamp/run-id in the filename).
- **Fix**: capture the exit status into a variable, verify the completion marker and the presence of the `DRAM Cache Manager Statistics` block, and fail loudly; write into a run-stamped subdirectory.

### MEDIUM-3 — No test covers same-address concurrency (the CRITICAL-1 trigger)

See §7.

### LOW-1 — `add_wq` duplicate merge is unaccounted

Same merge pattern on the write side. For writes it is *semantically* tolerable (two writes to the same address coalesce), but `wbDrains`/`localWrites` count a dispatch that never entered the queue, so write counts can overstate real far/near traffic. Would be fixed for free by the CRITICAL-1 fix.

### LOW-2 — Deep synchronous re-entrancy

`dispatchToNear()` → `add_rq()` → (WQ-forwarding branch) → `return_data()` → `completeRequest()` → `promoteFromCRB()` → `admitRequest()` → `driveState()` → `dispatchToNear()` … is fully re-entrant. I audited this for use-after-free and found **none**: `completeRequest()` deletes the ORB entry *before* calling `promoteFromCRB()`; `return_data()`'s far-read branch copies `fillPkt` *before* `completeRequest()`; and `driveState()`'s read case does not touch `e` after dispatch. It is nonetheless fragile and depends on invariants no test asserts. No bug found — flagged as a maintenance hazard only.

### COSMETIC-1 — Stale header comments

`inc/dram_cache_manager.h` still carries pre-Stage-5/6 text: *"BEAR-Wr-Opt and Oracle are NOT implemented"*, *"DCM_POLICY_ORACLE is NOT implemented yet"*, *"tagMetadataStore ... allocated, not yet consulted"*, and *"CRB = 32 entries -> exact (declared; not yet enforced)"*. All four are false today. Same for the `src/dram_cache_manager.cc` banner.

**FIXED (stage 17, comment-only)**: all of these are now corrected. The two policy claims (*"DCM_POLICY_ORACLE is NOT implemented yet"*, *"BEAR-Wr-Opt and Oracle are NOT implemented"*) were fixed first, then the remaining five in a follow-up pass: `inc/dram_cache_manager.h:21` (CRB is enforced at `dram_cache_manager.cc:271,347`), `:74` (link latency IS applied via `dispatchToFar()`/`pendingFarDispatches` and `--dcm_link_latency_ns`), `:578` (`tagMetadataStore` IS consulted by `classifyAndInstall()` and `warmupTagUpdate()`), and in `src/dram_cache_manager.cc` the file banner at `:11` and the inline *"Under BEAR/Oracle (not implemented)"* at `:576` (insert-on-miss is policy-independent). Verified comment-only: the comment-stripped translation units are byte-identical before and after, 16/16 suites pass, and full simulator output on a real trace is identical under all three policies.

---

## 5. Table II independent verification

I re-derived Table II from the paper (text layer extracted directly from
the PDF, so the numbers below are the paper's, not a transcription from
memory):

```
Access               Read                          Write
Hit/Miss        Hit          Miss            Hit           Miss
Dirty/Clean  Dirty  Clean  Dirty  Clean   Dirty  Clean   Dirty  Clean
Tot. Baseline    1      1      4      3      2      2       3      2
Tot. BEAR-Wr-Opt 1      1      4      3      1      1       3      2
Tot. Oracle      1      1      4      2      1      1       3      1
```

The paper's "Local Read" checkmark row independently corroborates the
Oracle semantics: Oracle omits the local read for exactly
RdMissClean, WrHitDirty, WrHitClean, WrMissClean — and keeps it for
RdHit(both), RdMissDirty, WrMissDirty.

I then measured the actual implementation with a purpose-written probe
(`/tmp/audit/probe_table2.cc`) that builds each victim/access
combination explicitly and takes per-operation stat deltas — **not**
reusing any existing test's expectations:

| Case | Baseline (locRd,locWr,farRd,farWr → tot / paper) | BEAR | Oracle |
|---|---|---|---|
| RD Hit Dirty | 1,0,0,0 → **1** / 1 | 1,0,0,0 → **1** / 1 | 1,0,0,0 → **1** / 1 |
| RD Hit Clean | 1,0,0,0 → **1** / 1 | 1,0,0,0 → **1** / 1 | 1,0,0,0 → **1** / 1 |
| RD Miss Dirty | 1,1,1,1 → **4** / 4 | 1,1,1,1 → **4** / 4 | 1,1,1,1 → **4** / 4 |
| RD Miss Clean | 1,1,1,0 → **3** / 3 | 1,1,1,0 → **3** / 3 | 0,1,1,0 → **2** / 2 |
| WR Hit Dirty | 1,1,0,0 → **2** / 2 | 0,1,0,0 → **1** / 1 | 0,1,0,0 → **1** / 1 |
| WR Hit Clean | 1,1,0,0 → **2** / 2 | 0,1,0,0 → **1** / 1 | 0,1,0,0 → **1** / 1 |
| WR Miss Dirty | 1,1,0,1 → **3** / 3 | 1,1,0,1 → **3** / 3 | 1,1,0,1 → **3** / 3 |
| WR Miss Clean | 1,1,0,0 → **2** / 2 | 1,1,0,0 → **2** / 2 | 0,1,0,0 → **1** / 1 |

**Result: 24/24 cells exact**, and the per-operation breakdown (not just
the totals) is correct in every cell. **No existing test expectation was
found to be wrong.** This is the strongest single piece of evidence that
the policy state machines are faithfully implemented.

---

## 6. Request-flow findings

Reconstructed from source for all twelve required paths. All twelve
behave as specified; the defects found are in the *dispatch* layer, not
the flow logic.

| Path | Enters | Stored | State | Ops generated | Metadata | Response | ORB retire / CRB |
|---|---|---|---|---|---|---|---|
| 1 Read hit | `add_rq` | ORB | LOC_MEM_READ→WAIT | 1 near read | eager at admit | after tag read, `f+b` | retire → promote 1 |
| 2 Write hit | `add_wq` | ORB | LOC_MEM_READ→LOC_MEM_WRITE | near read + near write | eager, dirty=1 | **none** (write contract) | retire at write dispatch |
| 3 Read miss clean | `add_rq` | ORB | LOC_RD→FAR_RD | near rd + far rd + fill | eager, dirty=0 | after far read, `f+2b` | retire before fill |
| 4 Read miss dirty | `add_rq` | ORB | LOC_RD→FAR_RD | + WB at tag-read completion | victim addr captured pre-update | `f+2b` | retire before fill |
| 5 Write miss clean | `add_wq` | ORB | LOC_RD→LOC_WR | near rd + near wr | dirty=1 | none | retire at write |
| 6 Write miss dirty | `add_wq` | ORB | LOC_RD→LOC_WR | + WB | — | none | retire at write |
| 7 Conflict | `add_rq/wq` | **CRB** | — | none until promoted | at promotion | at completion | promoted 1-at-a-time, original arrival tick preserved |
| 8 WB retry | `drainWB` | WB deque | — | far write | — | — | popped **only** after confirmed send |
| 9 BEAR write hit | `add_wq` | ORB | **LOC_MEM_WRITE** directly | 1 near write | dirty=1 | none | retire at write |
| 10 Oracle clean miss | either | ORB | read→**FAR_RD**, write→**LOC_WR** | skips near read | — | read: `f+2b` | — |
| 11 Bypass read | `add_rq` bypass branch | `bypassOutstandingReads` only | none | 1 far read | **untouched** | immediate, **no** controller latency | n/a |
| 12 Bypass write | `add_wq` bypass branch | none | none | 1 far write | untouched | none | n/a |

**Queue-full behavior**: paths 1–6 and 8 correctly retain and retry via
`pendingNearDispatches`/`pendingFarDispatches`. **Queue-*merge* behavior
(paths 1, 3, 4, 11 in particular) is the CRITICAL-1/2 hole** — a merge is
not a full queue and is not caught by the occupancy pre-check.

---

## 7. Test-quality findings

Assessed for *quality*, not count.

**Genuine strengths.** The Table II tests assert the full
per-operation breakdown, not just totals — I independently confirmed
every expectation is correct. The WB-retry and memory-dispatch suites
use a deterministic `forceWqFull`/`forceRqFull` override rather than
racing real timing, which is the right technique. The timing tests
measure elapsed-cycle *deltas* rather than absolutes. Several test files
document real methodology bugs found and fixed while writing them.

**Decisive gap — the CRITICAL-1 trigger is untested.** Every
conflict/CRB test constructs conflicts using **different addresses at
the same index** (`addr + DCM_DRAM_CACHE_SIZE`, e.g.
`tests/test_dcm_skeleton.cc:330,413,420`). No test ever places **two
requests for the same address** in ORB+CRB simultaneously. That is
precisely the configuration that loses a request. The suites therefore
pass while a deadlock-class defect is present — the clearest example in
this codebase of a test suite that cannot fail for the bug that matters.

**Other gaps.**
- No test asserts `ORB.empty()` at quiescence as a general post-condition; a stuck entry is invisible to every existing suite.
- No test drives the LLC→DCM path through the *real* `CACHE` class; all suites use a `FAKE_LLC` stub, so LLC-interaction bugs (MSHR vs. writeback concurrency — the realistic CRITICAL-1 trigger) cannot surface.
- No test covers `add_rq`/`add_wq` **merge** semantics at all (only *full*-queue semantics).
- No test covers warmup-phase behavior, so HIGH-2 (cold DRAM cache) is untested and undetected.
- No test validates `print_dcm_stats()` arithmetic; HIGH-1's underflow is untested.
- `test_dcm_bypass.cc` Test 7's `bothAdmitted` is computed but the CRB/ORB-empty checks carry the assertion — a benign redundancy, not a defect.

**Verdict**: coverage is strong for the *policy state machines* and
*capacity-exhaustion* paths, and absent for *aliasing/merge*,
*end-to-end LLC integration*, *warmup*, and *statistics arithmetic*.

---

## 8. Experiment-harness findings

Verified by inspection plus a full-scale `--dry-run` (42 lines) and
short live runs.

| Check | Result |
|---|---|
| Correct policy/bypass/link-latency flags per configuration | **Correct** — all 7 verified against the configuration table |
| Correct trace paths | **Correct** — absolute paths, corrupt `619.lbm_s` excluded by default |
| Correct warmup / simulation counts | **Correct** — propagated verbatim |
| Unique output files | **Correct** — 42 paths, `sort \| uniq -d` returns nothing; injective by construction |
| Statistics reset between runs | **Correct** — one OS process per run; `uncore` is a single global constructed once per process (`src/uncore.cc:4`); no cross-run state is possible |
| Records trace/config/warmup/sim/cycles/IPC/DCM stats | **Correct** — all present and labelled |
| Failure detection | **MISSING** (MEDIUM-2) — exit status printed but never acted on; no `set -e` |
| Incomplete-run detection | **MISSING** — no check for `ChampSim completed all CPUs` or for the stats block |
| Overwrite protection | **MISSING** — re-running overwrites silently |
| Stuck-request detection | **MISSING** (MEDIUM-1) — ORB residual not reported |

The harness is **structurally sound but not defensive**: it will run the
right experiments, and will not tell you when one went wrong.

---

## 9. Trace-based limitations

**A. Directly reproducible** — DRAM-cache organization and indexing;
all three policy state machines; ORB/CRB/WB structure, capacities and
backpressure; access amplification (Table II); dirty write-back;
respond-before-fill; controller frontend/backend latency; far-link
latency sweep (100/500/1000 ns); bypass/no-DRAM-cache comparison; peak
near/far bandwidth.

**B. Reproducible with documented adaptation** — write responses (gem5
responds to writes; ChampSim's write contract has no callback, so the
DCM completes writes synchronously — timing-equivalent for the modeled
quantities); retry protocol (gem5's asynchronous port retry vs. this
port's synchronous per-cycle polling — same retain-until-accepted
outcome); tag-in-ECC storage (modeled as a full-line read, which is the
only timing-visible consequence); workload substitution (SPEC CPU2006
for GAPBS/NPB — same *mechanism* exercised, different access patterns,
so trends only, never absolute numbers); DRAM-cache warmup (currently
**not** adapted — HIGH-2).

**C. Fundamentally not reproducible in this environment** —
- *Linux/full-system execution and checkpoint/restore*: ChampSim consumes a fixed instruction trace with no OS, no system calls, no page-table behavior; there is no execution state to checkpoint. Not a missing feature but an architectural property of trace-driven simulation.
- *OS/thread effects and multi-threaded GAPBS/NPB*: the traces are single-threaded and pre-recorded; thread interleaving cannot be varied, so the paper's multi-threaded 8-core sharing behavior cannot arise.
- *Exact HBM2 / DDR4 device models*: gem5's JEDEC-derived `DRAMInterface` timing tables are not published as ns values in either paper; only peak bandwidths are. Reproducing bandwidth exactly is possible (and done); reproducing device latency exactly is not, without inventing numbers.
- *NVM model*: requires read/write **latency asymmetry**, which `MEMORY_CONTROLLER` structurally cannot express (`dram_controller.cc:221-225` computes one `LATENCY` for both queues). See §13.
- *Physical ECC-bit storage*: ChampSim stores no data values at all (`dram_array` is dead code), so tag-in-data-line co-location has no representable physical form; only its timing consequence is modeled.
- *Full gem5 packet/port timing semantics* (`headerDelay`/`payloadDelay`, per-port retry callbacks): no ChampSim equivalent exists; the static latency components that dominate are modeled.
- *Traffic-generator validation (Fig. 4)*: would require a synthetic generator replacing the CPU; possible in principle, but nothing in the current tree provides it.

---

## 10. Remaining gaps — independent necessity assessment

| Item | Required by paper? | Required for core architecture? | Required for a case study? | Faithfully implementable? | Worth implementing? | Misleading if approximated? |
|---|---|---|---|---|---|---|
| Per-technology tRP/tRCD/tCAS | Implied, values never disclosed | No | No (BW dominates the reported effects) | **No** — numbers exist only inside gem5 SimObjects, not in either paper | No | **Yes** — inventing them would look paper-derived |
| Per-controller bus width | No (only aggregate BW is stated) | No | No | Yes, but invasive (global macro) | No — aggregate BW already matches | Mildly |
| NVM far memory | Yes (CS3 second half) | No | Yes, for CS3's NVM arm only | **No** — needs read/write latency asymmetry `MEMORY_CONTROLLER` cannot express | **No** | **Yes** — a symmetric "slow DDR4" would not even reproduce the paper's *direction* of effect |
| WB retry | Not in paper; is in gem5 | **Yes** (data integrity) | Yes | Yes | **Already done** and verified | — |
| Controller latency | Yes (Table I, 20 ns) | Yes | Yes | Yes | **Already done**, matches gem5's single/double distinction | — |
| `headerDelay`/`payloadDelay` | No | No | No | Not in ChampSim's model | No | No — small vs. the static terms for 64 B packets |
| 8-core configuration | Yes (Table I) | No | Affects magnitudes, not direction | Possible but large blast radius (LLC sizing macros, `ooo_cpu[]`) | Not now | **Yes if claimed as the paper's config** |
| Full-system GAPBS/NPB | Yes | No | Yes for numeric fidelity | No (no traces, no full system) | No | **Yes** — must never be described as equivalent |
| **DRAM-cache warmup (HIGH-2)** | **Yes, explicitly** | No | **Yes — all three** | **Yes** | **Yes — should be fixed** | **Yes** — 96% cold misses invalidates miss-ratio-driven conclusions |
| **ORB-residual reporting (MEDIUM-1)** | No | No | Yes (run validity) | Yes, trivially | **Yes** | Yes — hides CRITICAL-1 |

---

## 11. Bugs found (summary)

| ID | Severity | Component | One-line |
|---|---|---|---|
| CRITICAL-1 | **CRITICAL** | `trySend`/`add_rq` | Duplicate-address merge silently drops a promoted CRB request → stuck ORB entry → DRAM-cache index deadlocked forever |
| CRITICAL-2 | **CRITICAL** | bypass `add_rq` | Same merge defect loses a bypass read and leaks a `bypassOutstandingReads` entry |
| HIGH-1 | **HIGH** | `print_dcm_stats` | `finish_sim_cycle - begin_sim_cycle` double-subtracts; unsigned underflow yields ~1e-16% utilization |
| HIGH-2 | **HIGH** | `add_rq`/`add_wq` warmup guard | DRAM cache never warmed → ROI is 96% cold misses, contradicting the paper's stated methodology |
| HIGH-3 | **HIGH** (doc) | `chooseInitialState` comment | Asserts a false claim about gem5's `isDirty`; behavior is correct (paper-faithful), justification is not |
| MEDIUM-1 | MEDIUM | `print_dcm_stats` | No ORB/CRB residual or completion counters → stuck requests undetectable; prior "benign in-flight" conclusion unsubstantiated |
| MEDIUM-2 | MEDIUM | `run_case_studies.sh` | No failure/incomplete-run detection; silent overwrite |
| MEDIUM-3 | MEDIUM | tests | No same-address concurrency test → CRITICAL-1 invisible to the whole suite |
| LOW-1 | LOW | `add_wq` | Merged writes counted as dispatched |
| LOW-2 | LOW | dispatch re-entrancy | Deep synchronous recursion; audited safe, but unasserted |
| COSMETIC-1 | COSMETIC | headers | Stale "not implemented" comments for Oracle/BEAR/CRB/tag store |

---

## 12. Recommended fixes (in order)

1. **CRITICAL-1 + CRITICAL-2** — make `trySend()` treat a merge as a
   non-send by checking the return value (`rc >= 0` ⇒ merged ⇒ return
   `false`). Both call sites already retain-and-retry on `false`. Add
   the two missing tests (same-address read+read and read+writeback
   through CRB promotion), asserting `ORB.empty()` and full delivery.
2. **HIGH-1** — use `finish_sim_cycle` directly; guard against zero.
3. **MEDIUM-1** — print final `ORB.size()`, `CRB.size()`, both pending
   queue sizes, and the four completion counters; treat non-zero ORB
   residual as a failed run.
4. **HIGH-2** — decide and document the DRAM-cache warmup policy; at
   minimum surface cold-miss fraction as a first-class run-validity
   metric.
5. **HIGH-3** — correct the `chooseInitialState` comment to state
   plainly that the port implements the paper's Table II and
   *deliberately diverges* from gem5's literal (self-contradictory)
   `isDirty` handling, with the line numbers proving it.
6. **MEDIUM-2** — add failure/completion-marker detection and
   run-stamped output directories.
7. **MEDIUM-3** — add end-to-end tests through the real `CACHE`/LLC
   rather than only `FAKE_LLC`.
8. **COSMETIC-1** — refresh stale header comments.

---

## 13. Features that should NOT be implemented

- **NVM far memory** — confirmed again this audit. The paper's NVM
  finding rests on read/write latency *asymmetry* plus a limited write
  buffer; `MEMORY_CONTROLLER` computes a single `LATENCY` for both
  queues (`dram_controller.cc:221-225`), so a symmetric approximation
  would not reproduce even the *direction* of the reported effect. The
  only concrete numbers available (`NVM_2400_1x64`: tREAD 150 ns,
  tWRITE 500 ns) come from a generic ARM-authored 2020 gem5 SimObject,
  and the branch's own config script never instantiates it.
- **Per-technology tRP/tRCD/tCAS** — values are not disclosed in either
  paper; inventing them would misrepresent an approximation as
  paper-derived.
- **`headerDelay`/`payloadDelay`** — not part of the paper's 20 ns
  figure; negligible for 64 B packets.
- **8-core reconfiguration** — large blast radius, changes magnitudes
  not directions, and cannot be combined with the missing multi-threaded
  workloads anyway.
- **gem5's literal Oracle `isDirty` behavior** — reproducing gem5's
  post-update read would *break* Table II conformance. Keep the
  paper-faithful implementation.

---

## 14. Final confidence assessment

### Direct answers

1. **Is the core DRAM-cache architecture correctly implemented?** **Yes.** Placement, two independent controllers, direct-mapped 64 B organization, index/tag/valid/dirty, install-on-miss, replacement and cold state all verified against paper and gem5.
2. **Are all three policies correctly implemented?** **Yes** — verified by independent measurement of all 24 Table II cells, including per-operation breakdown. Oracle intentionally follows the paper where gem5 contradicts itself; that choice is correct.
3. **Is the request state machine correct?** **Yes** for state transitions and ordering (all 12 flows reconstructed). The defect is in the dispatch layer beneath it.
4. **Are ORB/CRB/WB correct?** **Structurally yes** (capacities, index-based conflict, one-promotion-per-retire, original arrival tick, WB pressure threshold, retain-until-accepted draining). **But the ORB can be permanently corrupted** by CRITICAL-1.
5. **Can any request still be silently lost?** **Yes.** Reproduced three times: promoted CRB read, promoted CRB writeback, and duplicate bypass read. The prior claim that no dispatch path can lose a request was based on *queue-full* analysis only and did not consider *queue-merge*.
6. **Are timing behaviors correct?** **Yes** for local/far DRAM timing, controller frontend/backend (including the single-vs-double backend distinction), far-link latency, and their additivity. **No** for the derived bandwidth-utilization statistic (HIGH-1).
7. **Is bypass correct?** **Yes** for isolation (measured: zero near-memory, ORB, CRB, WB activity) and for latency semantics (link latency applies, controller latency does not — matching gem5). **No** for duplicate-read safety (CRITICAL-2).
8. **Are the tests sufficient?** **No.** Excellent on policies and capacity exhaustion; blind to aliasing/merge, real-LLC integration, warmup, and statistics arithmetic.
9. **Is the code ready for long simulations?** **No** — not until CRITICAL-1/2 and HIGH-1 are fixed and MEDIUM-1 instrumentation exists to prove a run was clean.
10. **What MUST be fixed first?** CRITICAL-1, CRITICAL-2, HIGH-1, and MEDIUM-1 (needed to *verify* the fixes held).
11. **What SHOULD be fixed?** HIGH-2 (warmup — required for the case-study conclusions to mean anything), HIGH-3 (false fidelity claim), MEDIUM-2, MEDIUM-3.
12. **What should NOT be implemented?** §13.
13. **What remains a documented simulator limitation?** §9-C: full-system/Linux, checkpointing, OS/thread effects, GAPBS/NPB, device-level DRAM/NVM timing, ECC-bit storage, gem5 port semantics, traffic-generator validation, 8-core.

### Engineering confidence

| Dimension | Confidence | Justification |
|---|---|---|
| **A. Core functionality** | **80%** | All 24 Table II cells exact; all 12 request flows verified; ORB/CRB/WB semantics match gem5. Deducted for one reproduced deadlock-class defect that corrupts ORB state and leaks cache indices, and for the untested aliasing class it belongs to. |
| **B. Behavioral fidelity to gem5** | **88%** | Admission ordering, conflict definition, WB threshold, eager metadata, write-back trigger point, response latencies, CRB promotion, bypass semantics, and inert `alwaysHit`/`alwaysDirty` all verified line-by-line against re-fetched sources. Deducted for the documented-but-misjustified Oracle divergence (a *deliberate, correct* deviation) and the adapted write/retry contracts. |
| **C. Test confidence** | **55%** | Table II expectations independently confirmed correct — genuinely strong. But the suite cannot fail for the most severe defect present, uses only a stubbed LLC, and never tests merge, warmup, or statistics arithmetic. High quality where it looks; significant blind spots in where it looks. |
| **D. Experimental readiness** | **40%** | Harness selects configurations correctly and guarantees per-run isolation. Deducted heavily: a corrupting defect is reachable in long runs; bandwidth utilization is wrong and can silently underflow; the DRAM cache is 96% cold in the measured region, undermining the miss-ratio-driven conclusions of all three case studies; and there is no failure, incomplete-run, or stuck-request detection to tell you any of this happened. |

**Bottom line.** The scientific core — the DRAM-cache model and its
three policies — is faithfully implemented and independently verified
against the paper's own Table II. The engineering scaffolding around it
(dispatch aliasing, warmup, statistics arithmetic, run validation) is
not yet trustworthy for unattended multi-hour experiments. The required
fixes are small and well-localized; none of them touch the policy logic.

---

*Audit performed without modifying any source file. Probes used:
`/tmp/audit/probe_merge.cc`, `probe_merge2.cc`, `probe_bypass.cc`,
`probe_table2.cc`; gem5 sources re-fetched to `/tmp/audit/dis_*`.*

---

## 15. Remediation (post-audit fixes)

All four blocking findings — CRITICAL-1, CRITICAL-2, HIGH-1, HIGH-2 —
have been fixed and verified. Table II conformance was re-measured after
every change and remains **24/24 exact**; no policy behaviour, no
architectural semantics and no previously passing test expectation was
weakened.

### CRITICAL-1 / CRITICAL-2 — duplicate-address merge

**Root cause.** `MEMORY_CONTROLLER::add_rq()` (`src/dram_controller.cc`)
scans the destination read queue for an entry with the *same address*
and, on a match, returns that entry's index while **enqueuing nothing**.
In stock ChampSim that is safe because the caller is a `CACHE` which
merges via its own MSHR and expects no per-dispatch response. The DCM is
different: it requires exactly one `return_data()` per dispatched read to
advance the owning ORB entry. `trySend()` ignored the return value, so a
merged read was recorded as sent, its ORB entry was left in
`DCM_WAITING_LOC_MEM_READ_RESP` forever, and — since a live ORB entry
permanently owns its DRAM-cache index — every later request to that index
was blocked for the rest of the run.

The collision arises because `promoteFromCRB()` executes *inside*
`return_data()`, while `MEMORY_CONTROLLER::process()` only removes the
completing queue entry *after* `return_data()` returns. A request
promoted for that same address therefore collides with the entry that is
still resident.

**Fix** (`src/dram_cache_manager.cc`, `trySend()`): inspect the return
value. `rc >= 0` means "merged, not enqueued" — every other `add_rq()`
path (warmup shortcut, write-queue-forward service, normal insert)
returns `-1`, so this discriminates the merge precisely. On a merge the
packet is handed back to the caller's existing retain-and-retry path and
succeeds on a later cycle once the colliding entry has drained (bounded
by one cycle: the entry is removed immediately after `return_data()`).
A new `stats.dispatchMergeRetries` counter records each occurrence.

Duplicate merging was **not** disabled globally, per the task
constraint. **Write** merges are still accepted as successful sends:
coalescing two writes to one address is legitimate write-queue
behaviour, generates no response either way, and cannot lose anything in
a model that carries no data.

A second instance of the same hole was found while verifying the fix:
`processPendingFarDispatches()` released packets by calling
`farMC->add_rq()`/`add_wq()` **directly**, bypassing the new check —
unlike the near-side release path, which already went through
`trySend()`. That is why bypass mode (CRITICAL-2) still lost a duplicate
read after the first fix. `processPendingFarDispatches()` now releases
through `trySend()`, making both release paths symmetric.

**Tests** — `tests/test_dcm_duplicate_merge.cc` (6 blocks, all passing):
two concurrent reads to one address via CRB promotion; the LLC-reachable
read + dirty-writeback pairing; proof the index is **not** permanently
poisoned (follow-up traffic to the same index still completes); proof of
**no duplication** (two logical requests ⇒ exactly two serviced reads,
even though three `add_rq` attempts occur because one is refused and
retried); the bypass duplicate-read case; and the same collision under
BEAR and Oracle, confirming the fix sits below the policy layer.

### HIGH-1 — bandwidth-utilisation denominator

**Root cause.** `print_dcm_stats()` computed the ROI duration as
`finish_sim_cycle - begin_sim_cycle`, but `finish_sim_cycle` is *already*
the ROI duration (`src/main.cc` assigns it as
`current_core_cycle - begin_sim_cycle`). The second subtraction
double-counted the warmup and, whenever the ROI was shorter in cycles
than the warmup, **unsigned-wrapped** to ~1.8e19 — which the
`elapsedCycles > 0` guard did not catch, so the run silently reported
~1e-16% instead of failing.

**Fix.** `main.cc` now passes `finish_sim_cycle` directly. The
arithmetic itself was factored into a pure, unit-testable helper
`dcmBandwidthUtilization(opCount, mtps, elapsedCycles)`
(`inc/dram_cache_manager.h`), which returns `0.0` for degenerate inputs
rather than dividing by zero. The printed line now also states the ROI
cycle count it used, so the denominator is auditable from the result
file.

**Verified end-to-end** on the exact case that used to wrap
(`-warmup_instructions 1000000 -simulation_instructions 100000`, warmup
477272 cycles vs. ROI 83639 cycles): previously
`LOCAL_BW_UTILIZATION: 1.73472e-16%`, now
`LOCAL_BW_UTILIZATION: 0.0286947%  (over 83639 ROI cycles)`.

**Tests** — `tests/test_dcm_bandwidth_stats.cc` (6 blocks, all passing):
hand-derived exact values (100%/50%/25%); the warmup-longer-than-ROI
case, which also demonstrates the old expression really did wrap to
18446744073709158173 cycles → 4.3e-14%; very short and oversubscribed
ROIs; degenerate inputs (zero cycles, zero MT/s) producing no NaN/inf;
purity/run-independence; and the paper's actual near/far peaks, where the
far/near utilisation ratio must equal the inverse MT/s ratio.

### HIGH-2 — DRAM cache never warmed

**Root cause.** While `all_warmup_complete < NUM_CPUS`, `add_rq()`
returned the packet immediately and `add_wq()` dropped it, without ever
touching `tagMetadataStore`. The DCM was inert for the whole warmup
phase, so every measured region began against a stone-cold DRAM cache —
96% of ROI misses were compulsory on the 600M run — contradicting the
paper's Section V methodology.

**Fix.** Both warmup branches now call a new
`DRAM_CACHE_MANAGER::warmupTagUpdate()`, which applies byte-for-byte the
same tag/metadata rules as `classifyAndInstall()` (read hit keeps the
dirty bit, read miss installs clean, any write dirties the line) and
**nothing else**: no ORB/CRB/WB entry, no DRAM timing, no ROI statistic.
ChampSim's warmup contract of returning memory data immediately is
preserved exactly, so warmup speed is unchanged (measured: 2.14 IPC
during warmup, identical to before). Bypass mode installs nothing —
there is no DRAM cache in that path to warm.

At the warmup→ROI boundary `main.cc` now calls a new
`DRAM_CACHE_MANAGER::resetROIStats()`, placed beside the existing
`reset_cache_stats()` calls, which clears every ROI counter while
deliberately leaving `tagMetadataStore`, ORB, CRB, WB and the pending
queues untouched — the cache stays warm, only the statistics restart.
`stats.warmupTagUpdates` is preserved across the reset as positive
evidence that warming occurred.

**Tests** — `tests/test_dcm_warmup.cc` (8 blocks, all passing): cold
before warmup; warmup installs lines with zero ROI side effects; an ROI
access to a warmed line HITS; `resetROIStats()` clears counters but keeps
the cache warm; warmup-written dirty state survives into the ROI and
correctly drives a write-back on eviction; warming is identical under all
three policies; bypass stays bypassed; and no request is lost or
duplicated during warmup.

### MEDIUM-1 — run-validity instrumentation (also fixed)

`print_dcm_stats()` now prints the four completion counters
(`COMPLETED_READS_TO_LLC`, `COMPLETED_WRITES`, `COMPLETED_FROM_NEAR`,
`COMPLETED_FROM_FAR`), `WARMUP_TAG_UPDATES`, `DISPATCH_MERGE_RETRIES`,
and a **residual** line reporting final ORB/CRB/WB and pending-queue
occupancy, tagged either `[OK: no request left in flight]` or
`[*** WARNING: requests left in flight -- run may be invalid ***]`.
A result file is now self-validating: a stranded request can no longer
hide, which was the specific gap that made the earlier "3 fills in
flight, nothing lost" explanation unverifiable.

### Still outstanding (not fixed in this pass)

- **HIGH-3** — the `chooseInitialState()` comment still asserts that
  gem5's `isDirty` reads the *old* resident line. It does not: gem5 calls
  `handleRequestorPkt()` (which eagerly overwrites the metadata) at
  `recvTimingReq` line 366 and only then `setNextState()` at line 376, so
  `checkDirty()` there reads post-update state. The port's *behaviour* is
  correct and paper-faithful (Table II 24/24) and must not change; only
  the stated justification is wrong. Documentation-only defect.
- **MEDIUM-2** — `run_case_studies.sh` still has no failure or
  incomplete-run detection and still overwrites prior results silently.
- **MEDIUM-3** — no test drives the real `CACHE`/LLC; all suites use a
  `FAKE_LLC` stub.
- **LOW-1/LOW-2, COSMETIC-1** — unchanged.

---

## 16. NEW FINDING — address-granularity mismatch across the LLC boundary (CRITICAL — **FIXED**, stage 15)

**Discovered by** the real-LLC integration test added for MEDIUM-3
(`tests/test_dcm_llc_integration.cc`, Test 7) — precisely the class of
defect that a stubbed `FAKE_LLC` can never expose, which is why that gap
was worth closing.

**Status**: reported unfixed at the time of discovery (it fell outside
the three issues that pass was scoped to). **Now fixed** — see
"Resolution" at the end of this section. The description below is kept as
the original finding record; the measured evidence it cites is the
*pre-fix* behaviour.

### The defect

ChampSim's `CACHE` passes `PACKET::address` as a **block address**
(`full_addr >> LOG2_BLOCK_SIZE`). `DRAM_CACHE_MANAGER::returnIndexDC()`
then divides by `DCM_BLOCK_SIZE` (64) **a second time**:

```cpp
uint64_t DRAM_CACHE_MANAGER::returnIndexDC(uint64_t address) {
    uint64_t blockAddr = address / DCM_BLOCK_SIZE;   // address is ALREADY in blocks
    return blockAddr % numLines;
}
```

**Measured evidence** (probe through the real `CACHE`):

```
submitting full_addr 0x40000000 and 0x40000040   (two DISTINCT 64B lines)
  DCM received: address=0x1000000  full_addr=0x40000000
  DCM received: address=0x1000001  full_addr=0x40000040
DCM stats: totalRequests=2  misses=1  hits=1     <-- the two lines ALIASED
```

Two genuinely distinct 64-byte lines are reported as a miss followed by a
**hit**.

### Consequences in the production path

| Quantity | Paper / intended | Actual in full-binary runs |
|---|---|---|
| DRAM-cache line size | 64 B | **4096 B** (64 blocks alias per index) |
| Modelled capacity | 128 MB | **8 GB** (2,097,152 lines × 4 KB) |
| Hit rate | workload-determined | **systematically overstated** |
| Miss / cold-miss counts | workload-determined | **systematically understated** |

**CONFIRMED (stage 16).** This explains the 600M BASELINE run's implausible profile —
99.87% hit rate with only ~12 K misses over 500 M instructions on
`403.gcc`. With 64 adjacent lines collapsing into one index, spatial
locality is counted as temporal reuse.

### Why no existing test caught it

Every unit suite constructs packets with `p.address = <byte address>`,
which is the form `returnIndexDC()` expects, so index/tag arithmetic is
self-consistent inside those tests and Table II still measures 24/24
exact. Only the real `CACHE` supplies the other convention. The unit
tests are not wrong — they simply cannot observe the boundary.

### Resolution (applied — stage 15)

**Root cause.** `PACKET::address` is a **cache-line address** everywhere
in this ChampSim tree (`address == full_addr >> LOG2_BLOCK_SIZE`), and
that convention is uniform: the CPU sets it that way
(`src/ooo_cpu.cc:1696,2253`), `CACHE::get_set()` masks `address`
*directly* (`src/cache.cc:1058`), `CACHE` forwards the packet to
`lower_level` unmodified (`src/cache.cc:661,672,115,445`), and
`MEMORY_CONTROLLER::dram_get_channel()` uses `shift = 0`
(`src/dram_controller.cc:634`). The DCM was the **only** component that
disagreed, dividing by the line size a second time.

**Fix — a localized normalization at the DCM interface.** The global
meaning of `PACKET::address` is *not* changed; source inspection proved
it was already correct and consistent everywhere else, so the correct
architectural fix was to make the DCM agree with the existing contract:

```cpp
uint64_t DRAM_CACHE_MANAGER::returnIndexDC(uint64_t address)
{   // `address` IS the cache-line address -- see the contract comment above
    return address % DCM_NUM_LINES;   // direct-mapped
}
uint64_t DRAM_CACHE_MANAGER::returnTagDC(uint64_t address)
{
    return address / DCM_NUM_LINES;
}
```

`DCM_NUM_LINES = DCM_DRAM_CACHE_SIZE / DCM_BLOCK_SIZE = 2,097,152` is a
single source of truth: it is both the index modulus and the size of
`tagMetadataStore` (`tagMetadataStore.resize(DCM_NUM_LINES)`), so
`DCM_NUM_LINES * DCM_BLOCK_SIZE == 128 MiB` holds by construction and the
index can never fall outside the store. `tagMetadataStore` remains the
same compact per-line metadata vector — nothing larger is allocated or
iterated. A ~45-line contract comment above `returnIndexDC()` records the
convention and its source evidence.

**Table II was not touched.** Expected values are unchanged and remeasured
**24/24 exact** after the fix.

**Unit tests corrected, not weakened.** Eighteen sites in
`test_dcm_baseline.cc` / `test_dcm_bear.cc` / `test_dcm_oracle.cc` and
`idxOf` in `test_dcm_warmup.cc` had hand-rolled the *old* index formula to
locate metadata slots; they now call `dcm.returnIndexDC()` instead. That
is the test's packet-address convention being corrected — no expected
result changed.

**Test 7 is no longer a characterisation test.** It now asserts the
correct behaviour (`0x40000000` and `0x40000040` → indices 0 and 1,
`misses == 2, hits == 0`, both resident simultaneously). Three further
correctness tests were added alongside it: Test 8 (same-index /
different-tag conflict with dirty replacement and writeback), Test 9
(direct-injection vs real-LLC entry-path equivalence: identical index,
tag, metadata, hit/miss and operation sequence), Test 10
(capacity/granularity — `DCM_NUM_LINES * DCM_BLOCK_SIZE == 128 MiB`, and
line *L* vs *L + DCM_NUM_LINES* share an index but differ in tag by 1).

**Verification**: 16/16 suites pass; Table II 24/24 exact. A short real
trace through the actual binary, A/B on the same trace:

| metric | before (defective) | after (correct) |
|---|---|---|
| LLC misses | 422 | 422 |
| DCM hits | 395 | **0** |
| DCM misses | 27 | **422** |
| cache fills / far reads / local writes | 27 | **422** |
| DCM hit rate | 93.6% | **0.0%** |

Identical LLC misses in both confirms the blast radius was DCM-internal
only. The 395 pre-fix "hits" were pure aliasing: 422 distinct 64-byte
lines collapsing into 27 4-KiB super-lines (~15.6 lines each).

### Severity

**CRITICAL for experimental validity** (every Case Study number depends
on DRAM-cache hit/miss behaviour), though **not** a stability or
data-loss defect: nothing was lost, stranded or duplicated, and all
request-conservation invariants held exactly both before and after.

**Now resolved.** Any full-binary numbers collected before stage 15 must
be regenerated; none had been, so nothing published is affected. The
Case Study matrix is no longer blocked by this finding.

See `docs/gem5_to_champsim_mapping.md` §"ChampSim PACKET::address
convention and DCM normalization" for the full per-layer contract.

---

## 17. ChampSim PACKET::address convention and DCM normalization

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


---

## 18. Warmup methodology — audit finding P1, RESOLVED (stage 18)

The final independent audit's highest-severity finding was that the fixed
100M-instruction warmup does not reproduce the paper's warmed-cache
methodology: cold misses reached 32–90% of ROI misses on 5 of 6 traces
(zeusmp 47.1% of all accesses), and **55.5% of Oracle's benefit on zeusmp
— the largest speedup in the matrix — came from cold misses the paper
excludes by construction.**

That finding is now addressed by a DRAM-cache-**state** warmup termination
criterion, adopted from gem5's own `cache_warmup_ratio` parameter
(`PolicyManager.py:68`, default 0.7) and its gate at
`policy_manager.cc:1522`. Because a cold miss is the first fill of a
previously-invalid line, gem5's `numColdMisses` counts distinct lines
populated — its rule already is a distinct-fill criterion.

Full specification, the footprint-saturation second condition, the
termination cap, and why the criterion cannot be gamed:
`docs/limitations.md` § "DRAM-cache warmup termination criterion".

**Scope of the fix.** This changes *when warmup ends*, nothing else. All
Table-II behaviour, ORB/CRB/WB semantics, timing, link latency, bypass,
memory controller, cache hierarchy and packet semantics are untouched, and
`--warmup_instructions` can now only be extended, never shortened — so any
run that already met the criterion is bit-identical to before.

**Still open from the audit** (not in this stage's scope): P2 (no NVM
far-memory model; no `NO_DRAM_CACHE` x link-latency runs, so paper Fig. 13
remains unobtainable), P3 (stale pre-granularity-fix file
`BASELINE_postfix__403.gcc-16B.txt` still present in `Result_Verify_pls/`),
P4 (ORB/CRB/WB capacities never exercised at `NUM_CPUS=1`).

**The 42-run matrix predates this change and must be regenerated** before
per-trace Case Study 1/2 numbers are published.
