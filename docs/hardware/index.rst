.. SPDX-License-Identifier: CC-BY-SA-4.0
.. Copyright (C) 2026 Thalles Campagnani

========
Hardware
========

Hardware reference for the **PowerVR Rogue BXE-2-32** GPU (BVNC
``36.29.52.182``) and its integration in the **SpacemiT K1** SoC. These pages
consolidate the hardware behaviour documented in the clean-room specification
:dc:`docsClean/ <README>`, the public upstream ``drm/imagination`` / Mesa
definitions, and black-box silicon measurements recorded in
:lr:`docs/logRefs/ <>`.

Confidence and provenance tags used across these pages:

* ``[public]`` (**P**) — public upstream Linux ``drm/imagination`` headers
  (``pvr_rogue_cr_defs.h``, ``pvr_rogue_fwif*.h``) or public Mesa
  (``rogue_cr.xml``).
* ``[spec]`` (**S** / **O** / **D**) — from :dc:`docsClean/ <README>` (not
  yet independently exercised by our own firmware, or confirmed by black-box
  observation in the spec).
* ``[exp E0xx]`` (**E**) — directly verified on silicon with this project's
  firmware in :lr:`docs/logRefs/ <>`.
* ``[open]`` — unresolved or ``TO CONFIRM`` / ``PENDING`` hardware detail.

.. toctree::
   :maxdepth: 1

   architecture
   mips-core
   memory-and-remap
   gpu-bringup
   parameter-manager-and-spm
   kicks-geometry-ta
   kicks-compute-3d-transfer
   power-hwr-and-events
   registers
   test-platform
   glossary
