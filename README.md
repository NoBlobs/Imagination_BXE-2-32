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
| Compute jobs | **work**: `dEQP-VK.compute.pipeline.basic` passes 67/82 (6 failures are image cases, 9 not supported) |
| Hardware recovery (HWR) | works: a faulting or hung job is reset and discarded in ~0.3 ms |
| Transfer (TQ_3D) | the 3D pipe starts, but the output still faults, so image cases fail |
| Geometry and fragment kicks | not implemented |

See [docs/status.md](docs/status.md) for details and the next steps.

## Clean-room process

The work is split between two teams:

- The **dirty team** analysed the original firmware and wrote a functional
  specification in [docsClean/](docsClean/README.md). It contains no code.
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
docsClean/   functional specification from the dirty team (read-only)
src/         firmware: C23 + microMIPS assembly, Makefile, linker script
tools/       .fw packer, trace-ID generator, deploy / test / register-probe scripts
docs/        implementation documentation
docs/logRefs/  experiment log (E001…) with raw data in docs/logRefs/data/
build/       build output (git-ignored)
```

## Building

Requirements: clang and ld.lld with the Mips target, llvm-objdump, python3.

```bash
make -C src
```

The output is `build/rogue_36.29.52.182_v1.fw`, the container that the mainline
driver loads. Build flags, the LLVM microMIPS workarounds and the experiment knobs are
described in [docs/toolchain.md](docs/toolchain.md).

## Running on the K1

1. Build a kernel from the `IMG-BXE-2-32` branch of the Linux fork. It adds the K1 GPU
   power domain, the GPU device-tree node and the experimental BVNC entry; see
   [docs/board-and-kernel.md](docs/board-and-kernel.md).
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

| Document | Content |
|---|---|
| [docs/README.md](docs/README.md) | documentation index |
| [docs/architecture.md](docs/architecture.md) | source files, interrupt-driven execution model, state |
| [docs/boot-and-memory.md](docs/boot-and-memory.md) | memory map, MIPS TLB and wrapper remap, boot sequence |
| [docs/host-interface.md](docs/host-interface.md) | `.fw` container, host handshake, command rings, power, trace |
| [docs/scheduler-and-kicks.md](docs/scheduler-and-kicks.md) | client CCB scheduling, data masters, compute kick |
| [docs/registers.md](docs/registers.md) | registers used and their provenance, register survey |
| [docs/board-and-kernel.md](docs/board-and-kernel.md) | board, kernel branch, GRUB entry, scripts, safety rules |
| [docs/logRefs/](docs/logRefs/README.md) | experiments and findings |

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