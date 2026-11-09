# Kernel CCB (host → firmware)

The **Kernel CCB** (KCCB) is the command ring the **host sends to the firmware**. There is
one ring per OS (`osinit.kernel_ccb*`). The firmware processes the commands in the
**background task** (HW1), one at a time, with interrupts disabled.

## Structure

- Ring in shared memory + control block `rogue_fwif_ccb_ctl` (`write_offset +0x00`,
  `read_offset +0x80`, `wrap_mask +0x84`).
- Each command is a `rogue_fwif_kccb_cmd` of **0x58 bytes**: `cmd_type`, then a
  command-specific `cmd_data` union, then flags.
- `cmd_type` = `ROGUE_FWIF_KCCB_CMD_*`, i.e. `id | (ROGUE_CMD_MAGIC_DWORD << 16)` with
  `ROGUE_CMD_MAGIC_DWORD = 0x2ABC`.

## Processing

1. The host writes the command and advances `write_offset`, then wakes the firmware (MTS).
2. The BG task, while `read_offset != write_offset`:
   - reads the command at `read_offset`;
   - handles it according to `cmd_type`;
   - writes the **return value** into the return slot for that index
     (`osinit.kernel_ccb_rtn_slots_fw_addr`, one `u32` per slot). The value is a **bit
     mask** (**P**):
     - `ROGUE_FWIF_KCCB_RTN_SLOT_CMD_EXECUTED` (bit 0) — always set once the command was
       processed; the mainline host waits for this bit;
     - `ROGUE_FWIF_KCCB_RTN_SLOT_CLEANUP_BUSY` (bit 1) — set together with bit 0 when a
       cleanup could not be done because the resource is still in use (the host returns
       `-EBUSY` and retries later);
     - `ROGUE_FWIF_KCCB_RTN_SLOT_POLL_FAILURE` (bit 2) — a hardware poll failed while
       executing the command;
   - increments `osdata.kccb_cmds_executed`;
   - advances `read_offset`;
   - raises an IRQ to the host (after the write barrier).

## Command list (upstream numbering)

| id | `ROGUE_FWIF_KCCB_CMD_*` | Responsibility |
|---|---|---|
| 101 | `KICK` | submit work for a context (see below) |
| 102 | `MMUCACHE` | GPU MMU cache invalidate/flush |
| 103 | `BP` | configure a shader (USC) breakpoint |
| 105 | `SLCFLUSHINVAL` | SLC flush/invalidate |
| 106 | `CLEANUP` | clean up a resource (context, HWRTData, freelist, ZS buffer) |
| 107 | `POW` | power request (see [power-management](power-management.md)) |
| 108 | `ZSBUFFER_BACKING_UPDATE` | ZS buffer backing done |
| 109 | `ZSBUFFER_UNBACKING_UPDATE` | ZS buffer unbacking done |
| 110 | `FREELIST_GROW_UPDATE` | freelist grow done (see [partial-render-spm](partial-render-spm.md)) |
| 112 | `FREELISTS_RECONSTRUCTION_UPDATE` | freelist reconstruction done (after HWR) |
| 114 | `NOTIFY_WRITE_OFFSET_UPDATE` | the host added data to a circular buffer |
| 115 | `HEALTH_CHECK` | firmware health check |
| 116 | `FORCE_UPDATE` | force a blocked fence's checkpoints to errored |
| 117 | `COMBINED_GEOM_FRAG_KICK` | geometry + fragment kick in one command |
| 118 | `OS_ONLINE_STATE_CONFIGURE` | change an OS's online state |
| 200 | `REGCONFIG` | register configuration |
| 201 | `HWPERF_UPDATE_CONFIG` | configure HWPerf |
| 203 | `HWPERF_CTRL_BLKS` | enable/disable HWPerf blocks |
| 204 | `CORECLKSPEEDCHANGE` | new core frequency (updates timers/DVFS) |
| 206 | `LOGTYPE_UPDATE` | change the active trace groups |
| 207 | `PDVFS_LIMIT_MAX_FREQ` | PDVFS maximum frequency limit |
| 208 | `OSID_PRIORITY_CHANGE` | change an OS's priority |
| 209 | `STATEFLAGS_CTRL` | change state/config flags |
| 212 | `PDVFS_LIMIT_MIN_FREQ` | PDVFS minimum frequency limit |
| 213 | `PHR_CFG` | periodic hardware reset configuration |
| 215 | `WDG_CFG` | watchdog configuration |
| 216 | `COUNTER_DUMP` | performance counter dump |
| 217 | `HWPERF_CONFIG_ENABLE_BLKS` | HWPerf block enable configuration |
| 218 | `HWPERF_SELECT_CUSTOM_CNTRS` | select custom HWPerf counters |
| 220 | `HWPERF_CONFIG_BLKS` | HWPerf block configuration |

