# Host ↔ firmware interface

The host↔firmware contract is a binary ABI in shared memory between the host driver and
the firmware, addressed by the firmware through the `0xC0xxxxxx` window. This page covers
the general contract. Layouts are in [data-structures](data-structures.md), and the
command rings in [kernel-ccb](kernel-ccb.md), [firmware-ccb](firmware-ccb.md) and
[client-ccb](client-ccb.md).

> The interface follows the upstream headers `pvr_rogue_fwif*.h` (`drm/imagination`).
> They are the mainline driver's public interface and the clean team may read them (see
> [clean-room-process](clean-room-process.md)).

## Contract entry points

- **`rogue_fwif_sysinit`**: per-system block at `0xC0FF0000`, written by the host at boot.
  Holds the fault page physical address, heap bases, firmware addresses of the control
  blocks (trace buffer, `sysdata`, GPU utilisation, HWPerf), the initial core clock, the
  APM latency, the firmware perf configuration, and the `firmware_started` flag.
- **`rogue_fwif_osinit`**: per-OS/guest block at `0xC0FE0000 + n·16 MiB`. Holds the
  firmware addresses of the Kernel CCB and its control block, the KCCB return slots, the
  Firmware CCB and its control block, the HWR info buffer, the `osdata` block, and the
  compatibility-check block (`rogue_comp_checks`).
- **`rogue_fwif_sysdata`** and **`rogue_fwif_osdata`**: live state written by the firmware
  and read by the host (config flags, power state, counters, fault/HWR info, power sync
  address).

## Boot handshake (mainline host)

1. The host builds memory (firmware sections, page tables, remap, BOOT_DATA), fills
   `sysinit` and OS 0's `osinit`, and sets `sysinit.firmware_started = false`.
2. The host releases the MIPS from reset.
3. The firmware initialises (see [execution-environment](execution-environment.md)),
   publishes its initial state in `sysdata`/`osdata` and **sets
   `sysinit.firmware_started = true`**.
4. The host polls `firmware_started` (timeout 5 s, **P**) and then starts submitting
   commands through the Kernel CCB.

If the firmware hits an internal page fault (invalid pointer), it diverts through the
fault page. It may report the event with the FWCCB `CONTEXT_FW_PF_NOTIFICATION`
command, which is not fatal by itself. A healthy boot records no internal fault.

## Structure size and offset checks

The sizes and offsets of every shared structure are fixed by the public ABI and listed in
the upstream check headers (**P**: `pvr_rogue_fwif_check.h`,
`pvr_rogue_fwif_client_check.h`, `pvr_rogue_fwif_shared_check.h`), which the mainline
driver enforces at compile time. Key values:

| Item | Value |
|---|---|
| `sizeof(rogue_fwif_sysinit)` | `0x220` |
| `offsetof(sysinit, fault_phys_addr)` | `0x0` |
| `offsetof(sysinit, trace_buf_ctl_fw_addr / fw_sys_data_fw_addr)` | `0x140 / 0x144` |
| `sizeof(rogue_fwif_osinit)` | `0x68` |
| `sizeof(rogue_fwif_fwcommoncontext)` | `0x98` |
| `offsetof(fwcommoncontext, fw_mem_context_fw_addr)` | `0x88` |
| `offsetof(fwcommoncontext, run_node)` | `0x78` |
| `sizeof(rogue_fwif_tracebuf)` | `0x360` |
| `sizeof(rogue_fwif_sysdata)` | `0xE28` |
| `offsetof(sysdata, hwr_state_flags)` | `0xDF8` |
| `sizeof(rogue_fwif_osdata)` | `0x248` |
| `sizeof(rogue_fwif_hwrinfobuf)` | `0x920` |
| `sizeof(rogue_fwif_cmd_geom / cmd_frag)` | `0x70 / 0x1E0` |
| `sizeof(rogue_fwif_cmd_transfer / cmd_compute)` | `0xC0 / 0x60` |
| `sizeof(rogue_fwif_freelist)` | `0x40` |
| `sizeof(rogue_fwif_hwrtdata)` | `0x180` |
| `offsetof(hwrtdata, state / freelists_fw_addr)` | `0xAC / 0xB0` |
| `sizeof(rogue_fwif_ccb_cmd_header)` | `0x28` |
| `sizeof(rogue_fwif_kccb_cmd)` | `0x58` |

A reimplementation must use **exactly these layouts**; the authoritative list is the check
headers.

> **Runtime alignment checks.** `sysinit.align_checks` can point to a runtime table of
> sizes/offsets. The mainline driver sets it to **0** (no table), so the firmware must
> accept `align_checks == 0` and skip that check. The driver initialises the BVNC fields
> of `osinit.rogue_comp_checks`; it does not depend on the firmware filling that block.

## Host ↔ firmware interrupts

- **Host → FW:** after writing a command into the Kernel CCB and updating `write_offset`,
  the host wakes the firmware through the MTS. The firmware is also woken by GPU hardware
  events (work completion), which arrive as HW2 (IRQ).
- **FW → host:** the firmware raises `ROGUE_CR_MIPS_WRAPPER_IRQ_STATUS` and increments
  `osdata.interrupt_count` after publishing updates — typically after signalling
  completion UFOs or queueing a Firmware CCB command. The host handles the IRQ and reads
  the CCBs and structures.

**Visibility order:** every publication of data to the host comes after the **write
barrier** described in [execution-environment](execution-environment.md). Implementing
the barrier is mandatory: without it, the host may see updated offsets before the data.

## Configuration flags

- The host passes `sysinit`/`sysdata` config flags (`config_flags`, `config_flags_ext`,
  bits `ROGUE_FWIF_INICFG_*` / `ROGUE_FWIF_INICFG_EXT_*`) and per-OS flags
  (`ROGUE_FWIF_INICFG_OS_*`). Among other things they control: DM overlap
  (`ROGUE_FWIF_INICFG_DISABLE_DM_OVERLAP`), context switch per DM
  (`ROGUE_FWIF_INICFG_OS_CTXSWITCH_{TDM,GEOM,FRAG,CDM}_EN`), MLIST checking, clock gating,
  assert on out-of-memory and assert on HWR trigger.
- The firmware copies the flags at boot into its own state. The KCCB `STATEFLAGS_CTRL`
  command changes the state flags at runtime; `sysdata.config_flags` mirrors them for the
  host.

## Time correlation

The firmware maintains the GPU utilisation block `rogue_fwif_gpu_util_fwcb`, with
`time_corr[]` entries (`rogue_fwif_time_corr`: `os_timestamp`, `os_mono_timestamp`,
`cr_timestamp`, `cr_delta_to_os_delta_kns`, `core_clock_speed`), a sequence counter and
GPU statistics. The host uses it to convert GPU timestamps into host time. See
[data-structures](data-structures.md).
