.. SPDX-License-Identifier: CC-BY-SA-4.0
.. Copyright (C) 2026 Thalles Campagnani

============
Architecture
============

The **PowerVR Rogue BXE-2-32** GPU (BVNC ``36.29.52.182`` — Branch 36,
Version 29, Number of scalable units 52, Config 182; single core ``MC1``,
Rogue XE family) contains an internal **MIPS32r2 / microMIPS**
microcontroller inside its MIPS wrapper (:dc:`overview`). The host CPU does
not program GPU rendering or compute registers directly: it submits command
rings in shared DRAM, and the firmware acts as the GPU's conductor —
resolving fences, activating memory contexts, programming unit registers,
handling completions, power transitions and hardware recovery.

Coprocessor control loop
========================

.. code-block:: text

   Host CPU (RISC-V)                  Firmware (MIPS in GPU)              GPU hardware
   -----------------                  ----------------------              ------------
   build commands in Client CCB
   write KICK into Kernel CCB ------> BG task (HW1) reads KCCB
   + poke ROGUE_CR_MTS_SCHEDULE       resolves Client CCB fences
                                      queues context on DM run list
                                      schedules DM ---------------------> programs registers,
                                                                          starts unit
                                      (GPU executes) <------------------- finishes -> EVENT_STATUS
                                      IRQ task (HW2) handles completion
                                      flushes DM caches, retires cmd,
                                      applies UFO updates
   receives IRQ 75 <----------------- write barrier + wrapper IRQ
   releases frame / wakes waiters

Target summary
==============

.. list-table::
   :header-rows: 1
   :widths: 28 72

   * - Item
     - Value
   * - GPU core
     - PowerVR Rogue BXE-2-32, BVNC ``36.29.52.182``, 1 core (``MC1``), XE
       family (``XE_ARCHITECTURE = 1``)
   * - SoC integration
     - SpacemiT K1 (8× X60 RISC-V), GPU MMIO at ``0xCAC00000`` (512 KiB),
       IRQ 75, under ``multimedia-bus`` (``dma-ranges`` mapping
       ``0x80000000 → 0x1_00000000``)
   * - Power domain
     - APMU GPU domain (``K1_PD_GPU``), control at APMU ``+0xD0``, status at
       APMU ``+0xF0`` bit 0
   * - Firmware core
     - MIPS32r2 in **microMIPS** mode, o32 ABI, soft-float, little-endian,
       ELF32 ``EM_MIPS`` (:doc:`mips-core`)
   * - Address widths
     - 32-bit MIPS VA, **36-bit** physical bus width, **40-bit** GPU VA
   * - System-level cache (SLC)
     - 16 KiB, 1 bank, 512-bit cache lines, configurable bypass
       (``SLC_SIZE_IN_KILOBYTES = 16``)
   * - Multi-core / XPU
     - Single core (``ROGUE_CR_MULTICORE_SYSTEM = 1``) with XPU register
       broadcast (``XPU_REGISTER_BROADCAST = 1``, ``XPU_MAX_SLAVES = 3``,
       ``XPU_MAX_REGBANKS_ADDR_WIDTH = 19``)

Complete BVNC feature, BRN and ERN inventory
============================================

Values carried in the redistributable open firmware image for BVNC
``36.29.52.182`` (:dc:`memory-model`, ``[public]`` / ``[spec]``) and encoded
in ``tools/mkfw.py`` (:doc:`../linux-driver/fw-container-and-abi`):

.. list-table::
   :header-rows: 1
   :widths: 36 64

   * - Feature / quirk
     - Value
   * - BRN
     - ``63553`` (36-bit MIPS bus workaround)
   * - ERN
     - ``35421``, ``38748``, ``42290``, ``42606``, ``47025``, ``57596``
   * - ``VIRTUAL_ADDRESS_SPACE_BITS``
     - ``40``
   * - ``PHYS_BUS_WIDTH``
     - ``36``
   * - ``SLC_SIZE_IN_KILOBYTES``
     - ``16`` (causes SLC bypass to be set at boot, :doc:`gpu-bringup`)
   * - ``SLC_BANKS`` / ``SLC_CACHE_LINE_SIZE_BITS``
     - ``1`` / ``512`` (``SLC_SIZE_CONFIGURABLE`` present)
   * - ``NUM_CLUSTERS`` / ``NUM_RASTER_PIPES`` / ``MAX_PARTITIONS``
     - ``1`` / ``1`` / ``8``
   * - ``NUM_ISP_IPP_PIPES`` / ``ISP_MAX_TILES_IN_FLIGHT``
     - ``2`` / ``2``
   * - ``ISP_SAMPLES_PER_PIXEL`` / ``TILE_SIZE_X`` / ``TILE_SIZE_Y``
     - ``1`` / ``16`` / ``16``
   * - ``COMMON_STORE_SIZE_IN_DWORDS``
     - ``13312`` (``USC_MIN_OUTPUT_REGISTERS_PER_PIX = 2``)
   * - ``NUM_OSIDS``
     - ``8``
   * - ``FBCDC`` / ``FBCDC_ALGORITHM`` / ``FBCDC_ARCHITECTURE``
     - ``50`` / ``50`` / ``7``
   * - ``SIMPLE_PARAMETER_FORMAT_VERSION``
     - ``2`` (``SIMPLE_INTERNAL_PARAMETER_FORMAT`` + ``_V2``)
   * - ``CDM_CONTROL_STREAM_FORMAT`` / ``LAYOUT_MARS`` / ``XE_ARCHITECTURE``
     - ``1`` / ``1`` / ``1``
   * - ``XPU_MAX_SLAVES`` / ``XPU_MAX_REGBANKS_ADDR_WIDTH`` / ``XPU_REGISTER_BROADCAST``
     - ``3`` / ``19`` / ``1``

