<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# Questions to the spec maintainer (dirty team)

Gaps found while implementing from `docsClean/`. Each one is a functional question,
following the question flow in `docsClean/clean-room-process.md`. Until a question is
answered, the implementation uses the stated assumption.

## Open

No open questions at the moment. The next items (3D and geometry kicks) are covered by the
spec and are implementation work.

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
| Q14 first tile lost | launch = `ISP_RENDER = isp_render` then `0x0F00`, with `\|= 0x10` only on resume; per-DM cache flush before retiring. **Implemented: no failures in five groups.** | `docsClean/kick-compute-and-3d.md`; [E023](logRefs/E023-tq-launch-order-and-dm-flush.md) |
| Q11 HWR reset | SLC idle → partial renders off → SOFT_RESET pulse `0x13FFFC1D/0xFFEFFE00` → units re-init and BIF rebuild → discard → resume. **Implemented and working** (~0.28 ms). | `docsClean/hardware-recovery.md`; [E019](logRefs/E019-hwr-and-compute-basic-67.md) |
| Q7 GPU units init, exec bases | PDS `0x0610` (found by experiment, [E009](logRefs/E009-pds-exec-base-0x0610.md)), USC `0x4008/0x4010/0x4028`; the full section-A sequence. `0x0FC8` (E012) is an ISP register. **Implemented: compute works.** | `docsClean/boot-gpu-init.md` A; [E013](logRefs/E013-gpu-units-init-compute-pass.md) |
