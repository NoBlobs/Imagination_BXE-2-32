<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# E010 — a truncated firmware file makes the kernel fall back to the vendor firmware

**Question.** After the power cycle that followed E004, the driver loaded but behaved
differently. The firmware version and the trace were not ours. What happened?

**Observation.**
- `request_firmware()` first tried `/lib/firmware/updates/powervr/rogue_36.29.52.182_v1.fw`,
  which was our file. The hang had happened shortly after the deploy, before the page
  cache reached the SD card, so the file was **truncated**. The container check in the
  host failed (`-22`).
- The kernel then quietly loaded `/lib/firmware/powervr/rogue_36.29.52.182_v1.fw`,
  which is a different (vendor) firmware installed on the system. That firmware
  reports a version other than `v1.0 (build 1 OS)`.

**Actions.**
- The driver was unloaded at once. Nothing from that run (trace, registers) was kept
  or read further. The clean team does not observe the vendor firmware.
- `deploy.sh fw` now runs `sync` and prints the sha256 of the installed file.
- `gpu-probe.sh` unloads `powervr` immediately if the reported version is not
  `v1.0 (build 1 OS)`.

**Conclusion and recommendation.** As long as the vendor file exists in
`/lib/firmware/powervr/`, any problem with our file silently swaps in the vendor
firmware. Moving it out of the search path (outside `/lib/firmware`) removes the risk.
That decision belongs to the board owner.

**Clean-room note.** This is a process safeguard. No information from the vendor
firmware was used.
