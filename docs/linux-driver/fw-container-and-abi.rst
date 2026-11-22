.. SPDX-License-Identifier: GPL-2.0-only
.. Copyright (C) 2026 Thalles Campagnani

==============================
Firmware container and ABI
==============================

The binary contract between mainline ``powervr.ko`` and the firmware is
defined by the public Linux ``drivers/gpu/drm/imagination/pvr_rogue_fwif*.h``
and ``pvr_fw_info.h`` headers together with :dc:`host-interface` and
:dc:`memory-model`. Every structure in ``src/include/fwif.h`` is verified at
compile time via ``static_assert`` against the offsets in the public
``pvr_rogue_fwif_check.h``.

The ``.fw`` container (``tools/mkfw.py``)
=========================================

.. code-block:: text

   offset 0                               file end - 4 KiB         file end
   | firmware ELF | zero padding | device info | header (48 B) | layout table | 0... |
                                               +-------- last 4 KiB block -----------+

Header (``pvr_fw_info.h``)
--------------------------

* ``info_version = 3``
* ``header_len = 48``
* ``layout_entry_num = 6``, ``layout_entry_size = 24``
* ``bvnc = 36.29.52.182``
* ``fw_info_size = 4096``
* ``flags = OPEN_SOURCE``, version ``1.0``, build ``1``
* ``device_info_size``

Mainline ``powervr`` logs this header as ``FW version v1.0 (build 1 OS)``,
which the test scripts use to confirm that our clean-room build (and never a
fallback image) was loaded.

Layout table (6 entries)
------------------------

The host allocates two firmware-heap objects (*code* and *data*, both mapped
``DEVICE_UNCACHED``) and places each section according to the 6-entry layout
table ``(id, type, base_addr, max_size, alloc_size, alloc_offset)``.
``tools/mkfw.py`` verifies that every ELF ``PT_LOAD`` segment fits inside one
entry:

.. list-table::
   :header-rows: 1
   :widths: 28 12 16 22 22

   * - Section ID
     - Host object
     - Offset
     - MIPS VA
     - Size
   * - ``MIPS_CODE``
     - code
     - ``0x00000``
     - ``0xC0000000``
     - ``0x10000`` (64 KiB)
   * - ``MIPS_BOOT_CODE``
     - code
     - ``0x10000``
     - ``0xBFC00000``
     - ``0x1000`` (4 KiB)
   * - ``MIPS_EXCEPTIONS_CODE``
     - code
     - ``0x11000``
     - ``0x9FC02000``
     - ``0x1000`` (4 KiB)
   * - ``MIPS_PRIVATE_DATA``
     - data
     - ``0x00000``
     - ``0xC0ED0000``
     - ``0x10000`` (64 KiB)
   * - ``MIPS_BOOT_DATA``
     - data
     - ``0x10000``
     - ``0xBFC01000``
     - ``0x1000`` (4 KiB)
   * - ``MIPS_STACK``
     - data
     - ``0x11000``
     - ``0xCF600000``
     - ``0x2000`` (8 KiB)

The data object is placed at the fixed VA ``0xC0ED0000`` inside the top MiB
of the firmware heap reserved by the mainline host (alongside ``osinit`` at
``0xC0FE0000`` and ``sysinit`` at ``0xC0FF0000``).

Device info block
-----------------

Format (bit order in ``pvr_rogue_fwif_dev_info.h``): header
``(brn_mask_size=1, ern_mask_size=1, feature_mask_size, feature_param_size)``,
followed by the BRN mask, ERN mask, boolean feature masks and value-feature
parameters in feature order (:dc:`memory-model`, answer to Q2).

Device-info entries that directly control host driver behaviour:

.. list-table::
   :header-rows: 1
   :widths: 28 18 54

   * - Field
     - Value
     - Effect on ``powervr.ko``
   * - ``SYS_BUS_SECURE_RESET``
     - Set
     - **Required.** Without it the host never clears
       ``ROGUE_CR_SYS_BUS_SECURE`` and the MIPS cannot fetch instructions
       (:exp:`E001`)
   * - ``SLC_SIZE_IN_KILOBYTES``
     - ``16``
     - Host leaves DM overlap enabled (``config_flags = 0x0``)
   * - ``LAYOUT_MARS``
     - ``1``
     - ``pvr_fw_stop()`` checks ``MARS_IDLE`` instead of SIDEKICK/SLC idle
   * - ``XE_MEMORY_HIERARCHY``, ``XE_TPU2``
     - Set
     - Adjusts fragment context-state sizing and selects ``SOFT_RESET2``
   * - BRN / ERNs
     - BRN ``63553``; 6 ERNs
     - Enables the 36-bit MIPS bus workaround and BXE-2-32 enhancements

See :doc:`../hardware/architecture` for the full list of boolean and value
features.

Shared-memory objects and ``fw_addr`` encoding
==============================================

The host communicates pointers to shared structures via 32-bit firmware
virtual addresses (``fw_addr``) inside the 16 MiB firmware heap at
``0xC0000000`` (:dc:`data-structures`):

* ``BOOT_DATA`` (``0xBFC01000``): public host header in ``0x00..0x3F``
  carrying ``pt_phys_addr[0..3]`` (the four physical pages of the host MIPS
  page table).