Boolean features **present**:

``AXI_ACELITE``, ``COMPUTE``, ``COMPUTE_OVERLAP``, ``COREID_PER_OS``,
``GPU_MULTICORE_SUPPORT``, ``GPU_VIRTUALISATION``, ``GS_RTA_SUPPORT``,
``IRQ_PER_OS``, ``MIPS``, ``PBE2_IN_XE``, ``PBVNC_COREID_REG``, ``PERFBUS``,
``ROGUEXE``, ``SIMPLE_INTERNAL_PARAMETER_FORMAT``,
``SIMPLE_INTERNAL_PARAMETER_FORMAT_V2``, ``SLC_SIZE_CONFIGURABLE``,
``SOC_TIMER``, ``SYS_BUS_SECURE_RESET`` (required, :exp:`E001`),
``XE_MEMORY_HIERARCHY``, ``XE_TPU2``.

Features **absent** on BXE-2-32 (so their corresponding ``rogue_fwif_*``
fields are left unused by host and firmware):

``VDM_DRAWINDIRECT``, ``VDM_OBJECT_LEVEL_LLS``, ``TESSELLATION``,
``S7_TOP_INFRASTRUCTURE``, ``XT_TOP_INFRASTRUCTURE``, ``META*``
(``META_COREMEM_SIZE = 0``), ``ZLS_SUBTILE``, ``TPU_DM_GLOBAL_REGISTERS``,
``REQUIRES_FB_CDC_ZLS_SETUP``, ``COMPUTE_MORTON_CAPABLE``,
``CLUSTER_GROUPING``, ``CDM_USER_MODE_QUEUE``, ``FBC_MAX_*_DESCRIPTORS = 0``.

Data masters (DM)
=================

A **data master** is an independently queued GPU work unit indexed by the
public ``PVR_FWIF_DM_*`` enum (:dc:`data-masters`, :dc:`scheduler`):

.. list-table::
   :header-rows: 1
   :widths: 10 14 26 50

   * - Index
     - Enum
     - Unit
     - Role on BXE-2-32
   * - ``0``
     - ``GP``
     - General purpose / FW internal
     - Associated with MTS background context (DM0), MMU faults, GPIO and
       USC trigger events
   * - ``1``
     - ``TDM``
     - 2D / TDM transfer
     - Not used by mainline Mesa on BXE-2-32 (transfers run as ``TQ_3D`` on
       DM 3)
   * - ``2``
     - ``GEOM``
     - Geometry / Tiling Accelerator (TA / VDM)
     - Vertex and tiling pipeline backed by Parameter Manager context 0
       (:doc:`kicks-geometry-ta`)
   * - ``3``
     - ``FRAG``
     - Fragment / 3D pixel pipe (ISP / PBE)
     - Handles 3D renders (PM context 1), partial renders (``FRAG_PR``) and
       ``TQ_3D`` blits (:doc:`kicks-compute-3d-transfer`)
   * - ``4``
     - ``CDM``
     - Compute Data Master
     - Compute shaders (:doc:`kicks-compute-3d-transfer`)
   * - ``6``
     - ``GEOM2``
     - Second geometry pipe
     - Multicore only; unused on single-core BXE-2-32

Timers
======

* ``ROGUE_CR_TIMER`` (``0x0160``/``0x0164``): 48-bit GPU clock counter
  ticking at ``core_clock / 256`` (~1.2 MHz at 307.2 MHz, ~2.4 MHz at
  614.4 MHz). Used for trace timestamps, HWR timestamps, watchdog deadlines
  and host time correlation.
* MIPS CP0 ``Count`` / ``Compare``: 32-bit counter ticking at
  ``core_clock / 2`` — **128× faster** (``0x80``) than ``ROGUE_CR_TIMER``.
