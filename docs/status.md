<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# Status (BXE-2-32, 36.29.52.182)

Last update: 2026-11-08. Board: K1 BPI-F3, kernel `7.4.0-rc1-img` (branch
`IMG-BXE-2-32`), mainline `powervr`, Mesa `pvr`.

## Works on the board

| Item | Evidence |
|---|---|
| `.fw` container accepted (header, layout table, device info) | `FW version v1.0 (build 1 OS)` |
| MIPS boot: reset vector, wired TLB, wrapper remap, ERET to C | boot markers `0xB0071..0xC0DE0006` in scratch `0x1A00` |
| Dynamic TLB refill from the host MIPS page table, fault page for invalid PTEs | refill counter `0x1A08` |
| `firmware_started` handshake | `[drm] Initialized powervr 1.0.0` |
| Firmware trace decoded by the host | `debugfs/dri/0/pvr_fw/trace_0` |
| KCCB: MMUCACHE, CLEANUP, POW, HEALTH_CHECK, … return `CMD_EXECUTED` | `KCCB Slot n: Return value 1` in the trace |
| Runtime PM: FORCED_IDLE → OFF → domain off → warm boot | [E005](logRefs/E005-runtime-pm-cycle.md) |
| Vulkan device enumeration | `vulkaninfo`: `PowerVR B-Series BXE-2-32 MC1` |
| Client CCB: KICK, write offset, FENCE/FENCE_PR/UPDATE/RMW/PRIORITY/PADDING | implemented; fences and updates of retired tasks signal the host |
| BIF slot binding for client memory contexts | fault reports name the slot the firmware chose ([E006](logRefs/E006-compute-kick-mmu-fault.md)) |

## Partly working

| Item | State |
|---|---|
| Compute kick (CDM) | The kick reaches the hardware. Without a PDS base it faults at `0x26C0` ([E006](logRefs/E006-compute-kick-mmu-fault.md)). With `PDS_EXEC_BASE = 0x0610` programmed, that fault is gone ([E009](logRefs/E009-pds-exec-base-0x0610.md)), and the next fault is at `0x7B80`, probably the USC code base ([E011](logRefs/E011-event-enable-0x0128.md)). |
| Completion events | `COMPUTE_FINISHED` has not been observed yet. `EVENT_ENABLE` (`0x0128`) must stay 0 because it drives the host IRQ ([E011](logRefs/E011-event-enable-0x0128.md)). The MTS route for events to the firmware is still unknown (Q1). |

## Not implemented

Geometry, fragment and transfer kicks are retired at once, so the host does not hang
but the work is not done. HWR (fault recovery), SPM/freelist grow, context switch,
FWCCB messages, HWPerf and the GPU utilisation statistics are also missing.

## Findings so far (details in logRefs)

| # | Finding |
|---|---|
| E001 | `SYS_BUS_SECURE_RESET` must be in the device info, or the MIPS cannot fetch |
| E002 | C handlers must run with `EXL = 0`, otherwise a TLB miss loses EPC |
| E003 | the TLB/remap flush must run from an unmapped page |
| E004 | reading RGX registers with the GPU domain off hangs the SoC |
| E005 | a warm boot can be a full cold boot; IDLE must be reported accurately |
| E006 | CDM PDS addresses are offsets from a PDS exec base |
| E008 | writable-bit map: `0x0610` (40-bit, 1 MiB) and `0x0FC8` (bits 39:34) are exec-base candidates |
| E009 | **`0x0610` = `ROGUE_CR_PDS_EXEC_BASE`** (fault at `0x26C0` disappears) |
| E009/E011 | reloading `powervr` while a CDM task is stuck can hang the SoC |
| E010 | a truncated firmware file makes the kernel load the vendor firmware |
| E011 | **`0x0128` = `ROGUE_CR_EVENT_ENABLE`** (mask `0x0008FFFE`); it routes events to the host IRQ and must stay 0 |
| E012 | `0x0FC8` accepts `usc_exec_base`; the CDM stays active (`PERF_COMPUTE_CYCLE` keeps counting) |

## Next steps

1. ~~E012~~ done: `0x0FC8` takes the USC base, but the CDM stays active and stuck
   (`PERF_COMPUTE_CYCLE` keeps running, [E012](logRefs/E012-usc-base-0x0fc8.md)). Next, find
   the offsets of the remaining `rogue_fwif_compute_regs` fields (Q7) and the USC/PDS
   unit initialisation.
2. Find how `COMPUTE_FINISHED` reaches the firmware: `MTS_DMn_INTERRUPT_ENABLE`
   (`0x0B58 + 8n`), `MTS_GARTEN_WRAPPER_CONFIG` (`0x0B50`), `MTS_GPU_INT_STATUS`
   (`0x0BB0`, which mirrored the MMU fault in E006).
3. Minimal HWR: on an MMU fault, clear the fault, retire the task with an error and
   reset the CDM. Repeated experiments then no longer need a reboot.
4. Once an empty compute shader completes: make `gpu_units_init` permanent, then
   implement the TDM, 3D and TA kicks.
