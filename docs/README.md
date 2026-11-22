<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# docs — project documentation

This folder holds the documentation of the free PowerVR Rogue **BXE-2-32**
(BVNC `36.29.52.182`) GPU firmware for the **SpacemiT K1** SoC. It is written
by the clean team in reStructuredText (`.rst`) and built with Sphinx. It
complements, and never modifies, the clean-room specification in
[`../docsClean/`](../docsClean/README.md).

## Structure

```
docs/
├── index.rst            entry point, status overview and provenance labels
├── hardware/            BXE-2-32 GPU and K1 SoC hardware as seen by the firmware
├── linux-driver/        mainline Linux powervr driver integration and host contract
├── firmware/            clean-room GPLv3 firmware implementation
├── logRefs/             experiment log (E001…) and raw dumps in logRefs/data/
├── questions.md         questions for the clean-room spec maintainer (dirty team)
├── conf.py              Sphinx configuration (Read the Docs theme + dark mode)
├── Makefile             build entry points
└── requirements.txt     Python packages for the build (Sphinx, RTD theme, dark mode)
```

| Part | License (SPDX) | Contents |
|---|---|---|
| `hardware/` | `CC-BY-SA-4.0` | GPU architecture (BVNC `36.29.52.182`, MIPS32r2 microMIPS core, data masters), two-stage MIPS translation and wrapper remap, RGX register map with provenance and writable-bit probe, GPU bring-up / Parameter Manager / flush / HWR hardware behaviour, reference board (`BPI-F3`) and bench safety rules. |
| `linux-driver/` | `GPL-2.0-only` | Host side of the contract with mainline `drm/imagination` (`powervr.ko`) and Mesa `pvr`, kernel branch `IMG-BXE-2-32` (K1 APMU power domain, DT node, experimental BVNC), `.fw` container and FWIF ABI, host-firmware protocol (KCCB, FWCCB, IRQ, runtime PM, stats, HWR log, debugfs trace), scripts and test workflow. |
| `firmware/` | `GPL-3.0-or-later` | Clean-room implementation (`src/`): source files, interrupt-driven execution model, clang/lld microMIPS toolchain and experiment knobs, `boot.S` / TLB refill / `gpu_init.c`, client CCB scheduler and data-master kicks (CDM, TQ_3D, experimental GEOM/FRAG), power / HWR / stats, current status and experiment findings (`E001`–`E040`). |

### Licenses

Each section follows the license of its corresponding domain, stated in the
`.. SPDX-License-Identifier:` line at the top of every file:

- `hardware/` carries **`CC-BY-SA-4.0`**, matching `docsClean/`.
- `linux-driver/` carries **`GPL-2.0-only`**, matching the Linux kernel.
- `firmware/` (and the Sphinx configuration/index files) carries **`GPL-3.0-or-later`**, matching the project `LICENSE`.

## Building

Requirements: `python3` with `venv` and network access on the first build.
Sphinx and the [Read the Docs theme](https://sphinx-rtd-theme.readthedocs.io/)
with the [dark mode toggle](https://github.com/MrDogeBro/sphinx_rtd_dark_mode)
are installed into `docs/.venv/` the first time.

```sh
cd docs
make            # HTML (default)  -> docs/_build/html/index.html
make html       # same
make serve      # build and serve on http://127.0.0.1:8765
make clean      # remove _build/
make distclean  # also remove .venv/
```

`_build/` and `.venv/` are ignored by git (see `.gitignore`).

## Writing

- Start each new `.rst` page with its `.. SPDX-License-Identifier:` and
  `.. Copyright (C)` lines according to its section (`hardware/`, `linux-driver/`,
  or `firmware/`).
- Add new pages to the `toctree` in that section's `index.rst`.
- Cite using the link roles defined in `conf.py`:

  | Role | Example | Links to |
  |---|---|---|
  | `:dc:` | ``:dc:`boot-gpu-init` `` | `docsClean/boot-gpu-init.md` at the pinned submodule commit |
  | `:dc:` with title | ``:dc:`docsClean/ <README>` `` | submodule root at the pinned commit |
  | `:exp:` | ``:exp:`E013` `` | `docs/logRefs/E013-*.md` (warns if missing) |
  | `:lr:` | ``:lr:`data/survey-0-2000.txt` `` | file or directory under `docs/logRefs/` on `main` |

## Publishing (GitHub Pages)

`.github/workflows/docs.yml` builds the HTML with warnings treated as errors
(`sphinx-build -W --keep-going`) on every push to `main` that touches `docs/`
or the workflow, and deploys it with GitHub Pages. Pull requests build and
check without deploying.
