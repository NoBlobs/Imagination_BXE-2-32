# Hardware recovery (HWR)

HWR (*hardware recovery*) detects a hung or faulting GPU and recovers it without bringing
down the firmware or the host.

> **Provenance.** Flags, enums, the HWR log layout and the cleanup contract: **P**
> (`pvr_rogue_fwif.h`, `pvr_fw.c`, `pvr_ccb.c`). Recovery flow and timing: **O** (trace) +
> **D**.

## Detection

- **MMU fault / page fault:** the MMU page-fault event in `ROGUE_CR_EVENT_STATUS` (or a
  diversion through the fault page, see [memory-model](memory-model.md)) indicates an
  invalid access by a job. The IRQ task sees the event.
- **Failed hardware poll:** the firmware polls GPU registers (e.g. waiting for a bit in
  `ROGUE_CR_EVENT_STATUS` after a kick). If a poll fails (unexpected value or timeout), the
  firmware records the poll address/mask/count in `rogue_fwif_sysdata.cr_poll_addr`,
  `cr_poll_mask`, `cr_poll_count` and triggers HWR (HWR type `ROGUE_HWRTYPE_POLLFAILURE`).
  Trace HWR id 13 `HW poll … failed (reg:… val:…)`.
- **Watchdogs:** the BG task checks the deadlines armed at each kick (see the per-DM
  execution state in [scheduler](scheduler.md)). A DM that does not finish in time is a
  lockup (or an overrun, `ROGUE_HWRTYPE_OVERRUN`).

## Recovery flow

```
detect a fault (MMU fault / poll / watchdog)
   └─ rogue_fwif_sysdata.hwr_state_flags: clear HARDWARE_OK, set RESET_IN_PROGRESS
      (+ GENERAL_LOCKUP / DM_STALLING / FW_FAULT as applicable)
   └─ hold scheduling (trace HWR id 8 "Hold scheduling due lockup …")
   └─ for each DM: "DM… ready for HWR" (HWR id 32)
   └─ identify the culprit: "Analysis DM…: Lockup FWCtx … Need to skip to next command" (HWR id 28)
   └─ "Begin hardware reset (HWR Counter=…)" (HWR id 61):
        reset the GPU core (ROGUE_CR_SOFT_RESET), flush/invalidate the SLC
   └─ "HWR: Reset HW" (MAIN id 27), "GPU Units init" (MAIN id 69)
   └─ "Finished hardware reset (HWR Counter=…)" (HWR id 62)
   └─ per-DM recovery:
        - discard the failed command ("Discarded cmd on DM… FWCtx=…", HWR id 14)
        - move the context to the next safe command ("Recovery DM…: FWCtx … skipped to
          command @ …", HWR id 24)
        - update hwr_recovery_flags[DM] and the HWR counters
        - "Recovery DM…: DM fully recovered" (HWR id 25)
   └─ ask the host for FREELISTS_RECONSTRUCTION if needed (see partial-render-spm)
   └─ hwr_state_flags = HARDWARE_OK (| DM_RUNNING_OK); reschedule
```

`hwr_state_flags` bits (public): `ROGUE_FWIF_HWR_HARDWARE_OK` bit 0,
`RESET_IN_PROGRESS` bit 1, `GENERAL_LOCKUP` bit 3, `DM_RUNNING_OK` bit 4, `DM_STALLING`
bit 5, `FW_FAULT` bit 6, `RESTART_REQUESTED` bit 7.

Observed: from lockup to recovery, about 0.43 ms. See
[observed-sequences](observed-sequences.md).

## Reset sequence (answers the clean team's Q11)

