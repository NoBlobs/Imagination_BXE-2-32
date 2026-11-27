<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# E047 — New vkfwtest investigation tests on the board

**Question.** vkfwtest got tests aimed at the open firmware problems. What do they show on the
board?
- `second-*`: the second render on a device, one change at a time;
- `--baseline`: compare a run with the reference;
- `bigpb-layers`;
- `suspend-after`;
- `ctx-switch`;
- `rta` multiview step;
- `msaa` sample positions.

**Board.**
- BPI-F3, Arch Linux RISC-V, kernel `7.4.0-rc1-img`, GPU at 307.2 MHz.
- Firmware `f45474ed…` (E045).
- Mesa `5d2dc45` with RTA off (`~/mesa-rta-off`) unless stated.

## The second render on a device (group `second`)

Each test runs two renders on a fresh device (a full-screen triangle into a 64×64 RGBA8 image)
and changes one thing before the second:

| Test | Change before the second render | Result | Second render reads |
|---|---|---|---|
| `second-same` | none (new command buffer) | FAIL | the first render's colour (`7e0116ff`) |
| `second-resubmit` | none: **the same command buffer** (transfer to magenta, then the render) | FAIL | magenta: the transfer ran, the render wrote nothing |
| `second-newfb` | new image and framebuffer | FAIL | `00000000` |
| `second-newall` | new render pass, pipeline and framebuffer | FAIL | all pixels wrong |
| `second-compute` | a compute dispatch | FAIL | the first render's colour |
| `second-xfer` | a transfer | FAIL | all pixels wrong |

- **The second render writes nothing at all.** Even the render pass clear is missing.
- **It does not depend on what the commands contain.** Submitting the identical command
  buffer again fails the same way, and transfers and compute in between still work.
- **The cause is state left by the first render** on the device, in the driver, kernel or
  firmware.
- **The manufacturer firmware fails the same way**: `docsClean` `vkfwtest-results.md`,
  `multiframe` frame 1 reads frame 0's colour, `7e0116ff`.
- To be confirmed: the `second-*` tests with the manufacturer firmware.

## Comparison with the reference (`--baseline`)

```
vkfwtest -k -L 300 -T 40 -B mainline-mesa-f7c93a1-manufacturer-fw-1.1.txt all
```

| Count | Tests |
|---|---|
| **37 same** | the 39 old tests, except the two below |
| **2 worse** | `indirect` (known since E044); `load-store-none` (old Mesa lacked the extension and skipped; `5d2dc45` exposes it and fails, a Mesa difference) |
| **0 better** | — |
| **9 new** | `bigpb-layers` PASS, `suspend-after` PASS, the six `second-*` FAIL, `ctx-switch` FAIL |

`vertex-ext` and `queries` now match the reference. Totals: 19 pass / 27 fail / 2 skip. There is
no kernel or firmware fault.

## The other new tests

- **`bigpb-layers` passes** (2 frames, 4 M triangles on layer 0, clears on layers 1–3). Plain
  `bigpb` fails at frame 0. The two differ in:
  - a 2D array view;
  - a render pass built with `rp_build`;
  - one `vkCmdClearAttachments` per layer.

  This is a lead for `bigpb` (to investigate).
- **`suspend-after` passes**: the GPU suspends 100 ms after compute + sync work (autosuspend
  50 ms), then a render works. E043's "never suspends after the compute/sync suites" does not
  reproduce any more.
- **`ctx-switch`**: round 0 passes (both devices render and compute). Round 1 fails on
  device A's render, its second render.
- **`rta` step 6**, a 6-view multiview draw on a fresh device: passes with RTA on and off. When
  it ran as a later render on the same device it read `00000000`, the second-render failure
  again. So it now runs on a device of its own.
- **`msaa`**: the measured 4× sample positions equal the standard ones; the test passes.

## Cause of the second-render failures: Mesa ignores external subpass dependencies

**Trace of `second-resubmit`** (temporary trace on every `FENCE` check and UFO update; firmware
`5eb6ac85…`):
- **First submission:** the read-back copy (transfer context `0xc004a000`) is written into its
  CCB only after the fragment job finished. The host held it back, so the order was right.
- **Second submission:** the copy is in the CCB before the fragment job runs. It is kicked
  right after the image clear, reads the image while it is still magenta, and the render comes
  later.
