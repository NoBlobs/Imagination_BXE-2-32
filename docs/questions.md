<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# Questions to the spec maintainer (dirty team)

Gaps found while implementing from `docsClean/`. Each one is a functional question,
per the question flow in `docsClean/clean-room-process.md`. Until answered, the
implementation uses the stated assumption.

## Q1 — MTS initialisation (blocking for interrupt-driven operation)

The spec says the BG task (HW1) and the IRQ task (HW2) are delivered by the MTS, and
that the firmware acknowledges a task by writing `0x0B08`. It does not say how the
MTS must be configured by the firmware at boot.

- Which MTS registers must the firmware program at (warm) boot, and with which values?
  Candidates from the public `pvr_rogue_cr_defs.h`: `ROGUE_CR_MTS_SCHEDULE_ENABLE`
  (`0x0BC8`), `ROGUE_CR_MTS_BGCTX_THREAD0_DM_ASSOC` (`0x0B30`),
  `ROGUE_CR_MTS_INTCTX_THREAD0_DM_ASSOC` (`0x0B40`),
  `ROGUE_CR_MTS_DM0..5_INTERRUPT_ENABLE` (`0x0B58 + 8·n`),
  `ROGUE_CR_MTS_GARTEN_WRAPPER_CONFIG` (`0x0B50`). The host clears both
  `*_THREAD0_DM_ASSOC` registers in `pvr_fw_stop()`, which suggests the firmware sets them.
- How does a GPU event (`ROGUE_CR_EVENT_STATUS`) become an IRQ task (HW2)? Is
  `ROGUE_CR_EVENT_ENABLE` (`0x0128`) involved, and which `MTS_DMn_INTERRUPT_ENABLE`
  carries which event bits?
- Does the firmware have to read `ROGUE_CR_MTS_INTCTX` / `ROGUE_CR_MTS_BGCTX` to learn
  which DM scheduled the task?

**Assumption now:** both DM_ASSOC registers = `0xFFFF`, `MTS_SCHEDULE_ENABLE = 0xFF`; the
timer tick also polls the KCCB so a missing BG wake-up only adds latency.

## Q2 — Device info block for 36.29.52.182

The `.fw` container must carry the BRN/ERN/feature masks and feature values. The spec
gives the BVNC, the MIPS core and the 36-bit bus, but not the rest. Please confirm or
correct (current values in `tools/mkfw.py`):

- `SLC_SIZE_IN_KILOBYTES` (assumed 0; affects `ROGUE_FWIF_INICFG_DISABLE_DM_OVERLAP`)
- `VIRTUAL_ADDRESS_SPACE_BITS` (assumed 40)
- `NUM_ISP_IPP_PIPES` (assumed 2) or whether `XE_MEMORY_HIERARCHY` applies
- `CDM_CONTROL_STREAM_FORMAT`, `TPU_DM_GLOBAL_REGISTERS`, `VDM_DRAWINDIRECT`,
  `VDM_OBJECT_LEVEL_LLS`, `ZLS_SUBTILE`, `S7_TOP_INFRASTRUCTURE`,
  `XT_TOP_INFRASTRUCTURE`, `AXI_ACELITE`, `SYS_BUS_SECURE_RESET`, `LAYOUT_MARS`
  (all assumed absent)
- BRNs (only 63553 set) and ERNs (35421, 38748 set, from Mesa).

## Q3 — Remap register write order

`ROGUE_CR_MIPS_ADDR_REMAP_RANGE_CONFIG` is 64-bit and the MIPS writes it as two 32-bit
halves. Which half commits the entry? **Assumption now:** write low, high, low.

## Q4 — Geometry kick register-set offsets

`kick-geometry-ta.md` lists `CR[set+0xC88]`, `CR[set+0xC00]`… and calls the slot index
"register set". Which register does `set` denote (base address of the set, stride per
set), and how does it map to the public `ROGUE_CR_*` names that the host-prepared
`rogue_fwif_geom_regs` fields correspond to (`vdm_ctrl_stream_base`, `tpu_border_colour_table`,
`ppp_ctrl`, `te_psg`, `tpu`, `vdm_context_resume_task0_size`, `pds_ctrl`, …)?

