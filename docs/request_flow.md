# Request Flow — Complete Baseline + BEAR-Wr-Opt

This documents the exact, current request flow through `DRAM_CACHE_MANAGER`
for the baseline (`CascadeLakeNoPartWrs`) and BEAR-Wr-Opt policies, with
real tag/metadata behavior (no more `debugForceHit`). Captured directly
from `tests/test_dcm_baseline.cc`/`tests/test_dcm_bear.cc` debug-print
output, not hand-written.

## General shape (all cases)

```
add_rq()/add_wq() [entry point]
  -> conflict-by-index check (checkConflictInORB)
       conflict -> CRB (wait) -> promoted later, re-enters at admitRequest
       no conflict -> continue
  -> ORB-full check -> admitRequest()
       -> classifyAndInstall(): tag lookup, hit/miss/dirty/victim
          classification, EAGER tag/metadata install (before any physical
          op runs)
       -> state = DCM_LOC_MEM_READ, driveState()
  -> nearMC->add_rq(tag-check read)         [ALWAYS, never skipped -- baseline]
  ... nearMC timing ...
  -> return_data() [tag-check read completes]
       -> if handleDirtyLine: pushDirtyWriteBack() (WB insert -> farMC->add_wq, drained immediately)
       -> branch on {isWriteReq, isHit}:
            write            -> DCM_LOC_MEM_WRITE -> nearMC->add_wq() -> completeRequest() [no LLC callback]
            read + hit       -> completeRequest() [responds to LLC now]
            read + miss      -> DCM_FAR_MEM_READ -> farMC->add_rq()
                                  -> return_data() [far read completes]
                                       -> completeRequest() [responds to LLC NOW, "respond-before-fill"]
                                       -> nearMC->add_wq(background fill) [fire-and-forget]
  -> completeRequest(): ORB entry erased, promoteFromCRB(freedIndex) checked
```

## Case C: READ MISS + INVALID/COLD (`tests/test_dcm_baseline.cc`, Case C)

```
[DCM] req#1 READ addr=0x300000 index=49152 tag=0 arrival_cycle=2053
[DCM] req#1 classify MISS victim_valid=0 victim_dirty=0 victim_addr=0x0 needs_writeback=0
[DCM] req#1 -> LOC_MEM_READ (tag check)
[DCM] req#1 -> FAR_MEM_READ (miss fetch)
[DCM] req#1 DONE addr=0x300000 total_cycles=301
[DCM] background fill write addr=0x300000
```
Reading this: classification happens BEFORE the tag-check read is even
issued (eager install, per gem5 ordering) and correctly reports
`victim_valid=0` (nothing was ever resident at this index) so
`needs_writeback=0`. The tag-check read completes, sees no dirty victim,
and (since this is a read miss) proceeds straight to the far fetch. On
far completion the request is marked `DONE` (LLC responded to) and only
*then* is the background fill write issued — this is the
"respond-before-fill" ordering, visible here as the fill write appearing
in the log *after* `DONE`.

## Case E: READ MISS + DIRTY VICTIM (`tests/test_dcm_baseline.cc`, Case E)

```
[DCM] req#3 READ addr=0x8500000 index=81920 tag=1 arrival_cycle=4757
[DCM] req#3 classify MISS victim_valid=1 victim_dirty=1 victim_addr=0x500000 needs_writeback=1
[DCM] req#3 -> LOC_MEM_READ (tag check)
[DCM] req#3 WB insert victim addr=0x500000 wb_size=1
[DCM] req#3 -> FAR_MEM_WRITE (dirty victim writeback) addr=0x500000
[DCM] req#3 -> FAR_MEM_READ (miss fetch)
[DCM] req#3 DONE addr=0x8500000 total_cycles=101
[DCM] background fill write addr=0x8500000
```
Classification correctly identifies the resident victim at this index
(`0x500000`, valid+dirty) and records `needs_writeback=1` — again, this
decision is made at classification time (before the tag-check read is
issued), but the *push* into the WB deque and the resulting far write
only happen once the tag-check read physically completes (`WB insert` /
`FAR_MEM_WRITE` lines appear right after `LOC_MEM_READ`, matching gem5's
exact trigger point: the tag-check read is what "sources" the victim's
data since tag+data are co-located). Only after the write-back is queued
does the state machine proceed to fetch the new line from far memory.
Four operations total: local-read (tag-check), far-write (victim
write-back), far-read (fetch new data), local-write (background fill) —
matches Table II's `Read Miss Dirty = 4`.

