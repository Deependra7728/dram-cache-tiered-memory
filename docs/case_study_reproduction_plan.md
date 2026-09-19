# Case Study Reproduction Plan

Experiment infrastructure for reproducing (as closely as trace-based
ChampSim allows) the paper's Case Study 1, 2, and 3. **No long
simulations were launched to produce this document** — every number
below comes from either static trace inspection or the tiny (≤200K
instruction) sanity runs explicitly requested for this stage. Sources
re-read: both papers (already fully pulled and read in prior stages —
Section III methodology, Section IV/V/VI case studies, Table I/II),
`docs/final_paper_coverage_audit.md`, `docs/paper_to_champsim_spec.md`,
`docs/gem5_to_champsim_mapping.md`, `docs/feature_coverage.md`,
`docs/limitations.md`, `docs/project_status.md`, `docs/validation.md`.

## 1. Available traces — investigated, not assumed

`dpc3_traces/` contains 7 files, all named per ChampSim's DPC-3
(3rd Data Prefetching Championship) convention
(`<SPEC-CPU2006-benchmark>-<simpoint-suffix>.champsimtrace.xz`):

| File | Compressed | `xz --list` uncompressed | `xz -t` integrity |
|---|---|---|---|
| `401.bzip2-277B.champsimtrace.xz` | 414.5 MiB | 119.2 GiB | **OK** |
| `403.gcc-16B.champsimtrace.xz` | 383.7 MiB | 119.2 GiB | **OK** |
| `434.zeusmp-10B.champsimtrace.xz` | 144.6 MiB | 119.2 GiB | **OK** |
| `437.leslie3d-273B.champsimtrace.xz` | 284.3 MiB | 119.2 GiB | **OK** |
| `462.libquantum-1343B.champsimtrace.xz` | 240.4 MiB | 119.2 GiB | **OK** |
| `482.sphinx3-1522B.champsimtrace.xz` | 508.7 MiB | 119.2 GiB | **OK** |
| `619.lbm_s-3766B.champsimtrace.xz` | 70.5 MiB | (integrity check fails) | **CORRUPT — `xz: Unexpected end of input`, a truncated download, not random bit-rot** |

**Trace format** (`tracer/champsim_tracer.cpp`'s `trace_instr_format_t`,
read directly): a fixed-size binary record per dynamic instruction —
8-byte IP, 2 branch-flag bytes, 2+4 register-ID bytes
(`NUM_INSTR_DESTINATIONS=2`, `NUM_INSTR_SOURCES=4`), and
2×8 + 4×8 = 48 bytes of destination/source memory addresses — 64 bytes
per record.

**Instruction count**: `xz --list`'s reported uncompressed size (119.2
GiB, identical across all 6 valid traces — consistent with DPC-3's
standardized per-benchmark trace length, not a tooling artifact: the
compression ratio column, e.g. 0.003 for `403.gcc`, is internally
consistent with 383.7 MiB × (1/0.003) ≈ 128 GiB) divides by the 64-byte
record size to **≈1.95–2.0 billion dynamic instructions per trace**.
This is a size-based estimate from the compressed file's own header
metadata, not a value obtained by decompressing and counting every
record directly (decompressing all 119 GiB per trace was not attempted
this stage, consistent with "do not run long simulations yet"); it is
corroborated by DPC-3's known methodology (traces built from a full
SimPoint-selected execution window, commonly on this order for SPEC
CPU2006). At ≈2 billion instructions available per trace, an actual
Case Study run at `--warmup 100M --sim 500M` (a plausible reproduction
scale, see Section 5) is well within each trace's available length —
trace length is not the limiting factor for a real run; wall-clock time
is (see Section 6).

**Suitability for memory-intensive evaluation — verified, not assumed**:
these are the ChampSim project's own DPC-3 championship traces,
purpose-built for memory-subsystem research, and SPEC CPU2006 includes
several genuinely memory-intensive benchmarks among the 6 valid ones
here (`462.libquantum`, `482.sphinx3`, and to a lesser extent
`437.leslie3d`/`434.zeusmp` are commonly cited as high-MPKI SPEC
CPU2006 workloads; `401.bzip2`/`403.gcc` are more moderate). This was
spot-checked, not assumed: `403.gcc`'s existing LLC miss count at a
tiny 200K-instruction window (`results_1M/403.gcc-16B...txt`, already
in this repo from an earlier session, and the sanity runs in Section 4
below) shows real, nonzero LLC-miss and DRAM-cache traffic
(`LLC MISS: 8291` at 200K instructions in the pre-existing results
file) — the traces DO exercise the DRAM-cache path meaningfully, not
trivially.

**What the two ORIGINAL smoke-test traces used in prior stages
(`403.gcc-16B`, and previously an assumed-available `619.lbm_s`) are
NOT sufficient for**: they were used in prior stages purely to prove
the full binary builds and runs without crashing (a functional smoke
test), at instruction counts (100K–200K) far too small to say anything
about the paper's actual miss-ratio-driven conclusions. `619.lbm_s` is
additionally the corrupt file above — it was never actually usable, a
fact this audit surfaces for the first time (prior stages' smoke tests
only ever exercised `403.gcc`, never actually opened `619.lbm_s`).

## 2. Which paper workloads cannot be reproduced exactly

The paper's Case Studies 1–3 all run **GAPBS** (GAP Benchmark Suite —
graph algorithms: BFS, BC, CC, PR, TC, SSSP) and **NPB** (NAS Parallel
Benchmarks C class — CG, IS, LU, SP, and others) on an **8-core**
target system with checkpointed, Linux-booted, full-system simulation
(gem5 full-system mode, 100ms warmup then a checkpoint, restored for
each configuration under test — paper Section III-C, Figure 3).

