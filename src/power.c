// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/*
 * Power management: KCCB POW requests and the host power-sync handshake.
 *
 * The host writes 0 to the power-sync word, sends POW and polls the word;
 * the firmware writes 1 only once the requested transition is complete.
 */
#include "fw.h"
#include "mips.h"

/* internal "pow flags" (only printed in the trace) */
#define PF_OFF_PENDING   0x10000u
#define PF_FORCED_IDLE   0x40000u
#define PF_FORCED_REQ    0x80000u

static uint32_t pow_flags;

void pow_set_state(uint32_t s)
{
	g.pow_state = s;
	if (g.sysdata)
		g.sysdata->pow_state = s;
}

static void power_sync_signal(void)
{
	volatile uint32_t *sync = g.osdata ? fwptr(g.osdata->power_sync_fw_addr) : NULL;

	write_barrier();
	if (sync)
		*sync = 1;
	notify_host();
}

static void gpu_teardown(void)
{
	TRACE(SFID_MAIN_145);
	TRACE(SFID_MAIN_144);
	/* no dynamic MIPS mapping must survive into the next boot */
	tlb_flush_dynamic();
}

void pow_handle(const struct kccb_cmd *cmd)
{
	uint32_t type = cmd->data.pow.pow_type;
	uint32_t arg = cmd->data.pow.data;

	switch (type) {
	case POW_FORCED_IDLE_REQ:
		if (arg == POWER_FORCE_IDLE || arg == POWER_HOST_TIMEOUT) {
			TRACE(SFID_POW_19, pow_flags);
			/* Stop accepting work, wait until the DMs are idle. */
			g.forced_idle = true;
			pow_flags |= PF_FORCED_IDLE;
			sched_run();
			pow_set_state(POW_STATE_FORCED_IDLE);
		} else {
			TRACE(SFID_POW_20, pow_flags);
			g.forced_idle = false;
			pow_flags &= ~PF_FORCED_IDLE;
			pow_set_state(POW_STATE_IDLE);
		}
		power_sync_signal();
		break;

	case POW_OFF_REQ:
		pow_flags |= PF_OFF_PENDING | (arg ? PF_FORCED_REQ : 0);
		TRACE(SFID_POW_59, arg, pow_flags);
		gpu_teardown();
		stats_set_state(GPU_UTIL_STATE_IDLE);
		pow_set_state(POW_STATE_OFF);
		TRACE(SFID_POW_6, g.pow_state, POW_STATE_OFF, pow_flags);
		g.powered_off = true;
		/* stop the polling tick: nothing must run until the next boot */
		mtc0(C0_STATUS, 0, mfc0(C0_STATUS, 0) & ~0x0400u);
		power_sync_signal();
		pow_flags = 0;
		break;

	case POW_NUM_UNITS_CHANGE:
	case POW_APM_LATENCY_CHANGE:
	default:
		power_sync_signal();
		break;
	}
}
