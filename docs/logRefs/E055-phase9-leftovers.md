<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# E055 — Phase 9 leftovers: Q25 transfers, the `0x0C68` wait, the GPU clock

**Question.** Close the three small items of `steps.md` phase 9 on the current stack
(Arch board, kernel `7.4.0-rc1-img`, Mesa `~/mesa-dev`, firmware `dc69ef27…`).

## 1. Q25 transfer cases

These were `buffer_to_buffer_with_offset`, the NPOT copies and the mirrored blits. dEQP is
not installed on the Arch board: it was on the Debian SD system with the old
`/opt/mesa-pvr`. The `vkfwtest` transfer group covers the same cases, all pixel-checked:

| Test | Covers | Result |
|---|---|---|
| `xfer` | upload, **offset copy**, **mirrored blit**, MSAA resolve on **NPOT** and POT images | pass |
| `buffers` | `vkCmdCopyBuffer` with several regions, fill, update | pass |
| `image-copy`, `blit-mips`, `ds-copy` | 3D/layered copies, scaled blits, depth copies | pass |
| `texture` | NPOT sampling | pass |

`-k all` gives 48 pass / 0 fail / 2 skip, the manufacturer firmware's result on every test
(E049). The Q25 answers are implemented: GPU-init step 9 in E042, and the transfer kick is
the whole Q13 table. **Closed.**

## 2. The `0x0C68` wait

The spec makes the poll mandatory (`kick-geometry-ta.md` E): write 1, then wait until bit 0
reads 0. E038 saw it time out three times in a long run, with an older firmware. Measured
now, with the trace on during `-k all` and 20 × `stress`, sampling the trace every 0.5 s:

- 1576 first kicks were seen in the samples;
- `DBG_2 0xE028E000` (poll timeout) appeared **0 times**.

**Decision:** keep the wait as the spec says, bounded at 10 ms. A timeout only leaves a
trace line. No code change.

## 3. The GPU clock

Read from `/sys/kernel/debug/clk` on the board:

- `gpu_clk` (consumer `cac00000.gpu`, `core`) runs at **307.2 MHz**, with parent
  `pll1_d4_614p4` divided by 2;
- possible parents: `pll1_d4_614p4`, `pll1_d5_491p52`, `pll1_d3_819p2`, `pll1_d6_409p6`,
  `pll3_d6`, `pll2_d3`, `pll2_d4`, `pll2_d5`;
- the GPU node in the device tree has no OPP table and no `assigned-clock-rates`, so the
  rate is the clock driver's default.

The firmware takes the rate from the host, not from the clock tree. It uses
`sysinit.initial_core_clock_speed` at boot and `runtime_cfg.core_clock_speed` on
`CORECLKSPEEDCHANGE` (`power-management.md`, "Core clock and DVFS"). Its timers and
watchdog follow whatever rate is set. So the firmware has nothing to change.

Running faster is host-side: an OPP table or `assigned-clock-rates` in the device tree
(`pll1_d3_819p2` would match the oracle's 819.2 MHz), with the right regulator voltage.
1 GHz shows errors at stock voltage. That is kernel/DT work, outside the firmware.

## Conclusion

Phase 9 has no open firmware item:
- Q25 is closed by the transfer tests;
- the `0x0C68` wait stays as specified and no longer times out;
- the clock is a host/device-tree choice that the firmware already follows.
