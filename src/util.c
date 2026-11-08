// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/* Small runtime helpers: memory, time, barrier, host notification, trace. */
#include "fw.h"
#include "mips.h"

void *memset(void *d, int c, size_t n)
{
	uint8_t *p = d;

	while (n--)
		*p++ = (uint8_t)c;
	return d;
}

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

uint64_t __udivdi3(uint64_t n, uint64_t d) { return udivmod64(n, d, NULL); }
uint64_t __umoddi3(uint64_t n, uint64_t d) { uint64_t r; udivmod64(n, d, &r); return r; }

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

void udelay(uint32_t us)
{
	uint32_t per_us = g.count_per_ms / 1000;
	uint32_t start = mfc0(C0_COUNT, 0);

	if (!per_us)
		per_us = 1;
	while ((uint32_t)(mfc0(C0_COUNT, 0) - start) < us * per_us)
		;
}

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
	TRACE(SFID_HWR_13, off, cr_read32(off));
	return false;
}

/*
 * Make every previous write visible to the host before publishing an
 * offset or raising an interrupt: uncached barrier word, EHB, SYNC,
 * read-back, then a register read that drains the bus.
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

static uint32_t group_bit(uint32_t gid)
{
	/* SFID group number -> ROGUE_FWIF_LOG_TYPE_GROUP_* bit */
	static const uint32_t map[16] = {
		0, 1u << 1, 1u << 3, 1u << 4, 1u << 6, 1u << 7, 1u << 8, 1u << 2,
		1u << 5, 1u << 14, 1u << 9, 1u << 10, 1u << 11, 1u << 12, 1u << 13, 1u << 31,
	};

	return map[gid & 0xF];
}

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
