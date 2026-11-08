# Geometry (TA) kick — registers and ordering dependencies

Details the **geometry kick** (DM 2/6) at register level: **which registers must be
programmed**, **with what data**, and **which ordering dependencies are mandatory**. These
are hardware/behaviour facts, not a routine to copy: the code organisation is free as long
as the dependencies are respected. Complements [data-masters](data-masters.md) and
[command-structs](command-structs.md).

## Register notation

- **`CR[off]`**: register at offset `off` in the **primary core's** bank, in the window
  `0xCF800000 + core·0x80000` (see [memory-model](memory-model.md)).
- **`CR[set+off]`**: register of the **register set** assigned to the context (index 1–7);
  the set's displacement is part of the offset.
- **`BCAST[off]`**: broadcast write (all enabled cores, bank `0xCFA00000`).
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
- If the configuration requires it, the USC local memory is cleared first (a small
  compute job run by the firmware itself).
- The **memory context** is bound to a `ROGUE_CR_BIF_CAT_BASE` slot (1–7), which selects
  the **register set** to use. Trace: BIF id 14, `Activate MemCtx=… DM=… secure=…`. Must
  happen before any write to the register set.

### B. Load the render target into the PM and synchronise

- The command's HWRTData is loaded into the PM, for the geometry buffer and for the list
  (MLIST). Trace: RTD id 15, `TA RTData … loaded on HW context …`.
- **PM handshakes** (pause and resume while the state changes):
  - `CR[0x250] = 1`, then **poll** `CR[0x250]` until bit 0 = 1;
  - `CR[0x258] = 1`, then **poll** `CR[0x258]` until bit 0 = 0.

  The polls must complete before phase C.

### C. Program the register set (command data)

The values come either from the **host-prepared `rogue_fwif_geom_regs`** block inside the
command (field names below, from the public header — this resolves the earlier `PENDING`
and answers the clean team's Q4) or from **firmware-maintained context state** (the VDM
context-resume addresses the firmware tracks per context — these are **not** in the
command):

| Register | Source | `rogue_fwif_geom_regs` field (or state) |
|---|---|---|
| `CR[set+0xC18]` | command | `te_psg` |
| `CR[set+0xC20]` | command | `tpu` |
| `CR[set+0xC10]` | command | `pds_ctrl` |
| `CR[set+0xC00]` | command | `vdm_context_resume_task0_size` |
| `CR[set+0xC08]` | command | `vdm_context_resume_task3_size` (unused on this GPU: feature `VDM_OBJECT_LEVEL_LLS` absent) |
| `CR[set+0xC80]`/`+0xC84` | command | `tpu_border_colour_table` (first kick) or `vdm_draw_indirect0` (resume) — selected by a flag |
| `CR[set+0xC88]` … `+0xC98` | command + state | VDM control-stream base (`vdm_ctrl_stream_base`) and its control (with a command flag field) |
| **poll** `CR[set+0xC80]` | — | acceptance |
| `CR[set+0xC28]` | state | context RTData address in the PM |
| **poll** `CR[set+0xC38]`, `CR[set+0xC40]` | — | acceptance (both loaded from context state) |
| `CR[set+0x4098]` (byte) | command | TE partition/region configuration |

`ppp_ctrl` (`geom_regs` offset 0x1C) is consumed by the PM/HWRTData load path, not this
register set. Each poll must complete before the writes that follow it; otherwise the order
between independent writes is free. Trace: BIF id 19 `Setup register set=… DM=…`, RTD id
25/33 `TA Buffers: …`, RTD id 24 `TE Region headers base …`.

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
- Freelist/ALIST configuration broadcast to all cores: `BCAST[0x0408]`, `BCAST[0x040C]`
  (and the pair `CR[0x418]` on the primary core).

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
