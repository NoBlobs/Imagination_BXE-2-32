# GPU registers used by the firmware

GPU control registers that the firmware accesses. Names, offsets and field names follow the
public definitions in `pvr_rogue_cr_defs.h` (upstream `drm/imagination`), as `ROGUE_CR_*`.
Registers the public header does not name are given by offset only ("no public name"); their
role is **D** (firmware analysis) and/or **O** (observed).

## Access

- **Firmware (MIPS):** the bank of core *n* is at `0xCF800000 + n·0x80000` (written
  `CR[off]` for the primary core in these documents); the **XPU broadcast** bank is at
  `0xCFA00000` (`BCAST[off]`): one write goes to every core enabled in
  `ROGUE_CR_XPU_BROADCAST` (`0x0890`).
- **Host:** MMIO at `0xCAC00000` (SpacemiT glue) + offset.
- `ROGUE_CR_MULTICORE_SYSTEM` (`0xF308`) gives the number of cores (1 on this SoC). The
  firmware must remember which core is the **primary core** (used by the kicks).

## Events and MTS

| Register | Offset | Role |
|---|---|---|
| `ROGUE_CR_EVENT_STATUS` / `ROGUE_CR_EVENT_CLEAR` | `0x0130` / `0x0138` | GPU events (public field names): b1 `KERNEL_FINISHED`, b2 `COMPUTE_FINISHED`, b3 `PIXELBE_END_RENDER`, b5 `TA_FINISHED`, b6 `TA_TERMINATE`, b7 `PM_OUT_OF_MEMORY`, b8 `PM_3D_MEM_FREE`, b9 `MMU_PAGE_FAULT`, b10 `POWER_COMPLETE`, b11 `POWER_ABORT`, b12 `GPIO_REQ`, b13 `GPIO_ACK`, b15 `USC_TRIGGER`, … |
| `ROGUE_CR_MTS_SCHEDULE` | `0x0B00` | `0` = schedule the BG task; `0x20` (`CONTEXT` field, bit 5) = schedule the IRQ task |
| (no public name: MTS task end) | `0x0B08` | `0` = end of BG, `2` = end of IRQ (read back to order) |
| `ROGUE_CR_MTS_DM0_INTERRUPT_ENABLE` | `0x0B58` | b12 = power/GPIO interrupt |
| `ROGUE_CR_MIPS_WRAPPER_IRQ_STATUS` | `0x08A8` | write `1` = IRQ to the host |

> Polling `EVENT_STATUS` (`0x0130`) after a kick is where a hung GPU is detected (failed
> poll → HWR). See [hardware-recovery](hardware-recovery.md).

## Timer and revision

| Register | Offset | Role |
|---|---|---|
| `ROGUE_CR_TIMER` | `0x0160`/`0x0164` | 48-bit counter at clock/256 (about 2.4 MHz) |
| `ROGUE_CR_CORE_REVISION` | `0x0020` | read at the end of every MIPS write barrier |

## MIPS wrapper / remap

| Register | Offset | Role |
|---|---|---|
| `ROGUE_CR_MIPS_WRAPPER_CONFIG` | `0x0810` | `BOOT_ISA_MODE = MICROMIPS`, `REGBANK_BASE_ADDR` |
| `ROGUE_CR_MIPS_WRAPPER_IRQ_ENABLE` | `0x08A0` | enables the wrapper interrupts |
| `ROGUE_CR_MIPS_ADDR_REMAP_RANGE_CONFIG` | `0x0878`/`0x087C` | 32-bit pair per remap entry (see [memory-model](memory-model.md)) |

## BIF (GPU MMU / memory contexts)

