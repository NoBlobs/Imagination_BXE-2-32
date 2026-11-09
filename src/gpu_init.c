// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/*
 * GPU bring-up at every power-on of the GPU domain (cold and warm boot),
 * before the first kick: docsClean/boot-gpu-init.md.
 *
 *   gpu_power_on()   section C: XPU broadcast, SYS_BUS_SECURE, BIF fault page,
 *                    MTS task-context association; section B: event routing
 *   gpu_units_init() section A: dusts, SLC bypass, reset pulse, PDS/USC exec
 *                    bases, unit defaults, init-done handshakes
 *   dust_power()     section D: power-island request
 */
#include "fw.h"
#include "mips.h"

/* Registers without a public name (boot-gpu-init.md). */
#define CR_POWER_REQ            0x0038
#define CR_ACTIVE_DUSTS         0x0050
#define CR_UNIT_RESET_PULSE     0x6000
#define CR_PDS_EXEC_BASE        0x0610
#define CR_USC_EXEC_BASE0       0x4008
#define CR_USC_EXEC_BASE1       0x4010
#define CR_USC_EXEC_BASE2       0x4028
#define CR_USC_OVERALLOC_TEMPS  0x4050
#define CR_USC_OVERALLOC_SHARED 0x06D8
#define CR_INIT_DONE_A          0x4068
#define CR_INIT_DONE_B          0x0660
#define CR_INIT_DONE_C          0x0670
#define CR_INIT_DONE_D          0x0CD8
#define CR_TPU_FILTER_MODE      0x17B8

/* Public (pvr_rogue_cr_defs.h). */
#define CR_SLC_CTRL_BYPASS      0x3828
#define CR_BIF_FAULT_READ       0x13E0
#define CR_SYS_BUS_SECURE       0xA100

#define INICFG_POW_RASCALDUST   (1u << 4)

/* MTS_DMn_INTERRUPT_ENABLE values (boot-gpu-init.md, section B) */
#define MTS_INT_DM0  (EV_MMU_PAGE_FAULT | EV_GPIO_REQ | EV_USC_TRIGGER | (1u << 19))
#define MTS_INT_DM2  (EV_TA_FINISHED | EV_PM_OUT_OF_MEMORY)
#define MTS_INT_DM3  (EV_PIXELBE_END_RENDER)
#define MTS_INT_DM4  (EV_COMPUTE_FINISHED)

static bool units_ready;

void gpu_power_on(void)
{
	struct sysinit *si = g.sysinit;

	/* XPU broadcast to every core (one core on the K1) and secure-bus open. */
	cr_write32(CR_XPU_BROADCAST, 1);
	cr_write32(CR_SYS_BUS_SECURE, 0);
	(void)cr_read32(CR_SYS_BUS_SECURE);

	/* Faulting units read the host's dummy page instead of stalling the bus. */
	cr_write64(CR_BIF_FAULT_READ, si->fault_phys_addr & 0xFFFFFFFFF0ull);

	/* GPU events reach the IRQ task through the per-DM MTS enables. */
	cr_write32(CR_EVENT_CLEAR, 0x41FC);
	cr_write32(CR_EVENT_ENABLE, 0);         /* host IRQ path: must stay 0 (E011) */
	cr_write32(CR_MTS_DM0_INTERRUPT_ENABLE + 0x00, MTS_INT_DM0);
	cr_write32(CR_MTS_DM0_INTERRUPT_ENABLE + 0x10, MTS_INT_DM2);
	cr_write32(CR_MTS_DM0_INTERRUPT_ENABLE + 0x18, MTS_INT_DM3);
	cr_write32(CR_MTS_DM0_INTERRUPT_ENABLE + 0x20, MTS_INT_DM4);

	/* IRQ context serves every DM, BG context serves DM0 (KCCB kicks). */
	cr_write32(CR_MTS_INTCTX_THREAD0_DM_ASSOC, 0xFFFF);
	cr_write32(CR_MTS_BGCTX_THREAD0_DM_ASSOC, 1);
}

static uint32_t num_dusts(void)
{
	uint32_t n = g.runtime_cfg ? g.runtime_cfg->default_dusts_num_init : 0;

	if (!n || n > 8)
		n = 1;
	return n;
}

