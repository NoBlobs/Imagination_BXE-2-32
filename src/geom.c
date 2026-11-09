// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Thalles Campagnani
/*
 * Geometry (TA) kick — EXPERIMENTAL (E028).
 *
 * Sources: docsClean/kick-geometry-ta.md (phases A-G and the register values
 * observed by the spec maintainer), the public field layouts of Mesa's
 * rogue_cr.xml, our writable-bit probe (E008) and the value matching of E027.
 * The PM free-list programming is only partly known: what is not known is
 * left out on purpose, so that the symptoms (MMU fault address, stack
 * pointers) show what is missing.
 */
#include "kick.h"

/* public structure offsets (pvr_rogue_fwif_check.h) */
#define HWRT_PM_MLIST_DEV_ADDR   0
#define HWRT_VCE_CAT_BASE        8
#define HWRT_TE_CAT_BASE         72
#define HWRT_ALIST_CAT_BASE      136
#define HWRT_ALIST_SP            152
#define HWRT_MLIST_SP            160
#define HWRT_COMMON_FW_ADDR      164
#define HWRT_STATE               172
#define HWRT_FREELISTS           176
#define HWRT_VHEAP_TABLE         200
#define HWRT_TAIL_PTRS           240
#define HWRT_MACROTILE_ARRAY     248
#define HWRT_RGN_HEADER          256
#define HWRT_RTC                 264

#define COMMON_SCREEN_PIXEL_MAX  4
#define COMMON_MSCTL             8
#define COMMON_MSCTL_FLIPPED     16
#define COMMON_TE_SCREEN         32
#define COMMON_MTILE_STRIDE      36
#define COMMON_TE_MTILE1         44
#define COMMON_TE_MTILE2         48

#define FL_DEV_ADDR              0
#define FL_CURRENT_DEV_ADDR      8
#define FL_STACK_TOP             16

#define GEOM_REGS                16   /* rogue_fwif_cmd_geom.regs */
#define GEOM_FLAGS               80
#define GEOM_FLAGS_FIRSTKICK     (1u << 0)
#define GEOM_FLAGS_LASTKICK      (1u << 1)

#define RTDATA_STATE_KICK_GEOM        1
#define RTDATA_STATE_KICK_GEOM_FIRST  2
#define RTDATA_STATE_GEOM_FINISHED    3

/* registers (geometry core bank) */
#define CR_PM_FL_A               0x0210  /* local free list address (one per PM context) */
#define CR_PM_FL_STACKTOP        0x0224
#define CR_PM_VHEAP_TABLE        0x0248
#define CR_PM_HANDSHAKE_A        0x0250
#define CR_PM_HANDSHAKE_B        0x0258
#define CR_PM_ALIST_SP           0x0270
#ifndef EXP_MLIST_REG
#define EXP_MLIST_REG            0x02D8  /* PM_MLIST0_BASE candidate (E008 mask + Mesa layout), E028 */
#endif
#define CR_PM_MLIST_SP           0x02C0
#define CR_VDM_KICK              0x0400
#define CR_VDM_CTRL_STREAM_BASE  0x0408
#define CR_VDM_CALL_STACK_PTR    0x0418
#define CR_TE_MTILE1             0x0C08
#define CR_TE_MTILE2             0x0C10
#define CR_TE_SCREEN             0x0C18
#define CR_TE_MTILE_STRIDE       0x0C20
#define CR_TE_PSG                0x0C28
#define CR_TE_PSGREGION_ADDR     0x0C38
#define CR_TE_TPC_ADDR           0x0C40
#define CR_GEOM_CTX_ENABLE       0x0C68
#define CR_PPP_MSCTL             0x0C78
#define CR_PPP_MSCTL_FLIPPED     0x0C80
#define CR_PPP_CTRL              0x0C88
#define CR_PPP_SCREEN            0x0C98
#define CR_GEOM_CE0              0x0CE0
#define CR_TPU_BORDER_VDM        0x17A0
#define CR_BIF_PM_CAT_BASE_VCE0  0x1248
#define CR_BIF_PM_CAT_BASE_TE0   0x1250
#define CR_BIF_PM_CAT_BASE_ALIST0 0x1260
#define CR_MULTICORE_GEOM_CTRL   0xF320

static inline uint64_t rd64(const uint8_t *p, uint32_t off)
{
	return *(const uint64_t *)(p + off);
}

static inline uint32_t rd32(const uint8_t *p, uint32_t off)
{
	return *(const uint32_t *)(p + off);
}

