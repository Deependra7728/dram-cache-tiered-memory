# Hardware-Managed DRAM Cache for Tiered & Disaggregated Memory

A cycle-level DRAM cache controller built on [ChampSim](https://github.com/ChampSim/ChampSim), modeling a **128 MB HBM2 cache in front of 4 GB DDR4** main memory — the "2LM" tiered-memory mode shipped in Intel Cascade Lake and Sapphire Rapids, and the CXL-attached remote memory used in disaggregated systems.

The project has two parts: a **controller implementation** and a **performance study** that uses it.

---

## Part 1 — The Controller

`inc/dram_cache_manager.h` · `src/dram_cache_manager.cc`

A memory-side cache manager that sits between the last-level cache and two independent memory controllers (near = HBM2, far = DDR4). It is outside the coherence domain and is a drop-in replacement for ChampSim's memory controller.

| Component | Detail |
|---|---|
| Cache geometry | 128 MB, 64 B lines, 2,097,152 lines, direct-mapped |
| Request pipeline | 10-state machine (`DCM_REQ_STATE`), 4 action states each paired with a wait state |
| Queues | ORB (outstanding, 128) · CRB (conflict, 32) · WB (write-back, 64) |
| Backpressure | All three queues reject on occupancy and count rejections |
| Memory timing | Near/far configured independently (HBM2 4000 MTPS, DDR4 2400 MTPS) |
| Link latency | Configurable far-memory delay for CXL-style disaggregation |
| Policies | `baseline` (Cascade Lake), `bear` (write-hit optimized), `oracle` (idealized) |

**How a request flows.** Tags live alongside data, so every access begins with a read of the near memory to check the tag. What follows depends on the outcome:

| Case | Memory operations |
|---|---|
| Read hit | 1 — near read |
| Write hit | 2 — near read (tag), near write |
| Read miss, clean victim | 3 — near read, far read, near write |
| Read miss, dirty victim | 4 — near read, far write (evict), far read, near write |

One CPU request becomes up to four memory operations. That amplification is what Part 2 measures.

**Policies.** `baseline` always performs the tag-check read. `bear` skips it on write hits, where the fetched data is overwritten anyway. `oracle` additionally skips it on clean misses, assuming zero-latency tag knowledge — an upper bound on what tag-check elimination can buy.

---

## Part 2 — The Performance Study

42 configurations: 7 designs × 6 SPEC CPU2006 workloads, each 1B warmup + 500M measured instructions.

**Does a DRAM cache help?** No — it costs ~20% throughput versus no cache at all, on every workload tested.

| Workload | Miss ratio | Amplification | vs. no cache |
|---|---|---|---|
| 401.bzip2 | 3.5% | 1.31× | 0.90× |
| 403.gcc | 17.6% | 1.81× | 0.89× |
| 434.zeusmp | 36.7% | 2.08× | 0.82× |
| 437.leslie3d | 18.1% | 1.68× | 0.75× |
| 462.libquantum | 23.4% | 1.73× | 0.68× |
| 482.sphinx3 | 2.6% | 1.13× | 0.76× |
| **geomean** | | **1.1–2.1×** | **0.79×** |

**Why?** Not bandwidth. The `oracle` policy removes ~20% of all memory traffic and recovers only **4% IPC** — the eliminated operations were never on the critical path. The tag-check read still serializes ahead of the fill and the far-memory access. Latency, not bandwidth, is the limiter.

| Policy | Traffic removed | IPC gain |
|---|---|---|
| `bear` | 13.6% | +0.8% |
| `oracle` | 19.6% | +3.7% |

**Disaggregation.** Adding link latency to far memory collapses throughput, and the loss tracks miss ratio — workloads that rarely reach far memory barely notice.

| Link latency | 100 ns | 500 ns | 1 µs |
|---|---|---|---|
| Throughput vs. 0 ns | 0.87× | 0.60× | **0.45×** |

At 1 µs the DRAM cache system runs at 0.36× a system with no DRAM cache at all.

---

## Running It

**Build** (bimodal branch predictor, no prefetchers, LRU, 1 core):

```bash
./build_champsim.sh bimodal no no no no lru 1
```

**Traces** are not in this repo (2 GB, licensed). Download the DPC-3 SPEC CPU2006 traces from [dpc3.compas.cs.stonybrook.edu](https://dpc3.compas.cs.stonybrook.edu/?SW_IS) into `dpc3_traces/`.

**Single run:**

```bash
./bin/champsim --warmup_instructions 1000000000 \
               --simulation_instructions 500000000 \
               --dcm_policy oracle \
               -traces dpc3_traces/403.gcc-16B.champsimtrace.xz
```

| Flag | Values | Purpose |
|---|---|---|
| `--dcm_policy` | `baseline` \| `bear` \| `oracle` | Caching policy |
| `--dcm_bypass` | (no argument) | Disable the cache — the no-DRAM-cache control |
| `--dcm_link_latency_ns` | `0` \| `100` \| `500` \| `1000` | Far-memory link latency |

**Full 42-run matrix:**

```bash
./run_case_studies.sh              # all 7 configs × 6 traces
./run_case_studies.sh --dry-run    # print commands without running
./run_case_studies.sh --configs BASELINE,ORACLE --traces 403.gcc-16B
```

Each run prints a `DCM` statistics block: hit/miss breakdown by type, per-interface operation counts, queue occupancy, access amplification, and a residual check confirming all queues drained.

**Tests** — 16 suites, all self-contained:

```bash
g++ -std=c++11 -Iinc -o /tmp/t tests/test_dcm_baseline.cc \
    src/dram_cache_manager.cc src/dram_controller.cc src/block.cc && /tmp/t
```

`test_dcm_llc_integration.cc` additionally needs `src/cache.cc`, `replacement/base_replacement.cc`, and the three no-op prefetchers.

---

## Repository Layout

```
inc/dram_cache_manager.h     Controller interface, state machine, buffers
src/dram_cache_manager.cc    Implementation
tests/test_dcm_*.cc          16 test suites
docs/                        Design notes, validation, known limitations
run_case_studies.sh          42-configuration experiment harness
```

`docs/validation.md` records the correctness argument — per-case operation counts asserted exactly for all three policies. `docs/limitations.md` documents what this model does *not* capture, including the trace-driven and single-core constraints inherited from ChampSim.

---

## Notes and Limitations

- **Trace-driven, single-core.** ChampSim replays traces rather than booting an OS, so OS effects and multi-threaded interference are out of scope.
- **Warmup is instruction-based** (1B instructions), not checkpoint-based. Cold misses stay under 17% of measured misses on 5 of 6 workloads; `401.bzip2` is higher because its working set largely fits in the cache, leaving mostly compulsory misses.
- **`619.lbm_s` is excluded** — the trace file is truncated and fails `xz -t`.

Built on [ChampSim](https://github.com/ChampSim/ChampSim) (see `LICENSE`).