- **The transfer context never has a `FENCE`/`FENCE_PR` command.** The firmware is never asked
  to order the copy after the render. The kernel writes a `FENCE_PR` for every native
  dependency still pending when a job goes to the CCB (`pvr_queue_submit_job_to_cccb`). So
  the copy had no pending dependency on the render.

**Two tests isolate it:**

| Test | How the copy is ordered | Result (both Mesa builds) |
|---|---|---|
| `second-same` | only the render pass's `VkSubpassDependency` to `VK_SUBPASS_EXTERNAL` (`COLOR_ATTACHMENT_OUTPUT` → `TRANSFER`) | FAIL 6/6 |
| `second-barrier` | the same, plus an explicit `vkCmdPipelineBarrier` | **PASS 6/6** |
| `second-split` | the copy in a later submission, after the render's fence | **PASS 3/3** |

**Cause.** Public Mesa `pvr` (`5d2dc45`):
- uses only subpass dependencies between two subpasses of the pass
  (`pvr_arch_pass.c`, `srcSubpass != VK_SUBPASS_EXTERNAL && dstSubpass != VK_SUBPASS_EXTERNAL`);
- emits no barrier when the render pass ends (`CmdEndRenderPass2`).

So a transfer after a render pass is not ordered after it, although the Vulkan spec requires
it. The first render of a test passes by timing.

