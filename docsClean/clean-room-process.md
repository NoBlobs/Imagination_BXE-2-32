# Clean-room process

This document defines how the reimplementation must be carried out to keep the clean-room
separation.

## The two teams

- **Dirty team (analyst).** Produced this specification by reverse-engineering the
  original firmwares and the vendor driver, and by observing the hardware. Maintains the
  spec.
- **Clean team (implementer).** Writes the new firmware implementation. **Has access only
  to this spec** (`docsClean/`).

## The firewall

The clean team **must not**, at any time:

- Read the original firmware binary (`rgx.fw*`, `rogue_*.fw`), its disassembly,
  decompiler output, or memory dumps of the original firmware.
- Read the vendor's DDK/driver source code (`img-rogue`, the DDK's `rgx_fwif_*.h`) or any
  proprietary header.
- Receive from the dirty team any piece of code, assembly or pseudocode of the original.
- Modify this documentation.

The clean team **may**:

- Read this spec and implement from it.
- Read public hardware documentation referenced by the spec (MIPS32r2 / microMIPS ISA, o32
  conventions), the **upstream** Linux kernel headers that are part of the mainline
  driver's public ABI (`drm/imagination`: `include/uapi/drm/pvr_drm.h`,
  `pvr_rogue_fwif*.h`, `pvr_rogue_fwif*_check.h`, `pvr_rogue_cr_defs.h`,
  `pvr_rogue_mips.h`, `pvr_fw_info.h`), and the mainline driver itself. These are the
  public host↔FW interface, not the firmware.
- Use free toolchains (GCC/Clang for microMIPS o32 soft-float) and free libraries.
- Write new documentation derived from this one.
- Use the request channel to ask for improvements to the clean documentation; the
  maintainer evaluates whether each request is allowed.

## What goes into the spec (allowed)

This spec contains **interface and behaviour facts**, which are not protected creative
expression of the original firmware and are needed to interoperate:

- **Hardware interfaces:** addresses and semantics of RGX registers, page table/remap
  format, exception vectors, interrupt model.
- **Host↔firmware ABI:** shared structure layouts, field offsets, sizes, CCB command
  formats, return codes.
- **Protocols:** handshake sequences (boot, kick, power, HWR, freelist grow), power state
  machine, scheduling rules.
- **Functionally described algorithms:** *what* each part computes and the rules it
  follows (in prose, tables and behavioural pseudocode), not the original code.

## What does NOT go into the spec (forbidden)

- Source code, assembly or pseudocode transcribed from the original firmware.
- Function addresses of the original binary as an implementation reference (an
  implementer does not need them; they belong to the dirty team). **Hardware** addresses
  and **ABI structure** offsets are allowed — they are dictated by the hardware/host.
- The original's **internal organisation**: names of internal functions, variables and
  globals, its decomposition into functions, signatures and arguments of internal calls,
  and the layout of **private** firmware structures (those that are not part of the ABI
  with the host). The spec describes **what** must exist and happen. How to organise that
  in code is the clean team's choice.
- Compiler-generated forms from the original (e.g. `switch` normalisation constants, jump
  tables).
- Details of the closed variant (layouts, numbering, trace strings), which come from
  proprietary material.
- Names from the vendor's DDK. Every name used in the spec is the one from the **public
  upstream ABI**: `rogue_fwif_*` structures and fields, `ROGUE_FWIF_*` constants and
  command IDs, `ROGUE_CR_*` registers, `PVR_FWIF_DM_*` data masters, and trace entries
  identified by their public (group, id, format string).

## Source hierarchy

For the clean room, sources have **decreasing confidence**:

1. **Public upstream headers** and the public mainline driver. The clean team **may read
   them directly**.
2. **Observed behaviour** (black box: firmware trace, dEQP, sequences captured on the
   board).
3. **Firmware analysis**, which reaches the clean team **only** in the form of this spec,
   already converted into behaviour and interface facts.

The target is the **open variant** of the firmware. Items of this spec that came from
analysis are marked with provenance **D**. See [open-vs-closed](open-vs-closed.md).

## Question flow

1. The clean team finds a gap or ambiguity while implementing.
2. It opens a **functional** question ("when the host updates a cCCB `write_offset`
   without a KICK, must the firmware react?").
3. The dirty team answers by **extending/clarifying the spec** — never by sending
   original code. If needed, it performs new analysis and documents the observed
   behaviour.

## Acceptance criteria for the reimplementation

The new implementation is considered conformant when, on the K1 mainline stack
(`powervr` driver + Mesa `pvr`):

- It boots (the host reports `Initialized powervr`, no firmware page fault during boot).
- It processes the Kernel CCB (KICK/MMUCACHE/CLEANUP) and returns the correct return-slot
  values.
- It runs geometry, fragment, transfer and compute workloads to completion, with correct
  fence/UFO signalling to the host.
- It handles power management (idle → power-off → warm boot) without losing the device.
- It recovers from faults (HWR) without hanging.
- It passes pixel-checked render tests (e.g. dEQP-VK draw/renderpass) and real Vulkan
  workloads.

> **Validation advice.** Validate on **more than one DRAM configuration**, including the
> most conservative one available. On this board, **single-bit errors** in command reads
> under marginal DRAM produce symptoms that look like logic bugs (e.g. a geometry command
> read as compute). If a failure disappears with the most stable DRAM setting and
> reappears when the frequency is raised, the cause is memory margin, not the
> implementation.
