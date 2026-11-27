<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# E044 — `vkfwtest`: a pixel-checking Vulkan harness, and what it finds

**Question.** `docsClean` `040b65e` (`board-testing-oracle.md` section 4) asks for a
standalone Vulkan harness that checks its output on the CPU. What does the firmware do when
the pixels are actually checked?

**Harness.**
- **New repository `NoBlobs/vkfwtest`:**
  - GNU C23 for GCC, independent of the CPU architecture;
  - shaders embedded as SPIR-V arrays;
  - written only from the public Vulkan API and `docsClean`;
  - documented in `docs/firmware/test-harness.rst`.
- **Tests:** `multiframe` (`pre-3d`), `pre-ta`, `pre-cdm`, `xfer`, `bigpb`.
- **Its own checks:** on the development PC's NVIDIA GTX 1060, `vkfwtest -k all` passes
  every test. That includes `--iters 200`, NPOT sizes and `bigpb` at 8 M triangles.
- **Board build:** with `tools/deploy.sh vkfwtest` (GCC 14, `-std=gnu23 -Werror`).

**Board.** BPI-F3 16 GB, kernel `7.4.0-rc1-img`, firmware build `1b5eae38…`.

## Oracle with the new harness (`oracle.sh A D B`)

| Check | Result |
|---|---|
| A | pass |
| D: suspend, then `vkfwtest xfer` after the warm boot | **pass** |
| B `pre-cdm` (4 × 4096 words) | **pass** |
| B `xfer` (upload, offset copy, mirrored blit, 4× MSAA resolve; 137×91 and 256×256) | **pass** |
| B `multiframe` | **FAIL**: frame 0 right; frame 1 (the second HWRTData) is still frame 0's image, all 65 536 pixels |
| B `pre-ta` (131 072 triangles) | **FAIL**: frame 0 all zeros, not even the clear colour |
| B `bigpb` (4 M triangles) | **FAIL**: frame 0 all zeros, 1.8 s, no lockup |

None of the three failures shows a fault, a discard or an HWR in the trace. They were
invisible to the earlier `vkfwtest` (no checks) and to the oracle's trace proofs: `pre-3d`
"passed" in E043.

## Narrowing the two render failures

| Run | Result |
|---|---|
| `pre-ta -t 2` / `-t 2K` (one layer) | frame 0 **right**, frame 1 wrong |
| `pre-ta -t 30K` (32 768 triangles) | frame 0 all zeros |
| `multiframe -f 1 -s 512` | pass |
| `multiframe -f 3` | frame 1 wrong |

1. **Renders on the second HWRTData of a pair write nothing.** The output keeps the
   previous frame's pixels; even the render pass clear is missing. The trace shows a normal
   `Kick TA`, `TA finished`, `Kick 3D`, `3D finished`.
2. **A render whose geometry runs out of parameter memory writes nothing.**
   - `pre-ta -t 30K` gets one OOM (ready pages), a wait for grow, the grow update, then `TA
     finished`;
   - `Kick 3D` runs for 29 ms and `3D finished`;
   - the image is all zeros.

   Renders without OOM, on the first HWRTData, are right.

Both point at the 3D side of the render (the pixel back end writes nowhere), not at the
geometry. They are the next firmware problems to investigate. The harness keeps them
visible.

## More tests (`input-attach`, `rta`, `hwr-lockup`, `prio-*`)

The rest of `docsClean` section 4 is now implemented:
- **`input-attach`:** two subpasses; colour + `D32` depth read as input attachments.
- **`rta`:** a 6-layer target: layered clears, plus `gl_Layer` draws when available.
- **`hwr-lockup` (stage E):** a controlled lockup, then renders on new devices.
- **`prio-3d`/`-ta`/`-cdm` (stage F):** a LOW-priority long job vs HIGH-priority probes.

The LCG check now uses the closed form (composition by squaring), so jobs of up to 10⁸
iterations are still checked exactly. On the PC (GTX 1060) all of these pass. The `prio-ta`
probes are not delayed; `prio-3d` and `prio-cdm` are.

**On the board (firmware `1b5eae38…`):**

| Test | Result |
|---|---|
| `input-attach` | frame 0 right in both subpasses; **frame 1 wrong** (the second HWRTData again) |
| `rta` | **pass**: layered clears on 6 layers. Mesa offers no `VK_EXT_shader_viewport_index_layer`, so the draw part is skipped |
| `prio-cdm` | **pass**: outputs right; probes delayed up to 1.4 s (no preemption, as expected) |
| `prio-3d` | fails in calibration: its second render is on the second HWRTData |
| `hwr-lockup` | the watchdog recovers after 30 s (`Guilty overrunning`), but **the fence signals normally: no `VK_ERROR_DEVICE_LOST`**. After it, `pre-cdm` passes; `multiframe` fails only on the second-HWRTData bug |

