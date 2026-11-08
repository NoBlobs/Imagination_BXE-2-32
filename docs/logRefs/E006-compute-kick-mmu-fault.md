<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# E006 — first compute kick: the CDM starts and faults at VA `0x26C0`

**Question.** Does the CDM kick sequence from `docsClean/kick-compute-and-3d.md` start
the compute data master?

**Set-up.**
- Default build.
- `deqp.sh "dEQP-VK.compute.pipeline.basic.empty_shader"`.
- `gpu-regs.py` with `EXTRA_REGS` covering the CDM, BIF and event registers.

**Observation** (sampled after the kick):

```
0130=00000200            EVENT_STATUS: bit 9 MMU_PAGE_FAULT (no COMPUTE_FINISHED)
12B0=00001041            BIF_FAULT_BANK0_MMU_STATUS: CAT_BASE=1, DATA_TYPE=2, FAULT=1
12B8=000026c0 12BC=00101800
                         BIF_FAULT_BANK0_REQ_STATUS: XE RNW=1 (read), TAG_ID=0x18,
                         ADDRESS=0x00_0000_26C0
1208=860b3000            BIF_CAT_BASE1 = the context's page catalogue
1240=00010000            BIF_CAT_BASE_INDEX: CDM -> slot 1
0480=000b8000 0484=00000080   CDM control stream base 0x80_000B_8000 (general heap)
0498=0006b000 049C=00000080   CDM context state base  0x80_0006_B000
04A8=00001ec0 04AC=00001e80   CDM_CONTEXT_PDS0: code 0x1EC0, data 0x1E80
04B0=28800009                 CDM_CONTEXT_PDS1
04B8=00002090 04BC=00002080   CDM_TERMINATE_PDS: code 0x2090, data 0x2080
04C0=28100003                 CDM_TERMINATE_PDS1
17A8=0001c400 17AC=00000020   TPU border colour table 0x20_0001_C400
F330=00000001                 multicore compute control
0BB0=00000200                 MTS_GPU_INT_STATUS mirrors the fault event
```

Decoding uses the field layout in `pvr_rogue_cr_defs.h`.
- **`CDM_CONTEXT_PDS0`**: `DATA_ADDR` is bits 63:36 and `CODE_ADDR` is bits 35:4,
  both with 16-byte alignment. That gives code `0x1EC0` and data `0x1E80`.
- **`REQ_STATUS`**: `ADDRESS` is bits 39:4 with 16-byte alignment, which gives
  `0x26C0`.

**Analysis.**
- The binding works: the fault is reported against **cat base 1**, the slot the
  firmware chose.
- The control stream and context state addresses are full 40-bit VAs in the general
  heap (`0x80_xxxx_xxxx`), so the CDM reached the stage where it launches a PDS program.
- The PDS addresses are small values (`0x1E80`–`0x2090`). Mesa allocates PDS programs
  in the PDS code/data heap (`ROGUE_PDSCODEDATA_HEAP_BASE = 0xDA_0000_0000`), and the
  hardware takes these fields as **offsets from the PDS execution base**.
- The faulting read at `0x26C0` lies in the same small range, 0x800 past the context
  PDS code. It is a PDS fetch relative to an exec base of **0**. VA `0x0–0x3FFFFF` is
  reserved by the host and never mapped.
- The upstream header `pvr_rogue_heap_config.h` confirms the mechanism: "ROGUE_PDSCODEDATA_HEAP_BASE and
  ROGUE_USCCODE_HEAP_BASE will be programmed, on a global basis, into
  ROGUE_CR_PDS_EXEC_BASE and ROGUE_CR_USC_CODE_BASE_*". The offsets of those
  registers are not public. The firmware receives the values in
  `sysinit.pds_exec_base` and `usc_exec_base`.

**Conclusion.** The firmware must program the PDS and USC execution bases at boot. This
is question Q7, which E007–E009 investigate experimentally.

**Clean-room note.** Own firmware, public headers, public Mesa behaviour.
