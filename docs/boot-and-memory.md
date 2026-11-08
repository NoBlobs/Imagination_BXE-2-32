<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# Boot path and memory map

## Two address translations

The MIPS core sees 32-bit virtual addresses. The GPU system bus is 36 bits wide. A
32-bit address therefore goes through **two** translation stages:

1. **MIPS TLB** (16 entries, 4 KiB or 1 MiB page pairs). This firmware always programs
   it as an **identity** map: PFN = VA >> 12, uncached, dirty, valid, global. The TLB
   only says that a page is present.
2. **Wrapper remap** (`ROGUE_CR_MIPS_ADDR_REMAP_RANGE_CONFIG`, `0x0878`). It has 32
   entries. Each entry maps one 4 KiB page of MIPS "physical" address to a 36-bit bus
   address. **Entry `n` serves the even page of TLB slot `n`, and entry `n + 16` serves
   the odd page.**

A remap entry is written as two 32-bit halves:

```
low  = page_va | (entry << 1) | 1            (bit 0 = enable; bits 12+ = input page)
high = (bus_pa >> 12) << 4                   (36-bit PA, 4 KiB page number at bit 4)
write order: low, high, low                  (open question Q3; this order works)
```

To disable an entry, write it with the enable bit clear and high = 0.

## Memory map (MIPS virtual)

| VA | Size | What | Mapped by |
|---|---|---|---|
| `0x9FC02000` | 4 KiB | Exception page (EBase): refill, general, interrupt vectors, `tlb_flush_dynamic` | KSEG0, unmapped. The host remaps it. |
| `0xBFC00000` | 4 KiB | Boot code (reset/NMI vector) | KSEG1, unmapped. The host remaps it. |
| `0xBFC01000` | 4 KiB | BOOT_DATA: the host's public header (`0x00..0x3F`), then the firmware's private area at `0x800` | KSEG1. The host remaps it. |
| `0xC0000000` | 64 KiB | Main code and rodata (code object, offset 0) | dynamic TLB |
| `0xC0ED0000` | 64 KiB | Private data and .bss (data object, offset 0) | dynamic TLB |
| `0xC0FE0000` | | `rogue_fwif_osinit` (host) | dynamic TLB |
| `0xC0FF0000` | | `rogue_fwif_sysinit` (host) | dynamic TLB |
| `0xC0xxxxxx` | 16 MiB | The rest of the firmware heap (CCBs, contexts, trace…) | dynamic TLB |
| `0xCF000000` | 16 KiB | Host MIPS page table (4 pages) | wired TLB 0/1 |
| `0xCF600000` | 8 KiB | Stack (backed by data object `+0x11000`) | wired TLB 3 |
| `0xCF800000` | 2 MiB | GPU register bank (one 1 MiB page pair) | wired TLB 2 |
| `0xCFA00000` | 2 MiB | XPU broadcast register bank | wired TLB 4 |

Wired TLB entries 0–4 come from the boot code (`Wired = 5`). Entries 5–15 are the
dynamic pool, used round-robin by the refill handler.

### Host objects and the layout table

The host allocates two objects in the firmware heap, *code* and *data*. Each section
is then placed with the layout table in the `.fw` file:

| Section id | Object | Offset | VA | Size |
|---|---|---|---|---|
| `MIPS_CODE` | code | `0x00000` | `0xC0000000` | `0x10000` |
| `MIPS_BOOT_CODE` | code | `0x10000` | `0xBFC00000` | `0x1000` |
| `MIPS_EXCEPTIONS_CODE` | code | `0x11000` | `0x9FC02000` | `0x1000` |
| `MIPS_PRIVATE_DATA` | data | `0x00000` | `0xC0ED0000` | `0x10000` |
| `MIPS_BOOT_DATA` | data | `0x10000` | `0xBFC01000` | `0x1000` |
| `MIPS_STACK` | data | `0x11000` | `0xCF600000` | `0x2000` |

The data object sits at a **fixed** VA (`0xC0ED0000`), inside the last MiB of the heap
that the mainline host reserves for itself (sysinit and osinit live there too). This
works because the mainline host places the data object at the address the layout
table requests. All objects are mapped `DEVICE_UNCACHED`.

### BOOT_DATA private area (`0xBFC01000 + 0x800`)

The boot code and the refill handler run before the stack exists, or with only
`$26`/`$27` free. They keep their state in the KSEG1 boot-data page:

| Offset | Name | Use |
|---|---|---|
| `+0x800` | `BD_BARRIER` | uncached word written and read back by `write_barrier()` |
| `+0x804` | `BD_TLB_NEXT` | next dynamic TLB slot (round-robin 5..15) |
| `+0x808` | `BD_FAULT_HI` | remap high word of the host fault page (for invalid PTEs) |
| `+0x80C` | `BD_FAULT_COUNT` | refills that hit an invalid PTE |
| `+0x810`/`814`/`818` | `BD_LAST_BADVADDR`/`CAUSE`/`EPC` | last exception |
| `+0x81C` | `BD_IN_EXCEPTION` | nested-fault guard |
| `+0x820` | `BD_BOOT_STAGE` | progress marker (`0xA00n` in boot.S, `0xB007n`, then `4..6` in C) |
| `+0x824` | `BD_REFILL_COUNT` | number of TLB refills |
| `+0x840` | `BD_SAVE` | refill handler register save area (8 words) |
| `+0x880` | `BD_EXC_REGS` | register dump on a fatal exception |

The same progress markers are mirrored to the scratch register `0x1A00`. The host can
then read them through `/dev/mem` while the firmware runs (`tools/gpu-regs.py`).

