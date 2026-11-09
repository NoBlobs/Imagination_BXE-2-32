/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Thalles Campagnani */
/* RGX control registers (offsets from pvr_rogue_cr_defs.h or the spec). */
#pragma once

#define CR_CORE_REVISION                0x0020
#define CR_SOFT_RESET                   0x0100
#define CR_EVENT_ENABLE                 0x0128
#define CR_EVENT_STATUS                 0x0130
#define CR_EVENT_CLEAR                  0x0138
#define CR_TIMER                        0x0160
#define CR_MIPS_WRAPPER_CONFIG          0x0810
#define CR_MIPS_ADDR_REMAP_RANGE_CONFIG 0x0878
#define CR_MIPS_ADDR_REMAP_RANGE_READ   0x0880
#define CR_MIPS_ADDR_REMAP_RANGE_DATA   0x0888
#define CR_MIPS_WRAPPER_IRQ_ENABLE      0x08A0
#define CR_MIPS_WRAPPER_IRQ_STATUS      0x08A8
#define CR_MIPS_EXCEPTION_STATUS        0x08D0
#define CR_XPU_BROADCAST                0x0890
#define CR_MTS_SCHEDULE                 0x0B00
#define CR_MTS_TASK_END                 0x0B08 /* no public name (spec) */
#define CR_MTS_BGCTX_THREAD0_DM_ASSOC   0x0B30
#define CR_MTS_INTCTX_THREAD0_DM_ASSOC  0x0B40
#define CR_MTS_GARTEN_WRAPPER_CONFIG    0x0B50
#define CR_MTS_DM0_INTERRUPT_ENABLE     0x0B58
#define CR_MTS_INTCTX                   0x0B98
#define CR_MTS_BGCTX                    0x0BA0
#define CR_MTS_BGCTX_COUNTED_SCHEDULE   0x0BA8
#define CR_MTS_GPU_INT_STATUS           0x0BB0
#define CR_MTS_SCHEDULE_ENABLE          0x0BC8
#define CR_BIF_CAT_BASE0                0x1200
#define CR_BIF_CAT_BASE_INDEX           0x1240
#define CR_BIF_CTRL_INVAL               0x12A0
#define CR_BIF_FAULT_BANK0_MMU_STATUS   0x12B0
#define CR_SLC_CTRL_FLUSH_INVAL         0x3818
#define CR_SLC_STATUS0                  0x3820
#define CR_SCRATCH15                    0x1A78
#define CR_MULTICORE_SYSTEM             0xF308

/* EVENT_STATUS bits */
#define EV_KERNEL_FINISHED      (1u << 1)
#define EV_COMPUTE_FINISHED     (1u << 2)
#define EV_PIXELBE_END_RENDER   (1u << 3)
#define EV_TA_FINISHED          (1u << 5)
#define EV_TA_TERMINATE         (1u << 6)
#define EV_PM_OUT_OF_MEMORY     (1u << 7)
#define EV_PM_3D_MEM_FREE       (1u << 8)
#define EV_MMU_PAGE_FAULT       (1u << 9)
#define EV_POWER_COMPLETE       (1u << 10)
#define EV_POWER_ABORT          (1u << 11)
#define EV_GPIO_REQ             (1u << 12)
#define EV_GPIO_ACK             (1u << 13)
#define EV_USC_TRIGGER          (1u << 15)

/* Remap range config fields (low word) */
#define REMAP_ENTRY_SHIFT       1
#define REMAP_ENABLE            1u
#define REMAP_SIZE_4K           0u

/* Debug scratch registers (ROGUE_CR_SCRATCHn, 0x1A00 + 8n), readable by the host. */
#define CR_DBG_STAGE            0x1A00
#define CR_DBG_REFILLS          0x1A08
#define CR_DBG_LASTVA           0x1A10
#define CR_DBG_EXC_CAUSE        0x1A18
#define CR_DBG_EXC_EPC          0x1A20
#define CR_DBG_EXC_BADVA        0x1A28
#define CR_DBG_UNITS_PDS        0x1A30
#define CR_DBG_UNITS_USC        0x1A38
#define CR_DBG_IRQ_TIMER        0x1A40  /* HW0 count */
#define CR_DBG_IRQ_BG           0x1A48  /* HW1 (MTS BG task) count */
#define CR_DBG_IRQ_IRQ          0x1A50  /* HW2 (MTS IRQ task) count */
#define CR_DBG_IRQ_LASTEV       0x1A58  /* EVENT_STATUS seen by the last IRQ task */
#define CR_DBG_EVENTS_SEEN      0x1A60  /* OR of every event handled by kick_events */
#define CR_DBG_HWR_COUNT        0x1A68  /* completed hardware recoveries */
#define CR_DBG_BG_WITH_EV       0x1A70  /* BG tasks entered with a DM completion pending (E018) */
/* scratch 0x1A78 is CR_SCRATCH15 (busy DMs); the next two reuse the exec-base read-backs */
#define CR_DBG_BG_LASTEV        0x1A30  /* EVENT_STATUS at the last such BG task */
#define CR_DBG_BG_CTX           0x1A38  /* MTS_BGCTX at the last BG task entry */
#define CR_DBG_IRQ_COMPUTE      0x1A20  /* IRQ tasks that carried COMPUTE_FINISHED (shares EXC_EPC) */
#define CR_DBG_KICKS_CDM        0x1A18  /* CDM kicks issued (shares EXC_CAUSE) */
#define CR_DBG_INT_CTX          0x1A28  /* MTS_INTCTX at the last IRQ task entry (shares EXC_BADVA) */
