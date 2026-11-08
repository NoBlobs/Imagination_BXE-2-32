<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# Questions to the spec maintainer (dirty team)

Gaps found while implementing from `docsClean/`. Each one is a functional question,
following the question flow in `docsClean/clean-room-process.md`. Until a question is
answered, the implementation uses the stated assumption.

## Open

### Q8 — TQ_3D: the 3D pipe does not start

Programmed exactly as in `kick-compute-and-3d.md` "Transfer kick", with the
`cmd+N` → `rogue_fwif_transfer_regs` mapping in
[E014](logRefs/E014-tq3d-kick-does-not-start.md):
- `MULTICORE_FRAGMENT_CTRL_COMMON = 0x101`;
- DM 3 bound to the context's BIF slot (`CAT_BASE_INDEX` bits 10:8);
- `ISP_RENDER` written with the host's `isp_render` (`0x3`), then RMW `| 0x10`.

Result: `ISP_RENDER = 0x13`, `PERF_3D_CYCLE` stays 0, no fault, no
`PIXELBE_END_RENDER`.
- What else does the pixel pipe need before the start? A PM/HWRTData step (even for a
  transfer), 3D unit set-up, a register written on the core bank instead of the
  broadcast bank, or a different `FRAGMENT_CTRL_COMMON` value?
- Is `0x06B0` (PDS_BGRND2, with no command field) written, and with what value?
- Which event ends a TQ_3D on this core: `PIXELBE_END_RENDER` (bit 3) or another one?

### Q9 — MTS IRQ task not raised by `COMPUTE_FINISHED`

With the boot values from `boot-gpu-init.md` section B/C (`MTS_DM4_INTERRUPT_ENABLE =
0x04`, `INTCTX_THREAD0_DM_ASSOC = 0xFFFF`, `BGCTX = 1`, `EVENT_ENABLE = 0`), the BG
task arrives for every KCCB batch, but the IRQ task (HW2) fired **once** in a whole
test, with `EVENT_STATUS = 0x400` (POWER_COMPLETE). Compute completions are found by
the firmware's 1 ms poll ([E016](logRefs/E016-completion-path-counters.md)).
- Is `MTS_GARTEN_WRAPPER_CONFIG` (`0x0B50`) or `MTS_SCHEDULE_ENABLE` needed for HW2?
- Is the IRQ task ended with `MTS_TASK_END (0x0B08) = 2` and a read-back, or does it need
  something else, such as clearing `MTS_INTCTX` or a counted-task acknowledgement?
- How does the firmware tell which DM raised the IRQ task?

### Q10 — `empty_workgroup_x/y/z`

`dEQP-VK.compute.pipeline.basic.empty_workgroup_{x,y,z}` fail with "Unexpected value
found in buffer: 0 while expecting 1", while `empty_workgroup_all` passes. Does the
firmware have to treat a CDM command with a zero dimension specially (for example skip
the kick and only retire), or does the control stream carry several dispatches that
need several CDM runs?

### Q11 — HWR reset sequence

For a minimal recovery after a lockup or a MMU fault, which `ROGUE_CR_SOFT_RESET` (and
`SOFT_RESET2`) bits may the MIPS firmware assert without resetting itself (the core is
in the MARS domain, since `LAYOUT_MARS = 1`)? Is the order "soft reset → SLC
flush/invalidate → GPU units init (section A) → re-enable MTS routing" complete?

## Answered

| Q | Answer | Where |
|---|---|---|
| Q1 MTS init / event routing | `MTS_DMn_INTERRUPT_ENABLE` (DM0 `0x89200`, DM2 `0xA0`, DM3 `0x08`, DM4 `0x04`), `EVENT_ENABLE = 0`, `INTCTX = 0xFFFF`, `BGCTX = 1`. **Implemented**, but see Q9. | `docsClean/boot-gpu-init.md` B/C; [E011](logRefs/E011-event-enable-0x0128.md) |
| Q2 device info | values of the open image (SLC 16 KiB, 6 ERNs, `LAYOUT_MARS`, `XE_MEMORY_HIERARCHY`, …). **Implemented** in `tools/mkfw.py`. | `docsClean/memory-model.md`; [E001](logRefs/E001-sys-bus-secure-reset.md) |
| Q3 remap order | low word, high word, read back the high word. **Implemented.** | `docsClean/memory-model.md` |
| Q4 geometry register set | `rogue_fwif_geom_regs` field → `CR[set+0xC..]` mapping | `docsClean/kick-geometry-ta.md` (not implemented yet) |
| Q5 3D start | RMW `ISP_RENDER \| 0x10`, readiness `(ISP_RENDER & 3) == 3`. Tried for TQ, see Q8. | `docsClean/kick-compute-and-3d.md` |
| Q6 dust power | firmware-controlled: `0x0050` mask, `0x0038` request plus `POWER_COMPLETE` poll. **Implemented** at boot; per job only with `POW_RASCALDUST`, which mainline does not set. | `docsClean/boot-gpu-init.md` D |
| Q7 GPU units init, exec bases | PDS `0x0610` (found by experiment, [E009](logRefs/E009-pds-exec-base-0x0610.md)), USC `0x4008/0x4010/0x4028`; the full section-A sequence. `0x0FC8` (E012) is an ISP register. **Implemented: compute works.** | `docsClean/boot-gpu-init.md` A; [E013](logRefs/E013-gpu-units-init-compute-pass.md) |
