# Next Task

Stages 1–6 are all done and verified: integration skeleton; per-instance
timing + ORB/CRB; complete baseline tag/metadata behavior; WB
backpressure + near/far config + link latency + statistics; BEAR-Wr-Opt;
Oracle. **All three paper policies (baseline/CascadeLakeNoPartWrs,
BEAR-Wr-Opt, Oracle/RambusHypo) are now implemented, independently
selectable, gem5-cross-checked, and Table-II-verified.** This completes
the master project's core "port the DRAM cache model" objective for the
policy state machines.

A final no-code-changes audit (`docs/final_paper_coverage_audit.md`)
then found two remaining gaps against gem5: (a) controller
frontend/backend latency, and (b) `bypass_dcache` (no-DRAM-cache
comparison mode). **Both are now DONE** — see below. Two independent
follow-ups remain.

## Just completed — CRITICAL/HIGH audit fixes (stage 13)

The independent audit (`docs/final_independent_audit.md`) found four
blocking defects; **all four are now fixed and verified**
(full detail in that document's §15 Remediation, and in
`docs/implementation_status.md` Stage 13):

- **CRITICAL-1/-2 duplicate-address merge.** `MEMORY_CONTROLLER::add_rq()`
  merges a same-address packet and enqueues nothing; `trySend()` ignored
  that, so a CRB-promoted request could be silently dropped, stranding
  its ORB entry and permanently blocking that DRAM-cache index.
  `trySend()` now detects the merge (`rc >= 0`) and retries via the
  existing retain-and-retry path. A second instance of the same hole in
  `processPendingFarDispatches()` (raw `add_rq` instead of `trySend`)
  was found while verifying and also fixed.
- **HIGH-1 bandwidth denominator.** Was double-subtracting
  `begin_sim_cycle` and unsigned-underflowing whenever warmup exceeded
  the ROI in cycles (observed: `1.73e-16%`). Now uses the ROI delta
  directly via a unit-testable `dcmBandwidthUtilization()` helper.
- **HIGH-2 cold DRAM cache.** The DCM was inert during warmup, so every
  ROI started cold (96% cold misses). It now warms tag/metadata state
  via `warmupTagUpdate()` (metadata only — no timing, no ROI stats), and
  `resetROIStats()` clears ROI counters at the boundary while leaving
  the cache warm.
- **MEDIUM-1 instrumentation.** Result files now report completion
  counters and a residual ORB/CRB/WB line, so a stranded request can no
  longer hide.

Regression at that stage: **15/15 suites pass** (12 pre-existing, none weakened, plus
`test_dcm_duplicate_merge.cc`, `test_dcm_bandwidth_stats.cc`,
`test_dcm_warmup.cc`). Table II re-measured after every change:
**24/24 exact**.

## What's left

0. ~~BLOCKING — DRAM-cache address-granularity defect~~ — **DONE
   (stage 15)**. `PACKET::address` is a cache-line address everywhere in
   ChampSim; the DCM was dividing by the line size a second time, aliasing
   64 distinct lines onto one index (effective 4096 B lines / 8 GB
   capacity). Fixed by a localized normalization at the DCM interface —
   `returnIndexDC()` is now `address % DCM_NUM_LINES` and `returnTagDC()`
   is `address / DCM_NUM_LINES`, with `DCM_NUM_LINES` (= 2,097,152) as the
   single source of truth for both the index modulus and
   `tagMetadataStore`'s size. `PACKET::address`'s global meaning was not
   changed. See `docs/final_independent_audit.md` §16 (Resolution),
   `docs/limitations.md`, and `docs/gem5_to_champsim_mapping.md`
   §"ChampSim PACKET::address convention and DCM normalization".
   Table II re-measured **24/24 exact**; the 18 hand-rolled index-formula
   sites in the unit suites now call `dcm.returnIndexDC()` (no expected
   value changed); `test_dcm_llc_integration.cc` Test 7 is no longer a
   characterisation test and Tests 8–10 were added.

1. **Launch the full Case Study matrix** — **no longer blocked**, and the
   post-fix production pre-flight is now DONE (stage 16: `BASELINE` /
   `462.libquantum-1343B`, 600M instructions, 18/18 conservation
   invariants exact, fully drained, 79.37% DCM hit rate — see
   `docs/case_study_reproduction_plan.md` §11). All pre-fix full-binary
   results are invalidated and superseded.
   Infrastructure and the exact command are ready
   (`docs/case_study_reproduction_plan.md` Section 9) and the harness now
   validates every run. Note that DRAM-cache hit rates will be far lower
   (and correct) than any pre-stage-15 spot numbers: on a short sanity
   trace the same workload went from a 93.6% aliased hit rate to 0.0%
   with all 422 LLC misses correctly seen as 422 distinct lines.
2. ~~HIGH-3~~ — **DONE**: the false gem5 `isDirty` claim is corrected in
   the header and in every doc that repeated it.
3. ~~MEDIUM-2~~ — **DONE**: `run_case_studies.sh` now validates every run
   (exit code, output present/complete, required statistics per mode,
   instruction count reached, crash signatures, jammed DCM) and exits
   non-zero on any failure. Still overwrites prior results silently —
   run-stamped output directories remain a nice-to-have.
4. ~~MEDIUM-3~~ — **DONE**: `tests/test_dcm_llc_integration.cc` drives the
   real `CACHE`/LLC into the real DCM. It immediately found item 0 above.
5. **LOW-1/LOW-2, COSMETIC-1** — see `docs/final_independent_audit.md`.

## Explicitly deferred, no action needed yet

- `NUM_CPUS`/8-core Table I parameter — flagged in `limitations.md`, not
  changing without being asked.
- `DRAM_CHANNEL_WIDTH` per-instance differentiation (currently a global
  macro) — would require a larger, more invasive change to
  `dram_controller.h`/`.cc` than `set_timing()`'s field-per-instance
  approach; not requested yet.

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




## Stage 20 — fixed 1B / 500M production methodology (CURRENT)

The adaptive/dynamic DCM warmup experiment (stage 18) and its ROI-alignment
harness support (stage 19) were **rejected and removed**. The production
methodology is now a fixed, deterministic instruction-count warmup:

```
Warmup = 1,000,000,000 instructions
ROI    =   500,000,000 instructions
```

applied identically to all seven configurations, including
`NO_DRAM_CACHE`. `run_case_studies.sh` defaults to these values and the
42-run matrix structure (7 configurations x the 6 valid DPC-3 traces) is
unchanged. See `docs/limitations.md` § "Production warmup methodology --
fixed 1B / 500M" for the rationale and the accurate statement of what the
paper actually does (100 ms wall-clock warmup in full-system gem5 -- NOT
1B instructions).

The validated DCM implementation was not touched: 16/16 suites pass,
Table II is 24/24, and BASELINE/BEAR/ORACLE produce IPC identical to the
pre-revert build on the same short trace.

### What's left, in priority order

1. **Launch the 42-run matrix** at 1B/500M once the representative
   434.zeusmp result has been inspected and approved. Not launched yet.
2. **Close Case Study 3** (audit P2): the 18 `NO_DRAM_CACHE x
   {100,500,1000} ns` runs needed for paper Fig. 13. The NVM far-memory
   arm (Figs. 12b/13b) still has no model.
3. **Remove or quarantine** `Result_Verify_pls/BASELINE_postfix__403.gcc-16B.txt`
   (audit P3) -- a pre-granularity-fix result with a 99.88% hit rate.
4. Audit P4 (ORB/CRB/WB capacities never exercised at `NUM_CPUS=1`) remains
   open and is inherent to the 1-core adaptation.