| Register | Offset | Role |
|---|---|---|
| `ROGUE_CR_BIF_CAT_BASE0..7` | `0x1200 + 8n` | page catalogues (slot 0 = FW; 1–7 = contexts) |
| `ROGUE_CR_BIF_CAT_BASE_INDEX` | `0x1240` | slot per requester: fields `TA` [2:0], `PIXEL` [10:8], `CDM` [18:16], `TDM` [21:19], … |
| `ROGUE_CR_BIF_PM_CAT_BASE_{VCE,TE,ALIST}{0,1}` | `0x1248`–`0x1280` | catalogues of the 2 PMs |
| `ROGUE_CR_BIF_FAULT_BANK0_MMU_STATUS` | `0x12B0` | fault status per core (used to abort polls) |
| `ROGUE_CR_BIF_CTRL_INVAL` | `0x12A0` | invalidation: b0 `PT`, b1 `PD`, b2 `PC`, b3 `TLB1` |
| (no public name: PM TLB) | `0x1348` | PM TLB invalidation |

## SLC (system level cache)

| Register | Offset | Role |
|---|---|---|
| `ROGUE_CR_SLC_CTRL_FLUSH_INVAL` | `0x3818` | b0 `ALL`, b1 `DM_TA`, b2 `DM_PIXEL`, b3 `DM_COMPUTE`, b4 `DM_MMU`, … |
| `ROGUE_CR_SLC_STATUS0` | `0x3820` | b2 `FLUSH_INVAL_PENDING` (poll until clear) |

## Fence / performance / HWR / kicks

| Register | Offset | Role |
|---|---|---|
| `ROGUE_CR_MCU_FENCE` | `0x1740` | write fence; `DM` field [42:40], `ADDR` field |
| `ROGUE_CR_PERF_TA_CYCLE` / `ROGUE_CR_PERF_TA_PHASE` | `0x6020` / `0x6008` | geometry progress (lockup check) |
| `ROGUE_CR_PERF_3D_CYCLE` / `PERF_3D_PHASE` | `0x6028` / `0x6010` | same for 3D |
| `ROGUE_CR_PERF_COMPUTE_CYCLE` / `PERF_COMPUTE_PHASE` | `0x6030` / `0x6018` | same for compute |
| `ROGUE_CR_SOFT_RESET` | `0x0100` | GPU reset during HWR |
| `ROGUE_CR_ISP_RENDER` / `ROGUE_CR_ISP_CTL` | `0x0F08` / `0x0F38` | 3D render start / ISP pipes |
| `ROGUE_CR_CDM_CONTEXT_PDS0/1`, `ROGUE_CR_CDM_TERMINATE_PDS/1` | `0x04A8`/`0x04B0`, `0x04B8`/`0x04C0` | compute kick |
| `ROGUE_CR_MULTICORE_FRAGMENT_CTRL_COMMON` / `ROGUE_CR_MULTICORE_COMPUTE_CTRL_COMMON` | `0xF310` / `0xF330` | multicore control in the 3D / compute kicks |

Kick registers without a public name are listed in [kick-geometry-ta](kick-geometry-ta.md)
and [kick-compute-and-3d](kick-compute-and-3d.md).

## Power / misc

| Register | Offset | Role |
|---|---|---|
| (no public name: power request) | `0x0038` | b0 `1` = on / `0` = off, b1 request |
| (no public name: active dusts) | `0x0050` | `(1 << n) − 1` |
| (no public name: pending XPU events) | `0x08F0` | multicore: events to process |
| (no public name: CDM run / stream base) | `0x0478` / `0x0480` | CDM kick (also used for the firmware's own compute jobs, e.g. local memory clear) |
| (no public name: PM page manage op / ack) | `0x02A0` / `0x02A8` | the trace calls it `PM_PAGE_MANAGEOP` / its ack |
| (no public name: power monitor) | `0x17B0`, `0x6300`/`0x6330` | per-core energy counter |
| (no public name: GPIO) | `0x0150` / `0x0158` / `0x0B90` | GPIO input / ack / event re-arm (see [observed-sequences](observed-sequences.md)) |
| `ROGUE_CR_SCRATCH15` | `0x1A78` | the firmware publishes the busy-DM mask here (debug) |

USC (shader) breakpoints, no public names: `0x4030` address, `0x4038` handler, `0x4040`
control (b2:0 slot/DM, b3 enable), `0x4050`/`0x06D8` temporaries — see
[execution-environment](execution-environment.md).
