# Memory model

The firmware's MIPS core sees a 32-bit address space; the GPU hardware addresses a
**36-bit** bus. The bridge between them is the MIPS TLB (always identity) plus the RGX
wrapper's **remap registers**. The whole firmware VA→physical translation is **set up by
the host** (the driver manages the page tables).

## MIPS core address space

| VA | Contents |
|---|---|
| `0xBFC00000` | Boot ROM / reset+NMI vector (KSEG1, uncached). Boot/NMI code. |
| `0xBFC01000` | **BOOT_DATA** (also visible cached at `0x9FC01000`). Filled by the host at boot. See layout below. |
| `0x9FC02000` | **EBase** (exception/interrupt vectors). `+0x000` TLB refill, `+0x180` general exception, `+0x400` HW0 timer, `+0x500` HW1 BG, `+0x600` HW2 IRQ. |
| `0xC0000000` | Firmware code and private data (the internal layout is chosen by the image and exported to the host through the layout table, below). **Also the firmware heap window**: structures shared with the host are addressed as `0xC0xxxxxx`. |
| `0xCF000000` | MIPS page table (`EntryLo` format, written by the host). |
| `0xCF600000` | Firmware stack page (wired TLB entry; physical address comes from BOOT_DATA). |
| `0xCF800000` | RGX register bank of **core 0** (4 MiB, uncached). Core *n* at `0xCF800000 + n·0x80000`. |
| `0xCFA00000` | **XPU broadcast bank**: a write here goes to every core enabled in `ROGUE_CR_XPU_BROADCAST`. |

### BOOT_DATA (`0xBFC01000`)

The host writes the start of the page before releasing the MIPS from reset. That part is
public ABI (**P**, `struct rogue_mipsfw_boot_data` in `pvr_rogue_mips.h`): physical
addresses of the page-table pages, the stack and the register bank, the page size and the
number of page-table pages (see the end of this document).

The rest of the page is the **firmware's private area**, with a free layout. The firmware
must reserve room for:

- an error state and an "inside exception" flag;
- an **uncached** word used by the write barrier (see
  [execution-environment](execution-environment.md));
- a record of the last TLB fault (remap used, `BadVAddr`, `EntryLo0/1`, `ExcCode`), useful
  for diagnosis;
- the area where the MIPS state is saved on NMI.

## Translation contract (MIPS wrapper)

- The **TLB** maps VA → "MIPS PA" **1:1** (PFN = VA >> 12).
- The **RGX wrapper** converts each TLB page into the real 36-bit bus address using up to
  **32 `ROGUE_CR_MIPS_ADDR_REMAP_RANGE` entries** (`ROGUE_CR_MIPS_ADDR_REMAP_RANGE_CONFIG`,
  `0x0878`). Entry *i* serves the **even page** of TLB slot *i*; entry *i+16* the **odd
  page**:

  ```
  RANGE_CONFIG[31:0]  = BASE_ADDR_IN | (REGION_SIZE << 7) | (ENTRY << 1) | MODE_ENABLE
  RANGE_CONFIG[63:32] = OS_ID | (ADDR_OUT(PA >> 12) << 4)
  ```

  Field definitions: `pvr_rogue_cr_defs.h`. The fixed remaps for boot code, boot data and
  exception code (`ROGUE_CR_MIPS_ADDR_REMAP1..3_*`) are programmed by the host.
- The **TLB refill handler** (`0x9FC02000`) reads the physical PFN from the **host page
  table** (at `0xCF000000`), puts it into the corresponding remap entry, and keeps the TLB
  as identity. That is how the 32-bit MIPS reaches the 36-bit bus.

## Fault page

TLB faults (`Mod`/`TLBL`/`TLBS`) on pages without permission are **redirected to the
host's fault page** (`rogue_fwif_sysinit.fault_phys_addr`):

- **reads** return garbage,
- **writes** are discarded,
- the firmware records the fault (in the private fault record above) and raises
  `EVENT_STATUS.MMU_PAGE_FAULT` for the HWR path.

This makes the firmware robust to invalid pointers: instead of hanging, it diverts to the
fault page and triggers recovery. (See [hardware-recovery](hardware-recovery.md).)

## Heap window and per-OS virtualisation

Structures shared with the host live in the `0xC0xxxxxx` window. The firmware supports
**virtualisation**: several "OS" (guests) with their own CCBs and control blocks, isolated
by a fixed VA offset.

