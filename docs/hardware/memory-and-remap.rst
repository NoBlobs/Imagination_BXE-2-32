.. SPDX-License-Identifier: CC-BY-SA-4.0
.. Copyright (C) 2026 Thalles Campagnani

============================
Memory model and MIPS remap
============================

The MIPS microcontroller sees a 32-bit virtual address space while the GPU
system bus is **36 bits** wide and the GPU shader/data-master units address a
**40-bit** virtual space (:dc:`memory-model`). The host manages all MIPS page
tables; the firmware bridges 32-bit VA to 36-bit bus PA through the MIPS TLB
and the RGX wrapper remap registers.

Two-stage MIPS address translation
==================================

Every mapped MIPS access goes through **two** hardware translation stages:

1. **MIPS TLB** (16 entries, 4 KiB or 1 MiB page pairs). Every entry is
   programmed as a **1:1 identity map** (``PFN = VA >> 12``, dirty, valid,
   global). The TLB only marks a page as present and selects a slot ``n``
   (``0..15``).
2. **Wrapper remap table** (``ROGUE_CR_MIPS_ADDR_REMAP_RANGE_CONFIG``,
   ``0x0878``/``0x087C``). It has **32 entries**, each mapping one 4 KiB
   page of MIPS "physical" address to a 36-bit bus address. **Entry** ``n``
   **serves the even page of TLB slot** ``n``, **and entry** ``n + 16``
   **serves the odd page.**

Bitfield layout and write order (Q3, ``[spec]`` + :exp:`E003`):

.. code-block:: text

   RANGE_CONFIG[31:0]  (0x0878) = BASE_ADDR_IN | (REGION_SIZE << 7) | (ENTRY << 1) | MODE_ENABLE
   RANGE_CONFIG[63:32] (0x087C) = OS_ID | ((bus_pa >> 12) << 4)

   Write order (mandatory):
     1. CR[0x0878] = low word  (even page, ENTRY = slot)
     2. CR[0x087C] = high word (even page)
     3. CR[0x0878] = low word  (odd page,  ENTRY = slot + 16, VA + page_size)
     4. CR[0x087C] = high word (odd page)
     5. (void)CR[0x087C]  (or CORE_REVISION) — read-back flushes posted writes

Because the low word carries ``MODE_ENABLE`` (bit 0) and ``ENTRY``, writing
low then high installs that entry; reading back ``0x087C`` drains posted
writes before the mapping is used. To disable an entry, clear bit 0 and write
``high = 0``.

Fixed host-programmed remaps
----------------------------

Before releasing the MIPS from reset, the host programs fixed wrapper remaps
(``MIPS_ADDR_REMAP1..5_CONFIG1/2`` at ``0x0818..0x0860``, :exp:`E007`) for
the unmapped KSEG0/KSEG1 regions:

* ``0xBFC00000`` (4 KiB, KSEG1): boot code (reset/NMI vector);
* ``0xBFC01000`` (4 KiB, KSEG1; cached alias ``0x9FC01000``): ``BOOT_DATA``;
* ``0x9FC02000`` (4 KiB, KSEG0): exception/interrupt vectors (``EBase``).

The register banks at ``0xCF800000`` (core bank) and ``0xCFA00000`` (XPU
broadcast bank) are decoded directly by the wrapper and need wired TLB
entries only (no remap entries).

MIPS virtual address map
========================

