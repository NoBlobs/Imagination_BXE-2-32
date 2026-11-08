<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# Registers

The table lists every RGX register the firmware touches, with where its name and offset
come from:

- **P**: public `pvr_rogue_cr_defs.h` (upstream `drm/imagination`)
- **S**: the clean spec (`docsClean/registers.md`, `kick-*.md`)
- **E**: found by experiment on the board (see `logRefs/`)

The firmware reaches registers in three ways:

- the core bank at `0xCF800000 + off`, through wired TLB entry 2;
- the XPU broadcast bank at `0xCFA00000 + off`, as `bcast_write*`;
- 64-bit registers are written as two 32-bit stores, low word first.

## Used by the firmware

| Offset | Name | Src | Use |
|---|---|---|---|
| `0x0020` | `CORE_REVISION` | P | dummy read that drains posted writes |
| `0x0128` | **`EVENT_ENABLE`** | E | writable mask `0x0008FFFE`. It routes events to the **host** IRQ, so it must stay 0 ([E011](logRefs/E011-event-enable-0x0128.md)) |
| `0x0130`/`0x0138` | `EVENT_STATUS`/`EVENT_CLEAR` | P | GPU events: bit 2 `COMPUTE_FINISHED`, bit 9 `MMU_PAGE_FAULT`, … |
| `0x0160` | `TIMER` | P | 48-bit GPU timer (clock / 256), used for trace timestamps |
| `0x0478` | CDM run | S | compute kick trigger |
| `0x0480` | CDM control stream base | S | compute kick |
| `0x0498` | CDM context state base | S | compute kick |
| `0x04A8`/`B0`/`B8`/`C0` | `CDM_CONTEXT_PDS0/1`, `CDM_TERMINATE_PDS/1` | P | compute kick |
| `0x0610` | **`PDS_EXEC_BASE`** (named in `pvr_rogue_heap_config.h`) | E, S | `= sysinit.pds_exec_base`, bits 39:20. Found by the probe ([E009](logRefs/E009-pds-exec-base-0x0610.md)), confirmed by the spec. |
| `0x4008`/`0x4010`/`0x4028` | **USC exec base ×3** | S | `= sysinit.usc_exec_base` ([E013](logRefs/E013-gpu-units-init-compute-pass.md)) |
| `0x0FC8` | ISP register (fragment kick) | S, E | bits 39:34 writable. **Not** the USC base ([E012](logRefs/E012-usc-base-0x0fc8.md)). TQ writes 0, 3D writes 0. |
| `0x06A0`/`A8`/`B0`/`B8` | `PDS_BGRND0/1/2_BASE`, `PDS_BGRND3_SIZEINFO` | S, E | TQ/3D PDS background programs; layouts match public Mesa and the probe masks ([E014](logRefs/E014-tq3d-kick-does-not-start.md)) |
| `0x0878` | `MIPS_ADDR_REMAP_RANGE_CONFIG` | P | MIPS remap entries (boot, refill, flush) |
| `0x08A0` | `MIPS_WRAPPER_IRQ_ENABLE` | P | = 1 at init |
| `0x08A8` | `MIPS_WRAPPER_IRQ_STATUS` | P | = 1 interrupts the host |
| `0x0B08` | MTS task end | S | 0 after the BG task, 2 after the IRQ task |
| `0x0B30`/`0x0B40` | `MTS_BGCTX/INTCTX_THREAD0_DM_ASSOC` | P | BG = `1` (DM0), INT = `0xFFFF` |
| `0x0B58 + 8n` | `MTS_DMn_INTERRUPT_ENABLE` | P, S | DM0 `0x89200`, DM2 `0xA0`, DM3 `0x08`, DM4 `0x04` (see [E016](logRefs/E016-completion-path-counters.md)) |
| `0x1200 + 8n` | `BIF_CAT_BASEn` | P | page catalogue of BIF slot n |
| `0x1240` | `BIF_CAT_BASE_INDEX` | P | DM → slot (TA bit 0, 3D bit 8, CDM bit 16, TDM bit 19) |
| `0x12A0` | `BIF_CTRL_INVAL` | P | MMU cache invalidation (PT/PD/PC/TLB) |
| `0x12B0`/`0x12B8` | `BIF_FAULT_BANK0_MMU/REQ_STATUS` | P | read when debugging faults |
| `0x1348` | PM TLB invalidate | S | `MMUCACHE` with the PMTLB flag |
| `0x17A8` | TPU border colour table (CDM) | S | compute kick |
| `0x1A00..0x1A78` | `SCRATCHn` | P | debug mailboxes, see [boot-and-memory.md](boot-and-memory.md#debug-scratch-registers) |
| `0x3818`/`0x3820` | `SLC_CTRL_FLUSH_INVAL`/`SLC_STATUS0` | P | SLC flush after each DM completion |
| `0xF308` | multicore system | S | read before a compute kick |
| `0xF330` | multicore compute control | S | `execute_count << 8 \| 1` (broadcast) |

## GPU bring-up registers (spec, `boot-gpu-init.md`)

| Offset | Src | Value written by `gpu_init.c` |
|---|---|---|
| `0x0038` | S | power-island request: `1`, then `3`; poll `POWER_COMPLETE` |
| `0x0050` | S | active dusts `(1 << n) − 1` |
| `0x0890` `XPU_BROADCAST` | P | `1` |
| `0xA100` `SYS_BUS_SECURE` | P | `0` |
| `0x13E0` `BIF_FAULT_READ` | P | `sysinit.fault_phys_addr` |
| `0x3828` `SLC_CTRL_BYPASS` | P | `\|= 0x01000000`, high `\|= 0x2000` (16 KiB SLC) |
| `0x6000` | S | reset pulse `1` → `0` |
| `0x0600..0x0720`, `0x18C0..0x1900`, `0x1498`, `0x0358`, `0x20F8`, `0x2010`, `0x2100`, `0x13F8`, `0x4050`, `0x06D8` | S | unit defaults (table in the spec) |
| `0x4068`, `0x0CD8` | S | write `1`, poll bit 0 → 0 (init done) |
| `0x0660`, `0x0670` | S | `1` |
| `0x17B8` | S | `sysinit.filter_flags` |

## Values the host leaves after boot

These come from a read-only survey with the clean firmware idle, after one faulted
compute kick ([E007](logRefs/E007-register-survey.md),
raw data in `logRefs/data/survey-0-2000.txt`).

| Offset | Value | Note |
|---|---|---|
| `0x0000` | `0x002AAA002A22AAAA` | `CLK_CTRL`: the host sets every unit to AUTO |
| `0x0018` | `0x08470000` | `CORE_ID` |
| `0x0020` | `0x0024001D003400B6` | `CORE_REVISION` = 36.29.52.182 |
| `0x0080` | `0x0000000800000000` | `CLK_XTPLUS_CTRL` |
| `0x0810` | `0x0001CF80` | `MIPS_WRAPPER_CONFIG` (boot ISA microMIPS, register bank base `0xCF80`) |
| `0x0818..0x0860` | | `MIPS_ADDR_REMAP1..5_CONFIG1/2`: the host's fixed remaps for boot code, boot data and exception page |
| `0x08D0` | `0x20` | `MIPS_EXCEPTION_STATUS` while running |
| `0x08F8` | `0x6` | `MARS_IDLE` |
| `0x1490` | `0x4CD33` | no public name |
| `0x18C0..0x18D0` | `0x7FFFFF` | no public name (three identical registers) |

## Writable-bit map (black-box probe)

[E008](logRefs/E008-writable-mask-probe.md) wrote all-ones to bits 63:4 of every
register in `0x0200–0x0800` and `0x0C00–0x3000`, outside a deny list, then read the
value back and restored the original. Registers whose writable mask has the shape of a
40-bit virtual address are interesting, because every base address the host gives in
`sysinit` or the command structures is a 40-bit GPU VA:

| Mask shape | Meaning | Offsets |
|---|---|---|
| `0x000000FF_FFFFFFF0` | 40-bit address, 16-byte aligned | `0x210 0x218 0x240 0x248 0x2D8 0x2E0 0x408 0x428 0x480* 0x498* 0xC30 0xCC0 0xF20 0xF50–0xF78 0xFB0–0xFC0 0x2018 0x2040 0x2078 0x20B8 0x2108 0x2110` |
| `0x000000FF_FFFFFFC0` | 40-bit, 64-byte aligned | `0xC38 0xC40 0xCB0` |
| `0x000000FF_FFF00000` | **40-bit, 1 MiB aligned** | **`0x0610`** (unique) |
| `0x000000FC_00000000` | bits 39:34 only (16 GiB granule) | **`0x0FC8`** (unique) |
| `0x0000003F_FFFFFFF0` | 38-bit, 16-byte aligned | `0x1788 0x1790 0x17A0 0x17A8*` |
| `0x000007FF_FFFFFFE0` | 43-bit, 32-byte aligned | `0x1740` (`MCU_FENCE` is next to it at `0x1740`) |

`*` = registers whose role is already known (CDM control stream, CDM context state, TPU
border colour).

`0x0610` is the only register with a 1 MiB granule. The upstream heap configuration
requires the PDS and USC heap bases to be multiples of 4 MiB. That makes `0x0610` the
best candidate for `ROGUE_CR_PDS_EXEC_BASE`. `0x0FC8` (bits 39:34) holds
`ROGUE_USCCODE_HEAP_BASE = 0xE000000000` exactly (`0xE000000000 >> 34 = 0x38`), but
not the PDS base `0xDA00000000`. It is therefore a candidate for one of the
`USC_CODE_BASE_*` registers.

Side effect noted by the probe: `0x0C50` bit 30 stays set after it is written
("restore mismatch"). It is a sticky or self-setting bit, and the next GPU soft reset
clears it.
