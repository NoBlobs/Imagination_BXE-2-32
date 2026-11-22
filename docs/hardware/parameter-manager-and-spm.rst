.. SPDX-License-Identifier: CC-BY-SA-4.0
.. Copyright (C) 2026 Thalles Campagnani

=================================================
Parameter Manager (PM), freelists and SPM
=================================================

Geometry (TA) dynamically allocates Parameter Buffer (PB) pages from two
**freelists** (``rogue_fwif_freelist``) managed by the **Parameter Manager
(PM)** hardware, and Fragment (3D) frees those pages back when rendering
completes (:dc:`kick-geometry-ta`, :dc:`partial-render-spm`). When a freelist
runs out mid-geometry, the PM raises ``PM_OUT_OF_MEMORY`` and the **SPM**
subsystem either consumes reserved ready pages, asks the host to grow the
freelist, or runs a **partial render** (``FRAG_PR``).

Two PM hardware contexts
========================

* **PM context 0** ("HW context 0", ``PMDM0``): serves **Geometry (TA)**.
* **PM context 1** ("HW context 1", ``PMDM1``): serves **Fragment (3D)**.

Observed lifecycle across a geometry → fragment render pair
(``[spec]`` + :exp:`E035`–:exp:`E038`):

1. **Before the first geometry kick of an HWRTData**: load the HWRTData's
   **local** (``N-FL``) and **global** (``U-FL``) freelists into PM context
   0, load zero ALIST/MLIST stack pointers, and bind the HWRTData to HW
   context 0.
2. **Non-first geometry kicks** (``First kick:0`` on the same HWRTData): do
   **not** reload freelists or reset PM context 0 — the PM continues from its
   live stack pointers and catalogues (:exp:`E039`, ``[spec]`` Q16m).
3. **Before the fragment kick of that HWRTData**: store the freelists from
   PM context 0 (capturing the stack tops and allocated page counts left by
   TA), load them into PM context 1 together with the geometry's live
   ALIST/MLIST stack pointers and ``PM_CAT_BASE_*`` catalogues.
4. **After fragment completion**: the PM automatically returns the render's
   pages to both freelists and signals ``EVENT_STATUS.PM_3D_MEM_FREE``
   (bit 8).
5. **On HWRTData cleanup**: store freelists back from both PM contexts.

Local and global freelist registers
===================================

Local freelist (``N-FL``)
-------------------------

.. list-table::
   :header-rows: 1
   :widths: 22 22 56

   * - PM ctx 0 (TA)
     - PM ctx 1 (3D)
     - Role
   * - ``0x0210`` / ``0x0214``
     - ``0x0218`` / ``0x021C``
     - Local freelist base GPU VA (64-bit, ``freelist_dev_addr``)
   * - ``0x0224`` (``0x0220 = 0``)
     - ``0x0230`` / ``0x0234``
     - Stack top to load (``total - 1`` at rest; ctx 1 64-bit, top from
       bit 22, :exp:`E035` / :exp:`E036`)
   * - ``0x0390`` / ``0x0394``
     - ``0x0398`` / ``0x039C``
     - Allocated PB / MMU page counts to load (staging, not live;
       :exp:`E035`, :exp:`E036`)
   * - ``0x0208``
     - ``0x0200``
     - **Local freelist load command**: write ``1``, poll bit 0 = ``0``
       (:exp:`E035`)
   * - ``0x0348``
     - ``0x0350``
     - **Live local stack pointer** (read-only; counts down on TA alloc, up
       on 3D free; :exp:`E031`, :exp:`E035`, :exp:`E036`)
   * - ``0x03A0`` / ``0x2000``
     - ``0x03A8`` / ``0x2008``
     - **Live** local allocated PB (``4PB``) / MMU (``4PT``) page counts
       (``[spec]`` Q16k)

Global freelist (``U-FL``) — two banks (stride ``0x40``)
--------------------------------------------------------

Bank ``0x20B8`` belongs to **PM context 0 (geometry)** and bank ``0x2078``
belongs to **PM context 1 (fragment)** (``[spec]`` Q16e/Q16g, :exp:`E035`):

.. list-table::
   :header-rows: 1
   :widths: 14 18 18 50

   * - Offset
     - PM ctx 0 (TA)
     - PM ctx 1 (3D)
     - Role
   * - ``+0x00`` / ``+0x04``
     - ``0x20B8`` / ``0x20BC``
     - ``0x2078`` / ``0x207C``
     - Global freelist base GPU VA (64-bit; moves down by 4 bytes per page
       added on grow)
   * - ``+0x08``
     - ``0x20C0``
     - ``0x2080``
     - Stack pointer to load (staging)
   * - ``+0x10``
     - ``0x20C8``
     - ``0x2088``
     - **Live stack pointer** (read-only; ``SP = total - 1`` when all pages
       free; :exp:`E031`, :exp:`E035`)
   * - ``+0x18`` / ``+0x1C``
     - ``0x20D0`` / ``0x20D4``
     - ``0x2090`` / ``0x2094``
     - PB (``4PB``) / MMU (``4PT``) page counts to load (staging)
   * - ``+0x20`` / ``+0x28``
     - ``0x20D8`` / ``0x20E0``
     - ``0x2098`` / ``0x20A0``
     - **Live** allocated PB / MMU pages (read-only)
   * - Load cmd
     - ``0x20B0``
     - ``0x20A8``
     - **Global freelist load command**: write ``1``, poll bit 0 = ``0``
       (:exp:`E035`)

