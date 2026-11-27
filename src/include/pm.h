/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Thalles Campagnani */
/*
 * Parameter Manager (PM): free-list and list loads of the two PM contexts,
 * which HWRTData each context holds, and the out-of-memory handling (SPM).
 * docsClean/kick-geometry-ta.md phase B, partial-render-spm.md; E035, E037.
 */
#pragma once

#include "fw.h"

enum { PM_CTX_GEOM = 0, PM_CTX_FRAG = 1 };
enum { PM_FL_LOCAL = 0, PM_FL_GLOBAL = 1 };

/* public rogue_fwif_freelist layout */
struct pm_freelist {
	uint64_t freelist_dev_addr;
	uint64_t current_dev_addr;
	uint32_t current_stack_top;
	uint32_t max_pages;
	uint32_t grow_pages;
	uint32_t current_pages;
	uint32_t allocated_page_count;
	uint32_t allocated_mmu_page_count;
	uint32_t freelist_id;
	uint32_t grow_pending;
	uint32_t ready_pages;
	uint32_t freelist_flags;
	uint32_t pm_global_pb;
	uint32_t padding;
};
static_assert(sizeof(struct pm_freelist) == 0x40);

/* public rogue_fwif_hwrtdata offsets used by the firmware */
#define HWRT_PM_MLIST_DEV_ADDR   0x000
#define HWRT_VCE_CAT_BASE        0x008
#define HWRT_VCE_LAST_CAT_BASE   0x028
#define HWRT_TE_CAT_BASE         0x048
#define HWRT_TE_LAST_CAT_BASE    0x068
#define HWRT_ALIST_CAT_BASE      0x088
#define HWRT_ALIST_LAST_CAT_BASE 0x090
#define HWRT_ALIST_SP            0x098
#define HWRT_MLIST_SP            0x0A0
#define HWRT_COMMON_FW_ADDR      0x0A4
#define HWRT_FLAGS               0x0A8
#define HWRT_FLAGS_PRIOR_PR      0x8     /* a partial render ran (Q31 answer) */
#define HWRT_STATE               0x0AC
#define HWRT_FREELISTS           0x0B0
#define HWRT_VHEAP_TABLE         0x0C8
#define HWRT_RTA_CTL             0x0D0   /* render_target_index */
#define HWRT_RTA_CURRENT         0x0D4
#define HWRT_RTA_ACTIVE          0x0D8
#define HWRT_RTA_VALID_RTS       0x0E0
#define HWRT_RTA_NUM_PR          0x0E4   /* rta_num_partial_renders_fw_addr */
#define HWRT_RTA_MAX_RTS         0x0E8
#define HWRT_TAIL_PTRS           0x0F0
#define HWRT_MACROTILE_ARRAY     0x0F8
#define HWRT_RGN_HEADER          0x100
#define HWRT_RTC                 0x108
#define HWRT_OWNER_GEOM          0x110
#define HWRT_GEOM_CACHES_ZEROING 0x114

/* rogue_fwif_rtdata_state */
enum {
	RTDATA_STATE_NONE = 0,
	RTDATA_STATE_KICK_GEOM,
	RTDATA_STATE_KICK_GEOM_FIRST,
	RTDATA_STATE_GEOM_FINISHED,
	RTDATA_STATE_KICK_FRAG,
	RTDATA_STATE_FRAG_FINISHED,
	RTDATA_STATE_FRAG_CONTEXT_STORED,
	RTDATA_STATE_GEOM_OUTOFMEM,
	RTDATA_STATE_PARTIALRENDERFINISHED,
	RTDATA_STATE_HWR = 9,
};

/* registers with a public name */
#define CR_PM_PARTIAL_RENDER_ENABLE 0x0338
#define CR_BIF_PM_CAT_BASE_VCE0     0x1248
#define CR_BIF_PM_CAT_BASE_TE0      0x1250
#define CR_BIF_PM_CAT_BASE_ALIST0   0x1260
#define CR_BIF_PM_CAT_BASE_VCE1     0x1268
#define CR_BIF_PM_CAT_BASE_TE1      0x1270
#define CR_BIF_PM_CAT_BASE_ALIST1   0x1280
#define EV_PM_3D_MEM_FREE           (1u << 8)

static inline uint32_t hw_rd32(const void *p, uint32_t off)
{
	return *(const volatile uint32_t *)((const uint8_t *)p + off);
}

static inline uint64_t hw_rd64(const void *p, uint32_t off)
{
	return *(const volatile uint64_t *)((const uint8_t *)p + off);
}

static inline void hw_wr32(const void *p, uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(uintptr_t)((const uint8_t *)p + off) = v;
}

static inline void hw_wr64(const void *p, uint32_t off, uint64_t v)
{
	*(volatile uint64_t *)(uintptr_t)((const uint8_t *)p + off) = v;
}

static inline struct pm_freelist *hwrt_freelist(const void *hwrt, uint32_t which)
{
	return hwrt ? fwptr(hw_rd32(hwrt, HWRT_FREELISTS + 4 * which)) : NULL;
}

/* PM context selection bits written before a load (Q16h, E037). */
void pm_select(uint32_t ctx);
/* Free-list load into a PM context: value registers, then the load command. */
bool pm_load_freelist(uint32_t ctx, uint32_t which, const struct pm_freelist *fl);
/* Copy a context's live free-list state back into the free list. */
void pm_store_freelist(uint32_t ctx, uint32_t which, struct pm_freelist *fl);
/* Catalogues and MLIST base of a context, without the stack-pointer pulses. */
void pm_load_bases(uint32_t ctx, uint64_t mlist, uint64_t vce, uint64_t te, uint64_t alist);
/* PM-MMU / PM-TLB invalidation. */
void pm_mmu_inval(void);
/* Catalogues, MLIST base and ALIST/MLIST stack pointers of a context. */
void pm_load_lists(uint32_t ctx, uint64_t mlist, uint64_t vce, uint64_t te, uint64_t alist,
		   uint64_t alist_sp, uint32_t mlist_sp);

/* Which HWRTData / free lists each PM context holds. */
struct pm_slot {
	fwaddr_t hwrt;
	fwaddr_t fl[2];
	bool valid;     /* the context's registers were loaded from this HWRTData */
	bool dirty;     /* ...and its live free-list state is newer than the structs */
};
extern struct pm_slot pm_slots[2];
void pm_slot_set(uint32_t ctx, fwaddr_t hwrt);
void pm_slot_evict(uint32_t ctx, fwaddr_t next_hwrt);
bool pm_sync_load_freelists(uint32_t c, fwaddr_t hwrt_fw, const void *hwrt);
void pm_slot_stored(uint32_t ctx);
void pm_invalidate(void);

/* Out-of-memory handling (SPM). */
enum spm_state { SPM_NONE, SPM_WAIT_GROW, SPM_WAIT_HW, SPM_PR_RUNNING };
void spm_geom_kick(fwaddr_t hwrt, fwaddr_t cmd_fw, const void *pr_fence);
void spm_oom(void);
bool spm_pr_armed(fwaddr_t hwrt);
bool spm_pr_fence_ok(const struct ufo *u);
void spm_pr_started(void);
void spm_pr_done(void);
void spm_grow_update(fwaddr_t fl_addr, uint32_t delta, uint32_t new_pages, uint32_t ready);
void spm_reset(void);
/* HWR: ask the host to rebuild the free lists of the given HWRTData. */
void pm_hwr_reconstruct(const fwaddr_t *hwrts, uint32_t n);
