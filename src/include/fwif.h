/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Thalles Campagnani */
/*
 * Host <-> firmware shared-memory ABI (subset used by this firmware).
 *
 * Layouts follow the public upstream interface (drm/imagination
 * pvr_rogue_fwif*.h); every offset used here is checked with static_assert
 * against the values listed in the public check headers.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef uint32_t fwaddr_t; /* firmware (MIPS) address, 0xC0xxxxxx window */

#define ROGUE_CMD_MAGIC_DWORD           0x2ABCu
#define ROGUE_CMD_MAGIC(id)             (((uint32_t)ROGUE_CMD_MAGIC_DWORD << 16) | (id))
#define ROGUE_CCB_TYPE_TASK             0x8000u

/* ---------------------------------------------------------------- KCCB --- */
enum {
	KCCB_KICK = 101,
	KCCB_MMUCACHE = 102,
	KCCB_BP = 103,
	KCCB_SLCFLUSHINVAL = 105,
	KCCB_CLEANUP = 106,
	KCCB_POW = 107,
	KCCB_ZSBUFFER_BACKING_UPDATE = 108,
	KCCB_ZSBUFFER_UNBACKING_UPDATE = 109,
	KCCB_FREELIST_GROW_UPDATE = 110,
	KCCB_FREELISTS_RECONSTRUCTION_UPDATE = 112,
	KCCB_NOTIFY_WRITE_OFFSET_UPDATE = 114,
	KCCB_HEALTH_CHECK = 115,
	KCCB_FORCE_UPDATE = 116,
	KCCB_COMBINED_GEOM_FRAG_KICK = 117,
	KCCB_OS_ONLINE_STATE_CONFIGURE = 118,
	KCCB_REGCONFIG = 200,
	KCCB_HWPERF_UPDATE_CONFIG = 201,
	KCCB_HWPERF_CTRL_BLKS = 203,
	KCCB_CORECLKSPEEDCHANGE = 204,
	KCCB_LOGTYPE_UPDATE = 206,
	KCCB_PDVFS_LIMIT_MAX_FREQ = 207,
	KCCB_OSID_PRIORITY_CHANGE = 208,
	KCCB_STATEFLAGS_CTRL = 209,
	KCCB_PDVFS_LIMIT_MIN_FREQ = 212,
	KCCB_PHR_CFG = 213,
	KCCB_WDG_CFG = 215,
	KCCB_COUNTER_DUMP = 216,
};

#define KCCB_RTN_CMD_EXECUTED   (1u << 0)
#define KCCB_RTN_CLEANUP_BUSY   (1u << 1)
#define KCCB_RTN_POLL_FAILURE   (1u << 2)

/* FWCCB command IDs */
enum {
	FWCCB_ZSBUFFER_BACKING = 101,
	FWCCB_ZSBUFFER_UNBACKING = 102,
	FWCCB_FREELIST_GROW = 103,
	FWCCB_FREELISTS_RECONSTRUCTION = 104,
	FWCCB_CONTEXT_RESET_NOTIFICATION = 105,
	FWCCB_DEBUG_DUMP = 106,
	FWCCB_UPDATE_STATS = 107,
	FWCCB_CORE_CLK_RATE_CHANGE = 108,
	FWCCB_REQUEST_GPU_RESTART = 109,
	FWCCB_CONTEXT_FW_PF_NOTIFICATION = 112,
};

/* Client CCB command IDs */
enum {
	CCB_CMD_GEOM = 201,
	CCB_CMD_TQ_3D = 202,
	CCB_CMD_FRAG = 203,
	CCB_CMD_FRAG_PR = 204,
	CCB_CMD_CDM = 205,
	CCB_CMD_TQ_TDM = 206,
	CCB_CMD_FBSC_INVALIDATE = 207,
	CCB_CMD_TQ_2D = 208,
	CCB_CMD_PRE_TIMESTAMP = 209,
	CCB_CMD_NULL = 210,
	CCB_CMD_ABORT = 211,
	CCB_CMD_FENCE = 212,
	CCB_CMD_UPDATE = 213,
	CCB_CMD_RMW_UPDATE = 214,
	CCB_CMD_FENCE_PR = 215,
	CCB_CMD_PRIORITY = 216,
	CCB_CMD_POST_TIMESTAMP = 217,
	CCB_CMD_UNFENCED_UPDATE = 218,
	CCB_CMD_UNFENCED_RMW_UPDATE = 219,
	CCB_CMD_PADDING = 221,
};

