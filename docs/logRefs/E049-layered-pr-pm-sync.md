<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# E049 — Layered partial renders and the cross-context PM sync: same results as the manufacturer firmware

**Question.** `docsClean` `1524931` answers Q39 (layered `FRAG_PR`) and Q38 (`stress`
lockups). Once both are implemented, does our firmware match the new baseline
`mainline-mesa-pvr-bxe-2-32-manufacturer-fw-1.1.txt` (vkfwtest `f2285b5`)? That baseline is
the manufacturer firmware with our Mesa and kernel fixes (E048), and it reports 48 pass and 2
skip.

**Board.**
- BPI-F3, Arch Linux RISC-V, kernel `7.4.0-rc1-img` with `powervr` 1.1.0 (E048).
- Mesa `pvr-bxe-2-32` (`~/mesa-dev`, RTA on by itself).
- Our firmware `e3560cc0…`.

## What the answers say

- **Q39:**
  - `BCAST[0x03D8]` is the only per-layer register; the background task gets no layer of its
    own.
  - `FRAG_PR` runs the same layer loop as a render. Each `FRAG_PR` kick increments
    `rta_num_partial_renders[layer]`.
  - Later kicks pick, per layer, `has_pr = (flags & 8) && npr[layer] > 0`. That chooses
    `pds_pr_bgnd` with a forced Z/S load, or the normal `pds_bgnd`.
  - The manufacturer firmware with our Mesa and kernel passes `bigpb-layers` 4/4.
- **Q38:**
  - With the fixed Mesa, the manufacturer firmware passes `stress` 10/10.
  - When TA and 3D of different contexts overlap, the PM free lists must be synchronised:
    - pause the other half (`CR[0x02A0]` bit 0 TA alloc / bit 1 3D dealloc, ack in
      `0x02A8`);
    - store a list the other context holds before loading it;
    - do not store from this context a list the other one owns;
    - write `CR[0x02B0] = (local differs << 1) | (global differs)`;
    - then unpause.
  - The earlier manufacturer-firmware failures came from the unfixed Mesa.

## Firmware changes

- **Partial-render rules (`partial-render-spm.md`, Q30 items 1–3, never implemented before),
  in `src/frag.c`:**
  - `CR[0x01C0]` is 1 on `FRAG_PR` and 0 on a render.
  - The ZS bases come from the command on a partial render too (they were written as 0).
  - Forced Z/S store on every `FRAG_PR`; `0x06D0` is `0x1F`, `0x3F` with Z/S, or `0x7F` with
    flag `0x8000`.
  - With `has_pr`: forced Z/S load, `pds_pr_bgnd`, and `isp_bgobjvals |= 0x200`.
  - The `ISP_CTL` bit-17 rules.
  - `hwrtdata.flags |= 8` after a partial render. Our geometry clears it, and the per-layer
    counters, at the next first kick.
- **Layered `FRAG_PR` (Q39):**
  - `rta_setup()` and the loop at `PIXELBE_END_RENDER` run for partial renders too.
  - `frag_layer_state()` writes ZLS control, background, `ISP_CTL` and `0x06D0` per layer, and
    counts the layer's partial renders.
  - On OOM before the partial render, `geom_rta_oom()` (`src/geom.c`):
    - reads `CR[0x0D20]`;
    - flushes the RTC (our addition, so that the bitmap is in memory);
    - zeroes `rtc_dev_addr + 0x000..0x1FF`;
    - scans the bitmap into `valid_render_targets[]`.
- **PM sync across contexts (Q38):** `pm_sync_load_freelists()` (`src/pm.c`) replaces the
  free-list loads of the geometry and fragment kicks:
  - store or pause as the answer describes;
  - write `CONTEXT_PB_BASE` from a comparison with the other context's lists (it was always 3
    for the TA and 0 for the 3D);
  - the same-HWRTData handoff stops tracking the HWRTData in PM context 0, but keeps its lists
    as the registers' contents.

The build has no warnings; `fw-preflight` and `check-trace-args` pass.

## Results

| Run | Result |
|---|---|
| `vkfwtest -k all` | **48 pass / 0 fail / 2 skip** (`local-read`, `sparse`: not in the driver) |
| `bigpb`, `bigpb-layers` ×2, `rta`, `zs-load`, `input-attach`, `multiframe`, `pre-ta`, `msaa` | 9/9 pass, before the PM sync |
| `stress` | **8/8 pass**, no context reset in dmesg (it failed about half the runs before) |
| vs the new manufacturer baseline | **50 same, 0 better, 0 worse, 0 not in it**, exit code 0 |

There is no kernel or firmware fault in any run.

**Conclusion.** On the current vkfwtest battery, with the fixed Mesa and kernel, our firmware
gives exactly the manufacturer firmware's results: every test passes except the two the driver
does not support.
