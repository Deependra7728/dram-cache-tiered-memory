# Known Limitations

## Trace-based / not full-system (fundamental, inherent to ChampSim)

- No Linux boot, no gem5-style checkpointing. ChampSim's own
  warmup-instructions-then-ROI model is the available analog for "warm up
  the DRAM cache before measuring" — it is not equivalent to the paper's
  100ms-then-checkpoint methodology, and no claim of equivalence will be
  made.
- ChampSim traces carry addresses and control flow, not real memory
  content. `MEMORY_CONTROLLER` in this ChampSim tree never simulates
  actual data values (confirmed: `dram_array` fields exist in
  `inc/block.h`/`inc/dram_controller.h` but are never populated or read
  in `dram_controller.cc` — pre-existing dead code, not something this
  port relies on). Consequently the DRAM cache manager is a timing/state
  simulation (hit/miss/dirty bits, request counts, cycles), not a
  byte-accurate cache.
- No claim of exact gem5 IPC/BIPS/BW-number equivalence will be made.
  Comparisons are structural (does the request path and access-
  amplification pattern match Table II?).

## Current-ChampSim-specific constraints

- **`return_data` is a single per-object entry point**, not per-port (see
  `gem5_to_champsim_mapping.md` "Deliberate adaptations").
- **`add_pq` / prefetch traffic to the lower level is already a no-op**
  in the pre-existing `MEMORY_CONTROLLER`; mirrored, not changed.
- **`NUM_CPUS` is 1** in this tree's `champsim.h`, not 8 as in the
  paper's Table I. Not changed without being asked (large blast radius:
  LLC sizing macros, the `ooo_cpu` array).
- **`DRAM_CHANNEL_WIDTH` is a global macro (fixed at 8 bytes)**, not a
  per-`MEMORY_CONTROLLER` setting. This means near (HBM2) and far (DDR4)
  cannot differ in bus width in this port's bandwidth model — only in
  `DRAM_MTPS` (transfer rate). The paper's real HBM2/DDR4 channels likely
  differ in both width and rate; this port reproduces the *aggregate*
  declared peak bandwidth (32 GB/s / 19.2 GB/s) via rate alone. Flagged
  here rather than silently assumed equivalent.
- **tRP/tRCD/tCAS are NOT differentiated between near and far.** The
  mechanism to do so exists (`MEMORY_CONTROLLER::set_timing()`, stage 2)
  and is verified independent (`tests/test_dcm_near_far_config.cc` Test
  2), but `configureNearAsHBM2()`/`configureFarAsDDR4()`
  (`src/dram_cache_manager.cc`) intentionally give both the SAME generic
  12.5 ns default for these three latencies. **Why this cannot be done
  faithfully rather than just "not yet done": gem5's real per-technology
  access latencies live inside its `DRAMInterface` SimObjects
  (`HBM_2000_4H_1x64`, `DDR4_2400_16x4` — pulled directly from the gem5
  repo's own config script, `disaggregated_dram_cache_script.py`), which
  are internal JEDEC-derived timing tables, not disclosed as explicit
  nanosecond numbers anywhere in the paper's own text (arXiv:2303.13029
  or the ISPASS companion).** Only the two *peak bandwidth* numbers (32
  GB/s, 19.2 GB/s) are explicitly given in the paper (Table I) — those
  are reproduced exactly. Inventing plausible-sounding tRP/tRCD/tCAS
  numbers for real HBM2/DDR4 parts would misrepresent an approximation as
  a paper-derived fact, so this port does not do that. If real
  per-technology latency values become available (e.g. from gem5's
  `DRAMInterface` source directly), `configureNearAsHBM2`/
  `configureFarAsDDR4` are the two functions to update.
