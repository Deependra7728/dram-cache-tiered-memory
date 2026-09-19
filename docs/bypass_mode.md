# Bypass Mode (`bypassDcache` / "No-DRAM-Cache" comparison)

## What this is

The paper's Case Study 1 and 2 compare the DRAM cache against a
"No-DRAM-Cache" baseline configuration where the accelerator/host talks
directly to the far/backing memory, with no DRAM cache in the path at
all. gem5's `PolicyManager` implements this as a single boolean SimObject
param, `bypass_dcache` (default `False`), checked at exactly 3 call
sites inside `policy_manager.cc` (re-verified directly from the pulled
`dram_cache_disaggregated` branch source before writing any of this
port's code — not assumed from the paper's description). This document
describes the ChampSim-side port: `DRAM_CACHE_MANAGER::bypassDcache`,
toggled via `setBypassDcache(bool)`.

## gem5 source of truth (verified directly, not assumed)

Three call sites in `policy_manager.cc`, and only three:

```cpp
// recvTimingReq — the FIRST check, before ANY ORB/CRB/classification
// logic runs at all (policy_manager.cc:157-162):
bool PolicyManager::recvTimingReq(PacketPtr pkt)
{
    if (bypassDcache) {
        return farReqPort.sendTimingReq(pkt);
    }
    ... // (everything below this is skipped entirely in bypass mode)
}

// farMemRecvTimingResp — the response path (policy_manager.cc:560-566):
bool PolicyManager::farMemRecvTimingResp(PacketPtr pkt)
{
    if (bypassDcache) {
        port.schedTimingResp(pkt, curTick());
        return true;
    }
    ...
}

// farMemRecvReqRetry — the far-side retry path (policy_manager.cc:641-647):
void PolicyManager::farMemRecvReqRetry()
{
    if (bypassDcache) {
        port.sendRetryReq();
        return;
    }
    ...
}
```

Two facts fall directly out of this, both load-bearing for the port
below:

1. **`recvTimingReq`'s bypass check is the very first line of the
   function** — it runs before ORB/CRB conflict checks, before write-queue
   merge checks, before `checkConflictInDramCache`, before ANY
   classification. Nothing DCM-side ever sees a bypass request.
2. **`farMemRecvTimingResp`'s bypass branch schedules the response at
   `curTick()`, i.e. with ZERO added ticks.** The normal path's
   `accessAndRespond()` (which is what applies `frontendLatency +
   backendLatency[+ backendLatency]`, see
   `gem5_to_champsim_mapping.md`) is never called on the bypass path at
   all. **gem5 does NOT apply controller frontend/backend latency to a
   bypass response.** This was checked before coding, per the task's
   explicit "check gem5 before deciding" instruction — it would have
   been wrong to assume the controller latency stage's work simply
   layers on top of bypass mode unchanged.

There is no mention of "link latency" anywhere inside
`policy_manager.cc`/`.hh`/`PolicyManager.py` — the far-memory link
modeled in this port (`linkLatencyCycles`, Case Study 3) corresponds to
an external SimObject sitting on the physical connection between
`PolicyManager`'s `farReqPort` and far memory, not to any
`PolicyManager`-internal logic. Bypass mode's `farReqPort.sendTimingReq(pkt)`
call crosses that exact same port/connection as the normal far-fetch
path — so **the far-link latency is NOT bypassed**: it is a property of
the wire, not of `PolicyManager`'s own bookkeeping, and bypass mode only
skips the latter.

## Architectural decision

**`DRAM_CACHE_MANAGER` owns the bypass decision**, exactly mirroring
gem5 (`bypassDcache` lives inside `PolicyManager` itself, not as an
external SimObject wiring choice). This was chosen over the alternative
the final audit had flagged as already technically possible — manually
rewiring `uncore.LLC.lower_level = &uncore.DRAM` instead of
`&uncore.DCM` in `main.cc` — because:

- The task explicitly rules out "implement bypass by merely changing a
  test or manually editing `main.cc` each time" in favor of "a proper
  configurable mechanism."
- A manual rewiring bypasses `DRAM_CACHE_MANAGER` from OUTSIDE it, which
  cannot be toggled at run time, is not visible in any single place a
  reader would look for it, and is not what gem5 itself does (gem5's own
  `bypassDcache` is an in-object flag, not an external topology change).
