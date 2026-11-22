.. SPDX-License-Identifier: CC-BY-SA-4.0
.. Copyright (C) 2026 Thalles Campagnani

============
GPU bring-up
============

Every time the GPU power domain is turned on — cold boot, warm boot after
runtime PM suspend, or after ``SOFT_RESET`` during HWR — the firmware must
program the GPU-side registers before the first kick of any data master
(:dc:`boot-gpu-init`, verified on silicon in :exp:`E013`). Omitting the
execution bases or unit defaults causes immediate MMU faults or hung data
masters (:exp:`E006`, :exp:`E012`).

Unless noted, writes in section A go through the XPU broadcast bank
(``BCAST[off]`` at ``0xCFA00000 + off``); ``CR[off]`` targets the primary
core bank (``0xCF800000 + off``).

A. GPU units initialisation (before any kick)
=============================================

Order matters wherever a poll is indicated:

1. **Clock gating**: if ``ROGUE_FWIF_INICFG_DISABLE_CLKGATING_EN`` is set,
   force unit clocks on; otherwise leave clock gating enabled (mainline
   leaves ``CLK_CTRL = 0x002AAA002A22AAAA``, all units ``AUTO``;
   :exp:`E007`).
2. **Dust / unit power** (see section D below): power on
   ``runtime_cfg.default_dusts_num_init`` dusts (``1`` on BXE-2-32) via
   ``0x0050`` and ``0x0038`` and wait for ``EVENT_STATUS.POWER_COMPLETE``.
3. **SLC bypass** (16 KiB SLC ≤ 32 KiB): set bypass on
   ``ROGUE_CR_SLC_CTRL_BYPASS`` (``0x3828``): low word ``|= 0x01000000``,
   high word ``|= 0x2000``.
4. **Unit reset pulse**: ``BCAST[0x6000] = 1``, then ``BCAST[0x6000] = 0``.
5. **PDS and USC execution bases** (64-bit, low word first):

   .. list-table::
      :header-rows: 1
      :widths: 32 30 38

      * - Register pair
        - Value
        - Notes
      * - ``BCAST[0x0610]`` / ``0x0614``
        - ``sysinit.pds_exec_base``
        - PDS code heap base (bits 39:20 writable; :exp:`E009`)
      * - ``BCAST[0x4008]`` / ``0x400C``
        - ``sysinit.usc_exec_base``
        - USC execution base pair 1 (:exp:`E013`)
      * - ``BCAST[0x4010]`` / ``0x4014``
        - ``sysinit.usc_exec_base``
        - USC execution base pair 2 (:exp:`E013`)
      * - ``BCAST[0x4028]`` / ``0x402C``
        - ``sysinit.usc_exec_base``
        - USC execution base pair 3 (:exp:`E013`)

   Without ``0x0610`` the CDM faults at VA ``0x26C0`` (:exp:`E006`); without
   all three USC pairs the CDM runs forever without completing (:exp:`E012`,
   :exp:`E013`). ``0x0FC8`` is **not** a USC base — it is an ISP register
   written by fragment/TQ kicks.

