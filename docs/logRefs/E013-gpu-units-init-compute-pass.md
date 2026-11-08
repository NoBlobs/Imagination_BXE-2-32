<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# E013 — full GPU bring-up from the updated spec: the first compute jobs pass

**Question.** With the GPU bring-up sequence from the updated `docsClean/boot-gpu-init.md`,
do compute jobs complete?

**Changes (build `7402993b…`)**
- New `src/gpu_init.c`:
  - power-on: `XPU_BROADCAST`, `SYS_BUS_SECURE`, `BIF_FAULT_READ`, event routing through
    `MTS_DMn_INTERRUPT_ENABLE`, `EVENT_ENABLE = 0`, and MTS context association
    (`INTCTX = 0xFFFF`, `BGCTX = 1`);
  - GPU units init: dust power request, SLC bypass, reset pulse `0x6000`, PDS exec
    base `0x0610`, USC exec base ×3 (`0x4008/0x4010/0x4028`), the block of unit
    defaults, and the init-done handshakes `0x4068`/`0x0CD8`.
- Remap write order is now low, high, read-back high (spec answer to Q3).
- Device info uses the values documented in the spec: SLC 16 KiB, 6 ERNs, `LAYOUT_MARS`,
  `XE_MEMORY_HIERARCHY`, `XE_TPU2`, `AXI_ACELITE`, …
- An MMU fault while a DM runs is now reported in the trace (HWR id 80) and the task is
  retired.

**Observation** (fresh boot, `modprobe powervr exp_hw_support=1`)

```
0614=000000da 400c=000000e0 4014=000000e0 402c=000000e0   exec bases (high words)
0b58=00089200 0b78=00000004 0b30=00000001 0b40=0000ffff   MTS routing / association
0050=000000ff 3828=01000000 382c=00003d1f                 dusts, SLC bypass
trace: "HW Request On(1)/Off(0): 1, Units: 0x00000001"
       "GPU Units init (# mask: 0x1)"                     (again after every warm boot)
       "Initialised Firmware with config flags 0x00000000" (DM overlap now enabled by the host)
```

`dEQP-VK.compute.pipeline.basic.empty_shader`:

```
[16374446] : Kick Compute: FWCtx 0xc0056000 @ 0. (PID:538, …)
[16374747] : Compute finished                 (~300 timer ticks = ~0.25 ms)
[16374868] : UFO Updates for FWCtx 0xc0056000 @ 136
  Pass (Compute succeeded)
```

The whole `dEQP-VK.compute.pipeline.basic.*` group is in
[data/deqp-compute-basic-e013.txt](data/deqp-compute-basic-e013.txt) (the run in E015
has the same results with the SLC fix):

| Result | Cases |
|---|---|
| **Pass** | `empty_shader`, `atomic_barrier_sum_small`*, `branch_past_barrier`, `branching_uniform_array_access`, `concurrent_compute`, `copy_ssbo_multiple_groups`, `copy_ssbo_multiple_invocations`, `copy_ssbo_single_invocation`, `empty_workgroup_all` |
| Fail | `copy_image_to_ssbo_*`, `copy_ssbo_to_image_*`, `image_atomic_op_*` (images need TQ_3D, see E014) |
| Fail | `empty_workgroup_x/y/z` ("Unexpected value found in buffer: 0 while expecting 1"), cause not yet known |
| Hang | `image_atomic_op_local_size_8` in this run and `_local_size_1` in E015: the CDM stays active and, without HWR, every following job times out on the host |

\* `atomic_barrier_sum_small` failed in this first run and passed in E015.

**Conclusion.** The spec's bring-up sequence is correct and sufficient for compute.
The USC execution base is `0x4008/0x4010/0x4028`, not `0x0FC8` (E012 was wrong about
that register). Compute jobs that read and write SSBOs produce correct data on the
BXE-2-32 under the clean firmware.

**Clean-room note.** Values from `docsClean/`. Observation of our own firmware only.
