/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Thalles Campagnani */
/* Internal firmware declarations. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fwif.h"
#include "layout.h"
#include "regs.h"
#include "sfid.h"

/* ------------------------------------------------------------ registers */
static inline uint32_t cr_read32(uint32_t off)
{
	return *(volatile uint32_t *)(FW_REGS_VA + off);
}

static inline void cr_write32(uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(FW_REGS_VA + off) = v;
}

static inline uint64_t cr_read64(uint32_t off)
{
	uint32_t lo = cr_read32(off);
	uint32_t hi = cr_read32(off + 4);

	return ((uint64_t)hi << 32) | lo;
}

static inline void cr_write64(uint32_t off, uint64_t v)
{
	cr_write32(off, (uint32_t)v);
	cr_write32(off + 4, (uint32_t)(v >> 32));
}

static inline void bcast_write32(uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(FW_BCAST_VA + off) = v;
}

static inline void bcast_write64(uint32_t off, uint64_t v)
{
	bcast_write32(off, (uint32_t)v);
	bcast_write32(off + 4, (uint32_t)(v >> 32));
}

static inline volatile uint32_t *boot_data_word(uint32_t off)
{
	return (volatile uint32_t *)(FW_BOOT_DATA_VA + off);
}

/* Poll a register; returns true when (reg & mask) == value. */
bool cr_poll32(uint32_t off, uint32_t mask, uint32_t value, uint32_t timeout_us);

/* ------------------------------------------------------------ addresses */
/* Firmware address from the host -> pointer (null stays null). */
static inline void *fwptr(fwaddr_t a)
{
	if (!a)
		return NULL;
	return (void *)(uintptr_t)(a & FW_VA_MASK);
}

/* ------------------------------------------------------------ state */
struct fw_state {
	struct sysinit *sysinit;
	struct osinit *osinit;
	struct sysdata *sysdata;
	struct osdata *osdata;
	struct runtime_cfg *runtime_cfg;
	struct tracebuf *tracebuf;
	volatile uint32_t *trace_buf;
	uint32_t trace_size;

	struct ccb_ctl *kccb_ctl;
	uint8_t *kccb;
	volatile uint32_t *kccb_rtn;
	struct ccb_ctl *fwccb_ctl;
	uint8_t *fwccb;

	uint32_t core_clock_hz;
	uint32_t timer_ticks_per_ms;   /* CR_TIMER ticks per ms (clock / 256) */
	uint32_t count_per_ms;         /* CP0 Count per ms (clock / 2) */

	uint32_t pow_state;
	bool forced_idle;
	bool powered_off;
	bool kick_pending;             /* set by BG/IRQ, work for the scheduler */
	uint32_t config_flags;
	uint32_t timer_irqs, bg_irqs, irq_irqs, spurious_irqs;
	uint32_t cdm_kicks;
};

extern struct fw_state g;

/* ------------------------------------------------------------ helpers */
void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
uint64_t gpu_timer(void);
void udelay(uint32_t us);

void write_barrier(void);
void notify_host(void);

/* trace */
void trace_init(void);
void trace_emit(uint32_t sfid, const uint32_t *params, uint32_t n);
#define TRACE(sfid, ...) do { \
	const uint32_t __p[] = { 0, ##__VA_ARGS__ }; \
	trace_emit((sfid), __p + 1, (sizeof(__p) / sizeof(__p[0])) - 1); \
} while (0)

/* kccb / fwccb */
void kccb_process(void);
bool fwccb_send(const struct fwccb_cmd *cmd);

/* power */
void pow_handle(const struct kccb_cmd *cmd);
void pow_set_state(uint32_t s);

/* scheduler / client CCBs */
void sched_init(void);
uint32_t sched_kick(const struct kick_data *kick);
uint32_t sched_cleanup(uint32_t type, fwaddr_t addr);
void sched_run(void);
void sched_irq(uint32_t events);
void sched_force_update(fwaddr_t ctx, uint32_t fence_offset);

/* GPU bring-up (gpu_init.c) */
void gpu_power_on(void);
void gpu_units_init(void);
bool gpu_units_ready(void);
bool dust_power(bool on);

/* mmu */
void mmu_cache_op(const struct kccb_cmd *cmd);
void slc_flush_inval(void);
void tlb_flush_dynamic(void);

/* entry points */
void fw_main(void);
void irq_dispatch(uint32_t source);
void fatal_exception(void);
