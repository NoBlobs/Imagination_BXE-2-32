.. SPDX-License-Identifier: CC-BY-SA-4.0
.. Copyright (C) 2026 Thalles Campagnani

=========
Registers
=========

Master register map of every RGX control register accessed or probed on the
BXE-2-32 (``36.29.52.182``), with provenance:

* **P**: public ``pvr_rogue_cr_defs.h`` (upstream ``drm/imagination``) or
  public Mesa ``rogue_cr.xml``
* **S** / **O** / **D**: clean-room specification (:dc:`registers`,
  :dc:`boot-gpu-init`, :dc:`kick-geometry-ta`, :dc:`kick-compute-and-3d`)
* **E**: established by black-box board experiment (:lr:`docs/logRefs/ <>`)

Access rules:

* primary core bank at ``0xCF800000 + offset`` (wired TLB entry 2; core *n*
  at ``0xCF800000 + n * 0x80000``);
* XPU broadcast bank at ``0xCFA00000 + offset`` (wired TLB entry 4,
  ``bcast_write*``);
* 64-bit registers are written as two 32-bit stores, **low word first**.

Core, MTS, BIF, SLC, timer and debug registers
==============================================

.. list-table::
   :header-rows: 1
   :widths: 16 28 8 48

   * - Offset
     - Name
     - Src
     - Use
   * - ``0x0000`` / ``0x0080``
     - ``CLK_CTRL`` / ``CLK_XTPLUS_CTRL``
     - P
     - Unit clock gating (host leaves ``CLK_CTRL = 0x002AAA002A22AAAA``,
       AUTO; :exp:`E007`)
   * - ``0x0018`` / ``0x0020``
     - ``CORE_ID`` / ``CORE_REVISION``
     - P
     - ``CORE_REVISION`` = ``36.29.52.182``; dummy read drains posted writes
   * - ``0x0038`` / ``0x0050``
     - Power-island request / active dusts
     - S
     - ``0x0050 = (1 << n) - 1``; ``0x0038`` bit 0 on/off, bit 1 pulse
       (:doc:`gpu-bringup`)
   * - ``0x0100`` / ``0x0104``
     - ``SOFT_RESET``
     - P, S
     - HWR pulse ``0x13FFFC1D`` / ``0xFFEFFE00`` → ``0`` (:exp:`E019`)
   * - ``0x0128``
     - ``EVENT_ENABLE``
     - E, S
     - Routes events to **host** IRQ; must stay ``0`` on single-core MIPS
       (:exp:`E011`)
   * - ``0x0130`` / ``0x0138``
     - ``EVENT_STATUS`` / ``EVENT_CLEAR``
     - P
     - b1 ``KERNEL_FINISHED``, b2 ``COMPUTE_FINISHED``, b3
       ``PIXELBE_END_RENDER``, b5 ``TA_FINISHED``, b6 ``TA_TERMINATE``,
       b7 ``PM_OUT_OF_MEMORY``, b8 ``PM_3D_MEM_FREE``, b9
       ``MMU_PAGE_FAULT``, b10 ``POWER_COMPLETE``, b11 ``POWER_ABORT``,
       b12 ``GPIO_REQ``, b13 ``GPIO_ACK``, b14 ``ZLS_FINISHED``,
       b15 ``USC_TRIGGER``
   * - ``0x0150`` / ``0x0158``
     - GPIO input / ack output
     - S
     - 8-bit GPIO input and handshake ack (re-arm at ``0x0B90``)
   * - ``0x0160`` / ``0x0164``
     - ``TIMER``
     - P
     - 48-bit GPU timer (``core_clock / 256``)
   * - ``0x0810``
     - ``MIPS_WRAPPER_CONFIG``
     - P
     - ``BOOT_ISA_MODE = MICROMIPS``, ``REGBANK_BASE_ADDR = 0xCF80``
   * - ``0x0818..0x0860``
     - ``MIPS_ADDR_REMAP1..5_CONFIG1/2``
     - P
     - Host fixed remaps for boot code, boot data, exception page
   * - ``0x0878`` / ``0x087C``
     - ``MIPS_ADDR_REMAP_RANGE_CONFIG``
     - P
     - 32 dynamic remap entries (write low, high, read back high)
   * - ``0x0890``
     - ``XPU_BROADCAST``
     - P
     - Broadcast core mask (``1`` on single-core BXE-2-32)
   * - ``0x08A0`` / ``0x08A8``
     - ``MIPS_WRAPPER_IRQ_ENABLE`` / ``_STATUS``
     - P
     - Enable (``1`` at init) and raise host IRQ 75 (write ``1``)
   * - ``0x08D0`` / ``0x08F0`` / ``0x08F8``
     - ``MIPS_EXCEPTION_STATUS`` / XPU events / ``MARS_IDLE``
     - P, S
     - Wrapper exception status, multicore XPU events, MARS idle status
   * - ``0x0B00`` / ``0x0B08``
     - ``MTS_SCHEDULE`` / MTS task end
     - P, S
     - Schedule BG (``0``) or IRQ (``0x20``); end BG (``0``) or IRQ (``2``)
       + read-back
   * - ``0x0B30`` / ``0x0B40``
     - ``MTS_BGCTX`` / ``INTCTX_THREAD0_DM_ASSOC``
     - P
     - BG = ``1`` (DM0), INT = ``0xFFFF``
   * - ``0x0B50`` / ``0x0BC8``
     - ``MTS_GARTEN_WRAPPER_CONFIG`` / ``SCHEDULE_ENABLE``
     - P
     - Host-set Garten wrapper; ``SCHEDULE_ENABLE`` left at reset
   * - ``0x0B58 + 8n``
     - ``MTS_DMn_INTERRUPT_ENABLE``
     - P, S
     - DM0 ``0x89200``, DM2 ``0xA0``, DM3 ``0x08``, DM4 ``0x04``
       (:exp:`E016`, :exp:`E018`)
   * - ``0x0B90``
     - GPIO event re-arm / mask
     - S
     - Write ``0x1000`` after GPIO handshake
   * - ``0x1200 + 8n``
     - ``BIF_CAT_BASEn``
     - P
     - Page catalogue PA of BIF slot ``n`` (slot 0 FW, slots 1–7 clients)
   * - ``0x1240``
     - ``BIF_CAT_BASE_INDEX``
     - P
     - DM → slot (TA b0, 3D b8, CDM b16, TDM b19)
   * - ``0x1248``–``0x1280``
     - ``BIF_PM_CAT_BASE_{VCE,TE,ALIST}{0,1}``
     - P
     - PM page catalogues for PM contexts 0 and 1
   * - ``0x12A0``
     - ``BIF_CTRL_INVAL``
     - P
     - MMU invalidation: b0 ``PT``, b1 ``PD``, b2 ``PC``, b3 ``TLB1``
   * - ``0x12B0`` / ``0x12B8``
     - ``BIF_FAULT_BANK0_MMU`` / ``REQ_STATUS``
     - P
     - Fault address, catalogue slot and XE ``TAG_ID``/``TAG_SB`` requestor
   * - ``0x1348`` / ``0x1350``
     - PM TLB inval / ``BIFPM_STATUS_MMU``
     - S, P
     - PM TLB invalidation; outstanding PM MMU requests
   * - ``0x13E0``
     - ``BIF_FAULT_READ``
     - P
     - ``sysinit.fault_phys_addr`` (dummy fault read page)
   * - ``0x1740``
     - ``MCU_FENCE``
     - P
     - Completion fence (``0x180_00000000`` PIXEL, ``0x280_00000000`` CDM)
   * - ``0x1A00``–``0x1A78``
     - ``SCRATCHn``
     - P
     - Debug mailboxes (:doc:`../firmware/boot-and-runtime`)
   * - ``0x3818`` / ``0x3820`` / ``0x3828``
     - ``SLC_CTRL_FLUSH_INVAL`` / ``STATUS0`` / ``BYPASS``
     - P
     - SLC flush/inval (poll ``STATUS0`` bit 2) and 16 KiB SLC bypass
   * - ``0x6008..0x6030``
     - ``PERF_{TA,3D,COMPUTE}_{PHASE,CYCLE}``
     - P
     - Pipeline phase and cycle counters (lockup detection)
   * - ``0x17B0``, ``0x6300`` / ``0x6330``
     - Power monitor / energy counters
     - S
     - Per-core energy counter registers
   * - ``0xA100``
     - ``SYS_BUS_SECURE``
     - P
     - Written ``0`` at power-on; needs ``SYS_BUS_SECURE_RESET`` (:exp:`E001`)
   * - ``0xF308`` / ``F310`` / ``F320`` / ``F330``
     - ``MULTICORE_SYSTEM`` / ``{FRAG,GEOM,CDM}_CTRL_COMMON``
     - P, S
     - Multicore system & per-DM control (``F318``/``F338`` core indices)