static void geom_snapshot(uint32_t tag)
{
	TRACE(SFID_DBG_2, 0xE0280000u | tag);
	TRACE(SFID_DBG_9, cr_read32(CR_PM_FL_STACKTOP), cr_read32(CR_PM_ALIST_SP),
	      cr_read32(CR_PM_MLIST_SP), cr_read32(0x0410), cr_read32(0x20C8),
	      cr_read32(0x0348), cr_read32(CR_EVENT_STATUS), cr_read32(0x0D00));
}

bool kick_geom(fwaddr_t ctxaddr, struct fwcommoncontext *ctx,
	       const struct ccb_cmd_header *hdr, const void *payload, uint32_t cmd_off)
{
	const uint8_t *cmd = payload;
	const uint8_t *regs = cmd + GEOM_REGS;
	uint32_t flags = rd32(cmd, GEOM_FLAGS);
	uint8_t *hwrt = fwptr(rd32(cmd, 4));             /* cmd_shared.hwrt_data_fw_addr */
	const uint8_t *common = hwrt ? fwptr(rd32(hwrt, HWRT_COMMON_FW_ADDR)) : NULL;
	const uint8_t *fl_local = hwrt ? fwptr(rd32(hwrt, HWRT_FREELISTS)) : NULL;
	const uint8_t *gstate = fwptr(ctx->context_state_addr);
	uint32_t slot;

	if (!hwrt || !common || !fl_local)
		return false;   /* cannot run without a render target: retire */

	TRACE(SFID_DBG_3, 0xE0280A00u, cr_read32(CR_EVENT_STATUS));     /* at entry */
	/* A. preconditions */
	bcast_write32(CR_MULTICORE_GEOM_CTRL, 1);
	slot = bif_activate(ctx->fw_mem_context_fw_addr, DM_GEOM);
	bif_set_index(DM_GEOM, slot);

	/*
	 * B. load the HWRTData into PM context 0 (answers to Q16b-Q16g).
	 * E032: the PM state is written between the two handshakes (pause, then
	 * resume), as phase B describes them ("pause and resume while the state
	 * changes"); written before them, the live stack pointers stayed 0.
	 */
#ifdef EXP_PM_BCAST   /* E032: PM loads through the broadcast bank */
#define pm_w64 bcast_write64
#define pm_w32 bcast_write32
#else
#define pm_w64 cr_write64
#define pm_w32 cr_write32
#endif
#ifndef EXP_HS_MODE
#define EXP_HS_MODE 1   /* 0 none, 1 after the PM writes (spec order D), 2 around them (E032: fails) */
#endif
#if EXP_HS_MODE == 2
	TRACE(SFID_DBG_3, 0xE0280B00u, cr_read32(CR_EVENT_STATUS));
	cr_write32(CR_PM_HANDSHAKE_A, 1);
	cr_poll32(CR_PM_HANDSHAKE_A, 1, 1, 10000);
	TRACE(SFID_DBG_3, 0xE0280B01u, cr_read32(CR_PM_HANDSHAKE_A));
#endif
#ifdef EXP_CLK_ON
	/* E032: every AUTO clock forced ON (0x2 -> 0x1 per field) */
	cr_write64(0x0000, 0x0015550015115555ull);
	TRACE(SFID_DBG_4, 0xE0280B30u, cr_read32(0x0008), cr_read32(0x000C));   /* CLK_STATUS */
#endif
	/* 0x0240 = base of the general heap: the first change at a first load (Q16g) */
	cr_write64(0x0240, 0x8000000000ull);
	cr_write64(CR_PM_VHEAP_TABLE, rd64(hwrt, HWRT_VHEAP_TABLE));
	pm_w64(CR_PM_FL_A, rd64(fl_local, FL_CURRENT_DEV_ADDR));
	/* 0x0220 reads 0 with the original firmware (Q16b answer): leave it 0 */
	#ifdef EXP_SP32
	pm_w32(CR_PM_FL_STACKTOP, rd32(fl_local, FL_STACK_TOP));
#else
	pm_w64(CR_PM_FL_STACKTOP - 4, (uint64_t)rd32(fl_local, FL_STACK_TOP) << 32);
#endif
	{
		/*
		 * Global free list of PM context 0 (answers to Q16e-Q16g),
		 * see the bank layout below.
		 * see the bank layout below.
		 */
		const uint8_t *fl_global = fwptr(rd32(hwrt, HWRT_FREELISTS + 4));

		if (fl_global) {
#ifdef EXP_PM_PAUSE
			/* E032: the grow update loads the bank inside PM_PAGE_MANAGEOP = 1 */
			cr_write32(0x02A0, 1);
			for (uint32_t i = 0; i < 1000 && !(cr_read32(0x02A8) & 1); i++)
				;
			TRACE(SFID_DBG_4, 0xE0280B20u, cr_read32(0x02A0), cr_read32(0x02A8));
#endif
			/*
			 * PM context 0 (geometry) uses the bank at 0x20B8 (answer to Q16g;
			 * the Q16e table had the banks swapped): +0x00 base, +0x08 stack
			 * top (writing it loads the read-only live SP at +0x10),
			 * +0x18/+0x1C page counts at load.
			 */
			pm_w64(0x20B8, rd64(fl_global, FL_CURRENT_DEV_ADDR));
			#ifdef EXP_SP32
			pm_w32(0x20C0, rd32(fl_global, FL_STACK_TOP));   /* low word only (E032) */
#else
			pm_w64(0x20C0, rd32(fl_global, FL_STACK_TOP));
#endif   /* 64-bit: high word commits? (E032) */
			pm_w32(0x20D0, rd32(fl_global, 32));
			pm_w32(0x20D4, rd32(fl_global, 36));
#ifdef EXP_PM_PAUSE
			TRACE(SFID_DBG_4, 0xE0280B21u, cr_read32(0x20C8), cr_read32(0x02A8));
			cr_write32(0x02A0, 0);
			for (uint32_t i = 0; i < 1000 && (cr_read32(0x02A8) & 1); i++)
				;
			TRACE(SFID_DBG_4, 0xE0280B22u, cr_read32(0x20C8), cr_read32(0x02A8));
#endif
		}
		/* CONTEXT_PB_BASE (0x02B0) = 3 at the geometry load (answer to Q16e) */
		cr_write32(0x02B0, 3);
	}
	/* allocated page / MMU page counts of the local free list (Q16b answer) */
	cr_write32(0x0398, rd32(fl_local, 32));
	cr_write32(0x039C, rd32(fl_local, 36));
	cr_write64(CR_BIF_PM_CAT_BASE_VCE0, rd64(hwrt, HWRT_VCE_CAT_BASE));
	cr_write64(CR_BIF_PM_CAT_BASE_TE0, rd64(hwrt, HWRT_TE_CAT_BASE));
	cr_write64(CR_BIF_PM_CAT_BASE_ALIST0, rd64(hwrt, HWRT_ALIST_CAT_BASE));
	if (EXP_MLIST_REG)
		cr_write64(EXP_MLIST_REG, rd64(hwrt, HWRT_PM_MLIST_DEV_ADDR));
	/* 0x0270/0x02C0 are read-only (E008); the writable neighbours load the SPs */
	cr_write64(CR_PM_ALIST_SP + 8, rd64(hwrt, HWRT_ALIST_SP));
	cr_write32(CR_PM_MLIST_SP + 8, rd32(hwrt, HWRT_MLIST_SP));

#ifdef EXP_CANARY
	{
		/*
		 * E028 canaries: distinct unmapped GPU VAs (4 GiB + k * 16 MiB) in the
		 * unassigned 40-bit candidates; the fault address then names the register.
		 */
		static const uint16_t cand[] = { 0x0218, 0x2078, 0x02E0, 0x0230 };

		for (uint32_t k = 0; k < sizeof(cand) / sizeof(cand[0]); k++)
			cr_write64(cand[k], 0x0100000000ull + ((uint64_t)(k + 1) << 24));
	}
#endif
	TRACE(SFID_DBG_4, 0xE0280B10u, cr_read32(0x20C8), cr_read32(0x0348));   /* live SPs before resume */
#if EXP_HS_MODE == 2
	cr_write32(CR_PM_HANDSHAKE_B, 1);
	cr_poll32(CR_PM_HANDSHAKE_B, 1, 0, 10000);
	TRACE(SFID_DBG_5, 0xE0280B02u, cr_read32(0x20C8), cr_read32(0x0348), cr_read32(CR_PM_HANDSHAKE_B));
#elif EXP_HS_MODE == 1
	cr_write32(CR_PM_HANDSHAKE_A, 1);
	cr_poll32(CR_PM_HANDSHAKE_A, 1, 1, 10000);
	cr_write32(CR_PM_HANDSHAKE_B, 1);
	cr_poll32(CR_PM_HANDSHAKE_B, 1, 0, 10000);
	TRACE(SFID_DBG_5, 0xE0280B02u, cr_read32(0x20C8), cr_read32(0x0348), cr_read32(CR_PM_HANDSHAKE_B));
#endif

	/* C. geometry registers (observed by the spec, matched by value in E027) */
	cr_write32(CR_PPP_CTRL, rd32(regs, 28));
	cr_write32(CR_TE_PSG, rd32(regs, 32));
	cr_write64(CR_TE_PSGREGION_ADDR, rd64(hwrt, HWRT_RGN_HEADER));
	cr_write64(CR_TE_TPC_ADDR, rd64(hwrt, HWRT_TAIL_PTRS));
	cr_write64(CR_TPU_BORDER_VDM, rd64(regs, 8));
	cr_write32(CR_PPP_SCREEN, rd32(common, COMMON_SCREEN_PIXEL_MAX));
	cr_write32(CR_TE_SCREEN, rd32(common, COMMON_TE_SCREEN));
	cr_write32(CR_TE_MTILE1, rd32(common, COMMON_TE_MTILE1));
	cr_write32(CR_TE_MTILE2, rd32(common, COMMON_TE_MTILE2));
	cr_write32(CR_TE_MTILE_STRIDE, rd32(common, COMMON_MTILE_STRIDE));
	cr_write64(CR_PPP_MSCTL, rd64(common, COMMON_MSCTL));
	cr_write64(CR_PPP_MSCTL_FLIPPED, rd64(common, COMMON_MSCTL_FLIPPED));
	if (gstate)
		cr_write64(CR_VDM_CALL_STACK_PTR, rd64(gstate, 8)); /* call_stack_pointer_init */
	cr_write64(CR_VDM_CTRL_STREAM_BASE, rd64(regs, 0));

	/*
	 * D. context state bases from the render context's static state
	 * (geom_registers_caswitch of core 0): candidates found by E008 mask +
	 * Mesa layouts (VDM_CONTEXT_STATE_BASE, TA_CONTEXT_STATE_BASE), E028.
	 */
	{
		const uint8_t *st = (const uint8_t *)fwptr(ctxaddr) + 304;

#ifndef EXP_VDM_STATE_REG
#define EXP_VDM_STATE_REG 0x0428
#endif
#ifndef EXP_TA_STATE_REG
#define EXP_TA_STATE_REG 0x0CB0         /* found by the E028 canary (base + 0x200 read) */
#endif
		if (EXP_VDM_STATE_REG)
			cr_write64(EXP_VDM_STATE_REG, rd64(st, 0));
		if (EXP_TA_STATE_REG)
			cr_write64(EXP_TA_STATE_REG, rd64(st, 16));
	}

	/* new (non-resumed) kick: the observed 0xCE0 = 1 */
	cr_write32(CR_GEOM_CE0, 1);

	TRACE(SFID_DBG_3, 0xE0280C00u, cr_read32(CR_EVENT_STATUS));     /* after phase C/D */
	/* E. enable the context */
	cr_write32(CR_GEOM_CTX_ENABLE, 1);
	if (!cr_poll32(CR_GEOM_CTX_ENABLE, 1, 1, 10000))
		TRACE(SFID_DBG_2, 0xE028E000u);

	/* F. trace, state, watchdog */
	*(uint32_t *)(hwrt + HWRT_STATE) = (flags & GEOM_FLAGS_FIRSTKICK) ?
		RTDATA_STATE_KICK_GEOM_FIRST : RTDATA_STATE_KICK_GEOM;
	TRACE(SFID_MAIN_119, ctxaddr, cmd_off, rd32(cmd, 4), !!(flags & GEOM_FLAGS_FIRSTKICK),
	      !!(flags & GEOM_FLAGS_LASTKICK), 0, ctx->pid, ctx->priority, rd32(cmd, 0),
	      hdr->ext_job_ref, hdr->int_job_ref);
	geom_snapshot(0x1);
	cr_write32(CR_EVENT_CLEAR, EV_TA_FINISHED);
	dm_start(DM_GEOM, ctxaddr, cmd_off, EV_TA_FINISHED, SFID_MAIN_8, slot);
	dms[DM_GEOM].hwrt = rd32(cmd, 4);

	/* G. trigger: last write */
	cr_write32(CR_VDM_KICK, 1);
	return true;
}

/* TA finished: record the PM state reached into the HWRTData. */
void geom_done(struct dm_state *s)
{
	uint8_t *hwrt = fwptr(s->hwrt);

	geom_snapshot(0x2);
	if (!hwrt)
		return;
	*(uint64_t *)(hwrt + HWRT_ALIST_SP) = cr_read32(CR_PM_ALIST_SP);
	*(uint32_t *)(hwrt + HWRT_MLIST_SP) = cr_read32(CR_PM_MLIST_SP);
	*(uint32_t *)(hwrt + HWRT_STATE) = RTDATA_STATE_GEOM_FINISHED;
}