/* Power */
enum { POW_STATE_OFF = 0, POW_STATE_ON = 1, POW_STATE_FORCED_IDLE = 2, POW_STATE_IDLE = 3 };
enum { POW_OFF_REQ = 1, POW_FORCED_IDLE_REQ = 2, POW_NUM_UNITS_CHANGE = 3, POW_APM_LATENCY_CHANGE = 4 };
enum { POWER_FORCE_IDLE = 1, POWER_CANCEL_FORCED_IDLE = 2, POWER_HOST_TIMEOUT = 3 };

/* Cleanup types */
enum { CLEANUP_FWCOMMONCONTEXT = 1, CLEANUP_HWRTDATA = 2, CLEANUP_FREELIST = 3, CLEANUP_ZSBUFFER = 4 };

/* HWR state flags */
#define HWR_HARDWARE_OK         (1u << 0)
#define HWR_RESET_IN_PROGRESS   (1u << 1)
#define HWR_GENERAL_LOCKUP      (1u << 3)
#define HWR_DM_RUNNING_OK       (1u << 4)
#define HWR_DM_STALLING         (1u << 5)
#define HWR_FW_FAULT            (1u << 6)
#define HWR_RESTART_REQUESTED   (1u << 7)

/* Sync checkpoint values */
#define SYNC_CHECKPOINT_ACTIVE   0xAC1u
#define SYNC_CHECKPOINT_SIGNALED 0x519u
#define SYNC_CHECKPOINT_ERRORED  0xEFFu
#define UFO_ADDR_IS_SYNC_CHECKPOINT 1u

/* Data masters */
enum { DM_GP = 0, DM_TDM = 1, DM_GEOM = 2, DM_FRAG = 3, DM_CDM = 4, DM_GEOM2 = 6, DM_MAX = 7 };

/* ------------------------------------------------------- structures --- */

struct ccb_ctl {                         /* rogue_fwif_ccb_ctl */
	volatile uint32_t write_offset;
	uint32_t pad0[31];
	volatile uint32_t read_offset;
	uint32_t wrap_mask;
	uint32_t cmd_size;
	uint32_t pad1;
};
static_assert(offsetof(struct ccb_ctl, read_offset) == 128);
static_assert(offsetof(struct ccb_ctl, wrap_mask) == 132);
static_assert(offsetof(struct ccb_ctl, cmd_size) == 136);
static_assert(sizeof(struct ccb_ctl) == 144);

struct cccb_ctl {                        /* rogue_fwif_cccb_ctl */
	volatile uint32_t write_offset;
	volatile uint32_t read_offset;
	volatile uint32_t dep_offset;
	uint32_t wrap_mask;
	uint32_t read_offset2;
	uint32_t read_offset3;
	uint32_t read_offset4;
	uint32_t pad;
};

struct kick_data {                       /* rogue_fwif_kccb_cmd_kick_data */
	fwaddr_t context_fw_addr;
	uint32_t client_woff_update;
	uint32_t client_wrap_mask_update;
	uint32_t num_cleanup_ctl;
	fwaddr_t cleanup_ctl_fw_addr[3];
	uint32_t work_est_cmd_header_offset;
};
static_assert(sizeof(struct kick_data) == 32);

struct kccb_cmd {                        /* rogue_fwif_kccb_cmd, 0x58 bytes */
	uint32_t cmd_type;
	uint32_t kccb_flags;
	union {
		struct kick_data kick;
		struct { struct kick_data geom, frag; } combined;
		struct { uint32_t cache_flags; fwaddr_t sync_fw_addr; uint32_t sync_value; } mmucache;
		struct { uint32_t cleanup_type; fwaddr_t fw_addr; } cleanup;
		struct { uint32_t pow_type; uint32_t data; } pow;
		struct { fwaddr_t context_fw_addr; uint32_t ccb_fence_offset; } force_update;
		struct { fwaddr_t freelist_fw_addr; uint32_t delta_pages; uint32_t new_pages; uint32_t ready_pages; } grow;
		struct { uint32_t new_clock_speed; } coreclk;
		struct { uint32_t config_flags; uint32_t op; } stateflags;
		uint32_t raw[20];
	} data;
};
static_assert(offsetof(struct kccb_cmd, data) == 8);
static_assert(sizeof(struct kccb_cmd) == 88);

