// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/**
 * Firmware initialisation and interrupt dispatch.
 *
 * After boot the firmware has no main loop of its own: it sleeps in
 * ``WAIT`` and every piece of work runs from one of three interrupt sources
 * (timer, MTS background task, MTS interrupt task). As a safety net the timer
 * tick also polls the Kernel CCB, so a missed MTS wake-up only costs latency.
 */
/**/
#include "fw.h"
#include "mips.h"
#include "kick.h"

/** The firmware's global state (see :c:struct:`fw_state`). */
struct fw_state g;

/** Timer period in milliseconds, used as a polling backstop. */
#define TICK_MS 1

/** Program CP0 ``Compare`` for the next timer tick, :c:macro:`TICK_MS` from now. */
static void timer_arm(void)
{
	uint32_t period = g.count_per_ms * TICK_MS;

	mtc0(C0_COMPARE, 0, mfc0(C0_COUNT, 0) + period);
}

/**
 * Derive the clock-dependent rates from the core clock.
 *
 * Sets ``core_clock_hz``, the GPU timer ticks per millisecond (clock / 256)
 * and the CP0 ``Count`` ticks per millisecond (clock / 2). Neither rate is
 * ever left at zero.
 *
 * Args:
 *     hz: Core clock in Hz; 0 selects 307.2 MHz.
 */
static void clocks_update(uint32_t hz)
{
	if (!hz)
		hz = 307200000; /* never run with a zero clock */
	g.core_clock_hz = hz;
	g.timer_ticks_per_ms = hz / 256 / 1000;
	g.count_per_ms = hz / 2 / 1000;
	if (!g.timer_ticks_per_ms)
		g.timer_ticks_per_ms = 1;
	if (!g.count_per_ms)
		g.count_per_ms = 1;
}

/**
 * Find the host-shared structures.
 *
 * Reads ``sysinit`` and ``osinit`` at their fixed addresses and takes from
 * them the addresses of ``sysdata``, ``osdata``, the runtime configuration,
 * the trace control and both CCBs. Also stores the remap word for the fault
 * page (used by the TLB refill handler), sets the clocks from the runtime
 * configuration (or the initial clock) and reports the hardware as healthy.
 */
static void shared_init(void)
{
	struct sysinit *si = (struct sysinit *)FW_SYSINIT_VA;
	struct osinit *oi = (struct osinit *)FW_OSINIT_VA;
	uint64_t fault = si->fault_phys_addr;

	g.sysinit = si;
	g.osinit = oi;
	g.sysdata = fwptr(si->fw_sys_data_fw_addr);
	g.runtime_cfg = fwptr(si->runtime_cfg_fw_addr);
	g.tracebuf = fwptr(si->trace_buf_ctl_fw_addr);
	g.osdata = fwptr(oi->fw_os_data_fw_addr);
	g.kccb_ctl = fwptr(oi->kernel_ccbctl_fw_addr);
	g.kccb = fwptr(oi->kernel_ccb_fw_addr);
	g.kccb_rtn = fwptr(oi->kernel_ccb_rtn_slots_fw_addr);
	g.fwccb_ctl = fwptr(oi->firmware_ccbctl_fw_addr);
	g.fwccb = fwptr(oi->firmware_ccb_fw_addr);

	/* remap word for the fault page, used by the TLB refill handler */
	*boot_data_word(BD_FAULT_HI) = (uint32_t)((fault >> 12) << 4);

	clocks_update(g.runtime_cfg && g.runtime_cfg->core_clock_speed ?
		      g.runtime_cfg->core_clock_speed : si->initial_core_clock_speed);

	if (g.sysdata) {
		g.config_flags = g.sysdata->config_flags;
		g.sysdata->hwr_state_flags = HWR_HARDWARE_OK | HWR_DM_RUNNING_OK;
	}
}

/**
 * Firmware entry point, called by ``boot.S`` once the stack and caches are up.
 *
 * Initialises the shared structures, the trace and statistics, powers the
 * GPU on, initialises the units, the scheduler and the kick state, enables
 * the host interrupt, reports ``firmware_started`` to the host and unmasks
 * the three interrupt lines (HW0 timer, HW1 MTS background task, HW2 MTS
 * interrupt task). It then handles any KCCB command queued before it was
 * ready and sleeps in ``WAIT`` forever; all later work runs from
 * :c:func:`irq_dispatch`.
 *
 * Boot-stage words 4, 5 and 6 (and the ``0xC0DE000n`` debug register) mark
 * its progress.
 */