.. list-table::
   :header-rows: 1
   :widths: 16 10 48 26

   * - VA
     - Size
     - Contents
     - Translation
   * - ``0x9FC02000``
     - 4 KiB
     - ``EBase``: ``+0x000`` TLB refill, ``+0x180`` general exception,
       ``+0x400`` HW0 timer, ``+0x500`` HW1 BG, ``+0x600`` HW2 IRQ,
       ``tlb_flush_dynamic``
     - KSEG0, unmapped; host fixed remap
   * - ``0xBFC00000``
     - 4 KiB
     - Boot / NMI code (reset vector)
     - KSEG1, unmapped; host fixed remap
   * - ``0xBFC01000``
     - 4 KiB
     - ``BOOT_DATA`` (cached alias ``0x9FC01000``): public
       ``rogue_mipsfw_boot_data`` header (``0x00..0x3F``), firmware private
       scratch area at ``+0x800``
     - KSEG1, unmapped; host fixed remap
   * - ``0xC0000000``
     - 64 KiB
     - Main firmware code and rodata (code object, offset ``0``)
     - Dynamic TLB (slots 5–15)
   * - ``0xC0ED0000``
     - 64 KiB
     - Private data and ``.bss`` (data object, offset ``0``)
     - Dynamic TLB (slots 5–15)
   * - ``0xC0FD0000``
     - 4 KiB
     - OS 0 connection page (``+ n * 16 MiB`` per OSID ``[spec]``)
     - Dynamic TLB
   * - ``0xC0FE0000``
     - —
     - ``rogue_fwif_osinit`` of OS 0 (``+ n * 16 MiB`` per OSID)
     - Dynamic TLB (slots 5–15)
   * - ``0xC0FF0000``
     - —
     - ``rogue_fwif_sysinit``
     - Dynamic TLB (slots 5–15)
   * - ``0xC0xxxxxx``
     - 16 MiB
     - Shared firmware heap (CCBs, contexts, trace, HWR buffers)
     - Dynamic TLB (slots 5–15)
   * - ``0xCF000000``
     - 16 KiB
     - Host MIPS page table (4 physical pages from ``BOOT_DATA``)
     - Wired TLB entries 0 and 1
   * - ``0xCF600000``
     - 8 KiB
     - Firmware stack (backed by data object ``+0x11000``)
     - Wired TLB entry 3
   * - ``0xCF800000``
     - 2–4 MiB
     - RGX core 0 register bank (core *n* at ``0xCF800000 + n * 0x80000``)
     - Wired TLB entry 2
   * - ``0xCFA00000``
     - 2 MiB
     - XPU broadcast register bank (writes all cores enabled in
       ``ROGUE_CR_XPU_BROADCAST``)
     - Wired TLB entry 4

Heap window and per-OS virtualisation
=====================================

Shared-memory pointers (``fw_addr``) live in a 16 MiB window per OSID
(:dc:`memory-model`):

.. list-table::
   :header-rows: 1
   :widths: 34 66

   * - Parameter
     - ABI value
   * - Window size per OSID
     - 16 MiB (8 OSIDs supported: ``NUM_OSIDS = 8``)
   * - ``osinit`` / connection page of OS *n*
     - ``0xC0FE0000 + n * 16 MiB`` / ``0xC0FD0000 + n * 16 MiB``
   * - ``sysinit``
     - ``0xC0FF0000``
   * - Mandatory VA mask
     - ``VA & 0xC0FFFFFC`` (strips bit 0 sync-checkpoint flag and upper
       meta-DMA/cache flags; word-aligns)
   * - Valid-address predicate
     - ``(addr & 0x3F000003) == 0`` (test for ``0`` **before** masking)

Host MIPS page table and fault page
===================================

The host fills the 4-page MIPS page table at ``0xCF000000`` in MIPS
``EntryLo`` format (``pvr_vm_mips.c``, ``[public]``):

.. code-block:: text

   pfn = (VA - 0xC0000000) >> 12
   PTE = ((dma_addr >> 12) << ENTRYLO_PFN_SHIFT) & pfn_mask | pte_flags

On the 36-bit BXE-2-32 bus, the >32-bit ``pfn_mask`` puts the 24-bit PFN in
**bits 29:6** (``PA = ((pte >> 6) & 0xFFFFFF) << 12``) and **bit 1 = valid**.

* **Fault page redirection**: any refill hitting an address outside
  ``0xC0xxxxxx`` or an invalid PTE (or a ``Mod``/``TLBL``/``TLBS`` fault) is
  diverted to ``sysinit.fault_phys_addr`` so reads return harmless data and
  writes are discarded instead of locking up the bus. In the reference
  firmware ``[spec]``, it also records fault metadata and raises
  ``EVENT_STATUS.MMU_PAGE_FAULT`` / ``CONTEXT_FW_PF_NOTIFICATION`` for HWR.
