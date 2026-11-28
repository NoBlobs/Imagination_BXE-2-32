// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/**
 * The Kernel CCB (host to firmware) and the Firmware CCB (firmware to host).
 *
 * Both are rings of fixed-size commands with a control block of read and
 * write offsets and a wrap mask. The firmware owns the KCCB read offset and
 * the FWCCB write offset.
 */
/**/
#include "fw.h"
#include "mips.h"
#include "pm.h"

/**
 * Handle ``CORECLKSPEEDCHANGE``.
 *
 * Takes the new clock from the runtime configuration, recomputes the timer
 * rates and re-correlates the GPU and host time. A zero clock is ignored.
 */
static void clock_change(void)
{
	uint32_t hz = g.runtime_cfg ? g.runtime_cfg->core_clock_speed : 0;

	if (!hz)
		return;
	g.core_clock_hz = hz;
	g.timer_ticks_per_ms = hz / 256 / 1000 ? hz / 256 / 1000 : 1;
	g.count_per_ms = hz / 2 / 1000 ? hz / 2 / 1000 : 1;
	stats_time_corr();
	TRACE(SFID_MAIN_93, hz);
}

/**
 * Execute one Kernel CCB command.
 *
 * Commands without the ``ROGUE_CMD_MAGIC_DWORD`` in the upper half of
 * ``cmd_type`` are traced and skipped. Kicks, cleanups, MMU and SLC cache
 * operations, power requests, force updates, clock changes, state-flag
 * changes and free-list grow updates are handled; the other known commands
 * are acknowledged without action.
 *
 * Args:
 *     cmd: The command.
 *
 * Returns:
 *     The return-slot value: ``KCCB_RTN_CMD_EXECUTED``, plus the flags the
 *     handler adds (for example ``KCCB_RTN_CLEANUP_BUSY``).
 */
static uint32_t kccb_handle(const struct kccb_cmd *cmd)
{
	uint32_t rtn = KCCB_RTN_CMD_EXECUTED;

	if ((cmd->cmd_type >> 16) != ROGUE_CMD_MAGIC_DWORD) {
		TRACE(SFID_MAIN_150, cmd->cmd_type);
		return rtn;
	}

	switch (cmd->cmd_type & 0xFFFF) {
	case KCCB_KICK:
		rtn |= sched_kick(&cmd->data.kick);
		break;
	case KCCB_COMBINED_GEOM_FRAG_KICK:
		rtn |= sched_kick(&cmd->data.combined.geom);
		rtn |= sched_kick(&cmd->data.combined.frag);
		break;
	case KCCB_MMUCACHE:
		mmu_cache_op(cmd);
		break;
	case KCCB_CLEANUP:
		rtn |= sched_cleanup(cmd->data.cleanup.cleanup_type, cmd->data.cleanup.fw_addr);
		break;
	case KCCB_POW:
		pow_handle(cmd);
		break;
	case KCCB_FORCE_UPDATE:
		sched_force_update(cmd->data.force_update.context_fw_addr,
				   cmd->data.force_update.ccb_fence_offset);
		break;
	case KCCB_SLCFLUSHINVAL: {
		/*
		 * The SLC flush/invalidate control works on the whole cache
		 * (SLC_CTRL_FLUSH_INVAL.ALL = flush + invalidate), a superset of any
		 * address range or DM context the host names.  The mainline host does
		 * not send this command today.
		 */
		const typeof(cmd->data.slcflush) *f = &cmd->data.slcflush;

		TRACE(SFID_DBG_5, f->context_fw_addr, f->inval, f->dm_context, (uint32_t)f->size);
		slc_flush_inval();
		break;
	}
	case KCCB_CORECLKSPEEDCHANGE:
		clock_change();
		break;
	case KCCB_STATEFLAGS_CTRL: {
		uint32_t f = cmd->data.stateflags.config_flags;
		uint32_t op = cmd->data.stateflags.op;

		/* op: 0 = set exact, 1 = set bits, 2 = clear bits (best effort) */
		if (op == 1)
			g.config_flags |= f;
		else if (op == 2)
			g.config_flags &= ~f;
		else
			g.config_flags = f;
		if (g.sysdata)
			g.sysdata->config_flags = g.config_flags;
		break;
	}
	case KCCB_LOGTYPE_UPDATE:     /* log_type is read live from the trace control */
	case KCCB_HEALTH_CHECK:
	case KCCB_BP:
		break;
	case KCCB_FREELIST_GROW_UPDATE:
		spm_grow_update(cmd->data.grow.freelist_fw_addr, cmd->data.grow.delta_pages,
				cmd->data.grow.new_pages, cmd->data.grow.ready_pages);
		sched_run();
		break;
	case KCCB_FREELISTS_RECONSTRUCTION_UPDATE:
		/* the host rebuilt the free lists and reset their HWRTData (pvr_free_list.c) */
		pm_reconstruct_update(cmd->data.reconstruct.freelist_ids,
				      cmd->data.reconstruct.freelist_count);
		sched_run();            /* the held geometry and 3D may run now */
		break;
	case KCCB_ZSBUFFER_BACKING_UPDATE:
	case KCCB_ZSBUFFER_UNBACKING_UPDATE:
		spm_zsbuffer_update(cmd->data.zsbuffer.zs_buffer_fw_addr,
				    cmd->data.zsbuffer.done & 0xFF,
				    (cmd->cmd_type & 0xFFFF) == KCCB_ZSBUFFER_BACKING_UPDATE);
		sched_run();            /* a partial render may be armed now */
		break;
	case KCCB_NOTIFY_WRITE_OFFSET_UPDATE:
	case KCCB_OS_ONLINE_STATE_CONFIGURE:
	case KCCB_REGCONFIG:
	case KCCB_HWPERF_UPDATE_CONFIG:
	case KCCB_HWPERF_CTRL_BLKS:
	case KCCB_PDVFS_LIMIT_MAX_FREQ:
	case KCCB_PDVFS_LIMIT_MIN_FREQ:
	case KCCB_OSID_PRIORITY_CHANGE:
	case KCCB_PHR_CFG:
	case KCCB_WDG_CFG:
	case KCCB_COUNTER_DUMP:
		break;
	default:
		TRACE(SFID_MAIN_150, cmd->cmd_type);
		break;
	}
	return rtn;
}

