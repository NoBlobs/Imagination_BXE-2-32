<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# E046 — GPU clock: 307.2 MHz, 819.2 MHz and 1 GHz

**Question.** The manufacturer-firmware results in `docsClean` `50055ae` were taken with the
GPU at 819.2 MHz. Ours run at 307.2 MHz. Does the clock change any result, and how far can it
go?

**Board.**
- BPI-F3 16 GB, Arch Linux RISC-V, kernel `7.4.0-rc1-img`.
- Firmware `f45474ed…` (E045).
- Mesa `5d2dc45` with RTA off (`~/mesa-rta-off`), vkfwtest after E045.

## How the clock is set

- The mainline DT gives the GPU no rate, so it stays at the boot value, 307.2 MHz.
- `gpu_clk` (`drivers/clk/spacemit/ccu-k1.c`) is a mux, a 3-bit divider and a gate. Its
  parents are:

  | Parent | Rate |
  |---|---|
  | `pll1_d6` | 409.6 MHz |
  | `pll1_d5` | 491.52 MHz |
  | `pll3_d6` | 533.3 MHz |
  | `pll2_d5` | 600 MHz |
  | `pll1_d4` | 614.4 MHz |
  | `pll2_d4` | 750 MHz |
  | `pll1_d3` | 819.2 MHz |
  | `pll2_d3` | 1000 MHz |

  1 GHz is the highest rate available without reprogramming a PLL. We did not go above it:
  that would mean a PLL rate the clock driver does not offer, with no voltage control.
- **DT variants:** `k1-bananapi-f3.dts` plus, on `&gpu`, `assigned-clocks = <&syscon_apmu
  CLK_GPU>` and `assigned-clock-rates`. They are built outside the kernel tree, and the
  decompiled DTB differs from the installed one only in these two properties.
- **Where they live:** next to the original on the SD card's `bootfs`
  (`spacemit/7.4.0-rc1-img/k1-bananapi-f3-gpu{819200000,1000000000}.dtb`). GRUB has two
  entries for them: `arch-riscv-74-gpu819`, `arch-riscv-74-gpu1000`.
- **Applied at probe:** the rate is applied when `powervr` probes. The driver is blacklisted at
  boot and loaded by hand, so `clk_summary` shows the new rate only after `modprobe`.
  - At 819.2 MHz the parent is `pll1_d3_819p2`; at 1 GHz it is `pll2_d3`.
  - The firmware log reports `Core clock: <rate> Hz`.

## Results

Every test runs on its own (`~/clk-battery.sh`, no `hwr-*`). The raw data is in
[data/E046/](data/E046/).

| Clock | Pass | Fail | Skip | Faults in dmesg |
|---|---|---|---|---|
| 307.2 MHz | 18 | 22 | 2 | none |
| 819.2 MHz | 18 | 22 | 2 | none |
| 1 GHz | 16 | 24 | 2 | none |

- **307.2 MHz and 819.2 MHz** give the same result on every test.
- **1 GHz** adds two failures, and both are single-bit errors:
  - `xfer`: 1 of 65536 pixels wrong;
  - `image-copy`: `0x624fd300` instead of `0x624fd308`.
- **Repeated 10 times** (`~/clk-repeat.sh`):

  | Test | 819.2 MHz | 1 GHz |
  |---|---|---|
  | `xfer` | 10 / 10 | 9 / 10 |
  | `image-copy` | 10 / 10 | 9 / 10 |
  | `rta` | 10 / 10 | 10 / 10 |
  | `queries` | 10 / 10 | 10 / 10 |

## Conclusions

- **The clock does not explain any difference from the manufacturer firmware.** Raising it to
  the reference's 819.2 MHz changes nothing, and `indirect` and `vertex-ext` still fail. Their
  cause is elsewhere.
- **819.2 MHz is stable** in these runs. Future comparisons with `docsClean` results can use it.
- **1 GHz corrupts data** now and then, with no fault and no HWR. It is not usable.

## State after the test

- The GRUB default is back to `arch-riscv-74` (no clock in the DT, so 307.2 MHz).
- The two test entries and DTBs are still there for later use.
- The original `grub.cfg` is backed up as `EFI/BOOT/grub.cfg.bak-gpuclk` on the SD card's ESP.
