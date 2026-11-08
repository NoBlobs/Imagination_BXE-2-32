# Execution environment (MIPS core)

## ISA and ABI

- **MIPS32r2**, **microMIPS** mode (the RGX wrapper sets the boot ISA to microMIPS through
  `ROGUE_CR_MIPS_WRAPPER_CONFIG`, offset `0x0810`, field `BOOT_ISA_MODE`).
- **o32 ABI**, **soft-float** (no FPU; all floating-point maths is done in software).
- **Little-endian**, ELF32 `EM_MIPS`.
- Any microMIPS o32 soft-float toolchain works (e.g. bare-metal GCC or Clang).

The image is delivered as an ELF with code/data sections, packed into a `.fw` container
(see [memory-model](memory-model.md) for the header and layout table the host reads).

## Boot sequence

1. The host loads the firmware sections into DRAM, builds the MIPS page tables and the
   wrapper **remap registers** (see [memory-model](memory-model.md)), fills the
   **BOOT_DATA** block with physical addresses (page tables, stack, register bank), sets
   `rogue_fwif_sysinit.firmware_started = false`, and releases the MIPS core from reset.
2. The MIPS starts at the **reset/NMI vector at `0xBFC00000`** (KSEG1, uncached). Early
   boot:
   - initialises CPU state (CP0) and the caches;
   - sets up the fixed (*wired*) TLB entries for the register bank, page tables, stack and
     private data;
   - jumps to the main initialisation.
3. Main initialisation:
   - programs the CP0 performance counters (`PerfCtl0/1`) from
     `rogue_fwif_sysinit.firmware_perf`;
   - sets up the remap for the **fault page** (`rogue_fwif_sysinit.fault_phys_addr`);
   - enables the wrapper interrupts (`ROGUE_CR_MIPS_WRAPPER_IRQ_ENABLE`, `0x08A0`);
   - takes the configuration flags from `sysinit`/`osinit` and the initial core clock
     from `rogue_fwif_sysinit.initial_core_clock_speed`;
   - initialises its subsystems (shared data, GPU, timers, BIF, scheduler — see
     [scheduler](scheduler.md));
   - **sets `rogue_fwif_sysinit.firmware_started = true`** (and
     `firmware_started_timestamp`). The mainline host polls this flag and treats the boot
     as failed if it is not set within **5 s** (**P**);
   - enters a permanent **`WAIT`**.

> **Important:** reset/NMI is also the **warm-boot** path. During power management (see
> [power-management](power-management.md)) the host may restart the firmware, which
> re-enters at `0xBFC00000`. On an NMI the firmware first **saves the MIPS state** into
> its private BOOT_DATA area, for diagnosis.

## There is no main loop

After `WAIT`, all work happens in **three interrupt sources**:

| Source | Vector | Responsibility |
|---|---|---|
| **HW0 — timer** | `0x9FC02400` | `CP0 Count/Compare` expiry. Sets a "tick pending" flag and schedules a background task. Does no work in the ISR. |
| **HW1 — BG task (MTS)** | `0x9FC02500` | Processes **all** Kernel CCB commands; HWR watchdogs; expired timers; re-evaluates the contexts on each DM's *wait list*; SPM list. |
| **HW2 — IRQ task (MTS)** | `0x9FC02600` | Reads `ROGUE_CR_EVENT_STATUS`: MMU faults, per-DM completion (geometry/3D/compute/USC), multicore XPU events; runs the scheduler; triggers HWR and power management; updates utilisation. |

"MTS" is the RGX hardware's *Micro Task Scheduler*, which delivers the BG and IRQ tasks to
the MIPS as interrupts. The firmware schedules a BG task by writing
`ROGUE_CR_MTS_SCHEDULE = 0`.

### Interrupt model (CP0)

- `Cause.IV = 1` (separate interrupt vectors), `IntCtl.VS = 256` (256-byte spacing between
  vectors — hence the vectors at `…400/…500/…600`).