Compute, 3D/TQ, geometry, PM, USC and flush registers
=====================================================

Detailed per-phase programming tables live in:

* :doc:`gpu-bringup` — ``0x0610`` (``PDS_EXEC_BASE``), ``0x4008``/``4010``/``4028``
  (USC exec bases), unit defaults (``0x0600..0x0720``, ``0x18C0..0x1900``,
  ``0x1498``, ``0x0358``, ``0x20F8``, ``0x2010``, ``0x2100``, ``0x13F8``,
  ``0x4050``, ``0x06D8``), init polls (``0x4068``, ``0x0CD8``), ``0x17B8``.
* :doc:`mips-core` — USC shader breakpoints (``0x4030``, ``0x4038``, ``0x4040``).
* :doc:`parameter-manager-and-spm` — Local freelist (``0x0200``, ``0x0208``,
  ``0x0210..0x0234``, ``0x0348``/``0x0350``, ``0x0390..0x03A8``,
  ``0x2000``/``0x2008``), global freelist banks (``0x2078..0x20E0``, load
  cmds ``0x20A8``/``0x20B0``), ALIST/MLIST SPs & load cmds
  (``0x0270..0x0288``, ``0x02C0..0x02D0``, ``0x0190``), ``0x0240``–``0x0258``,
  ``0x02A0``/``0x02A8``, ``0x02B0``, ``0x02D8``/``0x02E0``, ``0x0338``,
  ``0x0380``/``0x0388``/``0x03E0``, ``0x03D0``/``0x03D4``.
