<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# Experiment log (logRefs)

This folder records the experiments on the board that produced a new finding: a
behaviour the spec did not describe, an assumption confirmed or refuted, or a hardware
fact found by observation. Each file covers one experiment and has these parts:

- **Question**: what we wanted to know
- **Set-up**: firmware build, kernel, commands
- **Observation**: raw values, with large dumps in `data/`
- **Conclusion**: what changed in the code or the docs
- **Clean-room note**: why the method is acceptable

Clean-room rule for every experiment: we observe **our own firmware** and the GPU
hardware it drives, through the public host driver and `/dev/mem`. The vendor firmware
is never run, read or disassembled, and no dirty-team material is used.

| Id | Date | Topic | Result |
|---|---|---|---|
| [E001](E001-sys-bus-secure-reset.md) | 2026-11-08 | MIPS executes nothing after reset | `SYS_BUS_SECURE_RESET` must be in the device info |
| [E002](E002-exl-in-c-handlers.md) | 2026-11-08 | TLB miss inside an interrupt handler | C handlers must run with `EXL = 0` |
| [E003](E003-tlb-flush-from-unmapped-page.md) | 2026-11-08 | TLB flush kills the firmware | the flush must run from an unmapped page |
| [E004](E004-register-read-power-off-hang.md) | 2026-11-08 | SoC hang during register sampling | never read RGX registers with the domain off |
| [E005](E005-runtime-pm-cycle.md) | 2026-11-08 | Runtime PM and warm boot | full cycle works with the full boot path at every power-on |
| [E006](E006-compute-kick-mmu-fault.md) | 2026-11-08 | First compute kick | CDM starts, MMU fault at VA `0x26C0`: exec base missing |
| [E007](E007-register-survey.md) | 2026-11-08 | Read-only register survey `0x0000–0x2000` | reset and boot values, fault state |
| [E008](E008-writable-mask-probe.md) | 2026-11-08 | Writable-bit map | `0x0610` (40-bit, 1 MiB) and `0x0FC8` (bits 39:34) are exec-base candidates |
| [E009](E009-pds-exec-base-0x0610.md) | 2026-11-08 | `pds_exec_base` → `0x0610` | the `0x26C0` fault disappears: `0x0610` = `PDS_EXEC_BASE`; task still never completes; SoC hangs after a driver reload |
| [E010](E010-firmware-fallback.md) | 2026-11-08 | Truncated firmware file after a hang | kernel falls back to `/lib/firmware/powervr/` |
| [E011](E011-event-enable-0x0128.md) | 2026-11-08 | `0x0128` event enable | it is `EVENT_ENABLE` (mask `0x8FFFE`); it routes events to the host IRQ, so it must stay 0; next fault at `0x7B80` (USC base?) |
| [E012](E012-usc-base-0x0fc8.md) | 2026-11-08 | `usc_exec_base` → `0x0FC8` | value accepted; no fault, no completion; `PERF_COMPUTE_CYCLE` shows the CDM active and stuck (USC base was elsewhere, see E013) |
| [E013](E013-gpu-units-init-compute-pass.md) | 2026-11-08 | Full GPU bring-up from the updated spec | **first compute jobs pass** (`empty_shader`, `copy_ssbo_*`, barriers, atomics) |
| [E014](E014-tq3d-kick-does-not-start.md) | 2026-11-08 | TQ_3D transfer kick | programmed per spec, `ISP_RENDER = 0x13`, but `PERF_3D_CYCLE` stays 0; `0x06A0..0x06B8` = PDS_BGRND0..3 |
| [E015](E015-slc-flush-before-mmu-inval.md) | 2026-11-08 | MMU fault in a new memory context | flush and invalidate the SLC before MMU invalidation |
| [E016](E016-completion-path-counters.md) | 2026-11-08 | Completion delivery | BG tasks work; the IRQ task seemed silent (**withdrawn by E018**) |
| [E017](E017-tq3d-launch-fault-at-zero.md) | 2026-11-08 | TQ_3D with the `0x0F00` launch | **the 3D pipe starts**, then writes to address 0 (Q12) |
| [E018](E018-irq-task-delivery.md) | 2026-11-08 | IRQ task delivery | HW2 fires on every completion; E016 was the poll winning a race |
| [E019](E019-hwr-and-compute-basic-67.md) | 2026-11-08 | HWR + full compute group | recovery in ~0.28 ms; `compute.pipeline.basic` **67/82 pass**, 6 fail (images), 9 not supported |
| [E020](E020-tq3d-pbe-two-banks.md) | 2026-11-08 | TQ_3D with the corrected Q12 table | PBE in two banks, tiles-in-flight, conditional mtile base: the write to address 0 remains; host PBE words carry no address (Q13) |
| [E021](E021-watchdog-deadline-wider-compute.md) | 2026-11-08 | Watchdog and wider compute groups | 2 s killed a long job; now uses the context's `max_deadline_ms`; indirect_dispatch 24/24, builtin_var 11/11 |
| [E022](E022-tq3d-completes-first-tile-lost.md) | 2026-11-09 | TQ_3D with the full Q13 table | **TQ jobs complete, no fault** (conditional registers were the cause); multi-tile transfers lose their first tile, non-deterministically (Q14) |
| [E023](E023-tq-launch-order-and-dm-flush.md) | 2026-11-09 | TQ launch order + per-DM completion flush | **no failures**: compute basic 73/73, indirect 24/24, builtin_var 11/11, fill_and_update 72/72, buffer_to_buffer 8/8; `ISP_RENDER` must be the write right before `0x0F00` |
| [E024](E024-vkfwtest.md) | 2026-11-09 | `vkfwtest` workloads | compute and transfer scenarios run cleanly; graphics scenarios finish but draw nothing (3D/TA kicks missing); no preemption |
| [E025](E025-stats-and-reset-notification.md) | 2026-11-09 | Statistics, HWR log, reset notification | utilisation and time correlation published; overrun and page-fault notifications decoded by the host (`fault address=26c0`) |
| [E026](E026-slcflush-copy-and-blit-stability.md) | 2026-11-09 | `SLCFLUSHINVAL`, `copy_and_blit`, stability | real SLC flush (not sent by mainline); copy_and_blit mostly passes, deterministic offset/NPOT/mirror failures not caused by caches; stability run interrupted by an external reboot |
| [E027](E027-render-structures-by-value.md) | 2026-11-09 | Host render structures vs. observed registers | `0x0C98`/`0x0C18`/`0x0C08`/`0x0C10`/`0x0C20`/`0x0C78`/`0x0C80`/`0x0418` mapped to `hwrtdata_common` and context-state fields |
| [E028](E028-ta-kick-bringup.md) | 2026-11-09 | First TA kick | canary method finds `0x0CB0` = TA context-state base; geometry runs and fetches its first 64 B, but the PM hands out no pages |
| [E029](E029-ta-pm-registers-from-answers.md) | 2026-11-09 | TA with the Q16b/Q16c answers | local FL counts and geometry constants applied, `0x0420` counts kicks as in the original; still no PM allocation; global-FL guess in the 0x2000 block changes nothing (Q16e) |
| [E030](E030-ta-global-freelist-pb-base.md) | 2026-11-09 | TA with the global FL bank and `CONTEXT_PB_BASE` | all identified PM registers programmed; still no allocation; `0x0D00 = 0x0F` is the inverse of the original's running state; PM catalogue setup is the remaining gap (Q16f) |
| [E031](E031-pm-live-stack-pointers.md) | 2026-11-09 | Global SP `0x2088` and live counters | these live registers ignore writes and stay 0; PM catalogues stay 0 too: the PM sees empty free lists (Q16g) |
| [E032](E032-pm-live-sp-load-variants.md) | 2026-11-09 | Q16g applied; load variants of the PM live SPs | none loads `0x20C8`/`0x0348`; `0x02A8` acks `PM_PAGE_MANAGEOP`; writes between the handshakes fault at address 0 (Q16h) |
| [E033](E033-pm-first-load-flags.md) | 2026-11-09 | Q16h first-load PM registers | idle PM state equals the original except `0x20F8`; with `0x20F8`/`0x03D0`/`0x03D4` written the live SPs still stay 0 (Q16i) |
| [E034](E034-pm-copy-q16i-variants.md) | 2026-11-09 | Q16i answer applied | `0xF320` back to 0, unit values and catalogue index matched; the live SPs still stay 0; the missing step changes no value (Q16j) |
| [E035](E035-pm-freelist-load-commands.md) | 2026-11-09 | Free-list load commands (Q16j answer) | write 1, wait bit 0 = 0 after the value registers: the PM loads the free lists, allocates, and the **TA finishes** |
| [E036](E036-first-3d-render.md) | 2026-11-09 | Fragment kick with PM context 1 load | **first correct image**; the PM does not free the pages and the next draw faults (Q16k) |
| [E037](E037-pm-list-sp-load-commands.md) | 2026-11-09 | ALIST/MLIST SP load commands | `0x0288`/`0x0190` load the list SPs of the PM context being loaded; pages freed; `simple_draw` 4/4, `api.smoke` 8/8, `basic_draw` 201/331 |
| [E038](E038-zls-mem-free-and-depth.md) | 2026-11-10 | Q16l: ZLS, MSAA words, `0x0F28`, `0x0C68`, `PM_3D_MEM_FREE` | `basic_draw` 203/331 with 0 fails; depth works; our IRQ task was clearing `PM_3D_MEM_FREE`; input-attachment renders fault (Q16m) |
| [E039](E039-input-attachment-diff.md) | 2026-11-10 | Input-attachment render diff (Q16m) | bases match; `0x0F18` stays `isp_mtile_size`; non-first kicks skip the PM load; shared-PB mode is the next lead |
| [E040](E040-input-attachment-dump-diff.md) | 2026-11-10 | Diff against the reference dump | dumped ranges now match; shared-PB rule; still faults; **board hang** during `basic_draw` (Q16n) |
| [E041](E041-render-default-and-spm.md) | 2026-11-26 | Rendering by default, PM module, SPM, tools (16 GB board) | `basic_draw` 203/0, smoke 8/8 on the default build; oracle stage by stage (C all pass but NPOT copies, D, B `xfer`/`pre-cdm` pass); OOM ready-pages update matches the reference but the TA does not resume (Q28); interleaved-context board hang fixed (PM store before evict, dirty flag); the second render on a device stalls the TA (Q29) |
| [E042](E042-spec-fab1fc9-tpc-oom-init.md) | 2026-11-27 | Spec `fab1fc9`: GPU-init defaults, TA-finished TPC/RTC flush, OOM resume, `FRAG_PR` queueing | `copy_and_blit` 23/23 (Q25 fixed); `bigpb` frame 0 completes through ~30 OOM events (Q28 fixed); every render on the second HWRTData of a dataset still fails (Q29b) |
| [E043](E043-pm-mmu-inval-first-kick-reset-hwr.md) | 2026-11-27 | Spec `d244503`: PM-MMU invalidation, first-kick PM reset, full HWR teardown, partial render reached | **`pre-3d`, `pre-ta`, input attachments (72/72) pass** (second HWRTData fixed); stage C complete; `bigpb` frames 0–2 pass; partial render runs once, the second restart faults (Q30); one variant **hung the board**; GPU never suspends after compute/sync suites (older bug) |
| [E044](E044-vkfwtest-harness.md) | 2026-11-27 | `vkfwtest`: pixel-checking Vulkan harness (own repository) | A, D, B `pre-cdm`/`xfer` pass; **renders on the second HWRTData and renders after a parameter-buffer OOM write nothing** (no fault): `multiframe`, `pre-ta`, `bigpb`, `input-attach` frame 1 fail; `rta` (layered clears) passes; `hwr-lockup`: no `VK_ERROR_DEVICE_LOST`; 12 more tests (texture, ZS load, MSAA, MRT, sizes, …): `msaa` and lone `sizes` pass, occlusion queries return 0, render after a transfer is empty; full 46-test battery on the board: 16 pass / 20 fail / 3 skip, compute, transfer, sync and memory all pass; vs the manufacturer firmware on the same stack (18 / 18 / 3, `docsClean` `50055ae`): same result on 36 of 39 tests, only `queries`, `indirect` and `vertex-ext` (`divisor 4`) differ |
| [E045](E045-rta-fw-1.1.md) | 2026-11-27 | Render-target arrays (firmware 1.1), Q31–Q36 answers (`dd37220`, `b50fabe`, `88e48e7`), Mesa `5d2dc45` cross-compiled for the Arch board | **RTA matches the manufacturer firmware on every `vkfwtest rta` step**: layer loop, RTC bitmap read through a GPU-MMU walk, `RENDER_TGT_ID` (`0x03D8`), `CR[0x03E0]`; 6 layers colour + D32, per-layer stencil, 3D image pass; the `LOAD` subset step and `msaa` fail on both firmwares (Mesa, Q34/Q36); **`queries` passes** (`CR[0x0FC0]` + vkfwtest timestamp fix); open: `FRAG_PR` layer loop (after Q30), kernel UAPI 1.1.0 + FW_INFO |
| [E046](E046-gpu-clock.md) | 2026-11-27 | GPU clock 307.2 MHz vs 819.2 MHz vs 1 GHz (DT `assigned-clock-rates`, GRUB test entries) | 307.2 and 819.2 MHz: identical battery (18 pass / 22 fail), no fault; 1 GHz: intermittent single-bit errors (`xfer`, `image-copy` 1 in 10); 1 GHz is the highest parent without reprogramming a PLL; the clock does not explain `indirect`/`vertex-ext` |
| [E047](E047-vkfwtest-investigation-tests.md) | 2026-11-27 | New vkfwtest tests (`second-*`, `--baseline`, `bigpb-layers`, `suspend-after`, `ctx-switch`, `rta` multiview, `msaa` sample positions); cause of the second-render failures | **The "second render writes nothing" failures are a Mesa `pvr` bug**: it ignores render-pass dependencies to `VK_SUBPASS_EXTERNAL`, so the read-back copy races the render (no `FENCE` reaches the transfer CCB); with an explicit barrier in vkfwtest's read-back: **42 pass / 6 fail (only the Mesa-bug detectors) / 2 skip**, every test that failed with the manufacturer firmware passes, `indirect` too |

The raw data is in [data/](data/).
