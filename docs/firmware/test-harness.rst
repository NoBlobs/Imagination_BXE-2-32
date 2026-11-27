.. SPDX-License-Identifier: GPL-3.0-or-later
.. Copyright (C) 2026 Thalles Campagnani

========================
Test harness: vkfwtest
========================

``vkfwtest`` is the firmware's main Vulkan test battery. It is a headless
program that drives the firmware through the mainline ``powervr`` kernel
driver and Mesa, and **checks every result on the CPU**, pixel by pixel or
word by word. A test passes only if every fence signals within the timeout
**and** every value read back matches. A firmware that skips work or renders
garbage fails even when nothing faults.

It lives in its own repository, ``NoBlobs/vkfwtest``, next to this one
(``../vkfwtest``), because the BXM-4-64 firmware uses the same harness. It is
written in GNU C23 for GCC, does not depend on the CPU architecture, and
comes only from the public Vulkan API and ``docsClean``
(``board-testing-oracle.md`` section 4).

Build and run
=============

``tools/deploy.sh vkfwtest`` sends the sources to the board and builds them
there; the board has GCC 14, ``libvulkan`` and ``glslangValidator``, but no
``rsync``. The binary is ``~/vkfwtest-clean/build/vkfwtest``. The earlier
``~/vkfwtest`` is left untouched. Run it like this:

.. code-block:: sh

   export VK_ICD_FILENAMES=/opt/mesa-pvr/share/vulkan/icd.d/powervr_mesa_icd.riscv64.json
   export PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1
   ~/vkfwtest-clean/build/vkfwtest -v all            # every test
   ~/vkfwtest-clean/build/vkfwtest -j bigpb -t 2M    # one test, JSON line
   ~/vkfwtest-clean/build/vkfwtest list

Options: ``--frames``, ``--size WxH``, ``--tris N[K|M]``, ``--iters``,
``--groups``, ``--seed``, ``--timeout``, ``--repeat``, ``--priority``,
``--device``, ``--require-preempt``, ``--keep-going``, ``--json``,
``-v``/``-q``. ``all`` runs every test except ``hwr-*`` and ``prio-*``.

Exit codes:

* ``0``: pass;
* ``1``: output mismatch;
* ``2``: fence timeout;
* ``3``: device lost;
* ``4``: setup error or bad option;
* ``77``: skipped.

Tests
=====

.. list-table::
   :header-rows: 1
   :widths: 18 42 40

   * - Test (alias)
     - Firmware paths
     - Check
   * - ``multiframe`` (``pre-3d``)
     - TA + 3D over several frames on one framebuffer, so Mesa alternates
       the two HWRTData of the render target
     - every pixel of every frame (per-frame colour, or a per-pixel LCG
       with ``--iters``)
   * - ``pre-ta``
     - geometry-heavy frames: layers of tiny tiled triangles (100 K by
       default)
     - every pixel equals the last layer's colour for its cell
   * - ``pre-cdm``
     - compute dispatches
     - every word equals ``lcg(id ^ seed, iters)``
   * - ``xfer``
     - ``TQ_3D``: upload, copy with offsets, mirrored blit, MSAA resolve on
       137×91 and 256×256 images
     - every byte
   * - ``bigpb``
     - parameter-buffer OOM, grows and partial renders: 4 M triangles into
       1024×1024 + stored ``D32_SFLOAT``
     - every pixel equals the first layer's colour (later layers are
       farther away)
   * - ``input-attach``
     - one render pass with two subpasses (two HWRTData); subpass 1 reads
       colour + ``D32`` depth as input attachments
     - both colour attachments
   * - ``rta``
     - a 6-layer render target: render pass clear, per-layer
       ``vkCmdClearAttachments``, and ``gl_Layer`` draws when
       ``VK_EXT_shader_viewport_index_layer`` exists
     - every pixel of every layer
   * - ``texture``, ``zs-load``, ``msaa``, ``mrt``, ``sizes``
     - texture unit (address modes, borders, linear, mips); depth/stencil
       load; 4× render + resolve; 8 colour attachments; 1×1 … 4096×4096 and
       a partial render area
     - every pixel (filtering and sRGB ±1)
   * - ``vertex``, ``indirect``, ``queries``, ``formats``
     - vertex/index buffers, instancing, points, lines; indirect draws and
       dispatches; occlusion queries and timestamps; RGB565, BGRA8, sRGB,
       RGBA16F, R32_UINT, blend, discard
     - every pixel, word or query result
   * - ``stress``, ``soak``, ``churn``
     - 4 devices at once; one long-lived device; 20 devices created and
       destroyed
     - the checks of the tests they run
   * - ``hwr-lockup`` (not in ``all``)
     - stage E: a compute shader that spins forever; then ``multiframe`` and
       ``pre-cdm`` on new devices without reloading the driver
     - ``VK_ERROR_DEVICE_LOST`` for the guilty device, then both tests
   * - ``hwr-fault`` (not in ``all``)
     - stage E with an MMU page fault (accesses ~4 GiB past a buffer)
     - as ``hwr-lockup``
   * - ``prio-3d``, ``prio-ta``, ``prio-cdm`` (not in ``all``)
     - stage F: a long LOW-priority job (about 1.5 s) while HIGH-priority
       probes run
     - every output; reports whether the probes were delayed
       (``--require-preempt`` makes a delay a failure)

On the host GPU of the development PC (an NVIDIA GTX 1060), every test of
``all`` passes, and so do the ``prio-*`` tests. So the expected values are right
(:exp:`E044`). ``hwr-lockup`` is not run there, because it would freeze the display.

On the board, ``docsClean/vkfwtest-results.md`` gives the reference: with the
manufacturer firmware, the vendor stack passes every test it can run (38/38),
and the mainline + Mesa ``pvr`` stack gets 18 pass / 18 fail / 3 skip. Those 18
tests must pass with our firmware too; :exp:`E044` compares the two
test by test.

Acceptance oracle
=================

``tools/oracle.sh`` uses ``vkfwtest`` for:

* **stage B:** ``multiframe``, ``pre-ta``, ``pre-cdm``, ``xfer``,
  ``bigpb``. Each needs ``rc == 0`` (the harness's own checks) plus the
  trace proofs: kick lines present and no hard failure in the new trace
  lines;
* **stage B** also runs ``input-attach`` and ``rta``;
* **stage D:** ``xfer`` from the suspended state;
* **stage E** (only when asked, ``oracle.sh E``): ``hwr-lockup`` and
  ``hwr-fault``;
* **stage F** (optional, ``oracle.sh F``): the other tests and ``prio-*``.

``VKFWTEST`` and ``VKFWTEST_ARGS`` override the binary and add options.
