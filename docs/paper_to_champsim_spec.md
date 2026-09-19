# Paper → ChampSim Behavioral Spec

This is the paper-level functional spec (not gem5-code-level — see
`gem5_to_champsim_mapping.md` for that) that the ChampSim port must
reproduce wherever ChampSim's trace-based nature allows it. It was
missing from the original doc set (the master project instruction listed
it) and is added now as part of re-checking the skeleton against all
required references.

## Source

Primary: arXiv:2303.13029 ("Enabling Design Space Exploration of DRAM
Caches in Emerging Memory Systems"), Sections II–VI. Secondary: the
ISPASS'23 companion abstract (same model, condensed).

## Functional requirements (paper Section II) and current status

| Requirement | Paper description | ChampSim status |
|---|---|---|
| DRAM Cache Manager as drop-in replacement for the memory controller | Sits outside coherence domain, receives all traffic from cores/LLC/DMA | **Done**: `DRAM_CACHE_MANAGER` is `uncore.LLC.lower_level` |
| No shared data bus requirement | Near and far memory need not share a bus | **Done**: `nearMC`/`farMC` are fully independent `MEMORY_CONTROLLER` objects, independent queues |
| ORB (Outstanding Requests Buffer) | All incoming requests reside here unless conflicting | **Done**: `std::map<addr, DCM_ORB_ENTRY*>`, size-limited to 128, admission-gated |
| CRB (Conflicting Requests Buffer) | Requests conflicting on DRAM-cache location wait here until the blocker retires, then are promoted | **Done**: index-based conflict detection, FIFO-per-index promotion, size-limited to 32, backpressure reported via `get_occupancy`/`get_size` |
| WB Buffer | Dirty evicted lines queued for write-back to backing store | **Done this stage**: real dirty-victim detection triggers `pushDirtyWriteBack()` at tag-check-read completion; insert+drain wired and verified (Table II cases E/H). Occupancy-based admission backpressure not yet ported |
| Tag/metadata co-located with data | Baseline: ECC-bit storage, so any tag check reads the whole line | **Done this stage**: `tagMetadataStore` fully consulted and updated on every request via `classifyAndInstall()`, ordering matches gem5's `handleRequestorPkt` exactly |
| Direct-mapped, 64B lines, insert-on-miss, write-back | Baseline cache organization | **Done**: real hit/miss/dirty classification + eager install/replacement, verified against all 8 Table II cases. **64 B granularity and 128 MiB capacity corrected in stage 15** — see "ChampSim PACKET::address convention and DCM normalization" below |
| Miss handling requires multiple accesses to local+far interfaces | Central performance-pathology claim of the paper | **Done this stage, verified**: exact operation counts (Table II) reproduced for all 8 cases, including the "up to 4/5 accesses" dirty-miss pathology (Case E: local-read+far-write+far-read+local-write=4) |
| Independent near/far memory technologies (HBM2 vs DDR4/NVM) | Case Study 1 baseline; Case Study 3 varies far tech and link latency | **Fixed this stage**: `MEMORY_CONTROLLER` timing is now per-instance (`set_timing()`), so near/far *can* diverge. They are not yet *actually* diverged in `main.cc` (both get identical default timing) — that's a deliberate, documented follow-up (see `gem5_to_champsim_mapping.md`), not a limitation of the mechanism anymore |
| Configurable far-memory link latency (100/500/1000ns, Case Study 3) | Additional latency on the far-memory path only | `DCM_LINK_LATENCY_CYCLES_*` constants defined, **not yet applied anywhere** |
| Three selectable policies: baseline (CascadeLakeNoPartWrs), BEAR-Wr-Opt, Oracle | Case Study 2 | `DCM_POLICY` enum exists; **none of the three transition tables implemented** (explicitly out of scope for this stage and the previous one) |
| Table II access-amplification per {read,write}×{hit,miss}×{clean,dirty} | The paper's own correctness spec for any implementation of this model | Not yet automated as tests — blocked on real hit/miss/dirty detection (next_task.md item 3) |

## Request cases from the original master instruction — status

1. **READ HIT — DONE** (`test_dcm_baseline.cc` Case A), from real tag store
2. **WRITE HIT — DONE** (Case B), from real tag store
3. **READ MISS + INVALID/COLD — DONE** (Case C, + request-flow log)
4. **READ MISS + CLEAN VICTIM — DONE** (Case D)
5. **READ MISS + DIRTY VICTIM — DONE** (Case E, + request-flow log)
6. **WRITE MISS + INVALID/COLD — DONE** (Case F)
7. **WRITE MISS + CLEAN VICTIM — DONE** (Case G)
8. **WRITE MISS + DIRTY VICTIM — DONE** (Case H, + request-flow log)
9. **Conflicting requests — DONE** (`test_dcm_skeleton.cc` Test 5, with
   request-flow log; now also exercises a *real* classification of the
   promoted request, not a hand-set one)
10. **ORB pressure/full — DONE**
11. **CRB pressure/full — DONE** (Test 6)
12. WB pressure/full — WB insert/drain is wired, but nothing yet makes
    the manager back off when it's "full" (it drains immediately, so it
    never actually backs up) — this is the one remaining item, tracked in
    `next_task.md`.

All 12 request cases from the original master instruction are now either
fully done (1–11) or partially done with the remaining gap identified
(12).

## Table I parameters — status after this stage

See `gem5_to_champsim_mapping.md`'s parameter table. Unchanged by this
stage except the WB Buffer row (dirty write-back itself now wired; only
its admission-pressure check remains).

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
