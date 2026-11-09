<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# E036 — first 3D render: geometry + fragment draw a correct triangle; the PM does not free the pages

**Question.** With the free lists loading (E035), can the fragment kick of the spec plus
the same load commands on PM context 1 finish a whole render?

**Implementation** (`src/frag.c`, new; reached with `-DEXP_TA`; build `ad65129f…` with
`-DEXP_TA -DEXP_HS_MODE=0 -DEXP_NO_F320`):
- **Geometry completion** (`geom_done`) also saves the PM page catalogues the PM built
  (`PM_CAT_BASE_{VCE,TE,ALIST}0`) in the HWRTData `*_last_cat_base` fields, next to the
  ALIST/MLIST stack pointers.
- **Fragment kick**, phases of `kick-compute-and-3d.md`:
  - **A.** `0xF310 = 1`, then memory context DM 3, then the **PM context 1 load**:
    - *store from context 0:* local live SP `0x0348` → free list stack top; global live SP
      `0x20C8` and live counts `0x20D8`/`0x20E0` → global free list;
    - flags `0x03D0 |= 0x10000`, `0x03D4 |= 0x100`;
    - *local, context 1:* `0x0218` base, `0x0230` = top << 22, counts `0x0398`/`0x039C`,
      then `0x0200 = 1` and poll bit 0 = 0;
    - *global, context 1:* bank `0x2078` (base, `+0x08` top, `+0x18`/`+0x1C` counts), then
      `0x20A8 = 1` and poll;
    - `0x02B0 = 0`, MLIST `0x02E0`, `PM_CAT_BASE_*1` from the saved catalogues, and the
      ALIST/MLIST SPs written to `0x0278`/`0x02C8`.
  - **B.** `0x4070`, clear registers `0x4078…0x4090`, `0x40A0` view index, `0x0F20` region
    headers, `0x3E40` = `screen_pixel_max`, `0x0F18` = `isp_mtile_size`.
  - **C.** scissor `0x0FB0`, depth bias `0x0FB8`, `0x0F80`/`0x0F88` background depth and
    values, `0x0F30` `isp_aa`, `0x17A0` border.
    - Not in the spec's 3D list, so written as in the transfer kick: `ISP_CTL`, tiles in
      flight `0x0FD8`, `PDS_BGRND0/1/3` from `pds_bgnd[0..2]`.
  - **D.** `ISP_RENDER = 0`, `0x0618 = 0x80`, `0x0620` = `event_pixel_pds_data`, `0x0628`
    = `event_pixel_pds_info`, `0x0F10`/`0x0FC8`/`0x0FCC` = 0, PBE words 0/1 of eight
    targets into `0x1510+8n`/`0x1550+8n`, `0x0298 = 0`, `0x01C8 = 0`.
  - **E–F.** trace `Kick 3D` (MAIN 120), then `0x0F00 = 1` and `MTS_SCHEDULE = 0`.
- **Fragment completion** (`frag_done`, on `PIXELBE_END_RENDER`): the context 1 live SPs
  are stored back into the free lists, and the HWRTData state is set to `FRAG_FINISHED`.

**Result 1: one draw renders correctly.** `dEQP-VK.draw.renderpass.simple_draw.simple_draw_triangle_list`
run alone: **Pass**, with the image compared by the CTS. This is the first image the clean
firmware has drawn. Trace (raw lines in `data/e036-two-draws-trace.txt`):

```
Kick TA  …  TA finished                      (~1.4 ms)
0xE0360B00  load ok local=1 global=1, live SP ctx1 local 0x10, global 0x1BF, 0x0280=0, 0x02D0=0
0xE0360C00  pds_info 0x201, pds_data 0x29C0, isp_ctl 0x80031000, isp_aa 0, mtile_size 0x40004, flags 0x8000
Kick 3D: … RTD 0xc0080000, Partial render:0 …
0xE0360D00  ctx1 live SP local 0x10, global 0x1BF, 0x0D00 = 0x3F
3D Transfer finished                          (~1.2 ms after the kick; the host's read-back TQ follows)
```

**Result 2: groups.**

| Group | Pass | Fail |
|---|---|---|
| `draw.renderpass.simple_draw.*` | 1 (the first case of the run) | 3 |
| `api.smoke.*` | 5: `triangle` **renders**, plus compute, transfer, sampler, shader | 3: `asm_triangle`, `asm_triangle_no_opname`, `unused_resolve_attachment` (image comparison) |

**Why later draws fail.** Two draws in a row (`simple_draw_instanced_triangle_*`): the first
passes. The second draw's geometry writes at GPU address `0x10` (BIF0 fault, MMU status
`0xB021`, request `0x00013000_00000010`: a write), then HWR. The fragment after it faults
too (request `0x00102500_00002000`).

The state the first render leaves explains it:
- **The PM never freed the pages.** In the original, the fragment returns the local pages
  and the context 1 local live SP climbs back to `0x1F`. Ours stays at `0x10` after the 3D.
- **The ALIST/MLIST stack pointers of PM context 1 (`0x0280`/`0x02D0`) stay 0** after we
  write `0x0278`/`0x02C8`. Neither register is writable (E008 masks). So the PM's context 1
  has no list of the pages to free, and the render is left half-finished in the PM.
  The next geometry load then starts from that state.
- `0x0390`/`0x0394` read 0 after the geometry. They are load values, not live counters.

**Conclusions.**
- **Geometry → fragment → PBE works end to end** on the clean firmware. The free-list load
  commands work for PM context 1 as well (`0x0200`, `0x20A8`, both polls succeed, live SPs
  loaded).
- What is missing is the **ALIST/MLIST stack-pointer load of PM context 1** (the trace
  line `Loading stack-pointers for 1 (0:MidTA,1:3D) on context 1`) and the **page
  deallocation at the end of the 3D** (Q20). Without them every render leaks its pages and
  leaves the PM in a state that faults on the next geometry. This is Q16k.
- No guessed command writes were tried in the PM range: a wrong command could make the PM
  write through its physical page catalogues.

The default build (no `EXP`) still retires geometry and fragment commands at once.

**Clean-room note.** Own firmware, own measurements and own register reads, the spec's
published observations; public `rogue_fwif_frag_regs` / `rogue_fwif_hwrtdata` layouts.
