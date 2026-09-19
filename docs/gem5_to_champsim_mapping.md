# gem5 → ChampSim Mapping

This document tracks how the DRAM-cache model from the paper
"Enabling Design Space Exploration of DRAM Caches in Emerging Memory
Systems" (arXiv:2303.13029, ISPASS'23) and its gem5 reference
implementation map onto this ChampSim tree. Updated incrementally as
implementation proceeds.

Source of truth used for the gem5 side: branch `dram_cache_disaggregated`
of `darchr/dram-cache-model`, files `src/mem/policy_manager.{hh,cc}`.

**IMPORTANT correction to initial assumptions**: the repo has two
relevant-looking branches. `dram_cache_CascadeLake` implements a
*unified* controller (`src/mem/dram_cache_ctrl.{hh,cc}`) where the DRAM
cache and main memory share one controller/bus — that is the *prior*
paper (arXiv:2303.13026, ref [18] in the arXiv version of our target
paper). `dram_cache_disaggregated`'s `PolicyManager` class is the
*discrete* model that talks to two independent memory controllers via
separate ports — that is the model this project ports. Confirmed by
reading both branches' file trees and the target paper's Section II
("A prior work proposed a unified DRAM cache controller... Our model
in this work is able to cover this case... we will focus on
[the discrete] model").

## Which ChampSim this is

This tree is the old single-binary academic ChampSim (~2021): global
`PACKET`/`PACKET_QUEUE` structs, a `MEMORY` interface base class,
`CACHE : public MEMORY`, and a cycle-driven `MEMORY_CONTROLLER : public
MEMORY` with explicit bank/row/column timing (`inc/dram_controller.h`,
`src/dram_controller.cc`). It is not the newer JSON/config-driven
multi-TU ChampSim. This matters because:
- The LLC's `lower_level` is a raw `MEMORY*` pointer — inserting our
  manager is a pointer retarget, not a config change.
- `MEMORY_CONTROLLER` already has cycle-level bank/row/dbus timing and
  its own request queues, which is structurally the closest existing
  ChampSim analog to gem5's `mem_ctrl`/`mem_interface` — so per the
  kickoff instructions we reuse it as-is for both near and far memory
  instead of writing a new DRAM timing model.

## Architecture mapping

| gem5 (`PolicyManager`) | ChampSim (this port) |
|---|---|
| `PolicyManager : public AbstractMemory`, sits where the memory controller was | `DRAM_CACHE_MANAGER : public MEMORY`, replaces `uncore.DRAM` as `uncore.LLC.lower_level` |
| `locReqPort` → local `MemCtrl`/`HBMCtrl` | `nearMC` → a `MEMORY_CONTROLLER` instance (DRAM cache device) |
| `farReqPort` → far `MemCtrl` | `farMC` → a second, independent `MEMORY_CONTROLLER` instance (backing store) |
| `ORB` : `std::map<Addr, reqBufferEntry*>` | `ORB` : `std::map<uint64_t, DCM_ORB_ENTRY*>`, keyed by `PACKET::address` |
| `CRB` : `std::vector<std::pair<Tick, PacketPtr>>` | `CRB` : `std::vector<DCM_CRB_ENTRY>` (declared in skeleton; not yet functional — see `next_task.md`) |
| `pktFarMemWrite` (WB buffer) | `WB` : `std::deque<DCM_WB_ENTRY>` (declared in skeleton; not yet functional) |
| `tagMetaStoreEntry` / `tagMetadataStore` | `DCM_TAG_ENTRY` / `tagMetadataStore` (allocated in skeleton, not yet consulted for hit/miss) |
| `reqState` enum (`start`, `locMemRead`, ...) | `DCM_REQ_STATE` enum, same stages, renamed |
| `enums::Policy` (`CascadeLakeNoPartWrs`, `BearWriteOpt`, `RambusHypo`) | `DCM_POLICY` (`DCM_POLICY_BASELINE_CASCADE_LAKE`, `DCM_POLICY_BEAR_WR_OPT`, `DCM_POLICY_ORACLE`) |
| `recvTimingReq` (admission + ORB/CRB backpressure) | `add_rq`/`add_wq` (skeleton only checks ORB size so far; CRB/WB pressure checks are next_task) |
| ports call back via `locMemRecvTimingResp`/`farMemRecvTimingResp` (two distinct functions, one per port) | single `DRAM_CACHE_MANAGER::return_data(PACKET*)`, disambiguated by looking up `ORB[addr]->state` — see "Deliberate adaptations" below |

## Confirmed gem5 behavioral facts (read directly from `policy_manager.cc`)

These are used to keep the ChampSim port honest, not invented:

1. **Respond-before-fill on read miss.** In `setNextState`/`handleNextState`,
   when the far-memory read response arrives for a read miss, gem5 calls
   `accessAndRespond()` on the *original* LLC-facing packet **immediately**,
   before issuing the local cache-line fill write. The fill write happens
   in the background and is not on the critical path of the response.
   (`policy_manager.cc:730-768`, `:1148-1184`, `:955-993`). The ChampSim
   port's read-miss path mirrors this: `completeRequest()` is called
   before the fill write is issued.
2. **Writes ack at admission, not at physical completion.** In
   `handleRequestorPkt`, `if (pkt->isWrite()) { accessAndRespond(...) }`
   happens right after admission, before the tag check even runs
   (`policy_manager.cc:1383-1417`). This matches ChampSim's own existing
   contract for writebacks: `CACHE::handle_writeback()` calls
   `lower_level->add_wq(&writeback_packet)` and never waits for a
   response (`cache.cc:115`), and `MEMORY_CONTROLLER::process()` never
   calls `return_data` for the write-queue branch at all
   (`dram_controller.cc:302-320` vs `:321-349`). So in this port, writes
   admitted via `add_wq` never produce an `upper_level_dcache` callback —
   this was true before this port existed and remains true.
3. **Admission order in `recvTimingReq`**: conflict-by-index check (→ CRB,
   retry if CRB full) → WB-buffer-occupancy check (retry if
   `pktFarMemWrite.size() >= orbMaxSize/2`) → ORB-full check (retry) →
   admit (`policy_manager.cc:296-366`). **UPDATE (this stage): the
   conflict-by-index check and CRB-full check are now ported** (see
   "ORB/CRB conflict handling" below). WB-buffer admission pressure is
   still a `next_task.md` item (WB buffer itself is still inert).
4. **Conflict is checked only against the ORB, not the CRB**
   (`checkConflictInDramCache`, `policy_manager.cc:1447-1460`): a new
   request conflicts if some *ORB* entry already occupies its DRAM-cache
   index. It does not separately check the CRB, because anything else
   waiting for that same index is already queued behind the ORB entry in
   the CRB — checking the ORB alone is sufficient. Ported exactly as
   `DRAM_CACHE_MANAGER::checkConflictInORB()`.
5. **`resumeConflictingReq` runs at every "done" transition**, one call
   site per policy per completion type (6 call sites total —
   `policy_manager.cc:1072,1121,1182,1228,1289,1338`, i.e. read-hit-done
   and write-done for each of the three policies). It: (a) removes the
   completed entry from the ORB, (b) scans the CRB front-to-back for the
   first entry whose index matches the freed index, (c) if found, removes
   it from the CRB and re-admits it via `handleRequestorPkt` with its
   *original* arrival tick preserved (`ORB.at(confAddr)->arrivalTick =
   entry.first;`, `policy_manager.cc:1654`), and (d) flags the newly
   promoted entry's `conflict` bool if yet another CRB entry is still
   queued behind it for the same index (`checkConflictInCRB`,
   `policy_manager.cc:1658,1698-1710` — informational bookkeeping, not
   read by any dispatch logic in the baseline policy). Ported as
   `DRAM_CACHE_MANAGER::promoteFromCRB()`, called from the single
   `completeRequest()` function that all of this port's completion paths
   already funnel through (one call site covers what gem5 needs six for,
   since this port doesn't yet have gem5's three separate policies).
6. **Per-policy state machines share the same skeleton** (`start` →
   `locMemRead` → hit-or-miss branch → ... → done) but differ in exactly
   which steps skip the local tag-check read (`BearWriteOpt` skips it for
   write hits only; `RambusHypo`/Oracle skips it for write hits *and*
   clean misses, deciding hit/dirty *before* state `start` even runs).
   None of the three per-policy tables are ported yet (`next_task.md`).
7. **RESOLVED this stage: eager metadata update + tag-check-completion
   write-back trigger.** Re-read `handleRequestorPkt`
   (`policy_manager.cc:1345-1444`) and `locMemRecvTimingResp`
   (`policy_manager.cc:521-545`) directly to confirm two easy-to-miss
   ordering facts, both now ported exactly (see `docs/request_flow.md`
   for evidence):
   - **Classification and the tag/metadata store update happen eagerly at
     admission**, `checkHitOrMiss` → dirty-victim capture → tag store
     write, all inline in `handleRequestorPkt`, i.e. BEFORE the tag-check
     read is even dispatched. The logical cache state is updated
     immediately; the physical DRAM timing for that same tag-check read is
     simulated afterward, decoupled. Ported as
     `DRAM_CACHE_MANAGER::classifyAndInstall()`, called at the top of
     `admitRequest()`.
   - **The dirty victim's write-back is pushed when the tag-check read
     *physically completes*** (`locMemRecvTimingResp`, unconditional on
     read/write outer op since the tag-check is always a physical read),
     not at admission time — the tag-check read is what "sources" the
     victim's data in the hardware being modeled (tag+data co-located in
     ECC bits). Ported as `DRAM_CACHE_MANAGER::pushDirtyWriteBack()`,
     called from `return_data()`'s `DCM_WAITING_LOC_MEM_READ_RESP` branch,
     before the hit/miss/write branch that follows it.

