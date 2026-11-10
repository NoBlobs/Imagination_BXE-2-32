/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Thalles Campagnani */
/* Data-master kicks. */
#pragma once

#include "fw.h"

struct dm_state {
	bool busy;
	fwaddr_t ctx;           /* running context */
	uint32_t cmd_off;       /* offset of the running command in its cCCB */
	uint32_t event_mask;    /* EVENT_STATUS bits that complete the task */
	uint32_t finish_sfid;   /* trace entry emitted on completion */
	uint64_t kick_time;     /* ROGUE_CR_TIMER at kick */
	fwaddr_t hwrt;          /* render target in use (geometry/fragment) */
	uint32_t bif_slot;
};

extern struct dm_state dms[DM_MAX];

void kick_init(void);
/* Start a task; true if the DM is now running it, false if it completed at once. */
bool kick_task(uint32_t dm, fwaddr_t ctxaddr, struct fwcommoncontext *ctx,
	       const struct ccb_cmd_header *hdr, const void *payload, uint32_t cmd_off);
void kick_events(uint32_t events);
bool kick_resource_busy(uint32_t type, fwaddr_t addr);
void sched_dm_done(uint32_t dm);
/* Poll EVENT_STATUS for completions (backstop when no IRQ task arrives). */
void sched_poll(void);

void bif_rebuild(void);

/* hwr.c */
enum { HWR_REASON_MMU_FAULT = 1, HWR_REASON_LOCKUP = 2 };
void hwr_recover(uint32_t reason, uint64_t mmu_status, uint64_t req_status);
uint64_t kick_bif_pc(uint32_t slot);
void hwr_watchdog(void);

/* shared kick helpers (kick.c) */
uint32_t bif_activate(fwaddr_t memctx_addr, uint32_t dm);
void bif_set_index(uint32_t dm, uint32_t slot);
void dm_start(uint32_t dm, fwaddr_t ctxaddr, uint32_t cmd_off, uint32_t evmask,
	      uint32_t finish_sfid, uint32_t slot);

/* geometry (geom.c) */
bool kick_geom(fwaddr_t ctxaddr, struct fwcommoncontext *ctx,
	       const struct ccb_cmd_header *hdr, const void *payload, uint32_t cmd_off);
void geom_done(struct dm_state *s);
/* fragment (frag.c) */
bool kick_frag(fwaddr_t ctxaddr, struct fwcommoncontext *ctx,
	       const struct ccb_cmd_header *hdr, const void *payload, uint32_t cmd_off);
void frag_done(struct dm_state *s);
/* free lists (local, global) currently loaded in PM context 1 (frag.c) */
extern fwaddr_t pm_ctx1_fl[2];