- `Status.IM2..IM4` enabled (mask `0x1C00`): HW0 (timer), HW1 (BG), HW2 (IRQ).
- **Critical sections:** the firmware disables interrupts by clearing `Status.IE` and
  restores the previous value on exit. Each Kernel CCB command is processed with `IE`
  cleared.
- **End-of-task acknowledgement (O/D):** when a BG or IRQ task finishes, the firmware
  writes the wrapper register at offset `0x0B08` (`0` = end of BG, `2` = end of IRQ; no
  public name) and reads it back to enforce ordering.

## Timers

- Deadlines are kept in units of `ROGUE_CR_TIMER` (the GPU's global 48-bit clock, ~2.4 MHz
  on the reference board).
- For the next deadline the firmware programs `CP0 Compare = (deadline − now) · 0x80` and
  clears `CP0 Count`. The MIPS `Count` runs **128× faster** than `ROGUE_CR_TIMER` (core
  clock/2 against core clock/256), hence the factor `0x80`.
- The timer ISR only sets the tick-pending flag and schedules BG; deadline processing (HWR
  watchdogs, context-switch timeouts, APM) happens in the BG task.

Timers are used for: the context-switch watchdog (HCS), the power deadline (APM latency),
health checks, and operation timeouts.

## Caches and TLB

### TLB (16 entries)

- The TLB **always maps VA → "MIPS PA" as identity** (PFN = VA >> 12). The real translation
  to the 36-bit bus is done by the wrapper's **remap registers** (see
  [memory-model](memory-model.md)) — that is how a 32-bit MIPS reaches 36 bits.
- Some entries are **fixed** (*wired*): register bank, page tables, stack, private data.
- The remaining entries are filled **on demand** by the **TLB refill vector**
  (`0x9FC02000`), which reads the physical PFN from the host page table and puts it into
  the remap entry.
- Before mapping host memory into a temporary window, the firmware **invalidates the
  dynamic entries**: each non-wired slot gets a distinct, never-used VPN with
  `EntryLo0/1 = 0`, and its pair of remap entries (`slot` and `slot+16`) is disabled.

### Caches (D-cache / I-cache)

- **D-cache write-back** with index-type `CACHE` operations over the whole cache, used in
  the power teardown and before publishing data.
- **D-cache invalidate by address** (hit-type `CACHE` operations) over freshly written
  host data, e.g. new Client CCB bytes (see [kernel-ccb](kernel-ccb.md)).
- **I-cache** primed for the boot code at reset.

### Write barrier (host↔FW visibility)

Before **publishing data to the host** or raising an interrupt to the host, the firmware
runs a deterministic barrier:

1. writes `1` to an **uncached** word reserved for this purpose (in the private part of the
   BOOT_DATA page, see [memory-model](memory-model.md));
2. `EHB` (execution hazard barrier);
3. `SYNC`;
4. reads the barrier word back;
5. reads `ROGUE_CR_CORE_REVISION` (offset `0x20`) — a register read that forces ordering on
   the bus.

Only then does it write the host-visible offset/value or raise
`ROGUE_CR_MIPS_WRAPPER_IRQ_STATUS` (`0x08A8`).

## MIPS performance counters

`CP0 PerfCnt0/1` (configured through `PerfCtl0/1` from `sysinit.firmware_perf`) can be
sampled into HWPerf data. HWPerf is optional for the mainline target (see
[hwperf-trace](hwperf-trace.md)).

## Shader (USC) breakpoints

Firmware-only registers, programmed through XPU broadcast, allow breakpoints in USC
shaders: `0x4030` address, `0x4038` handler, `0x4040` control (bits 2:0 slot/DM, bit 3
enable), `0x4050`/`0x06D8` temporaries (**D**, no public names). They are enabled per
memory context (`rogue_fwif_fwmemcontext.breakpoint_addr`, `bp_handler_addr`,
`breakpoint_ctl`) through the KCCB `BP` command. See [kernel-ccb](kernel-ccb.md).
