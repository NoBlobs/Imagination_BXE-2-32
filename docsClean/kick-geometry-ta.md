# Geometry (TA) kick — registers and ordering dependencies

Details the **geometry kick** (DM 2/6) at register level: **which registers must be
programmed**, **with what data**, and **which ordering dependencies are mandatory**. These
are hardware/behaviour facts, not a routine to copy: the code organisation is free as long
as the dependencies are respected. Complements [data-masters](data-masters.md) and
[command-structs](command-structs.md).

## Register notation

- **`CR[off]`**: register at offset `off` in a **core's** bank, in the window
  `0xCF800000 + core·0x80000` (see [memory-model](memory-model.md)).
- **`BCAST[off]`**: broadcast write (all enabled cores, bank `0xCFA00000`).

> **On the `CR[set+off]` notation used below (answers Q15).** The geometry registers below
> are at **fixed offsets** (`0xC00`, `0xC88`, `0x400`, …) in the bank of the core that runs
> the geometry (the **primary** core for DM 2; the second core of the active list for DM 6).
> There is **no** extra per-"set" displacement or stride beyond the per-core bank stride
> (`0x80000`): on this single-core SoC every such write lands in the primary core's bank.
> Read `CR[set+0xC00]` below as "offset `0xC00` in the geometry core's bank". The trace
> `Setup register set=N` reports the **BIF page-catalogue slot** chosen by the
> memory-context activation, not an addressing base for these registers. Observed (**O**):
> after `Setup register set=0 DM=2`, the context's page catalogue is in
> `ROGUE_CR_BIF_CAT_BASE1` and `ROGUE_CR_BIF_CAT_BASE_INDEX = 0x101` (`TA` and `PIXEL`
> fields = 1). So trace set *N* means catalogue register `CAT_BASE{N+1}` and index-field
> value *N+1*; `CAT_BASE0` belongs to the firmware. Compute and transfer bind the same way through
> `ROGUE_CR_BIF_CAT_BASE_INDEX` and do not use a separate addressing base either.
- Public names, where they exist, are from `pvr_rogue_cr_defs.h`. The public header names
  only a subset of the registers; the others are given by offset.

> **Provenance.** Offsets and dependencies: **D** (firmware analysis), confirmed by **O**
> (the milestones appear in the trace: `Activate MemCtx`, `Setup register set`, `Kick TA`).
> Values come from `rogue_fwif_cmd_geom` (public header) and from the context state.

## Inputs

The context (`rogue_fwif_fwcommoncontext`: memory context, register set), the geometry
command (`rogue_fwif_cmd_geom`, right after the 0x28-byte header in the cCCB) and the
command offset (used in the trace entry and on retirement).

## Phases and dependencies

The phases below have a **mandatory order**: each depends on the previous one.

### A. Preconditions

- The power domain must be **on** (wake the GPU if needed).
- On the first kick after a GPU-units initialisation, if the context's `rf_cmd_addr` is
  non-zero, the firmware first runs a compute job from it (see the transfer kick in
  [kick-compute-and-3d](kick-compute-and-3d.md)). The mainline host leaves it 0.
- The **memory context** is bound to a `ROGUE_CR_BIF_CAT_BASE` slot (1–7), which selects
  the **register set** to use. Trace: BIF id 14, `Activate MemCtx=… DM=… secure=…`. Must
  happen before any write to the register set.

> **Observation run (O) behind phases B, C and E.** Black-box observation on the board of
> one first-kick geometry job (`FIRSTKICK | LASTKICK`, from
> `dEQP-VK.draw.renderpass.simple_draw.simple_draw_triangle_list`, 256×256 target). The
> method compares the values in the geometry command (read from the cCCB while the job was
> live) and the addresses printed by the public trace with a read-only snapshot of the
> geometry core's registers, taken while the GPU was held powered. Firmware: the open image
> **v1.1, build 6976702** (the one this spec describes); the same mapping was also
> observed with the open image v1.0, build 6734358. Board DRAM at 1066 MT/s.

### B. Load the render target into the PM and synchronise