- **NVM far-memory profile (Case Study 3's second configuration) is
  NOT implemented, by deliberate decision, not oversight** — full audit
  in `docs/nvm_support_audit.md`. Two independent reasons, both load-bearing:
  (1) `MEMORY_CONTROLLER` has no mechanism to give reads and writes
  different latencies at all (verified directly,
  `dram_controller.cc:218-225`: `LATENCY` is computed identically for
  `RQ` and `WQ` scheduling, with no code path to differentiate them),
  and the paper's own explanation for its NVM finding specifically
  depends on that asymmetry (higher, *and asymmetric*, read/write
  latency interacting with NVM's limited write buffer to create
  back-pressure) — not on NVM merely being uniformly slower. A single
  elevated symmetric latency could not reproduce the paper's reported
  mechanism or even its reported DIRECTION of effect. (2) gem5's only
  concrete NVM device numbers (`NVM_2400_1x64`: `tREAD=150ns`,
  `tWRITE=500ns`, and a dozen other device-level parameters) come from a
  **generic, gem5-core SimObject authored by ARM in 2020, unrelated to
  this paper's own work**, and the paper's own reference config script
  (`disaggregated_dram_cache_script.py`, re-pulled and read directly for
  this audit) never actually instantiates it — the exact gem5
  configuration that produced the paper's own NVM figures (12b/13b) is
  not present in the public repository to cite. Implementing NVM support
  would therefore require either inventing a value with no source at
  all, or citing a plausible-but-unconfirmed generic gem5 default as if
  it were paper-specific. Case Study 3's primary conclusion (impact of
  link latency on DRAM-cache viability) does not depend on NVM at all —
  it is fully demonstrated by the paper's own DDR4-only sweep, which is
  already implemented and validated in this port
  (`tests/test_dcm_link_latency.cc`).
- **DCM controller latency models only gem5's static frontend/backend
  terms, not `headerDelay`/`payloadDelay`.** gem5's `accessAndRespond()`
  schedules the response at `curTick() + static_latency + headerDelay +
  payloadDelay`; this port implements `static_latency`
  (`frontendLatency`/`backendLatency`, single-vs-double per response
  path — see `gem5_to_champsim_mapping.md`) exactly, but does not model
  `headerDelay`/`payloadDelay`. These come from gem5's generic
  `Packet`/port-timing machinery (derived from bus width and packet
  size), are not part of the paper's declared "20ns round-trip" figure,
  and are typically small/near-zero relative to the static terms for the
  cache-line-sized (64B) packets this policy manager handles — flagged
  here rather than silently assumed zero. If they become relevant (e.g.
  reproducing exact tick-level gem5 traces), `scheduleResponse()`'s
  `delayCycles` parameter is where an additional term would be added.