## Deliberate adaptations (documented per kickoff instructions, not silent)

- **Single `return_data` instead of two ports.** gem5 uses two distinct
  `RequestPort`s (`locReqPort`, `farReqPort`), so `locMemRecvTimingResp`
  and `farMemRecvTimingResp` are naturally distinct callbacks. ChampSim's
  `MEMORY` interface has one `return_data(PACKET*)` entry point per
  object. Rather than build a port abstraction ChampSim doesn't have, both
  `nearMC` and `farMC` have their `upper_level_dcache`/`upper_level_icache`
  pointers set to the `DRAM_CACHE_MANAGER` itself, and `return_data`
  disambiguates by looking up `ORB[packet->address]->state`
  (`DCM_WAITING_LOC_MEM_READ_RESP` vs `DCM_WAITING_FAR_MEM_READ_RESP`).
  This is safe because, like gem5's ORB, a given address has at most one
  outstanding sub-request at a time in the state machine. Documented here
  instead of silently picked because it is the one place the port
  structure genuinely differs from gem5's.
- **Pre-warmup bypass preserved.** `MEMORY_CONTROLLER::add_rq`/`add_wq`
  short-circuit and call `return_data` immediately when
  `all_warmup_complete < NUM_CPUS` (`dram_controller.cc:419-426,495-496`).
  This existing ChampSim behavior (dummy responses during warmup, no real
  timing/state touched) is replicated at the `DRAM_CACHE_MANAGER` level so
  that swapping in the manager does not change what happens during
  ChampSim's warmup phase. NOTE: this means the DRAM cache's tag/metadata
  state is *not* warmed up the way gem5's checkpoint-based 100ms warmup
  warms it up (see `limitations.md`).
