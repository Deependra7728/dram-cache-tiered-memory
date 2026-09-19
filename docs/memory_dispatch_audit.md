# Memory Dispatch Audit — All DCM → Memory Controller Paths

Audit and fix, per explicit instruction, of every path where
`DRAM_CACHE_MANAGER` sends a request to `nearMC` or `farMC`, following
the prior stage's WB→far-write fix
(`docs/wb_retry_audit.md`). Sources re-read for this audit: the actual
current `DRAM_CACHE_MANAGER`/`MEMORY_CONTROLLER` source
(`inc/dram_cache_manager.h`, `src/dram_cache_manager.cc`,
`inc/dram_controller.h`, `src/dram_controller.cc`), the bypass path
(`docs/bypass_mode.md`), the WB path (`docs/wb_retry_audit.md`), and the
pending-dispatch path (`pendingFarDispatches`/`processPendingFarDispatches()`).
gem5's `policy_manager.cc`/`.hh` (already pulled in prior stages) used
only as behavioral reference, per instruction — not re-fetched this
stage since no new gem5 semantic question arose beyond what the WB
audit already established (the retry principle is the same for every
path: retain until confirmed accepted).

## Every DCM → memory dispatch call site, before this stage's fix

| # | Path | Call site | Destination | Protected before this stage? |
|---|---|---|---|---|
| 1 | Near-memory tag-check READ | `driveState()`, `DCM_LOC_MEM_READ` case | `nearMC->add_rq()` | **No** — bare direct call |
| 2 | Near-memory direct WRITE | `driveState()`, `DCM_LOC_MEM_WRITE` case | `nearMC->add_wq()` | **No** — bare direct call |
| 3 | Near-memory cache-fill WRITE | `return_data()`, `DCM_WAITING_FAR_MEM_READ_RESP` branch | `nearMC->add_wq()` | **No** — bare direct call |
| 4 | Far-memory demand READ | `driveState()`, `DCM_FAR_MEM_READ` case | `dispatchToFar()` | **Partially** — `dispatchToFar()` itself has an occupancy pre-check, but its `bool` return was **ignored** by this call site |
| 5 | Far-memory dirty WRITE-BACK | `drainWB()` | `dispatchToFar()` | **Yes** — fixed in the prior stage (`docs/wb_retry_audit.md`); `drainWB()` correctly uses the `bool` return and retains `WB.front()` on failure |
| 6 | Bypass-mode READ | `add_rq()`, `bypassDcache` branch | `dispatchToFar()` | **Partially** — same as #4: pre-check exists inside `dispatchToFar()`, but the return was ignored |
| 7 | Bypass-mode WRITE | `add_wq()`, `bypassDcache` branch | `dispatchToFar()` | **Partially** — same as #4/#6 |

(Note: the task lists 6 paths; this audit found 7 call sites, since
"near-memory cache-fill WRITE" and "near-memory direct WRITE" are two
textually distinct call sites with different triggers, both mapping to
the task's paths 1–2 conceptually, plus the already-fixed WB write-back
counted for completeness.)

## Required table: path → destination → queue → can fill? → behavior → risk → fix → test

