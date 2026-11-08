/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Thalles Campagnani */
/*
 * Firmware memory layout.  Shared by C, assembly, the linker script and the
 * packer (tools/mkfw.py parses the FWL_* defines from this file).
 */
#pragma once

/* MIPS virtual addresses fixed by the host/hardware contract. */
#define FW_HEAP_BASE        0xC0000000
#define FW_HEAP_SIZE        0x01000000
#define FW_VA_MASK          0xC0FFFFFC
#define FW_SYSINIT_VA       0xC0FF0000
#define FW_OSINIT_VA        0xC0FE0000
#define FW_PT_VA            0xCF000000
#define FW_STACK_VA         0xCF600000
#define FW_REGS_VA          0xCF800000
#define FW_BCAST_VA         0xCFA00000
#define FW_BOOT_CODE_VA     0xBFC00000
#define FW_BOOT_DATA_VA     0xBFC01000
#define FW_EXC_VA           0x9FC02000

/* Layout of the two host allocations (code object and data object). */
#define FWL_CODE_BASE       0xC0000000 /* code object = heap offset 0 */
#define FWL_CODE_SIZE       0x00010000 /* main code + rodata          */
#define FWL_BOOT_OFFSET     0x00010000 /* boot page inside code obj   */
#define FWL_EXC_OFFSET      0x00011000 /* exception page in code obj  */
#define FWL_CODE_ALLOC      0x00012000

#define FWL_DATA_BASE       0xC0ED0000 /* data object, in the host's reserved 1 MiB */
#define FWL_DATA_SIZE       0x00010000 /* private data + bss          */
#define FWL_BOOTDATA_OFFSET 0x00010000 /* boot data page in data obj  */
#define FWL_STACK_OFFSET    0x00011000 /* stack inside data obj       */
#define FWL_STACK_SIZE      0x00002000
#define FWL_DATA_ALLOC      0x00013000

#define FW_STACK_HEAP_VA    (FWL_DATA_BASE + FWL_STACK_OFFSET)

/* Private part of the BOOT_DATA page (after the public boot-data header). */
#define BD_PUBLIC_SIZE      0x40
#define BD_PRIV             0x800 /* start of the firmware private area */
#define BD_BARRIER          (BD_PRIV + 0x00) /* uncached write-barrier word */
#define BD_TLB_NEXT         (BD_PRIV + 0x04) /* next dynamic TLB slot       */
#define BD_FAULT_HI         (BD_PRIV + 0x08) /* remap hi word for fault page */
#define BD_FAULT_COUNT      (BD_PRIV + 0x0C)
#define BD_LAST_BADVADDR    (BD_PRIV + 0x10)
#define BD_LAST_CAUSE       (BD_PRIV + 0x14)
#define BD_LAST_EPC         (BD_PRIV + 0x18)
#define BD_IN_EXCEPTION     (BD_PRIV + 0x1C)
#define BD_BOOT_STAGE       (BD_PRIV + 0x20)
#define BD_REFILL_COUNT     (BD_PRIV + 0x24)
#define BD_SAVE             (BD_PRIV + 0x40) /* refill handler save area (8 words) */
#define BD_EXC_REGS         (BD_PRIV + 0x80) /* register dump on fatal exception (32 words) */

/* Wired TLB entries */
#define TLB_WIRED_PT0       0
#define TLB_WIRED_PT1       1
#define TLB_WIRED_REGS      2
#define TLB_WIRED_STACK     3
#define TLB_NUM_WIRED       4
#define TLB_NUM_ENTRIES     16
