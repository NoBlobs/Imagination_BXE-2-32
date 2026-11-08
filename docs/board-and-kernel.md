<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# Board, kernel and test workflow

## Board

| Item | Value |
|---|---|
| Board | Banana Pi BPI-F3, SpacemiT K1 (8× X60 RISC-V), DRAM trained at 1066 MT/s by the FSBL |
| Network | `thalles@192.168.0.106` (passwordless ssh and sudo) |
| Serial | `/dev/ttyUSB0` at 115200. **`/dev/ttyUSB1` is a different board (K3), never use it.** |
| Storage | SD card: Debian 13 with the mainline kernels. eMMC: vendor kernel. |
| Boot | U-Boot → GRUB EFI (`/boot/efi/EFI/BOOT/grub.cfg`). The default is the mainline 7.3 entry. |
| Display | none in mainline: everything is headless (Vulkan off-screen, dEQP) |
| Mesa | `/opt/mesa-pvr` (`powervr_mesa_icd.riscv64.json`), needs `PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1` |
| dEQP | `/home/thalles/cts/build/external/vulkancts/modules/vulkan/deqp-vk` |

## Kernel branch `IMG-BXE-2-32` (`/home/thalles/0-git/NoBlobs/linux`)

Base: `v7.4-rc1` merge. Three commits, about 205 lines:

1. **`pmdomain: spacemit: add K1 APMU power domain driver (GPU domain)`**
   - `drivers/pmdomain/spacemit/k1-apmu-pd.c` is an auxiliary driver
     (`spacemit_ccu.k1-apmu-pd`) created by the APMU clock controller. The CCU common
     code gained a `pd_name` field, which spawns a second auxiliary device.
   - It registers a genpd onecell provider on the APMU syscon node. Domain ids are in
     `include/dt-bindings/power/spacemit,k1-power.h` (VPU 1, GPU 2, LCD 3, ISP 4,
     AUDIO 5, GNSS 6, HDMI 7). Only **GPU** is implemented.
   - GPU domain, with the control register at APMU `+0xD0` and status at APMU `+0xF0`
     bit 0:
     - Power on: set bit 2 (SLEEP1), wait 20 µs; set bit 3 (SLEEP2), wait 20 µs; set
       bit 1 (ISO release); then poll status bit 0.
     - Power off: the same steps in reverse.
   - Mainline had no K1 power-domain driver. The sequence was worked out by watching
     the APMU registers on the vendor kernel while the GPU was used. That observes the
     SoC's power controller, not the GPU firmware.
2. **`riscv: dts: spacemit: k1: add the IMG BXE-2-32 GPU node`**
   ```dts
   gpu: gpu@cac00000 {
       compatible = "spacemit,k1-gpu", "img,img-bxe-2-32", "img,img-rogue";
       reg = <0x0 0xcac00000 0x0 0x80000>;
       interrupts = <75>;
       clocks = <&syscon_apmu CLK_GPU>;
       clock-names = "core";
       resets = <&syscon_apmu RESET_GPU>;
       power-domains = <&syscon_apmu K1_PD_GPU>;
       status = "disabled";
   };
   ```
   The node lives under `multimedia-bus`, whose `dma-ranges` map the GPU's DMA view
   (`0x80000000 → 0x1_00000000`). `k1-bananapi-f3.dts` enables it.
3. **`drm/imagination: add BXE-2-32 (36.29.52.182) as experimental`**
   - adds the BVNC to the GPU table as `PVR_GPU_EXPERIMENTAL`, so it needs
     `exp_hw_support=1`;
   - adds the `MODULE_FIRMWARE` line.

Build with the board's mainline config as the base (`config-7.3.0-mainline`), with
`SPACEMIT_K1_APMU_PD=y` and `DRM_POWERVR=m`:

```bash
make -C ../linux ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- LOCALVERSION= -j$(nproc) Image modules dtbs
```

The kernel release is `7.4.0-rc1-img`. `tools/linux-fw-boot-debug-dump.patch` is not
committed. It makes the driver dump the BOOT_DATA page when the firmware fails to
boot, and was used while bringing up `boot.S`.

