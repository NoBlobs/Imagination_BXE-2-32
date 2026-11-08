<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# E007 — read-only register survey `0x0000–0x2000`

**Question.** Which registers in the low RGX bank hold non-zero values once our
firmware is running? This is the baseline for the writable-bit probe (E008).

**Set-up.**
- Default build loaded, after one faulted compute kick (E006).
- GPU kept powered with `power/control = on`.
- `sudo python3 /tmp/reg-survey.py read 0 2000`, which reads every 64-bit register
  and prints the non-zero ones.

**Observation.**
- 93 non-zero registers. The full list is in [data/survey-0-2000.txt](data/survey-0-2000.txt).
- The SoC did not hang: reads of unimplemented offsets in this range return 0.
- Selected values:

| Offset | Value | Reading |
|---|---|---|
| `0x0000` | `002aaa002a22aaaa` | `CLK_CTRL` all AUTO |
| `0x0020` | `0024001d003400b6` | `CORE_REVISION` 36.29.52.182 |
| `0x0130` | `0x200` | `EVENT_STATUS`: MMU fault from E006 still latched |
| `0x0418` | `000000268e440000` | unknown, address-like |
| `0x0480`/`0x0498` | `0x80_000b8000` / `0x80_0006b000` | CDM bases left by the kick |
| `0x04A8..0x04C0` | as in E006 | CDM PDS registers |
| `0x0600` | `0080000030303000` | unknown, non-zero reset value |
| `0x0810` | `0x1CF80` | `MIPS_WRAPPER_CONFIG`: `BOOT_ISA_MODE` = microMIPS, `REGBANK_BASE_ADDR` = `0xCF80` |
| `0x0818..0x0860` | | host's fixed remaps (boot code `0x1FC00`, boot data `0x1FC01`, exception page `0x1FC02`) |
| `0x0878` | `0081bb90c0003039` | last remap written by the refill handler (entry 28 = slot 12 odd, VA `0xC0003000`) |
| `0x0B30`/`0x0B40` | `0xFFFF` | our MTS DM association |
| `0x0BB0` | `0x200` | `MTS_GPU_INT_STATUS` mirrors the event |
| `0x11D0..0x11F8` | pairs of equal values | unknown, three duplicated pairs |
| `0x1208` | `0x860b3000` | `BIF_CAT_BASE1` |
| `0x12B0`/`0x12B8` | `0x1041` / `00101800000026c0` | fault status (E006) |
| `0x1A00..0x1A10` | `c0de0006`, refills, last VA | our debug scratch |
| `0x1A78` | `0x10` | `SCRATCH15`: DM 4 (CDM) still marked busy, since the fault never completed |

**Conclusion.**
- This is the baseline map for later comparison.
- After a fault the CDM stays "busy" in our tables forever. Fault recovery (HWR) is
  needed before compute can be tested in a loop.

**Clean-room note.** Reads of hardware state produced by our own firmware.