- **bypassDcache has no ChampSim equivalent for gem5's far-side
  retry-on-NACK PROTOCOL SIGNAL specifically** (`farMemRecvReqRetry`'s
  bypass branch, `port.sendRetryReq()` — gem5's async port callback that
  tells the ORIGINAL requestor "you may resend now"). This is a
  narrower, cosmetic gap than it first appears: the underlying
  RETENTION/RETRY OUTCOME for bypass reads AND writes is now fully fixed
  (`docs/memory_dispatch_audit.md`'s `dispatchToFarGuaranteed()`, which
  bypass mode's `add_rq()`/`add_wq()` now route through) — a bypass
  request hitting a momentarily-full `farMC` is retained and retried
  automatically, never silently dropped. What remains genuinely absent
  is only gem5's explicit requestor-side "retry now" SIGNAL, which has
  no ChampSim equivalent in ANY mode (ChampSim's synchronous per-cycle
  polling model doesn't need one — ports here don't NACK the LLC the way
  gem5's ports NACK an upstream requestor) — not a bypass-specific gap
  at all, and not something reachable requests could ever be lost to.
- **A real ChampSim precision bug was found and fixed** in this stage:
  the original `DRAM_DBUS_RETURN_TIME` formula
  (`(BLOCK_SIZE/DRAM_CHANNEL_WIDTH) * (CPU_FREQ/DRAM_MTPS)`) uses pure
  integer division for `CPU_FREQ/DRAM_MTPS`. At `CPU_FREQ=4000`, this
  truncates to the same integer (1) for every `DRAM_MTPS` in
  [2001, 4000] — meaning near (4000 MT/s) and far (2400 MT/s) would have
  produced *identical* `DRAM_DBUS_RETURN_TIME` (8 cycles) despite a real,
  intended ~1.67x bandwidth difference, and even ChampSim's own original
  single-DRAM default (3200 MT/s) was already off by 20% (computed 8
  instead of the mathematically correct 10). Fixed via
  `computeDbusReturnTime()` (floating-point + round-to-nearest), used by
  `configureNearAsHBM2`/`configureFarAsDDR4` and by `main.cc`'s
  `--low_bandwidth` knob adjustment. Verified: near=8, far=13 cycles
  (`tests/test_dcm_near_far_config.cc` Test 1). Not fixed in
  `set_timing()`'s general contract or any other pre-existing call site,
  to avoid unrelated scope creep — see `next_task.md` if broader
  DRAM-timing precision work is wanted later.
- **ChampSim's own write-queue-forwarding shortcut**
  (`MEMORY_CONTROLLER::add_rq`, `dram_controller.cc:428-455`) services a
  read *instantly*, bypassing all real read timing, if a matching address
  is still sitting in that controller's write queue. This is pre-existing
  ChampSim behavior (not something this port added), but it surfaced
  directly during this stage's testing: a naive near/far timing-isolation
  test showed a *slower* configured near controller completing a "hit"
  faster than a fast one, because the slow controller's background fill
  write was still stuck in its WQ (not yet scheduled) when the "hit" read
  arrived, and got forwarded instantly instead of going through real read
  timing. Fixed in the test by waiting for the WQ to actually drain
  before measuring (`tests/test_dcm_near_far_config.cc` Test 2). Flagged
  here because it is a real, general property of this ChampSim tree that
  could similarly confuse anyone measuring hit latency shortly after a
  fill, DCM-related or not.

## WB-buffer admission backpressure — implemented, retry-on-full FIXED

`DCM_WB_PRESSURE_THRESHOLD` (`= DCM_ORB_MAX_SIZE/2 = 64`) is gem5's
*actual* mechanism (`policy_manager.cc:332`), not an independently
invented 64-entry cap — see the header comment in
`inc/dram_cache_manager.h` for the exact derivation and why it happens to
equal `DCM_WB_MAX_SIZE` numerically without being the same concept.
Draining is bounded to one entry per `operate()` cycle
(`DRAM_CACHE_MANAGER::drainWB()`), which is what lets the WB deque
genuinely hold a backlog under pressure — verified in
`tests/test_dcm_wb_pressure.cc`.

**A confirmed silent-write-loss bug, found by a dedicated audit
(`docs/wb_retry_audit.md`) and since FIXED and verified.** The audit
found and empirically demonstrated that `MEMORY_CONTROLLER::add_wq()`/
`add_rq()` silently discard a packet with ZERO signal to the caller when
their queue is genuinely full (`dram_controller.cc`'s admission loop has
no bounds check at all), and that this port's original fire-and-forget
`drainWB()`/`dispatchToFar()` inherited full exposure to it. Classified
MUST IMPLEMENT (before write-heavy Case Study reproduction). **Fix**:
`dispatchToFar()` now checks `farMC->get_occupancy()` against
`farMC->get_size()` BEFORE ever calling `add_wq()`/`add_rq()`, returning
`false` (nothing sent, nothing queued) if there is no room;
`drainWB()`/`processPendingFarDispatches()` only pop their front entry
once `dispatchToFar()` confirms success, retaining it for a later
`operate()` cycle otherwise — no `dram_controller.cc` changes needed
(`get_occupancy()`/`get_size()` already existed and were already used
for the identical purpose elsewhere). Verified with
`tests/test_dcm_wb_retry.cc` (9 required tests + a 3-wave, 150-write
stress test: `lost=0 duplicated=0`); all 10 pre-existing
`tests/test_dcm_*.cc` suites pass unchanged. See
`docs/wb_retry_audit.md`'s "RESOLUTION" section for full details and the
exact gem5 correspondence. The identical `MEMORY_CONTROLLER` exposure
was ALSO separately reachable via `nearMC`'s tag-check-read and
background-fill-write dispatch paths, and via `bypassDcache`'s reads and
writes — **all fixed in a follow-up stage**, see below.

## All remaining DCM → memory dispatch paths — audited and FIXED

A follow-up audit (`docs/memory_dispatch_audit.md`) found the identical
`MEMORY_CONTROLLER` silent-drop exposure (or, for far-read/bypass, an
ignored `dispatchToFar()` return value producing the functionally
equivalent outcome — a permanently stuck request) on FIVE more call
sites: near-memory tag-check READ, near-memory direct WRITE,
near-memory cache-fill WRITE, far-memory demand READ, bypass-mode READ,
and bypass-mode WRITE. **All fixed and verified.** Two new
guaranteed-delivery wrappers: `dispatchToNear()` (near side, new
`pendingNearDispatches` retry queue, drained by
`processPendingNearDispatches()` every `operate()` cycle) and
`dispatchToFarGuaranteed()` (far side, reuses the EXISTING
`pendingFarDispatches` queue already proven correct for WB —
`dispatchToFar()`'s own `bool`-returning, caller-decides contract stays
UNCHANGED, still used correctly and only by `drainWB()`). Verified with
`tests/test_dcm_memory_dispatch.cc` (12 required tests + 3 stress
tests, all `generated == accepted == completed`, `lost=0 duplicated=0`);
all 11 pre-existing `tests/test_dcm_*.cc` suites (including
`test_dcm_wb_retry.cc`) pass unchanged; full-binary smoke test
byte-identical. No DCM→memory dispatch path can silently drop a request
anymore. See `docs/memory_dispatch_audit.md` for the full audit table,
per-path gem5 correspondence, and design rationale (why two separate
wrapper functions rather than one over-generalized mechanism: far
dispatch has a link-latency concept near dispatch does not).

## Link latency — implemented this stage, scope note

`linkLatencyCycles` applies to both far-read fetches and far-write
write-backs (verified, `tests/test_dcm_link_latency.cc` Tests 1/2 and 4)
and never affects local-only hits (Test 3). It does NOT yet apply to
Case Study 3's other half — the paper studies far-memory *technology*
(DDR4 vs NVM) together with link latency; only the technology's
bandwidth (via `configureFarAsDDR4`) and the link latency (via
`setLinkLatency`) exist independently. An NVM far-memory profile
(higher latency than DDR4, per the paper) has not been added — not
asked for in this stage's task list, and the paper does not give NVM's
exact timing numbers either (same gap as the HBM2/DDR4 latency gap
above).

## Skeleton-stage-specific limitations carried forward (unchanged this stage)

- CRB promotion picks the oldest matching entry per index (matches gem5,
  `resumeConflictingReq` is per-index, not global-FIFO) — inherited
  behavior, not a new limitation.
- ~~The three per-policy transition tables (BEAR-Wr-Opt, Oracle) are not
  implemented~~ — **RESOLVED**: all three (baseline, BEAR-Wr-Opt, Oracle)
  are now implemented and verified (Stages 5-6).

## BEAR-Wr-Opt / Oracle stage-specific notes

- **A write hit (BEAR-Wr-Opt or Oracle) or an Oracle write-clean-miss
  completes synchronously**, within the same `add_wq()` call that
  admitted it, because skipping `DCM_LOC_MEM_READ` removes the only
  asynchronous step in that path. This is not a bug (it is the entire
  point of the optimization — these requests genuinely finish faster),
  but it means such a request can never be observed as "still
  outstanding" by anything else in a single-threaded synchronous test.
  Both `tests/test_dcm_bear.cc` and `tests/test_dcm_oracle.cc` initially
  hit this while designing their CRB-interaction tests and worked around
  it by using a read-based blocker instead (Oracle's read-clean-miss
  path goes to `DCM_FAR_MEM_READ`, which IS genuinely asynchronous) —
  documented here in case a future test design runs into the same thing.
- **Oracle's "zero-latency SRAM tag store" charges no separate latency**
  in either gem5 or this port — confirmed by reading gem5's code rather
  than assuming from the paper's prose. The entire modeled effect is
  "the physical DRAM read is never dispatched"; there is no separate SRAM
  access-time term added anywhere.
- **BEAR-Wr-Opt's "LLC-side metadata" is not actually implemented in
  gem5**, despite the paper's prose describing it that way (Section
  V-a: "BEAR determines the write hit accesses using the metadata stored
  in the last level cache"). The actual gem5 code reuses the DRAM cache
  manager's own eagerly-computed classification (`orbEntry->isHit`,
  shared by every policy) — there is no separate LLC-metadata mechanism
  anywhere in `policy_manager.cc`. This port matches gem5's actual code
  (`classifyAndInstall()`, likewise shared by every policy), which is the
  paper's own reference implementation and therefore the authoritative
  behavior — not the paper's simplified prose description. Documented
  here since it is exactly the kind of prose-vs-code discrepancy that
  could otherwise be silently "fixed" by someone reading only the paper.

## Case Study reproduction — trace/workload limitations (stage 11)

Full investigation and per-Case-Study classification in
`docs/case_study_reproduction_plan.md`. Summary of the load-bearing
limitations that carry into any reproduction attempt using this
port's new `run_case_studies.sh` harness:

- **No GAPBS/NPB traces exist in this environment.** The paper's Case
  Studies 1–3 all run GAPBS (graph algorithms) and NPB (NAS Parallel
  Benchmarks) on an 8-core, full-system, Linux-booted gem5 target
  (paper Section III). This ChampSim environment has only its own
  DPC-3 SPEC CPU2006 trace set (7 files, 6 valid + 1 corrupt — see
  below) — a categorically different, single-threaded, non-graph
  workload suite. No claim of equivalence between SPEC CPU2006 and
  GAPBS/NPB access patterns is made anywhere in this port's
  documentation; every Case Study reproduction is classified ADAPTED,
  not EXACT, specifically because of this substitution (in addition to
  the already-documented `NUM_CPUS=1` and no-full-system-boot gaps).
- **`619.lbm_s-3766B.champsimtrace.xz` is corrupt** — `xz -t` reports
  "Unexpected end of input," consistent with a truncated download
  (70.5 MB vs. the other traces' 144–509 MB range), not a random
  bit-flip. This was not previously flagged: prior stages' smoke tests
  only ever exercised `403.gcc-16B.champsimtrace.xz`. Excluded by
  `run_case_studies.sh`'s default trace list.
- **Trace instruction counts are a size-based estimate, not a directly
  verified count.** `xz --list`'s reported uncompressed size (119.2 GiB
  for every valid trace) divided by the 64-byte
  `trace_instr_format_t` record size gives ≈1.95–2.0 billion
  instructions per trace — decompressing and counting every record
  directly was not attempted (would require ~120 GB of disk and
  significant time per trace, out of scope for "do not run long
  simulations yet"). This estimate is sufficient to confirm trace
  length is not a limiting factor for a real reproduction run (Section
  9 of the reproduction plan), but is flagged as an estimate, not a
  measured fact.
- **NVM half of Case Study 3 remains NOT REPRODUCIBLE** — carried
  forward unchanged from `docs/nvm_support_audit.md`'s DO-NOT-IMPLEMENT
  decision; `run_case_studies.sh` has no NVM configuration for this
  reason.
- **Bandwidth-utilization metric (`print_dcm_stats()`'s new
  LOCAL_BW_UTILIZATION/FAR_BW_UTILIZATION fields) assumes `NUM_CPUS==1`**
  when converting elapsed cycles from `ooo_cpu[0]` alone — consistent
  with this build's actual `NUM_CPUS=1`, but would need a per-CPU or
  shared-channel treatment if that macro were ever changed (itself
  already a documented, deliberately-unchanged gap).

## Duplicate-address merge in MEMORY_CONTROLLER — contained, not removed

`MEMORY_CONTROLLER::add_rq()`/`add_wq()` (`src/dram_controller.cc`) merge
a packet whose address already appears in the destination queue: they
return that entry's index and enqueue nothing. This is **pre-existing
ChampSim behaviour and is left unchanged** — in stock ChampSim it is
correct, because the caller is a `CACHE` that merges via its own MSHR.

The DCM cannot tolerate it for **reads**, because it needs exactly one
`return_data()` per dispatched read to advance the owning ORB entry; a
merged read produced no completion and stranded that entry permanently,
also locking its DRAM-cache index (`docs/final_independent_audit.md`
CRITICAL-1). Rather than disabling merging globally, the DCM now
*detects* it: `trySend()` treats a non-negative `add_rq()` return as
"merged, not enqueued" and retries via the existing retain-and-retry
path. **Write** merges are still accepted as genuine sends — coalescing
two writes to one address is legitimate write-queue behaviour, produces
no response either way, and cannot lose anything in a model that carries
no data.

Residual limitation: the DCM therefore never has two same-address reads
concurrently resident in one controller queue. That is a deliberate
divergence from what stock ChampSim would do, and it is the correct
behaviour for a requester that has no MSHR of its own.

## DRAM-cache warmup is metadata-only

The DCM now warms its tag/metadata state during ChampSim's warmup phase
(`warmupTagUpdate()`), so the ROI no longer begins against a cold cache.
The warming is **metadata-only by design**: no ORB/CRB/WB entry is
created, no DRAM timing is simulated, and no ROI statistic is touched.

Consequences to be aware of when interpreting results:
- Warmup preserves ChampSim's existing contract of returning memory data
  immediately, so warmup wall-clock and warmup cycle counts are
  unchanged (measured: warmup completed at the identical cycle,
  46,674,846, before and after the change).
- Dirty victims evicted *during warmup* generate no write-back traffic,
  because no timing is modelled in that phase. Only the resulting
  metadata state carries into the ROI — which is what the ROI's
  hit/miss/dirty classification actually depends on.
- This is a faithful trace-based analogue of the paper's warmed-cache
  methodology, not an exact reproduction of it: gem5 warms by executing
  100 ms of full-system time with complete timing fidelity.

## ChampSim PACKET::address convention and DCM normalization

**Formerly an OPEN CRITICAL DEFECT — resolved in stage 15.** Recorded here
because it constrains how any future code touching the DCM boundary must
be written.

### The convention (verified by source inspection, not assumed)

`PACKET::address` is a **cache-line (block) address** everywhere in this
tree: `address == full_addr >> LOG2_BLOCK_SIZE`, where `full_addr` is the
physical **byte** address.

| Layer | Evidence |
|---|---|
| CPU → L1 | `src/ooo_cpu.cc:1696,2253` — `address = physical_address >> LOG2_BLOCK_SIZE`, `full_addr = physical_address` |
| CACHE (all levels) | `src/cache.cc:1058` — `get_set()` masks `address` *directly*; `get_way()` compares the whole `address` as a tag |
| CACHE → `lower_level` | `src/cache.cc:661,672,115,445` — the packet is forwarded **unmodified** |
| CACHE writeback | `src/cache.cc:107` — `writeback_packet.address = block[set][way].address` (already a line address) |
| MEMORY_CONTROLLER | `src/dram_controller.cc:634` — `dram_get_channel()` uses `shift = 0` |

The one exception is the **TLB path** (`src/ooo_cpu.cc:1459,1542`), where
`address` is a page number. That convention never reaches the DCM.

### What the DCM expects, and where normalization happens

The DCM consumes the same convention, so normalization is a no-op *by
design* and is localized entirely to `returnIndexDC()`/`returnTagDC()` in
`src/dram_cache_manager.cc`:

```
incoming PACKET::address  (already a cache-line address)
  -> lineAddr = address                    (no shift, no divide)
  -> indexDC  = lineAddr % DCM_NUM_LINES   (direct-mapped, paper baseline)
  -> tagDC    = lineAddr / DCM_NUM_LINES
```

**Constraint for future work**: `PACKET::address`'s global meaning must
not be redefined to fix a DCM-local problem. If the DCM ever needs a byte
address, take it from `full_addr`; do not re-derive it by shifting
`address`, and do not add a second normalization site.

### How 64-byte granularity is guaranteed

`indexDC` advances by exactly 1 per consecutive cache-line address, so
adjacent 64-byte lines *L* and *L+1* always land on different indices.
Asserted through the real LLC in `tests/test_dcm_llc_integration.cc`
Test 7.

### How 128 MiB capacity is guaranteed

`DCM_NUM_LINES = DCM_DRAM_CACHE_SIZE / DCM_BLOCK_SIZE = 128 MiB / 64 B =
2,097,152` is a single source of truth — simultaneously the index modulus
and the size of `tagMetadataStore` (`resize(DCM_NUM_LINES)` in the
constructor). So `DCM_NUM_LINES * DCM_BLOCK_SIZE == DCM_DRAM_CACHE_SIZE`
holds by construction and the index can never fall outside the store.
Asserted in Test 10.

### What the defect was, for the record

`returnIndexDC()` used to compute `(address / DCM_BLOCK_SIZE) % numLines`
— dividing by the line size a second time, because `address` was already
a line address. In the production path (`main.cc` → real LLC → DCM) that
made 64 consecutive distinct lines alias onto one index: effective line
size 4096 B and effective capacity 8 GB, with hit rates overstated and
miss counts understated. It was never a stability or data-loss defect —
every request-conservation invariant held exactly throughout.

It took a real-LLC integration test to find it: the unit suites were
internally self-consistent (they hand-rolled the same formula), so
Table II measured 24/24 exact both before and after. Those hand-rolled
sites now call `dcm.returnIndexDC()`; no expected value changed.

**Residual caveat**: full-binary hit/miss numbers collected before
stage 15 are invalid and must be regenerated. None had been collected for
publication, so nothing downstream is affected.


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
