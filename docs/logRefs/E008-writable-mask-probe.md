<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# E008 — writable-bit probe: candidates for the PDS/USC execution base

**Question.** The registers `ROGUE_CR_PDS_EXEC_BASE` and `ROGUE_CR_USC_CODE_BASE_*` exist
(they are named in the upstream `pvr_rogue_heap_config.h`) but their offsets are not
public. Can they be found from the shape of the writable bits?

**Hypothesis.** An exec base holds a 40-bit GPU virtual address that the host
guarantees to be a multiple of 4 MiB. Its writable mask should therefore be a run of
bits ending at bit 39 and starting at bit 22 or lower, with no low bits. Ordinary
address registers in the survey are 16-byte aligned (`0xFF_FFFFFFF0`).

**Set-up.**
- Default build, idle GPU kept powered (`control = on`).
- `reg-survey.py probe <start> <end>`: for each 64-bit register outside the deny list,
  1. read the original value;
  2. write `0xFFFFFFFF_FFFFFFF0 | (orig & 0xF)`, which leaves bits 3:0 untouched so
     that no trigger or enable bit is hit;
  3. read back, then restore the original and verify it.
- Deny list: `0x0000–0x0200` (clock, reset, events, timer); `0x0470–0x0480` (CDM
  run); `0x0400` and `0x0418` (possible TA run and its pair, see
  `kick-geometry-ta.md`); `0x0800–0x0C00` (MIPS wrapper, META slave port, MTS, IRQ);
  `0x0F00–0x0F10` (`ISP_RENDER`); `0x1200–0x1440` (BIF/MMU); `0x1A00–0x1AA0`
  (scratch); `0x3000–0x3A00` (FWCORE, SLC); `0xA000–0xA200`; `0xF000+`.
- Ranges probed: `0x0200–0x0800` and `0x0C00–0x3000`.

**Observation.** The full results are in [data/probe-200-800.txt](data/probe-200-800.txt) and
[data/probe-c00-3000.txt](data/probe-c00-3000.txt). The SoC stayed up, every value was
restored, and the firmware kept running.

Mask families:

| Mask | Offsets |
|---|---|
| `0x000000FF_FFFFFFF0` | `0x210 0x218 0x240 0x248 0x2D8 0x2E0 0x408 0x428 0x480 0x498 0xC30 0xCC0 0xF20 0xF50–0xF78 0xFB0–0xFC0 0x2018 0x2040 0x2078 0x20B8 0x2108 0x2110` |
| `0x000000FF_FFFFFFC0` | `0xC38 0xC40 0xCB0` |
| **`0x000000FF_FFF00000`** | **`0x0610` only** |
| **`0x000000FC_00000000`** | **`0x0FC8` only** |
| `0x0000003F_FFFFFFF0` | `0x1788 0x1790 0x17A0 0x17A8` |
| `0xFFFFFFF0_FFFFFFF0` (PDS pair layout) | `0x4A8 0x4B8 0x4D8 0x6A0 0x6A8` |
| `0x00000000_3FFFFFF0` (PDS1 layout) | `0x4B0 0x4C0 0x4E0` |
| full 64-bit | `0x438 0x448 0x450 0x460 0xC90 0xFA8 0x1510–0x1588` (16 registers) |

Side effect: `0x0C50` bit 30 stayed set after the restore. The next soft reset (driver
reload) clears it.

**Analysis.**
- `0x0610` is the only register in the probed range with a 1 MiB granule over a
  40-bit address. `0xDA_0000_0000` (PDS heap) and `0xE0_0000_0000` (USC heap) both fit.
  It is the prime candidate for **`PDS_EXEC_BASE`**, and possibly for a combined exec
  base.
- `0x0FC8` keeps only bits 39:34. `0xE0_0000_0000` fits exactly (`0x38 << 34`), and
  `0xDA_0000_0000` does not. It is a candidate for **one `USC_CODE_BASE_*`** register,
  the 16 GiB-aligned upper part of the USC code address.
- `0x06A0`/`0x06A8` have the same layout as the CDM PDS pairs (two 28-bit 16-byte
  aligned addresses). They are probably the PDS program registers of another unit.
  That fits a block of PDS-related registers around `0x0600`, which also contains
  `0x0610`.

**Conclusion.** Test `0x0610 = pds_exec_base` first (E009), then `0x0FC8`.

**Clean-room note.** Black-box probing of the hardware with our own firmware loaded. No
vendor firmware or driver was run or read.
