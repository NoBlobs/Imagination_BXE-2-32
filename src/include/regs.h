/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Thalles Campagnani */
/**
 * GPU control registers shared by several files: offsets from the public
 * ``pvr_rogue_cr_defs.h`` or from the specification, the ``CR_EVENT_STATUS``
 * bits, the remap configuration fields and the debug scratch registers the
 * host-side tools read. Registers used by one file only are defined there.
 */
/**/
#pragma once

/** Core revision (read-only); also read to drain posted writes. */
#define CR_CORE_REVISION                0x0020
/** Soft reset of the GPU units (64-bit). */
#define CR_SOFT_RESET                   0x0100
/** Events routed to the host interrupt (must stay 0, E011). */
#define CR_EVENT_ENABLE                 0x0128
/** Pending GPU events (``EV_*``). */
#define CR_EVENT_STATUS                 0x0130
/** Write 1 to clear GPU events. */
#define CR_EVENT_CLEAR                  0x0138
/** GPU timer, core clock / 256 (48 bits used). */
#define CR_TIMER                        0x0160
/** MIPS wrapper configuration. */
#define CR_MIPS_WRAPPER_CONFIG          0x0810
/** Address-remap entry configuration (low word: entry, input address, enable; high word: output address). */
#define CR_MIPS_ADDR_REMAP_RANGE_CONFIG 0x0878
/** Address-remap entry read-back select. */
#define CR_MIPS_ADDR_REMAP_RANGE_READ   0x0880
/** Address-remap entry read-back data. */
#define CR_MIPS_ADDR_REMAP_RANGE_DATA   0x0888
/** Firmware-to-host interrupt enable. */
#define CR_MIPS_WRAPPER_IRQ_ENABLE      0x08A0
/** Firmware-to-host interrupt status; writing 1 raises the host interrupt. */
#define CR_MIPS_WRAPPER_IRQ_STATUS      0x08A8
/** MIPS exception status. */
#define CR_MIPS_EXCEPTION_STATUS        0x08D0
/** XPU broadcast: register writes go to every core. */
#define CR_XPU_BROADCAST                0x0890
/** MTS schedule request: writing it asks for a background task. */
#define CR_MTS_SCHEDULE                 0x0B00
/** End of the current MTS task (no public name; from the specification). */
#define CR_MTS_TASK_END                 0x0B08 /* no public name (spec) */
/** Data masters served by the MTS background task. */
#define CR_MTS_BGCTX_THREAD0_DM_ASSOC   0x0B30
/** Data masters served by the MTS interrupt task. */
#define CR_MTS_INTCTX_THREAD0_DM_ASSOC  0x0B40
/** MTS Garten wrapper configuration. */
#define CR_MTS_GARTEN_WRAPPER_CONFIG    0x0B50
/** Events that start the interrupt task for DM0; DMn at ``+8n``. */
#define CR_MTS_DM0_INTERRUPT_ENABLE     0x0B58
/** MTS interrupt task context. */
#define CR_MTS_INTCTX                   0x0B98
/** MTS background task context. */
#define CR_MTS_BGCTX                    0x0BA0
/** MTS counted background schedule. */
#define CR_MTS_BGCTX_COUNTED_SCHEDULE   0x0BA8
/** MTS GPU interrupt status. */
#define CR_MTS_GPU_INT_STATUS           0x0BB0
/** MTS schedule enable. */
#define CR_MTS_SCHEDULE_ENABLE          0x0BC8
/** BIF page-catalogue base of slot 0; slot n at ``+8n``. */
#define CR_BIF_CAT_BASE0                0x1200
/** BIF slot used by each data master. */
#define CR_BIF_CAT_BASE_INDEX           0x1240
/** BIF MMU cache invalidation (PT, PD, PC, TLB bits). */
#define CR_BIF_CTRL_INVAL               0x12A0
/** MMU fault status of bank 0; the request status follows at ``+8``. */
#define CR_BIF_FAULT_BANK0_MMU_STATUS   0x12B0
/** SLC flush and invalidate control. */
#define CR_SLC_CTRL_FLUSH_INVAL         0x3818
/** SLC status. */
#define CR_SLC_STATUS0                  0x3820
/** ``CR_SLC_CTRL_FLUSH_INVAL``: flush and invalidate the whole SLC. */
#define SLC_FLUSH_INVAL_ALL             0x1u
/** ``CR_SLC_STATUS0``: a flush is in progress. */
#define SLC_STATUS_FLUSH_PENDING        0x4u
/** Scratch register 15: bit n set while data master n is busy (debug). */
#define CR_SCRATCH15                    0x1A78
/** Multicore system configuration. */
#define CR_MULTICORE_SYSTEM             0xF308

