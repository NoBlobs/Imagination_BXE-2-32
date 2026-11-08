<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# E004 — reading RGX registers with the GPU power domain off hangs the SoC

**Question.** While `gpu-regs.py` sampled registers in a loop, the board stopped
answering ssh, ping and the serial console. What happened?

**Observation.**
- The sampler kept reading after the driver had runtime-suspended the GPU: the
  firmware went through `POW OFF`, and the host turned off the APMU GPU domain.
- The K1 bus does not return an error for an access to a powered-off domain. The CPU
  waits forever.
- Only a power cycle recovered the board.

**Change.**
- `gpu-regs.py` and `reg-survey.py` set `power/control = on` first, which keeps the
  device resumed.
- They read only while `power/runtime_status == active`.

**Conclusion.** This is a property of the SoC, not of the firmware, but it constrains
every debugging tool. A register read from user space must always be guarded by the
runtime-PM state.

**Clean-room note.** Hardware behaviour only.
