<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# E030 — TA with the global free list bank and `CONTEXT_PB_BASE`: still no allocation

**Question.** The answer to Q16e places:
- the global free list in a `0x40`-stride bank (`0x2078` for PM context 0, `0x20B8` for
  context 1) with base, allocated PB pages (`+0x18`) and allocated MMU pages (`+0x1C`,
  copied at `+0x28`);
- `CONTEXT_PB_BASE` at `0x02B0`, which is `3` at the geometry load.

Does the PM allocate now?

**Changes in `src/geom.c` (`-DEXP_TA`, build `2d9ff361…`).**
- `0x2078` = global `current_dev_addr`; `0x2090`/`0x2094`/`0x20A0` = global allocated
  PB / MMU / MMU pages.
- `0x2080` = global stack top: our candidate, 22-bit in E008, with the pair `0x20C0` in
  bank 1. The answer did not identify this register. Knob `EXP_NO_GLOBAL_SP` leaves it out.
- `0x02B0 = 3`.
- E029's wrong write of the stack top to `0x2094` is gone.

**Result** (`simple_draw_triangle_list`):

```
            0410 (VDM)      0224  0270/02C0  0D00  2078        2080   02B0  EVENT
running     0x80_0013F040   0x1F  0/0        0x0F  0x00048900  0x1BF  3     0x400
… unchanged until the deadline, then HWR
```

All new values are accepted, and nothing else changes.

**Reading `0x0D00` with the answer.**
- Original firmware: `0x3F` after jobs, so a bit set means idle. While geometry runs,
  `0x30`/`0x34`/`0x35`: units 0–3 busy, units 4 and 5 idle.
- Ours: `0x0F`, the exact opposite: units 0–3 idle, units 4 and 5 busy.
- The answer also shows that `0x0410 = stream base + 0x40` after a *completed* job in the
  original. So the VDM reaching `+0x40` does not mean it is stuck: the 64-byte stream of
  one triangle has been read.
- The stall is therefore after the VDM. Two units we cannot name are busy, and the
  geometry pipeline itself is idle, waiting for something the PM never provides.

**Not tested, on purpose.** The answer reports that `PM_CAT_BASE_{VCE,TE,ALIST}0` receive
new, valid (bit 0) entries at every geometry kick in the original, even when the host
leaves them at 0. These registers hold **physical** page-catalogue addresses that the PM
uses without the GPU MMU. Guessing values could make the PM write anywhere in system
memory (DRAM starts at physical 0 on the K1). So no experiment writes them with anything
but the host's values. This becomes Q16f.

**Conclusion.** Every PM register the spec has identified is now programmed as observed.
The remaining difference is the page-catalogue setup, plus the global stack pointer.
Without them, geometry waits forever for its first page. The default build (`76dad11c…`)
keeps geometry and fragment commands retired at once; `compute.pipeline.basic` stays at
73/73.

**Housekeeping.** Claude's scratchpad (temporary directory) was emptied during this
session, which took the SSH known-hosts file and helper scripts with it.
`tools/ta-sample.sh` now lives in the repository.

**Clean-room note.** Own firmware, the spec's published observations.
