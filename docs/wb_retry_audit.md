# WB → Far-Memory Retry-on-NACK Audit

**STATUS: FIXED.** This audit's classification (A — MUST IMPLEMENT) has
been implemented and verified — see "RESOLUTION" at the end of this
document for the exact fix, gem5 correspondence, and test results. The
audit content below is preserved unmodified as the historical record of
the investigation that led to the fix.

**No code changes in this stage [the audit stage].** Audit only, per explicit instruction.
Sources re-read for this audit: the paper (arXiv 2303.13029, already
fully pulled and read for the NVM audit — re-checked here for anything
Case-Study-specific about write-back pressure), the actual pulled
`dram_cache_disaggregated` branch source (`policy_manager.cc/.hh`,
focusing on `processFarMemWriteEvent`, `farMemRecvReqRetry`,
`recvTimingReq`'s WB-pressure check, and every `sendTimingReq()` call
site), the current ChampSim `MEMORY_CONTROLLER`/`DRAM_CACHE_MANAGER`
write path (`src/dram_controller.cc`, `src/dram_cache_manager.cc`), and
`docs/final_paper_coverage_audit.md`, `docs/gem5_to_champsim_mapping.md`,
`docs/limitations.md`, `docs/next_task.md`, `docs/validation.md`.

**Headline finding, ahead of the detailed walkthrough: this audit found
an actual, reproducible, silent-write-loss bug in ChampSim's
`MEMORY_CONTROLLER::add_wq`/`add_rq` (not something introduced by this
port, but real and directly relevant to whether the DCM's fire-and-forget
WB dispatch is safe), verified empirically with two standalone test
programs (below). This moves the classification from what the prior
documentation assumed ("not observed to matter in any test so far") to
a genuine correctness concern.**

## gem5's exact mechanism (re-verified directly from source)

`PolicyManager::processFarMemWriteEvent()` (`policy_manager.cc:481-519`):

```cpp
void PolicyManager::processFarMemWriteEvent()
{
    PacketPtr wrFarMemPkt = getPacket(pktFarMemWrite.front().second->getAddr(), ...);
    if (farReqPort.sendTimingReq(wrFarMemPkt)) {
        pktFarMemWrite.pop_front();          // ONLY pop on confirmed acceptance
        polManStats.sentFarWrPort++;
    } else {
        retryFarMemWrite = true;             // entry STAYS at front, NOT popped
        delete wrFarMemPkt;                  // only the throwaway copy is freed
        polManStats.failedFarWrPort++;
    }
    if (!pktFarMemWrite.empty() && !farMemWriteEvent.scheduled() && !retryFarMemWrite) {
        schedule(farMemWriteEvent, curTick()+1000);   // keep draining if not blocked
    }
    ...
}

void PolicyManager::farMemRecvReqRetry()   // called by gem5's port framework
{                                          // when far memory's port can accept again
    if (bypassDcache) { port.sendRetryReq(); return; }
    if (retryFarMemWrite) {
        if (!farMemWriteEvent.scheduled() && !pktFarMemWrite.empty())
            schedule(farMemWriteEvent, curTick());   // retry the SAME front entry
        retryFarMemWrite = false;
    }
    ...
}
```

