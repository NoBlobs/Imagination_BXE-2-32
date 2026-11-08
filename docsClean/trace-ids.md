# Firmware trace IDs

Each firmware trace entry is a **trace ID** (SFID) with parameters (format in
[hwperf-trace](hwperf-trace.md)). The IDs and their format strings are defined in a
**public upstream header**, `pvr_rogue_fwif_sf.h` (`drm/imagination`). The clean team
**can and should use that header** as the authoritative list of IDs and strings (it is
public ABI, not the firmware).

> **Provenance:** **P** (public header). Which events appear where: **O** (trace on the
> board). The firmware only **emits** the ID + parameters; the host decodes them to text
> from the header.

## The contract

- Each entry of the public table has a **numeric id**, a **group** (`ROGUE_FW_GROUP_*`,
  matching the `ROGUE_FWIF_LOG_TYPE_GROUP_*` bits used by `log_type` / `LOGTYPE_UPDATE`), a
  **parameter count** and a **format string**. The SFID word packs the three numbers (see
  [hwperf-trace](hwperf-trace.md)).
- The firmware writes `{SFID, timestamp, param0..paramN}` into the circular trace buffer.
  It does not write text.
- To reproduce the behaviour, the reimplementation must **emit the same IDs at the same
  points** (kicks, command headers, completions, power, HWR), with the same parameters.
  This is what makes its trace comparable with the original's (validation criterion).

## Key events (group / id from the public header; strings as decoded)

| Group | Id | Format string (abridged) | Where |
|---|---|---|---|
| MTS | 9 | `Client command header DM = …, client CCB = …, cmd = …` | dispatch of a cCCB command |
| MTS | 19 / 16 / 17 | `KCCB Slot …: Cmd=…, OSid=…` / `KCCB Slot …: DM=…, Cmd=…` / `KCCB Slot …: Return value …` | KCCB processing |
| MTS | 8 | `Ready-to-run debug …` | scheduling |
| MAIN | 14 | `cCCB Woff update = …, DM = …, FWCtx = …` | host updated a cCCB |
| MAIN | 16 / 22 | `UFO Checks for FWCtx … @ …` / `UFO Updates for FWCtx … @ …` | fences |
| MAIN | 171 / 172 | `Unknown Client CCB Command processing fences / executing kick: …` | unknown command type |
| MAIN | 86 / 88 | `Deferring DM… from running context …` / `No longer deferring DM… …` | DM deferral |
| MAIN | 226 | `Skipping already executed TA FWCtx … @ …` | geometry already run |
| MAIN | 119 / 8 | `Kick TA: …` / `TA finished` | geometry |
| RTD | 15 / 24 / 25, 33 | `TA RTData … loaded …` / `TE Region headers base …` / `TA Buffers: …` | geometry kick |
| RTD | 12 | `Loading stack-pointers for …` | 3D kick |
| BIF | 14 / 19 | `Activate MemCtx=…` / `Setup register set=…` | every kick |
| MAIN | 120 / 2 / 121 / 4 | `Kick 3D: …` / `3D finished, …` / `Kick 3D TQ: …` / `3D Transfer finished` | fragment / transfer |
| MAIN | 122 / 6 | `Kick Compute: …` / `Compute finished` | compute |
| MAIN | 31 | `GP USC triggered` | USC event |
| SPM | 7, 66, 32–38, 41, 44, 54 | `OOM …`, `OOM TA/3D PR Check …`, `SPM State = …`, `Wait for pending grow …`, `OOM: Resumed TA …`, `Received grow update …` | partial render / grow |
| PM | 11 / 12 / 7 / 28 | `PM pause/unpause TA ALLOC …` / `Freelist update completed …` / `3D Timeout Now …` | PM |
| PM | 30 / 31 | `PM running primary config (Core …)` / `PM running secondary config (Core …)` | PM core config |
| POW | 1, 2, 6, 7, 9, 18, 19, 21, 23, 59 | `Check Pow state …`, `GPU idle …`, `GPU ready to be powered down …`, `HW Request On(1)/Off(0) …`, `Changing number of dusts …`, `Cancel power off request …`, `OS requested forced IDLE …`, `Idle timer start …`, `Active PM latency set to …`, `OS requested pow off …` | power |
| MAIN | 144 / 145 / 69 / 93 / 176 | `GPU deinit` / `GPU units deinit` / `GPU Units init …` / `Core clock set to … Hz` / `Initialised Firmware with config flags …` | init / teardown |
| MISC | 2 / 3 / 4 | `GPIO read …` / `GPIO enabled` / `GPIO disabled` | GPIO |
| HWR | 13 / 14 / 28 / 32 / 61 / 62 / 24 / 25 | `HW poll … failed …` / `Discarded cmd on DM…` / `Analysis DM…: … Need to skip …` / `DM… ready for HWR` / `Begin/Finished hardware reset …` / `Recovery DM…: …` | recovery |
| MAIN | 27 | `HWR: Reset HW` | recovery |
| HWR | 8 / 26 | `Hold scheduling due lockup: …` / `DM…: Hold scheduling due to R-Flag = …` | scheduling held |
| CSW | 5, 49, 48, 3, 29 / 11, 9, 8 / 18, 34, 38 | CDM store/resume / 3D store/resume / `Higher priority context scheduled for DM …` | preemption |

(Full list and exact formats: the public header `pvr_rogue_fwif_sf.h`.)

## Log groups and `log_type`

- The `log_type` field of `rogue_fwif_tracebuf` (see [data-structures](data-structures.md))
  selects which **groups** are active, plus bit 0 (`ROGUE_FWIF_LOG_TYPE_TRACE`).
- The host turns groups on and off with KCCB `ROGUE_FWIF_KCCB_CMD_LOGTYPE_UPDATE`. Value
  seen: `0x4FFE` (groups) → `0x4FFF` with the trace bit.
- For performance, the firmware should **check the group** before writing an event (do not
  write when the group is off).
