<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# E033 — the Q16h first-load registers (`0x20F8`, `0x03D0`/`0x03D4`): the live SPs still do not load

**Question.** The answer to Q16h (spec section "PM state set on the first load after boot")
finds three registers that change at the first load after boot, before the first live copy,
and that we never wrote:

| Order | Change |
|---|---|
| 0 | `0x20F8` 0 → 1 (only at the first load) |
| +56 µs | `0x0240/0x0244` → `0x80_0000_0000` |
| +69 µs | `0x03D0` → `0x100`, `0x03D4` → `0x1` (bit 8 / bit 0 per geometry load; bit 16 / bit 8 per fragment load) |
| +82 µs | `0x0210`, `0x0224 = 0x1F`, live `0x0348 = 0x1F` |
| +93 µs | `0x20C0 = 0x1BF`, live `0x20C8 = 0x1BF` |

It also corrects Q16g: writing `+0x08` does not update the live register by itself. In
48 of 136 loads the live value follows 5–13 µs later, so **the copy is a separate step**,
not yet identified.

Do these writes make the PM load the free lists?

**1. Idle comparison after GPU init, before any job** (`tools/reg-survey.py read`, fresh
module load). Our non-zero registers in `0x0200–0x03FC` and `0x2000–0x20FC`:

```
0x0358 = 5   0x0388 = 1   0x03C0 = 0x7F   0x03C8 = 0x7F   0x03E0 = 1   0x03F8 = 0xFF
0x20F8 = 1
```

This is **identical to the original's list, except `0x20F8`**: our GPU init writes it to 1
(`boot-gpu-init` table), while the original reads 0 until the first load. By the time the
first geometry kick runs, `0x0388` reads 0 on ours (our TQ kick writes it to 0 as the Q13
table says); the original keeps 1.

**2. Variants** (`src/geom.c`, `-DEXP_TA`, `simple_draw_triangle_list`, firmware trace
reads of `0x20C8`/`0x0348` right after the writes). In every variant the code first writes
`0x20F8 = 1`, then `0x0240`, then `0x03D0 = (v & ~0x10000) | 0x100` and
`0x03D4 = (v & ~0x100) | 1`, then the free lists:

| Build | Extra | `0x03D0`/`0x03D4` after | `0x20C8` / `0x0348` |
|---|---|---|---|
| `a98645fa…` | no handshakes (`EXP_HS_MODE=0`) | `0x100` / `0x1` | 0 / 0 |
| `e5fc6a52…` | `0x20F8` written 0 then 1 (`EXP_20F8_EDGE`) | `0x100` / `0x1` | 0 / 0 |
| `5d4a71e9…` | `0x0388 = 1` before the load (`EXP_0388`) | `0x100` / `0x1` | 0 / 0 |
| `bd45623f…` | as above, plus handshakes after the writes (`EXP_HS_MODE=1`) | `0x100` / `0x1` | 0 / 0; both polls succeed |
| `0c7f5c73…` | as above (no handshakes), plus the bases rewritten **after** the stack tops (`EXP_TOP_FIRST`) | `0x100` / `0x1` | 0 / 0 |

The new registers hold what we write. `PM_CAT_BASE_VCE0` stays 0 and `0x0D00` goes to `0x0F`;
the TA stalls until the deadline, then HWR. No MMU fault.

**Reading.**
- With the idle state now equal to the original's and every changed register of the first
  load written in the observed order, the only missing piece is the **copy step** the
  answer describes: an action that leaves no value change in 6–12 µs sampling. Candidates
  are a short pulse on a command register, or a write of a value a register already holds.
- `0x03D0`/`0x03D4` behave like plain storage on our firmware. If they are "PM context
  loaded" flags that the PM sets after the copy, writing them ourselves is only cosmetic.
  That would also mean that, in the original, the action that sets them is the same one
  that copies the stack pointers.
- `0x0250`/`0x0258` as pulses (write 1, poll) do not trigger the copy, in any order tried
  (E032 and here).

This becomes **Q16i**.

**Default build** `5abba4d2…` restored on the board afterwards: `compute.pipeline.basic` 73/73.

**Knobs added in `src/geom.c`:** `EXP_NO_PM_FLAGS` (leave out the three Q16h writes),
`EXP_20F8_EDGE`, `EXP_0388`, `EXP_TOP_FIRST`. The board was booted again into
`7.4.0-rc1-img` with the serial GRUB script (the dirty team's capture had rebooted it into
the default 7.3 entry).

**Clean-room note.** Own firmware, own register reads, the spec's published observations.
