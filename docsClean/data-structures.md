# Shared data structures

Layouts of the main shared structures. Names are those of the public upstream headers
(`pvr_rogue_fwif.h`, `pvr_rogue_fwif_shared.h`, `pvr_rogue_fwif_client.h`); offsets are
in bytes from the start of the structure and match the public check headers
(`pvr_rogue_fwif*_check.h`), which are authoritative. Global sizes are in
[host-interface](host-interface.md).

## `rogue_fwif_fwcommoncontext` (0x98 B)

Common part of a firmware context (geometry, 3D, compute, transfer).

| Offset | Field | Description |
|---|---|---|
| `+0x00` | `ccbctl_fw_addr` | firmware address of the Client CCB control block |
| `+0x04` | `ccb_fw_addr` | firmware address of the Client CCB buffer |
| `+0x1C` | `fw_com_ctx_flags` | context flags |
| `+0x20` | `priority` | priority (orders the run list) |
| `+0x24` | `priority_seq_num` | priority sequence number |
| … | `stats_*` | statistics (stores, OOM events, partial renders) |
| `+0x3C` | `dm` | data master of this context (`PVR_FWIF_DM_*`) |
| `+0x60` | `robustness_address` | robustness address (Vulkan device loss) |
| `+0x68` | `max_deadline_ms` | maximum deadline |
| `+0x70` | `waiting_node` | node of the DM's *wait list* (`rogue_fwif_dllist_node` `{p, n}`) |
| `+0x78` | `run_node` | node of the DM's *run list* |
| `+0x80` | `last_failed_ufo` | last UFO that failed (cheap re-evaluation) |
| `+0x88` | `fw_mem_context_fw_addr` | firmware address of the memory context (BIF) |
| `+0x8C` | `server_common_context_id` | context ID |
| `+0x90` | `pid` | owning process PID |

> The Client CCB base and control-block addresses are obtained from `ccb_fw_addr` /
> `ccbctl_fw_addr` with the window mask applied (`& 0xC0FFFFFC`, see
> [memory-model](memory-model.md)). The current command sits at base + `read_offset` of
> the control block (see [client-ccb](client-ccb.md)).

## `rogue_fwif_fwmemcontext`

Memory context (one GPU MMU / page catalogue).

| Field | Description |
|---|---|
| `pc_dev_paddr` (u64) | physical address of the page catalogue |
| `page_cat_base_reg_set` | which `ROGUE_CR_BIF_CAT_BASE` slot this context occupies |
| `breakpoint_addr` | shader breakpoint address |
| `bp_handler_addr` | breakpoint handler |
| `breakpoint_ctl` | breakpoint control |
| `fw_mem_ctx_flags` | flags |

## `rogue_fwif_sysdata` (0xE28 B) — per-system live state

| Offset | Field | Description |
|---|---|---|
| `+0x00` | `config_flags` | config flags (mirrors `STATEFLAGS_CTRL`) |
| `+0x04` | `config_flags_ext` | extended config flags |
| `+0x08` | `pow_state` | power state (`ROGUE_FWIF_POW_OFF/ON/FORCED_IDLE/IDLE` = 0/1/2/3) |
| `+0x0C`… | `hw_perf_*` | HWPerf buffer indices and counters |
| `+0x2C` | `os_runtime_flags_mirror[]` | per-OS runtime flags (`os_state` bits 2:0, `fl_ok` bit 3, `fl_grow_pending` bit 4, `isolated_os` bit 5) |
| `+0x50` | `fault_info[8]` | firmware fault records |
| `+0xDD0` | `fw_faults` | firmware fault counter |
| … | `cr_poll_addr/mask/count[2]` | address/mask/count of the last hardware poll (per thread) |
| `+0xDF0` | `start_idle_time` | idle start time (for APM) |
| `+0xDF8` | `hwr_state_flags` | `ROGUE_FWIF_HWR_*` bits: `HARDWARE_OK` (bit 0), `RESET_IN_PROGRESS` (bit 1), `GENERAL_LOCKUP` (bit 3), `DM_RUNNING_OK` (bit 4), `DM_STALLING` (bit 5), `FW_FAULT` (bit 6), `RESTART_REQUESTED` (bit 7) |
| … | `hwr_recovery_flags[DM]` | per-DM recovery flags |

## `rogue_fwif_osdata` (0x248 B) — per-OS live state

| Offset | Field | Description |
|---|---|---|
| `+0x228` | `last_forced_update_time` | time of the last forced update |
| `+0x230` | `interrupt_count[2]` | interrupt counter (incremented on every published completion) |
| `+0x238` | `kccb_cmds_executed` | Kernel CCB commands executed |
| `+0x23C` | `power_sync_fw_addr` | firmware **address** of the power-sync word; the firmware writes `1` there when a power request completes (see [power-management](power-management.md)) |

## `rogue_fwif_sysinit` (0x220 B) — per-system boot config

Key fields (see the public header for the full list):

