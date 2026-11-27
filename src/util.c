// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/**
 * Small runtime helpers: memory, time, register polling, the host write
 * barrier and interrupt, and the trace buffer.
 *
 * The firmware links without libc or libgcc, so this file also provides
 * ``memset``, ``memcpy`` and the 64-bit division helpers the compiler calls.
 */
/**/
#include "fw.h"
#include "mips.h"

/**
 * Fill memory with a byte.
 *
 * Args:
 *     d: Destination.
 *     c: Byte value (only the low 8 bits are used).
 *     n: Number of bytes.
 *
 * Returns:
 *     ``d``.
 */
void *memset(void *d, int c, size_t n)
{
	uint8_t *p = d;

	while (n--)
		*p++ = (uint8_t)c;
	return d;
}

/**
 * Copy memory.
 *
 * Copies 32-bit words when both pointers and the length are 4-byte aligned
 * (uncached GPU memory and registers must not be read byte by byte when a
 * word access is possible), bytes otherwise. The areas must not overlap.
 *
 * Args:
 *     d: Destination.
 *     s: Source.
 *     n: Number of bytes.
 *
 * Returns:
 *     ``d``.
 */
void *memcpy(void *d, const void *s, size_t n)
{
	uint8_t *dp = d;
	const uint8_t *sp = s;

	if ((((uintptr_t)dp | (uintptr_t)sp | n) & 3) == 0) {
		uint32_t *dw = d;
		const uint32_t *sw = s;

		for (n >>= 2; n; n--)
			*dw++ = *sw++;
		return d;
	}
	while (n--)
		*dp++ = *sp++;
	return d;
}

/* 64-bit division helpers needed by the compiler (no libgcc). */
uint64_t __udivdi3(uint64_t n, uint64_t d);
uint64_t __umoddi3(uint64_t n, uint64_t d);

/**
 * Unsigned 64-bit division by shift and subtract.
 *
 * Args:
 *     n: Dividend.
 *     d: Divisor. Division by zero returns all ones and a zero remainder.
 *     rem: Where to store the remainder, or ``NULL``.
 *
 * Returns:
 *     The quotient.
 */
static uint64_t udivmod64(uint64_t n, uint64_t d, uint64_t *rem)
{
	uint64_t q = 0, r = 0;

	if (!d) {
		if (rem)
			*rem = 0;
		return ~0ull;
	}
	for (int i = 63; i >= 0; i--) {
		r = (r << 1) | ((n >> i) & 1);
		if (r >= d) {
			r -= d;
			q |= 1ull << i;
		}
	}
	if (rem)
		*rem = r;
	return q;
}

/**
 * Compiler helper for unsigned 64-bit division (``n / d``).
 *
 * Args:
 *     n: Dividend.
 *     d: Divisor.
 *
 * Returns:
 *     The quotient.
 */
uint64_t __udivdi3(uint64_t n, uint64_t d) { return udivmod64(n, d, NULL); }
/**
 * Compiler helper for unsigned 64-bit remainder (``n % d``).
 *
 * Args:
 *     n: Dividend.
 *     d: Divisor.
 *
 * Returns:
 *     The remainder.
 */
uint64_t __umoddi3(uint64_t n, uint64_t d) { uint64_t r; udivmod64(n, d, &r); return r; }

/**
 * Read the GPU timer.
 *
 * ``CR_TIMER`` is read as high, low, high again until both high halves
 * agree, so that a carry between the two 32-bit reads is never seen.
 *
 * Returns:
 *     The 48-bit timer value (it counts at the core clock / 256).
 */
uint64_t gpu_timer(void)
{
	uint32_t hi, lo, hi2;

	do {
		hi = cr_read32(CR_TIMER + 4);
		lo = cr_read32(CR_TIMER);
		hi2 = cr_read32(CR_TIMER + 4);
	} while (hi != hi2);
	return (((uint64_t)hi << 32) | lo) & 0xFFFFFFFFFFFFull;
}

/**
 * Busy-wait.
 *
 * Counts CP0 ``Count`` ticks (core clock / 2), so the delay follows the
 * current clock in :c:var:`g`.
 *
 * Args:
 *     us: Delay in microseconds.
 */
void udelay(uint32_t us)
{
	uint32_t per_us = g.count_per_ms / 1000;
	uint32_t start = mfc0(C0_COUNT, 0);

	if (!per_us)
		per_us = 1;
	while ((uint32_t)(mfc0(C0_COUNT, 0) - start) < us * per_us)
		;
}

/**
 * Poll a GPU register until a masked value appears.
 *
 * On timeout the register, mask and a counter are recorded in ``sysdata``
 * (``cr_poll_*``, read by the host's debug dump) and a ``HWR_13`` trace entry
 * is written.
 *
 * Args:
 *     off: Register offset.
 *     mask: Bits to compare.
 *     value: Expected value of ``reg & mask``.
 *     timeout_us: Timeout in microseconds.
 *
 * Returns:
 *     ``true`` when ``(reg & mask) == value`` was seen, ``false`` on timeout.
 */
