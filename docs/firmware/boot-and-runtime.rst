.. SPDX-License-Identifier: GPL-3.0-or-later
.. Copyright (C) 2026 Thalles Campagnani

================
Boot and runtime
================

This page traces the firmware from MIPS reset release through TLB setup,
GPU bring-up, dynamic TLB refill, fatal exceptions and debug scratch
mailboxes. See :doc:`../hardware/memory-and-remap` for the hardware
translation model and MIPS virtual address map.

``BOOT_DATA`` private area (``0xBFC01000 + 0x800``)
===================================================

``boot.S`` and the TLB refill handler run before a stack exists (or with only
``$26``/``$27`` free), so they store private state in the unmapped KSEG1
``BOOT_DATA`` page above the host's public ``0x00..0x3F`` header:

.. list-table::
   :header-rows: 1
   :widths: 22 26 52

   * - Offset
     - Symbol
     - Use
   * - ``+0x800``
     - ``BD_BARRIER``
     - Uncached word written and read back by ``write_barrier()``
   * - ``+0x804``
     - ``BD_TLB_NEXT``
     - Next dynamic TLB slot (round-robin ``5..15``)
   * - ``+0x808``
     - ``BD_FAULT_HI``
     - Remap high word of the host fault page (for invalid PTEs)
   * - ``+0x80C``
     - ``BD_FAULT_COUNT``
     - Count of refills that hit an invalid PTE
   * - ``+0x810`` / ``814`` / ``818``
     - ``BD_LAST_BADVADDR`` / ``CAUSE`` / ``EPC``
     - Last exception snapshot
   * - ``+0x81C``
     - ``BD_IN_EXCEPTION``
     - Nested-fault guard
   * - ``+0x820``
     - ``BD_BOOT_STAGE``
     - Progress marker (``0xA00n``, ``0xB007n``, then ``4..6`` in C)
   * - ``+0x824``
     - ``BD_REFILL_COUNT``
     - Number of TLB refills
   * - ``+0x840``
     - ``BD_SAVE``
     - Refill handler register save area (8 words)
   * - ``+0x880``
     - ``BD_EXC_REGS``
     - Register dump on a fatal exception

Boot markers are mirrored to scratch register ``0x1A00`` so the host can
inspect boot progress live via ``tools/gpu-regs.py``.

Boot sequence (``src/boot.S``)
==============================

Every boot — cold or warm after runtime PM — executes the full sequence from
``0xBFC00000``:

.. list-table::
   :header-rows: 1
   :widths: 14 86

   * - Marker
     - Step
   * - ``0xA001``
     - ``Status = CU0 | BEV | ERL``, interrupts disabled
   * - ``0xA002``
     - ``Config.K0 = 2`` (KSEG0 uncached)
   * - ``0xA003``
     - ``Config3.ISAOnExc = 1`` (exception handlers are microMIPS)
   * - ``0xA004``
     - ``Cause.IV = 1`` (dedicated interrupt vectors)
   * - ``0xA005``
     - ``EBase = 0x9FC02000``
   * - ``0xA006``
     - ``IntCtl.VS = 8`` (256-byte vector spacing: HW0 ``+0x400``, HW1
       ``+0x500``, HW2 ``+0x600``); clear ``Count``, ``PageMask = 4K``,
       ``Wired = 0``, ``EntryLo0/1 = 0``
   * - ``0xA007``
     - Invalidate all 16 TLB entries with unique KSEG0 VPNs
   * - ``0xA008``
     - Wired TLB entry 2: core register bank ``0xCF800000``; entry 4:
       broadcast bank ``0xCFA00000`` (1 MiB page pairs, no wrapper remap)
   * - ``0xB0071``
     - Map the 4 host page-table pages from ``BOOT_DATA.pt_phys_addr[0..3]``
       via remap entries 0, 16, 1, 17 and wired TLB entries 0 and 1 at
       ``0xCF000000``
   * - ``0xB0072``
     - Read the two stack PTEs from the page table, program remap entries
       3 and 19, and install wired TLB entry 3 at ``0xCF600000``
   * - ``0xB0073``
     - ``Wired = 5``, ``BD_TLB_NEXT = 5``, load ``sp`` = top of stack,
       ``Status = CU0 | ERL``, ``ErrorEPC = fw_entry | 1``, ``ERET``

``fw_entry`` (in ``.text``, mapped on demand by the refill handler) clears
``.bss`` and calls ``fw_main()``, which emits markers ``0xC0DE0004..6``,
runs ``gpu_power_on()`` and ``gpu_units_init()`` in ``src/gpu_init.c``
(:doc:`../hardware/gpu-bringup`), publishes ``firmware_started = 1``,
arms the 1 ms timer, processes any queued KCCB entries and enters ``WAIT``.