**The two PM contexts (O, public trace).** Geometry uses **PM context 0** (trace "HW
context 0", "PMDM0"). Fragment uses **PM context 1** ("HW context 1", "PMDM1"). The observed
order is:

1. **Before the geometry kick:** load the HWRTData's **local** and **global** freelists into
   PM context 0. Trace PM: `Load  Freelist … type: 0 (0:local,1:global) for PMDM0: FL Total
   Pages …, FL-addr = …, stacktop = …, Alloc Page Count = …`, then the same for type 1.
2. Load the HWRTData on HW context 0: RTD id 15, `TA RTData … loaded on HW context 0`.
3. **Before the fragment kick of the same HWRTData:**
   - **store** the freelists from PM context 0 (`Store Freelist … for PMDM0`). This is
     where the freelist state left by the geometry is captured; the trace shows
     `stacktop` 0x1F → 0x10 and `Alloc Page Count` 0 → 6.
   - **load** the freelists into PM context 1 (`Load Freelist … for PMDM1`).
   - load the 3D stack pointers (RTD id 12, `Loading stack-pointers for 1 (0:MidTA,1:3D) on
     context 1, MLIST = …, ALIST = …`).
   - `3D RTData … ready on HW context 1`.
4. **On HWRTData cleanup:** store the freelists back from both PM contexts.

**Registers observed holding the loaded PM state (O):**

| Register | Observed value | Matches |
|---|---|---|
| `0x0248`/`0x024C` | `0x80_0013_A000` | `rogue_fwif_hwrtdata.vheap_table_dev_addr` (trace "VHeap") |
| `0x0210`/`0x0214`, `0x0218`/`0x021C` | `0x80_0013_9000` (both) | local freelist `freelist_dev_addr` (trace "FL-addr"), one register pair per PM context (`TO CONFIRM` which is which) |
| `0x0224` | `0x1F` | local freelist stack top at rest (32 pages, trace "stacktop") |
| `0x0270`, `0x0278`, `0x0280` | `0x2` | ALIST stack pointer (trace "ALIST = 0x2") |
| `0x02C0`, `0x02C8`, `0x02D0` | `0x9` | MLIST stack pointer (trace "MLIST = 0x9") |
| `ROGUE_CR_BIF_PM_CAT_BASE_{VCE,TE,ALIST}0` and `…1` | the same three values in both banks | page-catalogue entries of the HWRTData, bank 0 = PM context 0, bank 1 = PM context 1 |

The global freelist's address (`0x80_0004_8900` in this run) was not found in the sampled
ranges. How the 4-entry arrays `vce_cat_base[4]`/`te_cat_base[4]` map to the single entry
per bank is `TO CONFIRM`.

- **PM handshakes** (**D**, not observed directly):
  - `CR[0x250] = 1`, then **poll** `CR[0x250]` until bit 0 = 1;
  - `CR[0x258] = 1`, then **poll** `CR[0x258]` until bit 0 = 0.

  The polls must complete before phase C.

### C. Program the geometry registers (command data)

**Observed (O)**, for a first kick. Each command field is matched by value with the register
that held the same value:

| Register (geometry core bank) | Observed value | Source |
|---|---|---|
| `0x0C88` | `0x202` | `rogue_fwif_geom_regs.ppp_ctrl` |
| `0x0C28` | `0x0002_0000` | `rogue_fwif_geom_regs.te_psg` |
| `0x0C38`/`0x0C3C` | `0x80_0013_E000` | `rogue_fwif_hwrtdata.rgn_header_dev_addr` (trace "TE Region headers base") |
| `0x0C40`/`0x0C44` | `0x80_0013_B000` | `rogue_fwif_hwrtdata.tail_ptrs_dev_addr` (trace "TPC") |
| `0x0408`/`0x040C` | `0x80_0013_F000` | `rogue_fwif_geom_regs.vdm_ctrl_stream_base` |
| `0x17A0`/`0x17A4` | `0x20_0001_C400` | `rogue_fwif_geom_regs.tpu_border_colour_table` (the fragment kick writes the same register) |

Other non-zero values in the geometry bank after the job, not matched to a field
(`TO CONFIRM`; the `rogue_fwif_hwrtdata_common` fields are the likely sources):

| Register | Value | Candidate |
|---|---|---|
| `0x0C98` | `0x00FF_00FF` | x/y maximum 255/255 of the 256×256 target (`screen_pixel_max`?) |
| `0x0C18` | `0x0000_F00F` | 15/15 in tiles of 16×16 (`te_screen`?) |
| `0x0C08`, `0x0C10` | `0x0010_0000` | — |
| `0x0C20` | `0x10` | — |
| `0x0C30`, `0x0C34` | `2`, `3` | — |
| `0x0C48` | `8` | — |
| `0x0C70` | `3` | — |
| `0x0C78`, `0x0C80` | `0x88` | — |
| `0x0C90`/`0x0C94` | `0x3089_705F` | — |
| `0x0CA0` | `0x10` | — |
| `0x0CE0` | `1` | — |
| `0x0D00` | `0x3F` | — |
| `0x0410`/`0x0414` | `vdm_ctrl_stream_base + 0x40` | — |
| `0x0418`/`0x041C` | `0x80_0006_D000` | — |

`vdm_context_resume_task0_size` (`0x2001_0000` in this job) was **not** found in any
sampled register. It is probably used only when resuming a stored context (`TO CONFIRM`).
`tpu`, `pds_ctrl` and `view_idx` were 0 in this job, so their registers could not be
identified by value.

> **Correction.** Earlier versions of this phase had a table derived from firmware analysis
> that placed `te_psg` at `0xC18`, `tpu` at `0xC20`, `pds_ctrl` at `0xC10`, the resume sizes
> at `0xC00`/`0xC08`, `tpu_border_colour_table` at `0xC80`, the VDM control-stream base at
> `0xC88…0xC98`, and listed `0xC28`, `0xC38` and `0xC40` as context state or polls. Where
> that table and the observation disagree, the **observation above wins**: `0xC28` is
> `te_psg`, `0xC38`/`0xC40` are the region-header and TPC addresses, `0xC88` is `ppp_ctrl`,
> and the VDM control-stream base is at `0x0408`. Rows that are neither confirmed nor
> contradicted are dropped. Trace: BIF id 19 `Setup register set=… DM=…`, RTD id 25/33
> `TA Buffers: …`, RTD id 24 `TE Region headers base …`.

### D. Context-switch state and lists

- A **new kick** and a **resume** (after preemption) program the TA *state buffer* and the
  stack pointers differently: `CR[set+0xCE0]`, `CR[set+0xCB0]` (poll),
  `CR[set+0xCB8] = 1`, `CR[set+0xC48]`, and `CR[0xD20]` = state buffer address (0 if
  none).
- On **resume**, the TA state buffer is **flipped** relative to the last store.
- After an **HWR**, the RTC cache and the TA caches are cleared before the kick
  (`rogue_fwif_hwrtdata.geom_caches_need_zeroing`).
- `CR[0x40A8] = (primary core) & 1` (core/TE selection).

### E. Enable the context and broadcast the configuration

- `CR[set+0xC68] = 1`, then **poll** `CR[set+0xC68]` until bit 0 = 1 (context
  acceptance). Must complete before the trigger.
- `0x0408`/`0x040C` receive the VDM control-stream base (`vdm_ctrl_stream_base`, observed
  in phase C), not a freelist/ALIST configuration as earlier versions said. The pair
  `0x0418`/`0x041C` also holds an address after the job (`TO CONFIRM` which).

### F. Trace, timing and watchdog (before the trigger)

- Trace entry MAIN id 119: `Kick TA: FWCtx … @ …, RTD …, First kick:…, Last kick:…, CSW
  resume:…. (PID:…, prio:…, frame:…, ext:…, int:…)`.
- Read `ROGUE_CR_TIMER` (`0x0160`), arm the DM's **deadline watchdog** (see
  [hardware-recovery](hardware-recovery.md)) and record the DM's execution state
  (context, offset, kick time, deadline).

