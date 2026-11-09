// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/*
 * GPU utilisation statistics, time correlation and the HWR log
 * (docsClean/host-interface.md "Time correlation", data-structures.md,
 * hardware-recovery.md "HWR log"; layouts from the public pvr_rogue_fwif.h).
 *
 * Time base.  The mainline host does not write time-correlation entries, so
 * the firmware keeps its own "OS time": nanoseconds derived from ROGUE_CR_TIMER
 * (core clock / 256).  The conversion factor is published in time_corr[] the
 * way the public header defines it: delta_ns = (delta_cr * K) >> 20.
 * ROGUE_CR_TIMER restarts when the GPU domain is powered off; the utilisation
 * accounting never subtracts across such a jump (GPU_UTIL_GET_PERIOD rule).
 */
#include "kick.h"

static struct gpu_util_fwcb *util;
static struct hwrinfobuf *hwrbuf;
static uint64_t cr_to_ns_k;             /* (256 * 1e9 << 20) / core clock */

static uint64_t cr_to_ns(uint64_t cr)
{
	/* split to keep the product inside 64 bits for any 48-bit timer value */
	uint64_t hi = cr >> 20, lo = cr & ((1u << 20) - 1);

	return hi * cr_to_ns_k + ((lo * cr_to_ns_k) >> CRDELTA_ACCURACY_SHIFT);
}

uint64_t stats_now_ns(void)
{
	return cr_to_ns(gpu_timer());
}

/* Publish a time-correlation entry for the current clock. */
void stats_time_corr(void)
{
	if (!g.core_clock_hz)
		return;
	cr_to_ns_k = (256ull * 1000000000ull << CRDELTA_ACCURACY_SHIFT) / g.core_clock_hz;
	if (!util)
		return;

	uint32_t seq = util->time_corr_seq_count + 1;
	struct time_corr *tc = &util->time_corr[seq % TIME_CORR_ARRAY_SIZE];
	uint64_t cr = gpu_timer();

	tc->cr_timestamp = cr;
	tc->os_timestamp = cr_to_ns(cr);
	tc->os_mono_timestamp = tc->os_timestamp;
	tc->cr_delta_to_os_delta_kns = cr_to_ns_k;
	tc->core_clock_speed = g.core_clock_hz;
	write_barrier();
	util->time_corr_seq_count = seq;
}

void stats_init(void)
{
	util = fwptr(g.sysinit->gpu_util_fw_cb_ctl_fw_addr);
	hwrbuf = fwptr(g.osinit->hwr_info_buf_ctl_fw_addr);
	stats_time_corr();
	if (util) {
		/* new time base after (re)boot: restart the state word, keep the counters */
		util->last_word = (stats_now_ns() & ~GPU_UTIL_STATE_MASK) | GPU_UTIL_STATE_IDLE;
	}
}

/* Account the time spent in the previous state and switch to `state`. */
void stats_set_state(uint32_t state)
{
	if (!util)
		return;

	uint64_t w = util->last_word;
	uint32_t old = (uint32_t)(w & GPU_UTIL_STATE_MASK);

	if (old == state)
		return;

	uint64_t now = stats_now_ns() & ~GPU_UTIL_STATE_MASK;
	uint64_t then = w & ~GPU_UTIL_STATE_MASK;

	if (then && now > then)
		util->stats_counters[old] += now - then;
	util->last_word = now | state;
	write_barrier();

	if (state == GPU_UTIL_STATE_IDLE) {
		if (g.sysdata)
			g.sysdata->start_idle_time = now;
		TRACE(SFID_POW_2, g.pow_state, g.pow_state, state);
		/* debug group: idle / active / blocked totals in microseconds */
		TRACE(SFID_DBG_20, (uint32_t)(util->stats_counters[0] / 1000),
		      (uint32_t)(util->stats_counters[1] / 1000),
		      (uint32_t)(util->stats_counters[2] / 1000));
	} else if (old == GPU_UTIL_STATE_IDLE) {
		TRACE(SFID_POW_22, g.pow_state, g.pow_state, state);
	}
}

/*
 * Append one entry to the host-visible HWR log: the first 8 entries are kept
 * forever, the last 8 form a ring (hardware-recovery.md).
 */
void stats_hwr_log(uint32_t dm, uint32_t hwr_type, uint64_t req, uint64_t mmu,
		   uint64_t pc, uint32_t event_status, uint64_t kick_time,
		   uint64_t reset_start, uint64_t reset_finish, uint32_t pid)
{
	if (!hwrbuf)
		return;

	uint32_t idx = hwrbuf->write_index;
	struct hwrinfo *h = &hwrbuf->hwr_info[idx];

	memset(h, 0, sizeof(*h));
	h->bif_req_status = req;
	h->bif_mmu_status = mmu;
	h->pc_address = pc;
	h->cr_timer = reset_start;
	h->os_timer = cr_to_ns(reset_start);
	h->pid = pid;
	h->hwr_number = hwrbuf->hwr_counter;
	h->event_status = event_status;
	h->hwr_recovery_flags = g.sysdata ? g.sysdata->hwr_state_flags : 0;
	h->hwr_type = hwr_type;
	h->dm = dm;
	h->core_id = 0;
	h->cr_time_of_kick = kick_time;
	h->cr_time_hw_reset_start = reset_start;
	h->cr_time_hw_reset_finish = reset_finish;

	if (++idx >= HWINFO_MAX)
		idx = HWINFO_MAX_FIRST;
	hwrbuf->write_index = idx;
	hwrbuf->hwr_counter++;
	if (dm < FW_DM_MAX) {
		if (hwr_type == HWRTYPE_OVERRUN)
			hwrbuf->hwr_dm_overran_count[dm]++;
		else
			hwrbuf->hwr_dm_locked_up_count[dm]++;
		hwrbuf->hwr_dm_recovered_count[dm]++;
	}
	write_barrier();
}

/* Tell the host which context lost a job (FWCCB CONTEXT_RESET_NOTIFICATION). */
void stats_notify_context_reset(fwaddr_t ctxaddr, uint32_t dm, uint32_t reason,
				uint32_t job_ref, uint64_t pc, bool page_fault,
				uint64_t fault_addr)
{
	struct fwcommoncontext *ctx = fwptr(ctxaddr);
	struct fwccb_cmd cmd;
	struct fwccb_context_reset r;

	memset(&cmd, 0, sizeof(cmd));
	memset(&r, 0, sizeof(r));
	r.server_common_context_id = ctx ? ctx->server_common_context_id : 0;
	r.reset_reason = reason;
	r.dm = dm;
	r.reset_job_ref = job_ref;
	r.flags = page_fault ? CONTEXT_RESET_FLAG_PF : 0;
	r.pc_address = pc;
	r.fault_address = fault_addr;
	cmd.cmd_type = ROGUE_CMD_MAGIC(FWCCB_CMD_CONTEXT_RESET_NOTIFICATION);
	memcpy(cmd.data, &r, sizeof(r));
	fwccb_send(&cmd);
}