| Item | Value |
|---|---|
| Window size per OSID | 16 MiB |
| VA mask | `VA & 0xC0FFFFFC` |
| `osinit` of OS *n* | `0xC0FE0000 + n·16 MiB` |
| Connection page of OS *n* | `0xC0FD0000 + n·16 MiB` |
| `sysinit` | `0xC0FF0000` |
| Valid address | `(a & 0x3F000003) == 0` |

- The **VA mask** `0xC0FFFFFC` is applied when the firmware turns a firmware pointer
  received from the host (context fields, CCB bases, control blocks) into an address in
  its own space: it keeps the 16 MiB window and aligns to a word.
- **Use the same mask everywhere** a CCB, control block, context or command address is
  derived from a host value. It is an ABI value, not a free choice.
- A **null pointer** from the host is `0`. Test for null **before** applying the mask.

## MIPS page table (managed by the host)

- It sits at `0xCF000000` in the firmware's space; the host writes entries in `EntryLo`
  format (physical PFN + cache/valid flags).
- The host maps/unmaps firmware objects by writing this table and signalling the required
  MMU invalidation.
- When the firmware maps GPU memory into a temporary window (Vulkan robustness, RTC
  bitmap, buffer zeroing), it first invalidates the dynamic TLB entries (see
  [execution-environment](execution-environment.md)); if the `MMU_PAGE_FAULT` condition
  changes during the mapping, it triggers HWR.

## `.fw` container format (read by the host at boot)

The host needs to know where each firmware section goes in memory. The format is public
(**P**, `pvr_fw_info.h` and the validation in `pvr_fw.c`):

```
[ firmware ELF (code, data, boot/NMI, exception vectors…) ]
[ device info block (device_info_size bytes) ]
[ struct pvr_fw_info_header          @ (file size − 4096) ]
[ struct pvr_fw_layout_entry × layout_entry_num ]
```

- The file size must be a multiple of `FW_BLOCK_SIZE` (4 KiB).
- `pvr_fw_info_header`: `info_version` must be `PVR_FW_INFO_VERSION` (3); `header_len` and
  `layout_entry_size` must match the structure sizes; at most
  `PVR_FW_INFO_MAX_NUM_ENTRIES` (8) layout entries; `flags` must contain
  **`PVR_FW_FLAGS_OPEN_SOURCE`** (the mainline driver rejects images without it);
  `fw_version_major` must be non-zero and supported; `bvnc` must match the GPU.
- Each `pvr_fw_layout_entry` is `{id, type, base_addr, max_size, alloc_size,
  alloc_offset}`. Section IDs: `MIPS_CODE`, `MIPS_EXCEPTIONS_CODE`, `MIPS_BOOT_CODE`,
  `MIPS_PRIVATE_DATA`, `MIPS_BOOT_DATA`, `MIPS_STACK`. The host uses `base_addr` (VA) and
  `alloc_offset` (offset inside the code/data object) to build remaps and page tables.

> For a reimplementation, the essential point is to **export a correct layout table** so
> that the host remaps each section. `MIPS_BOOT_CODE`, `MIPS_BOOT_DATA` and
> `MIPS_EXCEPTIONS_CODE` have dedicated remaps.

## Page-table entry format (host side — public ABI)

The MIPS page table is **written by the host** in `EntryLo` format. The rule is in the
public mainline driver (`pvr_vm_mips.c`) and is quoted here because the firmware
**depends on it** (the TLB refill handler reads these entries). For the virtual page with
index `pfn` inside the heap (`pfn = (VA − ROGUE_FW_HEAP_BASE) >> 12`, 4 KiB pages):

```
PTE = ((dma_addr >> 12) << ENTRYLO_PFN_SHIFT) & pfn_mask
PTE |= pte_flags                      // read/write + cache policy
```

- **`pfn_mask`** and the **cache policy** depend on the physical bus width: on this SoC the
  bus is **> 32 bits**, so the >32-bit cached policy and `pfn_mask` are used (the 32-bit
  MIPS reaches 36 bits through remap). Objects flagged as device-uncached use the uncached
  policy.
- After writing/clearing entries, the host signals the required MMU invalidation.
- **BOOT_DATA** carries the physical addresses of the page-table pages, the page size and
  the number of page-table pages — this is how the firmware finds the table at boot.

> For the **firmware** reimplementation, what matters is that the TLB refill handler
> (`0x9FC02000`) reads the `EntryLo` from this table and puts it into the remap entry; the
> host takes care of the write format.
