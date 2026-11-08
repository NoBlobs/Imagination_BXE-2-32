<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# E003 — the dynamic TLB flush must run from an unmapped page

**Question.** `KCCB_MMUCACHE` made the firmware stop responding. The host then timed
out on the MMU cache sync value.

**Set-up.** `tlb_flush_dynamic()` was first written in C, in `.text` at
`0xC0000000`. That address is mapped through a dynamic TLB entry and its remap pair.

**Observation.**
- `ROGUE_CR_MIPS_ADDR_REMAP_UNMAPPED_STATUS` (`0x0868`) became non-zero right after
  the MMUCACHE command. Its address was inside the firmware code page.
- The next instruction fetch raised a coprocessor-unusable (CpU) style exception from
  a fetch of garbage.
- The flush loop had invalidated the TLB entry and **disabled the remap entry of the
  page holding the loop itself**.

**Change.** `tlb_flush_dynamic` was moved to assembly in the exception page
(`0x9FC02000`, KSEG0). That page is translated by the host's fixed remap and never
by the TLB.

**Conclusion.** Code that changes TLB or remap entries must live in an unmapped
segment (KSEG0/KSEG1). The same rule applies to any future code that remaps its own
pages.

**Clean-room note.** Own firmware, public register names (`pvr_rogue_cr_defs.h`).
