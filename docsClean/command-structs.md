# Kick command structures

The work payloads the host builds in the Client CCB and the firmware reads on dispatch
([client-ccb](client-ccb.md)). The **authoritative** definitions of these structures are
in the **upstream public headers** (`drm/imagination`: `pvr_rogue_fwif_client.h`, with
sizes and offsets in `pvr_rogue_fwif_client_check.h`) — the clean team **can and should
read them directly** (they are public ABI, not the firmware). This document gives the
**sizes** and the **semantic role** of the key contents, to guide the implementer.

> **Provenance.** Exact layout = **P** (public header). Field semantics = **P** + **O**
> (observed in the trace). Treat the header as the truth; use the notes below to
> understand what each part does.

## Command header — `rogue_fwif_ccb_cmd_header` (0x28 B)

Precedes every command in the cCCB: `cmd_type`, `cmd_size`, `ext_job_ref`,
`int_job_ref`, `work_est_kick_data` (work estimation for DVFS). **The firmware reads the
command type from `cmd_type`, the first word of the header**; bit 15 marks a task. The
payload starts right after the header.

## Key property: the host prepares the register values

Each kick command embeds a **block of register values already prepared by the host/Mesa**
(**P**): `rogue_fwif_geom_regs` (0x40 B) inside the geometry command,
`rogue_fwif_frag_regs` (0x1C0 B) inside the fragment command, and similar blocks for
compute and transfer. Their field names in the public header (e.g. `vdm_ctrl_stream_base`,
`isp_ctl`, `isp_zls_pixels`, `pbe_word`, `cdm_ctrl_stream_base`) identify which hardware
register each value is meant for. The firmware therefore mostly **copies prepared values
into the registers and starts the unit**, instead of computing the configuration itself.
This greatly reduces what the reimplementation must compute in a kick.

## Geometry — `rogue_fwif_cmd_geom` (0x70 B, DM 2/6)

Programs the *Tiling Accelerator*: a shared part (`cmd_shared`, which references the
HWRTData), the register block `regs` (`rogue_fwif_geom_regs`), `flags`, a partial-render
fence UFO and region-header initialisation data. Roles observed in the trace of the
geometry kick:

- **VHeap**, **TPC**, **TE region headers** (`TE Region headers base … (RGNHDR Init: …)`);
- **list stack pointers** (ALIST/MLIST);
- **freelist / Parameter Buffer** (local vs global);
- **HWRTData** link (`TA RTData … loaded on HW context …`);
- kick flags: first kick, last kick, CSW resume.

## Fragment — `rogue_fwif_cmd_frag` (0x1E0 B, DM 3)

`cmd_shared`, the register block `regs` (`rogue_fwif_frag_regs`: ISP, ZLS, PBE, PDS
background, …), `flags`, ZLS/SLS strides, `execute_count`. Roles observed: ISP
configuration, tiles in flight, dusts, partition mask (`Updating Tiles In Flight (Dusts=…,
PartitionMask=…, ISPCtl=…)`), ZS/MSAA buffers, HWRTData, 3D stack pointers (`Loading
stack-pointers for …`), partial-render flag, CSW resume.

## Transfer — `rogue_fwif_cmd_transfer` (0xC0 B)

Blit/resolve through 3D (TQ_3D), TDM (TQ_TDM) or 2D (TQ_2D). Carries a register block with
source/destination and resolve (MSAA) parameters.

## Compute — `rogue_fwif_cmd_compute` (0x60 B, DM 4)

`common`, the register block `regs` (`rogue_fwif_compute_regs`: control stream base,
context state base, circular buffer, cluster, …), `flags`, `num_temp_regions`,
`stream_start_offset`, `execute_count`. The kick uses the CDM registers
(`ROGUE_CR_CDM_CONTEXT_PDS0/1`, `ROGUE_CR_CDM_TERMINATE_PDS/1`).

## How the firmware consumes a command (common)

1. The command is at cCCB base + `read_offset` ([client-ccb](client-ccb.md)); its type is
   the header's `cmd_type`.
2. It skips the 0x28-byte header and reads the structure for that type.
3. It activates the memory context (BIF), programs the register set, **loads the prepared
   values into the unit's registers** and starts it. See [data-masters](data-masters.md),
   [kick-geometry-ta](kick-geometry-ta.md) and [kick-compute-and-3d](kick-compute-and-3d.md).

## Pending detail

The kick documents refer to some command fields as **`cmd+0xNN`**: the offset is measured
from the **start of the command, i.e. the start of the 0x28-byte header** (payload offset =
`0xNN − 0x28`). Example (compute): `cmd+0x50` = `regs.cdm_ctrl_stream_base` and `cmd+0x58`
= `regs.cdm_context_state_base_addr`, both 64-bit (payload offsets `0x28` and `0x30`, from
`pvr_rogue_fwif_client_check.h`).

`PENDING` — the field name of every `cmd+0xNN` used in the kick documents. It is obtained
mechanically from the public header and its check file, and the clean team can do it
directly.
