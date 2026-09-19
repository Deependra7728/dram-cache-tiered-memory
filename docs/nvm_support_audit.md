# NVM Far-Memory Support Audit (Case Study 3, second configuration)

**No code changes in this stage.** This is an audit only, per explicit
instruction. Sources re-read for this audit: both papers (arXiv
2303.13029 full text, pulled and read page-by-page — not from memory —
including Section VI "Case Study 3: Impact of Link Latency"; the
ISPASS'23 companion is the same work, no separate NVM content found
there beyond what the arXiv version already covers), the actual pulled
`dram_cache_disaggregated` branch source (`policy_manager.cc/.hh`,
`PolicyManager.py`, `disaggregated_dram_cache_script.py`,
`NVMInterface.py`, `nvm_interface.hh`), the current ChampSim
`MEMORY_CONTROLLER` implementation (`inc/dram_controller.h`,
`src/dram_controller.cc`), and `docs/final_paper_coverage_audit.md`,
`docs/gem5_to_champsim_mapping.md`, `docs/limitations.md`,
`docs/next_task.md`.

## 1. What NVM technology/model does gem5 use?

`src/mem/NVMInterface.py` defines a generic `NVMInterface` SimObject
("The following interface aims to model byte-addressable NVM... without
getting into too much detail of the media itself") and a concrete
device profile derived from it, `NVM_2400_1x64`:

```python
# NVM delays and device architecture defined to mimic PCM like memory.
# Can be configured with DDR4_2400 sharing the channel
class NVM_2400_1x64(NVMInterface):
    ...
    tCK = "0.833ns"        # 1200 MHz
    tREAD = "150ns"
    tWRITE = "500ns"
    tSEND = "14.16ns"
    tBURST = "3.332ns"
    tWTR = "1.666ns"
    tRTW = "1.666ns"
    tCS = "1.666ns"
    write_buffer_size = 128
    read_buffer_size = 64
    max_pending_writes = 128
    max_pending_reads = 64
    device_rowbuffer_size = "256B"
    device_bus_width = 64
    ...
```

Its own comment says exactly what it is: **"mimic PCM like memory"**,
explicitly designed to be bandwidth-comparable to a DDR4-2400 channel
("can be configured with DDR4_2400 sharing the channel") while having
device-level read/write latencies an order of magnitude higher and
asymmetric between read and write.

**Critical provenance finding:** `NVMInterface.py`'s copyright header is
`Copyright (c) 2020 ARM Limited` — this is a **generic, pre-existing
gem5-core SimObject, unrelated in authorship and origin to this paper's
own `PolicyManager`/DRAM-cache work** (which is `Copyright (c) 2023 The
Regents of the University of California`, Babaie/Akram/Lowe-Power). It
was contributed to gem5 mainline for unrelated NVM research, then
happened to be available for reuse by this paper's authors, exactly the
way they reused gem5's pre-existing `DDR4Interface`/`HBMInterface`
device models.

**Second critical finding:** the ONLY config script committed to the
`dram_cache_disaggregated` branch,
`disaggregated_dram_cache_script.py` (the reference config for this
exact paper's model — re-pulled and read in full for this audit), does
**NOT** instantiate NVM anywhere. It imports `NVMInterface`
(`from m5.objects.NVMInterface import *`, line 32) but the import is
unused — `system.far_mem_ctrl.dram` is set to `DDR4_2400_16x4(...)`
only (line 96), never to any `NVMInterface`/`NVM_2400_1x64` instance.
Checked the other two branches in the same repo
(`dram_cache_CascadeLake`, `stable`) for an alternate NVM-using config
script: neither contains `disaggregated_dram_cache_script.py` at all.
**The specific gem5 config actually used to produce this paper's Figures
12b/13b (NVM remote memory) is not present in the public reference
repository.** The paper's Case Study 3 NVM results were necessarily
produced with a config that substitutes an `NVMInterface`-derived
SimObject for `system.far_mem_ctrl.dram` — almost certainly
`NVM_2400_1x64` given the paper's own description matches it closely
(see Section 3 below) — but this is an informed inference from the only
gem5-committed NVM device profile that fits the paper's description, not
a citation of a script that was actually run and published.

## 2. What parameters are explicitly specified by the paper?

Quoted directly from the pulled PDF (Section VI, "Case Study 3: Impact
of Link Latency"):

> "The DRAM cache model we described in this paper is capable of
> employing any memory technologies that are modeled in gem5, including
> DDR3, DDR4, HBM1, HBM2, and NVM... In a second case, we change the
> remote main memory to a single channel of NVM. **The DDR4 and NVM
> models of gem5 provide 19.2 GB/s theoretical peak bandwidth per single
> channel, though NVM has higher read and write latencies than DDR4.**
> We also add a link between the DRAM cache manager and the remote
> backing store with a configurable latency... three different cases
> where the round-trip latency of the link will be set to 100 ns, 500
> ns, and 1000 ns."

And from "Results and Discussions":

> "The differences between the read and write latencies of the DDR4 and
> NVM devices can explain the case for 100 ns link latency... [With
> increased link latency,] this becomes more critical for NVM main
> memory systems due to their **limited write buffer**. Pressure on the
> remote main memory increases [as link latency grows]... the write
> buffer of NVM devices [citing reference [5], Wang et al., MICRO 2020]
> and the high link latency to access the remote NVM[] create back
> pressure on the NVM backing store."

**Paper-defined, exact, disclosed numbers for the NVM case:**
- Peak bandwidth: **19.2 GB/s** — explicitly stated to be IDENTICAL to
  the DDR4 case (same number Table I already gives for "Main Memory
  (DDR4/NVM)").
- Link round-trip latency: **100 / 500 / 1000 ns** — same three values
  as the DDR4 sweep, already fully implemented in this port
  (`DCM_LINK_LATENCY_CYCLES_{100,500,1000}NS`).

**Not disclosed by the paper as numbers at all:**
- The actual NVM read latency, write latency, or any other device
  timing parameter. The paper only ever says "higher read and write
  latencies than DDR4" (qualitative) and attributes the underlying
  device characterization to an external reference ([5], not this
  paper's own measurement or gem5 configuration choice disclosed in
  text). No ns figure for NVM read or write latency appears anywhere in
  the paper's text, table, or figure captions.

## 3. What additional parameters does gem5's NVM SimObject supply?

Everything in the `NVM_2400_1x64` listing under Section 1 above that is
NOT bandwidth or link latency — i.e., every device-level timing and
buffering parameter:

| Parameter | Value | Paper-defined? |
|---|---|---|
| `tREAD` (average read latency) | 150 ns | **No** — gem5-defined only |
| `tWRITE` (average write latency) | 500 ns | **No** — gem5-defined only |
| `tSEND` (access/command latency) | 14.16 ns | **No** — gem5-defined only |
| `tBURST` | 3.332 ns | **No** — gem5-defined only |
| `tCK` (clock period) | 0.833 ns (1200 MHz) | **No** — gem5-defined only |
| `tWTR`/`tRTW`/`tCS` (turnaround delays) | 1.666 ns each | **No** — gem5-defined only |
| `write_buffer_size` / `read_buffer_size` | 128 / 64 | **No** — gem5-defined only |
| `max_pending_writes` / `max_pending_reads` | 128 / 64 | **No** — gem5-defined only |
| `device_rowbuffer_size` | 256 B | **No** — gem5-defined only |
| `device_bus_width`, `devices_per_rank`, `ranks_per_channel`, `banks_per_rank`, `burst_length` | 64 / 1 / 1 / 16 / 8 | **No** — gem5-defined only |
| `two_cycle_rdwr` | `True` | **No** — gem5-defined only |

None of these appear in the paper. All are gem5-core defaults for a
generic "PCM-like" device, carried over unmodified from the unrelated
2020 ARM contribution — i.e., **ChampSim-assumed would be the wrong
label for any of these if reused; the honest label is gem5-generic,
paper-unconfirmed.**

## 4. Which of those parameters exist in current ChampSim?

`MEMORY_CONTROLLER::set_timing()` (`inc/dram_controller.h:101-108`,
verified by direct read) takes exactly five parameters:

```cpp
void set_timing(uint32_t v_tRP, uint32_t v_tRCD, uint32_t v_tCAS,
                 uint32_t v_DRAM_MTPS, uint32_t v_DRAM_DBUS_RETURN_TIME)
```

That is: row-precharge (`tRP`), row-activate (`tRCD`), column-access
(`tCAS`), a transfer rate (`DRAM_MTPS`, used for bandwidth/dbus
scheduling), and a derived dbus return time. **There is no read/write
split anywhere in this signature or in how it is used.**

Confirmed by direct read of the actual latency computation
(`src/dram_controller.cc:218-225`, the shared function both `add_rq`'s
and `add_wq`'s scheduling paths funnel through):

```cpp
uint64_t LATENCY = 0;
if (row_buffer_hit)
    LATENCY = tCAS;
else
    LATENCY = tRP + tRCD + tCAS;
...
bank_request[op_channel][op_rank][op_bank].cycle_available = current_core_cycle[op_cpu] + LATENCY;
```

`LATENCY` is computed identically regardless of whether the request came
from `RQ` (read) or `WQ` (write) — `queue->is_WQ` is checked
*afterward*, only for bookkeeping (`scheduled_writes`/`ROW_BUFFER_HIT`
counters), never to select a different latency value. **This is an
architectural fact, not a configuration gap**: there is no parameter
combination of the existing five `set_timing()` inputs that can make
reads and writes take different amounts of time on the same
`MEMORY_CONTROLLER` instance.

Mapped against the table in Section 3: **bandwidth (`DRAM_MTPS`) is the
only NVM-relevant parameter that has a direct ChampSim equivalent.**
Every device-level read/write-asymmetric timing parameter, the
write-buffer-depth distinction, and the burst/turnaround breakdown have
no equivalent at all.

## 5. Which can be represented directly?

- **Peak bandwidth (19.2 GB/s).** Exact, paper-defined, and already
  numerically identical to what `configureFarAsDDR4()` uses (`DCM_FAR_DDR4_MTPS
  = 2400`, the same 2400 MT/s the paper states for BOTH DDR4 and NVM).
  An `configureFarAsNVM()` could set `mtps = DCM_FAR_DDR4_MTPS` (or an
  identically-valued constant) with zero invention — this part of the
  paper's own NVM description is, by the paper's own words, not
  different from DDR4 at all.
- **Link latency sweep (100/500/1000 ns).** Already fully implemented
  (`setLinkLatency()`, Stage 4) and applies identically regardless of
  which far-memory profile is active — no NVM-specific work needed here
  at all.

## 6. Which require adaptation?

- **"NVM has higher [device] latency than DDR4"** as a single,
  undifferentiated (not read/write-split) bump to `tRCD`/`tCAS` — this
  is the only way to make an NVM profile "feel slower" than DDR4 given
  ChampSim's architecture (Section 4). It would require SOME elevated
  numeric value, and no faithful one exists: the only concrete gem5
  numbers available (`tREAD`=150ns, `tWRITE`=500ns) are gem5-generic
  defaults for an unrelated PCM device, not confirmed as the actual
  values behind this paper's own NVM figures (Section 1's provenance
  finding), and averaging/blending two asymmetric numbers into one
  symmetric one to fit ChampSim's model would itself be an invented
  synthesis, not an adaptation of a disclosed number.

## 7. Which cannot be represented faithfully?

- **Read/write latency asymmetry itself.** This is not a "we haven't
  gotten to it yet" gap — `MEMORY_CONTROLLER`'s bank-timing pipeline
  (Section 4) has no code path that could apply a different `LATENCY`
  for a read versus a write on the same controller instance without a
  structural change to `dram_controller.cc` (adding, at minimum, a
  second `tRCD_write`/`tCAS_write`-style pair threaded through the
  `is_WQ` branch already present in the scheduling loop).
- **The write-buffer-back-pressure mechanism the paper explicitly
  attributes its NVM finding to.** The paper's own causal explanation
  for why NVM degrades LESS than DDR4 under high link latency is not
  "NVM is uniformly slower" — it is specifically that NVM's *limited
  write buffer* creates back pressure that interacts with link latency
  in a particular way (quoted in Section 2). ChampSim's write queue
  (`DRAM_WQ_SIZE`, `farMC`'s `WQ[channel].SIZE`) has no NVM-specific
  smaller-capacity variant modeled anywhere in this port, and even if
  one were added, reproducing the paper's *directional* finding (NVM
  degrades less, not more) would depend on getting the read/write
  asymmetry right first — which Section 7's prior bullet already rules
  out as unfaithful. **A single elevated symmetric latency, with no
  write-buffer distinction, would not reproduce this mechanism at all —
  it could not even reproduce the paper's DIRECTION of effect (NVM
  losing less throughput than DDR4 as link latency grows), since a
  purely slower-and-symmetric device has no structural reason to behave
  that way.**

## 8. Would implementing an NVM profile improve paper reproduction, or require inventing unsupported parameters?

**It would require inventing or misattributing parameters, and even
granting that, it would not reproduce the paper's actual NVM finding.**
Concretely, any `configureFarAsNVM()` built on ChampSim's current
`MEMORY_CONTROLLER` faces an unavoidable choice among three options,
none of which improve reproduction fidelity:

1. **Bandwidth-only NVM (no latency change from DDR4).** Exact and
   invention-free, but behaviorally IDENTICAL to `configureFarAsDDR4()`
   in this port — provides a differently-named function that does
   nothing different, which is worse than not having the function at
   all (it would look like NVM support exists when it has zero
   behavioral effect).
2. **Bandwidth-identical, symmetrically elevated latency (adapted from
   gem5's generic `tREAD`/`tWRITE` somehow).** Requires either inventing
   a blended value (not a real number from any source) or citing one of
   gem5's two asymmetric numbers as if it applied to both directions
   (misattribution) — and even done "successfully," this cannot
   reproduce the paper's actual reported direction of effect (Section
   7), so it would not improve reproduction, it would produce a
   plausible-looking but behaviorally wrong result.
3. **A structural `MEMORY_CONTROLLER` change to support asymmetric
   read/write latency plus a distinct write-buffer cap for `farMC`.**
   The only option that could genuinely reproduce the mechanism — but
   this is a `dram_controller.cc` architecture change (not a
   `dram_cache_manager.cc`-scoped addition like every other DCM feature
   ported so far), touches code shared by `nearMC` as well as `farMC`
   and by every other in-flight ChampSim DRAM controller use, and would
   still need to source its actual `tREAD`/`tWRITE` numbers from
   somewhere — most defensibly gem5's own `NVM_2400_1x64` defaults,
   cited explicitly as "gem5-generic, not paper-confirmed" (Section 1's
   provenance finding), since no paper-disclosed or paper-specific
   gem5-config number exists to cite instead.

## Can Case Study 3 still be meaningfully evaluated without NVM?

**Yes — the paper's own results show the primary Case Study 3 claim
does not depend on the NVM comparison at all.** Case Study 3's stated
research question (quoted from the paper) is:

> "what is the impact of latency of the link between local DRAM caches
> and remote backing stores on the performance of systems?"

This question is answered by the **DDR4 sweep alone** (paper Figure 13a):
at 100ns link latency the DRAM cache underperforms a no-DRAM-cache DDR4
system (consistent with Case Study 1's baseline finding), but "once the
link latency increases toward 500ns and 1000ns... the DRAM cache with
DDR4 far main memory outperforms the same system without the DRAM
cache" — the paper's headline Case-Study-3 conclusion ("DRAM caches
become worthwhile once the link to remote memory is slow enough") is
fully demonstrated with DDR4 alone, and **this exact sweep (100/500/1000
ns link latency over a DDR4 far memory, with all three policies) is
already fully implemented and validated in this port**
(`tests/test_dcm_link_latency.cc`, `configureFarAsDDR4()`).

The NVM comparison is a **secondary, additional finding** — "NVM can
perform close to DDR4 devices [at long link latencies]... an interesting
use case to utilize large capacity of NVM devices" (paper's own
Case-Study-3 "Takeaway") — genuinely informative, but not the load-bearing
claim the case study exists to establish, and (per Section 8) not one
this port's current architecture can faithfully reproduce even if
implemented.

## Decision

**D — DO NOT IMPLEMENT**, because the specific mechanism the paper's
NVM finding depends on (asymmetric read/write device latency creating
differential write-buffer back-pressure) cannot be represented in
ChampSim's current `MEMORY_CONTROLLER` architecture without either (a)
inventing or misattributing timing numbers the task explicitly prohibits
fabricating, or (b) a nontrivial structural change to shared DRAM
controller code that, even if done, would still depend on citing
gem5-generic (not paper-confirmed) device defaults as the closest
available source — and Section 1's provenance finding shows the actual
config that produced the paper's own NVM figures is not present in the
public reference repository to verify against in the first place. This
is not blocking: Case Study 3's primary, headline conclusion (impact of
link latency on DRAM-cache viability) is already fully and faithfully
reproducible with the DDR4-only sweep already implemented in this port.

**If a future decision reverses this** (e.g., if the user decides an
openly-labeled, imperfect approximation has value for exploratory DSE),
what would need to be implemented, in order:

1. A `MEMORY_CONTROLLER` timing model change to support distinct
   read-latency and write-latency parameters (new fields + a
   `set_timing()` overload or additional setter; the `LATENCY`
   computation in `dram_controller.cc:221-225` would need an `is_WQ`
   branch to select between them).
2. A distinct, smaller write-buffer-depth cap specifically for an
   NVM-configured `farMC`, to give the write-buffer-back-pressure
   mechanism something to act on (reusing this port's own
   `DCM_WB_PRESSURE_THRESHOLD`-style admission-throttle pattern, keyed
   to `farMC`'s occupancy rather than the DCM's own WB).
3. `configureFarAsNVM(MEMORY_CONTROLLER &mc)`, paired with
   `configureFarAsDDR4()`, using `DCM_FAR_DDR4_MTPS` for bandwidth
   (exact, paper-stated) and gem5's `NVM_2400_1x64` `tREAD`/`tWRITE`
   values for the two new latency parameters, **explicitly documented as
   gem5-generic-PCM-device defaults, not paper-confirmed or
   paper-specific numbers** (per this audit's Section 1/3 findings).
4. Deterministic tests proving the read/write asymmetry applies exactly
   once per operation and produces the paper's claimed DIRECTIONAL
   effect (NVM degrading less than DDR4 as link latency grows) under a
   representative write-heavy workload — without such a test, "NVM
   support" would only be a labeled config with no evidence it reproduces
   anything.
5. Documentation carrying forward this audit's provenance caveat
   explicitly, so no future reader mistakes gem5-generic numbers for
   paper-verified ones.
