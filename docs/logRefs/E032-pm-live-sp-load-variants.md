<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# E032 — loading the PM's live stack pointers: six variants, none loads them

**Question.** The answer to Q16g says:
- the geometry bank of the global free list is **`0x20B8`** (the Q16e table had the banks
  swapped);
- writing **`+0x08` (`0x20C0`)** loads the read-only live stack pointer **`0x20C8`** in the
  same sample;
- the local live stack pointers are **`0x0348`** (PM context 0) and **`0x0350`** (context 1);
- the first change at a first load is `0x0244` 0 → `0x80`
  (`0x0240/0x0244 = 0x80_0000_0000`);
- `0x0250`/`0x0258` were never seen non-zero, and no memory header is needed.

Does the PM load the free lists and start allocating?

**Common part of every build** (`src/geom.c`, `-DEXP_TA`):
- `0x0240` = `0x80_0000_0000`, then local `0x0210` base and `0x0224` top, VHeap `0x0248`;
- global bank: `0x20B8` base (`0x48900`), `0x20C0` top (`0x1BF`), `0x20D0`/`0x20D4` counts;
- `0x02B0 = 3`, local counts `0x0398`/`0x039C`, MLIST `0x02D8`, ALIST/MLIST SPs;
- firmware trace points (`0xE0280Bxx`) that read `0x20C8` and `0x0348` right after the
  writes, so the result does not depend on the host's 200 ms sampling.

**Variants** (`simple_draw_triangle_list`):

| Build | Variant | `0x20C8` | `0x0348` | Other result |
|---|---|---|---|---|
| `…` (E031 order) | handshakes `0x0250`/`0x0258` after the PM writes (knob `EXP_HS_MODE=1`, the default) | 0 | 0 | both polls succeed; TA stalls until the 30 s deadline |
| `18a4cb9b…`, `2862e8c6…` | PM writes **between** `0x0250 = 1`/poll and `0x0258 = 1`/poll (`EXP_HS_MODE=2`) | 0 | 0 | the `0x0258` poll fails (`HW poll 600 failed`); **MMU read fault at address 0**, requester `0x0011B000_00000000` (XE layout: RNW = 1, `TAG_SB = 4`, `TAG_ID = 0x1B`); then HWR |
| `1ec6d58c…` | global bank written inside **`PM_PAGE_MANAGEOP` pause** (`0x02A0 = 1` … `0x02A0 = 0`, `EXP_PM_PAUSE`), as the spec saw at freelist updates | 0 (also inside the pause) | 0 | **`0x02A8` acknowledges the pause**: 1 one read after `0x02A0 = 1`, 0 after `0x02A0 = 0` |
| `d6d14448…` | as above, PM registers written through the **broadcast** bank (`EXP_PM_BCAST`) | 0 | 0 | the values land (`0x20C0 = 0x1BF`, `0x0224 = 0x1F`) |
| `b56f3a11…` | as above, **every clock forced ON** (`CLK_CTRL = 0x0015550015115555`, `EXP_CLK_ON`) | 0 | 0 | `CLK_STATUS` low word `0x07F075FF`; nothing else changes |
| `f6d7a347…` | **no handshakes**, pause, 32-bit-only writes of `0x20C0` and `0x0224` (`EXP_HS_MODE=0 -DEXP_SP32`) | 0 | 0 | same stall |

In every variant `PM_CAT_BASE_VCE0` (`0x1248`) stays 0, `0x0D00` goes from `0x3F` to
`0x0F`, and the TA runs (cycle counter counting) until the context deadline.

**Findings.**
1. **`0x02A8` is the ack of `PM_PAGE_MANAGEOP`.** It follows `0x02A0` within one register
   read. So the PM is clocked, out of reset, and answers register commands.
2. **The live stack pointers do not follow `+0x08`/`0x0224`** on our firmware, whatever the
   order, the bank (core or broadcast), the width of the write, the handshakes or the clock
   mode. In the original they follow in the same sample. **A precondition is missing**:
   PM state set elsewhere, at boot or by the host, that we do not set.
3. **Writing the PM state while `0x0250 = 1` makes a unit read address 0.** The handshake
   therefore *does* start something, probably a PM state load or store that uses registers
   still 0 at that time. Done after the writes, it raises no fault and changes nothing
   visible. Its order and meaning are still unknown.
4. Clock gating and the broadcast/core bank choice are **ruled out**.

**Not tried, on purpose.** Writing guessed values to `PM_CAT_BASE_*` (physical addresses,
no MMU), and writing unknown registers in the `0x2000` block beyond the spec's list.

This becomes **Q16h**.

**Default build** `1b572d7f…` restored on the board afterwards: `compute.pipeline.basic` 73/73.

**Knobs left in `src/geom.c`:** `EXP_HS_MODE` (0/1/2), `EXP_PM_PAUSE`, `EXP_PM_BCAST`,
`EXP_CLK_ON`, `EXP_SP32`. The default build (no `EXP`) keeps geometry and fragment commands
retired at once.

**Clean-room note.** Own firmware, the spec's published observations, public mainline
register layouts (fault request status).
