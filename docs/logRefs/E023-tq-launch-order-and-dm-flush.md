<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# E023 — the first-tile loss is gone: TQ launch order and per-DM completion flush

**Question.** The spec answer to Q14 corrects the TQ launch and adds a per-job cache flush
on completion:
- **Launch:** `ROGUE_CR_ISP_RENDER = isp_render` immediately before `BCAST[0x0F00] = 1`.
  `ISP_RENDER |= 0x10` belongs only to the context-resume path.
- **Fragment control:** `MULTICORE_FRAGMENT_CTRL_COMMON = 1` on a single-core GPU.
- **Completion flush,** after clearing the event and before retiring the command:
  front-end, USC/PDS, TPU, MCU fence, then the SLC flush for that DM.

  | Step | Fragment / transfer | Compute |
  |---|---|---|
  | 1 | `0x4000 = 1` | `0x4000 = 1` |
  | 2 | `0x0688 = 2`, poll | `0x0688 = 4`, poll |
  | 3 | `0x0668 = 3`, poll bit 0 | `0x0668 = 9`, poll bit 0 |
  | 4 | `0x1608 = 0x38001`, poll bit 0 | `0x1608 = 0x10001`, poll bit 0 |
  | 5 | `0x1480 = 1`, `0x1488 = 1` | `0x1488 = 1` |
  | 6 | `MCU_FENCE` DM_PIXEL | `MCU_FENCE` DM_COMPUTE |
  | 7 | `0x1720 = 0x2B`, poll `0x1728` | `0x1720 = 0x33`, poll `0x1728` |
  | 8 | write barrier | — |
  | 9 | `SLC_CTRL_FLUSH_INVAL = 6` (DM_TA \| DM_PIXEL) | `SLC_CTRL_FLUSH_INVAL = 8` (DM_COMPUTE) |

  After the launch, `MTS_SCHEDULE = 0` requests a BG pass.

**Implementation.** `kick_tq3d()` launch and `dm_cache_flush()` in `src/kick.c` (build
`8d5f6f8c…`; the same code as the default `2840e82c…`, which only adds knobs).

## Results — default build

The cases from E022 first:

```
fill_buffer_first_one                              Pass
fill_buffer_second_part                            Pass   (was: first tile missing)
fill_buffer_vk_whole_size_0_extra_bytes_offset_0   Pass   (was: byte 0 not written)
```

Then the groups, in one process, with no reboot:

| Group | Pass | Fail | Not supported |
|---|---|---|---|
| `dEQP-VK.compute.pipeline.basic.*` | **73** | **0** | 9 |
| `dEQP-VK.compute.pipeline.indirect_dispatch.*` | **24** | 0 | 12 |
| `dEQP-VK.compute.pipeline.builtin_var.*` | **11** | 0 | 0 |
| `dEQP-VK.api.fill_and_update_buffer.*` | **72** | 0 | 38 |
| `dEQP-VK.api.copy_and_blit.core.buffer_to_buffer.*` | **8** | 0 | 0 |

- HWR count 0 and no "HW poll … failed" in the trace: every flush poll completed.
- The image compute cases (`copy_image_to_ssbo_*`, `copy_ssbo_to_image_*`,
  `image_atomic_op_*`) and `empty_workgroup_x/y/z` all pass now.

## Controls — what fixed the lost first tile

| Build | `ISP_RENDER` write | `\|= 0x10` | `FRAG_CTRL` | Completion flush | fill (72) | buffer_to_buffer (8) |
|---|---|---|---|---|---|---|
| E022 default | early, before the configuration | yes | `0x101` | SLC only | 6–18 | 2–3 |
| E022 `EXP_NO_RESUME_BIT` | early | no | `0x101` | SLC only | (first tile lost) | — |
| `EXP_SLC_ONLY_FLUSH` (`66b3e04e`) | **just before `0x0F00`** | no | 1 | SLC only | **72** | 7 |
| `EXP_FRAG_CTRL=0x101` (`66fe2d3f`) | **just before `0x0F00`** | no | `0x101` | full | **72** | **8** |
| default (`8d5f6f8c`) | **just before `0x0F00`** | no | 1 | full | **72** | **8** |

- The decisive change is **where `ISP_RENDER` is written**. Written early, before the
  rest of the transfer configuration, the first tile was lost. Written last, right before
  `0x0F00 = 1`, every tile is written. A likely reading is that the ISP latches its tile
  configuration when `ISP_RENDER` is written, so the first tile used state from before
  the configuration writes. That is an inference.
- `FRAGMENT_CTRL_COMMON` (`1` or `0x101`) makes no difference here.
- Removing `| 0x10` alone (E022) was not enough, because `ISP_RENDER` was still written
  early.
- The per-DM flush matters too: without it one `buffer_to_buffer` case still failed
  (7/8).

Pitfall found on the way: a control build failed with `-Werror` (unused function) and
the old `.fw` was deployed. Compare the hash of every experimental `.fw` with the
default before trusting a control run.

Raw data: `data/deqp-e023-all.txt`, `data/deqp-e023-summary.txt`, `data/deqp-e023-controls.txt`.

**Conclusion.** Compute and transfer through the 3D pipe are complete for these groups.
Order in a TQ launch: configuration, then `ISP_RENDER = isp_render`, then `0x0F00 = 1`.
Every completion runs the DM's cache flush before the command is retired.

**Clean-room note.** Spec, public register names, own firmware.