- **ORB backpressure only, for now.** gem5 retries (returns `false` from
  `recvTimingReq`) when ORB/CRB/WB are full and the requestor is expected
  to resend. This ChampSim's `CACHE` never inspects `add_rq`/`add_wq`
  return values for the LLC→lower-level path — it pre-checks
  `get_occupancy()==get_size()` before calling `add_rq`/`add_wq` at all
  (`cache.cc:652`, `:319`). So backpressure is implemented by making
  `get_occupancy`/`get_size` honestly report ORB fullness (type 1 and 2
  both map to the shared ORB, matching gem5's single ORB governing both
  reads and writes), not by a return-value contract.

## RESOLVED: per-instance DRAM timing

`tRP`, `tRCD`, `tCAS`, `DRAM_MTPS`, `DRAM_DBUS_RETURN_TIME` used to be
**global** variables shared by every `MEMORY_CONTROLLER` instance
(`dram_controller.cc:4-5`, old version), which would have made it
impossible for `nearMC` (HBM2, 32 GB/s) and `farMC` (DDR4/NVM, 19.2 GB/s)
to run different timing simultaneously, as the paper requires.

**Fix applied**: these five fields are now members of `MEMORY_CONTROLLER`
(`inc/dram_controller.h`), zero-initialized in the constructor, set via a
new `MEMORY_CONTROLLER::set_timing(tRP, tRCD, tCAS, MTPS,
dbus_return_time)` method. `dram_controller.cc`'s method bodies needed no
changes — they already referred to `tRP`/`tCAS`/etc. unqualified, which
now bind to `this->` members instead of globals automatically.
`main.cc` now computes the same values it always did and calls
`set_timing()` on *both* `uncore.DRAM_CACHE_DEVICE` and `uncore.DRAM`
with those identical values — so the full-binary/trace-driven behavior is
provably unchanged (verified: pre- and post-fix trace runs produce the
same instruction/cycle counts up to the CRB-conflict-serialization
difference described in `implementation_status.md`). Actually giving
`nearMC`/`farMC` *different* real values (HBM2 vs DDR4/NVM profiles) is
deliberately deferred — see `next_task.md` — this fix only resolves the
*mechanism*, verified in `tests/test_dcm_skeleton.cc` Test 4 by giving two
fresh controllers deliberately different timing and confirming they
complete at different, independent speeds.

