<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# E054 — Hold geometry and 3D until the free-list reconstruction update

**Question.** After an HWR the firmware asks the host to rebuild the free lists
(`FWCCB_FREELISTS_RECONSTRUCTION`), but it ignored the host's
`KCCB_FREELISTS_RECONSTRUCTION_UPDATE` and scheduled again at once. The mainline host
(`pvr_free_list_reconstruct()`) rewrites each free list in memory: `current_stack_top`,
`allocated_page_count = 0`, and the HWRTData state `HWR`. A geometry or 3D kick in
between could load a list that is being rewritten, or store a stale one over it.

Hypothesis: this race causes the second HWR seen in E053 (3D lockup, page fault at address
0, about 36 ms after the first HWR).

## Sources

- `hardware-recovery.md` step 4 and `partial-render-spm.md` "Freelist reconstruction
  after HWR". The local summary `parameter-manager-and-spm.rst` says the firmware
  "blocks until the host confirms".
- Public trace strings:
  - `HWR_52` `Requesting reconstruction for freelist …`;
  - `HWR_53` `Reconstruction of freelist ID=%d complete`;
  - `HWR_55` `… failed`;
  - `HWR_63` `Holding Scheduling on DM %u for OSid %u due to pending freelist reconstruction`;
  - `RTD_22` `Freelist reconstruction completed`.
- Public `rogue_fwif_freelists_reconstruction_data` (`freelist_count`, `freelist_ids[18]`)
  and `ROGUE_FWIF_FREELISTS_RECONSTRUCTION_FAILED_FLAG` (`0x80000000`).

## Firmware change

- **`src/pm.c`:**
  - `pm_hwr_reconstruct()` records each requested free-list ID as pending and traces
    `HWR_52`. The duplicate-collecting loops are merged into `fl_collect()`.
  - `pm_reconstruct_update()` removes the listed IDs and traces `HWR_53`, or `HWR_55` when
    the host flags a failure; the mainline host never flags one, and the list is released
    either way. When none is left it traces `RTD_22`.
  - `pm_reconstruct_hold(dm)` is true for DM 2 and DM 3 while any reconstruction is
    pending. It traces `HWR_63` once per DM.
- **`src/kick.c`, `kick_task()`:** a `GEOM`, `FRAG` or `FRAG_PR` command on a held DM
  stays queued (`kick_defer`). Compute and transfers keep running.
- **`src/kccb.c`:** `FREELISTS_RECONSTRUCTION_UPDATE` calls `pm_reconstruct_update()`,
  then the scheduler.
- **`src/include/fwif.h`:** the KCCB union gains the update payload.

The build has no warnings; `check-trace-args` and `fw-preflight` pass. Firmware
`dc69ef27…`.

## Results

The protocol works. In the E053 scenario (a continuing `multiframe` beside `hwr-frag`):

```text
Begin hardware reset (HWR Counter=0)
Requesting reconstruction for freelist 0xc00b9000 (ID=3)   … IDs 1, 6, 4
Finished hardware reset (HWR Counter=0)
Holding Scheduling on DM 3 for OSid 0 due to pending freelist reconstruction
Reconstruction of freelist ID=3 complete                    … IDs 1, 6, 4
Freelist reconstruction completed
```

The hold lasts about 1 k timer ticks; then scheduling resumes.

**The hypothesis is wrong:** the second HWR still happens. The multiframe 3D locks up on DM 3
with a page fault at address 0. Its counts vary with timing:

| Build | Trace | Runs | Result per run |
|---|---|---|---|
| E054 (hold) | off | 2 | 3 wrong frames, 3 resets, ~74 s |
| E054 (hold) | on, read every 0.3 s | 4 | 1–2 wrong frames, 2–3 resets, ~75 s |
| E054 (hold) | on, not read | 5 | 3 wrong frames, **5 resets**, ~104 s |
| E054 with the hold disabled | on, not read | 3 | **identical**: 3 wrong frames, 5 resets, ~104 s |

The 5-reset case is not caused by the hold. The A/B with the hold disabled matches it
exactly, so it is the same pre-existing failure with different timing.

Regression with the final build:

| Run | Result |
|---|---|
| `vkfwtest -k all` | **48 pass / 0 fail / 2 skip** |
| `stress` | **50/50** |
| `hwr-lockup` ×3 | recovers after 30 s, as before (no device lost: kernel/Mesa, E044) |

**Conclusion.**
- The firmware now follows the reconstruction protocol: it no longer runs a geometry or a 3D
  on a free list that the host is rewriting.
- The second HWR has another cause. The likely one is the 3D of a render whose geometry
  the HWR discarded: that HWRTData's region headers and PM state are not valid.
- The spec does not say what the firmware does with such a 3D → Q42.
