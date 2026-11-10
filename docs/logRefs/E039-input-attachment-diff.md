<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# E039 — input-attachment render: register diff, `0x0F18`, and non-first geometry kicks

**Question.** The Q16m answer (spec: `kick-compute-and-3d.md` "Input-attachment renders",
`client-ccb.md`, `kick-geometry-ta.md`, observation) gives:
- the register values during the second render of
  `formats.d32_sfloat.input.clear.dont_care.draw`, which passes on the original;
- the decode of our fault: request `0x0010_2500_0000_2000` = an **IPF control-stream read**
  at `0x2000` (public vendor-driver tables).

Which register differs in our second render?

**1. Diff of the second render** (firmware trace just before the launch, build with
`0xE0390C0x` traces):

| Register | Ours, render 1 / render 2 | Original, render 2 |
|---|---|---|
| `0x0F20` region headers | `…D4000` / **`…D4080`** | `…D4080` |
| `0x02E0` MLIST ctx 1 | `…D2000` / `…D3000` (own) | own |
| `PM_CAT_BASE_{VCE,TE,ALIST}1` | valid, distinct / valid, distinct | the second HWRTData's |
| `0x0280` / `0x02D0` | 2 / 9 | — |
| `0x0F38` `isp_ctl` | `0x80031000` / `0x80011000` | `0x80011000` |
| `0x0F48`, `0x0F50` | depth fields / 0 | 0 |
| `0x0FB0` scissor | `0x532C0` / `0x5B480` | `0x5B480` |
| `0x0F28` | `0x10` | — (4 × `mtile_stride`) |
| `0x0F18` | `0x00020002` (`isp_mtile_size`) | **`0x00040004`** in the 64×64 depth tests |
| `rgnhdr_single_rt_size`, `rgnhdr_scratch_offset` (command) | 0 / 0 | — |

The second render's **VCE1 catalogue page is the first render's ALIST1 page**
(`0x83D44001`). That is consistent with the pages having been freed and handed out again.

**2. `0x0F18 = mtile_stride` (both halves).** This matches the two values the spec
observed. Tried as default (build `…`, knob now `EXP_MTILE_STRIDE`):
- **every render is wrong**: `simple_draw` 0/4, `api.smoke` 4/8, `basic_draw` 58/331,
  with no fault;
- the input case still faults.

Reverted. `0x0F18 = isp_mtile_size` is right for our renders. The spec's two samples must
come from a different relation (`TO CONFIRM` on their side).

**3. Non-first geometry kicks.** The answer says a kick with `First kick:0` reloads nothing
in the PM: context 0 continues. Implemented: phase B is skipped when `GEOM_FLAGS_FIRSTKICK`
is clear. Regression with build `32bed6e3…`: `simple_draw` 4/4, `api.smoke` 8/8,
`basic_draw` 203/331 with 0 failures, no reset. The current tests do not exercise
multi-kick geometry, so this is untested beyond "no regression".

**Still open.**
- **The input-attachment fault.** All the bases the answer points to (region headers,
  MLIST, catalogues, list SPs) are the second HWRTData's. The remaining difference the
  answer describes is the **PB mode**. For the second HWRTData of a render pass, the
  original does **not** load the free lists and writes `CONTEXT_PB_BASE = 0`: both PM
  contexts share them, and their live local SPs move together. Ours reloads context 0 and
  writes `CONTEXT_PB_BASE = 3`. That is the next experiment. It needs care: after render
  1's pages are freed through context 1, context 0's live local SP is stale.
- `FRAG_PR`: the answer confirms it is consumed without running, which we already do.
- Partial render: `PM_PARTIAL_RENDER_ENABLE` (`0x0338`) and the INIT_PAGE restart (spec);
  OOM and grow through the `0x02A0`/`0x02A8` pause. Not implemented yet.

**Clean-room note.** Own firmware and measurements; values from the spec's observation.
