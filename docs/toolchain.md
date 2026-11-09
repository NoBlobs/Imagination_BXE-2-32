<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# Build and toolchain

## Requirements

- clang and ld.lld with the Mips target (tested with LLVM on Arch Linux), plus
  llvm-objdump
- python3 (`tools/mkfw.py`, `tools/gen_sfids.py`)
- No libc or libgcc: the firmware is freestanding. `memset`, `memcpy`, `__udivdi3`
  and `__umoddi3` are in `util.c`.

## Commands

```bash
make -C src                                  # build/rogue_36.29.52.182_v1.fw
make -C src -B EXP="-DEXP_NO_EVENT_POLL"    # experiment knobs (see below)
make -C src sfid SFH=<path>/pvr_rogue_fwif_sf.h   # regenerate include/sfid.h
make -C src clean
```

Outputs in `build/`, which git ignores:

| File | Content |
|---|---|
| `obj/*.o` | objects |
| `fw.elf` | linked firmware (4 `PT_LOAD` segments) |
| `fw.dis` | microMIPS disassembly of `fw.elf` (`llvm-objdump --mattr=+micromips`) |
| `rogue_36.29.52.182_v1.fw` | the container the kernel loads |

The build is deterministic: the same sources give the same `.fw` byte for byte. The
default build (no `EXP`) of the current sources has sha256 `2840e82c…`, and that is the
build installed on the board.

## Compiler flags

```
--target=mipsel-unknown-elf -march=mips32r2 -mmicromips -mabi=32 -msoft-float
-std=c23 -O2 -ffreestanding -fno-builtin -fno-pic -mno-abicalls -G0 -mlong-calls
-fno-common -fno-stack-protector -fno-jump-tables -Wall -Wextra -Werror
```

| Flag | Why |
|---|---|
| `-mmicromips` | The MIPS core runs microMIPS. `Config3.ISAOnExc` is set so that exceptions enter in microMIPS mode. Function pointers carry the ISA bit (`\| 1`). |
| `-mlong-calls` | Code sits in three distant regions: `0x9FC02000`, `0xBFC00000` and `0xC0000000`. Calls between them must go through a register (`jalr`). |
| `-G0`, `-fno-pic -mno-abicalls` | No `$gp`-relative small data and no PIC. The image is linked at fixed addresses. |
| `-fno-jump-tables` | A `switch` is never turned into a table of absolute addresses in rodata. |
| `-msoft-float` | The core has no FPU. |

## LLVM quirks and their workarounds

### `MFC0`/`MTC0` in microMIPS R2

The LLVM assembler rejects the microMIPS `mfc0`/`mtc0` mnemonics unless the target
is R6. The encoding is the same in R2 and R6, so `include/cp0asm.h` emits the 32-bit
instruction by hand:

```
word = rt << 21 | rs << 16 | sel << 11 | op << 6 | 0x3C      op: MFC0 = 0x03, MTC0 = 0x0B
emitted as two .hword, high half first (microMIPS 32-bit instruction order)
```

`include/mips.h` exposes the same encoding to C as `mfc0(reg, sel)` and
`mtc0(reg, sel, v)`, going through `$2`. The encoding was checked against what LLVM
produces for `-mips32r6 -mmicromips`.

### lld and `R_MICROMIPS_26_S1`

ld.lld range-checks microMIPS `J`/`JAL` targets as if they were PC-relative, so any
code above `0x80000000` reports "relocation out of range". The value lld actually
writes is the correct region-relative one, verified in `fw.dis`. The Makefile
therefore links with `--noinhibit-exec`, and `tools/ld-wrap.sh` drops only those
warning lines while every other diagnostic is still printed.

## Experiment knobs

Compile-time knobs (`make -B EXP="-DNAME[=value]"`):

| Knob | Effect |
|---|---|
| `EXP_NO_EVENT_POLL` | the timer no longer polls `EVENT_STATUS`, so completions come from the MTS tasks only (E018) |
| `EXP_SLC_FLUSH_PER_KICK` | SLC flush and invalidate before every TQ kick (E020) |
| `EXP_TIF=<n>` | force the TQ's tiles-in-flight value (E022) |
| `EXP_SLC_ONLY_FLUSH` | completion flush = SLC only, instead of the per-DM sequence (E023 control) |
| `EXP_FRAG_CTRL=<v>` | value for `MULTICORE_FRAGMENT_CTRL_COMMON` in the TQ kick (default 1, E023) |

The exec-base and event-enable knobs of E009–E012 were removed once the spec gave the
registers (`gpu_init.c`).
Rebuild with `-B` when you change `EXP`, because make does not track it. **Check that the
build succeeded and that the `.fw` hash changed** before deploying: a failed build
leaves the previous `.fw` in place (E023).

## Trace IDs

`include/sfid.h` is generated from the public `pvr_rogue_fwif_sf.h`. Each entry
becomes `#define SFID_<GROUP>_<id> 0x7nngiiii /* nparams: "format" */`. Using the same
IDs as the public table is what lets the mainline host decode this firmware's trace
without changes.