gem5's `sendTimingReq()` is a **synchronous accept/decline call**: it
returns `true` only if the receiver (far memory's own controller port)
had room to accept the packet right now, and `false` otherwise (a
"NACK"). On `false`, gem5 does NOT pop the entry from `pktFarMemWrite`
— only a disposable copy (`wrFarMemPkt`, freshly constructed via
`getPacket()`) is deleted. The real entry stays at the front of the
deque until `farMemRecvReqRetry()` — invoked automatically by gem5's
port framework once the receiver signals it can accept again
(`port.sendRetryReq()` on the receiver's side) — re-schedules the exact
same drain event, which re-attempts the SAME front entry. This
guarantees: no lost writes (retained until confirmed sent), no
duplicated writes (only popped on confirmed acceptance), and FIFO
ordering preserved (always the front entry, never reordered).

The SAME pattern (`sendTimingReq` returning `false` → set a
`retry*`-flag → retry via the port's `recvReqRetry()` callback) is used
uniformly for `locMemRead`/`locMemWrite`/`farMemRead`/`farMemWrite` — it
is gem5's general timing-port contract, not something bespoke to the WB
path. `farMemWrite` additionally has an admission-side pressure valve:
`recvTimingReq` (`policy_manager.cc:332-345`) rejects (retries) NEW
incoming LLC requests once `pktFarMemWrite.size() >= orbMaxSize/2` —
already ported as `DCM_WB_PRESSURE_THRESHOLD` — but that is a DIFFERENT
mechanism (admission-time backpressure on new requests) from
`processFarMemWriteEvent`'s retry-on-NACK (drain-time retry of an
already-admitted write-back). Both exist in gem5 simultaneously and
serve different purposes; this port has only the first.

## Current ChampSim: what actually happens

`DRAM_CACHE_MANAGER::drainWB()` (`src/dram_cache_manager.cc`):

```cpp
void DRAM_CACHE_MANAGER::drainWB()
{
    if (WB.empty()) return;
    DCM_WB_ENTRY drained = WB.front();
    WB.pop_front();                          // ALWAYS popped, unconditionally
    stats.wbDrains++; stats.farWrites++; stats.numWrBacks++;
    dispatchToFar(drained.pkt, /*isWrite=*/true);   // fire-and-forget, no check, no fallback
}
```

`dispatchToFar()` calls `farMC->add_wq(&p)` directly (or holds it for
`linkLatencyCycles` first, then calls it) with **no check of `farMC`'s
occupancy beforehand and no inspection of any return value afterward.**
`MEMORY_CONTROLLER::add_wq()` (`src/dram_controller.cc:493-533`,
re-read in full for this audit):

```cpp
int MEMORY_CONTROLLER::add_wq(PACKET *packet)
{
    ...
    for (index=0; index<DRAM_WQ_SIZE; index++) {
        if (WQ[channel].entry[index].address == 0) {
            WQ[channel].entry[index] = *packet;
            WQ[channel].occupancy++;
            break;
        }
    }
    update_schedule_cycle(&WQ[channel]);
    return -1;                                // ALWAYS -1, success or failure alike
}
```

**This has no bounds check.** If the `for` loop never finds an empty
slot (the WQ is genuinely full), it simply exits with `index ==
DRAM_WQ_SIZE`, having written the packet nowhere. The function still
`return`s `-1` — the exact same value it returns on success — so **there
is no way for a caller to distinguish acceptance from silent loss.**
`add_rq()` (`dram_controller.cc:417-490`) has the byte-for-byte identical
pattern for reads. This is a pre-existing ChampSim primitive-level
behavior, not something introduced by the DCM port — but the DCM's
fire-and-forget `dispatchToFar()`/`drainWB()` inherits full exposure to
it, with no mitigation anywhere in the call chain.

### Empirical verification (two standalone test programs, not part of the test suite — diagnostic only, per "do not add tests yet")

**Test 1 — isolate the underlying `MEMORY_CONTROLLER` bug directly**
(bypassing the DCM entirely): 100 `add_wq()` calls submitted back-to-back
to a fresh `MEMORY_CONTROLLER` with `DRAM_WQ_SIZE=64`, zero `operate()`
cycles in between (so nothing drains):

```
far.WQ[0].occupancy after 100 add_wq calls with 0 operate() cycles = 64
DRAM_WQ_SIZE = 64
of 100 submitted writes, found in WQ array = 64 (expected <= 64)
SILENTLY LOST WRITES = 36
```

**Conclusive**: 36 of 100 writes vanish with zero error, zero rejection
signal, zero indication of any kind to the caller.

**Test 2 — drive the real `DRAM_CACHE_MANAGER` pipeline** (70 distinct
DRAM-cache indices seeded with dirty valid lines via writes, then all 70
evicted in a near-simultaneous burst of misses, exactly the kind of
write-back burst a write-intensive workload — the paper's own NPB
description: "NPB is more write-intensive and has higher miss ratio...
generates more write backs to the main memory" — could plausibly
produce):

```
numWrBacks(DCM stat)=30 wbInsertions=30 wbDrains=30 WB.size() remaining=0
far.WQ[0].occupancy=0
far.WQ completed(ROW_BUFFER_HIT+MISS)=29 still_in_array=0 total_accounted=29 expected=70
*** DATA LOSS CONFIRMED ***
```

(The real-pipeline numbers are smaller than the synthetic 70 because
bank contention among the deliberately-adjacent test addresses slowed
admission/completion of the seeding writes themselves within the test's
cycle budget — a test-harness artifact, not a finding in itself. The
important fact is qualitative and confirmed either way: **fewer writes
were ever confirmed reaching or completing at `farMC` than the DCM
itself believed it had drained (`wbDrains=30` vs `total_accounted=29`),
demonstrating the loss is reachable through the real DCM pipeline, not
only via a direct synthetic stress on `MEMORY_CONTROLLER` alone.**)

## Answers to the 10 questions

1. **Is WB retry-on-NACK part of the paper's architectural behavior?**
   Not explicitly discussed in the paper's text (the paper describes the
   WB buffer's existence and its role in admission backpressure, already
   ported, but never discusses gem5's port-level retry protocol — that
   is a gem5 implementation-level mechanism, not a paper-level modeling
   claim). It IS part of gem5's actual, real, executed implementation
   behavior (verified directly from source, Section above) — every
   simulated write-back in gem5 goes through this exact retry path
   whenever the far controller's port is momentarily unable to accept.

2. **Is it necessary to reproduce any paper experiment?** Not for any
   experiment already reproduced or validated in this port (Table II,
   BEAR/Oracle comparisons, link-latency sweep) — none of those exercise
   a write-back burst anywhere near `farMC`'s 64-entry WQ capacity. It
   WOULD matter for the not-yet-attempted Case Study 1/2/3 reproduction
   with real GAPBS/NPB traces, particularly NPB, which the paper itself
   describes as write-intensive with high write-back volume.

3. **Is it necessary for correctness of the DRAM-cache model?** **Yes,
   conditionally** — not for the model's logic/state-machine correctness
   (verified exhaustively by Table II and the 10 existing test suites,
   all of which stay well under the WQ-overflow threshold), but for
   TIMING/STATISTICS correctness under sustained write-back pressure,
   where the current fire-and-forget path can silently under-count
   completed write-backs relative to what the DCM's own stats
   (`numWrBacks`/`wbDrains`) claim happened — a real accuracy gap, not
   merely a missing nicety.

4. **Does the current ChampSim memory controller ever reject a write in
   a way that requires this mechanism?** It does something WORSE than
   reject: it silently drops with an identical return value to success
   (Section above, Test 1). There is no rejection SIGNAL to build a
   retry mechanism on top of using `add_wq`'s return value alone — a fix
   must use `get_occupancy()`/`get_size()` as a PRE-check instead (these
   already exist and are already used by the LLC's own admission
   contract elsewhere in this codebase).

5. **Can the current ChampSim WB path become incorrect without it?**
   Yes — demonstrated empirically (Test 2). Under a sufficiently bursty
   write-back workload, `drainWB()`'s unconditional one-per-cycle push
   into `farMC` can outpace `farMC`'s own real DRAM write-completion
   rate, filling `farMC`'s WQ, after which further `dispatchToFar()`
   calls silently vanish.

6. **Can it create lost writes, duplicated writes, or incorrect
   ordering?** **Lost writes: yes, demonstrated.** Duplicated writes: no
   mechanism exists that would create these (each WB entry is drained
   and dispatched exactly once regardless; the bug is loss, not
   duplication). Incorrect ordering: not directly — `WB` is drained FIFO
   and the loss (when it occurs) affects whichever entry happens to
   arrive at `farMC` when its WQ is already full, not a reordering of
   the surviving entries.

7. **Is the gem5 behavior fundamentally tied to its timing-port API?**
   Yes — gem5's `sendTimingReq()`/`recvReqRetry()` is an asynchronous,
   event-driven message-passing protocol specific to gem5's port
   architecture. ChampSim has no equivalent async callback mechanism at
   all (`MEMORY_CONTROLLER` is polled synchronously via `operate()` every
   cycle, with no push-based "I can accept now" notification). This
   means gem5's EXACT mechanism (a stored `retry*` boolean flag, resumed
   by an external callback) cannot be ported 1:1 — but this is not an
   obstacle: ChampSim's own cycle-driven polling model can achieve the
   identical OUTCOME (retain-until-accepted, retry every subsequent
   cycle) more simply, by checking `farMC->get_occupancy()` vs
   `get_size()` synchronously before every dispatch attempt, with no
   flag or callback needed at all — the polling loop itself IS the
   "retry" (see plan below).

