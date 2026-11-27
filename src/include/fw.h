/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Thalles Campagnani */
/**
 * Internal firmware declarations: register and boot-data accessors, the
 * global state :c:var:`g`, the trace macro, and the functions shared between
 * the source files (documented where they are defined).
 */
/**/
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fwif.h"
#include "layout.h"
#include "regs.h"
#include "sfid.h"

/* ------------------------------------------------------------ registers */
/**
 * Read a 32-bit GPU register (core bank).
 *
 * Args:
 *     off: Register offset.
 *
 * Returns:
 *     The register value.
 */
static inline uint32_t cr_read32(uint32_t off)
{
	return *(volatile uint32_t *)(FW_REGS_VA + off);
}

/**
 * Write a 32-bit GPU register (core bank).
 *
 * Args:
 *     off: Register offset.
 *     v: The value.
 */
static inline void cr_write32(uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(FW_REGS_VA + off) = v;
}

/**
 * Read a 64-bit GPU register, low word first.
 *
 * Args:
 *     off: Register offset.
 *
 * Returns:
 *     The register value.
 */
static inline uint64_t cr_read64(uint32_t off)
{
	uint32_t lo = cr_read32(off);
	uint32_t hi = cr_read32(off + 4);

	return ((uint64_t)hi << 32) | lo;
}

/**
 * Write a 64-bit GPU register, low word first.
 *
 * Args:
 *     off: Register offset.
 *     v: The value.
 */
static inline void cr_write64(uint32_t off, uint64_t v)
{
	cr_write32(off, (uint32_t)v);
	cr_write32(off + 4, (uint32_t)(v >> 32));
}

/**
 * Write a 32-bit GPU register through the XPU broadcast bank (every core).
 *
 * Args:
 *     off: Register offset.
 *     v: The value.
 */
static inline void bcast_write32(uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(FW_BCAST_VA + off) = v;
}

/**
 * Write a 64-bit GPU register through the broadcast bank, low word first.
 *
 * Args:
 *     off: Register offset.
 *     v: The value.
 */
static inline void bcast_write64(uint32_t off, uint64_t v)
{
	bcast_write32(off, (uint32_t)v);
	bcast_write32(off + 4, (uint32_t)(v >> 32));
}

/**
 * Get a word of the boot-data page (uncached KSEG1).
 *
 * Args:
 *     off: Byte offset (``BD_*``).
 *
 * Returns:
 *     Pointer to the word.
 */
static inline volatile uint32_t *boot_data_word(uint32_t off)
{
	return (volatile uint32_t *)(FW_BOOT_DATA_VA + off);
}

/* Poll a register; returns true when (reg & mask) == value. */
bool cr_poll32(uint32_t off, uint32_t mask, uint32_t value, uint32_t timeout_us);

/* ------------------------------------------------------------ addresses */
/**
 * Turn a firmware address from the host into a pointer.
 *
 * Args:
 *     a: The firmware address.
 *
 * Returns:
 *     The pointer in the heap window, or ``NULL`` when ``a`` is 0.
 */
static inline void *fwptr(fwaddr_t a)
{
	if (!a)
		return NULL;
	return (void *)(uintptr_t)(a & FW_VA_MASK);
}

/* ------------------------------------------------------------ state */
/** The firmware's global state, :c:var:`g`. */
struct fw_state {
	/** The host's system initialisation data. */
	struct sysinit *sysinit;
	/** The host's OS initialisation data. */
	struct osinit *osinit;
	/** System data reported to the host. */
	struct sysdata *sysdata;
	/** OS data reported to the host. */
	struct osdata *osdata;
	/** Runtime configuration written by the host. */
	struct runtime_cfg *runtime_cfg;
	/** Trace control. */
	struct tracebuf *tracebuf;
	/** Thread 0's trace ring, or ``NULL`` when tracing is off. */
	volatile uint32_t *trace_buf;
	/** Size of the trace ring in words. */
	uint32_t trace_size;

	/** Kernel CCB control block. */
	struct ccb_ctl *kccb_ctl;
	/** Kernel CCB commands. */
	uint8_t *kccb;
	/** Kernel CCB return slots. */
	volatile uint32_t *kccb_rtn;
	/** Firmware CCB control block. */
	struct ccb_ctl *fwccb_ctl;
	/** Firmware CCB commands. */
	uint8_t *fwccb;

	/** Core clock in Hz. */
	uint32_t core_clock_hz;
	/** ``CR_TIMER`` ticks per ms (clock / 256). */
	uint32_t timer_ticks_per_ms;   /* CR_TIMER ticks per ms (clock / 256) */
	/** CP0 ``Count`` ticks per ms (clock / 2). */
	uint32_t count_per_ms;         /* CP0 Count per ms (clock / 2) */

	/** Power state (``POW_STATE_*``). */
	uint32_t pow_state;
	/** The host forced the GPU idle: no new task starts. */
	bool forced_idle;
	/** Powered off: interrupts do nothing until the next boot. */
	bool powered_off;
	/** Set by the background or interrupt task: work for the scheduler (not used). */
	bool kick_pending;             /* set by BG/IRQ, work for the scheduler */
	/** Configuration flags (``sysdata.config_flags``). */
	uint32_t config_flags;
	/** Interrupt counters (debug). */
	uint32_t timer_irqs, bg_irqs, irq_irqs, spurious_irqs;
	/** Compute kicks (debug). */
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
/**
 * Write a trace entry.
 *
 * Args:
 *     sfid: The ``SFID_*`` identifier.
 *     ...: The parameters, as many as the SFID's format string expects
 *         (``tools/check-trace-args.py`` checks this at build time).
 */
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
void sched_ufo_signal(const struct ufo *u);

/* GPU bring-up (gpu_init.c) */
void gpu_power_on(void);
void gpu_units_init(void);
bool gpu_units_ready(void);
bool dust_power(bool on);

/* statistics, time correlation, HWR log, robustness (stats.c) */
void stats_init(void);
void stats_time_corr(void);
uint64_t stats_now_ns(void);
void stats_set_state(uint32_t state);
void stats_hwr_log(uint32_t dm, uint32_t hwr_type, uint64_t req, uint64_t mmu,
		   uint64_t pc, uint32_t event_status, uint64_t kick_time,
		   uint64_t reset_start, uint64_t reset_finish, uint32_t pid);
void stats_notify_context_reset(fwaddr_t ctxaddr, uint32_t dm, uint32_t reason,
				uint32_t job_ref, uint64_t pc, bool page_fault,
				uint64_t fault_addr);

/* mmu */
void mmu_cache_op(const struct kccb_cmd *cmd);
void slc_flush_inval(void);
void tlb_flush_dynamic(void);
bool gpu_read(fwaddr_t memctx, uint64_t va, void *buf, uint32_t len);
bool gpu_zero(fwaddr_t memctx, uint64_t va, uint32_t len);

/* entry points */
void fw_main(void);
void irq_dispatch(uint32_t source);
void fatal_exception(void);
