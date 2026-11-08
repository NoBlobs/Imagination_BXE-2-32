<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# BXE-2-32 firmware: clean-team implementation

This is a GPLv3 firmware for the MIPS microcontroller of the PowerVR Rogue BXE-2-32
(BVNC `36.29.52.182`) in the SpacemiT K1. It is written only from:

- the clean specification in `docsClean/`;
- the public upstream `drm/imagination` headers and driver;
- public Mesa;
- public MIPS architecture documents;
- black-box experiments on the board with this firmware, recorded in [logRefs/](logRefs/).

It runs under the unmodified mainline `powervr` driver, apart from the experimental
BVNC entry, and the Mesa `pvr` Vulkan driver.

## Documents

| Document | Content |
|---|---|
| [status.md](status.md) | what works, what does not, the next steps |
| [architecture.md](architecture.md) | source files, execution model (interrupt-driven), state, module map |
| [boot-and-memory.md](boot-and-memory.md) | MIPS TLB and wrapper remap, memory map, layout table, boot sequence, TLB refill, exceptions, debug scratch registers |
| [host-interface.md](host-interface.md) | `.fw` container and device info, start-up handshake, KCCB/FWCCB, host notification, power and warm boot, trace format |
| [scheduler-and-kicks.md](scheduler-and-kicks.md) | data masters, context table, client CCB walk and UFOs, completion, cleanup, BIF slots, compute kick |
| [registers.md](registers.md) | every register used, with provenance; boot values; writable-bit map; exec-base candidates |
| [toolchain.md](toolchain.md) | build, flags, LLVM microMIPS workarounds, experiment knobs |
| [board-and-kernel.md](board-and-kernel.md) | board facts, kernel branch `IMG-BXE-2-32`, GRUB entry, scripts, safety rules |
| [questions.md](questions.md) | questions for the spec maintainer (dirty team), with what experiments answered |
| [logRefs/](logRefs/README.md) | experiment log E001…: question, set-up, raw observation, conclusion |

## Repository layout

```
docsClean/   clean specification (read-only for the clean team)
src/         firmware source: C23 + microMIPS assembly, Makefile, linker script
tools/       container packer, trace-ID generator, deploy/test/probe scripts
docs/        this documentation; docs/logRefs/ = experiments; docs/logRefs/data/ = raw dumps
build/       build output (git-ignored)
```

## Quick start

```bash
make -C src                         # -> build/rogue_36.29.52.182_v1.fw
KH=<known_hosts> tools/deploy.sh fw # -> board /lib/firmware/updates/powervr/
# board, booted with the "img-gpu" GRUB entry (powervr blacklisted):
sudo modprobe powervr exp_hw_support=1
```

Expected kernel log:

```
powervr cac00000.gpu: [drm] FW version v1.0 (build 1 OS)
[drm] Initialized powervr 1.0.0 for cac00000.gpu on minor 0
```

## Clean-room rules followed

- Never read, run or disassemble the vendor firmware (`rogue_*.fw`, `rgx.fw*`), its
  dumps or captures.
- Never read the vendor DDK or kernel driver sources, or proprietary headers.
- Never use dirty-team material outside `docsClean/`.
- Never modify `docsClean/`. Gaps become questions in [questions.md](questions.md),
  or experiments on our own firmware in [logRefs/](logRefs/).