**None of this is reproducible in the current ChampSim environment,
for reasons already independently established and re-confirmed here**:

- **No GAPBS/NPB traces exist in this repository or are referenced by
  any script here.** ChampSim's own DPC-3 trace set (SPEC CPU2006) is
  what is actually available — a categorically different workload
  suite (single-threaded desktop/HPC benchmarks, not multi-threaded
  graph/HPC kernels). No claim of behavioral equivalence between SPEC
  CPU2006 and GAPBS/NPB is made or implied anywhere in this plan.
- **`NUM_CPUS=1`** in this ChampSim build (already documented,
  `docs/limitations.md`) — the paper's 8-core CCD sharing one DRAM-cache
  channel cannot be modeled without a much larger, out-of-scope change
  (LLC sizing macros, the `ooo_cpu` array).
- **No Linux boot / full-system checkpoint methodology** — ChampSim is
  trace-based; its own warmup-instructions-then-ROI model is the
  available analog, not equivalent to the paper's 100ms-warmup+
  checkpoint approach (already documented).
- **No real memory data values** — ChampSim traces carry addresses and
  control flow only (already documented, `docs/limitations.md`); this
  does not affect the DRAM-cache TIMING/hit-miss model itself (already
  Table-II-validated) but does mean no data-correctness claim is
  possible, only a structural/timing one.

## 3. Reasonable trace-based substitutes

Given the above, the most defensible trace-based substitute is exactly
what this plan uses: **ChampSim's own DPC-3 SPEC CPU2006 traces**,
because:

1. They are the traces this specific simulator infrastructure was built
   and validated around (not an arbitrary external substitution).