| Path | Destination | Queue | Can fill? | Current behavior (before fix) | Risk | Required fix | Test |
|---|---|---|---|---|---|---|---|
| 1. Near-memory tag-check READ | `nearMC` | RQ (`queue_type=1`) | Yes — `DRAM_RQ_SIZE=64`; reachable when many admissions dispatch their tag-check read concurrently (confirmed reachable in practice — see `docs/wb_retry_audit.md`'s stress-test tuning notes, where this exact path was incidentally triggered) | Bare `nearMC->add_rq(&p)`; `MEMORY_CONTROLLER::add_rq()` has no bounds check and silently discards the packet if RQ is full, with the SAME return value as success | **Confirmed real**: a silently-dropped tag-check read leaves its ORB entry `DCM_WAITING_LOC_MEM_READ_RESP` forever — a permanently stuck ORB entry, consuming an ORB slot and a DRAM-cache index conflict-lock indefinitely | Route through `dispatchToNear()`, which checks `nearMC->get_occupancy()`/`get_size()` before calling `add_rq()`, and on failure queues into `pendingNearDispatches` (retried every `operate()` cycle by `processPendingNearDispatches()`, FIFO) | `tests/test_dcm_memory_dispatch.cc` Test 1 |
| 2. Near-memory direct WRITE | `nearMC` | WQ (`queue_type=2`) | Yes — `DRAM_WQ_SIZE=64` | Bare `nearMC->add_wq(&p)`, immediately followed by `completeRequest(e)` (fire-and-forget; no callback ever awaited for this write, matching this port's write contract) | Same underlying `MEMORY_CONTROLLER::add_wq()` silent-drop bug — the WRITE'S DATA never reaches `nearMC` at all if dropped, silently corrupting the simulated DRAM-cache content (a real functional/data-integrity bug, not just a stuck-request bug, since nothing tracks this write's completion at all) | Route through `dispatchToNear()`, same mechanism as path 1 (the request's own ORB entry has ALREADY completed by the time this dispatches — see "NEAR-MEMORY FILL WRITE" design note below — so retention lives entirely in `pendingNearDispatches`, not in the ORB) | `tests/test_dcm_memory_dispatch.cc` Test 2 |
| 3. Near-memory cache-fill WRITE | `nearMC` | WQ (`queue_type=2`) | Yes — `DRAM_WQ_SIZE=64`; shares the SAME queue as path 2, so pressure from one can affect the other | Bare `nearMC->add_wq(&fillPkt)`, issued AFTER the LLC response already went out (`completeRequest()` already called) — deliberately off the critical response path per the paper's respond-before-fill design | Same silent-drop risk as path 2, with an additional subtlety: this write installs the just-fetched line's data into the DRAM cache to keep tag-metadata (already updated eagerly at admission, `classifyAndInstall()`) consistent with what is physically present. A dropped fill leaves the tag store claiming a line is resident when its physical data was never actually written — a metadata/data desync, strictly worse than an ordinary dropped write | Route through `dispatchToNear()`, same as paths 1–2. Explicitly verified: the LLC response is NOT delayed by this change (it already went out via `completeRequest()` before this dispatch is even attempted; `dispatchToNear()`'s retry, if needed, happens entirely after and independently of that) | `tests/test_dcm_memory_dispatch.cc` Test 3 |
| 4. Far-memory demand READ | `farMC` | RQ (`queue_type=1`) | Yes — `DRAM_RQ_SIZE=64` | `dispatchToFar(p, false)` called but its `bool` return **ignored**; `dispatchToFar()`'s own occupancy pre-check means the packet is never actually handed to the buggy `add_rq()` when full, but on failure NOTHING retries it — the read is decided (`e->state=DCM_WAITING_FAR_MEM_READ_RESP`, stats already incremented) but never actually sent | **Confirmed real, distinct from paths 1-3**: unlike the near paths, this is not exposed to `MEMORY_CONTROLLER`'s internal silent-drop bug directly (thanks to `dispatchToFar()`'s existing pre-check) — but the OUTCOME is identical: a permanently stuck ORB entry, since the read was correctly *recognized* as undispatchable but nothing ever retries it | Route through `dispatchToFarGuaranteed()`, which wraps `dispatchToFar()` and queues a failed immediate attempt into `pendingFarDispatches` (retried by the EXISTING `processPendingFarDispatches()` loop, ready immediately, same FIFO discipline already proven correct for WB) | `tests/test_dcm_memory_dispatch.cc` Test 4 |
| 5. Far-memory dirty WRITE-BACK | `farMC` | WQ (`queue_type=2`) | Yes — `DRAM_WQ_SIZE=64` | **Already fixed** (`docs/wb_retry_audit.md`): `drainWB()` checks `dispatchToFar()`'s `bool` return and retains `WB.front()` on failure | None remaining | No change this stage | `tests/test_dcm_wb_retry.cc` (existing, unchanged) |
| 6. Bypass-mode READ | `farMC` | RQ (`queue_type=1`) | Yes — `DRAM_RQ_SIZE=64` | `dispatchToFar(*packet, false)` called, return **ignored**; on failure the address was already inserted into `bypassOutstandingReads` but the read is never actually sent, so `return_data()` will never see a matching completion — the bypass read vanishes (with a permanently "leaked" entry left in `bypassOutstandingReads` besides) | **Confirmed real**: silently lost bypass read PLUS a leaked tracking-set entry (a second-order accounting bug: since `bypassOutstandingReads` is a plain address multiset with no per-request identity, the leaked entry does not misroute any actual response DATA, but it silently over-counts "outstanding" by one forever, and will eventually be consumed by `erase()` on some LATER, unrelated response to the same address, permanently absorbing one legitimate accounting slot) | Route through `dispatchToFarGuaranteed()`, same mechanism as path 4 — the `bypassOutstandingReads` insertion (which happens BEFORE the dispatch attempt) is unaffected and remains correct once the read is actually, eventually sent | `tests/test_dcm_memory_dispatch.cc` Test 5 |
| 7. Bypass-mode WRITE | `farMC` | WQ (`queue_type=2`) | Yes — `DRAM_WQ_SIZE=64` | `dispatchToFar(*packet, true)` called, return **ignored**; on failure the write is simply dropped (already flagged as a known, open gap in `docs/bypass_mode.md`/`docs/wb_retry_audit.md`'s "Remaining gaps" from the prior stage) | **Confirmed real** (already documented, now fixed): silently lost bypass write, with no holding structure of any kind for it | Route through `dispatchToFarGuaranteed()`, same mechanism as paths 4/6 — this closes the exact gap the prior stage's documentation flagged as open | `tests/test_dcm_memory_dispatch.cc` Test 6 |

## Answers to the 8 questions, per path

**1. Near-memory tag-check READ**
1. Can destination queue become full? Yes (`DRAM_RQ_SIZE=64`).
2. `add_rq()` return? Always `-1`, identical for success and silent failure.
3. Silently discarded? Yes, confirmed (`MEMORY_CONTROLLER::add_rq()` has no bounds check).
4. Did DCM retain on rejection (before fix)? No.
5. Retry required? Yes.
6. Retry already implemented (before this stage)? No.
7. Does gem5 require retention? Yes — gem5's `locMemRead` path uses the identical `sendTimingReq()`/`retryLocMemRead`/`locMemRecvReqRetry()` pattern as every other gem5 dispatch (verified in prior stages' pulled source), so gem5 retains here too.
8. Would fixing this change any previously-validated behavior? No — verified by full regression (all 11 pre-existing suites pass unchanged); this path never reaches capacity in any existing test's traffic volume.

**2. Near-memory direct WRITE**
1–3. Same as path 1 (WQ instead of RQ, same underlying bug).
4. Retained before fix? No.
5. Retry required? Yes.
6. Implemented before? No.
7. gem5 requires retention? Yes — gem5's `locMemWrite`/`retryLocMemWrite` uses the identical pattern.
8. Behavior change? No — regression confirms unchanged.

**3. Near-memory cache-fill WRITE**
1–3. Same underlying WQ exposure as path 2.
4. Retained before fix? No.
5. Retry required? Yes.
6. Implemented before? No.
7. gem5 requires retention? Yes — gem5's fill write goes through the same `locMemWrite` retry-capable path (this port's fill write is architecturally the closest analog to gem5's own cache-line-install write, dispatched via the identical local-memory port).
8. Behavior change? No, with one explicit verification: the LLC response's timing is UNCHANGED (it is issued via `completeRequest()` before `dispatchToNear()` is ever called for the fill — see Test 3's explicit assertion).

**4. Far-memory demand READ**
1. Can fill? Yes.
2. `dispatchToFar()`'s return? A real, correct `bool` — but ignored by this call site before the fix.
3. Silently discarded? No (pre-check prevents ever calling the buggy `add_rq()`) — but functionally equivalent to a drop, since nothing retried it.
4. Retained before fix? No (the packet was simply never sent, and nothing tracked that).
5. Retry required? Yes.
6. Implemented before? No (mechanism existed in `dispatchToFar()`; this call site didn't use it).
7. gem5 requires retention? Yes — `farMemRead`/`retryFarMemRead` is gem5's own explicit retry pattern (`policy_manager.cc`, pulled in the WB audit stage).
8. Behavior change? No — regression confirms unchanged.

**5. Far-memory dirty WRITE-BACK** — already fixed and verified in the prior stage; no new work this stage.

**6. Bypass-mode READ**
1–3. Same as path 4 (shares `dispatchToFar()`).
4. Retained before fix? No, AND a leaked `bypassOutstandingReads` entry compounded the bug (see table above).
5. Retry required? Yes.
6. Implemented before? No.
7. gem5 requires retention? Yes — gem5's bypass path forwards via the SAME `farReqPort.sendTimingReq()`/retry protocol as every other far dispatch (`policy_manager.cc:160-162`, re-verified in the bypass-mode stage); bypass does not exempt itself from gem5's retry discipline, only from `PolicyManager`'s OWN internal ORB/CRB bookkeeping.
8. Behavior change? No — `tests/test_dcm_bypass.cc` (15 blocks) passes unchanged.

**7. Bypass-mode WRITE**
1–3. Same as path 6.
4. Retained before fix? No — this was the ONE path with no fix at all (not even a partial pre-check helped, since the return was ignored and there was no holding structure).
5. Retry required? Yes.
6. Implemented before? No.
7. gem5 requires retention? Yes, same reasoning as path 6.
8. Behavior change? No — regression confirms unchanged.

## Design: the reusable mechanism actually built

Per the explicit design rule (prefer a reusable mechanism; do not
over-generalize where read/write semantics differ), three layers:

1. **`trySend(MEMORY_CONTROLLER *mc, PACKET pkt, bool isWrite)`** — the
   single shared low-level primitive. Checks `mc->get_occupancy()` vs
   `get_size()` for the correct queue, stamps `event_cycle` to "now" if
   there is room, and calls `add_wq()`/`add_rq()`. Returns whether the
   send happened. This is the ONE place in the whole file that
   ultimately decides "is there room right now" — every dispatch path
   (existing and new) funnels through it eventually.
2. **`dispatchToFar()`** (pre-existing, UNCHANGED contract) — still
   `bool`-returning, still "caller decides" on failure. Its immediate
   branch now calls `trySend()` internally (a pure refactor — behavior
   identical, since the same occupancy check and send happen either
   way). Kept as the direct primitive for `drainWB()`, the ONE existing
   caller that manages its own front-of-queue retry (the `WB` deque)
   and must NOT be changed, since it is already correct and tested.
3. **`dispatchToFarGuaranteed()` / `dispatchToNear()`** (both NEW) —
   the "reusable mechanism" the design rule asks for, one for each
   destination, since far dispatch has a link-latency concept near
   dispatch does not (the "do NOT over-generalize" boundary). Both
   GUARANTEE eventual delivery from the caller's perspective (no bool
   return, no caller-side bookkeeping needed): on an immediate failure,
   they queue into a retry deque (`pendingFarDispatches`, reused, or
   the new `pendingNearDispatches`) that is drained every `operate()`
   cycle, FIFO, stopping at the first still-blocked entry to preserve
   order — the exact discipline already proven correct for WB
   (`docs/wb_retry_audit.md`).

This distinguishes exactly the four states the design rule names:
**generated** (the dispatch decision — stats incremented, state set,
`dispatchTo*` called), **accepted by destination** (`trySend()` returns
`true`, either on the first attempt or a later retry), **pending
because destination is full** (sitting in `pendingFarDispatches`/
`pendingNearDispatches`), **finally accepted** (popped from the pending
queue on a later cycle). FIFO order is preserved by construction (both
are `std::deque`s, always operated on at the front, with `break` not
`continue` on a blocked entry). Request/parent-ID correctness is
preserved because packets are copied whole into the pending queues
(address, cpu, type all intact) and the ORB lookup that eventually
resolves them (`return_data()`'s `ORB.find(packet->address)`) is
untouched by any of this — the ORB entry's `state` field is already set
correctly BEFORE the dispatch attempt, so it remains correct
identically whether the underlying send happens immediately or after
several retries.

## NEAR-MEMORY READ (task's explicit callout)

Verified: `chooseInitialState()`/`driveState()` set
`e->state = DCM_WAITING_LOC_MEM_READ_RESP` BEFORE calling
`dispatchToNear()`. This means the ORB entry's state is correct — "this
request is waiting for its tag-check read" — from the very moment the
dispatch is DECIDED, regardless of whether the underlying `add_rq()`
call happens this cycle or several cycles later after retrying. Nothing
else touches or inspects this ORB entry while it is `WAITING_LOC_MEM_READ_RESP`
except `return_data()`, which can only fire once the real
`MEMORY_CONTROLLER::add_rq()` call has actually happened and nearMC has
actually completed it — so there is no window where the ORB state is
"wrong" while the operation is queued for retry. `checkConflictInORB()`
also continues to correctly treat this index as occupied (it checks
`validEntry && indexDC == indexDC`, both already set at admission,
unaffected by dispatch timing), so a conflicting request still
correctly queues in the CRB rather than racing ahead.

## NEAR-MEMORY FILL WRITE (task's explicit callout)

Verified via direct trace: `completeRequest(e, ...)` (which sends the
LLC response) is called and returns BEFORE `dispatchToNear(fillPkt,
true)` is ever invoked — the response and the fill dispatch are
sequential statements in `return_data()`'s
`DCM_WAITING_FAR_MEM_READ_RESP` branch, with the response first. This
means:
- The LLC response is NEVER delayed by fill-write capacity pressure —
  it has already been sent (and, if controller latency is nonzero,
  already scheduled via `scheduleResponse()`) by the time
  `dispatchToNear()` is even called.
- The fill itself is now guaranteed to eventually reach `nearMC` (never
  lost), via the same retry mechanism as every other near dispatch.
- Metadata consistency: `classifyAndInstall()` already updated
  `tagMetadataStore` eagerly at ADMISSION time (matching gem5's own
  eager-update ordering, established in earlier stages) — the fill
  write's job is only to make the near-memory DEVICE's simulated
  timing/occupancy reflect that a write happened, not to update
  metadata a second time. A delayed (retried) fill write does not
  desync metadata further than a delayed one already might in the
  pre-fix code, since metadata was never gated on the fill write's
  actual dispatch timing in the first place — only on GUARANTEEING the
  fill write is never silently lost, which this fix achieves.

## FAR READ (task's explicit callout)

Traced gem5 behavior (re-confirmed against prior stages' pulled
source): `farMemRead`/`retryFarMemRead` follows the IDENTICAL
`sendTimingReq()`-returns-`false` → retain → `farMemRecvReqRetry()` →
retry pattern as `farMemWrite` (the WB path, already fixed) — there is
no gem5 basis for treating far reads differently from far writes here.
Fire-and-forget was NOT safe (confirmed: an ignored `dispatchToFar()`
failure produces a permanently stuck ORB entry, functionally
equivalent to a dropped request from the requestor's perspective, even
though the underlying `MEMORY_CONTROLLER` primitive itself was never
actually asked to accept a doomed packet). Fixed via
`dispatchToFarGuaranteed()`.

## BYPASS WRITE (task's explicit callout)

This port's write contract has NO response callback in ANY mode
(`MEMORY_CONTROLLER` never calls `return_data()` for a WQ completion —
verified directly, `dram_controller.cc`). This means a rejected bypass
write cannot be "retained" the way a READ can be retained via an
ORB-entry-style state machine (there is no ORB entry for bypass
traffic at all, by design — see `docs/bypass_mode.md`). The retention
this fix provides is instead structural: the packet itself (a full,
self-contained copy) is held in `pendingFarDispatches` until
`trySend()` succeeds, entirely independent of any ORB/callback
machinery — this is exactly why `dispatchToFarGuaranteed()`'s queuing
mechanism does not depend on read-vs-write distinctions at the
retention layer (`DCM_PENDING_FAR_DISPATCH` already carries `isWrite`
and dispatches accordingly on release, with no response expected either
way). ChampSim's existing write semantics (fire-and-forget, no
callback) are completely preserved — the only change is that the
SENDING of the write is now guaranteed rather than best-effort.

## Files changed

- `inc/dram_cache_manager.h`: new `DCM_PENDING_NEAR_DISPATCH` struct;
  new `pendingNearDispatches` deque; new `nearDispatchRetries`/
  `farDispatchRetries` stats counters; new `trySend()`,
  `dispatchToFarGuaranteed()`, `dispatchToNear()`,
  `processPendingNearDispatches()` method declarations; updated
  comments on `dispatchToFar()`/`processPendingFarDispatches()`
  reflecting the new callers.
- `src/dram_cache_manager.cc`: `trySend()` (new, shared primitive);
  `dispatchToFar()`'s immediate branch refactored to call `trySend()`
  (pure refactor, behavior unchanged); `dispatchToFarGuaranteed()`,
  `dispatchToNear()`, `processPendingNearDispatches()` (new);
  `operate()` calls `processPendingNearDispatches()`; the 5 previously
  bare/ignored-return call sites (near tag-check read, near direct
  write, near fill write, far demand read, bypass read, bypass write —
  6 call sites, since near direct write and near fill write are
  separate) now route through `dispatchToNear()`/
  `dispatchToFarGuaranteed()`.

## Remaining gaps

None identified for the 6 audited paths — all now guarantee eventual
delivery, verified by `tests/test_dcm_memory_dispatch.cc` (see
`docs/validation.md` for full results). The `add_pq()` path
(prefetch traffic) remains a pre-existing, unaudited no-op — out of
scope (prefetch traffic to the lower level was already a no-op before
this port existed at all, per `docs/limitations.md`, and generates no
DCM-side dispatch to audit).
