<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# E037 — ALIST/MLIST stack-pointer load commands `0x0288`/`0x0190`: pages are freed, consecutive renders work

**Question.** E036 drew one correct image, but:
- the PM never freed the render's pages;
- the next render's geometry wrote at GPU address `0x10`.

The Q16k answer (spec "Experiments on PM loading, freeing and storing", observation only)
gives:
- **Order at a fragment load:** `0x03D0`/`0x03D4` get their context-1 bits; then the
  loaded ALIST/MLIST SPs (`0x0278`/`0x02C8`) and the live ones (`0x0280`/`0x02D0`) change
  together.
- **Geometry load:** context 0's live SPs (`0x0270`/`0x02C0`) drop to 0 at every new load.
- **Live local counts:** `0x03A0`/`0x03A8` (PB) and `0x2000`/`0x2008` (MMU).
- **`0x0388`:** set back to 1 by the fragment kick, after a transfer clears it.
- **Freeing:** the PM frees the pages by itself after `PIXELBE_END_RENDER`, then raises
  `PM_3D_MEM_FREE`.
- **Command candidates** sampled at 0 in every read: `0x0190`, `0x0200`, `0x0208`,
  `0x0288`, `0x20A8`, `0x20B0`, plus `0x0268` and `0x02B8`.

What makes the ALIST/MLIST stack pointers live?

**Experiments** (`simple_draw_triangle_list` and the `simple_draw` group, firmware trace
right after each step):

| Build | Change | Result |
|---|---|---|
| `b40cb22f…` | SPs, catalogues and MLIST written **before** the free-list commands; live counts stored; `0x0388 = 1` | `0x0280`/`0x02D0` still 0; the free-list commands do not copy them |
| `9b995331…` | `0x03D0`/`0x03D4` bits set **after** the SP writes | still 0: the flag bits are not the trigger |
| `c989d430…` | **`0x0288 = 1`, poll bit 0 = 0** after `0x0278`/`0x02C8` | poll ok; **`0x0280` = 2** (ALIST live), `0x02D0` still 0 |
| `ae8469c2…` | plus **`0x0190 = 1`, poll** | poll ok; **`0x02D0` = 9** (MLIST live) |
| `29723594…` | same, with the `cr_poll32` failure trace fixed (below) | at the end of the 3D **`0x0350` = `0x1F`**: the local pages are back. The first render passes; the next geometry still faults at `0x10` |
| `fca1e2ca…` | at the geometry, `0x0268 = 1` and `0x02B8 = 1` | both **stay 1** (poll fails): plain storage, not commands; no effect |
| `cf759862…` | at the geometry, **`0x0288` and `0x0190` again** (after `0x0278 = 0`, `0x02C8 = 0`) | live `0x0270`/`0x02C0` go from **2/9 to 0**; **`simple_draw.*` 4/4, no fault, no HWR** |

**Findings.**
1. **`0x0288` loads the ALIST stack pointer** and **`0x0190` loads the MLIST stack
   pointer**, from the load registers `0x0278` and `0x02C8`. Both are self-clearing commands:
   write 1, wait for bit 0 = 0.
2. **The same two commands serve both PM contexts.** At the geometry they load context 0
   (`0x0270`/`0x02C0`); at the fragment they load context 1 (`0x0280`/`0x02D0`). The
   context is selected by the load in progress, most likely through the `0x03D0`/`0x03D4`
   bits written just before (bit 8 / bit 0 for the geometry, bit 16 / bit 8 for the
   fragment).
3. **Why the next render faulted.** Without the context-0 load, the second geometry
   started with the first render's ALIST/MLIST SPs (2/9, trace `0xE0370A00 = 0x00090002`)
   while its page catalogues were 0. The PM then appended to lists that no longer exist,
   which gives the write at `0x10`.
4. **Freeing** follows once context 1's list SPs are live: `0x0350` climbs back to `0x1F`.
   `PM_3D_MEM_FREE` (bit 8) has **not** been seen in `EVENT_STATUS` on our firmware: it
   read `0x400` at the end of a 50 ms wait. The wait is now capped at 3 ms
   (`EXP_MEM_FREE_US`). This is the cause of the 50 ms-per-render stall in the first
   builds.
5. `0x0268` and `0x02B8` are storage registers (they keep a written 1), not commands. The
   probe left them at 1 until the next module reload powered the GPU off.

**Side fix: the poll failure trace.** `cr_poll32` emitted `SFID_HWR_13` (`HW poll %u … failed
(reg:… val:…)`, 3 arguments) with 2 arguments. The trace parser then swallowed the next
entry, which is why the `frag_done` lines were missing in E036. It now passes
`(expected state, register, value)`. The earlier "HW poll 600 … reg:0x1" lines (E032) were
misprinted by the same bug.

**Results with the new defaults** (build `6e983056…`, `make EXP=-DEXP_TA`):

| Group | Pass | Fail | Not supported |
|---|---|---|---|
| `dEQP-VK.draw.renderpass.simple_draw.*` | 4 | 0 | 0 |
| `dEQP-VK.api.smoke.*` | **8** | 0 | 0 |
| `dEQP-VK.draw.renderpass.basic_draw.*` | **201** | 2 | 128 |

- No MMU fault, no HWR, no context reset in `dmesg` for the whole run.
- The two failures are `draw.point_list.45` and `misc.flat_b_sat_error` (image
  comparison). Per-case results are in `data/deqp-basic-draw-e037.txt`.

**`EXP_TA` defaults now:**
- no PM handshakes (`EXP_HS_MODE=0`) and no `0xF320` write (`EXP_F320` brings it back);
- free-list commands at both loads;
- `0x0288`/`0x0190` at both loads;
- `0x0388 = 1` at the fragment kick;
- the 3 ms wait for the PM free.

**Open.**
- `PM_3D_MEM_FREE` never seen.
- The `0x0C68` context-enable poll fails on later geometry kicks, without visible harm.
- The two `basic_draw` failures.
- Multi-kick geometry, OOM/grow and partial renders (SPM) are untested.
- ZLS (depth/stencil load and store) fields are not programmed.

**Clean-room note.** Own firmware and measurements. Command roles found by experiment from
the spec's register lists. No firmware code was involved.