- **Not a firmware bug.** The manufacturer firmware shows the same symptom (`docsClean`
  `vkfwtest-results.md`: `multiframe` frame 1 reads frame 0's colour).
- **vkfwtest is correct by the spec** and now works around the bug: its read-back helpers
  issue an explicit pipeline barrier before the copy. The `second-*` tests, except
  `second-barrier` and `second-split`, still rely on the subpass dependency alone, so they keep
  showing the Mesa bug.

## The battery with the work-around

Firmware `f45474ed…`, Mesa `5d2dc45` with RTA off, vkfwtest with the read-back barrier:

```
== 42 pass, 6 fail, 0 timeout, 0 lost, 0 error, 2 skip
== baseline mainline-mesa-f7c93a1-manufacturer-fw-1.1.txt: 20 same, 19 better, 0 worse, 11 not in it
```

- **The 6 failures** are `second-same`, `-resubmit`, `-newfb`, `-newall`, `-compute`, `-xfer`:
  the Mesa bug detectors.
- **Every other test passes**, including:
  - the 19 that failed with the manufacturer firmware too: `multiframe`, `pre-ta`, `bigpb`,
    `input-attach`, `zs-load`, `mrt`, `sizes`, `vertex`, `formats`, `secondary`, `multiview`,
    `raster`, `vertex-ext`, `texture`, `idle-resume`, `stress`, `soak`, `churn`, and
    `load-store-none` (which the old Mesa skipped);
  - `indirect`, which E044 counted as a difference from the reference;
  - the new `bigpb-layers`, `suspend-after`, `ctx-switch`, `second-split` and
    `second-barrier`.
- Skipped: `local-read` and `sparse`, which the driver does not support.
- No kernel or firmware fault.
- **The "better" column comes from the vkfwtest work-around, not from the firmware.** The
  baseline was taken without it. Comparing firmwares again needs the manufacturer firmware run
  with this vkfwtest.
- **What E044 called firmware leads were the Mesa bug**: the second-HWRTData family, renders
  after an OOM, renders after a transfer, and `indirect`.

## Render-target arrays without the race (Mesa with RTA on)

The same vkfwtest, with the RTA-enabled Mesa (`~/mesa-rta`), each run 3 times:
- **`msaa` passes 3/3.** The Q34 failure (all resolved pixels 0) was the read-back race, not
  the PBE resolve.
- **`rta` step 3 still fails**, but differently:
  - layer 1 (whole-area clear) now passes;
  - layer 5 is wrong outside its inner rect (3072 of 4096 pixels): it holds **layer 0's
    contents**, so the `LOAD` of layer 5 read layer 0;
  - the trace shows `RTC with RTA present, 3 active render targets` and renders 2 and 3 of 3,
    so the layer list is `{0, 1, 5}` as before.
  - Mesa's load-op program reads `RENDER_TGT_ID` to pick the layer (`rta_load_clear`), the same
    value the end-of-tile program uses correctly. Opened as **Q37**, which also asks for the
    manufacturer firmware run with the current vkfwtest.

## Against the manufacturer firmware with the same vkfwtest (baselines `9d74761`)

vkfwtest `9d74761` adds two baselines:
- **The setup:** the manufacturer open firmware v1.1 (build `6976702 OS`), the same modified
  Mesa `5d2dc45` with RTA off and on, and vkfwtest `8932a2f`, which has the read-back barrier.
- **RTA off:** 41 pass, 7 fail (the six `second-*` and `suspend-after`), 2 skip.
- **RTA on:** 40 pass, 8 fail (adds `rta`, subset layer 5), 2 skip.

**Our firmware** (`f45474ed…`, `build 1 OS` checked in dmesg before the runs):

| Mesa | vs baseline | Differences |
|---|---|---|
| RTA off | **49 same, 1 better, 0 worse** | `suspend-after` passes on ours, failed in the baseline |
| RTA on | 47 same, 1 better, 2 worse | `bigpb-layers` (frame 1 layer 0, 835584 of 1048576 pixels), `stress` (frame 7); a context reset (`Guilty lockup`, page fault at 0) in dmesg |

**Run-to-run variation, not a firmware difference.**
- A rerun of the **manufacturer firmware** on the same board (made by mistake, while its build
  was still loaded) gave the same `bigpb-layers` failure: frame 1, layer 0, 835584 pixels.
- It also gave a `stress` failure (frame 10 with RTA on, frame 12 with RTA off), `Guilty
  lockup` / `Guilty overrunning` resets, and `suspend-after` passing.
- So with RTA on, `bigpb-layers` and `stress` fail on some runs with **both** firmwares, and
  `suspend-after` passes on some runs with the manufacturer firmware.

**Q37 answered by the data.** The manufacturer firmware fails `rta` step 3 the same way: layer 5
outside the rect holds layer 0's contents, 3072 of 4096 pixels. Both firmwares behave alike, so
the background load is most likely on the Mesa side.

**Conclusion.** On the whole current vkfwtest battery, our firmware gives the manufacturer
firmware's results with RTA off and with RTA on. The differences seen are run-to-run variation
that both firmwares show, plus `suspend-after`, where ours was better in these runs.

## The two differences with RTA on

### `bigpb-layers` with RTA on: Mesa's partial render supports layer 0 only

With RTA on, `bigpb-layers` is not flaky. It **fails 4/4**, always in frame 1, layer 0
(835584 of 1048576 pixels), with three `Guilty lockup` resets (page fault, address 0). With RTA
off it passes 4/4.

What the trace shows:
- **Frame 0** (HWRTData `0xc00bd000`): every OOM is resolved by growing the free list. The
  layer loop renders 4 layers. The frame passes.
- **Frame 1** (HWRTData `0xc00c0000`): the first OOM cannot grow any more. A partial render
  follows (`Kick 3D ... Partial render:1`), and at once `BIF0 page fault detected` (MMU status
  `0x1041`, request `0x0002b100…`), HWR, `Discarded cmd` on DM2 and DM3. The final 3D faults
  too (`0xd101`).

Plain `bigpb` (partial renders, one layer) passes with RTA on. So it is **partial render x
render-target array**.

**Cause, Mesa:** public Mesa's partial-render scratch setup (`pvr_arch_spm.c`,
`pvr_spm_setup_texture_state_words`) says "we only support writing to layer 0" and ignores the
framebuffer's layer count. The manufacturer firmware fails the same way: same frame, same
pixel count, same reset. Our missing `FRAG_PR` layer loop (E045) cannot be tested until Mesa
supports layered partial renders.

### `stress`: intermittent with both firmwares, not RTA

| Mesa | Runs | Failures | dmesg |
|---|---|---|---|
| RTA on | 3 | 1 (frame 3) | 1 `Guilty lockup`, 3 `Guilty overrunning` |
| RTA off | 3 | 2 (frames 5, 7) | 1 `Guilty lockup`, 2 `Guilty overrunning` |

- In the firmware trace, the failing runs show a DM3 lockup (`Lockup detected on DM3`) and a
  BIF0 page fault (`0x9041`, request `0x0000ed0000001180`) while 4 contexts run at once.
- The rerun of the manufacturer firmware failed `stress` too, with RTA off and on, with the
  same kinds of resets. Its baseline is a run where it passed.
- The cause is open. It is shared by both firmwares, so most likely it is not a gap of ours.
  Asked as **Q38**.
