# Feature Coverage

Tracks paper functionality against what exists in this ChampSim port.
Updated after every implementation stage. "Skeleton" = data structure /
wiring exists; "Wired" = behavior implemented and exercised by a test;
"Not started" = nothing yet.

## Stage 6: Oracle policy — DONE, verified

| Item | Status | Notes |
|---|---|---|
| Oracle selectable policy | **Wired** | `DCM_POLICY_ORACLE`, set via `dcm.policy` |
| Write-hit local-tag-check-read elimination | **Wired** | Same mechanism as BEAR-Wr-Opt, `chooseInitialState()` |
| Clean/cold-miss (read or write) local-tag-check-read elimination | **Wired** | Oracle's exemption beyond BEAR-Wr-Opt — implemented from the **paper** (Section V + Table II), using the OLD RESIDENT (victim) dirty bit. gem5's `enums::RambusHypo` (`policy_manager.cc:786-810`) does NOT implement this rule: its `isDirty` reads post-update metadata, so it deviates from its own Table II. Deliberate, documented divergence — see `inc/dram_cache_manager.h`'s `chooseInitialState()` comment |
| Read hit NOT exempted | **Verified** | Read hit still visits `DCM_LOC_MEM_READ` — the read fetches data, not just a tag; gem5 does the same |
| Dirty miss (read or write) NOT exempted | **Verified** | Still visits `DCM_LOC_MEM_READ` to source the victim's data for write-back; gem5 does the same |
| Baseline/BEAR-Wr-Opt unaffected when Oracle not selected | **Verified** | Full 45-block prior regression + full-binary trace runs byte-identical before/after this stage |
| Oracle write hit via CRB promotion | **Wired, verified** | `tests/test_dcm_oracle.cc` Test 13 |
| Policy-specific statistics | **Wired** | `oracleWriteHits`, `oracleCleanMisses`, `cleanMissOptOpportunities`, `cleanMissOptApplied`, `cleanMissOptNotApplicable` (plus reused `writeHitOpt*`/`localTagCheckReadsAvoided`) |
| Table II Oracle row | **Verified, 6/6 applicable cases** | `tests/test_dcm_oracle.cc`, see `validation.md` |
| Baseline vs. BEAR vs. Oracle comparison table | **Wired, verified** | `tests/test_dcm_oracle.cc` Test 17 — printed table matches Table II exactly for all 6 scenarios × 3 policies |
| gem5 code-level comparison | **Done, documented** | Full scenario-by-scenario table in `validation.md`; no behavioral discrepancy found |

All three policies from the paper (baseline/CascadeLakeNoPartWrs,
BEAR-Wr-Opt, Oracle/RambusHypo) are now implemented, independently
selectable, and independently verified.

## Stage 5: BEAR-Wr-Opt policy — DONE, verified

| Item | Status | Notes |
|---|---|---|
| BEAR-Wr-Opt selectable policy | **Wired** | `DCM_POLICY_BEAR_WR_OPT`, set via `dcm.policy` |
| Write-hit local-tag-check-read elimination | **Wired** | `chooseInitialState()`, ported from gem5 `setNextState`'s BEAR block (`policy_manager.cc:897-911`) |
| All non-write-hit cases unchanged from baseline | **Verified** | Reads (hit/miss) and write misses (clean/dirty victim) byte-for-byte identical — `tests/test_dcm_bear.cc` Tests 2,3,4,5,6,7 |
| Baseline unaffected when BEAR not selected | **Verified** | Full 31-block prior regression + full-binary trace runs produce byte-identical cycle counts before/after this stage |
| BEAR write hit via CRB promotion | **Wired, verified** | `tests/test_dcm_bear.cc` Test 11 — a request that becomes a hit only because of eager tag-store installation by the ORB entry it conflicted with is correctly detected and optimized |
| Policy-specific statistics | **Wired** | `writeHitOptOpportunities`, `writeHitOptApplied`, `writeHitOptNotApplicable`, `localTagCheckReadsAvoided` |
| Table II BEAR-Wr-Opt row | **Verified, 6/6 applicable cases** | `tests/test_dcm_bear.cc`, see `validation.md` |
| gem5 code-level comparison | **Done, documented** | Full scenario-by-scenario table in `validation.md`; no behavioral discrepancy found |

Oracle is now implemented (see Stage 6 above) — this note is retained
for history but is now stale as of this stage.