void fw_main(void)
{
	*boot_data_word(BD_BOOT_STAGE) = 4;
	cr_write32(CR_DBG_STAGE, 0xC0DE0004);

	memset(&g, 0, sizeof(g));
	shared_init();
	trace_init();
	stats_init();
	*boot_data_word(BD_BOOT_STAGE) = 5;
	cr_write32(CR_DBG_STAGE, 0xC0DE0005);

	TRACE(SFID_DBG_4, cr_read32(CR_MTS_SCHEDULE_ENABLE),
	      cr_read32(CR_MTS_BGCTX_THREAD0_DM_ASSOC),
	      cr_read32(CR_MTS_INTCTX_THREAD0_DM_ASSOC));
	gpu_power_on();
	gpu_units_init();
	sched_init();
	kick_init();

	/* host interrupt line from the MIPS wrapper */
	cr_write32(CR_MIPS_WRAPPER_IRQ_ENABLE, 1);

	pow_set_state(POW_STATE_IDLE);
	TRACE(SFID_POW_23, g.runtime_cfg ? g.runtime_cfg->active_pm_latency_ms : 0,
	      g.core_clock_hz);
	TRACE(SFID_MAIN_176, g.config_flags, g.sysdata ? g.sysdata->config_flags_ext : 0);

	g.sysinit->firmware_started_timestamp = (uint32_t)gpu_timer();
	write_barrier();
	g.sysinit->firmware_started = 1;
	write_barrier();
	*boot_data_word(BD_BOOT_STAGE) = 6;
	cr_write32(CR_DBG_STAGE, 0xC0DE0006);

	/* Interrupts: HW0 (timer), HW1 (BG task), HW2 (IRQ task) */
	timer_arm();
	uint32_t st = mfc0(C0_STATUS, 0);
	st &= ~0xFF00u;
	st |= 0x1C00u;
	st &= ~0x6u;            /* EXL, ERL */
	mtc0(C0_STATUS, 0, st);

	/* Anything queued before we were ready. */
	kccb_process();

	__asm__ volatile("ei\n\tehb" ::: "memory");
	for (;;)
		mips_wait();
}

/**
 * Tell the MTS that the current task has finished.
 *
 * Args:
 *     v: The task type: 0 for the background task, 2 for the interrupt task.
 */
static void mts_task_end(uint32_t v)
{
	cr_write32(CR_MTS_TASK_END, v);
	(void)cr_read32(CR_MTS_TASK_END);
}

/**
 * Handle one interrupt, called by the exception vector in ``vectors.S``.
 *
 * - **Timer**: re-arms the tick, then (unless powered off) processes the
 *   KCCB, polls the scheduler events, runs the HWR watchdog and the
 *   scheduler.
 * - **MTS background task**: processes the KCCB, polls the events and runs
 *   the scheduler, then ends the task.
 * - **MTS interrupt task**: reads and clears the GPU event status, passes the
 *   events to :c:func:`sched_irq` and ends the task.
 *
 * Each source also updates its debug counters.
 *
 * Args:
 *     source: 0 timer, 1 MTS background task, 2 MTS interrupt task; anything
 *         else counts as spurious.
 */
void irq_dispatch(uint32_t source)
{
	switch (source) {
	case 0: /* timer */
		g.timer_irqs++;
		cr_write32(CR_DBG_KICKS_CDM, g.cdm_kicks);
		cr_write32(CR_DBG_IRQ_TIMER, g.timer_irqs);
		timer_arm();
		if (!g.powered_off) {
			kccb_process();
#ifndef EXP_NO_EVENT_POLL
			sched_poll();
#endif
			hwr_watchdog();
			sched_run();
		}
		break;
	case 1: /* MTS background task */
		g.bg_irqs++;
		cr_write32(CR_DBG_IRQ_BG, g.bg_irqs);
		{
			/* E018: which task brings the DM completion events? */
			uint32_t pend = cr_read32(CR_EVENT_STATUS);

			cr_write32(CR_DBG_BG_CTX, cr_read32(CR_MTS_BGCTX));
			if (pend & (EV_COMPUTE_FINISHED | EV_PIXELBE_END_RENDER | EV_TA_FINISHED)) {
				cr_write32(CR_DBG_BG_WITH_EV, cr_read32(CR_DBG_BG_WITH_EV) + 1);
				cr_write32(CR_DBG_BG_LASTEV, pend);
			}
		}
		if (!g.powered_off) {
			kccb_process();
			sched_poll();
			sched_run();
		}
		mts_task_end(0);
		break;
	case 2: { /* MTS interrupt task: GPU events */
		uint32_t ev = cr_read32(CR_EVENT_STATUS);

		g.irq_irqs++;
		cr_write32(CR_DBG_IRQ_IRQ, g.irq_irqs);
		cr_write32(CR_DBG_IRQ_LASTEV, ev);
		cr_write32(CR_DBG_INT_CTX, cr_read32(CR_MTS_INTCTX));
		if (ev & EV_COMPUTE_FINISHED)
			cr_write32(CR_DBG_IRQ_COMPUTE, cr_read32(CR_DBG_IRQ_COMPUTE) + 1);
		if (ev)
			cr_write32(CR_EVENT_CLEAR, ev);
		if (!g.powered_off)
			sched_irq(ev);
		mts_task_end(2);
		break;
	}
	default:
		g.spurious_irqs++;
		break;
	}
}

/**
 * Report an unexpected exception, called by ``vectors.S``.
 *
 * The vector has saved ``Cause``, ``EPC`` and ``BadVAddr`` in the boot data
 * page. Sets ``HWR_FW_FAULT`` and counts the fault in ``sysdata``, writes a
 * trace entry (``0xDEAD00xx`` with the exception code) and interrupts the
 * host.
 */
void fatal_exception(void)
{
	uint32_t cause = *boot_data_word(BD_LAST_CAUSE);
	uint32_t epc = *boot_data_word(BD_LAST_EPC);
	uint32_t bad = *boot_data_word(BD_LAST_BADVADDR);

	if (g.sysdata) {
		g.sysdata->hwr_state_flags |= HWR_FW_FAULT;
		g.sysdata->fw_faults++;
	}
	TRACE(SFID_DBG_4, 0xDEAD0000u | ((cause >> 2) & 0x1F), epc, bad);
	notify_host();
}
