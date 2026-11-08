/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Thalles Campagnani */
/*
 * Reset / NMI entry (0xBFC00000, KSEG1, microMIPS).
 *
 * Brings the MIPS core to a state where C code can run from the mapped
 * firmware heap:
 *   - CP0 setup (exception base, vectored interrupts, uncached KSEG0);
 *   - TLB cleared, then wired entries for the register bank, the host page
 *     table and the stack, with the matching wrapper remap entries;
 *   - .bss cleared, stack pointer set, jump to fw_main() through ERET.
 *
 * This is also the warm-boot path: every boot re-initialises everything.
 */
#include "layout.h"
#include "regs.h"
#include "cp0asm.h"

	.set	micromips
	.set	noreorder
	.set	noat

/* Program wrapper remap entry: \entry (reg), base_in VA \va (reg), out word \hi (reg).
 * Order (docsClean/memory-model.md): low word, high word, read back the high word. */
	.macro	REMAP entry, va, hi, tmp, regs
	sll	\tmp, \entry, REMAP_ENTRY_SHIFT
	or	\tmp, \tmp, \va
	ori	\tmp, \tmp, REMAP_ENABLE
	sw	\tmp, CR_MIPS_ADDR_REMAP_RANGE_CONFIG(\regs)
	sw	\hi, (CR_MIPS_ADDR_REMAP_RANGE_CONFIG + 4)(\regs)
	lw	\tmp, (CR_MIPS_ADDR_REMAP_RANGE_CONFIG + 4)(\regs)	/* flush posted writes */
	.endm

/* hi word of a remap entry from a 64-bit physical address held in (\lo32, \hi32) */
	.macro	PHYS_TO_HI out, lo32, hi32, tmp
	srl	\out, \lo32, 12
	sll	\tmp, \hi32, 20
	or	\out, \out, \tmp
	sll	\out, \out, 4
	.endm

/* hi word of a remap entry from a host MIPS PTE */
	.macro	PTE_TO_HI out, pte, tmp
	srl	\out, \pte, 6
	li	\tmp, 0x00FFFFFF
	and	\out, \out, \tmp
	sll	\out, \out, 4
	.endm

/* progress marker in the boot-data page (KSEG1, works before the TLB is set up) */
	.macro	MARK v
	li	$26, \v
	sw	$26, BD_BOOT_STAGE($25)
	.endm

	.section .boot, "ax"
	.globl	_reset
	.ent	_reset
_reset:
	li	$25, FW_BOOT_DATA_VA
	MARK	0xA001
	/* Status: CU0 | BEV | ERL, interrupts off */
	li	$26, 0x10400004
	MTC0	26, 12, 0
	ehb

	MARK	0xA002
	/* Config.K0 = 2 (uncached): caches are never used by this firmware */
	MFC0	26, 16, 0
	li	$27, ~7
	and	$26, $26, $27
	ori	$26, $26, 2
	MTC0	26, 16, 0

	MARK	0xA003
	/* Config3.ISAOnExc = 1: exception handlers are microMIPS */
	MFC0	26, 16, 3
	lui	$27, 0x0001
	or	$26, $26, $27
	MTC0	26, 16, 3

	MARK	0xA004
	/* Cause.IV = 1 (dedicated interrupt vectors) */
	lui	$26, 0x0080
	MTC0	26, 13, 0

	MARK	0xA005
	/* EBase */
	li	$26, FW_EXC_VA
	MTC0	26, 15, 1

	MARK	0xA006
	/* IntCtl.VS = 256 bytes */
	MFC0	26, 12, 1
	li	$27, ~(0x1F << 5)
	and	$26, $26, $27
	ori	$26, $26, (0x08 << 5)
	MTC0	26, 12, 1

	MTC0	0, 9, 0		/* Count */
	MTC0	0, 5, 0		/* PageMask = 4 KiB */
	MTC0	0, 6, 0		/* Wired = 0 */
	MTC0	0, 2, 0
	MTC0	0, 3, 0
	ehb

	MARK	0xA007
	/* Invalidate all TLB entries: unique KSEG0 VPNs, never matched */
	li	$8, 0
	li	$9, TLB_NUM_ENTRIES
	lui	$10, 0x8000