## RESOLVED: ORB/CRB conflict handling

Ported `checkConflictInDramCache` → `checkConflictInORB()`,
`resumeConflictingReq` → `promoteFromCRB()` (called from the shared
`completeRequest()`), and the CRB-full admission check from
`recvTimingReq`. See facts #4–#5 above for the exact behavior ported, and
`tests/test_dcm_skeleton.cc` Tests 5–6 for verification (a request-flow
log is captured for Test 5 in `implementation_status.md`).

## RESOLVED: WB occupancy admission backpressure

Ported the exact remaining piece of gem5's `recvTimingReq` admission
ordering (fact #3): `pktFarMemWrite.size() >= orbMaxSize/2` → retry.
Implemented as `DCM_WB_PRESSURE_THRESHOLD = DCM_ORB_MAX_SIZE/2 = 64`,
checked in `add_rq`/`add_wq` between the conflict check and the ORB-full
check (gem5's exact order), and reflected in `get_occupancy`/`get_size`.
Draining (`drainWB()`) is bounded to one entry per `operate()` cycle
(mirrors gem5's one-send-per-scheduled-event pattern,
`processFarMemWriteEvent`), which is what lets the WB deque genuinely
hold a backlog under pressure. Verified in
`tests/test_dcm_wb_pressure.cc` (5 test blocks: below/at/above threshold,
retry-after-drain, and the interaction with conflict/ORB checks).

## RESOLVED: near/far memory configuration (paper's real values)

`configureNearAsHBM2()`/`configureFarAsDDR4()` (`dram_cache_manager.cc`)
now give `nearMC`/`farMC` the paper's own declared peak bandwidths (32
GB/s / 19.2 GB/s, Table I) via `DRAM_MTPS`, instead of identical
placeholder values. See the parameter derivation table below. Verified
independent in `tests/test_dcm_near_far_config.cc` (exact MT/s values;
changing near-only timing measurably changes hit latency without
touching far; near/far `PACKET_QUEUE`s remain fully separate arrays).

**Real bug found and fixed along the way**: ChampSim's
`DRAM_DBUS_RETURN_TIME` formula truncates `CPU_FREQ/DRAM_MTPS` via
integer division, collapsing every MTPS in [2001,4000] to the same
value at `CPU_FREQ=4000` — meaning near=4000 and far=2400 would have
produced *identical* dbus timing despite the intended bandwidth
difference. Fixed via `computeDbusReturnTime()` (floating-point,
round-to-nearest). See `docs/limitations.md` for the full writeup
(this bug pre-dates this port — ChampSim's own single-DRAM default was
already silently ~20% off).

## RESOLVED: manager↔far-memory link latency (Case Study 3)

`DRAM_CACHE_MANAGER::linkLatencyCycles` (default 0, set via
`setLinkLatency()`) delays dispatch to `farMC` by exactly the configured
number of cycles, applied identically to far-read fetches
(`driveState`'s `DCM_FAR_MEM_READ` case) and far-write write-backs
(`drainWB()`) — both physically cross the same near↔far link in the
paper's model — via a shared `dispatchToFar()` helper and a
`pendingFarDispatches` holding queue drained by `processPendingFarDispatches()`
each `operate()` cycle. `DCM_LINK_LATENCY_CYCLES_{100,500,1000}NS` give
the paper's three evaluated values. Verified in
`tests/test_dcm_link_latency.cc`: completion time scales with configured
latency almost exactly (measured deltas matched expected deltas exactly,
no tolerance needed, in the actual test run); local-only hits are
provably unaffected (never reach `dispatchToFar` at all); the write-back
path is confirmed to be held for the link too.

## RESOLVED: DCM controller frontend/backend latency

**gem5 source of truth** (re-verified directly from
`policy_manager.cc`/`PolicyManager.py` before coding, not assumed from
the paper's flat "20ns" description): `PolicyManager.py` declares two
separate SimObject params, `static_frontend_latency` and
`static_backend_latency`, each defaulting to `"10ns"`. gem5's
`accessAndRespond(pkt, static_latency)` schedules the LLC-visible
response at `curTick() + static_latency + headerDelay + payloadDelay`.
The *value* passed as `static_latency` differs by call site:
`frontendLatency + backendLatency` (single backend term) for a plain
hit/write-ack response; `frontendLatency + backendLatency +
backendLatency` (backend term counted **twice**) specifically for the
response that follows a far-memory fetch (a read miss) — i.e. gem5
charges the backend latency once for the near-memory tag-check
round-trip and once more for the far-fetch round-trip, both funneling
through the same controller backend, while the frontend latency is only
ever charged once per external request.

**ChampSim implementation**: `DRAM_CACHE_MANAGER` gained
`frontendLatencyCycles`/`backendLatencyCycles` fields (defaulting to
`DCM_FRONTEND_LATENCY_CYCLES`/`DCM_BACKEND_LATENCY_CYCLES`, each 10ns
converted via `CPU_FREQ`), settable via `setControllerLatency(frontend,
backend)` for deterministic tests. `completeRequest()` — the single
function all three policies (baseline/BEAR/Oracle) funnel through to
deliver an LLC-visible read response — now takes a
`responseLatencyCycles` parameter and, when nonzero, hands the packet to
`scheduleResponse()` instead of calling `return_data()` immediately.
`scheduleResponse()` enqueues a `DCM_PENDING_RESPONSE{targetCycle, pkt}`
onto a `pendingResponses` deque; `processPendingResponses()` (called
from `operate()`, mirroring the `pendingFarDispatches`/
`processPendingFarDispatches()` pattern used for link latency) drains
entries once `current_core_cycle[cpu] >= targetCycle`. `completeRequest()`
is called with `frontendLatencyCycles + backendLatencyCycles` at the
local tag-check-hit response site, and with `frontendLatencyCycles +
2*backendLatencyCycles` at the far-fetch response site — replicating
gem5's single-vs-double distinction exactly. Writes never produce an
LLC-visible response in this port at all (see fact #2 above the
adaptations table), so `completeRequest()`'s default
`responseLatencyCycles=0` there is inert by construction, not a gap.
ORB/CRB bookkeeping (`ORB.erase`, `delete e`, `promoteFromCRB`) still
happens synchronously inside `completeRequest()` regardless of response
latency, matching gem5's decoupling of the response schedule from the
synchronous request-tracking bookkeeping.

Distinguishing the four latency components (per the task's explicit
requirement):
- **DCM controller latency** (this section): applied once, at the final
  LLC-response boundary only, via `scheduleResponse`/`pendingResponses`.
  Never touches `nearMC`/`farMC` internal timing.
- **Near-memory DRAM latency**: `nearMC`'s own `tRP`/`tRCD`/`tCAS`/dbus
  timing (`MEMORY_CONTROLLER::set_timing()`), unrelated to and unaffected
  by controller latency.
- **Far-memory DRAM latency**: same, but `farMC`'s instance.
- **Far-link latency**: `linkLatencyCycles`, applied at dispatch-to-far
  time via `dispatchToFar()`/`pendingFarDispatches`, entirely upstream
  (in the request pipeline) of where controller latency is applied
  (downstream, at final response). The two mechanisms are structurally
  independent (different deques, different trigger points), which is
  exactly why they compose additively — verified in
  `tests/test_dcm_controller_latency.cc` Test 5: enabling both
  simultaneously produces `withNeither + ctrlContribution +
  linkContribution` exactly, with no double-application either way.

Verified in `tests/test_dcm_controller_latency.cc` (9/9 assertions):
zero-latency behaves exactly as before the feature existed; the paper
default (10ns+10ns=80 cycles @ `CPU_FREQ`) adds exactly that delay to a
read-hit response; a custom value changes the delay accordingly;
read-hit uses the single round trip (80 cycles) while read-miss-clean-
victim and read-miss-dirty-victim both use the double round trip (120
cycles) exactly once; write hit/miss/WB produce no LLC response and are
therefore unaffected by construction; controller latency does not
change local/far DRAM operation counts for any of the three policies;
controller and link latency compose additively.

## RESOLVED: bypassDcache / "No-DRAM-Cache" comparison mode

Full architectural writeup, exact gem5 call sites, request-path
diagrams, and test inventory in `docs/bypass_mode.md`. Summary
gem5-comparison table (Scenario → gem5 behavior → ChampSim behavior →
match/difference → classification):

| Scenario | gem5 behavior | ChampSim behavior | Match/Difference | Class |
|---|---|---|---|---|
| Bypass check placement | `recvTimingReq`'s FIRST line, before any ORB/CRB/classification | `add_rq`/`add_wq`'s bypass check placed immediately after the warmup shortcut (a ChampSim-only concept with no gem5 analog) and before ALL conflict/CRB/WB/ORB admission logic | Match (the warmup shortcut is orthogonal to and does not reorder anything gem5-meaningful) | A |
| Routing on bypass | `farReqPort.sendTimingReq(pkt)` directly | `dispatchToFar()` directly (same helper the normal far-fetch/write-back paths use) | Match | A |
| Response scheduling on bypass | `port.schedTimingResp(pkt, curTick())` — zero added ticks, no controller latency | `scheduleResponse` is not invoked at all on this path; `return_data()` delivers immediately via `bypassOutstandingReads` recognition | Match (verified via dedicated timing test: `ctrl_delta=0` exactly) | A |
| Far-link latency in bypass mode | No `link` concept anywhere inside `policy_manager.cc` — link is external to `PolicyManager`, on the physical `farReqPort` connection, still crossed in bypass mode | `dispatchToFar()` still honors `linkLatencyCycles` in bypass mode (same helper, same reasoning) | Match | A |
| Retry-on-NACK from far memory in bypass mode | `farMemRecvReqRetry`'s bypass branch: `port.sendRetryReq()` | No equivalent (this port has no retry-on-NACK concept in ANY mode — a pre-existing, already-tracked gap, not introduced by bypass mode) | Difference, but pre-existing and orthogonal to bypass mode specifically | B |
| Conflict tracking in bypass mode | None (`checkConflictInDramCache` never called) | None (`checkConflictInORB` never called; `bypassOutstandingReads` is a `multiset`, allowing multiple concurrent same-address bypass reads) | Match | A |
| Flow control / admission backpressure in bypass mode | Implicit in gem5's port/retry protocol (`sendTimingReq` return value, `farMemRecvReqRetry`) | `get_occupancy()`/`get_size()` delegate straight to `farMC`'s own queue capacity | Match in spirit (delegated to far memory's real capacity in both); exact retry protocol differs (see row above) | B |

**A = directly reproducible, B = reproducible with adaptation, C = not
reproducible.** No C-classified differences were found — every
observable behavior gem5's `bypassDcache` produces has either an exact
ChampSim equivalent or a documented, pre-existing adaptation (the
retry-on-nack gap, which affects normal-mode far dispatch identically
and is not specific to bypass mode).

## Paper Table I / Case Study 3 parameters — full derivation

Format: **Paper value → gem5 value → ChampSim value → exact/adapted → reason**

| Parameter | Paper | gem5 | ChampSim | Exact/Adapted | Reason |
|---|---|---|---|---|---|
| Cores | 8 | 8 (implied by target system) | `NUM_CPUS=1` | Adapted (unchanged) | Changing `NUM_CPUS` has a large blast radius (LLC sizing macros are `NUM_CPUS`-scaled, `ooo_cpu` array); out of scope unless asked |
| ORB size | 128 | `orb_max_size="128"` (config script) | `DCM_ORB_MAX_SIZE=128` | **Exact** | Direct 1:1 port |
| CRB size | 32 | `crb_max_size="32"` | `DCM_CRB_MAX_SIZE=32` | **Exact** | Direct 1:1 port |
| WB Buffer size | 64 entries | no hard cap in code (`pktFarMemWrite` is an unbounded `std::deque`); admission throttled via `orbMaxSize/2` | `DCM_WB_MAX_SIZE=64` (structural constant); `DCM_WB_PRESSURE_THRESHOLD=DCM_ORB_MAX_SIZE/2=64` (actual throttle, matches gem5's real mechanism) | **Exact** (both numbers, though for different reasons — see header comment) | gem5 itself approximates "WB full" via `orbMaxSize/2`, not a direct 64-entry check; this port replicates gem5's *actual* mechanism, which happens to numerically coincide with the paper's declared structural size |
| DRAM cache size | 128 MB | `dram_cache_size='128MiB'` | `DCM_DRAM_CACHE_SIZE=128*1024*1024` | **Exact** | Direct port; used for index/tag math only (no real data array, see limitations.md) |
| Block size | 64 B | 64 B (`block_size` param) | `DCM_BLOCK_SIZE=BLOCK_SIZE=64` | **Exact** | Reused ChampSim's existing global |
| Controller frontend/backend latency | 20 ns round-trip | `static_frontend_latency`/`static_backend_latency` (`PolicyManager.py`, 10ns each by default) | `DCM_FRONTEND_LATENCY_CYCLES`/`DCM_BACKEND_LATENCY_CYCLES` (10ns each @ `CPU_FREQ`, default; `setControllerLatency()` to override) | **Exact** (implemented this stage — see dedicated section below) | Two separate 10ns SimObject params in gem5, not one 20ns constant; single-vs-double `backendLatency` application distinguished per response path (see below) |
| Near memory (DRAM cache) technology | HBM2, 32 GB/s peak BW | `HBM_2000_4H_1x64` SimObject (2000 MT/s/pseudo-channel × 2 pseudo-channels × 8B = 32 GB/s) | `configureNearAsHBM2()`: `DCM_NEAR_HBM2_MTPS=4000` (single virtual channel × 8B = 32 GB/s aggregate) | **Bandwidth: exact. Latency (tRP/tRCD/tCAS): adapted** (kept at generic 12.5ns default) | Paper gives only the aggregate bandwidth number explicitly; gem5's real per-technology *latency* is baked into the SimObject, not disclosed as ns numbers in the paper text — see limitations.md for why this is not invented |
| Far memory (backing store) technology | DDR4, 19.2 GB/s peak BW | `DDR4_2400_16x4` SimObject (2400 MT/s) | `configureFarAsDDR4()`: `DCM_FAR_DDR4_MTPS=2400` | **Bandwidth: exact** (2400 MT/s matches the real DDR4-2400 JEDEC speed grade AND gem5's SimObject name exactly). **Latency: adapted**, same reason as near | Same as above |
| Far-memory link latency (Case Study 3) | 100 / 500 / 1000 ns round-trip | explicit `Link` SimObject latency parameter | `DCM_LINK_LATENCY_CYCLES_{100,500,1000}NS`, applied via `setLinkLatency()` | **Exact** | Direct 1:1 port; verified to scale completion time by (very close to) the exact configured cycle count |
| Bus width (near vs far) | not separately stated for width (bandwidth given directly) | HBM2 pseudo-channel: 64-bit; DDR4: wider, model-specific | `DRAM_CHANNEL_WIDTH=8` bytes, a single global macro shared by both | **Adapted (known gap)** | `DRAM_CHANNEL_WIDTH` is not a per-`MEMORY_CONTROLLER` field in this ChampSim tree (pre-existing); only `DRAM_MTPS` differs per instance. The paper's own two numbers (peak bandwidth) are still reproduced exactly by adjusting rate alone — see limitations.md |

## ChampSim PACKET::address convention and DCM normalization

Established by direct source inspection of every layer, not assumed. This
is the contract the DRAM cache manager's index/tag arithmetic depends on.

### What PACKET::address means in each layer

| Layer | Field | Meaning | Evidence |
|---|---|---|---|
| CPU → L1 (data/fetch) | `address` | **cache-line (block) address** | `src/ooo_cpu.cc:1696,2253` — `data_packet.address = physical_address >> LOG2_BLOCK_SIZE` |
| CPU → L1 | `full_addr` | physical **byte** address | same lines — `data_packet.full_addr = physical_address` |
| CPU → TLB | `address` | *page* number (`va >> LOG2_PAGE_SIZE`) | `src/ooo_cpu.cc:1459,1542` — a separate convention, never reaches the DCM |
| CACHE (any level) | `address` | **cache-line address** | `src/cache.cc:1058` — `get_set()` masks `address` DIRECTLY with `(NUM_SET-1)`; `get_way()` compares the whole `address` as the tag. Neither would be correct for a byte address. |
| CACHE → `lower_level` | both | **forwarded unmodified** | `src/cache.cc:661,672` (`add_rq`), `:115,445` (`add_wq`) — the packet is passed by pointer, untouched |
| CACHE writeback | `address` | **cache-line address** | `src/cache.cc:107` — `writeback_packet.address = block[set][way].address`, itself a line address |
| MEMORY_CONTROLLER | `address` | **cache-line address** | `src/dram_controller.cc:634` — `dram_get_channel()` uses `shift = 0`, i.e. there are no byte-offset bits to discard |

**The convention is uniform across the entire hierarchy**:
`PACKET::address == PACKET::full_addr >> LOG2_BLOCK_SIZE`.

### What the DCM expects, and where normalization occurs

The DCM consumes the same convention — it needs **no** shift or divide,
because its input is already a line address. Normalization is therefore a
no-op *by design*, localized entirely to `returnIndexDC()`/`returnTagDC()`
in `src/dram_cache_manager.cc`:

```
incoming PACKET::address   (cache-line address, normalized upstream)
  -> lineAddr = address                    (no shift, no divide)
  -> indexDC  = lineAddr % DCM_NUM_LINES   (direct-mapped)
  -> tagDC    = lineAddr / DCM_NUM_LINES
```

`PACKET::address`'s global meaning is **not** changed. Source inspection
proves that meaning is already correct and consistent in every other
component, so the correct architectural fix was to make the DCM agree with
the existing contract — not to redefine the contract.

**The defect this replaced**: `returnIndexDC()` previously computed
`(address / DCM_BLOCK_SIZE) % numLines`, dividing by the line size a
*second* time. Because `address` was already a line address, 64
consecutive distinct 64-byte lines collapsed onto one index.

### How 64-byte granularity is guaranteed

`indexDC` advances by exactly 1 for each consecutive cache-line address,
so line *L* and line *L+1* — two adjacent 64-byte lines — always occupy
**different** indices. Asserted directly through the real LLC in
`tests/test_dcm_llc_integration.cc` Test 7 (`0x40000000` and
`0x40000040` → indices 0 and 1, two misses, both resident simultaneously).

### How 128 MiB capacity is guaranteed

```
DCM_NUM_LINES = DCM_DRAM_CACHE_SIZE / DCM_BLOCK_SIZE
              = 128 MiB / 64 B = 2,097,152
```

`DCM_NUM_LINES` is simultaneously (a) the modulus of the direct-mapped
index and (b) the size of `tagMetadataStore` — one source of truth, set in
the constructor as `tagMetadataStore.resize(DCM_NUM_LINES)`. Therefore
`DCM_NUM_LINES * DCM_BLOCK_SIZE == DCM_DRAM_CACHE_SIZE == 128 MiB`
holds by construction, and the index can never fall outside the store.
Asserted in `tests/test_dcm_llc_integration.cc` Test 10, which also checks
that line *L* and *L + DCM_NUM_LINES* share an index and differ in tag by
exactly 1.

### Before / after

| | Before (defective) | After (correct) |
|---|---|---|
| index formula | `(address / 64) % 2097152` | `address % 2097152` |
| effective line size | 4096 B | **64 B** |
| effective capacity | 8 GB | **128 MiB** |
| `0x40000000` vs `0x40000040` | same index → false HIT | different indices → two misses |

### Entry-path equivalence

Both DCM entry paths — direct packet injection and real-LLC-generated
packets — use the identical convention, verified in
`tests/test_dcm_llc_integration.cc` Test 9: for the same logical byte
addresses, both produce identical line address, index, tag, hit/miss
classification, resulting metadata, and local/far operation counts. No
per-path normalization exists or is needed.
