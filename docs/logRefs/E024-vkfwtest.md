<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# E024 — `vkfwtest` workloads on the clean firmware (build `2840e82c…`)

**Question.** Does the board run real Vulkan workloads with the clean firmware, beyond
dEQP?

**Set-up.**
- `~/vkfwtest/vkfwtest <scenario> .` on the board: a headless workload generator with
  regular Vulkan API use and no result checking.
- Mesa pvr, kernel `7.4.0-rc1-img`, driver loaded by hand. Raw output:
  [data/vkfwtest-e024.txt](data/vkfwtest-e024.txt).

| Scenario | Work | Result |
|---|---|---|
| `pre-cdm` | long low-priority compute plus high-priority probes (two devices) | runs to `done`; calibration scales with n (0.040 s → 0.462 s); probe 1.79 ms alone, 141.59 ms average and 1.4 s worst during the long job (no preemption: the job runs to its end) |
| `xfer` | clears, copies, blits, buffer↔image, MSAA resolve, on 2 queues | 10 iterations, ~74 ms each, `done` |
| `sparse` | sparse binding, timestamps, timeline semaphores | Mesa: "sparse binding not supported", exit 0 |
| `pre-3d`, `pre-ta`, `bigpb` | fragment / vertex / millions of triangles | reach `done`, but every frame "takes" 0.002 s. Geometry and 3D kicks are **not implemented**: the firmware retires these commands at once (trace "Unknown Client CCB Command", 111 times), so nothing is drawn |

There were no `Job timeout` lines in the kernel log and no HWR (count 0).

**Conclusion.**
- Compute and transfer workloads run correctly and stably. The probe timings show that
  CDM context store (preemption) is not implemented: a high-priority job waits for the
  long one.
- Rendering does not work yet. Draw commands complete without running, so any Vulkan
  program that draws produces empty images, although it does not hang.
- `vkcube`/`vkgears` also need a display, and the mainline kernel has no display driver
  on this board.

**Clean-room note.** A Vulkan workload against our own firmware. Only the program's
usage text was read, not the capture notes it refers to.