New finding: stage E requires the guilty device to see `VK_ERROR_DEVICE_LOST`. Our recovery
runs the discarded command's updates, so its fence signals as if it had completed.

## Ten more test groups

Twelve more tests, to cover paths the `docsClean` list does not:

| Test | Paths |
|---|---|
| `texture` | NPOT texture, 4 address modes, 2 border colours, linear filter, 6 mip levels |
| `zs-load` | depth + stencil stored and loaded across render passes |
| `msaa` | 4× rendering resolved in the render pass |
| `mrt` | 8 colour attachments |
| `sizes` | targets from 1×1 to 4096×4096, plus a partial render area |
| `vertex` | vertex/index buffers, instancing, points, lines |
| `indirect` | draw / indexed / multi-draw / dispatch indirect |
| `queries` | occlusion queries and timestamps |
| `formats` | RGB565, BGRA8, sRGB, RGBA16F, R32_UINT, additive blend, `discard` |
| `stress` | 4 devices at once |
| `soak` | one long-lived device |
| `churn` | 20 devices created and destroyed |
| `hwr-fault` | stage E with an MMU fault, not in `all` |

`tools/oracle.sh` gains stage F (these tests plus `prio-*`) and runs `hwr-fault` in stage E.
On the PC's GTX 1060, all 19 tests of `all` pass.

**On the board (firmware `1b5eae38…`):**

| Test | Result |
|---|---|
| `msaa` | **pass**: the 4× render, resolve and diagonal coverage are right (first render on its device) |
| `sizes` | **pass** for every size when run alone (`--size`). In one run, the third render on the device fails |
| `zs-load`, `mrt`, `vertex`, `indirect`, `stress`, `soak`, `churn` | fail at the **second render on the device** (pass 2, frame 1, points after quads, …) |
| `texture` | the **first** render is all zeros. It follows the texture upload (a `TQ_3D`), so it may be "a render after a transfer" |
| `formats` | RGB565 and BGRA8 pass; sRGB (the third render on the device) fails |
| `queries` | the occlusion count is **0** on the first render. Our fragment kick probably does not program the visibility-result base |

New firmware leads from this run:
- renders after the first on a device: the same family as `multiframe`;
- a render right after a transfer (`texture`);
- occlusion query results (`queries`).

## Full battery on the board (vkfwtest 0.2, 46 tests)

vkfwtest now needs only Vulkan 1.0, and newer features are used only when present. On the
board it negotiated **Vulkan 1.2** (instance 1.4, device 1.2). Firmware build `1b5eae38…`,
run with `vkfwtest -k -L 300 -T 40 all`:

**16 pass, 20 fail, 3 skip; no timeout, no device lost, no board hang.**

| Group | Result |
|---|---|
| compute | **4/4**: `pre-cdm`, `resources` (incl. push descriptors), `compute-dims` (incl. `vkCmdDispatchBase`), `subgroups` |
| transfer | **5/5**: `xfer`, `buffers`, `ds-copy`, `blit-mips`, `image-copy` |
| sync | **2/2**: `sync` (incl. timeline semaphores, `sync_file`), `sync2` (through `VK_KHR_synchronization2`) |
| memory / robust | `memory`, `robust` pass; `sparse` skipped (no buffer residency) |
| texture | `texture2` passes (incl. ETC2); `texture` fails (below) |
| render | `msaa` (incl. depth resolve) and `rta` (layered clears) pass; 15 fail; `load-store-none` and `local-read` skipped (the driver lacks the extensions) |
| power / stress | `idle-resume`, `stress`, `soak`, `churn` fail |

The failures sort into the leads found earlier:

- **The second render on a device writes nothing.** This is the cause of most failures:
  - frame 1 or pass 2 fails in `multiframe`, `input-attach`, `zs-load`, `mrt`,
    `secondary`, `multiview`, `idle-resume`, `stress`, `soak` and `churn`;
  - the second sub-case fails in `vertex` (points after quads), `indirect`, `raster`
    (strip after the negative-viewport case) and `vertex-ext` (divisor after 8-bit
    indices);
  - the third render fails in `sizes` (33×17) and `formats` (sRGB).
- **A render whose geometry runs out of parameter memory writes nothing:** `pre-ta`,
  `bigpb`, frame 0.
- **A render right after a transfer writes nothing:** `texture`, frame 0, after the
  texture upload.
- **Occlusion query results are 0:** `queries`.

Compute, transfers, synchronisation and memory are right everywhere. The firmware work that
remains is on the 3D side of renders after the first.

**Clean-room note.** Own firmware, own harness, measurements on the board; `docsClean` section 4
for the test design.