| Offset | Field |
|---|---|
| `+0x00` | `fault_phys_addr` — physical address of the fault page |
| `+0x08` | `pds_exec_base` — PDS heap base |
| `+0x10` | `usc_exec_base` — USC heap base |
| `+0x140` | `trace_buf_ctl_fw_addr` — trace buffer control block |
| `+0x144` | `fw_sys_data_fw_addr` — `sysdata` |
| … | `gpu_util_fw_cb_ctl_fw_addr`, `hwperf_ctl_fw_addr`, `align_checks`, `initial_core_clock_speed`, `active_pm_latency_ms`, `firmware_started`, `firmware_started_timestamp`, `firmware_perf`, `core_clock_rate_fw_addr` |

## `rogue_fwif_osinit` (0x68 B) — per-OS boot config

Firmware addresses of: the Kernel CCB control block and buffer
(`kernel_ccbctl_fw_addr`, `kernel_ccb_fw_addr`), the KCCB return slots
(`kernel_ccb_rtn_slots_fw_addr`), the Firmware CCB and its control block
(`firmware_ccbctl_fw_addr`, `firmware_ccb_fw_addr`), the HWR info buffer, the `osdata`
block (`fw_os_data_fw_addr`), plus `hwr_debug_dump_limit` and the compatibility-check block
`rogue_comp_checks`.

## CCB control blocks

### `rogue_fwif_ccb_ctl` (KCCB/FWCCB)

| Field | Offset |
|---|---|
| `write_offset` | `+0x00` |
| `read_offset` | `+0x80` (separate cache line) |
| `wrap_mask` | `+0x84` |
| `cmd_size` | `+0x88` |

### `rogue_fwif_cccb_ctl` (Client CCB)

`{ write_offset, read_offset, dep_offset, wrap_mask, read_offset2, read_offset3,
read_offset4 }`. `dep_offset` separates commands whose dependencies are resolved (ready)
from those still blocked by a fence; the read offsets advance as commands are consumed.
See [client-ccb](client-ccb.md).

## `rogue_fwif_tracebuf` (0x360 B)

Firmware trace buffer control: `log_type` (active log groups + trace bit), one
`rogue_fwif_tracebuf_space` per thread (`trace_pointer` = write index,
`trace_buffer_fw_addr`, assert info), `tracebuf_size_in_dwords`, `tracebuf_flags`. See
[hwperf-trace](hwperf-trace.md).

## `rogue_fwif_hwrinfobuf` (0x920 B)

Hardware recovery history. **16** `hwr_info` entries (`ROGUE_FWIF_HWINFO_MAX_FIRST` 8 +
`ROGUE_FWIF_HWINFO_MAX_LAST` 8). Per entry: BIF request status, MMU status, page catalogue
(identifies the context), PID, HWRTData, number, `EVENT_STATUS` at the time, recovery
flags, **fault type** (`ROGUE_HWRTYPE_*`, e.g. `BIF0FAULT` = 3), DM. `+0x880`
`hwr_counter`, then `write_index`, and per-DM counters `hwr_dm_locked_up_count`,
`hwr_dm_overran_count`, `hwr_dm_recovered_count`, `hwr_dm_false_detect_count`. Times in
`ROGUE_CR_TIMER` ticks. See [hardware-recovery](hardware-recovery.md).

## `rogue_fwif_gpu_util_fwcb` — time correlation and utilisation

`time_corr[256]` (`rogue_fwif_time_corr`, 0x28 B each) at `+0x0`; `time_corr_seq_count` at
`+0x2800`; `gpu_util_flags` at `+0x2804`; `last_word` at `+0x2808`; `stats_counters` at
`+0x2810`.

## Kick commands (work payloads)

Structures the host builds in the Client CCB, read by the firmware on dispatch:

- `rogue_fwif_cmd_geom` (geometry, 0x70 B), `rogue_fwif_cmd_frag` (fragment, 0x1E0 B),
  `rogue_fwif_cmd_transfer` (0xC0 B), `rogue_fwif_cmd_compute` (0x60 B).
- Each is preceded by a **command header** `rogue_fwif_ccb_cmd_header` (**0x28 B**:
  `cmd_type`, `cmd_size`, `ext_job_ref`, `int_job_ref`, `work_est_kick_data`). See
  [command-structs](command-structs.md) and [data-masters](data-masters.md).

## Freelist and render target

- `rogue_fwif_freelist` (0x40 B): Parameter Buffer (PB) pages for geometry
  (`freelist_dev_addr`, `current_dev_addr`, `current_stack_top`, `max_pages`,
  `grow_pages`, `current_pages`, `allocated_page_count`, `allocated_mmu_page_count`,
  `freelist_id`, `grow_pending`, `ready_pages`, …).
- `rogue_fwif_hwrtdata` (0x180 B): hardware render-target data. `+0xAC` `state`
  (`ROGUE_FWIF_RTDATA_STATE_*`), `+0xB0` `freelists_fw_addr[]`. A geometry/fragment pair
  shares one HWRTData.