Authoritative list: `enum rogue_fwif_kccb_cmd_type` in `pvr_rogue_fwif.h`. A command the
firmware does not implement must still be consumed and marked `CMD_EXECUTED`, so the host
does not stall.

### Commands the mainline host sends (answers Q26)

The mainline driver (**P**, all of `drivers/gpu/drm/imagination/*.c`) only ever sends:
`KICK`, `COMBINED_GEOM_FRAG_KICK`, `MMUCACHE`, `CLEANUP`, `POW`, `FREELIST_GROW_UPDATE`,
`FREELISTS_RECONSTRUCTION_UPDATE`, `HEALTH_CHECK` and `LOGTYPE_UPDATE`. Every other command
in the table is never sent on this stack. For mainline compatibility it is enough to consume
those commands and answer `CMD_EXECUTED`.

What the public header says about the commands asked about in Q26:

| id | Command | Public description | On this GPU / stack |
|---|---|---|---|
| 114 | `NOTIFY_WRITE_OFFSET_UPDATE` | "the host has added more data to a **CDM2 circular buffer**" | concerns the CDM user-mode queue (`cdm_cb*` fields, feature `CDM_USER_MODE_QUEUE`), which this GPU does not have. It is **not** a cCCB write-offset notification. Not sent by mainline. |
| 118 | `OS_ONLINE_STATE_CONFIGURE` | a guest OS has come online / offline | virtualisation only; not sent by mainline |
| 103 | `BP` | configure a shader breakpoint | not sent by mainline |
| 200 | `REGCONFIG` | native/host-OS-only command | not sent by mainline |
| 213 | `PHR_CFG` | configure periodic hardware reset behaviour | not sent by mainline |
| 215 | `WDG_CFG` | configure the safety firmware watchdog | not sent by mainline |

Their full firmware-side behaviour is **not** described in this spec (`PENDING`). It is
not needed to interoperate with mainline.

### Values observed on the board (O)

`0x2abc0065` (KICK), `0x2abc0066` (MMUCACHE), `0x2abc006a` (CLEANUP), each answered in its
return slot — matching the upstream numbering.

## `KICK` command (id 101) in detail

Payload (`rogue_fwif_kccb_cmd_kick_data`): `context_fw_addr`, `client_woff_update`,
`client_wrap_mask_update`, `num_cleanup_ctl`, `cleanup_ctl_fw_addr[]`,
`work_est_cmd_header_offset`.

On a KICK the firmware:

1. for each of the `num_cleanup_ctl` cleanup control blocks (`rogue_fwif_cleanup_ctl`),
   **increments `submitted_commands`**;
2. **invalidates from the D-cache** the new bytes of the context's Client CCB (the range
   between the old and the new `write_offset`), so that it reads the commands the host
   just wrote;

   > **Coherency requirement:** the D-cache lines covering the new range must be
   > invalidated **by address** (hit-invalidate `CACHE` operations over every line of the
   > range), or the whole cache if the range is large. Without it the firmware can read
   > stale ring contents.
3. **updates the `write_offset`** of the context's Client CCB control block with
   `client_woff_update` (and `wrap_mask` with `client_wrap_mask_update`);
4. **checks the context** (see [client-ccb](client-ccb.md) and [scheduler](scheduler.md)):
   processes fences; if blocked, puts the context on the DM's *wait list*; if ready, on the
   *run list*, and marks the DM as pending for scheduling.

Guest addresses are rebased by `OSID × 16 MiB`.