## Stage 1 (skeleton) + Stage 2 (timing + ORB/CRB) + Stage 3 (complete baseline) + Stage 4 (WB pressure, near/far config, link latency, stats)

| Item | Status | Notes |
|---|---|---|
| `DRAM_CACHE_MANAGER : public MEMORY` | Wired | `inc/dram_cache_manager.h`, `src/dram_cache_manager.cc` |
| LLC → manager wiring | Wired | `uncore.LLC.lower_level = &uncore.DCM` |
| Two independent `MEMORY_CONTROLLER` instances | Wired | `uncore.DRAM_CACHE_DEVICE` (near), `uncore.DRAM` (far) |
| Per-instance near/far DRAM timing | Wired | `MEMORY_CONTROLLER::set_timing()` |
| **Near/far configured with paper's real HBM2/DDR4 bandwidths** | **Wired (stage 4)** | `configureNearAsHBM2()`/`configureFarAsDDR4()` (32 GB/s / 19.2 GB/s exact), wired into `main.cc`. Latency (tRP/tRCD/tCAS) not differentiated — documented gap, `limitations.md` |
| Internal request representation | Wired | `DCM_ORB_ENTRY` (owns a `PACKET` copy + DRAM-cache metadata + victim bookkeeping) |
| Request IDs | Wired | `DCM_ORB_ENTRY::requestId`, monotonic, printed in debug log |
| ORB structure | Wired | `std::map<uint64_t, DCM_ORB_ENTRY*> ORB`, size-limited to 128 |
| CRB structure / conflict handling | Wired | `checkConflictInORB()`, CRB admission + backpressure, FIFO-per-index `promoteFromCRB()` |
| DRAM-cache tag lookup + direct-mapped index/tag calculation | Wired | `returnIndexDC()`/`returnTagDC()`, `classifyAndInstall()` |
| Tag comparison / valid / dirty state | Wired | `DCM_TAG_ENTRY` fully consulted and updated on every request |
| Victim identification | Wired | `DCM_ORB_ENTRY::victimWasValid/victimWasDirty/victimFarAddr` |
| Cache-line installation / replacement | Wired | Eager tag/metadata install in `classifyAndInstall()` |
| Dirty-victim handling + WB-buffer insertion + far write-back | Wired | `pushDirtyWriteBack()`, triggered at tag-check-read completion |
| **WB occupancy admission backpressure** | **Wired (stage 4)** | `DCM_WB_PRESSURE_THRESHOLD = DCM_ORB_MAX_SIZE/2 = 64` (gem5's actual mechanism); one-per-cycle drain via `drainWB()`; verified `tests/test_dcm_wb_pressure.cc` (5 blocks) |
| **Manager↔far-memory link latency (Case Study 3)** | **Wired (stage 4)** | `linkLatencyCycles`/`setLinkLatency()`, `dispatchToFar()`, `pendingFarDispatches`; applies to far-read AND far-write; local hits unaffected; verified `tests/test_dcm_link_latency.cc` (4 blocks) |
| Policy/state enum | Wired | `DCM_POLICY`, `DCM_REQ_STATE`; only baseline transition table wired |
| Configuration parameters | Wired (documented) | Full paper→gem5→ChampSim derivation table in `gem5_to_champsim_mapping.md` |
| **Statistics: full coverage per this stage's checklist** | **Wired (stage 4)** | See "Statistics coverage" table below |
| Optional debug logging | Wired | `debugPrint`; logs classification, CRB, WB insert/drain, far-dispatch hold/release |

## Statistics coverage (this stage's explicit checklist)

| Required stat | Field(s) | Verified by |
|---|---|---|
| Total requests | `totalRequests` | `test_dcm_stats.cc` |
| Reads | `readRequests` | `test_dcm_stats.cc` |
| Writes | `writeRequests` | `test_dcm_stats.cc` |
| Read hits | `numRdHit` | `test_dcm_stats.cc`, `test_dcm_baseline.cc` |
| Write hits | `numWrHit` | `test_dcm_stats.cc`, `test_dcm_baseline.cc` |
| Misses (total, clean, dirty) | `numTotMisses`, `numRdMissClean/Dirty`, `numWrMissClean/Dirty` | `test_dcm_stats.cc`, `test_dcm_baseline.cc` |
| Local reads | `localReads` | all baseline/Table-II tests |
| Local writes | `localWrites` | all baseline/Table-II tests |
| Far reads | `farReads` | all baseline/Table-II tests |
| Far writes | `farWrites` | all baseline/Table-II tests, `test_dcm_link_latency.cc` |
| Dirty evictions / WB operations | `numWrBacks`, `wbInsertions`, `wbDrains` | `test_dcm_baseline.cc` (E, H, CHAIN), `test_dcm_wb_pressure.cc` |
| Conflicts | `crbInserts` | `test_dcm_skeleton.cc` Test 5 |
| ORB occupancy/full events | `orbFullRejects`, `orbMaxOccupancy`, `orbOccupancySum`/`avgOrbOccupancy()` | `test_dcm_stats.cc` Test 3 |
| CRB occupancy/full events | `crbFullRejects`, `crbMaxOccupancy`, `avgCrbOccupancy()` | `test_dcm_skeleton.cc` Test 6 |
| WB occupancy/full events | `wbFullRejects`, `wbMaxOccupancy`, `avgWbOccupancy()` | `test_dcm_wb_pressure.cc` |
| Cache fills | `cacheFills` (== `numTotMisses` in baseline: insert-on-miss) | `test_dcm_stats.cc` |
| Access amplification | `DCM_STATS::accessAmplification()` (computed: total sub-ops / total requests) | `test_dcm_stats.cc` |

All counters confirmed per-instance (not global/shared) —
`test_dcm_stats.cc` Test 2 constructs two independent `DRAM_CACHE_MANAGER`
instances and verifies one's activity does not appear in the other's stats.

## Complete BASELINE request cases (A–H) — all wired and verified

| Case | Status | Test |
|---|---|---|
| A. READ HIT | Wired, verified | `test_dcm_baseline.cc` Case A |
| B. WRITE HIT | Wired, verified | Case B |
| C. READ MISS + INVALID/COLD | Wired, verified | Case C (+ request-flow log) |
| D. READ MISS + CLEAN VICTIM | Wired, verified | Case D |
| E. READ MISS + DIRTY VICTIM | Wired, verified | Case E (+ request-flow log) |
| F. WRITE MISS + INVALID/COLD | Wired, verified | Case F |
| G. WRITE MISS + CLEAN VICTIM | Wired, verified | Case G |
| H. WRITE MISS + DIRTY VICTIM | Wired, verified | Case H (+ request-flow log) |
| Repeated access to same line | Wired, verified | `test_dcm_baseline.cc` "REPEAT" |
| Replacement chain (N addrs, one index) | Wired, verified | `test_dcm_baseline.cc` "CHAIN" |
| Conflicting requests + real classification | Wired, verified | `test_dcm_skeleton.cc` Test 5 |
| ORB pressure/full | Wired, verified | `test_dcm_skeleton.cc` |
| CRB pressure/full | Wired, verified | `test_dcm_skeleton.cc` Test 6 |
| **WB pressure/full (below/at/above threshold, retry, interaction w/ CRB)** | **Wired, verified (stage 4)** | `test_dcm_wb_pressure.cc` (5 blocks) |
| Table II access-amplification oracle | Wired, verified 8/8 | `test_dcm_baseline.cc` cases A–H, see `validation.md` |
| **Near/far independent timing + real paper bandwidths** | **Wired, verified (stage 4)** | `test_dcm_near_far_config.cc` (3 blocks) |
| **Link latency (100/500/1000ns, far-read + far-write, hit isolation)** | **Wired, verified (stage 4)** | `test_dcm_link_latency.cc` (4 blocks) |
| **Controller frontend/backend latency (0/default/custom, single vs. double round-trip, additive w/ link latency, op-count invariance across all 3 policies)** | **Wired, verified (stage 7)** | `test_dcm_controller_latency.cc` (9 assertions) |
| **bypassDcache / No-DRAM-Cache comparison mode (read/write, multiple/mixed/repeated access, same-index addresses, no ORB/CRB/WB/metadata touched, far-link latency composes, controller latency does not apply, stats isolation)** | **Wired, verified (stage 8)** | `test_dcm_bypass.cc` (15 blocks) |
| **WB → far-memory write acceptance/retry (capacity available/full/frees-up, multiple entries, sustained-pressure stress test, mixed traffic, ordering, parent-request independence, occupancy stats)** | **Wired, verified (stage 9)** | `test_dcm_wb_retry.cc` (9 tests + stress test) |
| **All remaining DCM→memory dispatch paths (near tag-check read, near direct write, near cache-fill write, far demand read, bypass read, bypass write) guarantee eventual delivery — no silent drops anywhere** | **Wired, verified (stage 10)** | `test_dcm_memory_dispatch.cc` (12 tests + 3 stress tests) |
| **Case Study 1/2/3 experiment harness — CLI-selectable policy/bypass/link-latency, DCM stats printing, `run_case_studies.sh` dry-run automation** | **Wired, verified (stage 11)** | 7-configuration tiny sanity runs, `docs/case_study_reproduction_plan.md` Section 6 |
| **Duplicate-address merge safety (CRB-promoted same-address request, LLC-reachable read+writeback, index not poisoned, no duplication, bypass, all 3 policies)** | **Wired, verified (stage 13)** | `test_dcm_duplicate_merge.cc` (6 blocks) |
| **Bandwidth-utilisation arithmetic (exact values, warmup>ROI, short/oversubscribed ROI, degenerate inputs, purity, paper near/far peaks)** | **Wired, verified (stage 13)** | `test_dcm_bandwidth_stats.cc` (6 blocks) |
| **DRAM-cache warmup (cold-before, install-on-warmup, ROI hit on warmed line, ROI-stat isolation, dirty survives, policy-independent, bypass excluded, no loss/dup)** | **Wired, verified (stage 13)** | `test_dcm_warmup.cc` (8 blocks) |
| **Real LLC → DCM integration via the actual ChampSim `CACHE` (read miss end-to-end, LLC hit absorbed, same-address read+writeback, sustained conservation, bypass, all 3 policies)** | **Wired, verified (stage 14)** | `test_dcm_llc_integration.cc` blocks 1–6 |
| **Address granularity / capacity correctness at the real LLC boundary** (adjacent 64 B lines must not alias; same-index/different-tag conflict + dirty replacement; direct-injection vs real-LLC entry-path equivalence; `DCM_NUM_LINES * DCM_BLOCK_SIZE == 128 MiB`) | **Correct, verified (stage 15)** — see "ChampSim PACKET::address convention and DCM normalization" below | `test_dcm_llc_integration.cc` Tests 7–10 |

All 12 request cases from the original master instruction are now fully
wired and verified.

`debugForceHit`/`debugForceDirty` remain declared, inert legacy test
hooks (not read by the live request path).

## Explicitly NOT implemented yet (do not assume otherwise)

- ~~BEAR-Wr-Opt~~ — **DONE, stage 5** (see above).
- ~~Oracle~~ — **DONE, stage 6** (see above). All three paper policies
  are now implemented.
- ~~Controller frontend/backend latency~~ — **DONE, stage 7** (see
  above).
- ~~`bypass_dcache`-equivalent / no-DRAM-cache comparison toggle~~ —
  **DONE, stage 8** (see above).
- ~~WB→far dispatch retry-on-nack modeling~~ — **DONE, stage 9** (see
  above).
- ~~near-side `MEMORY_CONTROLLER` dispatch hardening~~ and
  ~~`bypassDcache` write retry~~ — **DONE, stage 10** (see above; every
  DCM→memory dispatch path now guarantees eventual delivery, see
  `docs/memory_dispatch_audit.md`).
- Per-technology (HBM2 vs DDR4) tRP/tRCD/tCAS differentiation — a
  documented, explained gap (paper doesn't give the numbers; gem5's real
  values are baked into undisclosed SimObjects), not a "not yet" item —
  see `limitations.md`.
- Per-instance `DRAM_CHANNEL_WIDTH` (bus width) — global macro in this
  ChampSim tree, not adjustable per controller; documented gap.
- An NVM far-memory profile — **audited, decision: DO NOT IMPLEMENT**
  (`docs/nvm_support_audit.md`). ChampSim's `MEMORY_CONTROLLER` has no
  read/write-latency-asymmetry mechanism at all (verified directly,
  `dram_controller.cc:218-225`), which is specifically what the paper's
  own NVM finding depends on (asymmetric read/write latency + limited
  write buffer creating back-pressure, not merely "NVM is slower").
  Case Study 3's primary conclusion (impact of link latency on
  DRAM-cache viability) is already fully reproducible via the
  already-implemented DDR4-only sweep; NVM is a secondary finding this
  port's architecture cannot faithfully represent without either
  inventing numbers or citing gem5-generic (not paper-confirmed)
  defaults.

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
