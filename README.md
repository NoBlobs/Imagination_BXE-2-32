<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# Imagination BXE-2-32

This is free firmware (GPLv3, GNU C23) for the MIPS microcontroller of the PowerVR
Rogue **BXE-2-32** GPU (BVNC `36.29.52.182`) in the **SpacemiT K1** SoC. It is a 
clean-room implementation and runs under the unmodified mainline Linux `powervr` 
driver and the Mesa `pvr` Vulkan driver. The only kernel change needed is to accept
this BVNC as experimental.

## Status

| Area | State |
|---|---|
| Boot, `firmware_started` handshake | works (`[drm] Initialized powervr 1.0.0`) |
| Firmware trace decoded by the host | works |
| Kernel CCB commands (MMUCACHE, CLEANUP, POW, …) | works |
| Runtime PM: idle → power off → warm boot | works |
| Vulkan device enumeration (`vulkaninfo`) | works: `PowerVR B-Series BXE-2-32 MC1` |
| Client CCB scheduling, fences and updates | implemented |
| GPU bring-up (exec bases, unit init, dust power) | works |
| Compute jobs | **work**: every supported case of `compute.pipeline.basic` (73), `indirect_dispatch` (24) and `builtin_var` (11) passes |
| Hardware recovery (HWR) | works: a faulting or hung job is reset and discarded in ~0.3 ms, logged in the HWR buffer and reported to the host (context-reset notification) |
| Statistics | GPU utilisation counters and time correlation published for the host |
| Transfer (TQ_3D) | **works**: `api.fill_and_update_buffer` 72/72 and `api.copy_and_blit.core.buffer_to_buffer` 8/8 of the supported cases |
| Geometry and fragment kicks | not implemented |

See [docs/firmware/status-and-experiments.rst](docs/firmware/status-and-experiments.rst) for details and the next steps.

## Clean-room process

The work is split between two teams:

- The **dirty team** analysed the original firmware and wrote a functional
  specification in [docsClean/](docsClean/README.md). It contains no code. It is
  published as its own public repository,
  [NoBlobs-docsClean/Imagination_BXE-2-32](https://github.com/NoBlobs-docsClean/Imagination_BXE-2-32),
  included here as a git submodule.
- The **clean team** wrote everything else in this repository. Its only sources are:
  - `docsClean/`;
  - the public upstream Linux `drm/imagination` headers and driver;
  - public Mesa;
  - public MIPS architecture documents;
  - experiments on the board with **this** firmware, recorded in
    [docs/logRefs/](docs/logRefs/README.md).

The clean team never read, ran or disassembled the vendor firmware or the vendor
driver sources. Gaps in the specification are sent back as questions in
[docs/questions.md](docs/questions.md).

## Repository layout

```
docsClean/   functional specification from the dirty team (git submodule, read-only)
src/         firmware: C23 + microMIPS assembly, Makefile, linker script
tools/       .fw packer, trace-ID generator, deploy / test / register-probe scripts
docs/        implementation documentation
docs/logRefs/  experiment log (E001…) with raw data in docs/logRefs/data/
build/       build output (git-ignored)
```

## Getting the sources

`docsClean/` is a submodule, so clone with it:

```bash
git clone --recurse-submodules <this repository>
```

In an existing clone, `git submodule update --init` fetches it. To move to a newer
version of the specification, run `git submodule update --remote docsClean`, then
commit the new pointer.

## Building

Requirements: GCC and binutils for `mipsel-elf` (default: `/opt/rgxcross`), python3.

```bash
make -C src
```

The output is `build/rogue_36.29.52.182_v1.fw`, the container that the mainline
driver loads. Build flags, the toolchain and the experiment knobs are
described in [docs/firmware/build-and-toolchain.rst](docs/firmware/build-and-toolchain.rst).

## Running on the K1

1. Build a kernel from the `IMG-BXE-2-32` branch of the Linux fork. It adds the K1 GPU
   power domain, the GPU device-tree node and the experimental BVNC entry; see
   [docs/linux-driver/kernel-integration.rst](docs/linux-driver/kernel-integration.rst).
2. Install the firmware where the kernel finds it before the stock location:

   ```bash
   sudo install -D -m 0644 build/rogue_36.29.52.182_v1.fw \
       /lib/firmware/updates/powervr/rogue_36.29.52.182_v1.fw
   ```

3. Load the driver:

   ```bash
   sudo modprobe powervr exp_hw_support=1
   ```

   The kernel log should show `FW version v1.0 (build 1 OS)` and
   `Initialized powervr 1.0.0 for cac00000.gpu`.

`tools/deploy.sh` and the other scripts automate these steps for the development
board.

## Documentation

The documentation in `docs/` is written in reStructuredText and built with Sphinx (`make -C docs`, see [docs/README.md](docs/README.md)):

| Section | License | Content |
|---|---|---|
| [docs/index.rst](docs/index.rst) | `GPL-3.0-or-later` | documentation index, status overview and provenance labels |
| [docs/hardware/](docs/hardware/index.rst) | `CC-BY-SA-4.0` | BXE-2-32 architecture, MIPS memory/remap, RGX registers, GPU bring-up/PM/HWR, reference board and safety rules |
| [docs/linux-driver/](docs/linux-driver/index.rst) | `GPL-2.0-only` | mainline `powervr` driver role, kernel branch `IMG-BXE-2-32`, `.fw` container/ABI, host↔firmware protocol, test workflow |
| [docs/firmware/](docs/firmware/index.rst) | `GPL-3.0-or-later` | source architecture, GCC toolchain and knobs, boot/runtime/TLB, scheduler and kicks, power/HWR/stats, status and experiments |
| [docs/logRefs/](docs/logRefs/README.md) | `GPL-3.0-or-later` | experiments (`E001`–`E040`) and raw findings |

## License

Copyright (C) 2026 Thalles Campagnani

This project is free software: you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version. It is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See [LICENSE](LICENSE) for details.

This project is not affiliated with or endorsed by Imagination, SpacemiT, Banana Pi
or Samsung; product names belong to their owners.