- Owning the decision inside `DRAM_CACHE_MANAGER` lets it reuse its own
  already-built `dispatchToFar()`/far-link-latency machinery for free
  (see below), which an external rewiring could not do without
  duplicating that logic elsewhere.

**Reused, not duplicated:** bypass mode does not stand up a second
memory controller or a parallel dispatch path. It forwards straight to
the EXISTING `farMC` (the same `MEMORY_CONTROLLER` instance normal mode
already uses for the backing store) via the EXISTING `dispatchToFar()`
helper (originally built for Stage 4's far-link latency), and delegates
flow control (`get_occupancy`/`get_size`) straight to `farMC`'s own
queue occupancy rather than inventing a bypass-specific capacity model.
No new `MEMORY_CONTROLLER`, no new dispatch helper, no new admission
queue.

## Request path

**Normal mode** (`bypassDcache == false`, the default — unchanged from
every prior stage):

```
LLC
 → DRAM_CACHE_MANAGER::add_rq/add_wq
   → conflict-by-index check (ORB/CRB)
   → WB-pressure / ORB-full admission checks
   → admitRequest() → classifyAndInstall() (tag/metadata lookup + update)
   → chooseInitialState() (baseline/BEAR/Oracle policy decision)
   → driveState() → local tag-check read and/or far fetch/write-back
     (dispatchToFar(), far-link latency applies here)
 → completeRequest() (controller frontend/backend latency applies here,
   single or double round-trip per gem5's exact distinction)
 → LLC
```

**Bypass mode** (`bypassDcache == true`):

```
LLC
 → DRAM_CACHE_MANAGER::add_rq/add_wq
   → (bypassDcache check — the FIRST thing checked, mirroring gem5's
      recvTimingReq exactly)
   → dispatchToFar() directly (far-link latency STILL applies — same
     helper, same physical-link reasoning as gem5)
   → farMC (the SAME MEMORY_CONTROLLER instance normal mode uses)
 → return_data() recognizes the response via bypassOutstandingReads
   (no ORB entry exists to look up) and delivers it to the LLC
   IMMEDIATELY — no controller frontend/backend latency, matching
   gem5's schedTimingResp(pkt, curTick())
 → LLC
```

Everything between "conflict-by-index check" and "driveState" in the
normal-mode path — the entire `DRAM_CACHE_MANAGER`-owned request
lifecycle — is skipped in bypass mode. No ORB entry, no CRB entry, no
`classifyAndInstall()` call (so `tagMetadataStore` is never read or
written), no policy decision, no cache fill, no dirty-victim
identification or write-back, no near-memory operation at all.

## Implementation

- `DRAM_CACHE_MANAGER::bypassDcache` (bool, default `false`),
  `setBypassDcache(bool)`.
- `add_rq()`/`add_wq()`: the bypass check is placed immediately after
  the pre-existing warmup shortcut (which applies identically in both
  modes — ChampSim's warmup concept has no gem5 analog and both modes
  must behave the same way during it) and immediately BEFORE the
  conflict/CRB/WB/ORB admission logic — mirroring gem5's `recvTimingReq`
  bypass check being its first statement. On a read, the address is
  recorded in `bypassOutstandingReads` (a `std::multiset<uint64_t>`, not
  a plain set — bypass mode performs no conflict tracking of its own, so
  multiple outstanding bypass reads to the SAME address are valid and
  each must be accounted for independently, matching gem5's total
  absence of `checkConflictInDramCache` on this path). The packet's
  `event_cycle` is stamped to "now" before dispatch (see "a real
  ChampSim-specific bug found and fixed" below) and handed to the
  EXISTING `dispatchToFar()` helper.
- `return_data()`: checks `bypassOutstandingReads` for the incoming
  packet's address BEFORE the normal ORB lookup. If found, the address
  is erased from the set and the packet is delivered straight to
  `upper_level_icache`/`upper_level_dcache` — no controller latency, no
  ORB/CRB bookkeeping (there is none to do; a bypass request never
  created any). Writes never reach `return_data()` at all in this port,
  in ANY mode (`MEMORY_CONTROLLER` only calls `return_data()` for RQ
  completions, never WQ — verified in `dram_controller.cc` before
  relying on it), so no write-side tracking is needed.
- `get_occupancy()`/`get_size()`: delegate straight to `farMC`'s own
  `get_occupancy()`/`get_size()` when `bypassDcache` is set, so the
  LLC's pre-admission flow-control check
  (`CACHE::handle_read/handle_writeback`) reflects farMC's REAL queue
  capacity instead of this manager's own (irrelevant, in bypass mode)
  ORB/CRB/WB capacity.
- `DCM_STATS` gained three counters: `bypassReads`, `bypassWrites`,
  `bypassCompletedReads` — incremented ONLY on the bypass path, so a
  bypass-mode instance's activity is fully distinguishable from a
  normal-mode instance's, without touching any pre-existing counter
  (`localReads`/`localWrites`/`farReads`/`farWrites`/`cacheFills`/
  `numWrBacks`/etc. all stay exactly 0 for bypass traffic — verified in
  `tests/test_dcm_bypass.cc`'s normal-vs-bypass comparison).

### A real ChampSim-specific bug found and fixed while implementing this

`dispatchToFar()`'s existing callers (`driveState()`'s
`DCM_FAR_MEM_READ`/`DCM_LOC_MEM_WRITE` cases) always stamp
`p.event_cycle = nowCycle` immediately before dispatch. `PACKET`'s own
default constructor (`inc/block.h`) leaves `event_cycle` at
`UINT64_MAX`, and `MEMORY_CONTROLLER`'s scheduling never picks up a
request left at that value. In the normal-mode path, `admitRequest()`
is what would eventually lead to this stamping being done downstream;
bypass mode skips `admitRequest()` entirely (by design — see "Request
path" above) and so must do the equivalent stamping itself, immediately
before calling `dispatchToFar()`. Without this, a bypass request would
be silently accepted (`add_rq`/`add_wq` return normally, the packet is
inserted into `farMC`'s queue with `occupancy` incrementing) but would
NEVER be scheduled or completed — a real bug caught by
`tests/test_dcm_bypass.cc` Test 1 failing outright (0 responses,
timeout) before this fix, not a test-methodology artifact. Fixed by
stamping `packet->event_cycle = current_core_cycle[packet->cpu]` in
both `add_rq`'s and `add_wq`'s bypass branches, right before the
`dispatchToFar()` call.

## Timing

| Component | Applies in bypass mode? | Why |
|---|---|---|
| Far-memory DRAM latency (`farMC`'s own tRP/tRCD/tCAS/dbus timing) | **Yes** | The request still physically reaches and is serviced by real far memory — bypass mode does not change what far memory IS, only what sits in front of it |
| Far-link latency (`linkLatencyCycles`, Case Study 3) | **Yes** | Models the physical wire between the manager and far memory, external to `PolicyManager`'s own logic (verified: no "link" concept anywhere inside `policy_manager.cc`) — bypass mode's `farReqPort.sendTimingReq` crosses the exact same wire as the normal path |
| DCM controller frontend/backend latency | **No** | Verified directly from gem5's `farMemRecvTimingResp` bypass branch: `port.schedTimingResp(pkt, curTick())` — zero added ticks. `accessAndRespond()` (the function that applies this latency on the normal path) is never invoked in bypass mode at all |
| Near-memory DRAM latency (`nearMC`) | **N/A** | Never touched in bypass mode — there is no local operation to time |

Verified in `tests/test_dcm_bypass.cc`'s dedicated timing test: a bypass
read's completion time increases by (approximately, matching this
session's existing ±20% link-latency-jitter tolerance convention from
`tests/test_dcm_link_latency.cc`) the configured link latency when one
is set, and by EXACTLY 0 cycles when the DCM controller latency is left
at its (nonzero) default — proving controller latency genuinely does
not apply here, not merely that it wasn't tested.

## Statistics

`DCM_STATS::bypassReads`/`bypassWrites`/`bypassCompletedReads` are the
only counters touched by bypass traffic. Every pre-existing counter
(`localReads`, `localWrites`, `farReads`, `farWrites`, `numTotHits`,
`numTotMisses`, `cacheFills`, `numWrBacks`, `wbInsertions`, `crbInserts`,
etc.) stays at exactly 0 for a bypass-only run and is completely
unaffected by bypass mode being available at all when disabled (the
default) — verified in `tests/test_dcm_bypass.cc`'s statistics-isolation
test and in the normal-vs-bypass comparison test.

## Paper connection

This directly implements the mechanism needed for the paper's Case
Study 1 and 2 "No-DRAM-Cache" comparison configuration (Figures 5/10 in
the paper; `docs/final_paper_coverage_audit.md` Section 1.15, "MISSING
(newly found this audit)" before this stage). Toggling
`setBypassDcache(true)` on the same `DRAM_CACHE_MANAGER` instance used
for a normal run reproduces the paper's "route straight to backing
memory, no DRAM cache in the path" comparison arm, using the SAME
`farMC` configuration (`configureFarAsDDR4()` etc.) so the two arms of
the comparison are apples-to-apples.

**This alone does not run Case Study 1/2** — it provides the mechanism
the case studies need, but running the actual GAPBS/NPB-style
comparisons the paper describes is a separate, explicitly out-of-scope
item (`docs/next_task.md` item 4), requiring real workload traces
exercising the DRAM cache meaningfully. Not attempted this stage, per
the task's explicit instruction to implement bypass mode only.

## Limitations

- gem5's `farMemRecvReqRetry` bypass branch (`port.sendRetryReq()`) has
  no ChampSim equivalent. This WAS a general limitation shared with
  normal-mode WB write-backs, but the normal-mode side has since been
  FIXED (`docs/wb_retry_audit.md`'s "RESOLUTION" section):
  `dispatchToFar()` now checks `farMC`'s real capacity before dispatch
  and reports failure via a `bool` return, letting `drainWB()` retain a
  blocked WB entry for retry. Bypass mode calls the SAME
  `dispatchToFar()`, so a bypass request no longer risks tripping
  `MEMORY_CONTROLLER`'s internal silent-drop bug directly — but bypass
  mode has no `WB`-like holding structure to retain a rejected packet
  in, so a bypass write that hits a momentarily-full `farMC` is still
  silently dropped on a `false` return, same as before. This remains a
  real, still-open, bypass-mode-specific gap, intentionally not
  addressed by the WB-retry fix (out of that task's explicit scope).
- Bypass mode's flow control is delegated to `farMC`'s real queue
  capacity (`DRAM_RQ_SIZE`/`DRAM_WQ_SIZE`), which is a materially
  different (and more accurate, not less) capacity model than
  normal mode's ORB/CRB/WB-pressure-derived numbers — this is
  intentional (see "Architectural decision" above), not an
  approximation to be reconciled.
- `add_pq` remains a no-op in both modes, matching the pre-existing
  ChampSim behavior this port never changes (see `limitations.md`).

## Tests

`tests/test_dcm_bypass.cc`, 15 test blocks (14 required by this stage's
task plus a controller/link-latency-behavior timing test and a
statistics-isolation test), all passing:

1. Bypass read completes and reaches the LLC.
2. Bypass write completes (reaches `farMC`'s WQ; no LLC callback, as for
   every write in this port).
3. Multiple reads all complete independently.
4. Multiple writes all complete independently.
5. Mixed read/write traffic.
6. Repeated access to the same address (no DCM-side caching at all).
7. Addresses that would share a DRAM-cache index in normal mode do NOT
   conflict in bypass mode (no CRB, no ORB — verified both stay empty).
8. Write then read the same address never touches DCM dirty-eviction
   machinery (`WB` stays empty, `numWrBacks` stays 0).
9. Response/completion correctness (right addresses, right count).
10–13. No local operation, no metadata modification, no ORB/CRB entry,
   no WB entry — checked together across a mixed workload.
14. Far memory receives exactly the expected number of requests
    (`farMC`'s own RQ/WQ completion counts); `nearMC` sees nothing at
    all.
- Normal-vs-bypass comparison for the identical workload: normal mode
  shows real DCM activity, bypass mode shows exactly zero.
- Timing: far-link latency composes with bypass mode (~tolerance, per
  the established convention); controller latency contributes exactly 0.
- Statistics isolation: bypass counters stay 0 on a normal-mode instance
  that never enables bypass; normal-mode counters are unaffected by
  bypass mode merely existing.

Full regression: all 9 pre-existing `tests/test_dcm_*.cc` suites pass
UNCHANGED (no expected-result modification needed anywhere this stage —
unlike the controller-latency stage, `bypassDcache` defaults to `false`
and every existing code path is byte-for-byte untouched when it is).
Full-binary build (`make clean && make -j4`) succeeds, and a smoke-test
run (`dpc3_traces/403.gcc-16B.champsimtrace.xz`, 100K warmup / 200K sim
instructions) produces output BYTE-IDENTICAL to the pre-bypass build,
confirming normal mode is completely unaffected by this stage.