* ``rogue_fwif_osinit`` (``0xC0FE0000``): pointers to ``osdata``, KCCB and
  its control block, KCCB return slots, FWCCB and its control block, and
  ``hwr_info_buf_ctl_fw_addr``.
* ``rogue_fwif_sysinit`` (``0xC0FF0000``): pointers to ``sysdata``,
  ``runtime_cfg``, ``trace_buf_ctl_fw_addr``, ``fault_phys_addr``,
  ``pds_exec_base``, ``usc_exec_base``, ``filter_flags``,
  ``initial_core_clock_speed``, ``gpu_util_fwcb``, and ``firmware_started`` /
  ``firmware_started_timestamp``.

Flag bits in ``fw_addr``:

* **bit 0**: marks a sync checkpoint (used in UFO fences/updates);
* **bits 31:24**: meta-DMA / cached flag bits above the 16 MiB heap window.

The firmware strips these flags via ``fwptr()`` (mask ``0xC0FFFFFC``, keeping
``0`` as ``NULL``).

Key shared structure sizes and offsets
======================================

Authoritative layouts live in ``pvr_rogue_fwif*.h`` and
``pvr_rogue_fwif*_check.h`` (``[public]``, :dc:`data-structures`,
:dc:`command-structs`):

.. list-table::
   :header-rows: 1
   :widths: 30 12 58

   * - Structure
     - Size
     - Key fields / offsets
   * - ``rogue_fwif_sysinit``
     - ``0x220`` B
     - ``+0x00 fault_phys_addr``, ``+0x08 pds_exec_base``,
       ``+0x10 usc_exec_base``, ``+0x140 trace_buf_ctl_fw_addr``,
       ``+0x144 fw_sys_data_fw_addr``, ``initial_core_clock_speed``,
       ``active_pm_latency_ms``, ``firmware_started``
   * - ``rogue_fwif_osinit``
     - ``0x68`` B
     - KCCB / KCCB ctl / KCCB return slots, FWCCB / FWCCB ctl,
       ``hwr_info_buf_ctl_fw_addr``, ``fw_os_data_fw_addr``
   * - ``rogue_fwif_sysdata``
     - ``0xE28`` B
     - ``+0x00 config_flags``, ``+0x08 pow_state``,
       ``+0x2C os_runtime_flags_mirror[]``, ``+0xDD0 fw_faults``,
       ``cr_poll_*``, ``+0xDF0 start_idle_time``, ``+0xDF8 hwr_state_flags``
   * - ``rogue_fwif_osdata``
     - ``0x248`` B
     - ``+0x228 last_forced_update_time``, ``+0x230 interrupt_count[2]``,
       ``+0x238 kccb_cmds_executed``, ``+0x23C power_sync_fw_addr``
   * - ``rogue_fwif_fwcommoncontext``
     - ``0x98`` B
     - ``+0x00 ccbctl_fw_addr``, ``+0x04 ccb_fw_addr``,
       ``+0x1C fw_com_ctx_flags``, ``+0x20 priority``, ``+0x3C dm``,
       ``+0x60 robustness_address``, ``+0x68 max_deadline_ms``,
       ``+0x70/78 waiting_node/run_node``, ``+0x80 last_failed_ufo``,
       ``+0x88 fw_mem_context_fw_addr``, ``+0x8C server_common_context_id``,
       ``+0x90 pid``
   * - ``rogue_fwif_ccb_ctl`` (KCCB/FWCCB)
     - —
     - ``+0x00 write_offset``, ``+0x80 read_offset`` (separate cache line),
       ``+0x84 wrap_mask``, ``+0x88 cmd_size``
   * - ``rogue_fwif_cccb_ctl`` (cCCB)
     - —
     - ``write_offset``, ``read_offset``, ``dep_offset``, ``wrap_mask``,
       ``read_offset2..4``
   * - ``rogue_fwif_ccb_cmd_header``
     - ``0x28`` B
     - ``cmd_type`` (``0x2ABCxxxx``, bit 15 = task), ``cmd_size``,
       ``ext_job_ref``, ``int_job_ref``, ``work_est_kick_data``
   * - Kick command payloads
     - —
     - ``cmd_geom`` (``0x70`` B, ``geom_regs`` ``0x40`` B), ``cmd_frag``
       (``0x1E0`` B, ``frag_regs`` ``0x1C0`` B), ``cmd_transfer``
       (``0xC0`` B), ``cmd_compute`` (``0x60`` B)
   * - ``rogue_fwif_freelist`` / ``hwrtdata``
     - ``0x40`` / ``0x180`` B
     - Freelist base, stack top, page counts, ``ready_pages``; HWRTData
       ``+0xAC state``, ``+0xB0 freelists_fw_addr[]``
   * - ``rogue_fwif_tracebuf`` / ``hwrinfobuf``
     - ``0x360`` / ``0x920`` B
     - Trace ring control; 16 HWR info slots (8 preserved + 8 ring) and
       per-DM HWR counters
   * - ``rogue_fwif_gpu_util_fwcb``
     - —
     - ``+0x0 time_corr[256]`` (``0x28`` B each),
       ``+0x2800 time_corr_seq_count``, ``+0x2804 gpu_util_flags``,
       ``+0x2808 last_word``, ``+0x2810 stats_counters``
