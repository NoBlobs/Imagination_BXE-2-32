<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# E029 — TA kick with the Q16b/Q16c answers: still no PM allocation

**Question.** The spec now places the local free list, the allocation counters, the
per-context MLIST bases and the geometry constants (Q16b/Q16c answers). Does geometry now
get PB pages?

**Changes in `src/geom.c` (`-DEXP_TA`, build `fdc181e8…`).**
- `0x0220` is written 0: it reads 0 with the original firmware. `0x0224` holds the local
  stack top.
- `0x0398`/`0x039C` = the local free list's `allocated_page_count` and
  `allocated_mmu_page_count`.
- The geometry constants of both runs: `0x0C30/34 = 2/3`, `0x0C48 = 8`, `0x0C70 = 3`,
  `0x0C90/94 = 0x3089705F` ×2, `0x0CA0 = 0x10`.
- `0x02D8` (`pm_mlist_dev_addr`, PM context 0) was already being written. The answer
  confirms the register pair.

**Result** (`simple_draw_triangle_list`, sampled every 200 ms):

```
            6020 (TA cycles)  0410 (VDM ptr)  0224  0270/02C0  0D00   0420  EVENT
at rest     0                 0               0     0/0        0x3F   0     0x400
running     counting          0x80_0013F040   0x1F  0/0        0x0F   1     0x400
… unchanged until the context deadline (30 s), then HWR
```

- `0x0420` counts 1 per geometry kick, as the spec saw with the original firmware.
  `0x0D00` drops from `0x3F` (idle) to `0x0F` while the TA runs: two status bits (4, 5)
  report busy units.
- The VDM still stops after the first 64 bytes of the control stream. The local free-list
  stack top never moves, and no ALIST/MLIST entry appears.

**Experiment: global free list in the 0x2000 block** (`-DEXP_GLOBAL_FL`, `92e98527…`). The
answer says the global free list's PM registers are outside `0x0200–0x043F`. In the E008
masks, the `0x2000` block repeats the PM shapes:
- two-22-bit fields at `0x2090`/`0x20D0`, like `0x0220`/`0x0398`;
- 40-bit addresses at `0x2078`/`0x20B8`.

The global free list's `current_dev_addr` was written to `0x2078` and its stack top to
`0x2094`. The registers hold the values (`0x2078 = 0x00048900`, `0x2094 = 0x1BF`), but
**nothing changes**: same stall, same status.

**Conclusion.**
- The geometry pipe runs and is stuck before the first PM allocation. The remaining
  missing pieces are PM configuration that produces no fault, so neither the canary
  method nor value matching applies:
  - the global free list's real registers;
  - how the PM is started or unpaused (`PM_PAGE_MANAGEOP`, `0x02A0`, reads 0 here);
  - possibly the PM page catalogues.
- Further blind combinations are unlikely to succeed. This becomes Q16e.
- The default build (`c86de988…`) keeps geometry and fragment commands retired at once.
  `compute.pipeline.basic` is still 73/73.

**Clean-room note.** Own firmware, the spec's published observations, our own probe data.
