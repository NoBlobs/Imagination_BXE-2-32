<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# E019 — hardware recovery works; `compute.pipeline.basic`: 67/82 pass

**Question.** Does the reset sequence from the spec answer to Q11
(`hardware-recovery.md`, "Reset sequence") recover the GPU without killing the
firmware? How far does the compute group get with it?

**Implementation (`src/hwr.c`).** Two triggers start a recovery:
- an MMU fault seen by `kick_events()`;
- the watchdog: a DM busy for more than 2 s, checked on every timer tick.

The steps:
1. `hwr_state_flags`: clear `HARDWARE_OK`, set `RESET_IN_PROGRESS` (plus
   `GENERAL_LOCKUP` for a lockup). Trace `HWR_32`/`HWR_28` per busy DM, then `HWR_61`.
2. SLC flush and invalidate; poll until idle.
3. `PM_PARTIAL_RENDER_ENABLE (0x0338) = 0`.
4. `BCAST SOFT_RESET = 0x13FFFC1D / 0xFFEFFE00`, read back, wait 10 µs, then 0/0. Trace
   `MAIN_27`.
5. `gpu_power_on()` (event routing, MTS), `gpu_units_init()`, `bif_rebuild()` (re-write
   `BIF_CAT_BASE1..7` and invalidate).
6. `EVENT_CLEAR` everything. For each DM that was busy: `HWR_14`, retire the command
   (its updates run), `HWR_25`.
7. `HWR_62`, `hwr_state_flags = HARDWARE_OK | DM_RUNNING_OK`, and the scheduler is
   restarted. The count goes to scratch `0x1A68`.

**Observation.** A single recovery, from an MMU fault of a TQ (E017):

```
[2829719] : BIF0 page fault detected (… 0x1041 …)
[2829761] : DM3 ready for HWR
[2829776] : Analysis DM3: Lockup FWCtx: 0xc004a000. Need to skip to next command
[2829795] : Begin hardware reset (HWR Counter=0)
[2829831] : HWR: Reset HW
[2829884] : HW Request On(1)/Off(0): 1, Units: 0x00000001
[2829939] : GPU Units init (# mask: 0x1)
[2829970] : Discarded cmd on DM3 FWCtx=0xc004a000
[2830038] : Recovery DM3: DM fully recovered
[2830057] : Finished hardware reset (HWR Counter=0)
[2830135] : UFO Updates for FWCtx 0xc004a000 @ 232
```

From the fault to "Finished" takes 338 `CR_TIMER` ticks, about 0.28 ms; the spec's
observation is ~0.43 ms. The MIPS keeps running, and the next compute job right after
the reset passes.

**Full group** (`dEQP-VK.compute.pipeline.basic.*`, one process, no reboot;
[data/deqp-compute-basic-e019.txt](data/deqp-compute-basic-e019.txt)):

```
Passed: 67/82   Failed: 6/82   Not supported: 9/82
CDM kicks 75, HW2 with COMPUTE_FINISHED 16, HW2 total 109, HWR count 13
```

- **Fail (6):** `copy_image_to_ssbo_{small,large}`, `copy_ssbo_to_image_{small,large}`,
  `image_atomic_op_local_size_{1,8}`. All need the TQ_3D image path (E017). Each failed
  TQ was recovered by HWR, and there were no host timeouts.
- **`empty_workgroup_x/y/z` now pass.** Their earlier failures (E013) came from the
  device state after a stuck job. This matches the spec answer to Q10: the firmware does
  nothing special for zero dimensions.
- `image_barrier_*` and `indirect_after_base_dispatch` pass. The `replicated_composites_*`,
  `shared_*`, `ssbo_*` and `ubo_to_ssbo_*` cases all pass.
- **Not supported (9):** these need features that Mesa pvr does not expose (float16/64,
  int16, cooperative matrix, robustBufferAccess2, a compute-only queue).

**Conclusion.** The recovery sequence from the spec works on the K1. One faulting job
no longer blocks the device. The compute path is functional apart from images.

**Clean-room note.** Spec values, own firmware.