6. **Fixed block of FW-only unit defaults** (no public names; required for
   USC, TPU, MCU and VDM units, ``[spec]`` + :exp:`E013`):

   .. list-table::
      :header-rows: 1
      :widths: 44 56

      * - Register(s)
        - Required value
      * - ``BCAST[0x18C0]``, ``[0x18C8]``, ``[0x18D0]``, ``[0x18E0]``
        - ``0x007FC000``
      * - ``BCAST[0x0600]``
        - ``0x403F3F00``
      * - ``BCAST[0x0604]``
        - ``0x00800000``
      * - ``BCAST[0x4050]``
        - over-allocation temp-register count ``| 0x2000`` (``0x2000`` when
          none)
      * - ``BCAST[0x06D8]``
        - over-allocation shared-register count (``0`` when none)
      * - ``BCAST[0x0720]``
        - ``0x0000000F``
      * - ``BCAST[0x0640]`` / ``[0x0698]`` / ``[0x0630]`` / ``[0x0658]`` /
          ``[0x06C0]`` / ``[0x0648]`` / ``[0x0608]``
        - ``0x9`` / ``0xB`` / ``0x1`` / ``0xB`` / ``0xD`` / ``0x1`` / ``0x1``
      * - ``BCAST[0x18E8]``, ``[0x18F0]``, ``[0x18F8]``, ``[0x1900]``
        - ``0xF`` (open variant)
      * - ``BCAST[0x1498]``, ``[0x0700]``
        - ``1``
      * - ``BCAST[0x0358]``
        - ``5``
      * - ``BCAST[0x20F8]`` / ``[0x2010]`` / ``[0x2100]``
        - ``1`` / ``0`` / ``0``
      * - ``BCAST[0x13F8]``
        - ``7``

   .. note::

      ``[spec]`` Q16i: on the reference firmware, registers
      ``0x1500 = 0x80``, ``0x1780 = 0x21``, ``0x1788 = 0x20``,
      ``0x178C = 0x20`` and ``0x0FA0 = 0x88`` also change alongside the unit
      defaults at the first load.

7. **Init-done handshakes** (both polls must succeed before any kick):

   * ``BCAST[0x4068] = 1``, then poll ``0x4068`` until bit 0 = ``0``;
   * ``BCAST[0x0660] = 1`` and ``BCAST[0x0670] = 1``;
   * ``BCAST[0x0CD8] = 1``, then poll ``0x0CD8`` until bit 0 = ``0``.

8. **TPU filter flags**: ``BCAST[0x17B8] = sysinit.filter_flags``.
9. **Multicore index registers** (``0xF318`` / ``0xF338``, no public names):
   not needed on single-core BXE-2-32.
