<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# E031 — the PM's live stack pointers stay 0: the free lists look empty to the PM

**Question.** The answer to Q16f says:
- the PM fills `PM_CAT_BASE_*0` itself, from local free-list pages (no firmware write);
- `0x0224` keeps the *loaded* stack top and is not a symptom;
- the global free list's **live** stack pointer is `0x2088` (`0x20C8` for context 1), with
  live allocated PB/MMU pages at `0x2098`/`0x20A0` and the counts of the last load/store
  at `0x2090`/`0x2094`.

With the stack pointer fixed, does the PM allocate?

**Changes in `src/geom.c` (`-DEXP_TA`, build `6ed8317b…`).**
- `0x2088` = global `current_stack_top`, `0x2098`/`0x20A0` = allocated PB/MMU pages,
  `0x2090`/`0x2094` = the same counts.
- The wrong `0x2080` write is gone.

**Observation** (`simple_draw_triangle_list`, sampled every 200 ms):

```
            6020 (TA)  0D00  2088 (gSP)  2098  1248 VCE0  1250 TE0  1260 ALIST0  12B0/12C0/1430
at rest     0          0x3F  0           0     0          0         0            0 / 0 / 0
running     counting   0x0F  0           0     0          0         0            0 / 0 / 0
```

- **`0x2088` and `0x2098` read 0** although the firmware wrote `0x1BF` (447) and the count.
  In the E008 probe `0x2088` was not writable either. **They are hardware-maintained live
  registers: writing them does nothing.**
- **The PM catalogues stay 0** for the whole job. In the original they are valid while
  geometry runs. **The PM has taken no page at all**, not even catalogue pages from the
  local free list.
- No MMU fault in bank 0 (`0x12B0`), bank 1 (`0x12C0`) or the Texas BIF (`0x1430`).
- `CAT_BASE_INDEX` has no separate PM field (public layout: TA, PIXEL, CDM, TDM, TLA, HOST,
  RAY, RVTX), so the PM should use the TA's slot, which is set.

**Reading.** The PM sees **empty free lists**: its live stack pointers are 0, so it waits
for pages forever. That matches every symptom since E028:
- no catalogue page;
- no ALIST/MLIST entry;
- geometry units idle and two other units busy (`0x0D00 = 0x0F`).

What is still missing is **how the firmware gets the free lists into the PM's live
state**. The registers written so far are configuration and shadow copies (base, loaded
top, counts). Somewhere a load has to copy them into the live stack pointer. Candidates:
- the PM handshakes `0x0250`/`0x0258` with a different value, order or preconditions;
- the unidentified `0x2080` (`+0x08` of the bank: small values `0x20`, `0x108`, `0x109` in
  the original, possibly a load command or a page count);
- the PM reading a free-list header from memory.

This is Q16g.

The default build (`57404eb5…`) keeps geometry and fragment commands retired at once;
`compute.pipeline.basic` stays at 73/73.

**Clean-room note.** Own firmware, the spec's published observations.
