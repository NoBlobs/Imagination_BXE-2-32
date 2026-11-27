<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# E042 — spec `fab1fc9`: GPU-init defaults, TA-finished sequence, out-of-memory resume

**Question.** `docsClean` `fab1fc9` answers Q19b, Q25, Q27, Q28 and part of Q29. With those
answers implemented, what works on the board?

**Board.** BPI-F3 16 GB, mainline SD card, kernel `7.4.0-rc1-img` (GRUB entry `img-gpu`,
index 3). Final build `5676a2c5…`.

## What changed

**GPU init** (`src/gpu_init.c`, `boot-gpu-init.md` steps 6–10):
- the unit defaults are now split around the init-done polls (part 1 before, part 2
  after);
- new step 9:
  - `0x17B8` TPU filter mode: mode 8, or 4 when only `filter_flags` bit 3 is set; then
    bit 1 is forced on (read back, `| 2`);
  - the TPU/PBE/ISP/TE defaults (`0x1780`, `0x0C90/94`, `0x1500`, `0x1710/14`,
    `0x15A0..0x15DC`, `0x0C78`, `0x0708`, `0x0FA0`, `0x06B0`, `0x0470`, `0x0240/44`,
    `0x1788/8C`).

**Geometry kick** (`src/geom.c`, `kick-geometry-ta.md` E):
- `0x0C68 = 1` and the poll for 0 only on a first kick;
- RTC programming:
  - `0x0CB0` = `rtc_dev_addr`. This replaces the render-context state base found by the
    canary in E028;
  - without an RTC: `0x0CE0 = 1` on a first kick, `3` on later kicks;
  - with an RTC: `0x0C48` = `tpc_stride`, `0x0CE0 = 0`;
  - `0x0CB8 = 1`;
  - `0x0D20` = `active_render_targets`;
- `geom_caches_need_zeroing`:
  - first kick: the TA cache command `0x0258`; other kicks: `0x0250`;
  - the reference zeroes the TPC and the RTC with the CPU first. We cannot map client
    memory yet, so we flush the SLC instead. The host never sets the flag; only our HWR
    does.

**TA finished** (`geom_done`, now run before the cache flush):
1. VHEAP store `0x0260` (not on a last kick);
2. TPC flush `0x0C50 = 0x40000000`;
3. RTC flush `0x0CB8 = 2` when `0x0CB0 != 0`;
4. poll the TPC flush;
5. clear `geom_caches_need_zeroing` on a last kick.

**Geometry cache flush.** The full DM 2 column of the per-job flush table: TPU `0x10001`,
`0x1488`, MCU fence `DM_TA`, `0x1720 = 0x27`, barrier, SLC `DM_TA`. The three variants are
now one table in `dm_cache_flush`.

**Out-of-memory** (`src/pm.c`, `partial-render-spm.md` "Resuming geometry after OOM"):
- the global free list is always the one extended (`freelists[2]` when `flags` bit 8);
- the TA resumes after ready pages with **`0x0328 = 1`**;
- no ready pages: stop the TA:
  - `0x0320 = 1`, wait for `TA_TERMINATE`, clear it;
  - `0x0CF0 = 1` and poll;
  - after the grow update: HWRTData state 1, `0x0328 = 1`, `0x0CF8 = 1`;
- partial-render restart:
  - context 1 slot dropped, state 1, context 0 selected;
  - `0x0290`, `0x0198`, PM TLB invalidate;
  - `0x01B8` and poll;
  - `0x0328`, `0x0CA8`;
- `EXP_RESUME` removed.

**Two bugs found on the board:**
- **A grow update that only adds ready pages** (new total − ready = our current total) was
  taken as "the host cannot grow". The firmware then started a partial render, which
  never ran. Now those ready pages go straight to the waiting TA.
- **`FRAG_PR` was consumed too early.** Its fence often passes already (`PR Check … is 0
  requires 0`), so the fragment context consumed `FRAG_PR` before the out-of-memory
  event. Now:
  - `kick_task` can leave a command queued (`kick_defer`); `FRAG_PR` waits there while
    its HWRTData's geometry runs;
  - after a partial render it stays queued (`dm_state.keep`) for the next one in the same
    frame;
  - it is consumed without running once the geometry is done.

## Results

| Check | Result | Before (E041) |
|---|---|---|
| Stage A | pass | pass |
| `api.smoke` | 8/8 | 8/8 |
| `simple_draw` | 4/4 | 4/4 |
| `copy_and_blit…image_to_image.simple_tests` | **23/23** | 19/23 (NPOT) |
| `compute.pipeline.basic` | 73/82, 9 not supported | 73/82 |
| `basic_draw` | 203/331, 0 fail | 203/331 |
| `bigpb` frame 0 (524 288 triangles) | **done in 0.22 s**, about 30 out-of-memory events: ready pages, waits for grow, grow updates | lockup (TA never resumed) |
| `bigpb` frame 1 (second HWRTData) | BIF page fault in the TA at once, then lockups | — |
| `pre-3d` | render 1 passes; render 2 (second HWRTData) still stalls the TA | same |
| `pre-3d`, `EXP_SHARED_PB` (reference's `CONTEXT_PB_BASE = 0`, no reload) | render 2 still stalls; the board stays up | — |
| input attachment `d32_sfloat.input.clear.dont_care.draw` | 3D of the second HWRTData faults on the IPF read at `0x2000` | same |

- **Q25 is fixed** by the GPU-init defaults.
- **Q28 is fixed**: the first out-of-memory event, the waits for grow and the grow updates
  all resume the TA, and the frame finishes.
- No new polls time out: the `0xE042xxxx` traces never appear.

**What still fails is the second HWRTData of a pair.** The `basic_draw` trace shows many
renders on one HWRTData of the same device, all good. Every failure is on HWRTData 1:
- `pre-3d`: the TA stalls;
- `bigpb` frame 1: the TA takes a BIF fault;
- the input attachment: the 3D faults on the IPF read.

So it is not "the second render" but **"the second HWRTData"**. The TPC flush,
the RTC programming and the shared-PB mode did not change it. After the first failure,
every TA locks up, including HWRTData 0: our recovery does not bring the PM/TA back to a
working state (follow-up question Q29b).

**Not done.**
- Render-target arrays and firmware 1.1 (Q27): the answer is in, the code is not; the
  version stays 1.0.
- Context store and resume (Q24): optional.
- CPU zeroing of the TPC/RTC after HWR: needs a mapping of client memory.

**Clean-room note.** Own firmware and measurements; spec sections named above.
