# Final Paper / gem5 / ChampSim Coverage Audit

**No code was changed to produce this document.** Everything below was
re-verified against the actual source during this audit: both papers (re-read
in full), the actual `dram_cache_disaggregated` gem5 branch (re-pulled
files: `policy_manager.{hh,cc}`, `PolicyManager.py`,
`disaggregated_dram_cache_script.py`), the current
`inc/dram_cache_manager.h`/`src/dram_cache_manager.cc` (re-grepped for
every implemented method, not assumed from memory), and all 8
`tests/test_dcm_*.cc` files. Two previously-undocumented facts were found
during this re-verification (Section 3.A and Section 5) — flagged
explicitly below rather than folded in silently.

---

## 1. Paper Functionality Audit

Classification key: **EXACT** = implemented exactly as described;
**ADAPTED** = implemented with a documented, deliberate adaptation;
**NOT-REQUIRED** = not implemented, not required for core functionality;
**NOT-REPRODUCIBLE** = cannot be reproduced in trace-based ChampSim;
**MISSING** = not implemented and arguably should be.

| # | Item (paper section) | Classification | Notes |
|---|---|---|---|
| 1.1 | DRAM Cache Manager as memory-controller drop-in (§II) | **EXACT** | `DRAM_CACHE_MANAGER` = `uncore.LLC.lower_level` |
| 1.2 | Discrete model, no shared data bus (§II) | **EXACT** | `nearMC`/`farMC` independent `MEMORY_CONTROLLER`s |
| 1.3 | ORB (§II, §III) | **EXACT** | 128 entries, admission-gated, `checkConflictInORB` |
| 1.4 | CRB (§II, §III) | **EXACT** | 32 entries, FIFO-per-index promotion |
| 1.5 | WB Buffer (§II, §III) | **ADAPTED** | Structural size (64) + gem5's *actual* admission threshold (`orbMaxSize/2`=64, verified identical numerically but different mechanism, see `gem5_to_champsim_mapping.md`) both ported; drain is fire-and-forget (gem5 models `sendTimingReq` retry, this port doesn't — see item 3.C below) |
| 1.6 | Tag/metadata co-located with data, ECC-bit storage (§II) | **ADAPTED** | Modeled as a separate `tagMetadataStore` array (same as gem5's own `tagMetadataStore` — gem5 *also* doesn't literally simulate ECC bits, it uses a separate lookup table representing the same information; this port matches gem5's actual implementation, not a naive literal reading of "ECC bits" from the prose) |
| 1.7 | Direct-mapped, 64B granularity, insert-on-miss, write-back (§III) | **EXACT** | `returnIndexDC`/`returnTagDC`, `classifyAndInstall` |
| 1.8 | Baseline policy = CascadeLakeNoPartWrs (§III–IV) | **EXACT** | Full state machine ported, Table-II-verified |
| 1.9 | BEAR-Wr-Opt (§V-a, Table II) | **EXACT** | Ported directly from gem5 code (not paper prose — see 5.1 below); Table-II-verified |
| 1.10 | Oracle / RambusHypo (§V-b, Table II) | **EXACT** | Ported directly from gem5 code; Table-II-verified |
| 1.11 | Table II access-amplification specification | **EXACT** | All 3 policies × 6 applicable cases = 18 cells verified exactly |
| 1.12 | Near = HBM2, far = DDR4/NVM technology split (§III, Table I) | **ADAPTED** | Bandwidth exact (32/19.2 GB/s → 4000/2400 MT/s); tRP/tRCD/tCAS NOT differentiated (documented gap, §2 below); NVM far profile not implemented at all (bandwidth only defined for DDR4) |
| 1.13 | Far-memory link latency, Case Study 3 (§VI) | **EXACT** | 100/500/1000ns, applied to both far-read and far-write, verified to the exact cycle |
| 1.14 | Controller frontend/backend latency, 20ns round-trip (Table I) | **MISSING** | **Re-verified this audit, worse than previously documented**: gem5 applies `frontendLatency + backendLatency` (or `+2×backendLatency` for the miss-then-respond path) to **every single response**, for **every policy**, at 7 real (non-dead) call sites in `policy_manager.cc`. `DCM_CTRL_LATENCY_CYCLES` is defined in this port but applied **nowhere**. This is a bigger gap than "not requested yet" — it is real, systematic, affects every measurement, and previous docs (`project_status.md`) undersold it as a minor follow-up. See Section 3.A. |
| 1.15 | `bypass_dcache` / "No-DRAM-Cache" comparison system (§III Fig 2b, Case Studies 1–2) | **MISSING (newly found this audit)** | gem5's `PolicyManager` has a real, live `bypassDcache` flag (checked at 3 call sites, not dead code) that routes traffic straight to far memory, implementing the paper's "No-DRAM Cache" comparison configuration directly inside the same `PolicyManager`. **This port has no equivalent inside `DRAM_CACHE_MANAGER`.** The same *effect* is achievable in ChampSim today by wiring `uncore.LLC.lower_level = &uncore.DRAM` directly instead of `&uncore.DCM` (bypassing the manager entirely, one line in `main.cc`) — but this is a manual rewiring, not a runtime-toggleable, documented, tested feature. Needed for Case Study 1/2 reproduction (see Section 5). not found or flagged in any prior-stage documentation — genuinely new. |
| 1.16 | Traffic-generator validation methodology (§III-C, Fig. 4) | **NOT-REPRODUCIBLE (exactly), ADAPTED (in spirit)** | gem5's validation uses a `PyTrafficGen` SimObject with configurable read%/miss-ratio/dirty-ratio, run for 10ms to reach saturated bandwidth. ChampSim has no traffic-generator SimObject equivalent; this port's own tests (`tests/test_dcm_*.cc`) are the adapted equivalent — synthetic PACKETs driven directly at the manager, same spirit, different mechanism (deterministic small scenarios, not a saturated-bandwidth-seeking generator) |
| 1.17 | Full-system Linux boot + 100ms warmup + checkpoint methodology (§III-C, Fig. 3) | **NOT-REPRODUCIBLE** | Fundamental to ChampSim being trace-based, not full-system. ChampSim's own warmup-instructions-then-ROI is the available (non-equivalent) analog. Documented in `limitations.md` since the skeleton stage |
| 1.18 | 3.5% average cold-miss ratio during restore (§III-C) | **NOT-REPRODUCIBLE (as a specific number)** | This is a *result* of gem5's specific checkpoint methodology on specific full workloads; not a parameter to port. ChampSim traces will have their own, different cold-miss characteristics |
| 1.19 | NPB (C class) + GAPBS (2²² vertices) multithreaded workloads (§III-B) | **NOT IMPLEMENTED — workload availability, not architecture** | This port has been exercised only on two short ChampSim traces (`401.bzip2`, `462.libquantum`) as smoke tests, not the paper's actual workload suite. ChampSim trace format is fundamentally different from gem5 full-system binaries; the paper's *exact* workloads cannot be re-run, but *other* ChampSim traces of comparable character could be, if available. See Section 5 |
| 1.20 | Case Study 1: baseline vs. no-DRAM-cache, BIPS/speedup, MPKI/HPKI, bandwidth utilization (§IV) | **PARTIAL — architecture ready, experiment not run** | The DRAM cache manager itself is fully capable of producing the underlying data (miss ratios, MPKI/HPKI-equivalent stats, bandwidth-relevant counters all exist in `DCM_STATS`); the *comparison experiment itself* (running real workload traces through both configs and comparing BIPS) has not been executed — see Section 5 |
| 1.21 | Case Study 2: BEAR-Wr-Opt / Oracle speedup vs. baseline and vs. no-DRAM-cache (§V) | **PARTIAL — architecture ready, experiment not run** | Same as 1.20; both policies exist and are individually verified, but the paper's actual comparative speedup experiment (Fig. 10, 11) has not been run |
| 1.22 | Case Study 3: DDR4 vs. NVM far memory × 3 link latencies (§VI) | **PARTIAL, and NVM half MISSING** | Link latency mechanism exists and is verified exactly; DDR4 far-memory config exists; **NVM far-memory config does not exist** (no `configureFarAsNVM()`), so only half of this case study's matrix (DDR4 × 3 latencies) could currently be run, and even that hasn't been run as an actual experiment |
| 1.23 | Related-work positioning, analytical-model comparison (§VII) | **NOT-REQUIRED** | Prose/positioning content, not architecture |
| 1.24 | "Enables hardware/software co-design research" framing (§VIII) | **NOT-REQUIRED** | Aspirational framing, not a concrete requirement |

---

## 2. gem5 ↔ ChampSim Component Audit

| Component | gem5 responsibility | ChampSim responsibility | Behavioral equivalence | Difference | Reason |
|---|---|---|---|---|---|
| Policy state machine (baseline) | `setNextState`/`handleNextState`, `pol==CascadeLakeNoPartWrs` guards | `chooseInitialState()` (start only) + shared `driveState()`/`return_data()` | **Equivalent** | gem5 duplicates the full state machine per policy; this port shares post-`start` states across all 3 policies | Deliberate, verified via diff tests (Stage 5/6), not assumed |
| Policy state machine (BEAR) | `setNextState`, `pol==BearWriteOpt`, byte-identical `handleNextState` to baseline | Same shared machinery + one `chooseInitialState()` branch | **Equivalent** | Same as above | Same |
| Policy state machine (Oracle) | `setNextState`, `pol==RambusHypo`, byte-identical `handleNextState` to baseline | Same shared machinery + one `chooseInitialState()` branch | **Equivalent** | Same as above | Same |
| ORB | `std::map<Addr, reqBufferEntry*>` | `std::map<uint64_t, DCM_ORB_ENTRY*>` | **Equivalent** | None | Direct port |
| CRB | `std::vector<pair<Tick,PacketPtr>>`, front-to-back scan | `std::vector<DCM_CRB_ENTRY>`, front-to-back scan | **Equivalent** | None | Direct port |
| WB buffer | `std::deque<pair<Tick,PacketPtr>>` (`pktFarMemWrite`), event-scheduled one-send-per-tick drain | `std::deque<DCM_WB_ENTRY>`, one-per-`operate()`-cycle drain | **Equivalent** | gem5's drain reschedules based on port retry/backpressure from the real far controller; this port's drain always "succeeds" (fire-and-forget) | Item 3.C below |
| Admission ordering | conflict → WB-pressure → ORB-full → admit (`recvTimingReq`) | Identical order in `add_rq`/`add_wq` | **Equivalent** | None | Direct port, re-verified during this audit at `policy_manager.cc:296-362` |
| Conflict detection | index-based, ORB-only (`checkConflictInDramCache`) | `checkConflictInORB()`, index-based, ORB-only | **Equivalent** | None | Direct port |
| CRB promotion | `resumeConflictingReq`, oldest-match, preserves original arrival tick | `promoteFromCRB()`, oldest-match, preserves original arrival cycle | **Equivalent** | None | Direct port |
| Tag/metadata classification | `checkHitOrMiss`/`checkDirty`, eager update in `handleRequestorPkt` at admission | `classifyAndInstall()`, eager update at admission | **Equivalent** | None | Direct port, ordering re-verified against `policy_manager.cc:1345-1444` |
| Dirty write-back trigger | `handleDirtyCacheLine`, called from `locMemRecvTimingResp` at physical tag-check-read completion | `pushDirtyWriteBack()`, called from `return_data()` at the same point | **Equivalent** | None | Direct port |
| Timing model | gem5 `DRAMInterface`/`MemCtrl` (JEDEC-derived per-technology timing, event-driven) | ChampSim `MEMORY_CONTROLLER` (generic bank/row/dbus model, cycle-driven) | **Structurally different, functionally analogous** | ChampSim's timing model is coarser and shared code across all technologies (only rate differs, not per-technology latency) | Fundamental to reusing ChampSim's existing timing model rather than writing a new one (per original kickoff instructions) |
| Response ordering | "respond-before-fill" on read miss (`accessAndRespond` before the fill write is issued) | Same: `completeRequest()` before the background fill `add_wq()` | **Equivalent** | None | Direct port, verified via request-flow logs |
| Retry / backpressure semantics | `recvTimingReq` returns `false`, requestor's `sendRetryReq()` is invoked later | ChampSim's `CACHE` never inspects `add_rq`/`add_wq`'s return value — it pre-checks `get_occupancy()==get_size()` | **Equivalent effect, different mechanism** | gem5: reactive retry. ChampSim: proactive pre-check | Documented adaptation since the skeleton stage; ChampSim's own pre-existing contract, not invented for this port |
| Request/packet semantics | `PacketPtr`, `MemCmd`, coherence-aware fields | `PACKET`, `type` (LOAD/RFO/WRITEBACK/PREFETCH), no coherence fields | **Equivalent for what's used** | ChampSim's `PACKET` is simpler; this port only ever needs read/write distinction plus address/cpu, which both models provide | ChampSim's own pre-existing type |
| Memory-controller interaction | Two `RequestPort`s, `sendTimingReq`/`recvTimingResp` | Two `MEMORY_CONTROLLER*` pointers, `add_rq`/`add_wq`/`return_data` | **Equivalent effect, collapsed to one callback** | gem5: two distinct callback functions (one per port). ChampSim: one `return_data()`, disambiguated via `ORB[addr]->state` | Documented deliberate adaptation since the skeleton stage — verified safe because at most one sub-request per ORB entry is ever outstanding |
| Link latency | Implied by the paper's "configurable link... between DRAM Cache Manager and remote backing store" (§VI); **no explicit link-latency field found in `PolicyManager.py`** — likely modeled via a standard gem5 interconnect SimObject (Bridge/Link) external to `PolicyManager`, not confirmed from the files pulled for this audit | `linkLatencyCycles`/`setLinkLatency()`, internal to `DRAM_CACHE_MANAGER`, applied via `dispatchToFar()` | **Behaviorally equivalent, cannot confirm mechanism-level equivalence** | This port models the link *inside* the manager (a request-holding queue); gem5 most likely models it as a separate topological component the manager's `farReqPort` connects through | Honest limitation: the exact gem5 SimObject used for the link was not found in the files pulled for this port (`policy_manager.{hh,cc}`, `PolicyManager.py` contain no link-latency field). The *behavioral* result (fixed added latency on the far path only) is verified equivalent; the *implementation mechanism* is a documented inference, not a confirmed match |
| Statistics | `statistics::Scalar`/`Average`/`Formula`, ~40 named stats in `PolicyManagerStats` | `DCM_STATS` struct, ~45 fields + 4 computed methods | **Equivalent coverage, different types** | gem5 has native `statistics::Average`/`Formula` types; this port uses plain counters + sum/count pairs with computed averages | Straightforward equivalent, not a gem5-internal type ChampSim has |

---

## 3. The Four Remaining Items — Re-Investigated

### A. Controller frontend/backend latency

1. **Explicitly required by the paper?** Yes — Table I states "Frontend/Backend Latencies: 20 ns round-trip" as a named baseline parameter.
2. **Required for the paper's core architecture?** Yes, structurally — it's part of the DRAM Cache Manager's own modeled overhead, distinct from the memory devices' timing.
3. **Required for reproducing the reported experiments?** Yes — it is applied to **every single response** in gem5 (verified this audit: 7 live call sites, all three policies), so it is not a corner case; omitting it means every latency number this port could produce is systematically missing a fixed 20ns (≈80 cycles at this port's `CPU_FREQ`) per completed request.
4. **Does gem5 implement it?** Yes, confirmed live (not dead code) this audit: `frontendLatency(p.static_frontend_latency)`, `backendLatency(p.static_backend_latency)`, both defaulting to 10ns each, applied in every `accessAndRespond()` call.
5. **Can current ChampSim implement it faithfully?** Yes, easily — `completeRequest()` already has a single call site for the LLC-facing callback; adding a fixed cycle delay there (mirroring how `linkLatencyCycles`/`dispatchToFar()` already delays far-bound dispatch) is a small, well-contained, low-risk change following an already-established pattern in this codebase.
6. **Would implementing it improve correctness?** Yes, measurably — it is a real, non-negligible (20ns) additive latency applied uniformly.
7. **Would it only add complexity without benefit?** No — this is the opposite case: it is a real gap, not a complexity trap.

**Classification: SHOULD IMPLEMENT.** Re-graded upward from prior documentation (which listed it as a low-priority "smaller, independent follow-up") — this audit found it is applied unconditionally to every response in gem5, not a peripheral detail. Not marked MUST because no test or experiment in this project has yet needed absolute latency accuracy (all verification so far is relative — hit vs. miss, policy vs. policy — where a uniform additive constant cancels out in every comparison made so far). It would become MUST the moment absolute-latency numbers (not just relative comparisons) are reported for any Case Study reproduction.

### B. NVM far-memory profile

1. **Explicitly required by the paper?** Yes — Case Study 3 explicitly runs the far memory as NVM in addition to DDR4 (§VI, Fig. 12b/13b).
2. **Required for the paper's core architecture?** No — the *architecture* (near/far split, configurable technology) doesn't require NVM specifically; DDR4-only already exercises the same mechanism.
3. **Required for reproducing the reported experiments?** Yes, for the NVM half of Case Study 3 specifically (Fig. 12b, 13b) — the DDR4 half (Fig. 12a, 13a) doesn't need it.
4. **Does gem5 implement it?** Yes — the paper cites gem5's NVM/NVRAM interface model (ref [15]) and states "DDR4 and NVM models of gem5 provide 19.2 GB/s theoretical peak bandwidth per single channel, though NVM has higher read and write latencies than DDR4" — same bandwidth, different (higher) latency.
5. **Can current ChampSim implement it faithfully?** Only the bandwidth part (19.2 GB/s, same as DDR4 — trivial, would be numerically identical to `configureFarAsDDR4()`). The *latency* part cannot be implemented faithfully: the paper does not give NVM's exact tRP/tRCD/tCAS-equivalent numbers in its own text, the same documented gap that already blocks HBM2-vs-DDR4 latency differentiation (Section 1.12).
6. **Would implementing it improve correctness?** Only marginally as a bandwidth-only stand-in (which would then be indistinguishable from DDR4 in this port's model, defeating the purpose) — a real improvement requires the latency numbers this project does not have.
7. **Would it only add complexity without benefit?** A bandwidth-only `configureFarAsNVM()` would add a function that behaves identically to `configureFarAsDDR4()` — complexity without benefit. A faithful one needs data this project doesn't have.

**Classification: OPTIONAL** (bandwidth-only stand-in, low value) **/ DO NOT IMPLEMENT** (faithful version, blocked on missing data) — net: **OPTIONAL**, and only worth doing if a source for NVM's actual read/write latency (the paper cites the gem5 NVM interface model, ref [15], and Wang et al.'s NVM characterization, ref [5]) becomes available to this project.

### C. WB retry-on-NACK

1. **Explicitly required by the paper?** Not named explicitly; implied by "Whenever there is bandwidth available **or if the WB buffer becomes full**, the DRAM cache manager sends these write backs..." (§II) and the `tc` discussion in §V-A ("Once the WB buffer is full, the cache manager prioritizes the writes in the WB buffer over the requests in ORB").
2. **Required for the paper's core architecture?** Yes, in principle — it's part of the described WB-buffer-full prioritization behavior.
3. **Required for reproducing the reported experiments?** Only for workloads that drive the WB buffer to real exhaustion (paper's own `tc` example is exactly this case) — for lighter workloads, irrelevant.
4. **Does gem5 implement it?** Partially — gem5 models retry generically via `sendTimingReq()` returning `false` and a later `recvReqRetry()` callback, which `processFarMemWriteEvent` participates in like any other port user; there is no WB-specific retry logic beyond the generic port-retry mechanism every gem5 port has.
5. **Can current ChampSim implement it faithfully?** Partially — this port's `farMC` (a real `MEMORY_CONTROLLER`) already has its own WQ with a real capacity (64 entries by default); a faithful port would mean `drainWB()` checking `farMC->get_occupancy(2,...) == farMC->get_size(2,...)` before draining and deferring if full, which is a small, well-contained addition given `drainWB()` already exists as a single call site.
6. **Would implementing it improve correctness?** Only for write-heavy, high-miss-ratio workloads that actually exhaust `farMC`'s WQ — not observed to matter in any test or trace run so far (confirmed again this audit: no test drives `farMC`'s WQ to exhaustion).
7. **Would it only add complexity without benefit?** For the traces/tests exercised so far, yes — no observed scenario needs it yet. For a faithful large-scale Case Study 1/2 reproduction with real write-intensive NPB/GAPBS-style traces (the paper's own `tc`/`bt`/`sp` examples), it could matter.

**Classification: OPTIONAL**, trending toward **SHOULD IMPLEMENT** specifically if/when Case Study reproduction with real write-intensive traces is attempted (Section 5) — low implementation cost, currently unexercised.

### D. Full Case-Study-scale reproduction

1. **Explicitly required by the paper?** The Case Studies themselves are the paper's primary contribution; reproducing them was never an explicit instruction in any stage of this project, however (each stage's instructions asked for architectural fidelity and Table-II correctness, not experiment reproduction).
2. **Required for the paper's core architecture?** No — the architecture is complete and independently verified without running the case studies.
3. **Required for reproducing the reported experiments?** By definition, yes — but "reproducing the reported experiments" and "having a correct architecture" are different goals; this project's instructions have consistently targeted the latter.
4. **Does gem5 implement it?** Yes (that's what the paper's Figures 5–13 are).
5. **Can current ChampSim implement it faithfully?** Only partially even in principle: ChampSim cannot reproduce full-system Linux boot, the 100ms-checkpoint warmup methodology, or the exact NPB-C-class/GAPBS-2²²-vertex workloads (different trace format entirely). A ChampSim-native analog (real ChampSim traces, ChampSim's own warmup-then-ROI methodology) could produce *qualitatively* comparable results (does the DRAM cache slow things down, does BEAR/Oracle help, does link latency hurt) but never numerically comparable ones.
6. **Would implementing it improve correctness?** It would improve *confidence* (does the implemented architecture actually produce the qualitative trends the paper reports?) but would not change whether the architecture itself is correct — that's already established via Table II and the gem5 comparison tables, which are exact, not qualitative.
7. **Would it only add complexity without benefit?** No — it has real value as a capstone sanity check, but it is a large effort (finding/generating suitable ChampSim traces, running multiple configurations, interpreting results against the paper's qualitative claims) that goes beyond what any stage's instructions have asked for so far.

**Classification: OPTIONAL.** Valuable as a final capstone if requested, but not a correctness requirement — the architecture's correctness is already established at the mechanism level (Table II, gem5 comparison tables), which is a stronger, more precise form of validation than a qualitative trace-based experiment could provide on its own.

---

## 4. Trace-Based Limitations, By Category

### A. Architectural functionality — mostly reproducible
Everything in the DRAM Cache Manager's own architecture (ORB/CRB/WB,
tag/metadata, all 3 policies, admission control) is fully reproducible
and has been reproduced — none of it depends on full-system features.
**Not reproducible**: on-chip coherence-domain interactions (the paper
states the manager is "not in the on-chip coherence domain," implying
there IS a coherence domain above it in the real system) — ChampSim's
LLC has no coherence protocol either, so this is a non-issue (neither
system needs it modeled at this level, not a gap introduced by tracing).

### B. Timing behavior — partially reproducible
**Reproducible with adaptation**: DRAM cache/backing-store timing
(reused ChampSim's existing generic model), near/far bandwidth
differentiation, link latency. **Not reproducible without additional
data**: real per-technology (HBM2/DDR4/NVM) access latencies (gem5's
values are baked into SimObjects, not in the paper text — Section 1.12,
3.B). **Missing but reproducible** (this audit's finding): controller
frontend/backend latency (Section 3.A) — this is a real gap, not an
inherent trace-based limitation; it could be added without any
architectural obstacle.

### C. Workload generation — not reproducible (exact), adaptable (equivalent)
gem5's `PyTrafficGen` (validation) and full NPB/GAPBS full-system
binaries (case studies) have no ChampSim equivalent. ChampSim traces
(instruction streams with embedded ROI markers) are a fundamentally
different input format. Cannot re-run the paper's *exact* workloads;
*could* run ChampSim-native traces through the same architecture for a
qualitative (not numeric) comparison, as noted in Section 3.D.

### D. Experimental methodology — not reproducible
Linux boot, 100ms-then-checkpoint warmup, restore-and-run-N-configs-from-
one-checkpoint — none of this exists in ChampSim's execution model.
ChampSim's warmup-instructions-then-ROI is the closest available analog,
already documented as non-equivalent since the skeleton stage.

### E. Statistics — fully reproducible
Every statistic the paper reports (miss ratio, MPKI/HPKI-equivalent
breakdown by read/write × clean/dirty, bandwidth utilization inputs,
access amplification) has a corresponding field in `DCM_STATS`, already
implemented and verified (Stage 4). Nothing statistics-related is
blocked by ChampSim's trace-based nature — this category is the most
completely reproducible of the five.

---

## 5. Case Study Reproducibility

### Case Study 1 (baseline vs. no-DRAM-cache)

| | Paper | ChampSim | Status |
|---|---|---|---|
| Configuration | HBM2 DRAM cache + DDR4 main memory, vs. DDR4-only | `configureNearAsHBM2`/`configureFarAsDDR4` exist; **no-DRAM-cache comparison config does not exist as a toggle** (Section 1.15 finding) | Config A ready; Config B needs either the `bypass_dcache`-equivalent feature or a manual `main.cc` rewiring (undocumented as a supported path today) |
| Workload/input | NPB-C, GAPBS 2²² vertices, full-system | Any ChampSim trace | Available but not the paper's exact workloads (Section 1.19) |
| Statistics required | BIPS, speedup, miss ratio, MPKI/HPKI breakdown, bandwidth utilization | All exist in `DCM_STATS` + ChampSim's own IPC tracking | **Ready** |
| Timing requirement | No extra link latency; real HBM2/DDR4 relative timing | Bandwidth differentiated; frontend/backend latency missing (3.A) | **Partially ready** |
| Expected comparison | DRAM-cache system underperforms no-DRAM-cache (speedup < 1) for miss ratios > 20% | Not yet run | — |
| Known unavoidable differences | Absolute BIPS/cycle numbers will never match gem5's; only relative trends (speedup direction, miss-ratio sensitivity) are meaningful | | |

**Sufficient to run a meaningful reproduction?** Not yet — needs (a) a
no-DRAM-cache comparison configuration (missing) and (b) a workload
trace with a comparable miss-ratio profile to NPB/GAPBS (available in
principle, not verified yet).

### Case Study 2 (BEAR-Wr-Opt / Oracle vs. baseline and vs. no-DRAM-cache)

| | Paper | ChampSim | Status |
|---|---|---|---|
| Configuration | Same HBM2/DDR4 as Case Study 1, three policies | All three policies implemented and independently selectable | **Ready** |
| Workload/input | Same NPB/GAPBS | Same caveat as Case Study 1 | Available but not exact |
| Statistics required | Speedup vs. baseline, speedup vs. no-DRAM-cache, per-workload write-hit/miss-clean fraction sensitivity | All exist (`oracleWriteHits`, `oracleCleanMisses`, `writeHitOpt*`, `cleanMissOpt*`, plus Table-II-style counts) | **Ready** |
| Timing requirement | Same as Case Study 1 | Same gap (3.A) | **Partially ready** |
| Expected comparison | Oracle > BEAR-Wr-Opt > baseline, but still < no-DRAM-cache | Not yet run | — |
| Known unavoidable differences | Same as Case Study 1 | | |

**Sufficient to run a meaningful reproduction?** Architecturally yes
(more ready than Case Study 1, since all three policies already exist)
— blocked on the same two things: no-DRAM-cache config, and workload
availability.

### Case Study 3 (link latency × DDR4/NVM far memory)

| | Paper | ChampSim | Status |
|---|---|---|---|
| Configuration | Baseline policy only, HBM2 near, DDR4 OR NVM far, link latency ∈ {100,500,1000}ns | `configureFarAsDDR4` + `linkLatencyCycles` exist and are exactly verified; **NVM far config does not exist** (Section 3.B) | **DDR4 half ready; NVM half missing** |
| Workload/input | Same NPB/GAPBS | Same caveat | Available but not exact |
| Statistics required | BIPS at each latency, speedup vs. no-DRAM-cache at each latency | All exist | **Ready** (for the DDR4 half) |
| Timing requirement | Link latency exact per the paper's 3 values | Verified exact during this audit | **Ready** |
| Expected comparison | Performance drops as latency increases; NVM drops less than DDR4 as latency grows; DRAM cache eventually outperforms no-DRAM-cache at higher latencies | Not yet run | — |
| Known unavoidable differences | NVM's relative-latency advantage cannot be shown without NVM latency data this project doesn't have | | |

**Sufficient to run a meaningful reproduction?** Only for the DDR4 half,
and still blocked on the no-DRAM-cache comparison config and workload
availability, same as Case Studies 1–2.

---

## 6. Test Coverage Audit

Matrix of paper functionality → implementation → test → result. Test
block counts independently re-counted this audit by grepping each file's
`PASS`/`FAIL` print statements (not taken from prior summaries).

| Paper functionality | Implementation | Test file (block count) | Result |
|---|---|---|---|
| DCM/LLC wiring, near/far identity | `DRAM_CACHE_MANAGER` ctor, `uncore.h`/`main.cc` | `test_dcm_skeleton.cc` (part of 14) | PASS |
| Per-instance timing independence | `set_timing()` | `test_dcm_skeleton.cc` Test 4, `test_dcm_near_far_config.cc` (8 total) | PASS |
| ORB admission + full/backpressure | `add_rq`/`add_wq`, `get_occupancy`/`get_size` | `test_dcm_skeleton.cc` | PASS |
| CRB conflict + promotion | `checkConflictInORB`/`promoteFromCRB` | `test_dcm_skeleton.cc` Tests 5–6 | PASS |
| Real tag/metadata classification | `classifyAndInstall` | `test_dcm_baseline.cc` (22 total, cases A-H + repeat + chain) | PASS |
| Dirty write-back trigger point | `pushDirtyWriteBack` | `test_dcm_baseline.cc` cases E/H, `test_dcm_wb_pressure.cc` | PASS |
| WB occupancy backpressure | `DCM_WB_PRESSURE_THRESHOLD`, `drainWB` | `test_dcm_wb_pressure.cc` (10 total) | PASS |
| Near=HBM2/far=DDR4 real bandwidths | `configureNearAsHBM2`/`configureFarAsDDR4` | `test_dcm_near_far_config.cc` (8 total) | PASS |
| Link latency (far-read + far-write, hit isolation) | `linkLatencyCycles`/`dispatchToFar` | `test_dcm_link_latency.cc` (8 total) | PASS |
| Statistics completeness + independence | `DCM_STATS` | `test_dcm_stats.cc` (8 total) | PASS |
| BEAR-Wr-Opt (all 8 Table-II-relevant cases + CRB + WB) | `chooseInitialState()` BEAR branch | `test_dcm_bear.cc` (32 total) | PASS |
| Oracle (all 8 Table-II-relevant cases + CRB + WB + comparison table) | `chooseInitialState()` Oracle branch | `test_dcm_oracle.cc` (36 total) | PASS |
| **Sum** | | **62 tests, 8 files** | **62/62 PASS** (re-confirmed for this audit before the audit) |

### Functionality with NO deterministic test (gaps, not failures)

- Controller frontend/backend latency — **not implemented**, so nothing
  to test (Section 3.A).
- NVM far-memory profile — **not implemented** (Section 3.B).
- WB retry-on-NACK — **not implemented** (Section 3.C).
- `bypass_dcache`-equivalent / no-DRAM-cache config — **not implemented**
  (Section 1.15, newly found this audit).
- Any full trace-driven Case-Study-style comparison run (only two short
  smoke-test traces have been run, and only for crash/hang/byte-identical
  checks, not for producing or validating comparative results).
- Access-amplification statistic (`DCM_STATS::accessAmplification()`)
  is exercised incidentally inside `test_dcm_stats.cc`'s Test 1 scenario
  but has no test asserting its *formula* directly against a hand-computed
  expected ratio — a minor gap (the underlying counters it's computed
  from are all individually verified, so the formula itself is simple
  arithmetic over already-verified inputs, but it has not been asserted
  in isolation).

---

## 7. Final Completion Decision

**CORE PAPER FUNCTIONALITY: COMPLETE** for everything classified EXACT
or ADAPTED in Section 1 (items 1.1–1.13); **NOT COMPLETE** for the
experiment-reproduction items (1.14, 1.15, 1.20–1.22) — but those were
never the target of any prior stage's instructions, which consistently
asked for architectural/behavioral fidelity, not experiment reproduction.

**POLICY IMPLEMENTATION: COMPLETE.** All three paper policies (baseline,
BEAR-Wr-Opt, Oracle) are implemented, independently selectable, and
individually Table-II-verified with zero cross-contamination (regression-
tested at every stage).

**GEM5 BEHAVIORAL PORT: COMPLETE** for the DRAM Cache Manager's own
logic (Section 2 — every row is "Equivalent" or "Equivalent with a
documented, deliberate adaptation"); **NOT COMPLETE** for two
newly-audited items that are real, live gem5 mechanisms with no ChampSim
counterpart: controller frontend/backend latency (3.A) and
`bypass_dcache` (1.15/5). Both are implementable; neither has been
implemented.

**TEST COVERAGE: COMPLETE** for everything that is implemented (62/62,
zero gaps found between "implemented" and "tested" in Section 6);
**NOT APPLICABLE** (not a coverage gap, an implementation gap) for the
four items in Section 3 and the no-DRAM-cache config — there is nothing
to test because the feature doesn't exist yet.

**EXPERIMENTAL REPRODUCIBILITY: PARTIAL.** The architecture is capable
of producing every statistic the paper's Case Studies report (Section 6),
and two of three Case Studies (1, 2) are architecturally ready modulo the
no-DRAM-cache comparison config and real workload traces; Case Study 3
is ready for its DDR4 half only. No Case Study has actually been run as
an experiment — this project has validated the *mechanism* (Table II,
gem5 comparison tables) rather than the *emergent experimental result*,
which is a stronger form of correctness evidence for the architecture
itself but does not by itself demonstrate the paper's reported trends.

### Remaining work, ordered by importance

1. **Controller frontend/backend latency** (Section 3.A) — SHOULD
   IMPLEMENT. Small, well-contained change (one new delay at
   `completeRequest()`'s callback site, mirroring the existing
   `dispatchToFar()` pattern); closes a real, previously-underrated,
   systematically-applied gap affecting every measurement.
2. **`bypass_dcache`-equivalent / no-DRAM-cache toggle** (Section 1.15,
   newly found) — needed before Case Study 1 or 2 can be run at all,
   since "no DRAM cache" is one of the two arms of both comparisons.
   Smallest version: a documented, tested way to wire `uncore.LLC.lower_level`
   directly to a far-only `MEMORY_CONTROLLER`, bypassing `DRAM_CACHE_MANAGER`
   entirely — no new DCM-internal code strictly required, unlike gem5's
   in-manager flag, but currently not a supported/tested configuration path.
3. **WB retry-on-NACK** (Section 3.C) — OPTIONAL now, would become
   relevant the moment a write-intensive trace is used for Case Study
   reproduction.
4. **Case Study 1/2 DDR4-only reproduction run** — once items 1–2 above
   exist, this becomes the first fully-architecturally-ready experiment;
   needs suitable ChampSim traces (miss-ratio profile comparable to
   NPB/GAPBS) identified or generated.
5. **NVM far-memory profile** (Section 3.B) — OPTIONAL, blocked on
   missing latency data; bandwidth-only version has low value.
6. **Case Study 3 full (DDR4+NVM) reproduction run** — depends on item 5.
7. Everything else previously flagged and still open (`NUM_CPUS`=1,
   global `DRAM_CHANNEL_WIDTH`, per-technology tRP/tRCD/tCAS) — unchanged
   from prior audits, still correctly classified as documented gaps, not
   regressions.