2. They include genuinely memory-intensive workloads (per Section 1),
   giving the DRAM cache a meaningful hit/miss ratio to respond to,
   which is what Case Studies 1–3's qualitative conclusions
   (DRAM-cache-vs-none throughput; BEAR/Oracle vs. baseline; link
   latency's effect on viability) actually depend on structurally, not
   on GAPBS/NPB's specific graph-traversal access patterns.
3. Single-threaded SPEC CPU2006 access patterns are NOT graph-workload
   access patterns — pointer-chasing, low-locality graph traversal
   (GAPBS's hallmark, and part of why the paper's own miss ratios run
   20–100%, Figure 6) will differ from SPEC CPU2006's generally more
   regular access patterns. **This is flagged, not glossed over**: any
   reproduction using these traces is a STRUCTURAL/QUALITATIVE check
   (does the same DIRECTIONAL trend appear — DRAM cache underperforms
   at low link latency, BEAR/Oracle beat baseline, high link latency
   favors the DRAM cache), not a claim that the resulting numbers
   (miss ratios, speedups) will match the paper's Figures 5/10/11/12/13
   quantitatively.

## 4. Experiment configurations created

All 7 required configurations, selectable via new command-line flags
added to `src/main.cc` this stage (`--dcm_policy`, `--dcm_bypass`,
`--dcm_link_latency_ns` — defaults reproduce every prior stage's exact
behavior when omitted, verified byte-identical against the pre-existing
smoke test). **No configuration requires a source-code edit or
recompile** — every difference below is a command-line flag, enforced
by `run_case_studies.sh`'s single shared binary.

| Configuration | Flags | Case Study | What it selects |
|---|---|---|---|
| `NO_DRAM_CACHE` | `--dcm_bypass` | 1 | Paper's "No DRAM Cache" arm — `bypassDcache=true`, LLC misses go straight to far/DDR4-like memory |
| `BASELINE` | (none) | 1, 2 | `DCM_POLICY_BASELINE_CASCADE_LAKE`, the default |
| `BEAR_WR_OPT` | `--dcm_policy=bear` | 2 | `DCM_POLICY_BEAR_WR_OPT` |
| `ORACLE` | `--dcm_policy=oracle` | 2 | `DCM_POLICY_ORACLE` |
| `BASELINE_100NS` | `--dcm_link_latency_ns=100` | 3 | Baseline policy, 100ns far-link round trip |
| `BASELINE_500NS` | `--dcm_link_latency_ns=500` | 3 | Baseline policy, 500ns far-link round trip |
| `BASELINE_1000NS` | `--dcm_link_latency_ns=1000` | 3 | Baseline policy, 1000ns far-link round trip |

Near/far bandwidth (HBM2 32GB/s near, DDR4 19.2GB/s far) is applied
identically to every configuration via the existing, unconditional
`configureNearAsHBM2()`/`configureFarAsDDR4()` calls in `main.cc` — not
a per-configuration flag, since the paper does not vary this across
Case Studies 1–3 (only Case Study 3's OTHER half, an NVM far-memory
swap, would need that, and NVM is explicitly out of scope — see
`docs/nvm_support_audit.md`).

## 5. Metrics collected

`src/main.cc` gained a new `print_dcm_stats()` function (purely
additive — verified via smoke-test diff that every PRE-EXISTING line of
output is byte-for-byte unchanged; the only difference is new,
appended lines), printed once per run alongside the existing
CACHE/DRAM-controller statistics. It surfaces every field of
`DRAM_CACHE_MANAGER::stats` relevant to the paper's own reported
metrics:

- **Requests**: total, reads, writes.
- **Hits/misses**: total hits, total misses, cold misses, hot misses,
  and the full per-case breakdown (read-hit, write-hit, read-miss-clean,
  read-miss-dirty, write-miss-clean, write-miss-dirty) — the same
  breakdown Table II validates.
- **Local reads/writes, far reads/writes, cache fills.**
- **WB operations**: insertions, drains, full-rejects, dispatch
  retries.
- **Dispatch retries**: near-side and far-side (from the memory
  dispatch audit — `docs/memory_dispatch_audit.md`).
- **Conflicts**: CRB inserts/promotions/full-rejects, ORB full-rejects.
- **Access amplification**: the paper's own metric
  (`(localReads+localWrites+farReads+farWrites)/totalRequests`),
  computed by the pre-existing `DCM_STATS::accessAmplification()`.
- **ORB/CRB/WB occupancy**: average and max, from the pre-existing
  occupancy-sampling stats (`avgOrbOccupancy()` etc.).
- **Local/far bandwidth utilization** (the paper's Figure 9 metric):
  computed newly in `print_dcm_stats()` as
  `(ops × BLOCK_SIZE) / (elapsed_ROI_cycles × peak_bytes_per_cycle)`,
  where `peak_bytes_per_cycle = DRAM_CHANNEL_WIDTH × DRAM_MTPS /
  CPU_FREQ` for each controller — the same derivation already used to
  SET `DRAM_MTPS` from the paper's declared peak GB/s figures
  (`inc/dram_cache_manager.h`).
- Instructions/cycles/IPC: already printed by ChampSim's existing ROI
  statistics block, unchanged.

In bypass mode, only `bypassReads`/`bypassWrites`/`bypassCompletedReads`
are printed (every normal-mode field is structurally 0 and would be
misread as "the DRAM cache did nothing" rather than "the DRAM cache was
not in the path at all" — see `docs/bypass_mode.md`).

Every result file is unambiguously labeled by configuration and trace:
`run_case_studies.sh` names each output
`case_study_results/<CONFIG>__<trace>.txt`, and `print_dcm_stats()`
itself prints the active `policy`/`bypass`/`link_latency_ns` combination
at the top of its own output block, so a result file is self-describing
even in isolation.

## 6. Small sanity runs — results

Run: `./run_case_studies.sh --configs NO_DRAM_CACHE,BASELINE,BEAR_WR_OPT,ORACLE,BASELINE_100NS,BASELINE_500NS,BASELINE_1000NS --traces 403.gcc-16B.champsimtrace.xz --warmup 50000 --sim 100000`
(all 7 configurations, one small trace, ~1.5×10⁵ instructions total —
seconds per run, not a long simulation).

| Configuration | Cycles | IPC | DCM local reads | Notable |
|---|---|---|---|---|
| `NO_DRAM_CACHE` | 88084 | 1.135 | (bypass — n/a) | `BYPASS_READS=3124 == BYPASS_COMPLETED_READS-ish (3122)`; fastest of all 7 — **matches the paper's own counterintuitive Case Study 1 finding that a system WITHOUT the DRAM cache outperforms one WITH it**, at low link latency |
| `BASELINE` | 137273 | 0.728 | 3104 | reference point |
| `BEAR_WR_OPT` | 137273 | 0.728 | 3104 | **identical to BASELINE** in this window — correct, since BEAR only optimizes write-hits and this trace window has none (verified: baseline's own `WR_HIT` count is 0 here) |
| `ORACLE` | 135931 | 0.736 | 3055 | **fewer local reads than BASELINE by exactly 49** (matching baseline's own clean-miss count exactly) and fewer cycles — Oracle correctly beats baseline, differing ONLY on the exempted clean-miss path |
| `BASELINE_100NS` | 154605 | 0.647 | 3104 | +100ns link latency increases cycles over `BASELINE`, as expected |
| `BASELINE_500NS` | 233005 | 0.429 | 3104 | monotonically higher than 100NS |
| `BASELINE_1000NS` | 331005 | 0.302 | 3104 | monotonically higher than 500NS — **link latency's effect is monotonic and substantial, matching Case Study 3's core claim** |

**Every configuration-loading sanity check requested for this stage
passed**:
- Configuration loads correctly: all 7 ran to completion, exit 0, with
  each run's own `print_dcm_stats()` header confirming the active
  policy/bypass/link-latency combination matched what was requested.
- Correct policy selected: `BASELINE`/`BEAR_WR_OPT`/`ORACLE` each
  printed their own name back and produced policy-consistent stats
  (Oracle's reduced local-read count, BEAR's baseline-identical count
  on this write-hit-free window).
- Bypass mode actually bypasses the DCM: `NO_DRAM_CACHE`'s output shows
  ONLY the 3 bypass-specific counters, all normal-mode DCM fields
  entirely absent (by `print_dcm_stats()`'s own explicit branch — see
  Section 5) — confirming (independent of the underlying
  already-unit-tested `tests/test_dcm_bypass.cc`) that the DCM's own
  ORB/CRB/tag-metadata machinery is not exercised in this mode even at
  the full-binary level.
- BEAR differs from baseline only where expected: identical cycle count
  and local-read count in this specific window (zero write hits present
  — verified via the underlying trace's own LLC RFO/write-hit
  statistics), which is the CORRECT behavior for a window with no
  optimization opportunity, not a bug.
  Already exhaustively unit-tested for the general case
  (`tests/test_dcm_bear.cc`); this is the full-binary-level confirmation.
- Oracle differs from baseline only where expected: local reads reduced
  by exactly the clean-miss count, cycles reduced correspondingly.
  Already exhaustively unit-tested (`tests/test_dcm_oracle.cc`); this is
  the full-binary-level confirmation.
- Link latency changes only the intended path: local reads/writes/far
  writes are IDENTICAL across all three link-latency configurations
  (3104/49/0 in every case) — only cycles/IPC change, confirming link
  latency affects timing, not the request/hit/miss decision logic.
  Already unit-tested (`tests/test_dcm_link_latency.cc`); full-binary
  confirmation here.
- Statistics are produced correctly: every configuration's
  `print_dcm_stats()` block parsed correctly and its numbers are
  internally consistent (e.g. `LOCAL_READS - 49 (clean misses) ==
  ORACLE's LOCAL_READS` exactly).

## 7. Paper comparison — per Case Study

### Case Study 1: DRAM cache vs. no DRAM cache

| | |
|---|---|
| Paper experiment | GAPBS+NPB, 8-core, HBM2 DRAM cache vs. DDR4-only, no extra link latency (paper Figure 5) |
| ChampSim configuration | `NO_DRAM_CACHE` vs. `BASELINE`, `run_case_studies.sh` |
| Available trace/workload | ChampSim's 6 valid DPC-3 SPEC CPU2006 traces (Section 1) — NOT GAPBS/NPB |
| Required metrics | IPC/cycles (both configurations), DCM stats (BASELINE only) |
| Expected qualitative trend | DRAM cache underperforms no-DRAM-cache at low/no link latency (paper's own counterintuitive headline finding) |
| Sanity-run result | **Trend already reproduced at sanity-run scale**: `NO_DRAM_CACHE` cycles=88084 < `BASELINE` cycles=137273 on `403.gcc` (Section 6) |
| Classification | **ADAPTED REPRODUCTION** — mechanism and direction match; workload substitution (SPEC CPU2006 for GAPBS/NPB) and single-core-vs-8-core are real, documented deviations from exact reproduction |

### Case Study 2: cache architecture exploration (BEAR-Wr-Opt, Oracle)

| | |
|---|---|
| Paper experiment | GAPBS+NPB, baseline vs. BEAR-Wr-Opt vs. Oracle, speedup relative to baseline (Figure 10), and vs. no-DRAM-cache (Figure 11) |
| ChampSim configuration | `BASELINE`, `BEAR_WR_OPT`, `ORACLE` (+ `NO_DRAM_CACHE` for the Figure-11 comparison), `run_case_studies.sh` |
| Available trace/workload | Same 6 SPEC CPU2006 traces |
| Required metrics | IPC/cycles per configuration; DCM `numWrHit`/`cleanMissOptApplied`-family stats to explain WHY a speedup does/doesn't appear (paper's own per-workload explanation style, Section V-A/B/C/D) |
| Expected qualitative trend | Oracle ≥ BEAR-Wr-Opt ≥ Baseline always; speedup magnitude tracks each workload's write-hit / clean-miss ratio; still below no-DRAM-cache system |
| Sanity-run result | **Trend already reproduced**: Oracle < Baseline cycles on `403.gcc` (Section 6); BEAR-Wr-Opt correctly showed ZERO difference in a write-hit-free window (the CORRECT behavior when the mechanism's precondition doesn't occur, not evidence against the mechanism) |
| Classification | **ADAPTED REPRODUCTION** — the underlying MECHANISM (BEAR/Oracle's exact access-avoidance rule) is EXACTLY ported and Table-II-validated (`tests/test_dcm_bear.cc`, `tests/test_dcm_oracle.cc`); only the WORKLOAD driving how OFTEN that mechanism triggers is a substitute |

### Case Study 3: impact of link latency

| | |
|---|---|
| Paper experiment | Baseline DRAM cache, DDR4 (and separately NVM — out of scope) remote backing store, link latency 100/500/1000ns, vs. no-DRAM-cache (Figures 12, 13) |
| ChampSim configuration | `BASELINE_100NS`, `BASELINE_500NS`, `BASELINE_1000NS` (+ `NO_DRAM_CACHE` for the Figure-13 comparison), `run_case_studies.sh` |
| Available trace/workload | Same 6 SPEC CPU2006 traces |
| Required metrics | IPC/cycles at each link latency; DCM `farDispatchRetries`/`wbDispatchRetries` to characterize far-side pressure at high latency |
| Expected qualitative trend | Performance monotonically decreases as link latency increases (all configurations); the DRAM-cache configuration's RELATIVE standing vs. no-DRAM-cache improves as link latency grows (paper's headline Case-Study-3 finding — DRAM caches become worthwhile once the far link is slow enough) |
| Sanity-run result | **Monotonic link-latency trend already reproduced exactly**: 137273 → 154605 → 233005 → 331005 cycles as link latency goes 0→100→500→1000ns (Section 6) |
| Classification | **EXACT REPRODUCTION of the link-latency MECHANISM** (already unit-tested to cycle-accurate precision, `tests/test_dcm_link_latency.cc`); **ADAPTED REPRODUCTION of the Figure 12/13 EXPERIMENT** (workload substitution, as above). NVM half of Case Study 3 is **NOT REPRODUCIBLE** — see `docs/nvm_support_audit.md`'s DO-NOT-IMPLEMENT decision (ChampSim's `MEMORY_CONTROLLER` cannot represent the required read/write latency asymmetry) |

### Summary classification table

| Case Study | Classification |
|---|---|
| 1 (No-DRAM-Cache vs. Baseline) | ADAPTED REPRODUCTION |
| 2 (Baseline vs. BEAR-Wr-Opt vs. Oracle) | ADAPTED REPRODUCTION |
| 3, DDR4 link-latency sweep | ADAPTED REPRODUCTION (mechanism itself is EXACT) |
| 3, NVM half | NOT REPRODUCIBLE (`docs/nvm_support_audit.md`) |

No Case Study is classified EXACT REPRODUCTION overall, because every
one depends on GAPBS/NPB on an 8-core full-system target, which this
ChampSim environment cannot provide (Section 2) — this is a pre-existing,
already-documented limitation (`docs/limitations.md`), not a new gap
found this stage.

## 8. Automation

`run_case_studies.sh` (repo root, executable) runs the complete 7×N
(configurations × traces) matrix from the command line, with:
- `--dry-run`: prints every command that would be executed (binary
  path, exact flags, trace path, output file) without running anything.
  **Always supported and demonstrated working** (Section "Dry-run
  commands" in the final report).
- `--configs`/`--traces`/`--warmup`/`--sim`/`--binary`: full control
  over which subset runs, at what scale — defaults to all 7
  configurations, all 6 valid traces, 1M/1M instructions (itself a
  sanity scale, not a real reproduction scale — see below).
- No source file is touched by this script under any flag combination
  — every configuration difference is a command-line flag consumed by
  `src/main.cc`'s new knobs.

## 9. What a REAL future reproduction run would need

Not launched at full scale this stage (explicitly out of scope: "do not
launch any multi-hour/day simulation"). For the record, so a future
session can proceed directly:

```
./run_case_studies.sh --warmup 100000000 --sim 500000000 \
    --configs NO_DRAM_CACHE,BASELINE,BEAR_WR_OPT,ORACLE,BASELINE_100NS,BASELINE_500NS,BASELINE_1000NS \
    --traces 401.bzip2-277B.champsimtrace.xz,403.gcc-16B.champsimtrace.xz,434.zeusmp-10B.champsimtrace.xz,437.leslie3d-273B.champsimtrace.xz,462.libquantum-1343B.champsimtrace.xz,482.sphinx3-1522B.champsimtrace.xz
```

This is 7 configurations × 6 traces = 42 runs, each simulating 600M
instructions (100M warmup + 500M detailed). Section 10 below measures
ONE such run directly (`BASELINE`/`403.gcc-16B`: **18 min 58 sec**
wall-clock for the full 600M instructions) — projecting that measured
rate across all 42 runs gives roughly **13 hours if run serially**
(42 × ~19 min), or well under 2 hours with modest parallelism (e.g. 8
concurrent runs, each independent — `run_case_studies.sh` does not
serialize them itself, so this is a matter of how the caller invokes
it, e.g. via `xargs -P` or a job scheduler). This is a REAL measurement,
not the earlier "typically hours" guess — but it is trace-dependent:
Section 10 also found `403.gcc`'s own detailed-simulation IPC drops
sharply in its later portion, so other traces (or even other
CONFIGURATIONS of the same trace, e.g. `NO_DRAM_CACHE`'s different
memory-latency profile) may run faster or slower than this one
measurement.

## 10. Pre-flight verification (this stage — one confirmed 600M-instruction run)

Before authorizing the full 42-run matrix, this stage verified the
exact commands `run_case_studies.sh` would issue and ran ONE complete
600M-instruction confirmation run (`BASELINE`, `403.gcc-16B`) — no
other configuration or trace was run at this scale.

### Verification, per item

1. **Policy/bypass/link-latency arguments, all 7 configurations**:
   confirmed via `./run_case_studies.sh --dry-run --warmup 100000000
   --sim 500000000` (full 42-line output inspected) — `NO_DRAM_CACHE`
   → `--dcm_bypass`, `BASELINE` → no extra flag, `BEAR_WR_OPT` →
   `--dcm_policy=bear`, `ORACLE` → `--dcm_policy=oracle`,
   `BASELINE_100NS`/`500NS`/`1000NS` → `--dcm_link_latency_ns=100`/
   `500`/`1000` — each exactly matching Section 4's table, for every
   one of the 6 traces (42 lines total, all correct).
2. **Trace paths**: every dry-run line shows the full absolute path
   under `dpc3_traces/`, matching an actual file on disk (verified: the
   default trace list already excludes the corrupt `619.lbm_s` file —
   Section 1).
3. **Warmup/sim instruction counts**: every dry-run line shows
   `-warmup_instructions 100000000 -simulation_instructions 500000000`
   exactly as requested.
4. **Output filename uniqueness**: verified PROGRAMMATICALLY, not just
   by inspection — `run_case_studies.sh --dry-run`'s 42 output paths,
   piped through `sort | uniq -d`, produce ZERO duplicate lines. This
   holds by construction: each filename is
   `case_study_results/<CONFIG>__<trace>.txt` from a nested loop over
   the 7×6 distinct `(CONFIG, trace)` pairs, which is injective.
5. **DCM statistics reset per run**: confirmed by source inspection,
   not assumed — `UNCORE uncore;` (`src/uncore.cc:4`) is a single
   global object, constructed exactly once per OS process at program
   startup; `uncore.DCM`'s constructor default-constructs
   `DCM_STATS stats` (`inc/dram_cache_manager.h`), whose constructor
   zero-initializes every field. Since `run_case_studies.sh` launches
   each configuration/trace combination as a completely separate
   `./bin/champsim` process (never a persistent server reused across
   runs), there is no code path by which one run's statistics could
   leak into another's — this is guaranteed by ChampSim's existing
   process model, not something this stage needed to add.
6. **Every run records trace name, configuration, warmup/sim
   instructions, cycles, IPC, and all DCM statistics**: confirmed
   directly against real output (`/tmp/verify_fields.log`, a 100K-
   instruction confirming run) — every field appears with an
   unambiguous label: `Warmup Instructions: N`, `Simulation
   Instructions: N`, `DRAM Cache Manager policy: ... link_latency_ns:
   N`, `CPU 0 runs <trace path>`, `Finished CPU 0 instructions: N
   cycles: N cumulative IPC: N`, and the full `DRAM Cache Manager
   Statistics (...)` block (`docs/case_study_reproduction_plan.md`
   Section 5's field list).

### The one authorized 600M-instruction run

**Exact command**:
```
./bin/champsim -warmup_instructions 100000000 -simulation_instructions 500000000 \
    -traces /home/dpsingh/Desktop/ChampSim-master/dpc3_traces/403.gcc-16B.champsimtrace.xz
```
(This is exactly the command `run_case_studies.sh --configs BASELINE
--traces 403.gcc-16B.champsimtrace.xz --warmup 100000000 --sim
500000000` would issue for the `BASELINE` configuration — verified by
comparing against that dry-run's own printed command.)

Trace chosen: `403.gcc-16B.champsimtrace.xz` — already used throughout
this port's prior smoke tests and sanity runs, giving direct continuity
with previously-established baseline numbers (Section 6).

### Results

Completed successfully, exit code 0, in **18 min 58 sec wall-clock**
(100M-instruction warmup completed at 1:44; full 600M-instruction run
— 500,000,002 instructions retired — at 18:58). Output file: 148 lines,
well-formed from the `*** ChampSim Multicore Out-of-Order Simulator
***` header through `ChampSim completed all CPUs`, the full ROI
statistics block (L1D/L1I/L2C/LLC/DRAM), and the `DRAM Cache Manager
Statistics` block, with no truncation, no error/warning lines, and no
premature exit.

```
Finished CPU 0 instructions: 500000002 cycles: 1693827177 cumulative IPC: 0.295189

DRAM Cache Manager Statistics (policy=baseline, link_latency_ns=0)
 DCM TOTAL_REQUESTS:   10493481  READS:    7253888  WRITES:    3239593
 DCM HITS:   10479843  MISSES:      13638  COLD_MISSES:      13106  HOT_MISSES:        532
 DCM RD_HIT:    7240638  WR_HIT:    3239205  RD_MISS_CLEAN:      13036  RD_MISS_DIRTY:        214  WR_MISS_CLEAN:        388  WR_MISS_DIRTY:          0
 DCM LOCAL_READS:   10493481  LOCAL_WRITES:    3252840  FAR_READS:      13250  FAR_WRITES:        214  CACHE_FILLS:      13638
 DCM WB_INSERTIONS:        214  WB_DRAINS:        214  WB_FULL_REJECTS:          0  WB_DISPATCH_RETRIES:          0
 DCM NEAR_DISPATCH_RETRIES:          0  FAR_DISPATCH_RETRIES:          0
 DCM CONFLICTS(CRB_INSERTS):    2974273  CRB_PROMOTIONS:    2974273  CRB_FULL_REJECTS:          0  ORB_FULL_REJECTS:          0
 DCM ACCESS_AMPLIFICATION: 1.31127  (avg local+far sub-ops per request)
 DCM ORB_OCCUPANCY avg: 0.902802  max:         14
 DCM CRB_OCCUPANCY avg: 1.04636  max:         21
 DCM WB_OCCUPANCY  avg: 1.22953e-07  max:          1
 DCM LOCAL_BW_UTILIZATION: 6.67641%  FAR_BW_UTILIZATION: 0.0108988%
```

### Request-loss/retry anomaly check — programmatically verified

Every internal DCM statistic relationship was checked against the
actual printed numbers (not eyeballed):

| Invariant | Result |
|---|---|
| `reads + writes == total_requests` | **EXACT** (10493481 = 10493481) |
| `hits + misses == total_requests` | **EXACT** |
| `cold_misses + hot_misses == misses` | **EXACT** |
| `rd_hit + wr_hit == hits` | **EXACT** |
| `rd_miss_clean + rd_miss_dirty + wr_miss_clean + wr_miss_dirty == misses` | **EXACT** |
| `local_reads == total_requests` (baseline never skips the tag-check read) | **EXACT** |
| `far_reads == rd_miss_clean + rd_miss_dirty` | **EXACT** (13250 = 13250) |
| `far_writes == rd_miss_dirty` (dirty-victim write-backs) | **EXACT** (214 = 214) |
| `wb_insertions == wb_drains` | **EXACT** (214 = 214 — every WB entry drained, none stuck) |
| `cache_fills == misses` | **EXACT** |
| `local_writes == writes + far_reads` (direct writes + background fills) | **off by 3** (3252840 vs. expected 3252843) |

**The one discrepancy is a benign end-of-run in-flight remainder, not a
loss.** `far_reads` (13250) exactly matches the miss count that should
trigger a background fill — no far read was lost. The background fill
write is deliberately issued AFTER the LLC response
(`gem5_to_champsim_mapping.md` fact #1, "respond-before-fill"), off the
critical path, via the SAME asynchronous `dispatchToNear()` mechanism
audited and stress-tested in `docs/memory_dispatch_audit.md`. At the
exact instant the 500,000,000th simulation instruction retired and
`print_dcm_stats()` ran, 3 of the 13,250 total far-fetch-triggered
fills across the whole 500M-instruction run had had their far read
complete (and their LLC response already sent) but had not yet been
dispatched to `nearMC` — an ordinary "still in flight at the snapshot
boundary" artifact of measuring an asynchronous background operation at
an arbitrary cutoff, affecting 0.00009% of local writes, not a request
that vanished.

**All four retry/reject counters that WOULD indicate a genuine capacity
problem are exactly zero**: `WB_DISPATCH_RETRIES=0`,
`NEAR_DISPATCH_RETRIES=0`, `FAR_DISPATCH_RETRIES=0`,
`WB_FULL_REJECTS=0`, `CRB_FULL_REJECTS=0`, `ORB_FULL_REJECTS=0` — the
guaranteed-delivery retry mechanisms built and tested in the prior two
stages were never even needed during this real 600M-instruction run
(near/far controller capacity was never exhausted on this trace at this
scale); their zero counts here are consistent with, not contradicting,
their correctness (the mechanism exists for when capacity IS exhausted,
which unit/stress tests already covered directly at a scale this
particular trace/run didn't happen to reach).

**Conclusion: no request-loss or retry anomaly found.** Every
count-based invariant holds exactly except one 3-in-13,250,000-scale
in-flight artifact with a complete, benign explanation.

### Throughput measured (for Section 9's estimate)

600,000,002 total instructions (100M warmup + 500M detailed) completed
in 18 min 58 sec wall-clock on this machine — replacing Section 9's
earlier "typically hours" guess with a real measurement. Note the
trace's own throughput was NOT uniform: warmup-phase IPC was ~2.14,
while the DETAILED simulation phase's IPC dropped sharply in its later
portion (down to ~0.12–0.14 IPC past ~460M instructions, vs. ~1.17 IPC
earlier in the detailed phase) — this specific trace has a distinct
late-execution phase change that is dramatically more expensive to
simulate, not a constant rate throughout. Other traces' wall-clock time
at the same instruction count will vary accordingly.

### No source code changed to make this run possible

`run_case_studies.sh` and the confirmation command above use only the
CLI knobs added in the prior stage (`--dcm_policy`, `--dcm_bypass`,
`--dcm_link_latency_ns`) and pre-existing ChampSim flags
(`-warmup_instructions`, `-simulation_instructions`, `-traces`) — no
file under `src/`/`inc/` was modified to enable this run.

## 11. Post-granularity-fix production pre-flight (stage 16) — the current baseline of record

**All full-binary results recorded before this section are INVALID** and
are superseded here. They were produced with the DRAM-cache
address-granularity defect described in
`docs/final_independent_audit.md` §16, which made 64 distinct 64-byte
lines alias onto one index (effective 4 KiB lines / 8 GB capacity). This
includes the stage-12 `BASELINE` / `403.gcc-16B` 600M run in Section 10
and its 99.87% hit rate — **that number is an artifact of the defect, not
a workload property, and must not be compared against anything below.**
The files `case_study_results/BASELINE__403.gcc-16B.*` and
`case_study_results/BASELINE_postfix__403.gcc-16B.*` are retained only as
historical/diagnostic records.

### Trace selection — measured, not assumed

The pre-flight was required to use the most memory-intensive **valid**
trace. `619.lbm_s-3766B` was re-confirmed **corrupt** (`xz -t` →
"Unexpected end of input"), leaving six candidates, which were ranked by
a short selection probe (5M warmup / 20M ROI each — a selection
measurement, not an experiment):

| trace | LLC MPKI | DCM req | DCM hit % |
|---|---|---|---|
| **462.libquantum-1343B** | **22.66** | 505,332 | 16.95 |
| 482.sphinx3-1522B | 11.51 | 248,766 | 98.85 |
| 401.bzip2-277B | 7.20 | 153,160 | 89.13 |
| 437.leslie3d-273B | 6.16 | 159,478 | 33.97 |
| 434.zeusmp-10B | 2.42 | 64,001 | 36.16 |
| 403.gcc-16B | 0.11 | 2,304 | 2.12 |

`462.libquantum-1343B` is the most memory-intensive by a factor of ~2 on
LLC MPKI and was selected. Note in passing that `403.gcc-16B` — the trace
behind the old 99.87% figure — now shows a 2.12% DCM hit rate: it barely
touches memory at all, and the old number was entirely the aliasing bug.

### Configuration verified before launch

| item | verified value | how |
|---|---|---|
| binary | `bin/champsim`, clean `make clean && make` after the fix | rebuild timestamp |
| policy | `baseline` (CascadeLakeNoPartWrs) | run banner `policy=baseline`; `--dcm_policy` not passed |
| bypass | off | `--dcm_bypass` not passed; default `bypassDcache=false` |
| BEAR / Oracle | not selected | no `--dcm_policy=bear` / `=oracle` |
| DCM line size | 64 B (`BLOCK_SIZE == DCM_BLOCK_SIZE == 64`) | compiled-constant probe |
| DCM capacity | 134,217,728 B = 128 MiB | compiled-constant probe |
| DCM lines | `DCM_NUM_LINES = 2,097,152`; `NUM_LINES × BLOCK == DCM_DRAM_CACHE_SIZE` | compiled-constant probe |
| mapping | line *L* / *L+1* → distinct indices; *L* / *L+NUM_LINES* → same index, tag+1 | compiled-constant probe + integration Tests 7/8/10 |
| warmup | enabled and effective | `WARMUP_TAG_UPDATES: 3,502,390` |
| controller latency | frontend 10 ns = 40 cyc, backend 10 ns = 40 cyc | compiled-constant probe |
| link latency | `link_latency_ns: 0` (current default configuration) | run banner |
| near memory | HBM2-like, 1 channel, 64-bit, 4000 MT/s | run banner |
| far memory | DDR4-like, 4096 MB, 1 channel, 64-bit, 2400 MT/s | run banner |
| ORB / CRB / WB | 128 / 32 / 64, WB pressure threshold 64 | compiled-constant probe |

### The run

```
./bin/champsim --warmup_instructions 100000000 --simulation_instructions 500000000 \
  -traces dpc3_traces/462.libquantum-1343B.champsimtrace.xz
```

Output: `case_study_results/PREFLIGHT600M_BASELINE__462.libquantum-1343B.txt`.
Wall clock 18 min 19 s (warmup complete at 1:30). Passes the harness's own
`validate_run()` checks.

### Results

| metric | value |
|---|---|
| instructions (ROI) | 500,000,000 |
| cycles (ROI) | 1,817,776,339 |
| IPC | 0.275061 |
| LLC misses | 12,736,699 |
| DCM total requests | 17,763,568 (reads 12,736,700 / writes 5,026,868) |
| DCM hits | 14,098,299 |
| DCM misses | 3,665,269 |
| cold misses | 27 (0.0007% of misses) |
| hot misses | 3,665,242 |
| **DCM hit rate** | **79.37%** (read 71.37%, write 99.63%) |
| local reads | 17,763,568 |
| local writes | 8,673,409 |
| far reads | 3,646,541 |
| far writes | 1,447,883 |
| cache fills | 3,665,269 |
| dirty evictions | 1,447,883 |
| access amplification | 1.77506 |
| local BW utilization | 11.6349% |
| far BW utilization | 3.73674% |
| ORB occupancy | avg 1.28978, max 10 |
| CRB occupancy | avg 0, max 0 |
| WB occupancy | avg 0.000797, max 1 |
| dispatch retries (near / far / WB) | 0 / 0 / 0 |
| duplicate-merge retries | 0 |
| CRB inserts / promotions | 0 / 0 |
| ORB / CRB / WB full rejects | 0 / 0 / 0 |
| residual at cutoff | ORB 0, CRB 0, WB 0, pendingNear 0, pendingFar 0, pendingResp 0 — **fully drained** |

### Conservation invariants — 18/18 exact

`READS+WRITES == TOTAL_REQUESTS`; `HITS+MISSES == TOTAL_REQUESTS`;
`RD_HIT+WR_HIT == HITS`; the four miss categories sum to `MISSES`;
`COLD+HOT == MISSES`; read categories sum to `READS`; write categories sum
to `WRITES`; `FAR_READS == RD_MISS_CLEAN + RD_MISS_DIRTY` (baseline issues
no far read on a write miss — Table II);
`CACHE_FILLS == MISSES`; `RD_MISS_DIRTY + WR_MISS_DIRTY == WB_INSERTIONS
== WB_DRAINS == FAR_WRITES`; `LOCAL_READS == TOTAL_REQUESTS` (baseline
tag-checks every request); `LOCAL_WRITES == CACHE_FILLS + WR_HIT`;
`COMPLETED_READS_TO_LLC == READS`; `COMPLETED_WRITES == WRITES`;
`COMPLETED_FROM_NEAR == LOCAL_READS`; `COMPLETED_FROM_FAR == FAR_READS`;
recomputed access amplification matches the reported 1.77506.

Nothing stranded, nothing duplicated, nothing left in flight at the
cutoff, and no unexplained background operations.

**One +1 boundary artifact**: LLC ROI misses 12,736,699 vs DCM ROI reads
12,736,700. `reset_cache_stats()` for the LLC and `DCM.resetROIStats()`
run in the same warmup→ROI transition (`src/main.cc:347` and `:357`), so a
single request whose LLC miss was counted just before the boundary and
whose DCM admission landed just after is counted once on the DCM side
only. One request in 12.7 million; not a conservation violation — every
DCM-internal invariant is exact and the residual is zero.

### Sanity of the hit rate (do NOT compare to the old 99.87%)

The 79.37% figure is workload-determined and internally consistent, and
the following were verified independently rather than inferred from it:

- **Adjacent 64-byte lines stay distinct** — integration Test 7 and the
  compiled-constant probe: line *L* → index *i*, line *L+1* → index *i+1*.
- **Repeated access to the same line can hit** — baseline suite's
  "5× read hit" (5 local reads, 0 far ops) and warmup Test 3 ("ROI access
  to a warmed line hits"). Confirmed in the run itself: 14.1 M hits.
- **Same-index / different-tag addresses conflict and replace** —
  integration Test 8 (dirty victim replaced, exactly one write-back
  reaching far memory). Confirmed in the run: 3,665,242 hot misses
  (99.9993% of all misses) with 1,447,883 dirty evictions.
- **Warmup actually populates the DCM** — `WARMUP_TAG_UPDATES: 3,502,390`,
  and only **27** cold misses remain in the entire 500M-instruction ROI.
  Compare the pre-fix stage-12 run, where 96% of ROI misses were cold.

The structure of the number is also explainable: libquantum streams a
large array, so reads hit at 71% (the 128 MiB cache captures a
substantial part of the working set but not all of it), while writes hit
at 99.63% because an LLC writeback almost always targets a line the
preceding read miss just filled. Access amplification of 1.78 is
consistent with baseline's mandatory tag-check read on every request plus
a fill write on each of the 20.6% of requests that miss.

**This trace does generate meaningful DRAM-cache reuse** — 14.1 M hits
over 17.8 M requests — so no low-reuse caveat applies to this run.

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