* :doc:`kicks-geometry-ta` — VDM/TE/PPP geometry registers
  (``0x0400``–``0x0428``, ``0x0C08``–``0x0CE0``, ``0x0D00``, ``0x0D20``,
  ``0x17A0``, ``0x40A8``).
* :doc:`kicks-compute-3d-transfer` — CDM (``0x0478``–``0x04C0``, ``0x17A8``),
  ISP/ZLS/MSAA/PBE/PDS (``0x0618``–``0x0628``, ``0x06A0``–``0x06BC``,
  ``0x0F00``–``0x0FD8``, ``0x1000``, ``0x1020``, ``0x1510``–``0x1588``,
  ``0x3E40``, ``0x4070``–``0x40A0``), and completion flush
  (``0x4000``, ``0x0688``, ``0x0668``, ``0x1608``, ``0x1480``/``1488``,
  ``0x1720``/``1728``).

Values left by the host after boot (``E007``)
=============================================

From the read-only register survey with the clean firmware idle
(:exp:`E007`, raw dump :lr:`data/survey-0-2000.txt`):

.. list-table::
   :header-rows: 1
   :widths: 18 24 58

   * - Offset
     - Value
     - Note
   * - ``0x0000``
     - ``0x002AAA002A22AAAA``
     - ``CLK_CTRL``: host leaves every unit in AUTO
   * - ``0x0018``
     - ``0x08470000``
     - ``CORE_ID``
   * - ``0x0020``
     - ``0x0024001D003400B6``
     - ``CORE_REVISION`` = ``36.29.52.182``
   * - ``0x0080``
     - ``0x0000000800000000``
     - ``CLK_XTPLUS_CTRL``
   * - ``0x0810``
     - ``0x0001CF80``
     - ``MIPS_WRAPPER_CONFIG`` (microMIPS, reg bank ``0xCF80``)
   * - ``0x0818..0x0860``
     - —
     - ``MIPS_ADDR_REMAP1..5_CONFIG1/2``: host fixed remaps
   * - ``0x08D0``
     - ``0x20``
     - ``MIPS_EXCEPTION_STATUS`` while running
   * - ``0x08F8``
     - ``0x6``
     - ``MARS_IDLE``
   * - ``0x1490``
     - ``0x4CD33``
     - No public name
   * - ``0x18C0..0x18D0``
     - ``0x7FFFFF``
     - Reset default (overwritten to ``0x7FC000`` at GPU units init)

Writable-bit map (black-box probe, ``E008``)
============================================

:exp:`E008` wrote all-ones to bits 63:4 of every register in ``0x0200–0x0800``
and ``0x0C00–0x3000`` (outside a deny list), read the value back and restored
the original:

.. list-table::
   :header-rows: 1
   :widths: 24 26 50

   * - Mask shape
     - Meaning
     - Offsets
   * - ``0x000000FF_FFFFFFF0``
     - 40-bit address, 16-byte aligned
     - ``0x210 0x218 0x240 0x248 0x2D8 0x2E0 0x408 0x428 0x480* 0x498* 0xC30 0xCC0 0xF20 0xF50–0xF78 0xFB0–0xFC0 0x2018 0x2040 0x2078 0x20B8 0x2108 0x2110``
   * - ``0x000000FF_FFFFFFC0``
     - 40-bit, 64-byte aligned
     - ``0xC38 0xC40 0xCB0``
   * - ``0x000000FF_FFF00000``
     - **40-bit, 1 MiB aligned**
     - ``0x0610`` (unique — ``PDS_EXEC_BASE``, :exp:`E009`)
   * - ``0x000000FC_00000000``
     - Bits 39:34 only (16 GiB granule)
     - ``0x0FC8`` (unique — ISP register, :exp:`E012`)
   * - ``0x0000003F_FFFFFFF0``
     - 38-bit, 16-byte aligned
     - ``0x1788 0x1790 0x17A0 0x17A8*``
   * - ``0x000007FF_FFFFFFE0``
     - 43-bit, 32-byte aligned
     - ``0x1740`` (``MCU_FENCE``)

``*`` marks registers whose role was already known prior to the probe. Side
effect observed: ``0x0C50`` bit 30 is sticky/self-setting and clears on the
next GPU soft reset.
