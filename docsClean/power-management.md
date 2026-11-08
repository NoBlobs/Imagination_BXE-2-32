# Power management

The firmware implements **active power management (APM)**: it lets the GPU power domain be
turned off when the GPU is idle, and is restarted (warm boot) when work arrives.
Synchronisation with the host is critical: the host polls a power object and suspends or
resumes according to the result.

> **Provenance.** Command, enum and field names and the host sequence: **P**
> (`pvr_rogue_fwif.h`, `pvr_power.c`). Teardown order and the trace sequence: **O**.
> Requirements on idle detection: **D** + **O**.

## Power state

`rogue_fwif_sysdata.pow_state` (enum `rogue_fwif_pow_state`): `ROGUE_FWIF_POW_OFF` (0),
`ROGUE_FWIF_POW_ON` (1), `ROGUE_FWIF_POW_FORCED_IDLE` (2), `ROGUE_FWIF_POW_IDLE` (3, idle and
waiting for the host handshake). The firmware tracks idle/active per DM and the time idle
started (`rogue_fwif_sysdata.start_idle_time`).

The host considers the GPU idle when `pow_state == ROGUE_FWIF_POW_IDLE` **and** the KCCB is
empty (the state can be stale if a KCCB command has not been processed yet). The firmware
must therefore set `ROGUE_FWIF_POW_IDLE` only when it is truly idle, and leave it as soon as
it accepts new work.

## Power command (KCCB `ROGUE_FWIF_KCCB_CMD_POW`) and synchronisation

The host requests power transitions with the KCCB `POW` command
(`rogue_fwif_power_request`):

| `pow_type` | Meaning |
|---|---|
| 1 `ROGUE_FWIF_POW_OFF_REQ` | power off (`power_req_data.forced`) |
| 2 `ROGUE_FWIF_POW_FORCED_IDLE_REQ` | force idle without powering off (`power_req_data.pow_request_type`: `ROGUE_FWIF_POWER_FORCE_IDLE` / cancel) |
| 3 `ROGUE_FWIF_POW_NUM_UNITS_CHANGE` | change the number of active units (`power_req_data.num_of_dusts`) |
| 4 `ROGUE_FWIF_POW_APM_LATENCY_CHANGE` | change the APM latency |

Synchronisation: before each `POW` command the host writes `0` to the power sync object
(address in `rogue_fwif_osdata.power_sync_fw_addr`), sends the command, then polls the
object until it is non-zero, with a timeout. The firmware writes **`1`** there when **the
transition has finished**.

The mainline host suspend sequence is: `FORCED_IDLE_REQ` (`ROGUE_FWIF_POWER_FORCE_IDLE`) →
wait for sync → `OFF_REQ` (`forced = true`) → wait for sync → stop the firmware (MIPS held
in reset).

**Requirement (critical):**

- for **FORCED_IDLE**, signal with the GPU still on, after it has stopped accepting work;
- for **OFF**, signal **only after the whole teardown has completed** (caches, TLB, MMU,
  BIF slots). Every teardown step must finish in finite time.

If the signal does not arrive, the host suspend times out, runtime PM fails and the device
is marked lost. This is the most serious possible power-management failure.

## APM: power off when idle

- The APM latency is `rogue_fwif_sysinit.active_pm_latency_ms` (also in
  `rogue_fwif_runtime_cfg`). With latency `0` the firmware reports "ready to power down" as
  soon as it is idle. With latency > 0 it arms a timer (trace POW id 21 `Idle timer start…`)
  and reports when the timer expires without new work.
- **Only report "ready to power down" when the idle check actually found a power event.**
  The absence of an event must explicitly mean "do not power off". An undefined or
  ambiguous result here causes a spurious power-off at the first idle of a job.
- If work arrived recently, the power-off request is **cancelled** (trace POW id 18 `Cancel
  power off request int: …, ext: …, pow flags: …`).

### Teardown (before the OFF signal)

In the observed order: dusts and units turned off (trace MAIN id 145 `GPU units deinit`,
MAIN id 144 `GPU deinit`), GPIO disabled (MISC id 4 `GPIO disabled`), write-back of the
MIPS D-cache, MMU cache invalidation, **release/reset of the BIF slot state** (see
[data-masters](data-masters.md)), final cache operations and marking the firmware as no
longer active. Only then the power sync is signalled.

## Warm boot (power on again)

When work arrives with the GPU off, the host powers the domain on and restarts the firmware,
which re-enters through the reset vector (`0xBFC00000`, see
[execution-environment](execution-environment.md)). The firmware re-initialises (the trace
shows the APM latency and core clock again: POW id 23 `Active PM latency set to …ms. Core
clock: … Hz`) and continues processing the KCCB. A healthy warm boot does **not** cause a
page fault.

## Core clock and DVFS

- The host gives the core frequency at boot (`rogue_fwif_sysinit.initial_core_clock_speed`)
  and later with KCCB `ROGUE_FWIF_KCCB_CMD_CORECLKSPEEDCHANGE`
  (`rogue_fwif_runtime_cfg.core_clock_speed`). Trace MAIN id 93 `Core clock set to … Hz`.
- **Requirement:** parameters derived from the clock (timer ticks per ms, APM deadlines and
  watchdogs) must be **recomputed from the real value received**. The firmware must never
  run with a `0` clock: timers and APM degenerate.
- The firmware can notify the host of changes through FWCCB
  `ROGUE_FWIF_FWCCB_CMD_CORE_CLK_RATE_CHANGE`.

## Observed "pow flags" values

The POW trace entries print a *pow flags* word. Values seen (**O**; meaning **D**, the bit
names are not public): `0x10000` (power-off request pending), `0x40000` (forced idle),
`0x80000` (forced request), `0xA0000 = 0x80000 | 0x20000`. The encoding is internal: a
reimplementation may use any encoding, since only the trace prints it. Full sequences in
[observed-sequences](observed-sequences.md).
