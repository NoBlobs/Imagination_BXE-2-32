<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# Status (BXE-2-32, 36.29.52.182)

Last update: 2026-11-08, after the `docsClean` update that added `boot-gpu-init.md`.
Board: K1 BPI-F3, kernel `7.4.0-rc1-img` (branch `IMG-BXE-2-32`), mainline `powervr`,
Mesa `pvr`.

## Works on the board

| Item | Evidence |
|---|---|
| `.fw` container with the spec's device info (SLC 16 KiB, 6 ERNs, `LAYOUT_MARS`, …) | `FW version v1.0 (build 1 OS)` |
| MIPS boot, wired TLB, wrapper remap (low → high → read-back) | boot markers in scratch `0x1A00` |
| Dynamic TLB refill from the host page table | refill counter `0x1A08` |
| `firmware_started` handshake | `[drm] Initialized powervr 1.0.0` |
| **GPU bring-up** (`gpu_init.c`): dust power, SLC bypass, exec bases, unit defaults, init handshakes, MTS association | trace `HW Request On… Units: 0x1`, `GPU Units init (# mask: 0x1)` at every (warm) boot ([E013](logRefs/E013-gpu-units-init-compute-pass.md)) |
| KCCB commands, return slots, BG task per KCCB batch | [E016](logRefs/E016-completion-path-counters.md) |
| Runtime PM, warm boot | [E005](logRefs/E005-runtime-pm-cycle.md) |
| Vulkan enumeration | `PowerVR B-Series BXE-2-32 MC1` |
| Client CCB: fences, updates, cleanup, BIF slots | |
| **Compute (CDM) jobs** | `dEQP-VK.compute.pipeline.basic`: **67/82 pass**, 6 fail (images), 9 not supported ([E019](logRefs/E019-hwr-and-compute-basic-67.md)) |
| **Event delivery through the MTS IRQ task** | HW2 on every completion ([E018](logRefs/E018-irq-task-delivery.md)); the polls are a backstop |
| **HWR** | MMU fault or 2 s watchdog → SOFT_RESET pulse → units re-init → discard; ~0.28 ms; 13 recoveries in one dEQP run without a hang ([E019](logRefs/E019-hwr-and-compute-basic-67.md)) |
| TQ_3D launch | the 3D pipe starts (`ISP_RENDER \|= 0x10`, `0x0F00 = 1`) ([E017](logRefs/E017-tq3d-launch-fault-at-zero.md)) |

## Not working yet

| Item | State |
|---|---|
| TQ_3D output | the pipe runs, then writes to address 0 and HWR discards the job ([E017](logRefs/E017-tq3d-launch-fault-at-zero.md), Q12). Compute cases with images fail (6). |
| Geometry / fragment / TDM kicks | not implemented; retired at once |
| HWR info buffer, context-reset notification (FWCCB) | not implemented (the host is not told about a discarded job) |
| Per-job dust power (`POW_RASCALDUST`) | not needed: the mainline host does not set it. Dusts are powered at boot and after HWR. |

## Findings (logRefs)

| # | Finding |
|---|---|
| E001 | `SYS_BUS_SECURE_RESET` must be in the device info |
| E002 | C handlers must run with `EXL = 0` |
| E003 | TLB/remap flush must run from an unmapped page |
| E004 | reading RGX registers with the domain off hangs the SoC |
| E005 | warm boot = full boot; IDLE must be accurate |
| E006 | CDM PDS addresses are offsets from the PDS exec base |
| E008 | writable-bit map of `0x0200–0x3000` |
| E009 | `0x0610` = PDS exec base (confirmed by the spec) |
| E009/E011 | reloading `powervr` with a stuck job can hang the SoC |
| E010 | a truncated firmware file makes the kernel load the vendor firmware |
| E011 | `0x0128` = `EVENT_ENABLE`, must stay 0 (confirmed by the spec) |
| E012 | `0x0FC8` holds bits 39:34 but is **not** the USC base (an ISP register, per the spec) |
| E013 | the spec's GPU bring-up makes compute work |
| E014 | `0x06A0..0x06B8` = `PDS_BGRND0..3` (Mesa layouts match the probe masks); TQ start still missing |
| E015 | flush and invalidate the SLC before MMU invalidation (fault in a new context) |
| E016 | (withdrawn by E018) |
| E017 | TQ launch = `ISP_RENDER \|= 0x10` then `0x0F00 = 1`: the 3D pipe runs; fault writing to address 0 |
| E018 | the MTS IRQ task fires on every completion; the polls only race it |
| E019 | HWR from the spec works (~0.28 ms); compute basic 67/82 |

## Next steps

1. Q12: find the source of the TQ's address-0 write, so that images work.
2. Fragment (3D) kick: the spec maps the registers and the two-write launch.
3. Geometry (TA) kick, with PM/free-list handling (`partial-render-spm.md`).
4. HWR info buffer and FWCCB context-reset notification, so that Vulkan sees device loss.
5. Broader dEQP runs (`dEQP-VK.compute.*`, `api.*`, then `draw.*` once 3D/TA exist).