/* Power-island request: on = true powers the dusts up.  Returns success. */
bool dust_power(bool on)
{
	uint32_t req = on ? 1u : 0u;
	uint32_t n = num_dusts();
	bool ok;

	if (on)
		bcast_write32(CR_ACTIVE_DUSTS, (1u << n) - 1);
	cr_write32(CR_XPU_BROADCAST, 1);
	cr_write32(CR_EVENT_CLEAR, EV_POWER_COMPLETE | EV_POWER_ABORT);
	cr_write32(CR_POWER_REQ, req);
	cr_write32(CR_POWER_REQ, req | 2);
	ok = cr_poll32(CR_EVENT_STATUS, EV_POWER_COMPLETE, EV_POWER_COMPLETE, 10000);
	if (cr_read32(CR_EVENT_STATUS) & EV_POWER_ABORT)
		ok = false;
	cr_write32(CR_EVENT_CLEAR, EV_POWER_COMPLETE | EV_POWER_ABORT);
	cr_write32(CR_XPU_BROADCAST, 1);
	TRACE(SFID_POW_7, req, ok ? (1u << n) - 1 : 0);
	return ok;
}

void gpu_units_init(void)
{
	struct sysinit *si = g.sysinit;
	bool ok = true;

	units_ready = false;

	/*
	 * 1. clock gating: forcing clocks on is only needed with
	 *    INICFG_DISABLE_CLKGATING_EN, which the mainline host never sets;
	 *    the host leaves every unit in AUTO (CLK_CTRL = 0x002AAA002A22AAAA).
	 */

	/* 2. power up the dusts */
	if (!dust_power(true))
		ok = false;

	/* 3. SLC bypass: 16 KiB SLC on this GPU (<= 32 KiB) */
	bcast_write32(CR_SLC_CTRL_BYPASS, cr_read32(CR_SLC_CTRL_BYPASS) | 0x01000000);
	bcast_write32(CR_SLC_CTRL_BYPASS + 4, cr_read32(CR_SLC_CTRL_BYPASS + 4) | 0x2000);

	/* 4. reset pulse */
	bcast_write32(CR_UNIT_RESET_PULSE, 1);
	bcast_write32(CR_UNIT_RESET_PULSE, 0);

	/* 5. PDS / USC execution bases */
#ifdef EXP_NO_PDS_BASE
	bcast_write64(CR_PDS_EXEC_BASE, 0);     /* E025: reproduce the E006 page fault */
#else
	bcast_write64(CR_PDS_EXEC_BASE, si->pds_exec_base);
#endif
	bcast_write64(CR_USC_EXEC_BASE0, si->usc_exec_base);
	bcast_write64(CR_USC_EXEC_BASE1, si->usc_exec_base);
	bcast_write64(CR_USC_EXEC_BASE2, si->usc_exec_base);

	/* 6. unit defaults (FW-only configuration, values from the spec) */
	static const struct { uint16_t off; uint32_t val; } defaults[] = {
		{ 0x18C0, 0x007FC000 }, { 0x18C8, 0x007FC000 },
		{ 0x18D0, 0x007FC000 }, { 0x18E0, 0x007FC000 },
		{ 0x0600, 0x403F3F00 }, { 0x0604, 0x00800000 },
		{ CR_USC_OVERALLOC_TEMPS, 0 | 0x2000 },   /* no breakpoint over-allocation */
		{ CR_USC_OVERALLOC_SHARED, 0 },
		{ 0x0720, 0x0000000F },
		{ 0x0640, 0x9 }, { 0x0698, 0xB }, { 0x0630, 1 }, { 0x0658, 0xB },
		{ 0x06C0, 0xD }, { 0x0648, 1 }, { 0x0608, 1 },
		{ 0x18E8, 0xF }, { 0x18F0, 0xF }, { 0x18F8, 0xF }, { 0x1900, 0xF },
		{ 0x1498, 1 }, { 0x0700, 1 },
		{ 0x0358, 5 },
		{ 0x20F8, 1 }, { 0x2010, 0 }, { 0x2100, 0 },
		{ 0x13F8, 7 },
	};
	for (uint32_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++)
		bcast_write32(defaults[i].off, defaults[i].val);

	/* 7. init-done handshakes */
	bcast_write32(CR_INIT_DONE_A, 1);
	if (!cr_poll32(CR_INIT_DONE_A, 1, 0, 10000))
		ok = false;
	bcast_write32(CR_INIT_DONE_B, 1);
	bcast_write32(CR_INIT_DONE_C, 1);
	bcast_write32(CR_INIT_DONE_D, 1);
	if (!cr_poll32(CR_INIT_DONE_D, 1, 0, 10000))
		ok = false;

	/* 8. TPU filter mode */
	bcast_write32(CR_TPU_FILTER_MODE, si->filter_flags);

	/* 9. per-core index registers: single core, nothing to do */

	/* read-backs for the host-side sampler */
	cr_write32(CR_DBG_UNITS_PDS, cr_read32(CR_PDS_EXEC_BASE + 4));
	cr_write32(CR_DBG_UNITS_USC, cr_read32(CR_USC_EXEC_BASE0 + 4));

	units_ready = ok;
	TRACE(SFID_MAIN_69, ok ? (1u << num_dusts()) - 1 : 0);
}

bool gpu_units_ready(void)
{
	return units_ready;
}
