<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# Questions to the spec maintainer (dirty team)

Gaps found while implementing from `docsClean/`. Each one is a functional question,
following the question flow in `docsClean/clean-room-process.md`. Until a question is
answered, the implementation uses the stated assumption.

## Open

### Q14 — the first tile of a multi-tile TQ_3D is not written

With the Q13 table applied exactly, TQ jobs complete without faults ("3D Transfer
finished"), and single-tile transfers are correct. For a transfer spanning several
16×16 tiles, the **first tile is not written** and the others are
([E022](logRefs/E022-tq3d-completes-first-tile-lost.md)). Example,
`fill_buffer_second_part`:
- `frag_screen = 0x1f` (32×1);
- `isp_mtile_size = 0x00020001`, `isp_ctl = 0x21000`, `isp_render = 3`;
- PBE word0/word1 = `0x0200003c96800000` / `0x0000001f00000000`;
- `event_pixel_pds_info/code/data = 0x202/0x2620/0x2600`; no background program.

Result: px 48–63 filled, px 32–47 untouched. It happens on some runs but not all
(fill group 18 → 6 passes between two identical builds).

Changing any of the following makes no difference:
- forcing tiles in flight (`0x0FD8`) to 1;
- waiting 2 ms after `PIXELBE_END_RENDER`;
- omitting `ISP_RENDER |= 0x10` (`0x0F00` alone launches);
- flushing the SLC before the kick.

Questions:
- Is anything per-kick needed before the first tile can emit? For example a
  pixel-event/PBE state reset, the 3D phase-D "pipe reset" writes (`ISP_RENDER = 0`,
  `0x0618 = 0x80`, `0x0F10 = 0`) before programming, or a USC/PDS flush between jobs.
- Does the end-of-tile emit of tile 0 depend on state left by the previous job on DM 3?
  The previous TQ of the same context, or the compute job just before, for instance.
- Is there a firmware action at the end of a TQ, before the SLC flush, that makes the
  first tile's PBE writes visible (a PBE/pixel-backend flush register)?

## Answered

| Q | Answer | Where |
|---|---|---|
| Q1 MTS init / event routing | `MTS_DMn_INTERRUPT_ENABLE` (DM0 `0x89200`, DM2 `0xA0`, DM3 `0x08`, DM4 `0x04`), `EVENT_ENABLE = 0`, `INTCTX = 0xFFFF`, `BGCTX = 1`. **Implemented**, but see Q9. | `docsClean/boot-gpu-init.md` B/C; [E011](logRefs/E011-event-enable-0x0128.md) |
| Q2 device info | values of the open image (SLC 16 KiB, 6 ERNs, `LAYOUT_MARS`, `XE_MEMORY_HIERARCHY`, …). **Implemented** in `tools/mkfw.py`. | `docsClean/memory-model.md`; [E001](logRefs/E001-sys-bus-secure-reset.md) |
| Q3 remap order | low word, high word, read back the high word. **Implemented.** | `docsClean/memory-model.md` |
| Q4 geometry register set | `rogue_fwif_geom_regs` field → `CR[set+0xC..]` mapping | `docsClean/kick-geometry-ta.md` (not implemented yet) |
| Q5 3D start | RMW `ISP_RENDER \| 0x10`, readiness `(ISP_RENDER & 3) == 3`. Tried for TQ, see Q8. | `docsClean/kick-compute-and-3d.md` |
| Q6 dust power | firmware-controlled: `0x0050` mask, `0x0038` request plus `POWER_COMPLETE` poll. **Implemented** at boot; per job only with `POW_RASCALDUST`, which mainline does not set. | `docsClean/boot-gpu-init.md` D |
| Q8 TQ_3D start | second launch write `BCAST[0x0F00] = 1`, mandatory BIF gate, `0x06B0` not written, completion on `PIXELBE_END_RENDER`. **Implemented: the pipe starts.** Output is still wrong, see Q12. | `docsClean/kick-compute-and-3d.md`; [E017](logRefs/E017-tq3d-launch-fault-at-zero.md) |
| Q9 IRQ task | section-B routing is enough; end with `0x0B08 = 2` and a read-back; no "which DM" register. **Confirmed**: HW2 fires on every completion; E016 was a race. | `docsClean/boot-gpu-init.md` B′; [E018](logRefs/E018-irq-task-delivery.md) |
| Q10 zero workgroups | no special case: one kick per command. **Confirmed**: `empty_workgroup_x/y/z` pass once no stuck job is left. | `docsClean/kick-compute-and-3d.md`; [E019](logRefs/E019-hwr-and-compute-basic-67.md) |
| Q12 TQ address 0 | PBE two banks, `0x0FD8`, conditional `0x0F20`. **Implemented**, but the fault remains: see Q13. | `docsClean/kick-compute-and-3d.md`; [E020](logRefs/E020-tq3d-pbe-two-banks.md) |
| Q13 TQ destination | the destination is in the end-of-tile program data; complete table with conditions. **Implemented**: no more address-0 fault, TQs complete. See Q14. | `docsClean/kick-compute-and-3d.md`; [E022](logRefs/E022-tq3d-completes-first-tile-lost.md) |
| Q11 HWR reset | SLC idle → partial renders off → SOFT_RESET pulse `0x13FFFC1D/0xFFEFFE00` → units re-init and BIF rebuild → discard → resume. **Implemented and working** (~0.28 ms). | `docsClean/hardware-recovery.md`; [E019](logRefs/E019-hwr-and-compute-basic-67.md) |
| Q7 GPU units init, exec bases | PDS `0x0610` (found by experiment, [E009](logRefs/E009-pds-exec-base-0x0610.md)), USC `0x4008/0x4010/0x4028`; the full section-A sequence. `0x0FC8` (E012) is an ISP register. **Implemented: compute works.** | `docsClean/boot-gpu-init.md` A; [E013](logRefs/E013-gpu-units-init-compute-pass.md) |