1:	MTC0	8, 0, 0
	sll	$11, $8, 13
	or	$11, $11, $10
	MTC0	11, 10, 0
	ehb
	tlbwi
	ehb
	addiu	$8, $8, 1
	bne	$8, $9, 1b
	nop

	MARK	0xA008
	/*
	 * Wired entries 2 and 4: register bank (core banks) and XPU broadcast
	 * bank, 1 MiB pages, identity, uncached.  No remap needed: the wrapper
	 * decodes the register bank address itself.
	 */
	li	$26, 0x001FE000
	MTC0	26, 5, 0		/* PageMask = 1 MiB */
	li	$8, TLB_WIRED_REGS
	MTC0	8, 0, 0
	li	$26, FW_REGS_VA
	MTC0	26, 10, 0
	li	$26, ((FW_REGS_VA >> 12) << 6) | (2 << 3) | 7
	MTC0	26, 2, 0
	li	$26, (((FW_REGS_VA + 0x100000) >> 12) << 6) | (2 << 3) | 7
	MTC0	26, 3, 0
	ehb
	tlbwi
	ehb
	li	$8, TLB_NUM_WIRED	/* extra slot after the fixed ones: broadcast bank */
	MTC0	8, 0, 0
	li	$26, FW_BCAST_VA
	MTC0	26, 10, 0
	li	$26, ((FW_BCAST_VA >> 12) << 6) | (2 << 3) | 7
	MTC0	26, 2, 0
	li	$26, (((FW_BCAST_VA + 0x100000) >> 12) << 6) | (2 << 3) | 7
	MTC0	26, 3, 0
	ehb
	tlbwi
	ehb
	MTC0	0, 5, 0		/* back to 4 KiB pages */
	ehb

	MARK	0xA009
	li	$1, FW_REGS_VA		/* $1 = register bank */
	li	$25, FW_BOOT_DATA_VA	/* $25 = boot data (uncached KSEG1) */
	li	$26, 0xB0071
	sw	$26, BD_BOOT_STAGE($25)
	sw	$26, CR_DBG_STAGE($1)

	/* Page table: 4 pages at 0xCF000000, TLB slots 0/1, remaps 0,16,1,17 */
	li	$8, 0			/* page index */
2:	sll	$9, $8, 3
	addu	$9, $9, $25
	lw	$10, 16($9)		/* pt_phys_addr[i] low */
	lw	$11, 20($9)		/* pt_phys_addr[i] high */
	PHYS_TO_HI $12, $10, $11, $13
	srl	$14, $8, 1		/* TLB slot = i / 2 */
	andi	$15, $8, 1
	sll	$15, $15, 4
	addu	$14, $14, $15		/* remap entry = slot + 16 * odd */
	sll	$15, $8, 12
	li	$24, FW_PT_VA
	addu	$15, $15, $24		/* VA of page i */
	REMAP	$14, $15, $12, $13, $1
	addiu	$8, $8, 1
	li	$9, 4
	bne	$8, $9, 2b
	nop
	lw	$0, CR_CORE_REVISION($1)

	li	$8, 0			/* slot 0: pages 0/1, slot 1: pages 2/3 */
3:	MTC0	8, 0, 0
	sll	$9, $8, 13
	li	$24, FW_PT_VA
	addu	$9, $9, $24
	MTC0	9, 10, 0
	srl	$10, $9, 12
	sll	$10, $10, 6
	ori	$10, $10, (2 << 3) | 7
	MTC0	10, 2, 0
	addiu	$10, $10, (1 << 6)
	MTC0	10, 3, 0
	ehb
	tlbwi
	ehb
	addiu	$8, $8, 1
	li	$9, 2
	bne	$8, $9, 3b
	nop

	li	$26, 0xB0072
	sw	$26, BD_BOOT_STAGE($25)
	sw	$26, CR_DBG_STAGE($1)

	/* Stack: 2 pages at 0xCF600000 backed by the stack pages of the data object */
	li	$9, FW_PT_VA + (((FW_STACK_HEAP_VA - FW_HEAP_BASE) >> 12) << 2)
	lw	$10, 0($9)		/* PTE of stack page 0 */
	lw	$11, 4($9)		/* PTE of stack page 1 */
	li	$14, TLB_WIRED_STACK
	li	$15, FW_STACK_VA
	PTE_TO_HI $12, $10, $13
	REMAP	$14, $15, $12, $13, $1
	addiu	$14, $14, 16
	addiu	$15, $15, 0x1000
	PTE_TO_HI $12, $11, $13
	REMAP	$14, $15, $12, $13, $1
	lw	$0, CR_CORE_REVISION($1)

	li	$8, TLB_WIRED_STACK
	MTC0	8, 0, 0
	li	$9, FW_STACK_VA
	MTC0	9, 10, 0
	li	$10, ((FW_STACK_VA >> 12) << 6) | (2 << 3) | 7
	MTC0	10, 2, 0
	li	$10, (((FW_STACK_VA + 0x1000) >> 12) << 6) | (2 << 3) | 7
	MTC0	10, 3, 0
	ehb
	tlbwi
	ehb

	li	$8, TLB_NUM_WIRED + 1	/* fixed entries + broadcast bank */
	MTC0	8, 6, 0			/* Wired */
	sw	$8, BD_TLB_NEXT($25)
	sw	$0, BD_IN_EXCEPTION($25)
	ehb

	li	$26, 0xB0073
	sw	$26, BD_BOOT_STAGE($25)
	sw	$26, CR_DBG_STAGE($1)

	/* Fault page remap word (dynamic TLB refill uses it for invalid PTEs) */
	li	$9, FW_SYSINIT_VA
	/* sysinit is in the mapped heap: needs ERL=0 for TLB refill, read later in C */

	li	$sp, FW_STACK_VA + FWL_STACK_SIZE - 16

	/* Leave reset mode: ERET to fw_main with ERL cleared, BEV cleared */
	li	$26, 0x10000004		/* CU0 | ERL */
	MTC0	26, 12, 0
	ehb
	la	$26, fw_entry
	ori	$26, $26, 1		/* microMIPS ISA bit */
	MTC0	26, 30, 0		/* ErrorEPC */
	ehb
	eret
	nop
	.end	_reset