Page-conservation invariant (verified across all snapshots, ``[spec]`` Q16e):

.. code-block:: text

   live_SP + live_allocated_PB_pages + live_allocated_MMU_pages + 1 == total_pages

ALIST, MLIST, VHeap and PM catalogue registers
==============================================

.. list-table::
   :header-rows: 1
   :widths: 28 72

   * - Register(s)
     - Role
   * - ``0x0240`` / ``0x0244``
     - ``0x80_0000_0000`` (general heap base, set at first load, ``[spec]``
       Q16g)
   * - ``0x0248`` / ``0x024C``
     - ``PM_VHEAP_TABLE`` ← ``hwrtdata.vheap_table_dev_addr``
   * - ``0x02D8`` / ``0x02DC`` (ctx 0) and ``0x02E0`` / ``0x02E4`` (ctx 1)
     - MLIST base ← ``hwrtdata.pm_mlist_dev_addr``
   * - ``0x0270`` / ``0x0278`` / ``0x0280``
     - ALIST stack pointers: live ctx 0 / load-staging (64-bit, writable) /
       live ctx 1 (:exp:`E037`, ``[spec]`` Q16k)
   * - ``0x02C0`` / ``0x02C8`` / ``0x02D0``
     - MLIST stack pointers: live ctx 0 / load-staging (writable) / live
       ctx 1 (:exp:`E037`, ``[spec]`` Q16k)
   * - ``0x0288`` / ``0x0190``
     - **ALIST / MLIST SP load commands**: write ``1``, poll bit 0 = ``0``;
       commits ``0x0278``/``0x02C8`` into the live SPs of the PM context
       being loaded (:exp:`E037`)
   * - ``0x02B0`` (``CONTEXT_PB_BASE``)
     - ``3`` when TA and 3D freelists are separate (bit 0 = local, bit 1 =
       global); ``0`` when shared (both live local SPs ``0x0348`` and
       ``0x0350`` move together; ``[spec]`` Q16e/Q16m, :exp:`E040`)
   * - ``0x03D0`` / ``0x03D4``
     - Per-load context flags: geometry sets ``0x100`` / ``0x1`` (bit 8 /
       bit 0); fragment adds ``0x10000`` / ``0x100`` (bit 16 / bit 8)
       (:exp:`E033`, ``[spec]`` Q16h/Q16k)
   * - ``0x02A0`` / ``0x02A8``
     - ``PM_PAGE_MANAGEOP`` (``1`` pauses TA allocation during freelist
       updates; ``0x02A8`` acknowledges within one read; :exp:`E032`)
   * - ``0x0250`` / ``0x0258``
     - PM handshakes: must run **after** PM register writes — writing PM
       registers while ``0x0250 = 1`` causes an MMU fault at address 0
       (:exp:`E032`, ``[spec]`` Q16j: ``TAG_ID = 0x30`` PM allocator,
       ``TAG_SB = 0x6`` ``MAVP``)
   * - ``0x0388``
     - Set to ``1`` during 3D renders so the PM raises ``PM_3D_MEM_FREE``;
       ``TQ_3D`` transfers clear it to ``0`` (``[spec]`` Q16k/Q16l)
   * - ``0x1248``–``0x1280``
     - ``ROGUE_CR_BIF_PM_CAT_BASE_{VCE,TE,ALIST}{0,1}``: PM page catalogues
       for context 0 (filled on-demand by PM hardware from the **local**
       freelist during TA) and context 1 (loaded from context 0 at fragment
       kick; high-word ``INIT_PAGE`` bits 59:40 become non-zero after
       partial renders, ``[spec]`` Q16f/Q16m)

Automatic page freeing and ``PM_3D_MEM_FREE``
=============================================

At the end of a 3D render with ``0x0388 = 1`` and valid live context-1
ALIST/MLIST stack pointers (``0x0280``/``0x02D0``), the PM hardware returns
all render pages to both freelists autonomously (``[spec]`` Q16k/Q16l,
:exp:`E037`, :exp:`E038`):

1. ``PIXELBE_END_RENDER`` (``EVENT_STATUS`` bit 3) asserts while ``0x0350``
   and ``0x2088`` still reflect the allocated state.
