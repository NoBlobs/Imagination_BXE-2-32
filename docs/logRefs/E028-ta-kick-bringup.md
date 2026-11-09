<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# E028 — first geometry (TA) kick: it runs, but the PM hands out no pages

**Question.** With the partial spec answer and the E027 mapping, can a geometry job run?

**Implementation** (`src/geom.c`, compiled only with `-DEXP_TA`). It follows phases A–G
of `kick-geometry-ta.md`, at the fixed offsets of the Q15 answer:
- **A.** `MULTICORE_GEOMETRY_CTRL_COMMON = 1`, bind DM 2.
- **B. PM context 0:**
  - VHeap `0x0248`; local free list address `0x0210`;
  - `0x0220` = total pages (low) and stack top (high);
  - `PM_CAT_BASE_{VCE,TE,ALIST}0` from the HWRTData (0 on a first kick);
  - ALIST/MLIST stack pointers, MLIST base candidate `0x02D8`;
  - handshakes `0x0250`/`0x0258`.
- **C.** `ppp_ctrl`, `te_psg`, region headers `0x0C38`, TPC `0x0C40`, border colour
  `0x17A0`, and the E027 registers (`0x0C98`, `0x0C18`, `0x0C08`, `0x0C10`, `0x0C20`,
  `0x0C78`, `0x0C80`, `0x0418`); VDM stream `0x0408`.
- **D.** VDM state base candidate `0x0428`, TA state base `0x0CB0` (see below),
  `0x0CE0 = 1`.
- **E.** `0x0C68 = 1`, then poll.
- **F.** Trace `MAIN_119`; HWRTData state `KICK_GEOM(_FIRST)`.
- **G.** `0x0400 = 1`.
- **Completion:** on `TA_FINISHED`, the partial geometry flush, then the ALIST/MLIST
  stack pointers and the state `GEOM_FINISHED` are written to the HWRTData.

Test case: `dEQP-VK.draw.renderpass.simple_draw.simple_draw_triangle_list`.

## Iterations

| # | Change | Symptom |
|---|---|---|
| 1 | first version | MMU fault, **read at `0x200`**; it appears before the TA trigger |
| 2 | MLIST base at `0x02D8` | same |
| 3 | `EVENT_STATUS` traced after every step | clean until phase D, faulted after `0x0C68 = 1`: the fault happens when the geometry **context is enabled** |
| 4 | VDM state base → `0x0428`, TA state base → `0x0CC0` | same |
| 5 | **canaries** (`-DEXP_CANARY`): unmapped VAs `0x1_0k00_0000` written to `0x0230`, `0x0240`, `0x02E0`, `0x0CB0` | fault at **`0x1_0400_0200`**, canary 4: the hardware reads `[0x0CB0] + 0x200` when the context is enabled |
| 6 | `0x0CB0` = `geom_registers_caswitch.geom_reg_ta_context_state_base_addr` (`0x80_0006_F000`) | **no fault.** `PERF_TA_CYCLE` (`0x6020`) counts at the core clock; the VDM pointer `0x0410` moves to `stream_base + 0x40` (the spec saw the same value after its job); then a lockup, and the watchdog recovers 30 s later |
| 7 | `0x0220` low field = `current_pages` (`0x20`); ALIST/MLIST SPs written to the writable `0x0278`/`0x02C8` (the `0x0270`/`0x02C0` copies are read-only in E008) | same: `0x0220 = 0x20` and `0x0224 = 0x1F` accepted, the VDM stays at `+0x40`, the ALIST/MLIST SPs stay 0 and the stack top does not move |

## Findings

- **`0x0CB0` is the TA context-state base:** `geom_reg_ta_context_state_base_addr`,
  64-byte aligned. It is read at `+0x200` when the geometry context is enabled. This was
  found purely by the canary method.
- With it, **the geometry pipe starts**: the TA cycle counter runs, and the VDM fetches
  the first 64 bytes of the control stream, matching the spec's observation.
- **The PM gives no pages.** The free-list stack top stays at `0x1F` and no ALIST/MLIST
  entry is written, so geometry stalls and the watchdog recovers after the context
  deadline. Something in the PM load is still missing. Candidates:
  - the global free list registers;
  - the allocated-page and MMU-page counts printed by the public "Load Freelist" trace;
  - the PM page catalogues, which the host leaves at 0;
  - another enable.

  None of these shows up as a fault, so the canary method does not apply. This is the
  open part of Q16.
- The canary method itself is a useful tool: write distinct unmapped addresses into
  candidate address registers, and the MMU fault address names the register in use.

**State of the code.** The TA kick is kept behind `-DEXP_TA`. By default, geometry and
fragment commands are retired at once (no 30 s stall per draw). Compute stays at 73/73 with
the default build (`6a33a086…`).

**Clean-room note.** Own firmware, public layouts, the spec's published observation.
