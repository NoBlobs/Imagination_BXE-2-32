<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# Host interface as implemented

This document covers what the firmware does on its side of the contract with the
mainline `powervr` driver. The contract itself is defined by the public
`pvr_rogue_fwif*.h` headers and by `docsClean/host-interface.md`. Every structure in
`src/include/fwif.h` is checked at build time with `static_assert` against the
offsets in the public `pvr_rogue_fwif_check.h`.

## 1. The `.fw` container (`tools/mkfw.py`)

```
offset 0                               file end − 4 KiB         file end
│ firmware ELF │ zero padding │ device info │ header (48 B) │ layout table │ 0… │
                                           └──────── last 4 KiB block ─────────┘
```

- **Header** (`pvr_fw_info.h`): `info_version = 3`, `header_len = 48`,
  `layout_entry_num = 6`, `layout_entry_size = 24`, `bvnc = 36.29.52.182`,
  `fw_info_size = 4096`, `flags = OPEN_SOURCE`, version `1.0`, build `1`,
  `device_info_size`. The kernel prints it as `FW version v1.0 (build 1 OS)`, which is
  how the test scripts tell this build apart from any other firmware file.
- **Layout table**: 6 entries `(id, type, base_addr, max_size, alloc_size, alloc_offset)`,
  see [boot-and-memory.md](boot-and-memory.md#host-objects-and-the-layout-table).
  `mkfw.py` checks that every ELF `PT_LOAD` segment falls inside one entry.
- **Device info**: header `(brn_mask_size=1, ern_mask_size=1, feature_mask_size,
  feature_param_size)`, then the BRN mask, the ERN mask, the feature masks, and the
  values of the value features in feature order. Bit order: `pvr_rogue_fwif_dev_info.h`.

The host builds its whole feature and quirk view of the GPU from the device info, so
these bits change host behaviour:

| Item | Value | Why |
|---|---|---|
| BRN | 63553 | 36-bit MIPS bus workaround |
| ERN | 35421, 38748, 42290, 42606, 47025, 57596 | |
| `SYS_BUS_SECURE_RESET` | set | **required**. Without it the host never clears `ROGUE_CR_SYS_BUS_SECURE` and the MIPS cannot fetch anything ([E001](logRefs/E001-sys-bus-secure-reset.md)). |
| `SLC_SIZE_IN_KILOBYTES` | 16 | the host now leaves DM overlap enabled (config flags `0x0`) |
| `LAYOUT_MARS` | 1 | host `pvr_fw_stop()` checks `MARS_IDLE` instead of SIDEKICK/SLC idle |
| `XE_MEMORY_HIERARCHY`, `XE_TPU2` | set | host sizes the fragment context state differently and resets with `SOFT_RESET2` |
| value features | `CDM_CONTROL_STREAM_FORMAT 1`, `COMMON_STORE 13312`, `FBCDC 50`, `FBCDC_ALGORITHM 50`, `FBCDC_ARCHITECTURE 7`, `NUM_OSIDS 8`, `NUM_ISP_IPP_PIPES 2`, `SLC_BANKS 1`, `VA bits 40`, `PHYS_BUS_WIDTH 36`, `XE_ARCHITECTURE 1`, `XPU_MAX_REGBANKS_ADDR_WIDTH 19`, `XPU_MAX_SLAVES 3`, `XPU_REGISTER_BROADCAST 1`, … | |
| boolean features | `AXI_ACELITE`, `COMPUTE`, `COMPUTE_OVERLAP`, `COREID_PER_OS`, `GPU_MULTICORE_SUPPORT`, `GPU_VIRTUALISATION`, `GS_RTA_SUPPORT`, `IRQ_PER_OS`, `MIPS`, `PBE2_IN_XE`, `PBVNC_COREID_REG`, `PERFBUS`, `ROGUEXE`, `SIMPLE_INTERNAL_PARAMETER_FORMAT(_V2)`, `SLC_SIZE_CONFIGURABLE`, `SOC_TIMER` | |

These are the values the spec documents for the open firmware image of this BVNC
(`docsClean/memory-model.md`, the answer to Q2).

## 2. Start-up handshake

1. The host loads the sections, fills BOOT_DATA, sysinit and osinit, sets
   `firmware_started = 0` and releases the MIPS.
2. `fw_main` finds every shared object through `sysinit` (`0xC0FF0000`) and `osinit`
   (`0xC0FE0000`):
   `sysdata`, `runtime_cfg`, trace buffer control → from sysinit;
   `osdata`, KCCB and its control block, KCCB return slots, FWCCB and its control
   block → from osinit.
3. It reads the core clock from `runtime_cfg.core_clock_speed`, falling back to
   `sysinit.initial_core_clock_speed` and then to 307.2 MHz.
   - `CR_TIMER` ticks are clock / 256.
   - CP0 Count runs at clock / 2.
4. It sets `sysdata.config_flags` and `hwr_state_flags = HARDWARE_OK | DM_RUNNING_OK`.
5. MTS set-up, GPU unit init and scheduler init follow, then `pow_state = IDLE`.
6. Traces: `POW_23` and `MAIN_176` ("Initialised Firmware with config flags…").
7. It writes `firmware_started_timestamp` and, after a barrier, `firmware_started = 1`.
   The host is polling this word.
8. It arms the timer, enables HW0–HW2, processes anything already in the KCCB, and
   enters `WAIT`.

The kernel then prints `[drm] Initialized powervr 1.0.0 for cac00000.gpu`.

## 3. Kernel CCB (`kccb.c`)

The KCCB is an array of 88-byte `rogue_fwif_kccb_cmd` entries with a control block
`(write_offset, read_offset, wrap_mask)`. On every BG task and every timer tick,
`kccb_process()` works like this:

```
while read_offset != write_offset:
    idx  = read_offset & wrap_mask
    rtn  = handle(kccb[idx])            # always includes CMD_EXECUTED (bit 0)
    rtn_slots[idx] = rtn
    osdata.kccb_cmds_executed++
    barrier; read_offset = idx + 1 & wrap_mask
notify_host() if anything was processed
```

The host waits on the **return slot**: `ROGUE_FWIF_KCCB_RTN_SLOT_CMD_EXECUTED` (bit 0),
plus `CLEANUP_BUSY` (bit 1) when a cleanup cannot be done yet. Commands without the
magic value `0x2ABCxxxx` in their type are traced as `MAIN_150` and still acknowledged.

| Command | Handling |
|---|---|
| `KICK` (101) | `sched_kick()`: see [scheduler-and-kicks.md](scheduler-and-kicks.md) |
| `COMBINED_GEOM_FRAG_KICK` (117) | two `sched_kick()` calls |
| `MMUCACHE` (102) | SLC flush and invalidate ([E015](logRefs/E015-slc-flush-before-mmu-inval.md)), BIF invalidation, PM TLB, MIPS TLB flush, then the sync value is written |
| `CLEANUP` (106) | `sched_cleanup()` → 0 or `CLEANUP_BUSY` |
| `POW` (107) | `power.c` |
| `FORCE_UPDATE` (116) | marks the context's sync checkpoints as errored at the fence and re-runs the context |
| `CORECLKSPEEDCHANGE` (204) | reloads the clock from `runtime_cfg`, trace `MAIN_93` |
| `STATEFLAGS_CTRL` (209) | set, OR or clear `config_flags`, mirrored to `sysdata` |
| `LOGTYPE_UPDATE` (206) | nothing to do: `log_type` is read live from the trace control |
| `HEALTH_CHECK`, `SLCFLUSHINVAL`, `BP`, freelist/ZS-buffer updates, `REGCONFIG`, HWPerf, PDVFS, `PHR_CFG`, `WDG_CFG`, … | acknowledged, no action yet |

### Host notification

`notify_host()` makes all writes visible with `write_barrier()`. It then increments
`osdata.interrupt_count[0]`, barriers again, and writes `ROGUE_CR_MIPS_WRAPPER_IRQ_STATUS
= 1`, which raises the GPU interrupt line (IRQ 75). The host's handler compares
`interrupt_count` with its own copy to decide whether the interrupt is its own.

`write_barrier()` performs these steps in order:
1. Write the uncached `BD_BARRIER` word.
2. `EHB` and `SYNC`.
3. Read the word back.
4. Read `CORE_REVISION`.

All memory is uncached, but writes still need to drain from the MIPS write buffer and
the wrapper before the host reads them.

## 4. Firmware CCB (`fwccb_send`)

FWCCB is the firmware-to-host ring of 0x58-byte commands. `fwccb_send()` waits while
the ring is full. It notifies the host and spins in 10 µs steps, emitting the trace
`MAIN_90` once. It then copies the command, publishes `write_offset` and notifies.
Nothing sends FWCCB commands yet. Freelist grow, context reset and the like will need
them later.

## 5. Power (`power.c`)

The host runtime-suspends the GPU after the autosuspend delay (`active_pm_latency_ms`)
when `sysdata.pow_state == IDLE` and the KCCB is empty. The handshake uses the
**power-sync word** (`osdata.power_sync_fw_addr`): the host writes 0, sends
`KCCB_POW` and polls for a non-zero value.

| Request | Firmware action |
|---|---|
| `FORCED_IDLE_REQ` (force/timeout) | stop dispatching new tasks (`g.forced_idle`), `pow_state = FORCED_IDLE`, sync = 1 |
| `FORCED_IDLE_REQ` (cancel) | resume, `pow_state = IDLE`, sync = 1 |
| `OFF_REQ` | traces `POW_59`, `MAIN_145`/`144`; flush the dynamic TLB; `pow_state = OFF`; mask the timer interrupt; set `g.powered_off`; sync = 1 |
| `NUM_UNITS_CHANGE`, `APM_LATENCY_CHANGE` | sync = 1 |

The scheduler keeps `pow_state` current: **ON** while any DM is busy or any context
has pending commands, otherwise **IDLE** (`update_pow_state()`).

### Warm boot

After `OFF`, the host turns the power domain off (APMU, see
[board-and-kernel.md](board-and-kernel.md)). When work arrives, it turns the domain
back on, reloads nothing, rewrites BOOT_DATA and restarts the MIPS at the reset
vector. The firmware runs the **full** boot path again: .bss is cleared and all state
is rebuilt from sysinit and osinit. Contexts created before the power-off reappear
through new KICK commands. This cycle (`IDLE → FORCED_IDLE → OFF → domain off →
cold reset → firmware_started`) is verified on the board, see
[E005](logRefs/E005-runtime-pm-cycle.md).

## 6. Trace buffer (`util.c`)

`sysinit.trace_buf_ctl_fw_addr` → `rogue_fwif_tracebuf`: `log_type` and, per thread,
`trace_pointer` and `trace_buffer_fw_addr`. The size is
`tracebuf_size_in_dwords`. Only thread 0 is used.

Entry format (read by `pvr_fw_trace.c`, shown in
`/sys/kernel/debug/dri/N/pvr_fw/trace_0`):

```
word 0   SFID  = 0x70000000 | nparams << 16 | group << 12 | id
word 1   timestamp high (ROGUE_CR_TIMER, 48 bits)
word 2   timestamp low
word 3…  parameters (32-bit each)
```

The ring wraps word by word, and the host reads it modulo its size. An entry is
written only when `log_type` bit 0 (trace enabled) is set **and** the group's
`ROGUE_FWIF_LOG_TYPE_GROUP_*` bit is set. The group number in the SFID and the
`log_type` bit are not the same number. The map is:

| group | 0 NULL | 1 MAIN | 2 CLEANUP | 3 CSW | 4 PM | 5 RTD | 6 SPM | 7 MTS | 8 BIF | 9 MISC | 10 POW | 11 HWR | 12 HWP | 13 RPM | 14 DMA | 15 DBG |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| bit | – | 1 | 3 | 4 | 6 | 7 | 8 | 2 | 5 | 14 | 9 | 10 | 11 | 12 | 13 | 31 |

To enable everything from the host:
`echo 0xffffffff > /sys/kernel/debug/dri/N/pvr_fw/trace_mask`.

Trace points currently emitted:

| SFID | When |
|---|---|
| `DBG_4` | MTS register values before init; `0xDEAD00cc, EPC, BadVAddr` on a fatal exception |
| `POW_23`, `MAIN_176` | end of init |
| `MTS_19` / `MTS_17` | KCCB command picked / return value |
| `MAIN_14` | KICK: new write offset, DM, context |
| `MTS_9` | task dispatched to a DM |
| `BIF_14`, `BIF_19` | memory context bound to a BIF slot |
| `MAIN_122` | compute kick |
| `MAIN_6` | compute finished |
| `MAIN_22` | UFO update |
| `MAIN_171` | bad cCCB command |
| `MAIN_172` | task type not implemented (retired at once) |
| `MAIN_93` | clock change |
| `POW_19`, `POW_59`, `POW_6`, `MAIN_145`, `MAIN_144` | power transitions |
| `HWR_13` | `cr_poll32` timeout (address, last value). Also recorded in `sysdata.cr_poll_*`. |
| `MAIN_90` | FWCCB full |
