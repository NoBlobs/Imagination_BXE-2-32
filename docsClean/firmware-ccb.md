# Firmware CCB (firmware → host)

The **Firmware CCB** (FWCCB) is the ring of **notifications from the firmware to the
host** — asynchronous requests and events the host must handle (grow a freelist, back a ZS
buffer, context reset, debug dump, clock change, etc.).

## Structure and sending

- Ring + control block `rogue_fwif_ccb_ctl` (same structure as the KCCB: `write_offset
  +0x00`, `read_offset +0x80`, `wrap_mask +0x84`), referenced from `osinit`.
- To send a command:
  1. **acquire a slot**: if the ring is full, the firmware **busy-waits** until the host
     consumes entries (public trace: group MAIN id 90, `FWCCB for OSid %u is full, we will
     have to wait for space!`);
  2. write the command into the slot;
  3. apply the **write barrier**;
  4. advance `write_offset`;
  5. raise an IRQ to the host.

## Command list (`ROGUE_FWIF_FWCCB_CMD_*`, same IDs in both variants)

| id | Command | Meaning |
|---|---|---|
| 101 | `ZSBUFFER_BACKING` | ask the host to back a ZS buffer (partial render) |
| 102 | `ZSBUFFER_UNBACKING` | ask the host to unback a ZS buffer |
| 103 | `FREELIST_GROW` | ask the host to grow a freelist (PM OOM) |
| 104 | `FREELISTS_RECONSTRUCTION` | ask for freelist reconstruction (after HWR) |
| 105 | `CONTEXT_RESET_NOTIFICATION` | notify a context reset (robustness) |
| 106 | `DEBUG_DUMP` | ask for a debug dump |
| 107 | `UPDATE_STATS` | update statistics on the host |
| 108 | `CORE_CLK_RATE_CHANGE` | report a core clock change |
| 109 | `REQUEST_GPU_RESTART` | ask for a GPU restart |
| 112 | `CONTEXT_FW_PF_NOTIFICATION` | report a firmware-internal page fault |

Authoritative list: `enum rogue_fwif_fwccb_cmd_type` in `pvr_rogue_fwif.h`.

## Protocols that use the FWCCB

- **Freelist growth (OOM):** when geometry runs out of PM pages, the firmware sends
  `FREELIST_GROW` and enters "wait for grow"; the host grows the freelist and answers with
  the KCCB `FREELIST_GROW_UPDATE`. See [partial-render-spm](partial-render-spm.md).
- **On-demand ZS buffer (partial render):** `ZSBUFFER_BACKING` ↔ KCCB
  `ZSBUFFER_BACKING_UPDATE`.
- **Freelist reconstruction after HWR:** `FREELISTS_RECONSTRUCTION` ↔ KCCB
  `FREELISTS_RECONSTRUCTION_UPDATE`. See [hardware-recovery](hardware-recovery.md).
- **Firmware page fault notification:** `CONTEXT_FW_PF_NOTIFICATION` — the firmware
  diverted through the fault page on an invalid pointer; the host logs it. Not fatal by
  itself.

Each protocol is a **request (FWCCB) → host action → confirmation (KCCB)** pair; the
firmware keeps a per-freelist/per-context state machine to match requests and responses.
