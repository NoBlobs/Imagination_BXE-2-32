.. SPDX-License-Identifier: GPL-3.0-or-later
.. Copyright (C) 2026 Thalles Campagnani

=========================================================
Imagination BXE-2-32 — free GPU firmware: documentation
=========================================================

This is the project documentation for the free (GPLv3, GNU C23) firmware that
runs on the internal MIPS microcontroller of the **Imagination Rogue BXE-2-32** GPU
(BVNC ``36.29.52.182``) in the **SpacemiT K1** SoC. It is a clean-room
implementation running under the unmodified mainline Linux ``powervr`` driver
(apart from the experimental BVNC entry and K1 SoC enablement) and the Mesa
``pvr`` Vulkan driver.

The documentation is written directly from the implementation in ``src/``,
the host integration on the ``IMG-BXE-2-32`` Linux branch, and black-box
board experiments recorded in :lr:`docs/logRefs/ <>`. It complements — and
never modifies — the read-only clean-room specification in
:dc:`docsClean/ <README>`.

Clean-room sources used by the clean team:

* the clean specification in :dc:`docsClean/ <README>`;
* the public upstream Linux ``drm/imagination`` headers and driver;
* public Mesa (``pvr`` Vulkan driver and ``rogue_cr.xml``);
* public MIPS32r2 / microMIPS architecture documents;
* black-box experiments on the board with **this** firmware
  (:lr:`docs/logRefs/ <>`).

Gaps in the specification are sent back as questions in ``docs/questions.md``
or investigated through experiments on our own firmware.

.. toctree::
   :maxdepth: 2
   :caption: Contents

   hardware/index
   linux-driver/index
   firmware/index
   firmware-source/index

Current status
==============

Board: Banana Pi BPI-F3 (SpacemiT K1), kernel ``7.4.0-rc1-img`` (branch
``IMG-BXE-2-32``), mainline ``powervr``, Mesa ``pvr``.

.. list-table::
   :header-rows: 1
   :widths: 32 68

   * - Area
     - State
   * - Boot, MIPS TLB/remap, ``firmware_started`` handshake
     - Works (``FW version v1.0 (build 1 OS)``,
       ``[drm] Initialized powervr 1.0.0 for cac00000.gpu``)
   * - GPU bring-up (``gpu_init.c``)
     - Works: dust power, SLC bypass, PDS/USC exec bases, unit defaults,
       init handshakes, MTS association (:exp:`E013`)
   * - Kernel CCB, FWCCB, host IRQ, trace buffer
     - Works: BG task per KCCB batch (:exp:`E016`), host-decoded trace in
       debugfs
   * - Runtime PM (idle → power off → warm boot)
     - Works: full cycle verified on the board (:exp:`E005`)
   * - Vulkan enumeration (``vulkaninfo``)
     - Works: ``PowerVR B-Series BXE-2-32 MC1``
   * - Client CCB, UFO fences/updates, BIF slots, MTS IRQ task
     - Works: HW2 IRQ task fires on every completion (:exp:`E018`)
   * - Compute (CDM) jobs
     - **Works**: ``compute.pipeline.basic`` 73/73 supported,
       ``indirect_dispatch`` 24/24, ``builtin_var`` 11/11 (:exp:`E021`,
       :exp:`E023`)
   * - Transfer (``TQ_3D``) jobs
     - **Works**: ``api.fill_and_update_buffer`` 72/72,
       ``buffer_to_buffer`` 8/8 (:exp:`E023`); most of
       ``api.copy_and_blit.core`` passes (:exp:`E026`)
   * - Per-DM completion flush
     - Works: front-end, USC/PDS, TPU, MCU fence and SLC flush before
       retiring every fragment/transfer/compute job (:exp:`E023`)
   * - Hardware recovery (HWR)
     - Works: MMU fault or watchdog (``max_deadline_ms``) → ``SOFT_RESET``
       pulse → units re-init → discard in ~0.28 ms (:exp:`E019`, :exp:`E021`)
   * - Statistics and robustness
     - Works: GPU utilisation, time correlation, HWR log, FWCCB
       context-reset notification (:exp:`E025`)
   * - Geometry (TA) and fragment (3D) kicks
     - Experimental (``make EXP=-DEXP_TA``, ``src/geom.c``, ``src/frag.c``):
       ``simple_draw`` 4/4, ``api.smoke`` 8/8, ``basic_draw`` 203 pass /
       0 fail, depth load/store (:exp:`E038`). Open: input-attachment
       renders (:exp:`E039`, :exp:`E040`), ``FRAG_PR``, multi-kick geometry,
       OOM/grow and partial renders. The default build still retires them at
       once.

See :doc:`firmware/status-and-experiments` for the full experiment log
summary and next steps.

Provenance labels
=================

Register and hardware tables cite where each offset, name or behaviour comes
from:

**P** (public)
   Public upstream Linux ``drm/imagination`` headers (``pvr_rogue_cr_defs.h``,
   ``pvr_rogue_fwif*.h``) or public Mesa (``rogue_cr.xml``).
**S** (spec) / **O** (spec observation) / **D** (spec statement)
   The clean-room specification in :dc:`docsClean/ <README>`.
**E** (experiment)
   Established by black-box experiment on the board with this firmware,
   recorded in :lr:`docs/logRefs/ <>`.

Licenses
========

Each part of this documentation carries its own SPDX license identifier on its
first line:

* ``hardware/`` — ``CC-BY-SA-4.0`` (matching :dc:`docsClean/ <README>`);
* ``linux-driver/`` — ``GPL-2.0-only`` (matching the Linux kernel);
* ``firmware/``, ``firmware-source/`` and the top-level Sphinx files —
  ``GPL-3.0-or-later`` (matching the project ``LICENSE``).