## Case H: WRITE MISS + DIRTY VICTIM (`tests/test_dcm_baseline.cc`, Case H)

```
[DCM] req#3 WRITE addr=0x8800000 index=131072 tag=1 arrival_cycle=7860
[DCM] req#3 classify MISS victim_valid=1 victim_dirty=1 victim_addr=0x800000 needs_writeback=1
[DCM] req#3 -> LOC_MEM_READ (tag check)
[DCM] req#3 WB insert victim addr=0x800000 wb_size=1
[DCM] req#3 -> FAR_MEM_WRITE (dirty victim writeback) addr=0x800000
[DCM] req#3 -> LOC_MEM_WRITE
[DCM] req#3 DONE addr=0x8800000 total_cycles=51
```
Same victim write-back trigger point as Case E, but since this is a
**write**, there is never a far-read (write misses never fetch from far
memory in the baseline — they simply install the incoming write data
locally). Three operations: local-read (tag-check), far-write (victim
write-back), local-write (install) — matches Table II's
`Write Miss Dirty = 3`. Also note: no `DONE`-triggered LLC callback at
all — writes never produce one (see `gem5_to_champsim_mapping.md` fact
#2) — `DONE` here just means the ORB entry is retired and
`promoteFromCRB` is checked.

## Conflict + real classification interaction (`tests/test_dcm_skeleton.cc`, Test 5)

```
[DCM] req#1 READ addr=0x10000 index=1024 tag=0 arrival_cycle=0
[DCM] req#1 classify MISS victim_valid=0 victim_dirty=0 victim_addr=0x0 needs_writeback=0
[DCM] req#1 -> LOC_MEM_READ (tag check)
[DCM] CRB insert (conflict) READ addr=0x8010000 index=1024 crb_size=1
[DCM] req#1 -> FAR_MEM_READ (miss fetch)
[DCM] req#1 DONE addr=0x10000 total_cycles=301
[DCM] CRB promote addr=0x8010000 index=1024 waited_cycles=301 crb_size=0
[DCM] req#2 READ addr=0x8010000 index=1024 tag=1 arrival_cycle=0
[DCM] req#2 classify MISS victim_valid=1 victim_dirty=0 victim_addr=0x10000 needs_writeback=0
[DCM] req#2 -> LOC_MEM_READ (tag check)
[DCM] background fill write addr=0x10000
[DCM] req#2 -> FAR_MEM_READ (miss fetch)
[DCM] req#2 DONE addr=0x8010000 total_cycles=402
[DCM] background fill write addr=0x8010000
```
This demonstrates the full required chain for a conflicting request:
arrival → CRB (while req#1 occupies the index) → promotion (once req#1
retires) → **re-enters classification fresh** (`req#2 classify`), where
it correctly sees req#1's now-resident line (`victim_valid=1
victim_addr=0x10000`, clean since req#1 was a read) as its own victim —
a real READ MISS + CLEAN VICTIM classification produced purely from the
conflict/promotion mechanism, not hand-set up. No write-back
(`needs_writeback=0`, victim was clean) → local-read, far-read,
local-write only (matches Table II `Read Miss Clean = 3`, confirmed by
the test's exact stat assertions in `implementation_status.md`).

## BEAR-Wr-Opt

BEAR-Wr-Opt reuses the ENTIRE baseline flow above unchanged. The only
difference is a single extra decision, `chooseInitialState()`, inserted
right after `classifyAndInstall()` (which is itself completely
policy-independent — the same classification code runs for every
policy): if this request is a write AND `classifyAndInstall()` already
found it to be a hit, jump directly to `DCM_LOC_MEM_WRITE`, skipping
`DCM_LOC_MEM_READ` (the tag-check read) and the wait for it entirely.
Every other combination (read hit, read miss, write miss of any kind)
enters `DCM_LOC_MEM_READ` exactly like baseline — verified case-by-case
in `tests/test_dcm_bear.cc`.

### BEAR write hit (`test_dcm_bear.cc` Test 1)

```
[DCM] req#2 WRITE addr=0x1000000 index=262144 tag=0 arrival_cycle=601
[DCM] req#2 classify HIT victim_valid=1 victim_dirty=0 victim_addr=0x1000000 needs_writeback=0
[DCM] req#2 BEAR-Wr-Opt: write HIT -> skip tag-check read, go straight to LOC_MEM_WRITE
[DCM] req#2 -> LOC_MEM_WRITE
[DCM] req#2 DONE addr=0x1000000 total_cycles=0
```
The complete required trace: **request → ORB** (admitted, `req#2` assigned)
**→ BEAR metadata decision** (`classify HIT`, using the exact same
`tagMetadataStore` lookup baseline uses — no separate "BEAR metadata")
**→ write hit** (`isWriteReq && isHit` both true) **→ no local tag-check
read** (`LOC_MEM_READ` never appears in this log — contrast with every
other trace in this document, where it always does) **→ completion**
(`LOC_MEM_WRITE` then `DONE`, `total_cycles=0`). The `total_cycles=0` is
not a bug: since this is a write, there is no LLC-facing callback to wait
for (matches baseline's own write contract), and skipping the read
removes the only step that would otherwise have taken real cycles — the
whole point of the optimization is that a write hit finishes
essentially instantly instead of paying for a tag-check round-trip.

### BEAR write miss + dirty victim (`test_dcm_bear.cc` Test 4)

```
[DCM] req#3 WRITE addr=0x9300000 index=311296 tag=1 arrival_cycle=3803
[DCM] req#3 classify MISS victim_valid=1 victim_dirty=1 victim_addr=0x1300000 needs_writeback=1
[DCM] req#3 -> LOC_MEM_READ (tag check)
[DCM] req#3 WB insert victim addr=0x1300000 wb_size=1
[DCM] req#3 -> LOC_MEM_WRITE
[DCM] req#3 DONE addr=0x9300000 total_cycles=51
[DCM] WB drain -> FAR_MEM_WRITE (dirty victim writeback) addr=0x1300000 remaining_wb_size=0
```
A write **miss** (even with a dirty victim) is NOT exempted — `LOC_MEM_READ`
still appears, exactly like baseline's Case H (`docs/request_flow.md`'s
earlier WRITE MISS + DIRTY VICTIM trace uses identical structure). BEAR
only ever touches the `start` decision for write **hits**.

### BEAR write hit produced by CRB promotion (`test_dcm_bear.cc` Test 11)

```
[DCM] req#1 WRITE addr=0x1a00000 index=425984 tag=0 arrival_cycle=10864
[DCM] req#1 classify MISS victim_valid=0 victim_dirty=0 victim_addr=0x0 needs_writeback=0
[DCM] req#1 -> LOC_MEM_READ (tag check)
[DCM] CRB insert (conflict) WRITE addr=0x1a00000 index=425984 crb_size=1
[DCM] req#1 -> LOC_MEM_WRITE
[DCM] req#1 DONE addr=0x1a00000 total_cycles=151
[DCM] CRB promote addr=0x1a00000 index=425984 waited_cycles=151 crb_size=0
[DCM] req#2 WRITE addr=0x1a00000 index=425984 tag=0 arrival_cycle=10864
[DCM] req#2 classify HIT victim_valid=1 victim_dirty=1 victim_addr=0x1a00000 needs_writeback=0
[DCM] req#2 BEAR-Wr-Opt: write HIT -> skip tag-check read, go straight to LOC_MEM_WRITE
[DCM] req#2 -> LOC_MEM_WRITE
[DCM] req#2 DONE addr=0x1a00000 total_cycles=151
```
`req#1` is a genuine cold write miss to a fresh address; `req#2` is a
SECOND write to the exact same address, submitted while `req#1` is still
outstanding, so it correctly conflicts (same address ⇒ same index) and
queues in the CRB. Because `classifyAndInstall()` installs `req#1`'s tag
*eagerly at admission* (before `req#1`'s own tag-check read even
completes — see `gem5_to_champsim_mapping.md` fact #7), by the time
`req#1` retires and `req#2` is promoted, the resident tag already equals
`req#2`'s own tag — `req#2` classifies as a genuine **HIT**, purely as a
consequence of CRB promotion, not hand-set up. BEAR correctly applies the
same read-elimination optimization to it as to any directly-admitted
write hit (`BEAR-Wr-Opt: write HIT -> skip...` appears for `req#2` too).
This proves the optimization decision is made fresh, from real
classification state, every time a request enters the state machine —
whether by direct admission or by promotion.

### Baseline vs. BEAR-Wr-Opt diff, identical non-write-hit scenario (`test_dcm_bear.cc` Test 14)

Running the exact same 3-step scenario (cold read, write-to-same-address,
then a read that misses evicting a dirty victim) under both policies and
diffing:

```
BASELINE:                                    BEAR-Wr-Opt:
req#3 classify MISS ... needs_writeback=1    req#3 classify MISS ... needs_writeback=1
req#3 -> LOC_MEM_READ (tag check)            req#3 -> LOC_MEM_READ (tag check)
req#3 WB insert victim ...                   req#3 WB insert victim ...
req#3 -> FAR_MEM_READ (miss fetch)           req#3 -> FAR_MEM_READ (miss fetch)
WB drain -> FAR_MEM_WRITE ...                WB drain -> FAR_MEM_WRITE ...
req#3 DONE ...                               req#3 DONE ...
background fill write ...                    background fill write ...
```
Byte-for-byte identical for the read-miss-dirty-victim step (only the
arrival-cycle timestamp differs, because BEAR's earlier write-hit step
completed a few cycles faster — see Test 1's `total_cycles=0` note
above). Measured over the full 3-step scenario, baseline does exactly
one more `local_read` than BEAR (`base=(3,3,2,1)` vs `bear=(2,3,2,1)`) —
attributable entirely to the one write-hit step BEAR optimizes away, and
nothing else.

## Oracle

Oracle reuses the entire baseline/BEAR flow unchanged too. The only new
code is `chooseInitialState()`'s Oracle branch, which additionally
exempts any miss (read or write) whose old resident was not dirty
(clean-valid or cold/invalid), on top of the write-hit exemption it
shares with BEAR-Wr-Opt.

### Oracle write hit (`test_dcm_oracle.cc` Test 2)

```
[DCM] req#2 WRITE addr=0x2100000 index=540672 tag=0 arrival_cycle=1253
[DCM] req#2 classify HIT victim_valid=1 victim_dirty=0 victim_addr=0x2100000 needs_writeback=0
[DCM] req#2 Oracle: write HIT -> skip tag-check read, go straight to LOC_MEM_WRITE
[DCM] req#2 -> LOC_MEM_WRITE
[DCM] req#2 DONE addr=0x2100000 total_cycles=0
```
Identical shape to BEAR's write-hit trace (same underlying mechanism).

### Oracle clean miss (`test_dcm_oracle.cc` Test 3, read cold miss)

```
[DCM] req#1 READ addr=0x2200000 index=557056 tag=0 arrival_cycle=1753
[DCM] req#1 classify MISS victim_valid=0 victim_dirty=0 victim_addr=0x0 needs_writeback=0
[DCM] req#1 Oracle: READ CLEAN MISS -> skip tag-check read, go straight to FAR_MEM_READ
[DCM] req#1 -> FAR_MEM_READ (miss fetch)
[DCM] req#1 DONE addr=0x2200000 total_cycles=151
[DCM] background fill write addr=0x2200000
```
`LOC_MEM_READ` never appears — Oracle's additional exemption over BEAR:
a clean (or cold) miss goes straight to the far fetch, skipping the tag
check entirely, because the SRAM tag store already told Oracle the line
was clean/invalid without needing to touch the DRAM cache at all.

### Oracle dirty miss (`test_dcm_oracle.cc` Test 7, read dirty miss)

```
[DCM] req#3 READ addr=0xa600000 index=622592 tag=1 arrival_cycle=5408
[DCM] req#3 classify MISS victim_valid=1 victim_dirty=1 victim_addr=0x2600000 needs_writeback=1
[DCM] req#3 -> LOC_MEM_READ (tag check)
[DCM] req#3 WB insert victim addr=0x2600000 wb_size=1
[DCM] req#3 -> FAR_MEM_READ (miss fetch)
[DCM] WB drain -> FAR_MEM_WRITE (dirty victim writeback) addr=0x2600000 remaining_wb_size=0
[DCM] req#3 DONE addr=0xa600000 total_cycles=101
[DCM] background fill write addr=0xa600000
```
`LOC_MEM_READ` DOES appear here, even under Oracle — a dirty victim's
data must still be physically read out to source its write-back; Oracle
only ever eliminates *tag checks*, never data reads that are actually
needed. Structurally identical to baseline's/BEAR's dirty-miss trace.

### Oracle read hit (`test_dcm_oracle.cc` Test 1)

```
[DCM] req#2 READ addr=0x2000000 index=524288 tag=0 arrival_cycle=451
[DCM] req#2 classify HIT victim_valid=1 victim_dirty=0 victim_addr=0x2000000 needs_writeback=0
[DCM] req#2 -> LOC_MEM_READ (tag check)
[DCM] req#2 DONE addr=0x2000000 total_cycles=51
```
Also unchanged from baseline — a read hit still needs the local read to
fetch the actual data (Oracle's SRAM only tells it there IS data to
fetch, not what that data is).

### Oracle read miss (clean-victim variant, `test_dcm_oracle.cc` Test 5)

Same shape as the cold-miss trace above (`LOC_MEM_READ` skipped, straight
to `FAR_MEM_READ`) — confirms the exemption applies identically whether
the old resident was genuinely clean-and-valid or cold/invalid, matching
the paper's rule not distinguishing a clean victim from an invalid one.

### Oracle write hit produced by CRB promotion (`test_dcm_oracle.cc` Test 13)

```
[DCM] req#1 READ addr=0x2c00000 index=720896 tag=0 arrival_cycle=11620
[DCM] req#1 classify MISS victim_valid=0 victim_dirty=0 victim_addr=0x0 needs_writeback=0
[DCM] req#1 Oracle: READ CLEAN MISS -> skip tag-check read, go straight to FAR_MEM_READ
[DCM] req#1 -> FAR_MEM_READ (miss fetch)
[DCM] CRB insert (conflict) WRITE addr=0x2c00000 index=720896 crb_size=1
[DCM] req#1 DONE addr=0x2c00000 total_cycles=151
[DCM] CRB promote addr=0x2c00000 index=720896 waited_cycles=151 crb_size=0
[DCM] req#2 WRITE addr=0x2c00000 index=720896 tag=0 arrival_cycle=11620
[DCM] req#2 classify HIT victim_valid=1 victim_dirty=0 victim_addr=0x2c00000 needs_writeback=0
[DCM] req#2 Oracle: write HIT -> skip tag-check read, go straight to LOC_MEM_WRITE
[DCM] req#2 -> LOC_MEM_WRITE
[DCM] req#2 DONE addr=0x2c00000 total_cycles=151
[DCM] background fill write addr=0x2c00000
```
Note `req#1` itself is a read-clean-miss that ALSO skips its own read —
this is the scenario that gives `req#2` a genuine asynchronous window to
conflict (a direct-to-`LOC_MEM_WRITE` skip, as used for BEAR's equivalent
test, completes synchronously and offers no such window; a
direct-to-`FAR_MEM_READ` skip does, since the far fetch is real
asynchronous latency). `req#2` is re-classified fresh at promotion time
and correctly resolves to a write hit, with its own read eliminated too.

## bypassDcache / "No-DRAM-Cache" comparison mode

Full architectural writeup and gem5 call-site derivation in
`docs/bypass_mode.md`. This section captures the actual request flow,
directly from debug-print output (not hand-written), for a read and a
write issued to two addresses that would share a DRAM-cache index in
normal mode (`addrB = addrA + DCM_DRAM_CACHE_SIZE`) with
`bypassDcache=true`:

```
[DCM] bypass response addr=0x9000000
```

Note how much is ABSENT compared to every trace above this section: no
`req#N` line at all (no ORB entry is ever created, so there is no
`requestId` to log), no `classify`/`HIT`/`MISS` line (`classifyAndInstall()`
is never called), no `-> LOC_MEM_READ`/`-> FAR_MEM_READ` state-machine
line (`chooseInitialState()`/`driveState()` are never invoked), no `CRB
insert`/`CRB promote` line despite `addrB` sharing `addrA`'s DRAM-cache
index (conflict tracking is index-based and bypass mode has no index
concept at all), and no `background fill write` line (there is no
DRAM-cache line to fill). The single `bypass response` line is the
entire DCM-side footprint of the read; the write produces NO debug
output at all under `debugPrint`, matching every OTHER write in this
port (no LLC callback in any mode) — its only observable effect is
`farMC`'s WQ occupancy rising then draining, exactly like a normal
far-write-back's own `farMC->add_wq()` call, just reached directly
instead of via `pushDirtyWriteBack()`/`drainWB()`.

General shape, for direct comparison against the "General shape (all
cases)" diagram at the top of this file:

```
add_rq()/add_wq() [entry point]
  -> bypassDcache check [FIRST, before conflict/CRB/ORB/admission --
                          mirrors gem5 recvTimingReq exactly]
       true  -> dispatchToFar() directly [far-link latency still applies]
                  -> farMC->add_rq()/add_wq() [SAME farMC normal mode uses]
       false -> (falls through to the normal-mode flow above, unchanged)
  ... farMC timing (+ optional link-latency hold before dispatch) ...
  -> return_data() [read only -- writes never produce a callback in ANY mode]
       -> bypassOutstandingReads lookup (NOT an ORB lookup -- none exists)
       -> deliver to LLC immediately [NO controller frontend/backend latency]
```

Every step between "bypassDcache check" and "farMC timing" that exists
in the normal-mode diagram — conflict-by-index check, ORB-full check,
`admitRequest()`, `classifyAndInstall()`, `chooseInitialState()`,
`driveState()`'s local tag-check read, `pushDirtyWriteBack()` — is
entirely absent from the bypass path, not merely skipped conditionally
within shared code. This is the same architectural point made in
`bypass_mode.md`'s request-path section, shown here as an actual trace
rather than only a diagram.

## WB → far-memory write acceptance/retry (stage 9)

Full architectural writeup and gem5 correspondence in
`docs/wb_retry_audit.md`'s "RESOLUTION" section. This section captures
the actual request flow, directly from debug-print output (not
hand-written), for a dirty eviction whose write-back is blocked because
`farMC`'s WQ is completely full (64/64), then released once a slot frees
up:

```
[DCM] req#2 READ addr=0x13000000 index=786432 tag=2 arrival_cycle=151
[DCM] req#2 classify MISS victim_valid=1 victim_dirty=1 victim_addr=0xb000000 needs_writeback=1
[DCM] req#2 -> LOC_MEM_READ (tag check)
[DCM] req#2 WB insert victim addr=0xb000000 wb_size=1
[DCM] req#2 -> FAR_MEM_READ (miss fetch)
[DCM] WB drain BLOCKED (farMC WQ full) addr=0xb000000 retained, wb_size=1
[DCM] WB drain BLOCKED (farMC WQ full) addr=0xb000000 retained, wb_size=1
... (repeats once per operate() cycle, 100 times in the captured run,
     while farMC's pre-filled WQ works through its own real DRAM
     completion timing) ...
[DCM] WB drain BLOCKED (farMC WQ full) addr=0xb000000 retained, wb_size=1
[DCM] WB drain -> FAR_MEM_WRITE (dirty victim writeback) addr=0xb000000 remaining_wb_size=0
```

Matching this section's title exactly:

```
WB entry created           -> "[DCM] req#2 WB insert victim addr=0xb000000 wb_size=1"
  -> far queue full        -> (no log line -- absence IS the signal: no
                               "far dispatch"/"add_wq" line appears)
  -> WB entry retained     -> "[DCM] WB drain BLOCKED (farMC WQ full)
                               addr=0xb000000 retained, wb_size=1"
     (repeats every operate() cycle farMC stays full -- note wb_size
     stays at 1 throughout: the entry is truly UNTOUCHED, not
     re-queued or duplicated on each retry attempt)
  -> later cycle            -> (100 operate() cycles pass in the
                                captured run, while farMC's OWN real
                                DRAM completion timing works through
                                its pre-filled backlog)
  -> capacity available     -> (the exact cycle farMC's occupancy first
                                drops below its size -- no separate log
                                line for this; it's the condition
                                dispatchToFar()'s pre-check evaluates)
  -> far write accepted     -> "[DCM] WB drain -> FAR_MEM_WRITE (dirty
                               victim writeback) addr=0xb000000
                               remaining_wb_size=0"
  -> WB entry removed       -> (implicit in the line above: `WB.pop_front()`
                               happens immediately before this log line
                               is printed, and `remaining_wb_size=0`
                               confirms it)
```

Note what does NOT appear anywhere in this trace: no duplicate
`req#2 WB insert` line (the entry is inserted exactly once, at the
start), no second `-> FAR_MEM_WRITE` line for the same address (dispatch
happens exactly once, on success), and `wb_size=1` never changes to
anything else during the blocked period (the entry is genuinely
untouched while retained, not removed-and-re-added on each retry
attempt).

## All remaining DCM → memory dispatch paths (stage 10)

Full architectural writeup and gem5 correspondence in
`docs/memory_dispatch_audit.md`. This section captures the actual
request flow, directly from debug-print output (not hand-written), for
a near-memory tag-check READ whose dispatch is blocked because
`nearMC`'s RQ is completely full, then released once capacity frees up:

```
[DCM] req#1 READ addr=0xe000000 index=1572864 tag=1 arrival_cycle=0
[DCM] req#1 classify MISS victim_valid=0 victim_dirty=0 victim_addr=0x0 needs_writeback=0
[DCM] req#1 -> LOC_MEM_READ (tag check)
[DCM] near dispatch retry-queued (nearMC full) addr=0xe000000 READ
[DCM] near dispatch BLOCKED (nearMC queue full) addr=0xe000000 READ retained
... (repeats once per operate() cycle while nearMC's RQ stays full) ...
[DCM] near dispatch BLOCKED (nearMC queue full) addr=0xe000000 READ retained
[DCM] near dispatch released addr=0xe000000 READ cycle=11
[DCM] req#1 -> FAR_MEM_READ (miss fetch)
```

Matching the audit's design directly: `req#1`'s ORB entry transitions to
`DCM_WAITING_LOC_MEM_READ_RESP` (implicit in `-> LOC_MEM_READ (tag
check)`, per `driveState()`) BEFORE the dispatch is even attempted —
this is why the ORB state stays correct throughout the entire blocked
period, whether the underlying send happens this cycle or ten cycles
later. The FIRST attempt (`dispatchToNear()`'s own call to `trySend()`)
fails silently from this trace's perspective (no explicit log line for
that specific first attempt — only the resulting "retry-queued" line),
then `processPendingNearDispatches()` retries every subsequent cycle
("BLOCKED... retained") until capacity frees up, at which point it logs
"released" and `req#1` immediately proceeds to its next state
(`-> FAR_MEM_READ`), exactly as if the tag-check read had completed on
the very first attempt — the retry delay is entirely transparent to the
rest of the state machine.

Same pattern applies identically to the far-demand-read and
bypass-read/write paths (via `dispatchToFarGuaranteed()` and the
existing `pendingFarDispatches`/`processPendingFarDispatches()`
machinery already shown in the WB section above) and to the near
cache-fill write (via `dispatchToNear()`, logged identically to the
tag-check read shown here) — all six previously-unprotected paths now
share this same "retained, retried, released, transparent to the rest
of the state machine" shape.

## Same-address collision through CRB promotion (stage 13)

The defect fixed here (`docs/final_independent_audit.md` CRITICAL-1) was
a *dispatch-layer* aliasing bug, not a state-machine bug, so the flow
below is the ordinary CRB-promotion flow — what changed is only what
happens at the moment of re-dispatch.

```
LLC issues read A                -> add_rq(A): no conflict -> ORB[A], state LOC_MEM_READ
                                 -> dispatchToNear(A, read) -> nearMC RQ now holds A
LLC issues read/writeback A      -> add_rq/add_wq(A): checkConflictInORB(index(A)) == true
                                 -> CRB entry for A
... nearMC services A ...
MEMORY_CONTROLLER::process()     -> return_data(A)                     <-- A still IN nearMC's RQ
   DCM return_data(A)            -> completeRequest(ORB[A])
        completeRequest          -> response scheduled, ORB[A] erased
                                 -> promoteFromCRB(index(A))
                                      -> admitRequest(A) -> ORB[A] (new entry)
                                      -> driveState -> LOC_MEM_READ
                                      -> dispatchToNear(A, read)
                                           -> trySend -> nearMC->add_rq(A)
                                              *** A is STILL resident: add_rq MERGES ***
   (process() only now)          -> queue->remove_queue(A)
```

**Before the fix**, `trySend()` ignored `add_rq()`'s return value, so the
merge looked like a successful send: the new ORB[A] sat in
`DCM_WAITING_LOC_MEM_READ_RESP` forever and, because a live ORB entry
owns its DRAM-cache index, every later request to that index queued in
the CRB until it filled and was then rejected outright.

**After the fix**, `add_rq()` returning a non-negative index is
recognised as "merged, not enqueued":

```
   ... dispatchToNear(A, read) -> trySend -> add_rq(A) returns >= 0
                               -> stats.dispatchMergeRetries++
                               -> returns false
                               -> packet retained in pendingNearDispatches
   (process() completes)       -> queue->remove_queue(A)     <-- A now gone
   next operate() cycle        -> processPendingNearDispatches()
                               -> trySend succeeds, A enqueued for real
                               -> completes normally, ORB[A] retires
```

The retry is bounded to a single cycle, because the colliding entry is
removed immediately after `return_data()` returns. Observed directly in
`tests/test_dcm_duplicate_merge.cc` Test 4: **three** `add_rq` attempts
for A but exactly **two** serviced reads — one attempt was refused and
retried, and nothing was duplicated.

## Warmup-phase flow (stage 13)

During ChampSim's warmup phase the DCM deliberately runs a reduced path:

```
LLC request (warmup)
  -> DCM add_rq/add_wq
       -> all_warmup_complete < NUM_CPUS
       -> warmupTagUpdate(): tag/index/valid/dirty/farMemAddr installed
          using the SAME rules as classifyAndInstall()
       -> NO ORB entry, NO CRB, NO WB, NO DRAM timing, NO ROI statistic
       -> reads: return_data() immediately (ChampSim's warmup contract)
       -> writes: dropped after the metadata update (no response exists
          for a write in any phase)
warmup -> ROI boundary (main.cc)
  -> reset_cache_stats(L1I/L1D/L2C/LLC)   [pre-existing]
  -> uncore.DCM.resetROIStats()           [new: clears ROI counters,
                                           KEEPS tagMetadataStore warm]
ROI request to a warmed line
  -> add_rq -> full path -> classifyAndInstall() -> isHit == true
```

Bypass mode skips `warmupTagUpdate()` entirely — there is no DRAM cache
in that path to warm.

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

### Address transformation per hop

No hop below the CPU rewrites `PACKET::address`; the DCM's index/tag
derivation is the only place it is interpreted differently from a plain
tag comparison.

| Hop | `full_addr` | `address` | Transformation |
|---|---|---|---|
| CPU issues | `PA` | `PA >> 6` | set once, in `ooo_cpu.cc` |
| L1 → L2 → LLC | `PA` | `PA >> 6` | none — `get_set()`/`get_way()` consume `address` as-is |
| LLC → `lower_level` (DCM) | `PA` | `PA >> 6` | **none** — packet forwarded unmodified |
| LLC writeback → DCM | `PA` | `PA >> 6` | none — `block[set][way].address` is already a line address |
| DCM index/tag | — | `PA >> 6` | `index = address % DCM_NUM_LINES`, `tag = address / DCM_NUM_LINES` |
| DCM → near/far `MEMORY_CONTROLLER` | `PA` | `PA >> 6` | none — `dram_get_channel()` uses `shift = 0` |
