<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# E043 — spec `d244503`: PM-MMU invalidation, first-kick PM reset, full HWR teardown

**Question.** `docsClean` `d244503` answers Q29b. Does the second HWRTData of a
render-target dataset now work, and does the device recover after a lockup?

**Board.** BPI-F3 16 GB, kernel `7.4.0-rc1-img`. Builds `8a80c9de…` (Q29b), then
`06b7d8ff…`/`cb8dcd2b…` (partial render), final `3c929a9c…`.

## What changed

**PM-MMU / PM-TLB invalidation** (`pm_mmu_inval`, `kick-geometry-ta.md` B step 3):
1. SLC flush/invalidate `0x10`, then poll;
2. `BIF_CTRL_INVAL = 4`, poll bit 2;
3. `0x1348 = 1`, poll bit 0.

It runs:
- at every geometry PM load;
- at every fragment PM load;
- at every `3D finished`.

**Geometry PM load** (`geom_pm_load`), now in the reference's order:
1. context select;
2. free lists and `CONTEXT_PB_BASE`;
3. PM-MMU invalidation;
4. MLIST base and catalogues;
5. VHeap;
6. on a first kick:
   - `geom_caches_need_zeroing = 1` (it already was cleared at TA finished of a last
     kick);
   - `0x0258 = 1`, poll;
   - context-1 select bits cleared (`0x03D0 &= ~0x10000`, `0x03D4 &= ~0x100`);
   - the **PM reset pulses `0x0290 = 1`, `0x0198 = 1`**.

Changes from before:
- **No stack-pointer load pulses on a first kick.** `0x0190`/`0x0288` now run only for a
  3D load, or for a mid-TA reload of an evicted HWRTData. The stack-pointer load values
  are per context: `0x0268`/`0x02B8` for context 0, `0x0278`/`0x02C8` for context 1. The
  pulse order follows the spec (MLIST, then ALIST).
- Later kicks write `0x0250 = 1` and poll.

**Geometry registers** (`kick-geometry-ta.md` C table):
- `0x0C00` = `te_aa`;
- `0x0C80` = `multi_sample_ctl`, or the flipped value when `te_aa != 0` and command flag
  bit 2 is set;
- `0x0C78` keeps its boot default;
- `0x4098 = 0`. The spec reads `pds_ctrl` at `cmd + 0x6C`, which is padding in the public
  command layout, so we write the observed value.

**Hardware recovery** (`hardware-recovery.md` "Detailed bus quiesce"):
1. **Before the reset:**
   - stall BIF/MMU (`0x1340 |= 1`), wait for `0x1350`/`0x1338` to drain;
   - MMU invalidate `0x1C`, barrier;
   - wait for the SLC idle bits (`0x03C8`, `0x3898`).
2. **Two-stage `SOFT_RESET`:** the old values, then `| 0x10610000` / `| 0x00080004`, then
   release.
3. **After the units re-init:**
   - unstall;
   - `0x1608 = 0x78001`, poll;
   - SLC flush;
   - BIF rebuild;
   - `0x02A0 &= ~3`.
4. **Free lists:** reconstruction is requested for every free list held by a PM slot, not
   only those of the busy DMs.
5. **Flags:** the geometry HWRTData's `flags` bit 10 is cleared.

**Partial render reached** (`06b7d8ff…`). In the public kernel the PR job's `FENCE_PR` also
waits for the paired geometry's done fence (`pvr_queue.c`), so it stays blocked while the
geometry runs. That is the reference's `UFO PR-Check … requires >= 2`. Now:
- while a partial render is armed (`wait for HW`), the scheduler accepts a `FENCE_PR` UFO
  at the geometry's timeline address (the SPM failing-UFO check of the spec);
- it then kicks the `FRAG_PR` behind it **without moving the queue**
  (`run_partial_render`, non-mutating);
- after the partial render, the fence is evaluated again;
- the firmware no longer signals the geometry's fence itself;
- `frag_pm_load_pr` also runs the PM-MMU invalidation.

## Results

**Full oracle, build `8a80c9de…`:**