## Q5 — 3D start value

`kick-compute-and-3d.md` marks the exact `ROGUE_CR_ISP_RENDER` start value as TO CONFIRM.

## Q6 — Power domain on/off bits for the dusts

`registers.md` lists `0x0038` (power request) and `0x0050` (active dusts) without public
names. Is touching them required for a mainline-compatible firmware, or is the domain
fully controlled by the host?

## Q7 — GPU unit initialisation, PDS/USC execution base (BLOCKING for every kick)

**Observed on the board (O):** the first compute kick (`dEQP-VK.compute.pipeline.basic.empty_shader`)
starts the CDM, which immediately takes an MMU page fault: `EVENT_STATUS = 0x200`,
`BIF_FAULT_BANK0_MMU_STATUS = 0x1041` (cat base 1, PT level), `BIF_FAULT_BANK0_REQ_STATUS`
address `0x26C0` (read). The CDM PDS program address written from
`rogue_fwif_cdm_registers_cswitch.cdmreg_cdm_context_pds0` (`0x00001E80_00001EC0`) is an
**offset into the PDS heap**, so the hardware needs the PDS execution base
(`rogue_fwif_sysinit.pds_exec_base`, the host sets it to `ROGUE_PDSCODEDATA_HEAP_BASE`)
and most likely the USC base (`usc_exec_base`).

- Which register(s) receive `pds_exec_base` and `usc_exec_base`, at which offsets, and in
  which format?
- `overview`/`execution-environment` say the firmware "initialises its subsystems (… GPU …)"
  and the trace has `GPU Units init`. What is the full list of GPU registers the firmware
  programs at (warm) boot before the first kick (exec bases, SLC, `PDS_CTRL`, USC/TPU
  defaults, event enables, `BIF_CAT_BASE0` for the firmware, …)?
- Are the remaining `rogue_fwif_compute_regs` fields (`tpu`, `cdm_item`, `compute_cluster`,
  `tpu_tag_cdm_ctrl`, `cdm_resume_pds1`) written by the compute kick, and to which offsets?

## Answered by experiment

- **Q7 (part), PDS exec base:** `ROGUE_CR_PDS_EXEC_BASE` = **`0x0610`**. It is 64-bit,
  bits 39:20 are writable, and it takes `sysinit.pds_exec_base`. With it the CDM no
  longer faults on its PDS fetch ([E008](logRefs/E008-writable-mask-probe.md),
  [E009](logRefs/E009-pds-exec-base-0x0610.md)). Please confirm, and give the USC code
  base register(s): the candidate is `0x0FC8`, with bits 39:34 writable, and it
  accepts `0xE0_0000_0000`. With both bases set, the empty compute shader neither
  faults nor completes, and `PERF_COMPUTE_CYCLE` keeps counting
  ([E012](logRefs/E012-usc-base-0x0fc8.md)). What else must be programmed before a
  CDM kick can complete?
- **Q1 (part), event enable:** `0x0128` is `ROGUE_CR_EVENT_ENABLE` (writable mask
  `0x0008FFFE`). Setting it routes events to the **host** IRQ line, where the mainline
  host rejects them (`irq 30: nobody cared`). A MIPS firmware must keep it at 0
  ([E011](logRefs/E011-event-enable-0x0128.md)). It is still open how
  `COMPUTE_FINISHED` reaches the firmware. Which MTS registers route GPU events to the
  IRQ task?
- **Q2 (part):** `SYS_BUS_SECURE_RESET` must be present in the device info. Without it the
  host leaves `ROGUE_CR_SYS_BUS_SECURE` at its reset value and the MIPS cannot fetch from
  0xBFC00000 (the core stays in `EXL|ERL`, nothing executes).
