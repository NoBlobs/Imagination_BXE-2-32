<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# E056 — `docsClean` `05cfa90`: 3D store/resume (Q40), ZS backing details (Q41), HWR discard (Q42)

**Question.** Implement the three answers of `docsClean` `05cfa90`:
- `scheduler.md`, "3D context store and resume register protocol" (Q40);
- `partial-render-spm.md`, "On-demand ZS / MSAA buffer backing details" (Q41);
- `hardware-recovery.md`, "Discarded geometry and subsequent 3D commands" (Q42).

## Q42 — discard on an HWRTData in the `HWR` state

- **`src/pm.c`, new `hwrt_discard()`:** when `hwrtdata.state == HWR (9)`, the command is
  retired without touching the GPU. `fw_com_ctx_flags &= ~0xFF`, and the trace is `HWR_15`
  (`Discarded cmd on DM%u (reason=1) …`). The `UPDATE` commands that follow then signal
  its fences.
- **`src/frag.c`:** checked at the start of every 3D kick.
- **`src/geom.c`:** checked at the start of every non-first geometry kick. A first kick is
  never discarded: it moves the state from 9 to 2.
- **`src/hwr.c`:** when the HWR retires a running 3D, its context's `NEED_RESUME` bit (bit 0)
  is cleared.

The answer also confirms the E054 hold: the host's reconstruction sets `HWR` on every
HWRTData of a rebuilt free list.

## Q41 — ZS/MSAA buffer backing, revised (replaces the E052 assumptions)

- **Unbacking:** only on `CLEANUP_ZSBUFFER` (`spm_zsbuffer_cleanup()`), no longer when the
  final render finishes. The rules, in order:
  - `executed < submitted`: busy;
  - on demand and not unbacked: post `ZSBUFFER_UNBACKING` (unless already
    `UNBACKING_PENDING`), set `UNBACKING_PENDING`, busy;
  - otherwise the buffer is freed. The spec's D-cache flush does not apply: host memory is
    uncached in this firmware.

  `ZSBUFFER_UNBACKING_UPDATE` sets `UNBACKED`.
- **Requests at OOM** (`prb_request_all()`): both non-NULL buffers are requested. A
  buffer in `UNBACKED` or in `UNBACKING_PENDING` is requested at once. One that is not on
  demand, already `BACKED`, or already `BACKING_PENDING` is left alone.
- **Readiness** (`prb_ready()`): the partial render waits only while a buffer is
  `BACKING_PENDING`. A failed backing (`done == 0`) makes it `UNBACKED` (`SPM_42`), and the
  `FRAG_PR` then runs without it.
- **At the `FRAG_PR` kick:** if the ZS buffer exists and is not `BACKED`
  (`spm_pr_zs_unbacked()`), the forced depth/stencil store bits (`0x80004` / `0x40004`) and
  load bits (`0x8002` / `0x4002`) are left out.
- **`submitted` (`+0x10`) / `executed` (`+0x14`):** the spec names the fields but not who
  counts them. **Assumption:** the firmware counts them. Each partial render adds one to
  `submitted` of its buffers at the kick, and one to `executed` when it ends. A partial
  render that an HWR cuts short also counts as executed, so that cleanup does not wait
  forever.
- The E052 per-HWRTData table of backed buffers (`prb_used[]`) and
  `spm_zsbuffer_release()` are removed.

## Q40 — 3D context store and resume

- **Gate:** `ROGUE_FWIF_INICFG_OS_CTXSWITCH_FRAG_EN` (bit 2) in
  `osdata.fw_os_config_flags`. The mainline host leaves it at 0, so the default build
  does not preempt. `EXP_CSW_FORCE` turns it on for tests. The E051 probe
  (`EXP_CSW_PROBE`) is removed.
- **Request** (`frag_csw_request()`, from the scheduler when a `FRAG` or `FRAG_PR` waits
  for the busy 3D): if the waiting context has a higher priority, the firmware traces
  `CSW_38`, then, if `(ISP_RENDER & 3) != 3`, writes `BCAST[0x1000] = 1` and traces `CSW_11`.
  It is not requested:
  - during a partial render;
  - during a render-target-array layer loop. **Assumption:** the spec does not describe
    how to resume a layer loop.
- **End** (`frag_csw_end()`, at `PIXELBE_END_RENDER` when a store was requested):
  `fw_com_ctx_flags &= ~0xFF`.
  - **Stored** when `CR[0x0130] & 0x8` and `!(CR[0x1038] & 1)`:
    - `isp_store[0] = CR[0x1008]`;
    - poll `CR[0x03F8] & 0xC2`. On timeout, `EVENT_CLEAR = 0x4108` and `DBG_2 0xE0560001`;
    - `+0x00 = CR[0x0360]`, `+0x04 = CR[0x0370]`;
    - `NEED_RESUME`, traces `CSW_9` and `CSW_7`;
    - HWRTData state `3DCONTEXTSTORED (6)`, PM-MMU invalidation, `EVENT_CLEAR = 0x8`
      (`0x4008` with `0x4000` set);
    - DM 3 cache flush. The command stays at the head of its queue (`keep`).
    - Skipped: `PM_3D_MEM_FREE`, the layer loop, the retire, UFO updates.
  - **Otherwise** (the request came too late): `+0x00` and `+0x04` are zeroed and the
    normal completion follows.
- **Yield:** the stored context does not kick its 3D again while the context that
  preempted it is still pending. The hold ends when that context kicks a 3D.
- **Resume** (`frag_csw_resume()`, in the 3D kick after the layer state, before
  `0x0F00`):
  - normal kick: `0x0298 = 0`, `0x01C8 = 0`;
  - with `fw_com_ctx_flags & 0xFF`:
    - trace `CSW_8`;
    - `CR[0x1020] = isp_store[0]`;
    - `ISP_RENDER |= 0x10`;
    - `CR[0x0298] = +0x00`, `CR[0x01C8] = +0x04`;
    - `Kick 3D … CSW resume:1`.

    The flags are cleared after the launch.

## Status

The build has no warnings; `check-trace-args` passes. Firmware `41d0d13d…`.

**Tested in [E057](E057-spec-19b96b7-preemption-tests.md).** The board went offline during
the first deploy (not caused by the firmware). The tests then found one bug: the 3D store
was never detected, because the caller had already cleared the event bit. E057 fixes it.
