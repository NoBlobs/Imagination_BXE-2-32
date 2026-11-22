.. SPDX-License-Identifier: CC-BY-SA-4.0
.. Copyright (C) 2026 Thalles Campagnani

============================
MIPS core execution model
============================

The internal GPU microcontroller is a **MIPS32r2** core operating in
**microMIPS** mode (:dc:`execution-environment`). This page describes the
hardware execution model, interrupt vectors, caches, barriers, performance
counters and USC breakpoint registers exposed to firmware.

ISA, ABI and reset vector
=========================

* **ISA**: MIPS32r2, **microMIPS** mode. Before releasing reset, the host
  configures ``ROGUE_CR_MIPS_WRAPPER_CONFIG`` (``0x0810``) with
  ``BOOT_ISA_MODE = MICROMIPS`` and ``REGBANK_BASE_ADDR = 0xCF80``
  (:exp:`E007`).
* **ABI**: o32, **soft-float** (no FPU), little-endian, ELF32 ``EM_MIPS``.
* **Reset / NMI vector**: ``0xBFC00000`` (KSEG1, uncached). Both cold boot
  and **warm boot** after runtime PM suspend enter at ``0xBFC00000``. On an
  NMI, the reference firmware saves MIPS state into the private area of
  ``BOOT_DATA`` before halting ``[spec]``.

Event-driven interrupt model
============================

There is **no main loop**: once boot completes, the MIPS core sits in
``WAIT`` and all work executes across **three vectored interrupt sources**:

.. list-table::
   :header-rows: 1
   :widths: 18 16 66

   * - Source
     - Vector (``EBase = 0x9FC02000``)
     - Responsibility
   * - **HW0 — timer** (``IM2``)
     - ``0x9FC02400``
     - ``CP0 Count/Compare`` expiry. In the reference firmware ``[spec]``
       sets a tick-pending flag and schedules a BG task; in our firmware
       runs periodic watchdog checks and backstop KCCB/event polling.
   * - **HW1 — BG task** (``IM3``, MTS)
     - ``0x9FC02500``
     - Raised by the Micro Task Scheduler (MTS) when host or firmware writes
       ``ROGUE_CR_MTS_SCHEDULE = 0`` (``0x0B00``). Processes Kernel CCB
       commands, HWR watchdogs, expired timers, wait-list re-evaluation and
       SPM state (:exp:`E016`).
   * - **HW2 — IRQ task** (``IM4``, MTS)
     - ``0x9FC02600``
     - Raised by the MTS on GPU events enabled in
       ``ROGUE_CR_MTS_DMn_INTERRUPT_ENABLE`` (or manually via
       ``ROGUE_CR_MTS_SCHEDULE = 0x20``). Reads core-local
       ``ROGUE_CR_EVENT_STATUS`` (``0x0130``) for DM completions,
       ``PM_OUT_OF_MEMORY``, ``MMU_PAGE_FAULT``, ``GPIO_REQ`` and
       ``USC_TRIGGER`` (:exp:`E018`).

CP0 interrupt configuration
---------------------------

* ``Config3.ISAOnExc = 1``: exception and interrupt handlers execute in
  microMIPS mode.
* ``Cause.IV = 1`` and ``IntCtl.VS = 8`` (256-byte vector spacing →
  ``+0x400`` / ``+0x500`` / ``+0x600``).
* ``Status.IM2..IM4`` enabled (mask ``0x1C00``).
* **Critical sections**: mask interrupts by clearing ``Status.IE``; each
  Kernel CCB command executes with ``IE = 0``.
* ``EXL`` **must be cleared before C handlers run** (:exp:`E002`): if a TLB
  miss happens while ``EXL = 1``, MIPS routes it to the general exception
  vector (``+0x180``) instead of the TLB refill vector (``+0x000``) and
  does not update ``EPC``.
* **End-of-task acknowledgement** ``[spec]`` / ``[exp E018]``: when a BG or
  IRQ task finishes, write ``CR[0x0B08]`` (``0`` for BG, ``2`` for IRQ) and
  **read it back** to enforce bus ordering. No ``MTS_INTCTX`` clear is
  needed.

CP0 timers vs. ``ROGUE_CR_TIMER``
=================================

Deadlines (HWR watchdogs, HCS context-switch timeouts, APM idle timer) are
tracked in units of ``ROGUE_CR_TIMER`` (``core_clock / 256``). To arm the
next hardware interrupt for ``deadline`` ticks ahead, the reference firmware
programs ``CP0 Compare = (deadline - now) * 0x80`` and clears ``CP0 Count``
(factor ``128`` = ``(core_clock / 2) / (core_clock / 256)``) ``[spec]``.

Caches and deterministic write barrier
======================================

MIPS caches ``[spec]``
----------------------

The reference firmware enables D-cache and I-cache and uses MIPS ``CACHE``
instructions for coherency:

* **D-cache write-back** (index operations over the whole cache) during
  power teardown and before publishing shared data.
* **D-cache hit-invalidate by address** over newly written Client CCB bytes
  on every ``KCCB_KICK`` (or whole cache for large ranges), so the MIPS
  does not read stale ring contents.
* **I-cache** primed at reset.

.. note::

   Our clean-room firmware sets ``Config.K0 = 2`` and maps every TLB entry
   uncached, avoiding cache-coherency hazards at the cost of extra bus
   traffic; see :doc:`../firmware/architecture`.

Write barrier (host ↔ firmware visibility)
------------------------------------------

Even with uncached memory, writes must drain out of the MIPS write buffer
and the wrapper before the host observes updated offsets or an interrupt
(:dc:`execution-environment`, :dc:`observed-sequences`):

1. Write ``1`` to a dedicated **uncached** word in the private ``BOOT_DATA``
   area (``0xBFC01800``).
2. Execute ``EHB`` (execution hazard barrier) followed by ``SYNC``.
3. Read the barrier word back.
4. Read ``ROGUE_CR_CORE_REVISION`` (``0x0020``) to flush posted bus writes.

Only after step 4 does the firmware publish a host-visible offset, increment
``osdata.interrupt_count[0]``, or write
``ROGUE_CR_MIPS_WRAPPER_IRQ_STATUS = 1`` (``0x08A8``).

Performance counters and USC breakpoints ``[spec]``
===================================================

* **CP0 performance counters**: ``PerfCtl0/1`` are configured from
  ``rogue_fwif_sysinit.firmware_perf`` and ``PerfCnt0/1`` can be sampled for
  HWPerf (optional on mainline).
* **USC shader breakpoints** (XPU broadcast, no public names):

  .. list-table::
     :header-rows: 1
     :widths: 22 78

     * - Offset
       - Role
     * - ``0x4030``
       - Shader breakpoint address (from ``fwmemcontext.breakpoint_addr``)
     * - ``0x4038``
       - Breakpoint handler address (``bp_handler_addr``)
     * - ``0x4040``
       - Breakpoint control: bits 2:0 slot/DM, bit 3 enable
         (``breakpoint_ctl``)
     * - ``0x4050`` / ``0x06D8``
       - Over-allocation temp-register (``| 0x2000``) and shared-register
         counts configured at GPU units init (:doc:`gpu-bringup`)

  Configured per memory context via KCCB ``BP`` (not sent by mainline
  ``powervr``).
