<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# E053 — CPU zeroing of the TPC and the RTC after an HWR

**Question.** Replace the SLC flush that stood in for the post-HWR cache zeroing (E042,
Q19b item 3) with the CPU zeroing of the spec, now that the firmware can write client
memory through a GPU-MMU walk (`gpu_zero()`, E045).

## Sources

- `kick-geometry-ta.md` phase E: on a `FIRSTKICK` with `hwrtdata.geom_caches_need_zeroing`
  (`+0x114`) non-zero, the firmware maps GPU memory and zeroes, with the MIPS CPU:
  - the TPC at `tail_ptrs_dev_addr` (`+0xF0`), `hwrtdata_common.tpc_size` (`+0x1C`) bytes;
  - the RTC at `rtc_dev_addr` (`+0x108`), `0x300` bytes, when there is one.

  It then sets the flag to 1 and runs `CR[0x0258]`. A normal `LASTKICK` clears the flag
  at TA finished.
- `hardware-recovery.md` step 4: the HWR leaves the flag at 1. The RTC zeroing is what
  emits `Zero RTC for FWCtx` (`SFID_HWR_69`).

## Firmware change

`src/geom.c`, new `geom_zero_caches()`, called from `geom_pm_load()` on a first kick when
the flag is set (the HWR sets it in `pm_hwr_reconstruct()`):
- flushes and invalidates the SLC, so no dirty line lands over the zeroes;
- zeroes the TPC with `gpu_zero()` through the render context's memory context;
- traces `Zero RTC for FWCtx` and zeroes the first `0x300` bytes of the RTC, when there is
  one;
- flushes and invalidates the SLC again, so the GPU does not read a stale line.

When part of a range is not mapped, it traces `DBG_4` `0xE0530001` (TPC) or `0xE0530002`
(RTC). The SLC flushes still run.

The build has no warnings; `check-trace-args` and `fw-preflight` pass. Firmware
`9ddda2a5…`.

## Test

The `hwr-*` tests don't reach this path, because each recovers on new devices with new
HWRTData. On the mainline stack the kernel reports no device lost, so an application
keeps using the same HWRTData after an HWR. The test reproduces that:
- a throwaway copy of `vkfwtest` (scratchpad only, the repository is unchanged) in which
  `multiframe` logs a wrong frame and goes on;
- run with `-f 900` and `-f 1500` in the background, while `hwr-frag` locks up the 3D.

A temporary probe trace (`0xE0530000`, TPC address and size; removed afterwards) showed:

| Run | Result |
|---|---|
| Probe build, one run | 2 zeroings after the HWR (`TPC 0x17F000`, `0x200` bytes), one for each HWRTData of the render target (they share the TPC). Frames 80 and 81, lost in the HWR, are wrong; frames 82–1499 are right |
| Final build, 3 runs | frames 82–84 wrong, the rest right. 3 context resets and 1 page fault at address 0 per run |
| Old behaviour (SLC flush only), 3 runs | **identical**: frames 82–84 wrong, 3 resets and 1 page fault per run |

`multiframe` has no RTC, so `Zero RTC` did not run in these tests. That path uses the same
`gpu_zero()` as the RTC zeroing of `geom_rta_oom()` (E045).

Regression with the final build:

| Run | Result |
|---|---|
| `vkfwtest -k all` | **48 pass / 0 fail / 2 skip** |
| `stress` | **50/50**, no reset |

**Conclusion.** The zeroing runs after the HWR and the renders that follow on the same
HWRTData are correct. It changes nothing in the A/B comparison.

**Pre-existing, not caused by this change:** with a renderer running beside a 3D lockup,
the first HWR is followed by a second one about 36 ms later on that renderer's 3D
(`Guilty lockup`, page fault at address 0). It costs three frames and happens with or
without the zeroing. [E054](E054-freelist-reconstruction-hold.md) rules out the
free-list reconstruction race; the question is Q42.
