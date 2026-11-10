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

## Kick and recovery registers added in this round

| Offset | Src | Use |
|---|---|---|
| `0x0F00` | S | pixel-pipe launch: `BCAST = 1`, the last write of a 3D/TQ kick ([E017](logRefs/E017-tq3d-launch-fault-at-zero.md)) |
| `0x0F08` `ISP_RENDER` | P, S | `regs.isp_render`, written right before the launch (E023); `\| 0x10` only when resuming |
| `0xF310` `MULTICORE_FRAGMENT_CTRL_COMMON` | P | `1` (single-core GPU) |
| `0x1510 + 8n` / `0x1550 + 8n` | S, E | PBE `WORD0`/`WORD1` of MRT n (planar reading) |
| `0x0100`/`0x0104` `SOFT_RESET` | P, S | HWR pulse `0x13FFFC1D`/`0xFFEFFE00` → 0 ([E019](logRefs/E019-hwr-and-compute-basic-67.md)) |
| `0x0338` `PM_PARTIAL_RENDER_ENABLE` | P | 0 during HWR |

## Geometry registers (spec observation + E027/E028)

| Offset | Src | Value | Confidence |
|---|---|---|---|
| `0x0400` | S | `1`: TA/VDM trigger (last write) | spec |
| `0x0408` | S, O | `vdm_ctrl_stream_base` | spec observation |
| `0x0410` | E | VDM current pointer (read); reached `base + 0x40` in E028 | observed |
| `0x0418` | O, E | `VDM_CALL_STACK_POINTER` ← geometry context state `+8` (call stack init) | value match |
| `0x0428` | E | candidate `VDM_CONTEXT_STATE_BASE` | guess (E008 mask) |
| `0x0210` / `0x0218` | S, O | local free list address (one per PM context) | spec observation |
| `0x0220` / `0x0224` | O | `0x0220` reads 0 with the original firmware; `0x0224` = local free-list loaded stack top | spec observation |
| `0x0240` / `0x0244` | O | `0x80_0000_0000` (general heap base), the first change at a first load | spec observation (Q16g) |
| `0x0348` / `0x0350` | O, E | local free list **live** stack pointer, PM context 0 / 1; stays 0 on our firmware (E032) | spec (Q16g) + E032 |
| `0x0390` / `0x0394` | D, E | local free list allocated PB / MMU page counts loaded into PM context 0; not live (read 0 after the TA) | E035 statement + E036 |
| `0x0398` / `0x039C` | D, E | the same for PM context 1 (corrected: earlier docs had them as context 0) | E035 statement |
| `0x0208` / `0x0200` | D, E | **local free-list load command**, PM context 0 / 1: write 1 after the value registers, poll bit 0 = 0 | E035 (works) |
| `0x20B0` / `0x20A8` | D, E | **global free-list load command**, PM context 0 (bank `0x20B8`) / 1 (bank `0x2078`) | E035 (works) |
| `0x0230` / `0x0234` | D, E | local free-list stack top of PM context 1, 64-bit, top from bit 22 | E035 statement + E036 |
| `0x0350` | O, E | local free list live SP, PM context 1; loads with `0x0200` | spec + E036 |
| `0x0280` / `0x02D0` | O, E | ALIST / MLIST live stack pointer of PM context 1; loaded from `0x0278`/`0x02C8` by `0x0288`/`0x0190` | spec + E037 |
| `0x0288` | E | **ALIST SP load command**: write 1, poll bit 0 = 0; loads `0x0278` into the live SP of the PM context being loaded (`0x0270` at the geometry, `0x0280` at the fragment) | E037 |
| `0x0190` | E | **MLIST SP load command**: the same for `0x02C8` → `0x02C0` / `0x02D0` | E037 |
| `0x0268` / `0x02B8` | E | plain storage (keep a written 1), not commands | E037 |
| `0x03A0` / `0x03A8` | O | live local allocated PB pages, PM context 0 / 1 | spec (Q16k) |
| `0x2000` / `0x2008` | O | live local allocated MMU pages, PM context 0 / 1 | spec (Q16k) |
| `0x02A0` / `0x02A8` | O, E | `PM_PAGE_MANAGEOP`: 1 pauses TA allocation while the PM state changes; `0x02A8` acks it within one read | spec observation (trace) + E032 |
| `0x02D8` / `0x02E0` | O, E | MLIST base (`pm_mlist_dev_addr`) of PM context 0 / 1 | spec observation + E028 guess |
| `0x0420` | O, E | geometry kick counter (+1 per kick) | spec + E029 |
| `0x02B0` | O | `CONTEXT_PB_BASE`: `3` at the geometry load, `0` at the fragment load | spec observation |
| `0x20B8` / `0x2078` | O | global free list bank of PM context 0 (geometry) / 1 (fragment), stride `0x40`, `+0x00` = base (corrected by Q16g) | spec observation |
| `0x20C0` / `0x2080` | O, E | `+0x08`: stack pointer to load; in the original the live `+0x10` follows in the same sample, on ours it does not (E032) | spec (Q16g) + E032 |
| `0x20C8` / `0x2088` | O, E | global free list **live** stack pointer, context 0 / 1; read-only (E031) | spec + E031 |
| `0x2090` / `0x2094` | O | global PB / MMU page counts at the last load/store | spec observation |
| `0x2098` / `0x20A0` | O, E | global **live** allocated PB / MMU pages; read 0 after a write (E031) | spec + E031 |
| `0x0C30/0x0C34`, `0x0C48`, `0x0C70`, `0x0C90/0x0C94`, `0x0CA0` | O | constants `2/3`, `8`, `3`, `0x3089705F` ×2, `0x10` | spec observation |
| `0x0D00` | O, E | status: `0x3F` idle, `0x0F` while our stalled TA runs | observed |
| `0x0248` | O | `PM_VHEAP_TABLE` ← `vheap_table_dev_addr` | spec observation |
| `0x20F8` | O, E | 0 → 1 at the first load after boot, then stays 1; our init writes it 1 at boot | spec (Q16h) + E033 |
| `0x03D0` / `0x03D4` | O, E | per-load bits: geometry bit 8 / bit 0, fragment bit 16 / bit 8 ("PM context loaded"?); plain storage on ours | spec (Q16h) + E033 |
| `0x0388` | O, E | 1 after init and after jobs in the original; our TQ kick writes 0 (Q13 table) | spec + E033 |
| `0xF320` | P, O | `MULTICORE_GEOMETRY_CTRL_COMMON`: stays 0 in the original (Q16i); our earlier write of 1 removed in E034 experiments | mainline defs + spec |
| `0x1500`, `0x1780`, `0x1788`/`0x178C`, `0x0FA0`, `0x17B8` | O | change with the unit init at the first load: `0x80`, `0x40`→`0x21`, `0x20`/`0x20`, `0x88`, `0xA` | spec (Q16i) |
| `0x0250` / `0x0258` | S, E | PM handshakes; PM writes made while `0x0250 = 1` fault at address 0 (E032) | spec + E032 |
| `0x0270`/`0x0278`/`0x0280`, `0x02C0`/`0x02C8`/`0x02D0` | O, E | ALIST / MLIST stack pointers (`0x0278`/`0x02C8` writable) | partly |
| `0x0C08` / `0x0C10` | O, E | `TE_MTILE1/2` ← `te_mtile1/2` | value + mask + Mesa |
| `0x0C18` | O, E | `TE_SCREEN` ← `te_screen` | value + mask + Mesa |
| `0x0C20` | O, E | ← `mtile_stride` | value + mask |
| `0x0C28` | O | `TE_PSG` ← `te_psg` | spec observation + mask |
| `0x0C38` / `0x0C40` | O | `TE_PSGREGION_ADDR` ← `rgn_header_dev_addr`; `TE_TPC_ADDR` ← `tail_ptrs_dev_addr` | spec observation |
| `0x0C68` | S | `1`, poll bit 0 = 1 (context enable) | spec + E028 |
| `0x0C78` / `0x0C80` | O, E | ← `multi_sample_ctl` / `flipped_multi_sample_ctl` | value |
| `0x0C88` | O | `PPP_CTRL` ← `ppp_ctrl` | spec observation |
| `0x0C98` | O, E | `PPP_SCREEN` ← `screen_pixel_max` | value + mask + Mesa |
| `0x0CB0` | **E** | TA context-state base ← `geom_reg_ta_context_state_base_addr`; read at `+0x200` on context enable | **canary experiment (E028)** |
| `0x0CE0` | O | `1` for a new kick | spec observation |
| `0x17A0` | O | `tpu_border_colour_table` (geometry and 3D) | spec observation |
| `0x6020` | P | `PERF_TA_CYCLE`: counts while the TA runs | public |

## Completion flush registers (spec, answer to Q14)

| Offset | Src | Fragment/transfer | Compute |
|---|---|---|---|
| `0x4000` | S | `1` | `1` |
| `0x0688` | S | `2`, poll | `4`, poll |
| `0x0668` | S | `3`, poll bit 0 | `9`, poll bit 0 |
| `0x1608` | S | `0x38001`, poll bit 0 | `0x10001`, poll bit 0 |
| `0x1480` / `0x1488` | S | `1` / `1` | — / `1` |
| `0x1740` `MCU_FENCE` | P | `0x180_00000000` (DM_PIXEL) | `0x280_00000000` (DM_COMPUTE) |
| `0x1720` / `0x1728` | S | `0x2B`, poll `0x1728` bit 0 | `0x33`, poll |
| `0x3818` `SLC_CTRL_FLUSH_INVAL` | P | `6` (DM_TA \| DM_PIXEL) | `8` (DM_COMPUTE) |
| `0x0388` | S | `0` in every TQ kick (role unknown) | |

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