/* EVENT_STATUS bits */
/** Event: kernel finished. */
#define EV_KERNEL_FINISHED      (1u << 1)
/** Event: the compute data master finished. */
#define EV_COMPUTE_FINISHED     (1u << 2)
/** Event: the pixel back end finished a render. */
#define EV_PIXELBE_END_RENDER   (1u << 3)
/** Event: the TA (geometry) finished. */
#define EV_TA_FINISHED          (1u << 5)
/** Event: the TA stopped (out of memory). */
#define EV_TA_TERMINATE         (1u << 6)
/** Event: the PM ran out of free-list pages. */
#define EV_PM_OUT_OF_MEMORY     (1u << 7)
/** Event: the PM returned a render's pages. */
#define EV_PM_3D_MEM_FREE       (1u << 8)
/** Event: MMU page fault. */
#define EV_MMU_PAGE_FAULT       (1u << 9)
/** Event: a power-island transition completed. */
#define EV_POWER_COMPLETE       (1u << 10)
/** Event: a power-island transition was aborted. */
#define EV_POWER_ABORT          (1u << 11)
/** Event: GPIO request. */
#define EV_GPIO_REQ             (1u << 12)
/** Event: GPIO acknowledge. */
#define EV_GPIO_ACK             (1u << 13)
/** Event: USC trigger. */
#define EV_USC_TRIGGER          (1u << 15)

/* Remap range config fields (low word) */
/** Position of the entry index in a remap configuration word. */
#define REMAP_ENTRY_SHIFT       1
/** Remap configuration: entry enabled. */
#define REMAP_ENABLE            1u
/** Remap configuration: 4 KiB region. */
#define REMAP_SIZE_4K           0u

/* Debug scratch registers (ROGUE_CR_SCRATCHn, 0x1A00 + 8n), readable by the host. */
/** Debug: boot stage (``0xC0DE000n`` / ``0xB007n``). */
#define CR_DBG_STAGE            0x1A00
/** Debug: number of TLB refills. */
#define CR_DBG_REFILLS          0x1A08
/** Debug: address of the last TLB refill. */
#define CR_DBG_LASTVA           0x1A10
/** Debug: ``Cause`` of the last fatal exception. */
#define CR_DBG_EXC_CAUSE        0x1A18
/** Debug: ``EPC`` of the last fatal exception. */
#define CR_DBG_EXC_EPC          0x1A20
/** Debug: ``BadVAddr`` of the last fatal exception. */
#define CR_DBG_EXC_BADVA        0x1A28
/** Debug: read-back of the PDS execution base (high word). */
#define CR_DBG_UNITS_PDS        0x1A30
/** Debug: read-back of the USC execution base (high word). */
#define CR_DBG_UNITS_USC        0x1A38
/** HW0 count. */
#define CR_DBG_IRQ_TIMER        0x1A40  /* HW0 count */
/** HW1 (MTS BG task) count. */
#define CR_DBG_IRQ_BG           0x1A48  /* HW1 (MTS BG task) count */
/** HW2 (MTS IRQ task) count. */
#define CR_DBG_IRQ_IRQ          0x1A50  /* HW2 (MTS IRQ task) count */
/** EVENT_STATUS seen by the last IRQ task. */
#define CR_DBG_IRQ_LASTEV       0x1A58  /* EVENT_STATUS seen by the last IRQ task */
/** OR of every event handled by kick_events. */
#define CR_DBG_EVENTS_SEEN      0x1A60  /* OR of every event handled by kick_events */
/** Completed hardware recoveries. */
#define CR_DBG_HWR_COUNT        0x1A68  /* completed hardware recoveries */
/** BG tasks entered with a DM completion pending (E018). */
#define CR_DBG_BG_WITH_EV       0x1A70  /* BG tasks entered with a DM completion pending (E018) */
/* scratch 0x1A78 is CR_SCRATCH15 (busy DMs); the next two reuse the exec-base read-backs */
/** EVENT_STATUS at the last such BG task. */
#define CR_DBG_BG_LASTEV        0x1A30  /* EVENT_STATUS at the last such BG task */
/** MTS_BGCTX at the last BG task entry. */
#define CR_DBG_BG_CTX           0x1A38  /* MTS_BGCTX at the last BG task entry */
/** IRQ tasks that carried COMPUTE_FINISHED (shares EXC_EPC). */
#define CR_DBG_IRQ_COMPUTE      0x1A20  /* IRQ tasks that carried COMPUTE_FINISHED (shares EXC_EPC) */
/** CDM kicks issued (shares EXC_CAUSE). */
#define CR_DBG_KICKS_CDM        0x1A18  /* CDM kicks issued (shares EXC_CAUSE) */
/** MTS_INTCTX at the last IRQ task entry (shares EXC_BADVA). */
#define CR_DBG_INT_CTX          0x1A28  /* MTS_INTCTX at the last IRQ task entry (shares EXC_BADVA) */
