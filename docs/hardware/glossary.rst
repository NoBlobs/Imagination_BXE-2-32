.. SPDX-License-Identifier: CC-BY-SA-4.0
.. Copyright (C) 2026 Thalles Campagnani

========
Glossary
========

Acronyms and terms used across the PowerVR Rogue BXE-2-32 hardware, driver
and firmware documentation (:dc:`glossary`).

.. list-table::
   :header-rows: 1
   :widths: 24 76

   * - Term
     - Meaning
   * - **APM**
     - *Active Power Management*: power the GPU domain off when idle and
       restart via warm boot when work arrives (:doc:`power-hwr-and-events`).
   * - ``BCAST[off]``
     - Register write through the XPU broadcast bank (``0xCFA00000 + off``).
   * - **BIF**
     - Bus Interface / GPU MMU: translates 40-bit GPU VAs through page
       catalogues (``ROGUE_CR_BIF_CAT_BASE0..7``).
   * - **BVNC**
     - Core identifier: Branch.Version.Number-of-scalable-units.Config
       (``36.29.52.182`` for BXE-2-32).
   * - **CCB**
     - Circular Command Buffer: shared-memory command ring with
       ``write_offset``, ``read_offset`` and ``wrap_mask``.
   * - **cCCB / Client CCB**
     - Per-context work ring carrying tasks (GEOM, FRAG, TQ_3D, CDM),
       fences and UFO updates.
   * - **CDM**
     - Compute Data Master (DM 4): executes Vulkan compute shaders.
   * - ``CR[off]``
     - GPU control register at offset ``off`` in the primary core bank
       (``0xCF800000 + off``).
   * - **CSW**
     - Context Switch (*context store / resume*): saving and restoring a
       running context for higher-priority preemption.
   * - **DM**
     - Data Master: independently queued GPU work unit indexed by
       ``PVR_FWIF_DM_*`` (0 GP, 1 TDM, 2 GEOM, 3 FRAG, 4 CDM, 6 GEOM2).
   * - **DVFS**
     - Dynamic Voltage and Frequency Scaling (``KCCB_CORECLKSPEEDCHANGE``).
   * - **FWCCB**
     - Firmware CCB: firmware-to-host asynchronous notification ring.
   * - **FW common context**
     - ``rogue_fwif_fwcommoncontext``: firmware state for one work stream
       (one cCCB, one DM).
   * - **FW memory context**
     - ``rogue_fwif_fwmemcontext``: one GPU virtual address space (page
       catalogue physical address and BIF slot).
   * - **HCS**
     - Hard Context Switch: watchdog forcing a switch if a context store
       does not finish in time.
   * - **HWPerf**
     - Hardware performance-counter packet stream (optional / unused by
       mainline ``powervr``).
   * - **HWR**
     - Hardware Recovery: detects MMU faults, poll failures or lockups and
       resets the GPU pipelines without rebooting the MIPS core.
   * - **HWRTData**
     - ``rogue_fwif_hwrtdata``: hardware render-target data shared between a
       geometry pass and its fragment pass.
   * - **IPF**
     - ISP Parameter Fetcher: reads region headers and control streams
       during fragment rendering (``TAG_ID 0x25`` in BIF fault status).
   * - **ISP**
     - Image Synthesis Processor: rasterisation, depth/stencil (ZLS) and
       tile scheduling inside the 3D pixel pipe.
   * - **KCCB**
     - Kernel CCB: host-to-firmware command ring with parallel return slots.
   * - **MTS**
     - Micro Task Scheduler: hardware block delivering BG (HW1) and IRQ
       (HW2) tasks to the MIPS core as interrupts.
   * - **NMI**
     - Non-Maskable Interrupt; enters through the MIPS reset vector
       ``0xBFC00000``.
   * - **OSID**
     - OS / guest identifier for GPU virtualisation; each OSID gets its own
       16 MiB firmware heap window.
   * - **PB**
     - Parameter Buffer: geometry memory dynamically allocated from
       freelists by the PM.
   * - **PBE**
     - Pixel Back End: formats and emits rendered tiles to memory via
       end-of-tile pixel-event PDS/USC programs.
   * - **PDS**
     - Programmable Data Sequencer: launches USC shader tasks using offsets
       relative to ``PDS_EXEC_BASE`` (``0x0610``).
   * - **PM**
     - Parameter Manager: hardware allocator managing local and global
       freelists, ALIST/MLIST stack pointers and PM page catalogues.
   * - **PR**
     - Partial Render (``ROGUE_FWIF_CCB_CMD_TYPE_FRAG_PR``): mid-frame 3D
       render triggered by SPM to reclaim PB pages on OOM.
   * - **SFID**
     - String Format ID: 32-bit trace entry header encoding parameter count,
       log group and message ID (``pvr_rogue_fwif_sf.h``).
   * - **SLC**
     - System Level Cache (16 KiB, 1 bank on BXE-2-32).
   * - **SPM**
     - Smart Parameter Management: out-of-memory / freelist-grow /
       partial-render subsystem.
   * - **TA**
     - Tiling Accelerator: the geometry pipeline (VDM + TE + PPP, DM 2).
   * - **TQ**
     - Transfer Queue: blit/resolve operations (``TQ_3D`` via the 3D pipe on
       BXE-2-32).
   * - **UFO**
     - Updated Fence Object (``{addr, value}``) used for cCCB fences,
       timeline ``FENCE_PR``, updates and sync checkpoints.
   * - **USC**
     - Unified Shading Cluster: programmable shader execution cores (offsets
       relative to ``0x4008`` / ``0x4010`` / ``0x4028``).
   * - **VDM**
     - Vertex Data Master: front-end control-stream processor of the
       geometry pipeline.
   * - **XPU broadcast**
     - Write through ``0xCFA00000`` reaching all cores enabled in
       ``ROGUE_CR_XPU_BROADCAST``.
   * - **ZLS**
     - Depth (Z) / Stencil Load-Store unit in the ISP (``0x0F48``–``0x0F6C``).
