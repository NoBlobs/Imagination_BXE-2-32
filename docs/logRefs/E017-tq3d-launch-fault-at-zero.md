<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# E017 — TQ_3D with the two-write launch: the 3D pipe starts, then writes to address 0

**Question.** The spec answer to Q8 adds a second trigger write, `BCAST[0x0F00] = 1`
after `ISP_RENDER |= 0x10`, and says `0x06B0` is not written by the transfer kick. Does
the TQ now complete?

**Changes.**
- `kick_tq3d()` now ends with `BCAST[0x0F00] = 1`, and `0x06B0` is no longer written.
- The kick is enabled by default.
- New HWR (E019), so that a failing TQ no longer blocks the device.
- Diagnostic traces of the host's transfer registers (`DBG_7`/`DBG_9`, debug group, mask
  `0x80007FFE`).

**Observation** (`dEQP-VK.compute.pipeline.basic.copy_ssbo_to_image_small`):

```
pbe_wordx_mrty[0..2] = 0x0200007c_96800000  0x0000003f_0000003f  0x00000000_00000000
isp_mtile_base       = 0x00000000_00000000
pds_bgnd0_base       = 0x00001e40_00041000
pds_bgnd3_sizeinfo   = 0x01000000_00010001
isp_render = 0x3 (FAST_SCALE)   isp_rgn = 0
[39301434] : Kick 3D TQ: FWCtx 0xc004a000 @ 0, …
[39301697] : BIF0 page fault detected (Core 0 MMU Status: 0x0000000000002041
             Req Status: 0x0002b10000000000)
… HWR (E019) … "Recovery DM3: DM fully recovered"
```

- **The pixel pipe now starts.** It reaches memory less than 1 µs after the kick, and
  across the group the firmware saw `PIXELBE_END_RENDER` (bit 3) and
  `ISP_END_MACROTILE` (bit 4).
- It then takes an MMU fault: **write** (RNW = 0), address **`0x00_0000_0000`**, tag
  field `0x2B1` (TAG_ID 0x31 / TAG_SB 0x0A), with the context's cat base (1 or 2). The
  fault is the same whether the PBE words are written contiguously (9 × 64-bit from
  `0x1510`) or planar (`WORD0_MRTn = 0x1510 + 8n`, `WORD1_MRTn = 0x1550 + 8n`), so the
  PBE register layout is not the cause.
- The PBE register words hold format, stride and clip only (`0x3f/0x3f` = a 64×64
  surface). The destination address is not among them. It must reach the PBE through the
  end-of-tile PDS program (`event_pixel_pds_*`), or through a register the kick does not
  program yet.
- The host's `isp_mtile_base` is 0 for this TQ ("when applicable" in the spec). By
  public Mesa, `ISP_MTILE_BASE` has address bits 39:2, but the writable mask of `0x0F20`
  is `0xFF_FFFFFFF0`. So `0x0F20` may not be `ISP_MTILE_BASE`.

**Conclusion.** The launch sequence is right: the 3D pipe runs. Some output address
is still 0. This is question Q12 (which unit writes to address 0, and which register it
takes the address from). The default build uses the planar PBE layout, the spec's
reading; `PBE_LAYOUT=0` gives the contiguous one.

**Clean-room note.** Spec, public headers and Mesa layouts, own firmware.