10. Emit trace ``MAIN_69`` (``GPU Units init (# mask: …)``).

B. Event routing to MTS IRQ task (HW2)
======================================

GPU events in ``ROGUE_CR_EVENT_STATUS`` reach the firmware's HW2 IRQ task
solely through the per-DM ``ROGUE_CR_MTS_DMn_INTERRUPT_ENABLE`` registers —
**not** through ``ROGUE_CR_EVENT_ENABLE`` (:dc:`boot-gpu-init`, :exp:`E011`,
:exp:`E018`):

.. list-table::
   :header-rows: 1
   :widths: 36 14 50

   * - Register
     - Value
     - Routed ``EVENT_STATUS`` bits
   * - ``ROGUE_CR_EVENT_CLEAR`` (``0x0138``)
     - ``0x41FC``
     - Clear any stale pending events first
   * - ``MTS_DM0_INTERRUPT_ENABLE`` (``0x0B58``)
     - ``0x89200``
     - ``MMU_PAGE_FAULT`` (b9), ``GPIO_REQ`` (b12), ``USC_TRIGGER`` (b15),
       ``SLAVE_REQ`` (b19)
   * - ``MTS_DM2_INTERRUPT_ENABLE`` (``0x0B68``)
     - ``0xA0``
     - ``TA_FINISHED`` (b5), ``PM_OUT_OF_MEMORY`` (b7)
   * - ``MTS_DM3_INTERRUPT_ENABLE`` (``0x0B70``)
     - ``0x08``
     - ``PIXELBE_END_RENDER`` (b3)
   * - ``MTS_DM4_INTERRUPT_ENABLE`` (``0x0B78``)
     - ``0x04``
     - ``COMPUTE_FINISHED`` (b2)
   * - ``ROGUE_CR_EVENT_ENABLE`` (``0x0128``)
     - ``0``
     - **Must stay 0 on single-core MIPS firmware** (:exp:`E011`)

Why ``EVENT_ENABLE`` stays 0 on BXE-2-32
----------------------------------------

``ROGUE_CR_EVENT_ENABLE`` drives the GPU's external event-IRQ line to the
host, which mainline ``powervr`` does not service (causing ``nobody cared``
IRQ storms if set; :exp:`E011`). The reference firmware only writes
``EVENT_ENABLE = 0x8EAC`` on multicore systems
(``ROGUE_CR_MULTICORE_SYSTEM >= 2``) to propagate events from secondary
cores ``[spec]``.

MTS wrapper setup and event consumption
---------------------------------------

* ``ROGUE_CR_MTS_GARTEN_WRAPPER_CONFIG`` (``0x0B50``) is configured by the
  **host** (``IDLE_CTRL_META``), not the firmware.
* ``ROGUE_CR_MTS_SCHEDULE_ENABLE`` (``0x0BC8``) is left at its reset value
  by both host and firmware.
* There is no "which DM interrupted" register: the HW2 handler reads the
  **core-local** ``CR[0x0130]`` (``EVENT_STATUS``), dispatches each set bit
  (b1 ``KERNEL_FINISHED``, b2 ``COMPUTE_FINISHED``, b3
  ``PIXELBE_END_RENDER``, b5 ``TA_FINISHED``, b7 ``PM_OUT_OF_MEMORY``, b9
  ``MMU_PAGE_FAULT``), clears handled bits in ``CR[0x0138]``, and ends the
  task with ``CR[0x0B08] = 2`` + read-back.

C. MTS context association and power-on setup
=============================================

.. list-table::
   :header-rows: 1
   :widths: 40 14 46

   * - Register
     - Value
     - Meaning
   * - ``ROGUE_CR_XPU_BROADCAST`` (``0x0890``)
     - ``1``
     - Enable primary core in broadcast mask
   * - ``ROGUE_CR_SYS_BUS_SECURE`` (``0xA100``)
     - ``0``
     - Clear secure bus bit, then read back
   * - ``ROGUE_CR_BIF_FAULT_READ`` (``0x13E0``)
     - ``fault_phys_addr``
     - Dummy page for faulting GPU reads
   * - ``MTS_INTCTX_THREAD0_DM_ASSOC`` (``0x0B40``)
     - ``0xFFFF``
     - IRQ context serves all DMs (cleared by host on FW stop)
   * - ``MTS_BGCTX_THREAD0_DM_ASSOC`` (``0x0B30``)
     - ``1``
     - BG context serves DM0
   * - ``MIPS_WRAPPER_IRQ_ENABLE`` (``0x08A0``)
     - ``1``
     - Enable wrapper host-IRQ generation

After units init and scheduler init complete, the firmware writes
``sysinit.firmware_started_timestamp = ROGUE_CR_TIMER``, executes a write
barrier, and sets ``sysinit.firmware_started = 1`` (polled by the host with a
5 s timeout).

D. Dust / power-island control
==============================

While the host controls the external SoC power domain (APMU ``K1_PD_GPU``),
internal GPU dust gating is controlled by the firmware (:dc:`boot-gpu-init`,
:exp:`E013`):

1. ``BCAST[0x0050] = (1 << num_dusts) - 1`` (8-bit active-dust mask).
2. ``CR[ROGUE_CR_XPU_BROADCAST] = 1``.
3. ``CR[0x0038] = req`` (bit 0 = ``1`` on, ``0`` off), then
   ``CR[0x0038] = req | 2`` (bit 1 = request pulse).
4. Poll ``ROGUE_CR_EVENT_STATUS`` for ``POWER_COMPLETE`` (bit 10) or
   ``POWER_ABORT`` (bit 11).
5. Restore ``CR[ROGUE_CR_XPU_BROADCAST] = 1``.

When ``ROGUE_FWIF_INICFG_POW_RASCALDUST`` (bit 4) is set, the reference
firmware gates dusts on and off around every job (traces ``POW_9`` and
``POW_7``); mainline ``powervr`` does not enable ``POW_RASCALDUST``, so
powering dusts on at boot and after HWR is sufficient (:exp:`E013`).