2. Both live stack pointers climb back to ``total - 1`` over ~7 µs (small
   render) to 0.4–2.5 ms (large PB).
3. ``PM_3D_MEM_FREE`` (``EVENT_STATUS`` bit 8) asserts once freeing is
   complete (no ``EVENT_ENABLE`` bit is needed).
4. The firmware must clear bit 8 before bit 3 and **before** loading the
   next geometry kick on the same PM context. Care must be taken that the HW2
   IRQ task does not clear bit 8 before the fragment completion path sees it
   (:exp:`E038`).

If ``0x0288``/``0x0190`` were not pulsed to make ``0x0280``/``0x02D0`` live,
``0x0350`` stays stuck after the render and subsequent draws fault
(:exp:`E036`, :exp:`E037`).

Out-of-memory and partial render (SPM) ``[spec]``
=================================================

When geometry exhausts PB pages, the PM asserts
``EVENT_STATUS.PM_OUT_OF_MEMORY`` (bit 7). The SPM subsystem
(:dc:`partial-render-spm`) follows a three-tier decision tree:

1. **Consume reserved ready pages** (no host round trip):

   * If ``freelist.ready_pages > 0``, pause TA allocation
     (``PM_PAGE_MANAGEOP = 1``, wait ``0x02A8``), add ``ready_pages`` to the
     freelist total, lower the freelist base GPU VA by **4 bytes per added
     page** (the page stack grows downward), reload the global bank
     (``0x20B8`` / ``0x20C0`` + load command), unpause (``0x02A0 = 0``), and
     resume TA without a new ``Kick TA``.

2. **Grow the freelist** (host round trip):

   * Enter SPM state ``wait for grow`` (trace ``SPM_34``), set
     ``freelist.grow_pending`` and ``os_runtime_flags.fl_grow_pending``, and
     send FWCCB ``ROGUE_FWIF_FWCCB_CMD_FREELIST_GROW`` (103).
   * The host allocates more pages and replies with KCCB
     ``ROGUE_FWIF_KCCB_CMD_FREELIST_GROW_UPDATE`` (110, carrying
     ``new_pages`` and ``ready_pages``).
   * The firmware holds back ``ready_pages`` for the next OOM (setting
     ``current_total = new_pages - ready_pages``), updates the PM under
     ``PM_PAGE_MANAGEOP``, sets ``SPM State = PR avoided``, and resumes TA.

3. **Partial render** (``FRAG_PR``) (when the global freelist is at
   ``max_pages``):

   * Check ``partial_render_geom_frag_fence`` (trace ``SPM_66``).
   * If on-demand ZS/MSAA buffers are needed, request them via FWCCB
     ``ZSBUFFER_BACKING`` (101) ↔ KCCB ``ZSBUFFER_BACKING_UPDATE`` (108).
   * Transition ``PR blocked`` → ``wait for HW`` until DM 3 is idle.
   * Load PM context 1 with geometry's **current** ``PM_CAT_BASE_*0``
     catalogues, MLIST base (``0x02E0``) and live ALIST/MLIST stack
     pointers, set ``ROGUE_CR_PM_PARTIAL_RENDER_ENABLE`` (``0x0338 = 1``)
     and ``0x0388 = 1``, and kick the context's ``FRAG_PR`` command
     (``SPM State = PR running``).
   * On ``PIXELBE_END_RENDER`` (and ``ZLS_FINISHED``), live stack pointers
     climb back to full. Clear ``0x0338 = 0``, reload PM context 0 with zero
     ALIST/MLIST SPs (``PM_CAT_BASE_VCE0`` gets new pages and non-zero
     ``INIT_PAGE`` high word), transition ``PR executed`` → ``none``, and
     restart TA without a new ``Kick TA``.

SPM states (trace group ``SPM``): ``32 = none``, ``33 = PR blocked``,
``34 = wait for grow``, ``35 = wait for HW``, ``36 = PR running``,
``38 = PR executed``.

Freelist reconstruction after HWR
---------------------------------

If HWR corrupts active freelists, the firmware sends FWCCB
``FREELISTS_RECONSTRUCTION`` (104) and blocks until the host confirms via
KCCB ``FREELISTS_RECONSTRUCTION_UPDATE`` (112).

PM 3D timeout sequence ``[spec]``
---------------------------------

Primary-core registers (no public names, :dc:`observed-sequences`):

.. code-block:: text

   disable: s = CR[0x03E0] ; CR[0x03E0] = 0
            CR[0x0388] = 1 ; CR[0x0298] = 0 ; CR[0x01C8] = 0
            EVENT_CLEAR = 0x100 ; CR[0x0380] = 1
   restore: poll EVENT_STATUS bit 8 (PM_3D_MEM_FREE) with timeout
            if ok: EVENT_CLEAR = 0x100
            CR[0x03E0] = s
