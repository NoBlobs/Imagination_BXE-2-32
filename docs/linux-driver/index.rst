.. SPDX-License-Identifier: GPL-2.0-only
.. Copyright (C) 2026 Thalles Campagnani

============
Linux Driver
============

The mainline Linux ``drm/imagination`` driver (``powervr.ko``) and the Mesa
``pvr`` Vulkan driver form the host side of the firmware contract. The
firmware in ``src/`` is a drop-in ``rogue_36.29.52.182_v1.fw`` image that
runs under the unmodified mainline ``powervr`` driver; the only kernel
changes on branch ``IMG-BXE-2-32`` add the SpacemiT K1 GPU power domain, the
K1 GPU device-tree node and the experimental BVNC table entry.

These pages document the host-side role, the Linux kernel integration, the
``.fw`` container and shared-memory ABI, the runtime protocol between
``powervr.ko`` and the firmware, and the deployment and testing workflow.

.. toctree::
   :maxdepth: 1

   role
   kernel-integration
   fw-container-and-abi
   host-protocol
   testing-and-workflow