The complete ordered sequence run from the IRQ task once a lockup/fault is confirmed
(**D**/**O**):

1. **Stop the DMs** and hold scheduling (HWR state `RESET_IN_PROGRESS`).
2. **Wait for the SLC to go idle** — poll `ROGUE_CR_SLC_STATUS0` (bit 2
   `FLUSH_INVAL_PENDING` clear), after a flush/invalidate via `ROGUE_CR_SLC_CTRL_FLUSH_INVAL`.
3. **Disable partial renders** (`ROGUE_CR_PM_PARTIAL_RENDER_ENABLE = 0`).
4. **Pulse `ROGUE_CR_SOFT_RESET`** through the **XPU broadcast** bank, then release it:
   - assert: low word (`0x0100`) = `0x13FFFC1D`, high word (`0x0104`) = `0xFFEFFE00`;
   - release: both words = 0.

   These bits reset the GPU data-master/pipeline units **but not the MIPS core or the MTS
   (Garten) wrapper**, so the firmware keeps running. With `LAYOUT_MARS` present the MIPS
   core sits in its own (MARS) power/reset domain, consistent with these bits not touching
   it. The firmware does **not** use `ROGUE_CR_SOFT_RESET2` in recovery. (The larger
   "assert-and-hold" value `0x13FFFC1D`/`0xFFEFFE00` differs from the full
   power-on assert `0xF3FFFC1D`/`0xFFEFFFF8`, which is **not** used by HWR.)
5. **Re-initialise the GPU units** — re-run the whole GPU-units init (exec bases, SLC,
   unit defaults, event routing), i.e. section A of [boot-gpu-init](boot-gpu-init.md), and
   **rebuild the BIF page-catalogue slots** (`ROGUE_CR_BIF_CAT_BASE1..7`).
6. **Per-DM recovery:** skip/retire the guilty command, clear the DM lockup flags and its
   events (`ROGUE_CR_EVENT_CLEAR`), request freelist reconstruction if needed.
7. **Resume scheduling** — set `hwr_state_flags = HARDWARE_OK` and **kick the MTS**
   (`ROGUE_CR_MTS_SCHEDULE`).

So the clean team's draft order ("soft reset → SLC flush/invalidate → GPU units init →
re-enable MTS routing") is close but incomplete: the SLC wait and **disable partial
renders** come **before** the reset; the reset is a **pulse** (not assert-and-hold); after
it, **re-init the units and rebuild the BIF catalogue** and then do the **per-DM
skip/retire** before kicking the MTS. The MTS per-DM event routing is re-established as part
of the units re-init (section B of boot-gpu-init).

## HWR log (`rogue_fwif_hwrinfobuf`)

Each recovery is logged in a `rogue_hwrinfo` entry (see
[data-structures](data-structures.md)): fault data (union of BIF info — request status,
MMU status, page-catalogue address, which identifies the context —, MMU info, poll info,
MIPS TLB info), `cr_timer`/`os_timer`, `frame_num`, `pid`, `active_hwrt_data`,
`hwr_number`, `event_status` at the time, `hwr_recovery_flags`, **`hwr_type`**
(`enum rogue_hwrtype`, e.g. `ROGUE_HWRTYPE_BIF0FAULT = 3`, `ROGUE_HWRTYPE_MIPSTLBFAULT = 8`),
`dm`, `core_id`, and the `ROGUE_CR_TIMER` ticks of kick / reset start / reset finish /
freelist ready.

The buffer holds `ROGUE_FWIF_HWINFO_MAX` (16) entries: the first 8 are never overwritten,
the last 8 are a ring. Counters: `hwr_counter`, `write_index`,
`hwr_dm_locked_up_count[DM]`, `hwr_dm_overran_count[DM]`, `hwr_dm_recovered_count[DM]`, …

## Cleanup (KCCB `ROGUE_FWIF_KCCB_CMD_CLEANUP`)

The host asks for resources to be released with the KCCB `CLEANUP` command
(`ROGUE_FWIF_CLEANUP_FWCOMMONCONTEXT`, `_HWRTDATA`, `_FREELIST`, `_ZSBUFFER`). The firmware
may only clean an **idle** resource.

**Return contract (ABI, critical):** the return slot is a **bit mask**.

| Situation | Return slot |
|---|---|
| resource cleaned now **or already clean** | `ROGUE_FWIF_KCCB_RTN_SLOT_CMD_EXECUTED` (`0x1`) |
| resource still in use (pending submitted commands, DM running) | `CMD_EXECUTED \| ROGUE_FWIF_KCCB_RTN_SLOT_CLEANUP_BUSY` (`0x3`) — the host returns `-EBUSY` and retries |

The host waits for `CMD_EXECUTED` and then tests the `CLEANUP_BUSY` bit. Every path must
produce one of these results explicitly. Answering "busy" for a resource that is already
clean makes the host retry until it gives up, and the resource stays stuck.

## Context robustness (Vulkan device loss)

Contexts have `rogue_fwif_fwcommoncontext.robustness_address`. On a reset that affects a
context, the firmware can notify the host through FWCCB
`ROGUE_FWIF_FWCCB_CMD_CONTEXT_RESET_NOTIFICATION`, which reports device loss to the Vulkan
application.

## Interaction with power management

HWR and power management share state:

- during HWR **no** new work is scheduled (overlap rules, see [scheduler](scheduler.md));
- the power teardown walks the same BIF slot state that HWR resets (see
  [power-management](power-management.md)). Both paths must leave that state consistent;
- a power transition must not start while a recovery is in progress.
