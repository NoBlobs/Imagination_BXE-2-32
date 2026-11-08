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
| **Compute (CDM) jobs** | `dEQP-VK.compute.pipeline.basic`: 9 pass ([E013](logRefs/E013-gpu-units-init-compute-pass.md), [E015](logRefs/E015-slc-flush-before-mmu-inval.md)) |
| MMU fault reporting | trace HWR id 80 with MMU and request status; the task is retired |

## Not working yet

| Item | State |
|---|---|
| TQ_3D (transfer via the 3D pipe) | programmed per spec, but the 3D pipe never starts ([E014](logRefs/E014-tq3d-kick-does-not-start.md)). Compiled only with `-DEXP_TQ3D`; by default retired at once (Q8). |
| Completion through the MTS IRQ task | `COMPUTE_FINISHED` does not raise HW2. Completions are found by the 1 ms poll ([E016](logRefs/E016-completion-path-counters.md), Q9). |
| Compute cases that need images | fail, because they need TQ_3D |
| `empty_workgroup_x/y/z` | fail with a zero result, cause unknown (Q10) |
| HWR | no reset or recovery. A stuck job (`image_atomic_op_*`) blocks every later job until reboot (Q11). |
| Geometry / fragment / TDM kicks | not implemented; retired at once |
| Per-job dust power (`POW_RASCALDUST`) | not needed: the mainline host does not set it. Dusts are powered once at boot. |

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
| E016 | BG tasks arrive; `COMPUTE_FINISHED` does not raise the IRQ task |

## Next steps

1. Get answers to Q8 (TQ start) and Q9 (IRQ task routing).
2. Minimal HWR (Q11): soft reset of the GPU units, then GPU units init again, then
   discard the faulting command, so that one bad job no longer blocks the device.
3. Fragment (3D) and geometry kicks: the spec now maps the geometry register set
   (`kick-geometry-ta.md`, Q4 answered) and the 3D start (Q5 answered).
4. Investigate `empty_workgroup_x/y/z`.
