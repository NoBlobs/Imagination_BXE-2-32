<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# E034 — Q16i answer applied: `0xF320`, the unit values of the first load, the TA catalogue index; still no live copy

**Question.** The answer to Q16i (spec section "What happens around the live copy") finds:
- `MULTICORE_GEOMETRY_CTRL_COMMON` (`0xF320`) stays **0** in the original;
- at the first load, `0x1500`, `0x1780`, `0x1788`, `0x178C`, `0x0FA0` and `0x17B8` change
  together with the unit init;
- `CAT_BASE1` and `CAT_BASE_INDEX` change in the **same** sample as the local live copy;
- the copy itself goes with no visible register change; candidates left are the step behind
  trace PM 30 (`PM running primary config`), the range `0x0A00–0x0BFF`, and writes of
  unchanged values.

Which of these makes the PM copy its stack pointers?

**1. State during our stall vs the original's first load**
(`simple_draw_triangle_list`, host read at 1.8 s):

| Register | Original after the first load | Ours |
|---|---|---|
| `0xF320` | 0 | **1** (our phase A wrote it; not in the spec, our guess from the compute kick) |
| `0x1240` `CAT_BASE_INDEX` | `0x1` | `0x101` (TA = 1; the PIXEL field from an earlier transfer) |
| `0x1208` `CAT_BASE1` | the application's catalogue | the application's catalogue |
| `0x1500` | `0x80` | 0 |
| `0x1780` | `0x21` | `0x40` |
| `0x1788` / `0x178C` | `0x20` / `0x20` | 0 / 0 |
| `0x0FA0` | `0x88` | 0 |
| `0x17B8` | `0xA` | 0 |
| `0x0C78`, `0x0CE0`, `0x1498`, `0x18E8`, `0x18C0` | `0x88`, 1, 1, `0xF`, `0x7FC000` | the same |

**2. Variants** (all with the E033 first-load writes and no handshakes):

| Build | Change | `0x20C8` / `0x0348` |
|---|---|---|
| `0e55a519…` | no `0xF320` write (`EXP_NO_F320`) | 0 / 0 |
| `563bb590…` | plus the six unit values above, before the PM writes (`EXP_UNIT_EXTRA`) | 0 / 0 |
| `328340e4…` | plus the TA catalogue index written **after** the PM writes (`EXP_IDX_AFTER`; 0 during them), then ~2000 loop iterations | 0 / 0 |

Each value lands; nothing else changes. Same stall and HWR at the deadline.

**3. The `0x0800–0x0BFF` range on ours.** A read while idle and while the TA is stalled shows
only the MIPS wrapper and remap registers (`0x0810–0x08F8`), the MTS DM interrupt enables
(`0x0B58–0x0B78`), and `0x0B20`/`0x0B30`/`0x0B40`/`0x0BB0`/`0x0BC8`. None of them looks
like PM configuration, and none changes with the PM writes.

**Reading.**
- With `0xF320` back to 0, the unit values matched, the memory context bound and the
  index written before or after, our register state at the TA kick now matches every
  value the spec observed in the original. Only the live stack pointers differ.
- The missing step leaves no register value change, so register sampling cannot find it
  any more. It can only be:
  - a write of a value the register already holds;
  - a pulse shorter than one sample;
  - a register outside every sampled range.
- That needs the sequence of **writes** the original makes between `Activate MemCtx` and
  `Load Freelist`, including writes of unchanged values. This is Q16j.

**Default build** `69f4e8d2…` restored on the board afterwards: `compute.pipeline.basic` 73/73.

**Knobs added:** `EXP_NO_F320`, `EXP_UNIT_EXTRA`, `EXP_IDX_AFTER`.

**Clean-room note.** Own firmware, own register reads, the spec's published observations.
