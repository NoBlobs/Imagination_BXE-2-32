# Firmware trace and HWPerf

The firmware offers two observability channels: a **circular trace** (log events for
debugging) and **HWPerf** (binary performance packets). Both go to shared-memory buffers
that the host reads. With the mainline driver, **only the trace is consumed**: HWPerf is
optional for a mainline-compatible reimplementation.

> **Provenance.** Trace layout, groups, entry format and the host side: **P**
> (`pvr_rogue_fwif.h`, `pvr_rogue_fwif_sf.h`, `pvr_fw_trace.c`). Values seen at run time:
> **O**.

## Firmware trace (`rogue_fwif_tracebuf`)

- Control structure `rogue_fwif_tracebuf`: `log_type`, one `rogue_fwif_tracebuf_space`
  per thread (`trace_pointer`, `trace_buffer_fw_addr`, `assert_buf`),
  `tracebuf_size_in_dwords`, `tracebuf_flags`. The ring is 12000 dwords.
- `log_type` selects the active **log groups** (`ROGUE_FWIF_LOG_TYPE_GROUP_*`, mask
  `ROGUE_FWIF_LOG_TYPE_GROUP_MASK = 0x80007FFE`) plus bit 0 `ROGUE_FWIF_LOG_TYPE_TRACE`
  (trace on). The mainline host always sets bit 0 when any group is enabled. Value seen:
  `0x4FFE` (groups) / `0x4FFF` (groups + trace).
- The host changes the groups with KCCB `ROGUE_FWIF_KCCB_CMD_LOGTYPE_UPDATE` (after writing
  `log_type`) and reads the ring (mainline debugfs `trace_0`, mask in `trace_mask`, initial
  mask from the `init_fw_trace_mask` module parameter).

### Entry format

Each entry is: one **SFID** word `0x70000000 | (n_params << 16) | (group << 12) | id`, a
64-bit timestamp, then `n_params` 32-bit parameters. An entry is valid if
`(word & 0xFFF00000) == 0x70000000`. The host decodes the SFID against the public table of
format strings (`pvr_rogue_fwif_sf.h`), so **the reimplementation must emit the SFIDs and
parameter counts exactly as in that table** for the host to print them. Group and id
numbers used in these documents are listed in [trace-ids](trace-ids.md).

### Useful events for validation

| Event (decoded text) | Group / id | Meaning |
|---|---|---|
| `Kick TA` / `Kick 3D` / `Kick 3D TQ` / `Kick Compute` | MAIN 119 / 120 / 121 / 122 | a data master was started |
| `Client command header DM = …, client CCB = …, cmd = …` | MTS 9 | dispatch of a cCCB command |
| `cCCB Woff update = …, DM = …, FWCtx = …` | MAIN 14 | the host updated a cCCB write offset |
| `KCCB Slot …: Cmd=0x…, OSid=…` / `Return value …` | MTS 19 / 17 | Kernel CCB command processed |
| `HW poll … failed (reg:… val:…)` | HWR 13 | hardware poll failed → HWR path |
| `Discarded cmd on DM… FWCtx=…` | HWR 14 | command discarded during HWR |
| `GPU ready to be powered down` / `GPU deinit` / `GPIO disabled` | POW 6 / MAIN 144 / MISC 4 | power-off sequence |
| `Unknown Client CCB Command …` | MAIN 171 / 172 | unknown command type in a cCCB |

## HWPerf

- HWPerf is controlled by `ROGUE_FWIF_INICFG_HWPERF_EN` and by the KCCB commands
  `ROGUE_FWIF_KCCB_CMD_HWPERF_UPDATE_CONFIG`, `HWPERF_CTRL_BLKS`,
  `HWPERF_CONFIG_ENABLE_BLKS`, `HWPERF_SELECT_CUSTOM_CNTRS`, `HWPERF_CONFIG_BLKS`
  (`rogue_fwif_hwperf_ctrl` and related structures are public).
- The **packet format** of the HWPerf stream is **not** in the public headers, and the
  mainline driver does not read it. A mainline-compatible reimplementation may accept the
  HWPerf KCCB commands, return `CMD_EXECUTED`, and emit no packets.
- The cCCB `ROGUE_FWIF_CCB_CMD_TYPE_NULL` (210) command must still be processed (fences and
  updates) even if HWPerf is not implemented.

> The trace is the main **functional validation** tool for the reimplementation: comparing
> the sequence of events (kicks, command headers, completions, power) with the expected one
> confirms the behaviour without looking at the original binary.
