/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Thalles Campagnani */
/* Firmware image layout (addresses must match include/layout.h). */
OUTPUT_ARCH(mips)
ENTRY(_reset)

PHDRS
{
	boot PT_LOAD;
	exc  PT_LOAD;
	text PT_LOAD;
	data PT_LOAD;
}

SECTIONS
{
	.boot 0xBFC00000 : { KEEP(*(.boot)) } :boot
	.exc  0x9FC02000 : { KEEP(*(.exc)) } :exc
	.text 0xC0000000 : {
		*(.text .text.*)
		*(.rodata .rodata.* .srodata*)
	} :text
	__text_end = .;

	.data 0xC0ED0000 : {
		*(.data .data.* .sdata .sdata.*)
		. = ALIGN(4);
	} :data
	.bss (NOLOAD) : {
		__bss_start = .;
		*(.sbss .sbss.* .bss .bss.* COMMON)
		. = ALIGN(4);
		__bss_end = .;
	} :data

	/DISCARD/ : {
		*(.MIPS.abiflags) *(.reginfo) *(.MIPS.options) *(.pdr)
		*(.comment) *(.note*) *(.gnu.attributes)
	}
}

ASSERT(SIZEOF(.boot) <= 0x1000, "boot code larger than one page")
ASSERT(SIZEOF(.exc) <= 0x1000, "exception code larger than one page")
ASSERT(__text_end <= 0xC0010000, "code larger than FWL_CODE_SIZE")
ASSERT(__bss_end <= 0xC0EE0000, "data larger than FWL_DATA_SIZE")
