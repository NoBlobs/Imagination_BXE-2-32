<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# E020 — TQ_3D with the corrected table: the address-0 write remains

**Question.** The spec answer to Q12 changes the transfer table:
- PBE state in two banks, `WORD0_MRTn → 0x1510 + 8n` and `WORD1_MRTn → 0x1550 + 8n`,
  taken from **consecutive** pairs `pbe_wordx_mrty[2n]`, `[2n+1]`;
- `0x0FD8` = tiles in flight, `min(ISP_CTL[15:12] + 1, 2)`;
- `0x0F20` (`isp_mtile_base`) written only when `(isp_render & 3) == 2`;
- the `event_pixel_pds_*` line is gone from the TQ table.

The spec says the PBE state holds the destination address and that the fault came
from a zero WORD1. Does the TQ complete now?

**Variants tested** (`dEQP-VK.compute.pipeline.basic.copy_ssbo_to_image_small`):

| Build | `0x0628/0618/0620` (`event_pixel_pds_info/code/data`) | Result |
|---|---|---|
| `757d69d1…` (table literally) | not written | no fault, **no end of render**; the watchdog fires 2 s later and HWR recovers |
| `fcf716f2…` | written | `BIF0 page fault … MMU 0x1041, Req 0x0002b10000000000`: **write to 0**, the same tag as E017 |
| `+ EXP_SLC_FLUSH_PER_KICK` | written, SLC flush and invalidate right before the kick | same fault, so stale SLC lines are ruled out |

Host values for this TQ (debug trace):

```
pbe_wordx_mrty[0..2]           = 0x0200007c96800000  0x0000003f0000003f  0x0
event_pixel_pds_info/code/data = 0x00000202 / 0x000028a0 / 0x00002880
pds_bgnd0_base = 0x00001e4000041000   pds_bgnd3_sizeinfo = 0x0100000000010001
isp_render = 3 (FAST_SCALE)   isp_mtile_base = 0   isp_rgn = 0
```

**Analysis.**
- For RT0 the corrected PBE layout is the same as E017's planar layout (`[0]→0x1510`,
  `[1]→0x1550`). The fault was already present with it, so the PBE bank layout alone does
  not explain the address-0 write.
- The host's PBE words contain **no address**. Read with the public Mesa field layouts
  (`PBE_WORD0_MRT0`): word0 holds swizzle, min-clip, line stride and memory layout;
  word1 = `0x3f/0x3f` is the 63×63 max clip; word2 = 0. A 40-bit destination such as
  `0x80_xxxx_xxxx` appears nowhere in them.
- `event_pixel_pds_info = 0x202` is a pixel-event task with one shared-register block
  and 2 data words. Its PDS program (`code 0x28A0`, `data 0x2880`, offsets in the PDS
  heap) is the end-of-tile program that emits the PBE state, including the address in
  Mesa's design.
  - Without these registers, the pipe never signals the end of render.
  - With them, the PBE writes to address 0.
  So the end-of-tile path runs, but the address it delivers is 0. Either `0x0618` is not
  the code address (the 3D kick writes a constant `0x80` there), or the emit needs
  something else.

**Decision.** By default the firmware keeps writing `event_pixel_pds_*`. The TQ then
fails within 1 ms and HWR discards it, instead of waiting for the watchdog.
`-DTQ_NO_EVENT_PDS` follows the table literally. This is question Q13.

**Clean-room note.** Spec, public Mesa layouts, own firmware.
