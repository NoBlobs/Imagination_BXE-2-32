<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# E027 — host render structures matched by value with the spec's register observation

**Question.** The partial answer to Q16–Q21 lists geometry and 3D registers observed with
a value but no known source, in `kick-geometry-ta.md` phase C and
`kick-compute-and-3d.md` phase B. Can the sources be found without more dirty-team work?

**Method.**
- Build with `-DEXP_DUMP_RENDER`. When a `GEOM`, `FRAG` or `FRAG_PR` command arrives
  (still retired at once), the firmware writes raw words to the debug trace (group DBG,
  mask `0x80007FFE`):
  - the command payload;
  - `rogue_fwif_hwrtdata` (288 B) and `rogue_fwif_hwrtdata_common` (88 B);
  - the three free lists;
  - the render context's static state (`geom_registers_caswitch`, core 0);
  - the geometry context-state buffer.
- `tools/parse-dump.py` turns the trace into tables
  ([data/e027-structure-dump.txt](data/e027-structure-dump.txt), raw trace
  [data/e027-structure-dump-trace.txt](data/e027-structure-dump-trace.txt)).
- Test case: the same one the spec observed,
  `dEQP-VK.draw.renderpass.simple_draw.simple_draw_triangle_list` (256×256).

**Result.**
- The host allocated the same GPU addresses as in the spec's run (region headers
  `0x80_0013_E000`, TPC `0x80_0013_B000`, VHeap `0x80_0013_A000`, local free list
  `0x80_0013_9000`, VDM stream `0x80_0013_F000`), so the values can be compared directly.
- Matches were also checked against the writable-bit masks of E008 and the field layouts
  of public Mesa (`rogue_cr.xml`).

| Register | Spec observed | Host field (this dump) | E008 mask / Mesa layout |
|---|---|---|---|
| `0x0C98` | `0x00FF00FF` | `hwrtdata_common.screen_pixel_max` | `0x7FFF7FF0` = `PPP_SCREEN` (pixymax 30:16, pixxmax 14:0) |
| `0x0C18` | `0xF00F` | `common.te_screen` | `0x001FF1F0` = `TE_SCREEN` (ymax 20:12, xmax 8:0) |
| `0x0C08` / `0x0C10` | `0x00100000` | `common.te_mtile1` / `te_mtile2` | `0x07FFFFF0` = `TE_MTILE1/2` (27 bits) |
| `0x0C20` | `0x10` | `common.mtile_stride` | `0x7FFF0` |
| `0x0C78` / `0x0C80` | `0x88` | `common.multi_sample_ctl` / `flipped_multi_sample_ctl` | 8-bit; `0x88` = sample at (8,8), the 1× position of `PPP_MULTISAMPLECTL` |
| `0x0418` | `0x80_0006_D000` | geometry context state `geom_reg_vdm_call_stack_pointer_init` (`context_state_addr + 8`) | `VDM_CALL_STACK_POINTER` |
| `0x0C28` | `0x00020000` | `geom_regs.te_psg` | `0x003E67F0` = `TE_PSG` layout |
| `0x0408` | `0x80_0013_F000` | `geom_regs.vdm_ctrl_stream_base` | 40-bit |

The fragment command was decoded as well, with the field offsets of the public check
header:
- `usc_pixel_output_ctrl 0x7F8`, `isp_bgobjdepth 1.0f`, `isp_bgobjvals 0x3FF`,
  `isp_ctl 0x80031000`;
- `event_pixel_pds_info/data 0x201/0x29C0`;
- `isp_scissor_base 0x80_0005_3280`, `isp_dbias_base 0x80_0005_3240`;
- PBE RT0 `0x020001FD_68800000` / `0x800000FF_800000FF`;
- `pds_bgnd[0..2]` = `0x00002640_00002600`, `0x00002A00_00000000`,
  `0x01000000_00010002`; `flags = 0x8000` (`DISABLE_PIXELMERGE`).

Notes on the 3D registers:
- The spec saw `0x0F18 = 0x00100010` after a 3D job, but the host's
  `common.isp_mtile_size` is `0x00040004`. The spec notes that the read-back transfer
  following the render overwrites 3D registers: a 256×256 copy has a 16×16-tile
  `isp_mtile_size` of exactly `0x00100010`. So for a real 3D kick, `0x0F18` should
  probably be `common.isp_mtile_size` (inference).
- `0x3E40 = 0x00FF00FF` is `screen_pixel_max` either way.
- Still unmatched: `0x0C30/0x0C34` (2/3), `0x0C48` (8), `0x0C70` (3), `0x0C90` (64-bit
  `0x3089705F`), `0x0CA0` (0x10), `0x0D00` (0x3F).

**Clean-room note.** Values come from our own firmware's view of the host structures,
public layouts, and the observation published in `docsClean`.