| Check | Result | Before (E042) |
|---|---|---|
| A cold boot | pass | pass |
| B `pre-3d` | **pass** (16 renders on both HWRTData of two datasets, no fault, no HWR) | TA stall on HWRTData 1 |
| B `pre-ta` | **pass** | not run (same stall) |
| B `pre-cdm`, `xfer` | pass | pass |
| B `bigpb` | frames 0–2 (up to 2 M triangles) pass; frame 3 (4 M) needs a partial render → lockup | frame 1 faulted |
| C `api.smoke`, `simple_draw` | 8/8, 4/4, 0 failed; flagged FAIL only because `bigpb`'s fault was still in the trace buffer (oracle bug, fixed below) | pass |
| C `compute.pipeline.basic` | 73/82, 9 not supported | same |
| C `copy_and_blit…simple_tests` | 23/23 | 23/23 |
| C `synchronization.basic` | 24/29, 5 not supported | — |
| C `api.buffer` | 1030/3076, 2046 not supported | — |
| D runtime suspend | GPU stayed `active` after the `bigpb` lockups | pass (E041) |

**Q29b is solved.** The PM-MMU invalidation and the first-kick PM reset pulses make the
second HWRTData work.

**Partial render:**

| Build | Result |
|---|---|
| `06b7d8ff…` (restart with `0x1348 = 1` only) | **first partial render runs and the TA restarts**; the next OOM runs a second partial render; at its restart the `0x01B8` poll times out (`0xE04201B8`), then a BIF0 page fault (request `0x0011f08001008ff0`); the board stays up |
| `cb8dcd2b…` (restart with the full PM-MMU invalidation) | **the board hung** (no network, no serial) during `bigpb`; power cycle needed |

The default is again the `06b7d8ff` restart; `EXP_PR_FULL_INVAL` selects the other. Asked as
Q30.

**`tools/oracle.sh`.** The firmware trace buffer cannot be cleared, so an earlier failure
made every later check fail. Each check now looks only at the trace lines added since it
started (`new_trace`).

## After the power cycle (builds `3c929a9c…`, `1b5eae38…`)

**Full oracle `A C D B`, build `3c929a9c…`** (with the `new_trace` fix):

| Check | Result |
|---|---|
| A | pass |
| C: `smoke` 8/8, `simple_draw` 4/4, compute 73, `copy_and_blit` 23/23, sync 24, buffer 1030 | **all pass** |
| D | FAIL: GPU stays `active` (see below) |
| B: `pre-3d`, `pre-ta`, `pre-cdm`, `xfer` | **pass** |
| B: `bigpb` | frames 0–2 pass; frame 3 needs a second partial render (Q30); the frames after a lockup fail too |

**Input attachments.** All 72 cases of
`renderpass1.suballocation.formats.d32_sfloat.input.*` pass with no fault. That is the case
that faulted on the IPF read at `0x2000` since E039 (Q16m/Q16n/Q19b).

**Stage D after stage C: a separate, older bug.** Each suite was run alone after a driver
reload, then the runtime status checked:

| Suite | Then |
|---|---|
| `smoke`, `simple_draw`, `copy_and_blit` | `suspended` |
| `compute.pipeline.basic`, `synchronization.basic` | **`active`, for good** (another Vulkan run does not change it) |

- **It is not today's work.** The committed E041 firmware (`aad92ad`) gives the same
  result.
- **D passes on its own** right after stage A.
- **How the host decides.** The public kernel takes a power reference only around each
  KCCB send. It suspends when the firmware's `pow_state` reads IDLE and the KCCB is
  empty. A device that stays active means our firmware keeps reporting ON.
- **The trace of `synchronization.basic`** holds only MMU-cache and cleanup KCCB commands
  (all `Return value 1`). It has no kick and no `GPU idle` line.
- **A debug trace** of what keeps `any_busy()` true (`0xE0440xxx`) is in the build; it has
  not shown up yet.

To investigate on our side.

**Other fix.** `kccb.c`: `KCCB_LOGTYPE_UPDATE`, `KCCB_HEALTH_CHECK` and `KCCB_BP` fell
through into `spm_grow_update` (since E041). Each now ends with its own `break`.

**Clean-room note.** Own firmware and measurements; spec sections named above.