struct fwccb_cmd {                       /* rogue_fwif_fwccb_cmd, 0x58 bytes */
	uint32_t cmd_type;
	uint32_t fwccb_flags;
	uint32_t data[20];
};
static_assert(sizeof(struct fwccb_cmd) == 88);

struct osinit {                          /* rogue_fwif_osinit, 0x68 bytes */
	fwaddr_t kernel_ccbctl_fw_addr;
	fwaddr_t kernel_ccb_fw_addr;
	fwaddr_t kernel_ccb_rtn_slots_fw_addr;
	fwaddr_t firmware_ccbctl_fw_addr;
	fwaddr_t firmware_ccb_fw_addr;
	fwaddr_t work_est_firmware_ccbctl_fw_addr;
	fwaddr_t work_est_firmware_ccb_fw_addr;
	fwaddr_t hwr_info_buf_ctl_fw_addr;
	uint32_t hwr_debug_dump_limit;
	fwaddr_t fw_os_data_fw_addr;
	uint8_t  comp_checks[64];
};
static_assert(offsetof(struct osinit, fw_os_data_fw_addr) == 36);
static_assert(offsetof(struct osinit, comp_checks) == 40);
static_assert(sizeof(struct osinit) == 104);

struct sysinit {                         /* rogue_fwif_sysinit, 0x220 bytes */
	uint64_t fault_phys_addr;
	uint64_t pds_exec_base;
	uint64_t usc_exec_base;
	uint64_t fbcdc_state_table_base;
	uint64_t fbcdc_large_state_table_base;
	uint64_t texture_heap_base;
	uint64_t hw_perf_filter;
	uint64_t slc3_fence_dev_addr;
	uint32_t tpu_trilinear_frac_mask[4];
	uint8_t  sigbuf_ctl[72];
	uint8_t  pdvfs_opp_info[136];
	uint8_t  coremem_data_store[16];
	uint8_t  counter_dump_ctl[8];
	uint32_t filter_flags;
	fwaddr_t runtime_cfg_fw_addr;
	fwaddr_t trace_buf_ctl_fw_addr;
	fwaddr_t fw_sys_data_fw_addr;
	fwaddr_t gpu_util_fw_cb_ctl_fw_addr;
	fwaddr_t reg_cfg_fw_addr;
	fwaddr_t hwperf_ctl_fw_addr;
	fwaddr_t align_checks;
	uint32_t initial_core_clock_speed;
	uint32_t active_pm_latency_ms;
	volatile uint32_t firmware_started;
	uint32_t marker_val;
	uint32_t firmware_started_timestamp;
	uint32_t jones_disable_mask;
	uint32_t firmware_perf;
	fwaddr_t core_clock_rate_fw_addr;
	uint32_t gpio_validation_mode;
	uint8_t  bvnc_km_feature_flags[160];
	uint32_t tfbc_compression_control;
};
static_assert(offsetof(struct sysinit, sigbuf_ctl) == 80);
static_assert(offsetof(struct sysinit, pdvfs_opp_info) == 152);
static_assert(offsetof(struct sysinit, coremem_data_store) == 288);
static_assert(offsetof(struct sysinit, counter_dump_ctl) == 304);
static_assert(offsetof(struct sysinit, runtime_cfg_fw_addr) == 316);
static_assert(offsetof(struct sysinit, trace_buf_ctl_fw_addr) == 320);
static_assert(offsetof(struct sysinit, fw_sys_data_fw_addr) == 324);
static_assert(offsetof(struct sysinit, align_checks) == 340);
static_assert(offsetof(struct sysinit, initial_core_clock_speed) == 344);
static_assert(offsetof(struct sysinit, firmware_started) == 352);
static_assert(offsetof(struct sysinit, firmware_started_timestamp) == 360);
static_assert(offsetof(struct sysinit, gpio_validation_mode) == 376);
static_assert(offsetof(struct sysinit, bvnc_km_feature_flags) == 380);
static_assert(offsetof(struct sysinit, tfbc_compression_control) == 540);
static_assert(sizeof(struct sysinit) == 544);

struct runtime_cfg {                     /* rogue_fwif_runtime_cfg */
	uint32_t active_pm_latency_ms;
	uint32_t runtime_cfg_flags;
	uint32_t active_pm_latency_persistant;
	uint32_t core_clock_speed;
	uint32_t default_dusts_num_init;
	uint32_t phr_mode;
	uint32_t hcs_deadline_ms;
	uint32_t wdg_period_us;
	uint32_t osid_priority[8];
	fwaddr_t hwperf_buf_fw_addr;
	uint32_t padding;
};
static_assert(sizeof(struct runtime_cfg) == 72);