## Boot sequence (`boot.S`)

The host releases the MIPS from reset at `0xBFC00000` (KSEG1). Every boot, cold or
warm, takes this path:

| Marker | Step |
|---|---|
| `0xA001` | `Status = CU0 \| BEV \| ERL`, interrupts off |
| `0xA002` | `Config.K0 = 2`: KSEG0 uncached |
| `0xA003` | `Config3.ISAOnExc = 1`: exception handlers are microMIPS |
| `0xA004` | `Cause.IV = 1`: dedicated interrupt vectors |
| `0xA005` | `EBase = 0x9FC02000` |
| `0xA006` | `IntCtl.VS = 8` (256-byte vector spacing: HW0 at `+0x400`, HW1 `+0x500`, HW2 `+0x600`) |
| | `Count = 0`, `PageMask = 4K`, `Wired = 0`, `EntryLo0/1 = 0` |
| `0xA007` | All 16 TLB entries invalidated. Each gets a unique KSEG0 VPN, which is never translated. |
| `0xA008` | Wired entry 2: register bank `0xCF800000`. Entry 4: broadcast bank `0xCFA00000`. Both are 1 MiB pages and need no remap, because the wrapper decodes those addresses itself. |
| `0xB0071` | Page table: the 4 physical pages from BOOT_DATA `pt_phys_addr[0..3]`, remap entries 0, 16, 1, 17. Then TLB entries 0 and 1, identity at `0xCF000000`. |
| `0xB0072` | Stack: the PTEs of the two stack pages in the data object are read from the page table, then remap entries 3 and 19, and TLB entry 3 at `0xCF600000`. |
| `0xB0073` | `Wired = 5`, `BD_TLB_NEXT = 5` |
| | `sp` = top of stack. `Status = CU0 \| ERL`. `ErrorEPC = fw_entry \| 1` (ISA bit). `ERET` |

`fw_entry` (`.text`, mapped through the refill handler) clears `.bss` and calls
`fw_main()`. That function writes markers 4, 5 and 6, and sets `firmware_started` just
before marker 6.

## TLB refill (`vectors.S`, `+0x000` → `refill_main` at `+0xA00`)

When an address in `0xC0xxxxxx` misses in the TLB:

1. Save `$1`, `$8`–`$13` in `BD_SAVE`. Count the refill in `BD_REFILL_COUNT` and scratch
   `0x1A08`, and store the VA in `0x1A10`.
2. Align the VA to the even page of the pair. The PTE index is `(VA & 0x00FFF000) >> 12`.
   Read the two PTEs (even and odd) from the host page table at `0xCF000000`.
   - A host MIPS PTE has the **PFN in bits 29:6** and **bit 1 = valid**, so
     `PA = ((pte >> 6) & 0xFFFFFF) << 12`.
   - Any address outside `0xC0xxxxxx`, or an invalid PTE, is sent to the host's
     **fault page** (`sysinit.fault_phys_addr`, cached in `BD_FAULT_HI`). The access
     then reads harmless data instead of locking up, and `BD_FAULT_COUNT` counts it.
3. Choose slot `BD_TLB_NEXT` (round-robin over 5..15). Write remap entries `slot` and
   `slot + 16`, and read back `CORE_REVISION` to drain the writes.
4. Write the identity TLB entry with `tlbwi`, restore the registers and return with
   `ERET`.

There is no eviction policy beyond round-robin. The firmware touches few pages: code,
data, stack, its own structures, and the CCB and context pages of active work.

### `tlb_flush_dynamic`

`KCCB_MMUCACHE` and the power-off path call it. The host may have changed firmware
mappings, and no dynamic mapping may survive into the next boot. It invalidates TLB
entries `Wired..15` and disables their two remap entries. It must run from an
**unmapped** page, the exception page in KSEG0. If it ran from mapped code, it would
disable the remap of the page it is executing from
([E003](logRefs/E003-tlb-flush-from-unmapped-page.md)).

## Exceptions

Any exception that is not a TLB refill or an interrupt lands at `+0x180` →
`general_main`, which works as follows:

1. Store Cause, EPC and BadVAddr in BOOT_DATA and in scratch registers
   `0x1A18`/`0x1A20`/`0x1A28`.
2. On a nested fault, stop (`WAIT` loop).
3. Otherwise, switch to the top of the stack with `EXL/ERL/IE = 0` and call
   `fatal_exception()`. That function sets `HWR_FW_FAULT` in `sysdata.hwr_state_flags`,
   increments `fw_faults`, emits a `0xDEAD00cc` trace and interrupts the host.

The firmware does not recover after a fatal exception. The host notices the dead
firmware through its own timeouts.

## Debug scratch registers

`ROGUE_CR_SCRATCHn` (`0x1A00 + 8n`) are plain read/write registers that the host can
read at any time while the GPU is powered.

| Register | Name | Written by |
|---|---|---|
| `0x1A00` | `CR_DBG_STAGE` | boot progress: `0xB007n`, then `0xC0DE0004..6` |
| `0x1A08` | `CR_DBG_REFILLS` | refill handler (count) |
| `0x1A10` | `CR_DBG_LASTVA` | refill handler (last missed VA) |
| `0x1A18`/`20`/`28` | `CR_DBG_EXC_*` | general exception: Cause, EPC, BadVAddr |
| `0x1A30`/`38` | `CR_DBG_UNITS_PDS`/`USC` | read-back of the exec-base experiment ([E009](logRefs/E009-pds-exec-base-0x0610.md)) |
| `0x1A78` | `CR_SCRATCH15` | bitmask of the busy data masters (set at kick, cleared at completion) |