bool cr_poll32(uint32_t off, uint32_t mask, uint32_t value, uint32_t timeout_us)
{
	uint32_t per_us = g.count_per_ms / 1000;
	uint32_t start = mfc0(C0_COUNT, 0);

	if (!per_us)
		per_us = 1;
	for (;;) {
		if ((cr_read32(off) & mask) == value)
			return true;
		if ((uint32_t)(mfc0(C0_COUNT, 0) - start) >= timeout_us * per_us)
			break;
	}
	if (g.sysdata) {
		g.sysdata->cr_poll_addr[0] = off;
		g.sysdata->cr_poll_mask[0] = mask;
		g.sysdata->cr_poll_count[0]++;
	}
	TRACE(SFID_HWR_13, value ? 1u : 0u, off, cr_read32(off));   /* 3 args (E037 fix) */
	return false;
}

/**
 * Make every previous write visible to the host.
 *
 * Used before publishing a CCB offset or raising an interrupt: writes the
 * uncached barrier word, runs ``EHB`` and ``SYNC``, reads the word back, then
 * reads a register so that the bus is drained.
 */
void write_barrier(void)
{
	volatile uint32_t *w = boot_data_word(BD_BARRIER);

	*w = 1;
	mips_ehb();
	mips_sync();
	(void)*w;
	(void)cr_read32(CR_CORE_REVISION);
}

/**
 * Raise the firmware-to-host interrupt.
 *
 * Increments ``osdata->interrupt_count[0]`` (the host uses it to recognise
 * a new interrupt) between two write barriers, then sets the MIPS wrapper
 * interrupt status.
 */
void notify_host(void)
{
	write_barrier();
	if (g.osdata) {
		g.osdata->interrupt_count[0]++;
		write_barrier();
	}
	cr_write32(CR_MIPS_WRAPPER_IRQ_STATUS, 1);
}

/* ------------------------------------------------------------- trace */
/**
 * Locate the trace buffer.
 *
 * Takes the address and size of thread 0's buffer from the trace control
 * structure. Tracing stays off when either is zero; a write pointer outside
 * the buffer is reset to 0.
 */
void trace_init(void)
{
	struct tracebuf *tb = g.tracebuf;

	if (!tb)
		return;
	g.trace_buf = fwptr(tb->tb[0].trace_buffer_fw_addr);
	g.trace_size = tb->tracebuf_size_in_dwords;
	if (!g.trace_buf || !g.trace_size)
		g.trace_buf = NULL;
	else if (tb->tb[0].trace_pointer >= g.trace_size)
		tb->tb[0].trace_pointer = 0;
}

/**
 * Map an SFID group number to its log-type group bit.
 *
 * Args:
 *     gid: Group number, bits 12..15 of the SFID.
 *
 * Returns:
 *     The ``ROGUE_FWIF_LOG_TYPE_GROUP_*`` bit that enables the group.
 */
static uint32_t group_bit(uint32_t gid)
{
	/* SFID group number -> ROGUE_FWIF_LOG_TYPE_GROUP_* bit */
	static const uint32_t map[16] = {
		0, 1u << 1, 1u << 3, 1u << 4, 1u << 6, 1u << 7, 1u << 8, 1u << 2,
		1u << 5, 1u << 14, 1u << 9, 1u << 10, 1u << 11, 1u << 12, 1u << 13, 1u << 31,
	};

	return map[gid & 0xF];
}

/**
 * Append one entry to the trace ring.
 *
 * Nothing is written unless tracing is on (``log_type`` bit 0) and the
 * SFID's group is enabled. The entry is the SFID, the 64-bit GPU timer
 * (high word first) and the parameters; it may wrap around the end of the
 * ring. Interrupts are masked while the entry is written. Normally called
 * through the :c:macro:`TRACE` macro.
 *
 * Args:
 *     sfid: String format identifier (``SFID_*``).
 *     params: The parameters.
 *     n: Number of parameters; it must match the SFID's format string.
 */
void trace_emit(uint32_t sfid, const uint32_t *params, uint32_t n)
{
	struct tracebuf *tb = g.tracebuf;
	uint32_t s, ptr;
	uint64_t ts;

	if (!g.trace_buf)
		return;
	if (!(tb->log_type & 1))
		return;
	if (!(tb->log_type & group_bit((sfid >> 12) & 0xF)))
		return;

	s = irq_save();
	ts = gpu_timer();
	ptr = tb->tb[0].trace_pointer;
	if (ptr >= g.trace_size)
		ptr = 0;
	/* the host reads the ring modulo its size: entries may wrap */
#define TPUT(v) do { g.trace_buf[ptr] = (v); if (++ptr >= g.trace_size) ptr = 0; } while (0)
	TPUT(sfid);
	TPUT((uint32_t)(ts >> 32));
	TPUT((uint32_t)ts);
	for (uint32_t i = 0; i < n; i++)
		TPUT(params[i]);
#undef TPUT
	tb->tb[0].trace_pointer = ptr;
	irq_restore(s);
}