* **Dynamic TLB flush constraint** (:exp:`E003`): invalidating non-wired TLB
  entries (assigning unique unused KSEG0 VPNs, clearing ``EntryLo0/1``, and
  disabling remap pairs ``slot`` / ``slot + 16``) **must execute from an
  unmapped page** (the KSEG0 exception page at ``0x9FC02000``); running it
  from mapped code disables the remap of the executing page itself.
* **Temporary GPU-memory windows** ``[spec]``: before mapping arbitrary GPU
  memory into a temporary window (Vulkan robustness, RTC bitmap, zeroing),
  the firmware invalidates dynamic TLB entries first and checks
  ``MMU_PAGE_FAULT``.

GPU BIF / MMU and catalogue slots
=================================

GPU data masters translate 40-bit GPU virtual addresses through the Bus
Interface (BIF) MMU. Eight page-catalogue registers
``ROGUE_CR_BIF_CAT_BASE0..7`` (``0x1200 + 8n``) hold 4 KiB-aligned physical
addresses (``pc & 0xFF_FFFF_F000``):

* **Slot 0** is reserved for the firmware itself and is never assigned to
  client memory contexts.
* **Slots 1–7** hold client ``rogue_fwif_fwmemcontext.pc_dev_paddr``
  bindings (trace ``Setup register set = N`` corresponds to ``CAT_BASE{N+1}``
  and index value ``N + 1``, ``[spec]``).
* A slot holding the same page catalogue is reused without MMU invalidation;
  a slot in use by a busy DM must never be evicted.

Each DM selects its slot via ``ROGUE_CR_BIF_CAT_BASE_INDEX`` (``0x1240``):

.. list-table::
   :header-rows: 1
   :widths: 26 28 46

   * - Data master
     - Bit field in ``0x1240``
     - Notes
   * - ``GEOM`` (TA, DM 2)
     - bits 2:0 (shift 0)
     - Bound before PM / TA writes (:exp:`E034`)
   * - ``FRAG`` (3D / TQ_3D, DM 3)
     - bits 10:8 (shift 8)
     - Mandatory before any ISP/PBE write
   * - ``CDM`` (compute, DM 4)
     - bits 18:16 (shift 16)
     - Verified in :exp:`E006` (``0x00010000`` → slot 1)
   * - ``TDM`` (DM 1)
     - bits 21:19 (shift 19)
     - Unused by Mesa on BXE-2-32

Cache and MMU invalidation order
--------------------------------

Before invalidating the BIF MMU (``ROGUE_CR_BIF_CTRL_INVAL = 0xF``: bit 0
``PT``, bit 1 ``PD``, bit 2 ``PC``, bit 3 ``TLB1``) or binding a new page
catalogue into a slot, flush and invalidate the 16 KiB SLC
(``SLC_CTRL_FLUSH_INVAL`` → poll ``SLC_STATUS0`` bit 2). Omitting this flush
can cause the first job of a new memory context to fault on its USC shader
fetch (:exp:`E015`).

Bus security and fault reporting
--------------------------------

* ``ROGUE_CR_SYS_BUS_SECURE`` (``0xA100``): cleared by the host when
  ``SYS_BUS_SECURE_RESET`` is set in device info (:exp:`E001`), and cleared
  again (write ``0`` + read back) during GPU power-on.
* ``ROGUE_CR_BIF_FAULT_READ`` (``0x13E0``): set to
  ``sysinit.fault_phys_addr`` so faulting GPU units read the dummy page.
* ``BIF_FAULT_BANK0_MMU_STATUS`` (``0x12B0``) and
  ``BIF_FAULT_BANK0_REQ_STATUS`` (``0x12B8``): report the faulting address,
  catalogue slot and requestor. Under BXE-2-32's ``XE_MEMORY_HIERARCHY``
  layout (``[public]``, vendor ``rgxmhdefs_km.h``), ``REQ_STATUS`` encodes
  ``TAG_SB`` in bits 51:46 and ``TAG_ID`` in bits 45:40 (e.g. ``0x25`` = IPF
  control request, ``0x30`` = PM allocation unit, ``0x31`` = PM deallocation
  unit).
