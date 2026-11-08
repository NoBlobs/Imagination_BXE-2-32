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
- **Write order of a remap entry (answers the clean team's Q3).** The 64-bit
  `RANGE_CONFIG` is written as two 32-bit halves, **low word first, then high word** (**D**):

  ```
  CR[0x0878]     = BASE_ADDR_IN | (REGION_SIZE << 7) | (ENTRY << 1) | MODE_ENABLE   // low
  CR[0x087C]     = (ADDR_OUT(PA >> 12)) << 4 | OS_ID                                // high
  // for the paired odd page, same two writes with ENTRY+16 and VA+page_size:
  CR[0x0878]     = (low word, ENTRY+16, BASE_ADDR_IN + page_size)
  CR[0x087C]     = (high word for the odd page)
  (void)CR[0x087C]   // read back the high word to flush the posted register writes
  ```

  The **low word carries `MODE_ENABLE` (bit 0) and the `ENTRY` index**, so the entry it
  selects is the one named in the low word; writing the low word then the high word installs
  that entry. A **read-back of the high half** (`0x087C`) flushes the posted writes before
  the mapping is relied on. So the committing order is low→high per entry, not "low, high,
  low".
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

### Device information block

The device-info block (between the ELF and the info header) carries the **BRN/ERN masks and
the feature mask + feature values** for this specific GPU. The host (and the firmware, via
`sysinit`) uses these to size structures and gate behaviour. The format is public
(`pvr_fw_info.h`, consumed in `pvr_fw.c`); the **values** below are the ones carried by the
redistributable **open** firmware image for BVNC `36.29.52.182` (**P** — read from the open
image's own device-info block, not from the closed firmware). This answers the clean team's
Q2 and corrects several of its assumptions.

- **Image facts:** info version 3; 6 layout entries; device-info block 4096 bytes.
- **BRN:** `63553`.
- **ERN:** `35421, 38748, 42290, 42606, 47025, 57596` (the clean team had only the first
  two — `42290, 42606, 47025, 57596` were missing).
- **Feature values (corrections in bold):**

| Feature | Value |
|---|---|
| `VIRTUAL_ADDRESS_SPACE_BITS` | 40 |
| `PHYS_BUS_WIDTH` | 36 |
| **`SLC_SIZE_IN_KILOBYTES`** | **16** (not 0 — affects the SLC-bypass decision at boot, see [boot-gpu-init](boot-gpu-init.md)) |
| `SLC_BANKS` | 1; `SLC_CACHE_LINE_SIZE_BITS` 512; `SLC_SIZE_CONFIGURABLE` present |
| `NUM_ISP_IPP_PIPES` | 2 |
| `NUM_CLUSTERS` | 1; `NUM_RASTER_PIPES` 1; `MAX_PARTITIONS` 8 |
| `ISP_MAX_TILES_IN_FLIGHT` | 2; `ISP_SAMPLES_PER_PIXEL` 1; `TILE_SIZE_X`/`Y` 16 |
| `COMMON_STORE_SIZE_IN_DWORDS` | 13312; `USC_MIN_OUTPUT_REGISTERS_PER_PIX` 2 |
| `NUM_OSIDS` | 8 |
| `FBCDC` / `FBCDC_ALGORITHM` | 50; `FBCDC_ARCHITECTURE` 7 |
| `SIMPLE_PARAMETER_FORMAT_VERSION` | 2 (`SIMPLE_INTERNAL_PARAMETER_FORMAT` + `_V2`) |
| `XE_ARCHITECTURE` | 1 |
| `XPU_MAX_SLAVES` | 3; `XPU_MAX_REGBANKS_ADDR_WIDTH` 19; `XPU_REGISTER_BROADCAST` 1 |

- **Boolean features present** (the clean team had assumed several of these **absent** —
  marked ✗→present): `MIPS`, `COMPUTE`, `COMPUTE_OVERLAP`, `COREID_PER_OS`, `IRQ_PER_OS`,
  `GPU_MULTICORE_SUPPORT`, `GPU_VIRTUALISATION`, `GS_RTA_SUPPORT`, `PBE2_IN_XE`,
  `PBVNC_COREID_REG`, `PERFBUS`, `ROGUEXE`, `SOC_TIMER`, **`AXI_ACELITE`**,
  **`CDM_CONTROL_STREAM_FORMAT` = 1**, **`LAYOUT_MARS` = 1**, **`SYS_BUS_SECURE_RESET`**,
  **`XE_MEMORY_HIERARCHY`**, `XE_TPU2`.
- **Features absent** on this GPU (so the corresponding `rogue_fwif_*` fields are unused):
  `VDM_DRAWINDIRECT`, `VDM_OBJECT_LEVEL_LLS`, `TESSELLATION`, `S7_TOP_INFRASTRUCTURE`,
  `XT_TOP_INFRASTRUCTURE`, `META*` (`META_COREMEM_SIZE` 0), `ZLS_SUBTILE`,
  `TPU_DM_GLOBAL_REGISTERS`. `FBC_MAX_*_DESCRIPTORS` = 0.

> **`SYS_BUS_SECURE_RESET` is required** (the clean team found this by experiment): without
> it in the device info, the host leaves `ROGUE_CR_SYS_BUS_SECURE` at its reset value and
> the MIPS cannot fetch from the reset vector `0xBFC00000`. The firmware also writes
> `ROGUE_CR_SYS_BUS_SECURE = 0` during power-on (see [boot-gpu-init](boot-gpu-init.md)).

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
