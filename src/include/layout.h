/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Thalles Campagnani */
/**
 * Firmware memory layout: the MIPS virtual addresses fixed by the host and
 * hardware contract, the layout of the two host allocations (code and data
 * objects), the firmware's private words in the boot-data page and the wired
 * TLB entries.
 *
 * Shared by C, assembly, the linker script and the packer
 * (``tools/mkfw.py`` parses the ``FW_*`` and ``FWL_*`` defines from this
 * file, one per line).
 */
/**/
#pragma once

/* MIPS virtual addresses fixed by the host/hardware contract. */
/** Start of the firmware heap window (host-mapped through the MIPS page table). */
#define FW_HEAP_BASE        0xC0000000
/** Size of the heap window. */
#define FW_HEAP_SIZE        0x01000000
/** Mask that turns a host firmware address into a heap pointer. */
#define FW_VA_MASK          0xC0FFFFFC
/** Address of the host's ``sysinit`` structure. */
#define FW_SYSINIT_VA       0xC0FF0000
/** Address of the host's ``osinit`` structure. */
#define FW_OSINIT_VA        0xC0FE0000
/** Where the host's MIPS page table is mapped (four pages, wired). */
#define FW_PT_VA            0xCF000000
/** Where the firmware stack is mapped (two pages, wired). */
#define FW_STACK_VA         0xCF600000
/** GPU register bank (core banks), 1 MiB identity pages, wired. */
#define FW_REGS_VA          0xCF800000
/** XPU broadcast register bank, wired. */
#define FW_BCAST_VA         0xCFA00000
/** Reset vector and boot code (KSEG1). */
#define FW_BOOT_CODE_VA     0xBFC00000
/** Boot-data page (KSEG1, uncached): host boot data and the firmware's private words. */
#define FW_BOOT_DATA_VA     0xBFC01000
/** Exception page (``EBase``, KSEG0). */
#define FW_EXC_VA           0x9FC02000

/* Layout of the two host allocations (code object and data object). */
/** Code object: heap offset 0. */
#define FWL_CODE_BASE       0xC0000000 /* code object = heap offset 0 */
/** Main code and read-only data. */
#define FWL_CODE_SIZE       0x00010000 /* main code + rodata          */
/** Boot page inside the code object. */
#define FWL_BOOT_OFFSET     0x00010000 /* boot page inside code obj   */
/** Exception page inside the code object. */
#define FWL_EXC_OFFSET      0x00011000 /* exception page in code obj  */
/** Size of the code object. */
#define FWL_CODE_ALLOC      0x00012000

/** Data object, in the host's reserved 1 MiB of the heap. */
#define FWL_DATA_BASE       0xC0ED0000 /* data object, in the host's reserved 1 MiB */
/** Private data and ``.bss``. */
#define FWL_DATA_SIZE       0x00010000 /* private data + bss          */
/** Boot-data page inside the data object. */
#define FWL_BOOTDATA_OFFSET 0x00010000 /* boot data page in data obj  */
/** Stack inside the data object. */
#define FWL_STACK_OFFSET    0x00011000 /* stack inside data obj       */
/** Stack size. */
#define FWL_STACK_SIZE      0x00002000
/** Size of the data object. */
#define FWL_DATA_ALLOC      0x00013000

/** Heap address of the stack pages (backing :c:macro:`FW_STACK_VA`). */
#define FW_STACK_HEAP_VA    (FWL_DATA_BASE + FWL_STACK_OFFSET)

/* Private part of the BOOT_DATA page (after the public boot-data header). */
/** Size of the public boot-data header written by the host. */
#define BD_PUBLIC_SIZE      0x40
/** Start of the firmware's private area in the boot-data page. */
#define BD_PRIV             0x800 /* start of the firmware private area */
/** Uncached write-barrier word. */
#define BD_BARRIER          (BD_PRIV + 0x00) /* uncached write-barrier word */
/** Next dynamic TLB slot. */
#define BD_TLB_NEXT         (BD_PRIV + 0x04) /* next dynamic TLB slot       */
/** Remap high word of the host fault page. */
#define BD_FAULT_HI         (BD_PRIV + 0x08) /* remap hi word for fault page */
/** Number of refills sent to the fault page. */
#define BD_FAULT_COUNT      (BD_PRIV + 0x0C)
/** ``BadVAddr`` of the last fault. */
#define BD_LAST_BADVADDR    (BD_PRIV + 0x10)
/** ``Cause`` of the last fatal exception. */
#define BD_LAST_CAUSE       (BD_PRIV + 0x14)
/** ``EPC`` of the last fatal exception. */
#define BD_LAST_EPC         (BD_PRIV + 0x18)
/** Set while the general exception handler runs (stops nested faults). */
#define BD_IN_EXCEPTION     (BD_PRIV + 0x1C)
/** Boot progress marker. */
#define BD_BOOT_STAGE       (BD_PRIV + 0x20)
/** Number of TLB refills. */
#define BD_REFILL_COUNT     (BD_PRIV + 0x24)
/** Save area of the TLB refill handler (8 words). */
#define BD_SAVE             (BD_PRIV + 0x40) /* refill handler save area (8 words) */
/** Register dump of a fatal exception (32 words). */
#define BD_EXC_REGS         (BD_PRIV + 0x80) /* register dump on fatal exception (32 words) */

/* Wired TLB entries */
/** Wired TLB entry: page-table pages 0 and 1. */
#define TLB_WIRED_PT0       0
/** Wired TLB entry: page-table pages 2 and 3. */
#define TLB_WIRED_PT1       1
/** Wired TLB entry: register bank. */
#define TLB_WIRED_REGS      2
/** Wired TLB entry: stack. */
#define TLB_WIRED_STACK     3
/** Number of fixed wired entries; the broadcast bank takes the next one. */
#define TLB_NUM_WIRED       4
/** TLB size. */
#define TLB_NUM_ENTRIES     16