/**
 * Execute every pending Kernel CCB command.
 *
 * Runs with interrupts masked. For each command it writes the return slot,
 * counts it in ``osdata``, and advances the read offset after a write
 * barrier. Stops early once a command powers the firmware off. Interrupts
 * the host once if any command was executed.
 */
void kccb_process(void)
{
	struct ccb_ctl *ctl = g.kccb_ctl;
	bool any = false;

	if (!ctl || !g.kccb)
		return;

	uint32_t s = irq_save();

	while (ctl->read_offset != ctl->write_offset) {
		uint32_t idx = ctl->read_offset & ctl->wrap_mask;
		const struct kccb_cmd *cmd =
			(const struct kccb_cmd *)(g.kccb + idx * sizeof(struct kccb_cmd));
		uint32_t rtn;

		TRACE(SFID_MTS_19, idx, cmd->cmd_type, 0);
		rtn = kccb_handle(cmd);
		TRACE(SFID_MTS_17, idx, rtn);

		if (g.kccb_rtn)
			g.kccb_rtn[idx] = rtn;
		if (g.osdata)
			g.osdata->kccb_cmds_executed++;
		write_barrier();
		ctl->read_offset = (idx + 1) & ctl->wrap_mask;
		any = true;
		if (g.powered_off)
			break;
	}
	if (any)
		notify_host();
	irq_restore(s);
}

/**
 * Send a command to the host through the Firmware CCB.
 *
 * When the ring is full it interrupts the host and waits (10 µs at a time)
 * until the host has read an entry.
 *
 * Args:
 *     cmd: The command to copy into the ring.
 *
 * Returns:
 *     ``true`` when sent, ``false`` when the FWCCB is not known yet.
 */
bool fwccb_send(const struct fwccb_cmd *cmd)
{
	struct ccb_ctl *ctl = g.fwccb_ctl;
	uint32_t woff, next;
	bool warned = false;

	if (!ctl || !g.fwccb)
		return false;

	woff = ctl->write_offset & ctl->wrap_mask;
	next = (woff + 1) & ctl->wrap_mask;
	while (next == ctl->read_offset) {
		if (!warned) {
			TRACE(SFID_MAIN_90, 0, ctl->read_offset, woff);
			warned = true;
		}
		notify_host();
		udelay(10);
	}
	memcpy(g.fwccb + woff * sizeof(struct fwccb_cmd), cmd, sizeof(*cmd));
	write_barrier();
	ctl->write_offset = next;
	notify_host();
	return true;
}
