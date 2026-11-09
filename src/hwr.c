// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/*
 * Hardware recovery (docsClean/hardware-recovery.md, "Reset sequence").
 *
 * Triggered by an MMU page fault (IRQ task or poll) or by the watchdog when a
 * DM has been busy for longer than HWR_TIMEOUT_MS.  The GPU units are reset
 * with a SOFT_RESET pulse that leaves the MIPS core and the MTS wrapper alone,
 * re-initialised, and every running command is discarded (its updates still
 * run, so the host is never left waiting).
 */
#include "kick.h"

#define CR_PM_PARTIAL_RENDER_ENABLE 0x0338
#define SOFT_RESET_HWR_LO           0x13FFFC1Du
#define SOFT_RESET_HWR_HI           0xFFEFFE00u

/*
 * Watchdog: the context's max_deadline_ms (the mainline host sets 30 s; its
 * own job timeout is 60 s).  2 s was too short for large dispatches (E020).
 */
#define HWR_TIMEOUT_DEFAULT_MS      30000

static uint32_t hwr_counter;

/* ext_job_ref of the command a DM is running (job ref reported to the host) */
static uint32_t running_job_ref(const struct dm_state *s)
{
	struct fwcommoncontext *ctx = fwptr(s->ctx);
	uint8_t *ccb = ctx ? fwptr(ctx->ccb_fw_addr) : NULL;

	if (!ccb)
		return 0;
	return ((struct ccb_cmd_header *)(ccb + s->cmd_off))->ext_job_ref;
}

void hwr_recover(uint32_t reason, uint64_t mmu_status, uint64_t req_status)
{
	uint32_t busy = 0;
	uint32_t ev = cr_read32(CR_EVENT_STATUS);
	uint64_t reset_start = gpu_timer();
	struct {
		fwaddr_t ctx;
		uint64_t kick_time, pc;
		uint32_t job_ref, pid;
	} lost[DM_MAX];

	for (uint32_t d = 0; d < DM_MAX; d++) {
		if (!dms[d].busy)
			continue;
		struct fwcommoncontext *ctx = fwptr(dms[d].ctx);

		busy |= 1u << d;
		lost[d].ctx = dms[d].ctx;
		lost[d].kick_time = dms[d].kick_time;
		lost[d].pc = kick_bif_pc(dms[d].bif_slot);
		lost[d].job_ref = running_job_ref(&dms[d]);
		lost[d].pid = ctx ? ctx->pid : 0;
	}

	/* 1. hold scheduling */
	if (g.sysdata) {
		g.sysdata->hwr_state_flags &= ~HWR_HARDWARE_OK;
		g.sysdata->hwr_state_flags |= HWR_RESET_IN_PROGRESS |
			(reason == HWR_REASON_LOCKUP ? HWR_GENERAL_LOCKUP : 0);
	}
	for (uint32_t d = 0; d < DM_MAX; d++) {
		if (busy & (1u << d)) {
			TRACE(SFID_HWR_32, d);
			TRACE(SFID_HWR_28, d, dms[d].ctx);
		}
	}
	TRACE(SFID_HWR_61, hwr_counter);

	/* 2. SLC flush/invalidate and wait for idle */
	slc_flush_inval();

	/* 3. disable partial renders */
	cr_write32(CR_PM_PARTIAL_RENDER_ENABLE, 0);

	/* 4. soft-reset pulse of the GPU units (not the MIPS / MTS wrapper) */
	bcast_write32(CR_SOFT_RESET, SOFT_RESET_HWR_LO);
	bcast_write32(CR_SOFT_RESET + 4, SOFT_RESET_HWR_HI);
	(void)cr_read32(CR_SOFT_RESET);
	udelay(10);
	bcast_write32(CR_SOFT_RESET, 0);
	bcast_write32(CR_SOFT_RESET + 4, 0);
	(void)cr_read32(CR_SOFT_RESET);
	TRACE(SFID_MAIN_27);

	/* 5. re-initialise the units, event routing and the BIF catalogue */
	gpu_power_on();
	gpu_units_init();
	bif_rebuild();

	/* 6. per-DM recovery: discard the guilty commands */
	cr_write32(CR_EVENT_CLEAR, 0xFFFFFFFFu);
	for (uint32_t d = 0; d < DM_MAX; d++) {
		if (!(busy & (1u << d)))
			continue;
		TRACE(SFID_HWR_14, d, dms[d].ctx);
		cr_write32(CR_SCRATCH15, cr_read32(CR_SCRATCH15) & ~(1u << d));
		sched_dm_done(d);
		TRACE(SFID_HWR_25, d);
	}

	/*
	 * Host-visible record (HWR log) and robustness notification per lost job
	 * (hardware-recovery.md "HWR log", "Context robustness").
	 */
	uint64_t reset_finish = gpu_timer();
	bool pf = reason == HWR_REASON_MMU_FAULT;
	uint64_t fault_addr = req_status & 0xFFFFFFFFF0ull;      /* REQ_STATUS.ADDRESS */

	for (uint32_t d = 0; d < DM_MAX; d++) {
		if (!(busy & (1u << d)))
			continue;
		stats_hwr_log(d, pf ? HWRTYPE_BIF0FAULT : HWRTYPE_OVERRUN,
			      pf ? req_status : 0, pf ? mmu_status : 0, lost[d].pc, ev,
			      lost[d].kick_time, reset_start, reset_finish, lost[d].pid);
		stats_notify_context_reset(lost[d].ctx, d,
					   pf ? CONTEXT_RESET_REASON_GUILTY_LOCKUP :
						CONTEXT_RESET_REASON_GUILTY_OVERRUNING,
					   lost[d].job_ref, lost[d].pc, pf, pf ? fault_addr : 0);
	}

	TRACE(SFID_HWR_62, hwr_counter);
	hwr_counter++;
	cr_write32(CR_DBG_HWR_COUNT, hwr_counter);

	/* 7. resume */
	if (g.sysdata)
		g.sysdata->hwr_state_flags = HWR_HARDWARE_OK | HWR_DM_RUNNING_OK;
	sched_run();
}

/* Called from the timer tick. */
void hwr_watchdog(void)
{
	uint64_t now = gpu_timer();

	for (uint32_t d = 0; d < DM_MAX; d++) {
		if (!dms[d].busy)
			continue;
		struct fwcommoncontext *ctx = fwptr(dms[d].ctx);
		uint32_t ms = ctx && ctx->max_deadline_ms ? ctx->max_deadline_ms :
			      HWR_TIMEOUT_DEFAULT_MS;
#ifdef EXP_WATCHDOG_MS
		ms = EXP_WATCHDOG_MS;   /* E025: force the overrun path */
#endif
		uint64_t limit = (uint64_t)g.timer_ticks_per_ms * ms;

		if (now - dms[d].kick_time > limit) {
			TRACE(SFID_HWR_1, d, dms[d].ctx);
			hwr_recover(HWR_REASON_LOCKUP, 0, 0);
			return;
		}
	}
}
