<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# E022 — TQ_3D completes, but the first tile of a multi-tile transfer is not written

**Question.** The spec answer to Q13 gives the complete transfer register table with its
conditions. Does the TQ produce correct output now?

**Changes (build `8e0eb680…`).** The kick follows the table exactly:
- `ISP_RENDER = isp_render` first.
- Written only in FAST_2D (`(isp_render & 3) == 2`): `0x0F20/24` (`isp_mtile_base`),
  `0x0F28` (`isp_rgn`), `0x0FC8/0x0FCC = 0/0xE4`.
- `0x0FA8/0x0FAC = 0x88888888` only if `isp_aa & 3`.
- New: `0x0388 = 0`.
- `event_pixel_pds_*` written, and PBE in two banks.
- Launch: `ISP_RENDER |= 0x10`, then `0x0F00 = 1`.

Before, the firmware wrote `0x0F28`, `0x0FC8/0x0FCC` and `0x0FA8/0x0FAC`
unconditionally.

## Result 1 — the address-0 fault is gone, TQ jobs complete

```
[2888204] : Kick 3D TQ: FWCtx 0xc004a000 @ 0, CSW resume:0. …
[2888505] : 3D Transfer finished                    (~0.25 ms, no fault)
```

Writing those registers outside their conditions was what made the PBE write to
address 0 (spec hypothesis 3). Across the runs below, no MMU fault and no lockup came
from a TQ.

## Result 2 — single-tile transfers are correct, multi-tile ones lose the first tile

`dEQP-VK.api.fill_and_update_buffer.dedicated_alloc` (a fill done by a TQ, no
background program, colour from `usc_clear_register*`):

| Case | `frag_screen` | `isp_mtile_size` | `isp_ctl` | PBE word1 (clip) | Result |
|---|---|---|---|---|---|
| `fill_buffer_first_one` | `0x0` (1×1) | `0x00010001` | `0x21000` | `0/0` | **Pass** |
| `fill_buffer_second_part` | `0x1f` (32×1) | `0x00020001` | `0x21000` | `0x1f/0` | Fail |

Images decoded from the dEQP log (`--deqp-log-images=enable`, 64-px buffer viewed as
RGBA8):

```
expected : px  0..31 original, px 32..63 = 00010203 (filled)
result   : px  0..47 original,             px 48..63 = 00010203
```

The filled region is 32 px wide, two 16×16 tiles. The **second tile (px 48–63) is
written correctly. The first (px 32–47) is not written at all.** A whole-buffer fill
fails the same way: "Invalid byte at position 0 (found 0xff but expected 0x1)".

Variants that change nothing:

| Variant | Knob | Result |
|---|---|---|
| tiles in flight forced to 1 | `EXP_TIF=1` | same first-tile loss |
| 2 ms delay between `PIXELBE_END_RENDER` and retirement | `EXP_TQ_DELAY_US=2000` | same, so not an early end event |
| no `ISP_RENDER \|= 0x10` (public name of bit 4: `RESUME`) | `EXP_NO_RESUME_BIT` | same; `0x0F00 = 1` alone starts the pipe |
| SLC flush and invalidate before the kick | `EXP_SLC_FLUSH_PER_KICK` | (E020) not related |

`SLC_CTRL_FLUSH_INVAL` is written with `ALL` (bit 0, flush + invalidate) after every
completion, so the PBE data is not dropped in the SLC.

## Group results (default build `212c6eb7…`)

| Group | Run 1 (`8e0eb680`) | Run 2 (`212c6eb7`, the same code plus a trace) |
|---|---|---|
| `api.fill_and_update_buffer.*` (72 supported) | 18 pass | 6 pass |
| `api.copy_and_blit.core.buffer_to_buffer.*` (8) | 3 pass | 2 pass |
| `compute.pipeline.basic.*` (73 supported) | — | **64 pass** |

- The results vary between identical builds, so the lost tile is **not
  deterministic**.
- `compute.pipeline.basic` dropped from 67 to 64. `empty_workgroup_x/y/z` fail again
  because they initialise their buffers with a fill, which is now a half-written TQ.
  When the TQ was faulting (E019), the buffer kept its previous content and they passed.

Raw data: `data/deqp-transfer-e022-run1.txt`, `-run2.txt`, `data/deqp-compute-basic-e022.txt`.

**Conclusion.** The transfer register programming is now correct enough to run without
faults. The remaining defect is that the first tile of a multi-tile transfer is
missing, and not on every run. This is question Q14.

**Clean-room note.** Spec, public Mesa layouts, own firmware, dEQP output images.