Dynamic TLB refill and flush (``src/vectors.S``)
================================================

``refill_main`` (``+0x000`` → ``+0xA00``) handles misses in ``0xC0xxxxxx``:

1. Save ``$1``, ``$8``–``$13`` in ``BD_SAVE``; increment ``BD_REFILL_COUNT``
   and scratch ``0x1A08``; record missed VA in ``0x1A10``.
2. Align VA to the even page of the pair and look up both PTEs in the host
   page table at ``0xCF000000`` (index ``(VA & 0x00FFF000) >> 12``). Any VA
   outside ``0xC0xxxxxx`` or with invalid PTE bit 1 is redirected to
   ``BD_FAULT_HI`` (the host fault page) and counted in ``BD_FAULT_COUNT``.
3. Pick slot ``BD_TLB_NEXT`` (round-robin ``5..15``), write remap entries
   ``slot`` and ``slot + 16``, and read back ``CORE_REVISION`` to drain
   posted writes.
4. Install the identity TLB entry with ``tlbwi``, restore registers and
   ``ERET``.

``tlb_flush_dynamic()`` runs from the unmapped KSEG0 exception page
(required by :exp:`E003`) whenever ``KCCB_MMUCACHE`` or ``KCCB_POW``
``OFF_REQ`` invalidates dynamic mappings, clearing TLB entries
``Wired..15`` and disabling their remap entries.

Fatal exceptions
================

Any exception other than a TLB refill or interrupt vectors to ``+0x180`` →
``general_main``:

1. Record ``Cause``, ``EPC`` and ``BadVAddr`` in ``BOOT_DATA`` and scratch
   registers ``0x1A18``/``0x1A20``/``0x1A28``.
2. On a nested fault, halt in a ``WAIT`` loop.
3. Otherwise switch to the top of stack with ``EXL/ERL/IE = 0`` and call
   ``fatal_exception()``, which sets ``HWR_FW_FAULT`` in
   ``sysdata.hwr_state_flags``, increments ``fw_faults``, emits trace
   ``DBG_4 (0xDEAD00cc, EPC, BadVAddr)`` and interrupts the host.

Debug scratch registers (``0x1A00``–``0x1A78``)
===============================================

``ROGUE_CR_SCRATCHn`` (``0x1A00 + 8n``) serve as host-readable telemetry
while the GPU power domain is active:

.. list-table::
   :header-rows: 1
   :widths: 22 28 50

   * - Register
     - Name
     - Meaning
   * - ``0x1A00``
     - ``CR_DBG_STAGE``
     - Boot progress: ``0xB007n``, then ``0xC0DE0004..6``
   * - ``0x1A08``
     - ``CR_DBG_REFILLS``
     - Dynamic TLB refill count
   * - ``0x1A10``
     - ``CR_DBG_LASTVA``
     - Last missed VA
   * - ``0x1A18`` / ``20`` / ``28``
     - ``CR_DBG_EXC_*``
     - Fatal exception ``Cause``, ``EPC``, ``BadVAddr`` (at runtime,
       ``0x1A18``/``20`` count CDM kicks and ``COMPUTE_FINISHED`` IRQs, and
       ``0x1A28`` records ``MTS_INTCTX``)
   * - ``0x1A30`` / ``38``
     - ``CR_DBG_UNITS_PDS`` / ``USC``
     - High words of PDS/USC exec bases read back after ``gpu_units_init``;
       later overwritten with BG ``EVENT_STATUS`` and ``MTS_BGCTX``
   * - ``0x1A40`` / ``48`` / ``50``
     - ``CR_DBG_IRQ_TIMER`` / ``BG`` / ``IRQ``
     - Counts of HW0 / HW1 / HW2 interrupts (:exp:`E016`)
   * - ``0x1A58``
     - ``CR_DBG_IRQ_LASTEV``
     - ``EVENT_STATUS`` seen by the last IRQ task
   * - ``0x1A60``
     - ``CR_DBG_EVENTS_SEEN``
     - Bitwise OR of all events handled by ``kick_events()``
   * - ``0x1A68``
     - ``CR_DBG_HWR_COUNT``
     - Completed hardware recoveries
   * - ``0x1A70``
     - ``CR_DBG_BG_WITH_EV``
     - BG tasks entered with a DM completion pending (:exp:`E018`)
   * - ``0x1A78``
     - ``CR_SCRATCH15``
     - Bitmask of currently busy data masters