## GRUB entry `img-gpu`

`tools/deploy.sh kernel` appends this entry (after backing up `grub.cfg` to
`grub.cfg.bak-img`). It is **not** the default:

```
menuentry "Debian 13 - Linux 7.4.0-rc1-img (GPU clean-room FW, powervr blacklisted)" --id img-gpu
    linux /vmlinuz-7.4.0-rc1-img root=UUID=… … modprobe.blacklist=powervr
    devicetree /spacemit/7.4.0-rc1-img/k1-bananapi-f3.dtb
```

The driver is never loaded at boot. A broken firmware therefore cannot stop the board
from booting, and every test starts with a manual `modprobe`.

To boot it, use the menu index (0-based, currently 3) through the serial console:

```bash
python3 tools/boot-entry.py /dev/ttyUSB0 3 thalles@192.168.0.106 -o UserKnownHostsFile=$KH
```

The script reboots over ssh, waits for the GRUB menu on the serial line, sends the
arrow keys with delays (GRUB drops keys sent too fast), presses Enter and waits for
`login:`.

## Scripts (`tools/`)

| Script | Runs on | Purpose |
|---|---|---|
| `deploy.sh fw` | host | build, copy the `.fw` to `/lib/firmware/updates/powervr/` (searched before `/lib/firmware/powervr/`), `sync`, print sha256 |
| `deploy.sh kernel` | host | build and install the kernel, modules, dtb and initramfs, add the GRUB entry |
| `gpu-test.sh` | board | `modprobe powervr exp_hw_support=1`, dmesg, firmware trace from debugfs |
| `gpu-probe.sh` | board | like `gpu-test.sh`, plus register sampling during the load. **Unloads the driver if a firmware other than this build was loaded.** |
| `gpu-regs.py <s> [step]` | board | samples selected registers through `/dev/mem`, but only while `runtime_status == active`, after forcing `power/control = on`. `EXTRA_REGS="0610 1a30"`, `ONLY_EXTRA=1`. |
| `reg-survey.py read\|probe <start> <end>` | board | black-box register survey: non-zero values, or the writable-bit mask (write, read back, restore) with a deny list. See [E007](logRefs/E007-register-survey.md) and [E008](logRefs/E008-writable-mask-probe.md). |
| `deqp.sh <pattern> [timeout] [lines]` | board | runs dEQP-VK cases on Mesa pvr and prints the results |
| `boot-entry.py` | host | GRUB entry selection through the serial console |
| `mkfw.py`, `gen_sfids.py`, `ld-wrap.sh` | host | build helpers |

SSH: use a separate known-hosts file (`KH=…/known_hosts`, `-o UserKnownHostsFile=$KH`),
because the host key of `.106` differs between the SD and eMMC systems.
`deploy.sh` reads `KH` from the environment.

## Typical cycle

```bash
tools/deploy.sh fw
scp tools/*.sh tools/*.py thalles@192.168.0.106:/tmp/
ssh thalles@192.168.0.106 'sudo modprobe -r powervr; bash /tmp/gpu-test.sh'
ssh thalles@192.168.0.106 'bash /tmp/deqp.sh "dEQP-VK.compute.pipeline.basic.empty_shader" 60'
```

## Safety rules learnt the hard way

- **Never read GPU registers while the power domain is off.** The SoC bus hangs and
  only a power cycle helps ([E004](logRefs/E004-register-read-power-off-hang.md)). The
  tools check `runtime_status` and force `control = on` first.
- After a hang, check that `/lib/firmware/updates/powervr/rogue_36.29.52.182_v1.fw` is
  intact (sha256). A truncated file makes the kernel silently fall back to
  `/lib/firmware/powervr/`, which holds the vendor firmware the clean team must not use
  ([E010](logRefs/E010-firmware-fallback.md)).
- Experiments that make the GPU fetch from new addresses can hang the SoC
  ([E009](logRefs/E009-pds-exec-base-0x0610.md)). Run them only when a power cycle is
  possible.
