<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# E009 — `sysinit.pds_exec_base` → `0x0610`

**Question.** Is `0x0610` (found in E008) the PDS execution base?

**Set-up.**
- Build `make -B -C src EXP="-DEXP_PDS_REG=0x610"`, sha256 `95868c18…`.
  `gpu_units_init()` writes `cr_write64(0x0610, sysinit.pds_exec_base)` at every
  (warm) boot and copies the read-back of the high word to scratch `0x1A30`.
- Test case: `dEQP-VK.compute.pipeline.basic.empty_shader`.

## Run 1: driver reloaded after the E007/E008 probes

The sequence was:
1. `modprobe -r powervr`, then load the new build (`[drm] Initialized … on minor 1`).
2. Start the dEQP case.
3. **The SoC hung**: no ping, no serial console. A power cycle was needed.

Nothing was captured, because the sampler only wrote to the ssh session.

## Run 2: fresh boot, serial capture

- The board was power-cycled and booted into `img-gpu` with `tools/boot-entry.py`.
- `gpu-regs.py` ran with `KMSG=1`, which copies every sample to `/dev/kmsg`. The
  serial console carries it to the host (`data/serial-e009-e011.txt`), so the data
  survives a hang.

Step 1, load and power cycle without work:

```
0610/0614 = 0x00000000 / 0x000000DA     -> 0x0610 holds 0xDA_0000_0000 = ROGUE_PDSCODEDATA_HEAP_BASE
1a30      = 0x000000DA                  read-back from the firmware
runtime suspend -> vulkaninfo: "PowerVR B-Series BXE-2-32 MC1", runtime active again
```

The value is accepted and the register keeps the whole value. A power cycle and warm
boot still work.

Step 2, the compute case. Sampled every 50 ms for 40 s:

```
0130 (EVENT_STATUS)            = 0x00000000   (E006: 0x200 MMU fault)
12b0 (BIF_FAULT_BANK0_MMU)     = 0x00000000   (E006: 0x1041)
1a78 (busy DMs)                = 0x00000010   CDM busy for the whole run
03c0 (unknown, idle-like)      = 0x7f         (after the E006 fault: 0x5e)
firmware trace: "Kick Compute: FWCtx 0xc0056000 @ 0 …", then nothing from the DM
dEQP: no result before the 40 s timeout; the host cleaned the context afterwards
```

The full register dump after the run is in `data/survey-after-e009.txt`.

## Conclusions

- **With `0x0610` programmed, the fault at `0x26C0` is gone.** Without it, the same
  kick faults on every run (E006). `0x0610` is therefore, with high confidence,
  **`ROGUE_CR_PDS_EXEC_BASE`**: a 40-bit VA, 1 MiB granule, value `pds_exec_base`.
- The task still does not complete. No `COMPUTE_FINISHED` was seen, and the CDM stays
  busy in the firmware tables. E011 shows that the CDM then goes on to fault at a
  second low address, `0x7B80`, which again looks like a missing base (USC code).
- Run 1 hung the SoC, but run 2 of the same build did not. The difference is that run 1
  came after a driver reload that followed a faulted, never-completed CDM task, and
  after the E008 register probe. E011 hung the same way after a reload. **Reloading
  `powervr` while a CDM task is stuck is the main suspect for the SoC hangs.** Until
  HWR exists, compute experiments should start from a fresh boot.

**Clean-room note.** Own firmware. Register and value candidates come from E008 and the
public heap configuration.