struct sysdata {                         /* rogue_fwif_sysdata, 0xE28 bytes */
	volatile uint32_t config_flags;
	volatile uint32_t config_flags_ext;
	volatile uint32_t pow_state;
	uint32_t hw_perf[8];
	volatile uint32_t os_runtime_flags_mirror[8];
	uint8_t  pad0[4];
	uint8_t  fault_info[3456];
	uint32_t fw_faults;
	uint32_t cr_poll_addr[2];
	uint32_t cr_poll_mask[2];
	uint32_t cr_poll_count[2];
	uint32_t pad1;
	uint64_t start_idle_time;
	volatile uint32_t hwr_state_flags;
	uint32_t hwr_recovery_flags[9];
	uint32_t fw_sys_data_flags;
	uint32_t mc_config;
};
static_assert(offsetof(struct sysdata, os_runtime_flags_mirror) == 44);
static_assert(offsetof(struct sysdata, fault_info) == 80);
static_assert(offsetof(struct sysdata, fw_faults) == 3536);
static_assert(offsetof(struct sysdata, cr_poll_addr) == 3540);
static_assert(offsetof(struct sysdata, start_idle_time) == 3568);
static_assert(offsetof(struct sysdata, hwr_state_flags) == 3576);
static_assert(offsetof(struct sysdata, hwr_recovery_flags) == 3580);
static_assert(offsetof(struct sysdata, fw_sys_data_flags) == 3616);
static_assert(sizeof(struct sysdata) == 3624);

struct osdata {                          /* rogue_fwif_osdata, 0x248 bytes */
	uint32_t fw_os_config_flags;
	uint32_t fw_sync_check_mark;
	uint32_t host_sync_check_mark;
	uint32_t forced_updates_requested;
	uint8_t  slr[536];
	uint64_t last_forced_update_time;
	volatile uint32_t interrupt_count[2];
	volatile uint32_t kccb_cmds_executed;
	fwaddr_t power_sync_fw_addr;
	uint32_t fw_os_data_flags;
	uint32_t pad;
};
static_assert(offsetof(struct osdata, last_forced_update_time) == 552);
static_assert(offsetof(struct osdata, interrupt_count) == 560);
static_assert(offsetof(struct osdata, kccb_cmds_executed) == 568);
static_assert(offsetof(struct osdata, power_sync_fw_addr) == 572);
static_assert(sizeof(struct osdata) == 584);

struct tracebuf_space {                  /* rogue_fwif_tracebuf_space, 424 bytes */
	volatile uint32_t trace_pointer;
	fwaddr_t trace_buffer_fw_addr;
	uint32_t trace_buffer_host_ptr[2];
	uint8_t  assert_buf[408];
};
static_assert(sizeof(struct tracebuf_space) == 424);

struct tracebuf {                        /* rogue_fwif_tracebuf, 0x360 bytes */
	volatile uint32_t log_type;
	uint32_t pad;
	struct tracebuf_space tb[2];
	uint32_t tracebuf_size_in_dwords;
	uint32_t tracebuf_flags;
};
static_assert(offsetof(struct tracebuf, tb) == 8);
static_assert(offsetof(struct tracebuf, tracebuf_size_in_dwords) == 856);
static_assert(sizeof(struct tracebuf) == 864);

/* Config flags (sysdata.config_flags) */
#define INICFG_DISABLE_DM_OVERLAP (1u << 19)

/* ------------------------------------------------- contexts and cCCB --- */
struct dllist_node { fwaddr_t p, n; };

struct ufo { fwaddr_t addr; uint32_t value; };

