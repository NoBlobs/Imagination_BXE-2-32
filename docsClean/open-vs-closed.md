# Open vs. closed — sources and corroboration

There are **two variants** of the firmware for the **same MIPS MCU** and the **same BVNC
`36.29.52.182`**:

- **open**: speaks the **upstream** interface `pvr_rogue_fwif*.h` (`drm/imagination`,
  `powervr` driver). **It is the target of the reimplementation.**
- **closed**: ships with the vendor's proprietary driver. It has **other structure
  layouts, other command numbering and some extra features**.

The high-level logic of both is the same. This spec describes **only the open variant**.
Layout values and other details of the closed variant are deliberately **not** included:
the target does not need them, and they come from proprietary material.

## Source hierarchy (clean room)

From most to least "clean":

1. **Public upstream headers** (`pvr_rogue_fwif*.h`, `pvr_rogue_fwif*_check.h`,
   `pvr_rogue_cr_defs.h`, `pvr_rogue_mips.h`, `pvr_fw_info.h`) and the public mainline
   driver. The clean team **may read them directly**.
2. **Observed behaviour** in a black box (firmware trace, dEQP, sequences captured on the
   board).
3. **Firmware analysis**. It reaches the clean team **only** in the form of this spec.

## Corroboration

When an interface fact appears **in both variants**, it is almost certainly imposed by the
hardware or by the ABI, not a choice of one implementation. These are the **most solid**
requirements of this spec:

- the **three-interrupt model** (timer/BG/IRQ) and the absence of a main loop;
- the **command magic** (`ROGUE_CMD_MAGIC_DWORD`, `0x2ABC` in the upper 16 bits) and the
  **bit 15** marking a task in the cCCB (`ROGUE_CCB_TYPE_TASK`);
- the **FWCCB commands** (same IDs in both variants) and the core KCCB commands (KICK,
  MMUCACHE, CLEANUP, POW, HEALTH_CHECK, FREELIST_GROW_UPDATE…);
- the MIPS **remap contract** (identity TLB + `MIPS_ADDR_REMAP_RANGE`) and the fault page;
- the **`ROGUE_CR_*` registers**, which belong to the hardware;
- the **power** (idle → off → warm boot) and **HWR** (detect → reset → recover → rebuild
  freelists) state machines.

The closed variant also exposes **hardware capabilities** the open target does not
require (cancelling submitted work, extended virtualisation, a second geometry pipe on
multicore). They are out of scope.

## Traceability (provenance of each requirement)

Every host↔firmware interface requirement can be labelled by provenance:

- **P**: defined in a **public upstream header** and/or used by the `powervr` driver /
  Mesa. The cleanest source.
- **O**: **observed** in a black box (execution counted in the trace, dEQP result,
  captured sequence).
- **D**: comes from firmware analysis.

The interface is **stable and well covered**: most KCCB/FWCCB commands, all shared
structure sizes and offsets (`pvr_rogue_fwif_check.h`) and the trace-ID table
(`pvr_rogue_fwif_sf.h`) have provenance **P**, and many items also have **O**. The clean
team implements most of the contract from public sources and observed behaviour. The **D**
items of this spec fill in **semantics** (what each field means, in what order to act).

> **Practical advice.** Implement from **P** (public headers) and **O** (behaviour). Use
> the **D** items of this spec for semantics and ordering, always cross-checking names and
> layouts against the public header.
