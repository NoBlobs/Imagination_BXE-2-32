<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# E038 — Q16l applied: ZLS, multisample words, `0x0F28`, `0x0C68`, and the lost `PM_3D_MEM_FREE`

**Question.** The Q16l answer (spec: `kick-compute-and-3d.md` "Observed during the
render" and `kick-geometry-ta.md` phase E, observation only) gives:
- **ZLS registers:** `0x0F48` = `isp_zlsctl`; `0x0F50`/`0x0F58` = `isp_zload_store_base`;
  `0x0F60`/`0x0F68` = `isp_stencil_load_store_base`. They hold the field values only while
  the render runs, and 0 when the buffer is absent.
- **`0x0F28`** = 4 × `mtile_stride`.
- **`0x0C68`:** 1 for ≤ 20 µs at **every** geometry kick, back to 0 before the trigger.
- **`PM_3D_MEM_FREE`** follows every render with `0x0388` = 1, without `EVENT_ENABLE`.
- In `point_list.45`, `0x0FA0`/`0x0FA8` read `0x88` during the render.
- The two failing `basic_draw` cases pass with the original.

**Changes** (`src/frag.c`, `src/geom.c`; `make EXP=-DEXP_TA`):
1. **ZLS:**
   - `0x0F48` ← `isp_zlsctl`;
   - `0x0F50` and `0x0F58` ← `isp_zload_store_base`;
   - `0x0F60` and `0x0F68` ← `isp_stencil_load_store_base`.
   All are written unconditionally, so a buffer that is absent gives 0. `fb_cdc_zls` is not
   used.
2. **`0x0F28`** ← 4 × `hwrtdata_common.mtile_stride`.
3. **Multisample words:** `0x0FA0` ← `multi_sample_ctl` and `0x0FA8` ← `flipped_multi_sample_ctl`
   (`hwrtdata_common`, `0x88` for one sample: the value seen during `point_list.45`). The
   old `0x88888888` write, done only when `isp_aa & 3`, is gone.
4. **`0x0C68`:** write 1, then wait for bit 0 = **0** (acceptance done), not for 1. On
   later kicks the hardware accepts before the first read, which is why the "poll for 1"
   failed there (E037).
5. **`PM_3D_MEM_FREE`.** `0x0388` was already 1 during our renders (trace
   `0xE0370D01 … 0x0388 = 1`), yet the bit was never seen. Cause: **our IRQ task** reads
   `EVENT_STATUS` and clears every bit it saw (`main.c`). In a small render bit 8 comes
   ~7 µs after `PIXELBE_END_RENDER`, so the IRQ task usually saw both and cleared bit 8
   before `frag_done` looked for it. `frag_done` now first checks the bits the IRQ task saw
   (`CR_DBG_IRQ_LASTEV`), then waits up to 3 ms. Result: **bit 8 seen after 6 of 6
   renders**.

**Results** (build `b12001c2…`; raw summary in `data/deqp-e038-summary.txt`):

| Group | Pass | Fail | Not supported | Before (E037) |
|---|---|---|---|---|
| `draw.renderpass.simple_draw.*` | 4 | 0 | 0 | 4 / 0 |
| `api.smoke.*` | 8 | 0 | 0 | 8 / 0 |
| `draw.renderpass.basic_draw.*` | **203** | **0** | 128 | 201 / 2 |
| `renderpasses.renderpass1.suballocation.formats.d32_sfloat.*` | 45 | 36 | 0 | — |

- `point_list.45` and `flat_b_sat_error` now pass. The multisample words are the likely
  fix, since they were the visible difference in `point_list.45`.
- **Depth:** every `d32_sfloat` case without input attachments passes: `clear`, `load`,
  `dont_care` × `store`/`dont_care` × `clear`/`draw`/`clear_draw`.
- **All 36 failures are the `input.*` cases.** There the second subpass reads the depth
  attachment as an input attachment.

**The `input.*` failure** (`d32_sfloat.input.load.store.draw`, traced):

```
Kick TA (RTD …80000) … TA finished
Kick 3D (RTD …80000), flags 0x8080 (depth), isp_ctl 0x80031000 … PM_3D_MEM_FREE, done
Kick TA (RTD …81000) … TA finished
Kick 3D (RTD …81000), flags 0x8000 (colour only), isp_ctl 0x80011000
BIF0 page fault: MMU status 0xD001, request 0x00102500_00002000  (read at GPU VA 0x2000)
HWR
```

- The first render, with depth, completes, including the PM free.
- The second render, colour only, reads the first render's depth as an input attachment.
  It faults reading **GPU address `0x2000`**. That is the same request signature as the
  fragment fault after an HWR in E036.
- The host also queues a `FRAG_PR` command (`0x2ABC80CC`) before each fragment. We still
  retire it without running it ("Unknown Client CCB Command").
- After a run of these faults the GPU state degrades. A long run then shows unrelated
  failures, which is why the first `d24s8` run looked worse than it is. Each test should
  start from a fresh module load when measuring.

**Open (Q16m).**
- The read at `0x2000` in a render that reads an input attachment.
- `FRAG_PR` and partial renders.
- Multi-kick geometry, OOM and grow.
- `0x0C68` sometimes stays 1 for the whole 10 ms wait (3 times in a long run), with no
  visible harm.

The default build (no `EXP`) still retires geometry and fragment commands at once.

**Clean-room note.** Own firmware and measurements; register roles from the spec's
published observations.