struct fwcommoncontext {                 /* rogue_fwif_fwcommoncontext, 0x98 bytes */
	fwaddr_t ccbctl_fw_addr;
	fwaddr_t ccb_fw_addr;
	uint64_t ccb_meta_dev_addr;
	fwaddr_t ccb_meta_fw_addr;
	uint32_t pad0;
	fwaddr_t context_state_addr;
	uint32_t fw_com_ctx_flags;
	uint32_t priority;
	uint32_t priority_seq_num;
	fwaddr_t rf_cmd_addr;
	uint32_t stats_pending;
	int32_t  stats_num_stores;
	int32_t  stats_num_out_of_memory;
	int32_t  stats_num_partial_renders;
	uint32_t dm;
	uint64_t wait_signal_address;
	struct dllist_node wait_signal_node;
	struct dllist_node buf_stalled_node;
	uint64_t cbuf_queue_ctrl_addr;
	uint64_t robustness_address;
	uint32_t max_deadline_ms;
	uint32_t read_offset_needs_reset;
	struct dllist_node waiting_node;
	struct dllist_node run_node;
	struct ufo last_failed_ufo;
	fwaddr_t fw_mem_context_fw_addr;
	uint32_t server_common_context_id;
	uint32_t pid;
	uint32_t geom_oom_disabled;
};
static_assert(offsetof(struct fwcommoncontext, context_state_addr) == 24);
static_assert(offsetof(struct fwcommoncontext, priority) == 32);
static_assert(offsetof(struct fwcommoncontext, dm) == 60);
static_assert(offsetof(struct fwcommoncontext, robustness_address) == 96);
static_assert(offsetof(struct fwcommoncontext, waiting_node) == 112);
static_assert(offsetof(struct fwcommoncontext, run_node) == 120);
static_assert(offsetof(struct fwcommoncontext, last_failed_ufo) == 128);
static_assert(offsetof(struct fwcommoncontext, fw_mem_context_fw_addr) == 136);
static_assert(offsetof(struct fwcommoncontext, pid) == 144);
static_assert(sizeof(struct fwcommoncontext) == 152);

struct cdm_registers_cswitch {           /* rogue_fwif_cdm_registers_cswitch */
	uint64_t cdm_context_pds0;
	uint64_t cdm_context_pds1;
	uint64_t cdm_terminate_pds;
	uint64_t cdm_terminate_pds1;
	uint64_t cdm_resume_pds0;
	uint64_t cdm_context_pds0_b;
	uint64_t cdm_resume_pds0_b;
};

struct fwcomputecontext {                /* rogue_fwif_fwcomputecontext */
	struct fwcommoncontext cdm_context;
	struct cdm_registers_cswitch ctxswitch_regs;
	uint32_t work_est_ccb_submitted;
	uint32_t compute_ctx_flags;
	uint32_t wgp_state;
	uint32_t wgp_checksum;
	uint32_t core_mask_a;
	uint32_t core_mask_b;
};

struct fwmemcontext {                    /* rogue_fwif_fwmemcontext, 32 bytes */
	uint64_t pc_dev_paddr;
	uint32_t page_cat_base_reg_set;
	uint32_t breakpoint_addr;
	uint32_t bp_handler_addr;
	uint32_t breakpoint_ctl;
	uint32_t fw_mem_ctx_flags;
	uint32_t padding;
};
static_assert(sizeof(struct fwmemcontext) == 32);

struct ccb_cmd_header {                  /* rogue_fwif_ccb_cmd_header, 0x28 bytes */
	uint32_t cmd_type;
	uint32_t cmd_size;
	uint32_t ext_job_ref;
	uint32_t int_job_ref;
	uint32_t work_est[6];
};
static_assert(sizeof(struct ccb_cmd_header) == 0x28);

struct cleanup_ctl { volatile uint32_t submitted_commands, executed_commands; };

struct compute_regs {                    /* rogue_fwif_compute_regs, 72 bytes */
	uint64_t tpu_border_colour_table;
	uint64_t cdm_cb_queue;
	uint64_t cdm_cb_base;
	uint64_t cdm_cb;
	uint64_t cdm_ctrl_stream_base;
	uint64_t cdm_context_state_base_addr;
	uint32_t tpu;
	uint32_t cdm_resume_pds1;
	uint32_t cdm_item;
	uint32_t compute_cluster;
	uint32_t tpu_tag_cdm_ctrl;
	uint32_t padding;
};
static_assert(sizeof(struct compute_regs) == 72);

struct cmd_compute {                     /* rogue_fwif_cmd_compute, 0x60 bytes */
	uint32_t frame_num;
	uint32_t pad;
	struct compute_regs regs;
	uint32_t flags;
	uint32_t num_temp_regions;
	uint32_t stream_start_offset;
	uint32_t execute_count;
};
static_assert(offsetof(struct cmd_compute, flags) == 80);
static_assert(sizeof(struct cmd_compute) == 0x60);
