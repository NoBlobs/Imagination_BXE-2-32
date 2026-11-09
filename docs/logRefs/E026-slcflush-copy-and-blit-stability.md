<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# E026 — `SLCFLUSHINVAL`, `api.copy_and_blit`, and an interrupted stability run

Build `0b51fd6f…` (default). This is step 1 of the plan in [status.md](../status.md):
the work that does not need the dirty team.

## 1. KCCB `SLCFLUSHINVAL`

- The command used to be acknowledged without doing anything. It now flushes and
  invalidates the whole SLC (`SLC_CTRL_FLUSH_INVAL.ALL`, poll `SLC_STATUS0`).
  - The control register works on the whole cache, which is a superset of any range or
    DM context named in `rogue_fwif_slcflushinvaldata`.
  - The layout is checked against the public check header: `address` at +16, `size` at
    +24.
  - Trace `DBG_5`: context, inval, dm_context, size.
- **Not exercised on the board:** the mainline host never sends this command (no
  `.c` file of `drm/imagination` builds it).

## 2. `dEQP-VK.api.copy_and_blit.core` (normal queue)

`tools/cab-run.sh`, one process per subgroup. Summary in
[data/copy-and-blit-e026-summary.txt](data/copy-and-blit-e026-summary.txt).

| Subgroup | Pass | Fail | NS | Reading |
|---|---|---|---|---|
| `buffer_to_image` (+ `_general_layout`) | 88 | 0 | 27 | OK |
| `buffer_to_depthstencil` / `depthstencil_to_buffer` | 32 / 32 | 0 | 12 / 12 | OK |
| `image_to_image.3d_images` / `.array` / `.cube` | 6 / 9 / 6 | 0 | 0 | OK |
| `depth_stencil_msaa_copy` | 48 | 0 | 384 | OK |
| `image_to_buffer` (+ `_general_layout`) | 126 / 122 | 10 / 10 | 618 | a few unaligned / `buffer_offset_relaxed_r8` cases |
| `image_to_image.simple_tests` (+ `_general_layout`) | 19 | 4 | 0 | `partial_image_npot_*` |
| `blit_image.simple_tests` | 74 | 42 | 295 | `mirror_subregions.*` and similar |
| `buffer_to_buffer_with_offset` | 0 | **64** | 0 | "expected 137 but found 136" (off by one, deterministic) |
| `resolve_image` | 0 | 38 | 202 | "Intermediate verification failed": the image is first produced by a **draw** (no 3D kick yet) |
| `use_after_copy` | 0 | 182 | 2110 | "Unexpected output in color buffer": draws after the copy (no 3D/TA kick yet) |
| `*_indirect`, `memory_to_*_indirect` | 0 | 0 | all | not supported by Mesa pvr |

- No HWR, no reset notification and no `Job timeout` during the whole batch. Every TQ
  completed.
- `use_after_copy` and `resolve_image` fail because they render, which is the missing
  geometry and 3D work. They are not a transfer problem.

**Cache experiment on the remaining failures** (`-DEXP_DM_FLUSH_PER_KICK`: the full DM 3
flush also *before* every TQ kick):

```
buffer_to_buffer_with_offset   0/64   (unchanged)
image_to_image.simple_tests    19/23  (unchanged, same 4 failures)
image_to_buffer.2d_images      120 pass, 10 fail (unchanged)
```

Stale data between jobs is ruled out. These failures are deterministic and limited to
byte offsets, NPOT partial regions and mirrored sub-regions. The most likely cause is
how Mesa pvr encodes those transfers, but without a reference this is not verified.
`buffer_to_buffer_with_offset` (all 64) is the most suspicious and the best candidate for
a closer look (trace of the TQ register values versus the expected copy).

## 3. Stability run — interrupted by an external reboot

`tools/stab-run.sh` was started on the board: 5 repetitions of `compute.pipeline.basic`,
then 100 runtime-PM cycles, each waiting for suspend and then waking with one compute
job.

About 2 minutes in, the board rebooted into the vendor system on the eMMC
(`6.6.63-pvrmod`). The journal of the SD system, read from a read-only mount, shows why:

```
[18697.9] sshd: Accepted publickey for thalles from 192.168.0.3      (a session not run by this experiment)
[18698.1] sudo: COMMAND=/usr/bin/grep -nE ^\s*(menuentry|submenu) /boot/grub/grub.cfg
[18704.4] sudo: COMMAND=/usr/bin/cat /boot/grub/grubenv
[18710.2] sudo: COMMAND=/usr/sbin/reboot
[18713.3] systemd: Reached target reboot.target - System Reboot.  (orderly shutdown)
```

- It was an orderly reboot requested from another session, not a crash.
- Up to the reboot the kernel log has no `powervr` error, timeout or reset notification.
- The `mmc1` SDHCI register dump at 18400 s, during the copy_and_blit batch, is an
  unrelated SD-controller message.
- The stability results lived in `/tmp` and were lost. The run has to be repeated once
  the board is back on the SD system. Nothing was done with the GPU on the vendor
  system.

**Clean-room note.** Own firmware. The vendor system was only used to read the SD
card's journal; the GPU was not touched there.
