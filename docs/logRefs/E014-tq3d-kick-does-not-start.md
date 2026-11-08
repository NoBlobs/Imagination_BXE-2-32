<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# E014 — TQ_3D (transfer through the 3D pipe): programmed, but the 3D pipe never starts

**Question.** Compute tests that use images fail because every `TQ_3D` command
(`0x2ABC80CA`) was retired without running. Does the transfer kick from
`kick-compute-and-3d.md` start the pixel pipeline?

## Mapping `cmd+N` to `rogue_fwif_cmd_transfer`

The spec's `cmd+N` offsets are relative to the **cCCB command header** (0x28 bytes), so
`cmd+0x30` = `regs+0`. Every offset in the spec then lands on a field boundary of the
public `rogue_fwif_transfer_regs` (192-byte command, size confirmed by the cCCB
`CmdSize=192`):

| Register | Spec | Field |
|---|---|---|
| `0x0F88` | `cmd+0x30` | `isp_bgobjvals` |
| `0x4070` | "control value" | `usc_pixel_output_ctrl` (`cmd+0x34`) |
| `0x4078/80/88/90` | `cmd+0x38…+0x44` | `usc_clear_register0..3` |
| `0x0F18` / `0x0F10` | `cmd+0x48` / `+0x4C` | `isp_mtile_size` / `isp_render_origin` |
| `0x0F38` (`ISP_CTL`) | | `isp_ctl` (`cmd+0x50`) |
| `0x0F30` | `cmd+0x58` | `isp_aa` |
| `0x0628/0618/0620` | `cmd+0x5C/60/64` | `event_pixel_pds_info/code/data` |
| `0x0F28` | `cmd+0x6C` | `isp_rgn` |
| `0x3E40` | `cmd+0x70` | `frag_screen` |
| `0x06A0…0x06BC` | `cmd+0x78…` | `pds_bgnd0_base`, `pds_bgnd1_base`, `pds_bgnd3_sizeinfo` |
| `0x0F20/24` | `cmd+0x90/94` | `isp_mtile_base` |
| `0x1510…` | `cmd+0x98…` | `pbe_wordx_mrty[9]` |

`0x06A0..0x06B8` are four registers but the command has only three PDS background
fields. The E008 writable masks match the field layouts that public Mesa
(`rogue_cr.xml`) gives for `PDS_BGRND0_BASE`, `PDS_BGRND1_BASE`, `PDS_BGRND2_BASE` and
`PDS_BGRND3_SIZEINFO`:

| Offset | E008 mask | Mesa layout |
|---|---|---|
| `0x06A0` | `FFFFFFF0_FFFFFFF0` | BGRND0: two 28-bit addresses, bits 63:36 and 31:4 |
| `0x06A8` | `FFFFFFF0_FFFFFFF0` | BGRND1: same |
| `0x06B0` | `00000000_FFFFFFF0` | BGRND2: one address, bits 31:4 |
| `0x06B8` | `FF803FFF_FFFFFFF0` | BGRND3_SIZEINFO: bits 63:55, 45:32, 31:… |

So the firmware writes `bgnd0 → 0x06A0`, `bgnd1 → 0x06A8`, `0 → 0x06B0`, and
`bgnd3_sizeinfo → 0x06B8`. This mapping is itself a finding.

## Run

- Build `641eff61…` with the kick enabled.
- Fresh boot.
- Test case: `dEQP-VK.compute.pipeline.basic.copy_ssbo_to_image_small`.

```
[3140661] : Kick Compute: FWCtx 0xc0056000 @ 0 …
[3142817] : Compute finished
[3144760] : Kick 3D TQ: FWCtx 0xc004a000 @ 0, CSW resume:0. (PID:530, …)
gpuregs 2.64s  0130=00000400 12b0=0 1a78=00000008 0f08=00000013 6028=00000000 6030=00083826
```

- `ISP_RENDER` reads `0x13`. The host's `isp_render` is `0x3` (mode `FAST_SCALE`), and the
  start RMW adds bit 4. That matches the spec's readiness value `(ISP_RENDER & 3) == 3`.
- **`PERF_3D_CYCLE` (`0x6028`) stays 0**, while the compute cycle counter moved for the
  earlier compute job. The 3D pipe never started.
- There is no MMU fault and no `PIXELBE_END_RENDER`. DM 3 stays busy and the host times
  out.

**Conclusion.** The register programming is complete as far as the spec describes, but
something needed to start the pixel pipe is missing. Candidates:
- the PM/HWRTData load (a TQ has none);
- 3D-specific unit set-up;
- the exact trigger sequence: whether `isp_render` is written first, and whether the
  write goes to the core register or the broadcast bank;
- `MULTICORE_FRAGMENT_CTRL_COMMON`: the value used was `0x101`, i.e. execute count 1 and
  GPU 0, by analogy with compute.

This becomes question Q8. Until then the TQ kick is compiled only with `-DEXP_TQ3D`, and
by default `TQ_3D` tasks are retired at once, so the host does not hang.

**Clean-room note.** Spec, public header, public Mesa layouts, our own probe data.