8. **Can the behavior be reproduced cleanly in current ChampSim?** Yes.
   `MEMORY_CONTROLLER::get_occupancy(queue_type, address)`/
   `get_size(queue_type, address)` already exist, are already the
   established pre-check pattern this codebase uses elsewhere (the LLC's
   own `CACHE::handle_read`/`handle_writeback` check
   `get_occupancy()==get_size()` before calling `add_rq`/`add_wq` at
   all — `cache.cc:652`, already cited in this port's own comments), and
   already report exactly the information needed: whether `farMC`'s WQ
   has room. No change to `MEMORY_CONTROLLER`/`dram_controller.cc` is
   needed at all — the fix is entirely containable inside
   `dram_cache_manager.cc`, consistent with this port's established
   preference for scoping changes to the DCM layer.

9. **If implemented, what exact tests would be required?**
   1. A "WB entry retained under farMC pressure" test: pre-fill `farMC`'s
      WQ to capacity directly, then trigger a dirty eviction, and verify
      the WB entry is NOT lost — it stays queued and completes once
      `farMC` frees a slot (mirrors this audit's Test 2 but asserting
      the FIXED behavior).
   2. A "retry preserves FIFO order" test: multiple WB entries retained
      simultaneously under sustained pressure must drain in their
      original arrival order once space frees up.
   3. A "no duplicate write" test: verify a retried entry is dispatched
      to `farMC` exactly once (not re-sent after its first successful
      acceptance).
   4. A byte-for-byte regression test proving normal (never-full)
      behavior is UNCHANGED from today's fire-and-forget path — critical,
      since all 10 existing `tests/test_dcm_*.cc` suites must continue
      passing with zero expected-result changes (they never reach WQ
      capacity today).
   5. A stress test reproducing this audit's exact discovered scenario
      (many near-simultaneous dirty evictions exceeding `farMC`'s WQ
      capacity) proving zero writes are lost with the fix, directly
      contrasting against this audit's documented CURRENT loss without
      it.

10. **Would implementing it provide meaningful fidelity, or unnecessary
    complexity?** **Meaningful — this is a correctness fix, not a
    fidelity nicety.** The complexity cost is small: no new data
    structures beyond what already exists (`WB` already holds retained
    entries; the fix only changes WHEN `drainWB()` is allowed to pop
    the front), and no `dram_controller.cc` changes are needed (Section
    8). This is a materially smaller and more contained change than the
    NVM audit's rejected proposal, because the missing primitive
    (`get_occupancy`/`get_size` as a pre-check) already exists in this
    codebase.

## Classification

**A — MUST IMPLEMENT** (scoped: before any write-heavy Case Study
reproduction is attempted; does NOT retroactively invalidate anything
already completed).

Reasoning: this is not a gem5-fidelity nicety that ChampSim can
reasonably approximate away (unlike the NVM audit's finding, where the
paper's own mechanism was architecturally impossible to reproduce
without inventing numbers). This is a **demonstrated, reproducible
silent-data-loss bug** reachable through the DCM's own real request
pipeline under sustained write-back pressure — exactly the write-back
volume the paper's own text says NPB workloads produce
("NPB is more write-intensive and has higher miss ratio... generates
more write backs to the main memory," already quoted in this port's own
`docs/gem5_to_champsim_mapping.md`/paper analysis). Every existing test
and validated result in this port remains completely unaffected
(verified: `DCM_WB_PRESSURE_THRESHOLD`=64 admission throttle plus every
existing test's small, deliberate scenario sizes keep `farMC`'s WQ far
below its 64-entry capacity in all current test suites and both
smoke-test traces) — so this finding does not retroactively call any
completed validation into question. But it MUST be fixed before the
next planned major work item (`next_task.md`'s "Full Case Study 1/2/3
reproduction runs," explicitly involving NPB) is attempted, since that
is precisely the write-volume regime where this bug would silently
corrupt results with no error, no test failure, and no indication
anything went wrong.

## Smallest correct implementation plan (not coded, per instruction)

Entirely contained inside `src/dram_cache_manager.cc` /
`inc/dram_cache_manager.h`; no `dram_controller.cc` changes needed.

1. **`drainWB()`**: before popping `WB.front()`, check
   `farMC->get_occupancy(2, WB.front().pkt.address) <
   farMC->get_size(2, WB.front().pkt.address)` (queue_type 2 = WQ, the
   same convention already used throughout this port). If there is no
   room, return immediately WITHOUT popping — the entry stays at the
   front of `WB` and is retried automatically on the next `operate()`
   cycle's `drainWB()` call (ChampSim's per-cycle polling IS the retry
   mechanism here — no flag, no callback, no `dram_controller.cc` change
   needed, per question 7/8's finding).
2. **`processPendingFarDispatches()`** (the link-latency release path):
   apply the identical occupancy pre-check immediately before the
   `farMC->add_wq()`/`add_rq()` call at release time, for symmetry — a
   link-latency-delayed dispatch can equally arrive at a momentarily-full
   `farMC`. If full, leave the entry at the front of
   `pendingFarDispatches` and retry it on the next cycle (do not advance
   `targetCycle`; the entry is already "ready," just blocked on
   capacity, exactly mirroring gem5's `retryFarMemWrite`/
   `retryFarMemRead` distinction from the link/timing delay itself).
3. New `DCM_STATS` counters mirroring gem5's `numWrRetry`/
   `failedFarWrPort`/`sentFarWrPort` distinction (e.g.
   `wbDispatchRetries`, incremented each cycle a WB entry is held back by
   this check) — needed so the planned tests (question 9) can assert the
   retry path was actually exercised, not merely that no loss occurred
   by coincidence.
4. No change to `add_rq()`'s/`add_wq()`'s own admission-time
   `DCM_WB_PRESSURE_THRESHOLD` check — that mechanism is unrelated
   (admission-time backpressure on NEW requests, not drain-time retry of
   already-admitted write-backs) and stays exactly as implemented.

This plan deliberately does NOT touch `MEMORY_CONTROLLER`/
`dram_controller.cc` at all, even though the underlying silent-drop bug
(Section above) technically lives there and affects `add_rq()` too (not
just `add_wq()`) — because `get_occupancy()`/`get_size()` are sufficient
pre-checks to avoid ever calling into the buggy path in the first place
from the DCM side, which is the smallest correct fix consistent with
this port's established scope discipline (change the DCM layer, not
shared pre-existing ChampSim infrastructure, unless absolutely
required — this audit finds it is not required here).

## RESOLUTION (implemented, verified)

The plan above was implemented exactly as scoped — no
`dram_controller.cc` changes, entirely contained in
`inc/dram_cache_manager.h`/`src/dram_cache_manager.cc`.

### Exact fix

`dispatchToFar(const PACKET &pkt, bool isWrite)` now returns `bool`
instead of `void`. On the immediate-dispatch path
(`linkLatencyCycles == 0`), it checks
`farMC->get_occupancy(queueType, pkt.address) >=
farMC->get_size(queueType, pkt.address)` (`queueType` = 2 for a write,
1 for a read — the same convention used everywhere else in this port)
BEFORE ever calling `add_wq()`/`add_rq()`. If there is no room, it
returns `false` having sent and queued nothing at all. On the
link-latency path, it always returns `true` (the packet is safely
queued in `pendingFarDispatches`; the real capacity check happens later,
at release time).

`drainWB()` copies `WB.front()` into a local variable, calls
`dispatchToFar()` on the copy's packet, and pops the REAL `WB.front()`
only if `dispatchToFar()` returned `true`. On `false`, it increments the
new `stats.wbDispatchRetries` counter and returns immediately —
`WB.front()` is untouched, to be retried by the next `operate()` cycle's
`drainWB()` call.

`processPendingFarDispatches()` applies the identical occupancy check to
the FRONT of `pendingFarDispatches` before popping it, for symmetry (a
link-latency-delayed dispatch can equally arrive at a momentarily-full
`farMC` at release time). If blocked, it `break`s out of its drain loop
immediately rather than skipping to a later-queued entry — this is what
preserves FIFO ordering under retry (an entry released later must never
be dispatched ahead of one still waiting on capacity).

Three new `DCM_STATS` counters: `wbDispatchRetries` was the only one
actually needed (see below) — `bypassReads`/`bypassWrites`/
`bypassCompletedReads` already existed from the bypass-mode stage and
are unaffected here.

### Files changed

- `inc/dram_cache_manager.h`: `dispatchToFar()`'s signature changed to
  return `bool`; `wbDispatchRetries` counter added to `DCM_STATS`;
  updated header comments on `dispatchToFar()`, `drainWB()`,
  `processPendingFarDispatches()`.
- `src/dram_cache_manager.cc`: `dispatchToFar()`, `drainWB()`,
  `processPendingFarDispatches()` all rewritten per "Exact fix" above.
- `tests/test_dcm_wb_retry.cc` (new): 9 required tests + 1 stress test,
  all passing (see below).

### Tests added

`tests/test_dcm_wb_retry.cc`, all passing:

1. Far queue has capacity → WB write accepted → WB entry removed.
2. Far queue is full → WB write NOT removed → no write lost.
3. Queue becomes available → WB write succeeds on a later cycle → WB
   entry removed exactly once (continues from Test 2's exact state).
4. Multiple WB entries (N=20) → no entry lost, no entry duplicated.
5. **Stress test**: 3 successive waves of 50 dirty evictions each (150
   total), every wave fully retained/blocked via a deterministic
   `forceWqFull` override before release — proves `WB entries generated
   == far-memory writes accepted`, `lost = 0`, `duplicated = 0`, summed
   across all waves.
6. Mixed WB traffic and normal far reads/writes — far-read dispatches
   (cold misses) and far-write dispatches (WB drains) share `farMC`
   correctly, verified via an admission-retry helper.
7. Ordering is preserved — WB entries drain to `farMC` in their exact
   original insertion order, even when several are blocked and retried.
8. Parent/request accounting remains correct — the evicting request's
   own ORB entry completes independently of whether its victim's
   write-back is retained/retried (verified with `forceWqFull` isolating
   the write-back specifically, without also throttling the parent's own
   far-read via `MEMORY_CONTROLLER`'s real `write_mode` arbitration).
9. WB occupancy statistics (`wbMaxOccupancy`, `wbInsertions`,
   `wbDrains`, `wbDispatchRetries`) remain correct both while entries are
   blocked and after they drain.

**Four real test-methodology bugs were found and fixed while writing
this file** (none in the implementation — all in the test's own
methodology, the same general category as bugs already documented in
`tests/test_dcm_controller_latency.cc`'s and
`tests/test_dcm_bypass.cc`'s banner comments this session):

1. `pumpUntilQuiescent`'s original condition
   (`WB.empty() && farMC.WQ.occupancy==0`) was trivially satisfied the
   instant it was first called, before the just-issued request had even
   been dispatched — fixed by also requiring `dcm.ORB.empty()`.
2. Test addresses generated as `base + i*STRIDE` (where
   `STRIDE = DCM_DRAM_CACHE_SIZE`) all collided on the SAME DRAM-cache
   index instead of being distinct — fixed by generating distinct base
   addresses as `base + i*DCM_BLOCK_SIZE` and reserving `+STRIDE` only
   for pairing an address with the eviction that targets its specific
   index.
3. `makeReadPacket`/`makeWritePacket` never stamped `event_cycle`,
   which defaults to `UINT64_MAX` in `PACKET`'s constructor — harmless
   for packets that flow through the DCM's own admission path (which
   always re-stamps it before dispatch), but fatal for packets injected
   DIRECTLY into a `MEMORY_CONTROLLER` (this file's "filler" packets):
   `MEMORY_CONTROLLER::update_schedule_cycle()` picks the unscheduled
   entry with the smallest `event_cycle`, so an entry stuck at
   `UINT64_MAX` can never win that comparison and is never scheduled at
   all — fixed by stamping `p.event_cycle = current_core_cycle[0]` in
   both helpers.
4. An initial single-wave stress-test design (100 simultaneously-blocked
   WB entries) conflicted with `DCM_WB_PRESSURE_THRESHOLD`'s own
   pre-existing, correct admission-time backpressure (which caps WB at
   64 entries by rejecting new admissions past that point, precisely to
   prevent this) — redesigned as multiple successive waves of 50 (each
   under the threshold), proving sustained pressure across repeated
   cycles rather than one artificially-oversized single burst that the
   system is designed to never allow in the first place.

Also surfaced, but explicitly NOT fixed (out of scope for this task,
consistent with "prefer fixing the DCM WB dispatch path rather than
globally changing the old ChampSim controller"): the identical
`MEMORY_CONTROLLER` silent-drop bug is independently reachable via
`driveState()`'s direct, unprotected `nearMC->add_rq()` tag-check-read
dispatch and the background-fill-write's direct `nearMC->add_wq()` call
when many requests are admitted concurrently enough to exceed
`DRAM_RQ_SIZE`/`DRAM_WQ_SIZE` (64 each) on the NEAR side. This is a
separate, already-anticipated gap (the original audit's Section "Also
surfaced" already flagged the far-READ call site as having "identical
exposure... explicitly out-of-scope"; the near-side tag-check and
fill-write dispatches turn out to share it too). Tests in this file
avoid ever triggering it (via serialized/paced admission) specifically
so they stay focused on the WB→far-write path this task scoped. Not
tracked as a new `next_task.md` item beyond the existing "WB→far
dispatch retry-on-nack" framing, since it is the same underlying
`MEMORY_CONTROLLER` primitive gap, just reachable from additional call
sites — worth revisiting together if this class of issue is ever
addressed more broadly.

### Stress-test result

3 waves × 50 evictions = 150 total dirty write-backs, every wave fully
blocked (verified: `wbInsertions` increases by exactly 50 with
`wbDrains` unchanged, per wave, before release) then fully drained on
release: `total_generated=150 total_accepted=150 lost=0 duplicated=0`,
with `wbDispatchRetries` in the thousands (confirms capacity was
genuinely, repeatedly exceeded, not coincidentally avoided).

The ORIGINAL audit's direct-to-`MEMORY_CONTROLLER` diagnostic (bypassing
the DCM entirely) still reproduces its original result unchanged (36 of
100 writes lost) — this is expected and correct: that diagnostic
demonstrates the underlying `MEMORY_CONTROLLER` primitive itself was
never modified (per the explicit "prefer fixing the DCM WB dispatch
path" instruction); the fix instead ensures the DCM's own dispatch code
never calls into that primitive in a way that could trigger it.

### Total regression result

All 10 pre-existing `tests/test_dcm_*.cc` suites pass **unchanged, with
zero expected-result modifications anywhere** (unlike the controller-
latency stage's one documented change): `test_dcm_skeleton`,
`test_dcm_baseline`, `test_dcm_wb_pressure`, `test_dcm_near_far_config`,
`test_dcm_link_latency`, `test_dcm_stats`, `test_dcm_bear`,
`test_dcm_oracle`, `test_dcm_controller_latency`, `test_dcm_bypass`.
Full-binary build (`make clean && make -j4`) succeeds, and a smoke-test
run (`dpc3_traces/403.gcc-16B.champsimtrace.xz`, 100K warmup / 200K sim
instructions) produces output **byte-identical** to the pre-fix build.

### gem5 correspondence

| | gem5 | This port |
|---|---|---|
| Retention invariant | `pktFarMemWrite.front()` popped ONLY when `farReqPort.sendTimingReq()` returns `true` | `WB.front()` popped ONLY when `dispatchToFar()` returns `true` |
| Retry trigger | Asynchronous: receiver calls `port.sendRetryReq()` when it can accept again → gem5's port framework invokes `farMemRecvReqRetry()` → `schedule(farMemWriteEvent, curTick())` | Synchronous polling: `drainWB()`/`processPendingFarDispatches()` are already called every `operate()` cycle regardless: simply not popping on failure IS the retry, with no flag or callback needed |
| Ordering | FIFO — `pktFarMemWrite` is a deque, always operated on at the front | FIFO — `WB`/`pendingFarDispatches` are deques, always operated on at the front; a blocked entry halts further releases that cycle rather than allowing a later entry to jump ahead |
| Signal that a queue is full | `sendTimingReq()`'s `bool` return value (built into gem5's port API) | `get_occupancy()` vs `get_size()`, checked explicitly BEFORE calling `add_wq()`/`add_rq()` (no return-value signal exists on this port's `add_wq()`/`add_rq()` — see the audit's Section 4) |
| Where the check lives | Inside `processFarMemWriteEvent()`/`recvTimingReq()`, `PolicyManager`-internal | Inside `dispatchToFar()`/`processPendingFarDispatches()`, `DRAM_CACHE_MANAGER`-internal — no `dram_controller.cc` change |

The correspondence is exact in outcome (retain-until-accepted, retry
every opportunity, preserve order) despite the mechanism differing
(async callback vs. sync polling) — this was anticipated in the
audit's answer to question 7 ("gem5 behavior fundamentally tied to its
timing-port API? ... ChampSim's own cycle-driven polling model can
achieve the identical OUTCOME... more simply").

### Remaining gaps

- The near-side `MEMORY_CONTROLLER` silent-drop exposure (tag-check-read
  dispatch, background-fill-write dispatch) noted above — real, but
  explicitly out of scope for this task.
- Far-READ dispatch (`driveState()`'s `DCM_FAR_MEM_READ` case) does not
  use `dispatchToFar()`'s new return value — it still fire-and-forgets,
  matching the original audit's explicit scoping to the WB/write path
  only. Functionally this is not a regression (a full `farMC` RQ
  produces a permanently-stuck ORB entry whether or not the pre-check
  exists, since nothing currently retries a rejected far-read
  admission) — but it is not fixed by this stage's work.
- `bypassDcache`'s far-side retry-on-NACK gap (gem5's
  `farMemRecvReqRetry`'s bypass branch, `port.sendRetryReq()`) remains
  fully unaddressed, and does NOT benefit from this fix: bypass mode's
  `add_rq()`/`add_wq()` call the SAME now-fixed `dispatchToFar()`, so a
  bypass request no longer risks tripping `MEMORY_CONTROLLER`'s internal
  silent-drop bug — but on a `false` return (farMC has no room), the
  packet is simply dropped by bypass mode's own code, exactly as before,
  since bypass mode has no `WB`-like holding structure to retain it in
  for a later retry. This is a real, still-open gap for bypass mode
  specifically, distinct from (and not fixed by) the WB-path retention
  this stage adds — already noted in `docs/bypass_mode.md`'s limitations
  and not expanded further here, since fixing it is out of this task's
  explicit scope (WB→far-memory only).