### G. Trigger

- **`CR[set+0x400] = 1`**: the TA *run* register. It is **the last write**: everything
  above must be programmed and accepted before it.

## Completion

The GPU signals in `ROGUE_CR_EVENT_STATUS` (HW2/IRQ). Geometry completion handling (trace
MAIN id 8, `TA finished`) updates the HWRTData and freelist, flushes the TPC, retires the
command and processes the updates (see [client-ccb](client-ccb.md) and
[scheduler](scheduler.md)).

**Which events reach the firmware (part of Q19).** The boot routing for DM 2
(`ROGUE_CR_MTS_DM2_INTERRUPT_ENABLE = 0xA0`, see [boot-gpu-init](boot-gpu-init.md)) enables
only `TA_FINISHED` (bit 5) and `PM_OUT_OF_MEMORY` (bit 7). `TA_TERMINATE` (bit 6, public
name) is **not** routed to the IRQ task, so a firmware that follows this routing completes
geometry on `TA_FINISHED` alone. When the hardware raises `TA_TERMINATE` is not
established by this spec.

**Freelist write-back (part of Q19, O).** The freelist state reached by the geometry is
captured when the freelists are **stored from PM context 0 at the fragment kick** of the same
HWRTData (phase B, step 3), not at `TA finished`. The fields involved are the ones the trace
prints: stack top, current pages, allocated page count. Geometry completion itself is traced
as `TA finished` then `TA RTData … finished on HW context 0`.

`PENDING`: the registers the PM state is read back from, the HWRTData fields updated after
the TA, the context registers of phase D, and the full geometry variant of the completion
cache flush.
## Confidence and pending items

- **High confidence:** the phases and their dependencies (preconditions → PM → register
  set → CSW → enable → watchdog → **trigger last**), the listed offsets and the mandatory
  polls. The `rogue_fwif_geom_regs` field → register mapping in Phase C (Q4) is resolved
  above.
- The VDM control pair (`CR[set+0xC88]`…`+0xC98`) mixes the host `vdm_ctrl_stream_base`
  with firmware context state; the exact split of the 64-bit base across `+0xC90`/`+0xC98`
  is a detail the implementer can finish from the public field widths.

The 3D, transfer and compute kicks follow the same skeleton. See
[kick-compute-and-3d](kick-compute-and-3d.md).
