<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# E005 — runtime PM cycle and warm boot

**Question.** Does the mainline host's power-off/power-on cycle work with this
firmware, including the restart of the MIPS on a GPU that was fully powered down?

**Set-up.** Default build, `modprobe powervr exp_hw_support=1`. Then wait longer than
the autosuspend delay and start Vulkan work (`vulkaninfo`, dEQP).

**Observation.**
- The firmware trace (debugfs `pvr_fw/trace_0`) shows the full sequence: `POW_19` →
  `POW_59` → `MAIN_145` → `MAIN_144` → `POW_6`.
- `power/runtime_status` goes `active → suspended`, and the APMU GPU domain status bit
  goes to 0.
- When work arrives, the domain powers on and the host restarts the MIPS. Boot
  markers `0xB0071..0xC0DE0006` appear again in scratch `0x1A00`, and the KCCB
  commands queued in the meantime are executed. Return slots show
  `KCCB Slot n: Return value 1`.
- `vulkaninfo` enumerates `deviceName = PowerVR B-Series BXE-2-32 MC1` after several
  such cycles.

**Conclusions.**
- The host needs `sysdata.pow_state == IDLE` before it autosuspends. The firmware must
  report IDLE whenever no DM is busy and no context has pending commands, and ON
  otherwise.
- `OFF` must flush the dynamic TLB and stop the timer tick. Nothing may run between
  the power-sync answer and the domain going down.
- A warm boot can simply be a cold boot. Every structure the firmware needs can be
  found again through sysinit and osinit, so no state has to be preserved.

**Clean-room note.** Own firmware, public host driver.
