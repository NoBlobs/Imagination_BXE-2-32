# Geometry (TA) kick — registers and ordering dependencies

Details the **geometry kick** (DM 2/6) at register level: **which registers must be
programmed**, **with what data**, and **which ordering dependencies are mandatory**. These
are hardware/behaviour facts, not a routine to copy: the code organisation is free as long
as the dependencies are respected. Complements [data-masters](data-masters.md) and
[command-structs](command-structs.md).

## Register notation

- **`CR[off]`**: register at offset `off` in a **core's** bank, in the window
  `0xCF800000 + core·0x80000` (see [memory-model](memory-model.md)).
- **`BCAST[off]`**: broadcast write (all enabled cores, bank `0xCFA00000`).

> **On the `CR[set+off]` notation used below (answers Q15).** The geometry registers below
> are at **fixed offsets** (`0xC00`, `0xC88`, `0x400`, …) in the bank of the core that runs
> the geometry (the **primary** core for DM 2; the second core of the active list for DM 6).
> There is **no** extra per-"set" displacement or stride beyond the per-core bank stride
> (`0x80000`): on this single-core SoC every such write lands in the primary core's bank.
> Read `CR[set+0xC00]` below as "offset `0xC00` in the geometry core's bank". The trace
> `Setup register set=N` reports the **BIF page-catalogue slot** chosen by the
> memory-context activation, not an addressing base for these registers. Observed (**O**):
> after `Setup register set=0 DM=2`, the context's page catalogue is in
> `ROGUE_CR_BIF_CAT_BASE1` and `ROGUE_CR_BIF_CAT_BASE_INDEX = 0x101` (`TA` and `PIXEL`
> fields = 1). So trace set *N* means catalogue register `CAT_BASE{N+1}` and index-field
> value *N+1*; `CAT_BASE0` belongs to the firmware. Compute and transfer bind the same way through
> `ROGUE_CR_BIF_CAT_BASE_INDEX` and do not use a separate addressing base either.
- Public names, where they exist, are from `pvr_rogue_cr_defs.h`. The public header names
  only a subset of the registers; the others are given by offset.

> **Provenance.** Offsets and dependencies: **D** (firmware analysis), confirmed by **O**
> (the milestones appear in the trace: `Activate MemCtx`, `Setup register set`, `Kick TA`).
> Values come from `rogue_fwif_cmd_geom` (public header) and from the context state.

## Inputs

The context (`rogue_fwif_fwcommoncontext`: memory context, register set), the geometry
command (`rogue_fwif_cmd_geom`, right after the 0x28-byte header in the cCCB) and the
command offset (used in the trace entry and on retirement).

## Phases and dependencies

The phases below have a **mandatory order**: each depends on the previous one.

### A. Preconditions

- The power domain must be **on** (wake the GPU if needed).
- On the first kick after a GPU-units initialisation, if the context's `rf_cmd_addr` is
  non-zero, the firmware first runs a compute job from it (see the transfer kick in
  [kick-compute-and-3d](kick-compute-and-3d.md)). The mainline host leaves it 0.
- The **memory context** is bound to a `ROGUE_CR_BIF_CAT_BASE` slot (1–7), which selects
  the **register set** to use. Trace: BIF id 14, `Activate MemCtx=… DM=… secure=…`. Must
  happen before any write to the register set.

> **Observation run (O) behind phases B, C and E.** Black-box observation on the board of
> one first-kick geometry job (`FIRSTKICK | LASTKICK`, from
> `dEQP-VK.draw.renderpass.simple_draw.simple_draw_triangle_list`, 256×256 target). The
> method compares the values in the geometry command (read from the cCCB while the job was
> live) and the addresses printed by the public trace with a read-only snapshot of the
> geometry core's registers, taken while the GPU was held powered. Firmware: the open image
> **v1.1, build 6976702** (the one this spec describes); the same mapping was also
> observed with the open image v1.0, build 6734358. Board DRAM at 1066 MT/s.

### B. Load the render target into the PM and synchronise

**The two PM contexts (O, public trace).** Geometry uses **PM context 0** (trace "HW
context 0", "PMDM0"). Fragment uses **PM context 1** ("HW context 1", "PMDM1"). The observed
order is:

1. **Before the geometry kick:** load the HWRTData's **local** and **global** freelists into
   PM context 0. Trace PM: `Load  Freelist … type: 0 (0:local,1:global) for PMDM0: FL Total
   Pages …, FL-addr = …, stacktop = …, Alloc Page Count = …`, then the same for type 1.
2. Load the HWRTData on HW context 0: RTD id 15, `TA RTData … loaded on HW context 0`.
3. **Before the fragment kick of the same HWRTData:**
   - **store** the freelists from PM context 0 (`Store Freelist … for PMDM0`). This is
     where the freelist state left by the geometry is captured; the trace shows
     `stacktop` 0x1F → 0x10 and `Alloc Page Count` 0 → 6.
   - **load** the freelists into PM context 1 (`Load Freelist … for PMDM1`).
   - load the 3D stack pointers (RTD id 12, `Loading stack-pointers for 1 (0:MidTA,1:3D) on
     context 1, MLIST = …, ALIST = …`).
   - `3D RTData … ready on HW context 1`.
4. **On HWRTData cleanup:** store the freelists back from both PM contexts.

**Registers observed holding the loaded PM state (O):**

| Register | Observed value | Matches |
|---|---|---|
| `0x0248`/`0x024C` | `0x80_0013_A000` | `rogue_fwif_hwrtdata.vheap_table_dev_addr` (trace "VHeap") |
| `0x0210`/`0x0214`, `0x0218`/`0x021C` | `0x80_0013_9000` (both) | local freelist `freelist_dev_addr` (trace "FL-addr"), one register pair per PM context (`TO CONFIRM` which is which) |
| `0x0224` | `0x1F` | local freelist stack top at rest (32 pages, trace "stacktop") |
| `0x0270`, `0x0278`, `0x0280` | `0x2` | ALIST stack pointer (trace "ALIST = 0x2") |
| `0x02C0`, `0x02C8`, `0x02D0` | `0x9` | MLIST stack pointer (trace "MLIST = 0x9") |
| `ROGUE_CR_BIF_PM_CAT_BASE_{VCE,TE,ALIST}0` and `…1` | the same three values in both banks | page-catalogue entries of the HWRTData, bank 0 = PM context 0, bank 1 = PM context 1 |

The global freelist's address (`0x80_0004_8900` in this run) was not found in the sampled
ranges. How the 4-entry arrays `vce_cat_base[4]`/`te_cat_base[4]` map to the single entry
per bank is `TO CONFIRM`.

#### PM freelist registers (answers Q16b, O)

Two observation runs on the open v1.1 image:
- the draw above;
- `bigpb`: a 1024×1024 target with millions of triangles, out-of-memory, freelist grows and
  partial renders.

In both, register snapshots were compared with the values printed by the public
`Load`/`Store Freelist` trace lines. Results:

| Register | Holds | Evidence |
|---|---|---|
| `0x0210`/`0x0214` | **local** freelist base (`freelist_dev_addr`) of **PM context 0** | changes at the geometry load; equals the trace "FL-addr" |
| `0x0218`/`0x021C` | the same for **PM context 1** | changes only at the fragment load, to the same address |
| `0x0224` | local freelist **stack top** | `0x1F` at rest (32 pages → total − 1), `0` when exhausted; equals the trace "stacktop" |
| `0x0398` | local freelist **allocated page count as loaded for PM context 1** (corrected by Q16k; the live counts are `0x03A0`/`0x03A8`) | `6` and `20` in the two runs = trace "Alloc Page Count" |
| `0x039C` | local freelist **allocated MMU page count as loaded for PM context 1** (corrected by Q16k; the live counts are `0x2000`/`0x2008`) | `9` and `11` = trace "Alloc MMU Page Count" |
| `0x02D8`/`0x02DC` (PM context 0), `0x02E0`/`0x02E4` (PM context 1) | a per-HWRTData address | alternates between the two HWRTData of `bigpb` at each geometry kick, and is copied to the context 1 pair at the fragment kick. Consistent with `pm_mlist_dev_addr` (`TO CONFIRM`: the HWRTData contents could not be read to match the value) |
| `0x0270/0x0278/0x0280`, `0x02C0/0x02C8/0x02D0` | ALIST / MLIST stack pointers | the first register of each triple follows the geometry, the other two take its value at the fragment kick |

Other answers:
- **`0x0220`** read back as **0** in every snapshot of the original firmware, including
  while freelists were loaded. Whether its low field is the total page count is
  `TO CONFIRM`; the observed stack top is at `0x0224`.
- **No register in `0x0200–0x043F` holds a value of the global freelist** (address, total,
  stack top, counts), although these values changed many times during `bigpb`. The global
  freelist's PM registers are outside the sampled range (`PENDING`).
- **`PM_CAT_BASE_*` on a first kick:** bank 0 (`…0`) receives new, valid (bit 0 = 1)
  entries at **every** geometry kick, including first kicks where the host left the
  HWRTData catalogue bases at 0. Bank 1 (`…1`) takes the same values at the fragment kick.
  The PM fills bank 0 itself during the geometry, from the local freelist (see Q16f
  below). The second word of
  `VCE0`/`TE0` (`0x124C`/`0x1254`) becomes non-zero only around partial renders.
- **Other PM enables:** `ROGUE_CR_PM_PARTIAL_RENDER_ENABLE` (`0x0338`) read 0 in every
  snapshot, and `0x03E0` read 1 throughout. A separate PM "context select" or "start"
  register was not identified (`PENDING`).

#### PM allocation follow-up (answers Q16e, O)

A second `bigpb` run on the open v1.1 image sampled `0x0200–0x0800` and `0x2000–0x2200` at
every freelist, OOM, grow and kick event. The global freelist's base, size and stack pointer
change hundreds of times in that run, which makes matching possible.

**1. Global freelist registers.** They are in the `0x2000` block, as **two banks with a
stride of `0x40`**. The bank at **`0x20B8` belongs to PM context 0 (geometry)**, the bank at
**`0x2078` to PM context 1 (fragment)**. This mapping was corrected by Q16g: register sampling
at the loads shows the geometry load writing `0x20B8` and the fragment load writing `0x2078`.
The trace lines used are `UFL-TA-Base`/`UFL-3D-Base`, `Freelist update completed` and
`OOM: Resumed TA`.

| Bank offset | PM ctx 0 (geometry) | PM ctx 1 (fragment) | Holds | Evidence |
|---|---|---|---|---|
| `+0x00`/`+0x04` | `0x20B8`/`0x20BC` | `0x2078`/`0x207C` | **global freelist base** (64-bit) | equals the trace base |
| `+0x08` | `0x20C0` | `0x2080` | **stack pointer to load** | written at every load and freelist update; the live `+0x10` takes the same value within ≤ 13 µs (Q16g, Q16h) |
| `+0x10` | `0x20C8` | `0x2088` | **live stack pointer** (the trace `SP = …`), maintained by the PM, not writable | see the identity below; `0xFFFF` = 65535 with all 65536 pages free, `9` at the partial render where the trace prints `SP = 9` |
| `+0x18` | `0x20D0` | `0x2090` | PB page count at the last freelist update/store | equals the trace "4PB" at those points (11212 at a grow update), then holds its value |
| `+0x1C` | `0x20D4` | `0x2094` | MMU page count at the same points | equals the trace "4PT" (19, 26, 124…) |
| `+0x20` | `0x20D8` | `0x2098` | **allocated PB pages** (live) | `65400` at the partial render where the trace prints `4PB = 65400` |
| `+0x28` | `0x20E0` | `0x20A0` | **allocated MMU pages** (live) | `126` at the same point (`4PT = 126`) |

When both PM contexts use the same global freelist (geometry and fragment running from the
same parameter buffer), both banks are updated together and their live values move
together.

In every snapshot of two runs, for both banks:
**stack pointer + allocated PB pages + allocated MMU pages + 1 = total pages** of the
freelist (for example `0xAEA + 0x2102 + 0x13 + 1 = 11264`). The stack pointer counts down as
the PM hands out pages.

The **local** freelist has no copy in the `0x2000` block. Its registers are the
`0x0210`/`0x0218`, `0x0224` and `0x0398`/`0x039C` given above, and the registers of Q16k.

**2. Starting the PM.** `PM_PAGE_MANAGEOP` (`0x02A0`) read **0 in all 37 snapshots**,
including while the geometry was running and allocating. It goes to 1 only briefly, around
freelist updates (`PM pause/unpause TA ALLOC`, see
[partial-render-spm](partial-render-spm.md)). Nothing indicates that the firmware writes it
to start allocation on a first kick: 0 means "not paused".

The public trace prints `CONTEXT_PB_BASE set to 0x…` when freelists are loaded:
- `0x3` at the geometry load, together with `FL different between TA/3D: local: 1, global: 1`;
- `0x0` at the fragment load, when TA and 3D freelists are the same.

Register **`0x02B0`** held this same value in **73 of 75 snapshots** across three runs. It is
therefore the register behind `CONTEXT_PB_BASE` (`TO CONFIRM` the bit meaning: the observed
pairing suggests bit 0 = local and bit 1 = global freelist differ between the two PM
contexts).

**3. `0x0D00`.** With the original firmware the values seen were `0x30`, `0x34` and `0x35`
while geometry was running, `0x3F` after jobs, and 0/1/4/5 at other moments. **`0x0F` was
never observed**: in the original, bits 4 and 5 are set while the geometry runs. Which units
these bits stand for is not established.

**4. Write-ordered list.** Not provided. The complete ordered sequence of the original's
register writes would be a transcription of its kick routine, which the clean-room rules
exclude, and the observation cannot produce it anyway. What this spec gives is the
registers, their values and sources, and the ordering constraints the hardware imposes.
Registers that are written and then self-clear during the kick are, by construction,
invisible to these snapshots (`PENDING`).

#### PM page catalogues on a first kick (answers Q16f, O)

A third `bigpb` run on the open v1.1 image read, at each geometry kick, the `PM_CAT_BASE_*`
registers, both freelists' page stacks and the HWRTData. All reads were done from the host,
through the GPU page tables of the application's memory context. Six geometry kicks were
examined; they include first kicks of both HWRTData.

**1. Who fills `PM_CAT_BASE_*0`.** The **PM**, during the geometry, with pages it takes from
the **local** freelist. Evidence:
- At the first kick, the HWRTData's `vce_cat_base[]`, `te_cat_base[]` and `alist_cat_base`
  are all 0 (the host leaves them at 0).
- While that geometry runs, `PM_CAT_BASE_VCE0`, `TE0` and `ALIST0` are valid (bit 0 = 1).
  Their `ADDR` fields are pages of the local freelist's stack: entries **29, 18 and 13**,
  with the stack top loaded at 31 (trace `FL-TA-Base … SP = 31`).
- The positions were the **same in all six kicks**, although the stack's contents were in a
  different order each time (pages come back in a different order).
- The three pages are **not consecutive** stack entries: other pages were handed out between
  them. This is what on-demand allocation by the PM looks like, not a preamble that takes
  pages before the kick.

Nothing was found that the firmware writes to make this happen besides the freelist and PM
context registers already described (local and global freelist, `CONTEXT_PB_BASE`, VHeap,
MLIST). No separate enable was identified. Whether the firmware also writes
`PM_CAT_BASE_*0` (for example to 0) before a first kick is `TO CONFIRM`: such a write would
be overwritten before any snapshot.

Two corrections to the reading of the earlier answers:
- **`0x0224` does not move while the PM allocates.** In the original it stays `0x1F` during
  geometry, even after the PM has taken all 32 local pages (stores report `SP = 0`). It holds
  the stack top **as loaded**, not the live one. A stalled `0x0224 = 0x1F` is therefore not
  in itself the symptom. The live local stack pointer was not identified: it reaches 0
  quickly, and the snapshots only list non-zero registers.
- Bank 1 (`…1`) at a geometry kick still holds the **previous** HWRTData's catalogue pages,
  which may already have been freed and reused for other data.

**2. Relation to HWRTData and freelists.** The catalogue pages come from the **local
freelist**, not from the HWRTData's own buffers. In this run, `pm_mlist_dev_addr`,
`vheap_table_dev_addr`, `macrotile_array_dev_addr` and `rgn_header_dev_addr` translate to
bus pages `0x84E86000`/`0x84E87000`, `0x84E83000`, `0x84E84000` and
`0x84E88000`/`0x84E89800`. These are outside the freelist and differ from every
`PM_CAT_BASE` value.

Example from the first kick (bus addresses; freelist entries are page numbers = bus address
>> 12, as the host writes them):

| Item | Value |
|---|---|
| local freelist base / stack top at load | `0x80_00AB_D000` / 31 |
| local freelist entries 0…31 | `0x84E63`, `0x84E64`, … `0x84E82` (consecutive at the first kick) |
| global freelist base / stack top at load | `0x80_0004_8900` / 447 |
| `PM_CAT_BASE_VCE0` | `0x0000_0000_84E8_0001` = entry 29 |
| `PM_CAT_BASE_TE0` | `0x0000_0000_84E7_5001` = entry 18 |
| `PM_CAT_BASE_ALIST0` | `0x0000_0000_84E7_0001` = entry 13 |
| `VCE1`, `TE1`, `ALIST1` | 0 (no fragment yet) |

**What the catalogue pages contain.** While the geometry runs:
- The first 32-bit word of each catalogue page is `(page number << 4) | 1`. The page number is
  another page of the local freelist: `0x0084E7F1` in the `VCE0` page points to page
  `0x84E7F`.
- Further entries in later snapshots are 64-bit, `bus address | 0x11`, and point to pages
  **outside** the local freelist (`0x8F796011`, `0x96F85011`, …). These are consistent with
  parameter-buffer pages from the global freelist (`TO CONFIRM`).
- The `ALIST1` page holds a list of 64-bit `bus address | 1` entries for local-freelist
  pages.

The high word of `PM_CAT_BASE_*1` (the public `INIT_PAGE` field, bits 59:40) was non-zero
only after partial renders (`0x3EF4`, `0x7DE8` in `VCE1`; `0xDD`, `0x1BA` in `TE1`).

**3. Global freelist stack pointer:** the live one is `0x20C8` (PM context 0, geometry) and
`0x2088` (PM context 1, fragment). It is loaded through `+0x08` of the bank; see the table
under Q16e above and Q16g below.

**4. `0x0D00` bits 4 and 5.** Not established. Observation only shows the following:
- At the first kick of this run, `0x0D00` read `0x04`, with the catalogue bases already
  valid; it then read `0x34`/`0x35` while the geometry kept running.
- The bits are therefore not a direct "PM has pages" indicator.

#### Loading the freelists into the PM (answers Q16g, O)

Four more `bigpb` runs on the open v1.1 image, sampled with a tight host loop (one sample
every 15–60 µs) instead of event snapshots:
- run 1: the two global banks, `0x0210`/`0x0224`, `0x0250`/`0x0258`, `0x02A0`, `0x02B0`,
  `0x0D00` and `PM_CAT_BASE_*0`;
- run 2: every register in `0x0200–0x02FC`;
- run 3: every register in `0x0300–0x03FC`;
- run 4: a mix of the above.

Only changes of value are visible. A register written with the value it already holds, or
written and cleared within one sample, leaves no trace.

**1. What fills the live stack pointers.**
- **Global freelist:** the `+0x08` register of the bank. On every load and every freelist
  update (hundreds per run), `+0x08` and the live `+0x10` change **to the same value in the
  same sample**, together with the base. Examples:
  - the first geometry load: `0x20B8` 0 → `0x48900`, `0x20C0` 0 → `0x1BF`, `0x20C8` 0 →
    `0x1BF` (trace `UFL-TA-Base … SP = 447`);
  - a fragment load: `0x2078` → `0x3E000`, `0x2080` → `0x20`, `0x2088` → `0x20` (trace
    `SP = 32`);
  - a grow/ready-pages update: `0x02A0` 0 → 1, then the base, `+0x08` and the live `+0x10`
    change together (`0x20C8` 7 → `0x47`), then `0x02A0` 1 → 0.

  The live register is read-only (writes to it have no effect). It takes the `+0x08` value
  shortly after `+0x08` is written. Q16h shows that this copy is a separate step and needs
  PM state that is set on the first load.
  The bank of **PM context 0 is at `0x20B8`**, not `0x2078`. `0x2078`/`0x2080`/`0x2088` are
  the fragment-side bank.
- **Local freelist:** the live stack pointer is **`0x0348` (PM context 0)** and **`0x0350`
  (PM context 1)**:
  - It counts down as the PM takes local pages: `0x1F` → `0x1B` within ~60 µs of the first
    geometry kick, then 2, 1, 0.
  - It counts back up as the fragment returns them: 4, `0xA`, `0x10`, `0x12`, `0x1F`.
  - It does not move while the global freelist is used.

  `0x0224` (loaded stack top, context 0) stays `0x1F` throughout. **Correction (Q16i):** after
  the first load, `0x0348` does not get back to `0x1F` through a reload. The fragment returns
  the local pages one by one, so it already reads `0x1F` when the next geometry load comes.
  Only the first load of a boot shows a real copy (`0x0348` 0 → `0x1F`, in the same sample as
  the `0x0224` write; see Q16i). At one fragment load,
  `0x0350` went `0x1F` → 0, matching the trace `FL-3D-Base … SP = 0`. The register that
  holds the loaded top for context 1 was not identified.
- **`0x0250`/`0x0258`:** never seen non-zero in any sample. If they are written, they clear
  within one sample. No load was seen to depend on them.

**2. Memory reads at load time.** None were found to be needed. The values above appear in
the registers in the same sample as the writes, and no header is involved: the freelist in
memory is only the host-written stack of page numbers (`pvr_free_list.c`). The PM reads that
stack when it takes pages (Q16f: catalogue pages are stack entries). It reads it through the
geometry's memory context, since the freelist base is a GPU virtual address in the
application's context. Whether the PM prefetches stack entries at load time cannot be seen
from the host (`TO CONFIRM`).

**3. State right after the geometry load, before the geometry starts** (first kick, run 4;
order in which values **changed**, not a list of writes):

| Sample (µs from the first change) | Change |
|---|---|
| 0 | `0x20B8` → `0x48900`, `0x20C0` → `0x1BF`, `0x20C8` → `0x1BF` |
| +64 | `0x02B0` → 3, `0x02D8` → this HWRTData's MLIST, **`PM_CAT_BASE_VCE0` → 0** |
| +121 | `0x0348` `0x1F` → `0x1B`, `PM_CAT_BASE_VCE0` → new valid page; geometry running |

In run 2, the first changes at a first load were:
- `0x0244` 0 → `0x80` (`0x0240/0x0244` = `0x80_0000_0000`, the base of the general heap;
  meaning `TO CONFIRM`);
- then `0x0210`/`0x0214`/`0x0224`;
- then `0x0248`/`0x024C`, `0x02B0` and `0x02D8`/`0x02DC`;
- then the ALIST/MLIST stack pointers (`0x0270`, `0x02C0`) and `0x02F8` start counting.

So, just before the geometry starts:
- `0x20C8` already equals the stack pointer printed by the trace;
- `0x0348` equals the local top (`0x1F`);
- `PM_CAT_BASE_VCE0` is 0.

The live global value is the `+0x08` value and the live local value the `0x0224` value; the
step that copies them is examined in Q16h.

#### PM state set on the first load after boot (answers Q16h, O)

Three `bigpb` runs on the open v1.1 image after fresh reboots, with register reads from the
host only:
- a full read of `0x0200–0x03FC`, `0x2000–0x20FC` and `0x1200–0x12FC` after GPU init (module
  loaded, GPU powered, no job yet), and again after the job;
- tight sampling of the **first** geometry load after boot, every ~12 µs;
- tight sampling (~6 µs) of `0x20C0`, `0x20C8`, `0x0250`, `0x0258`, `0x0224` and `0x0348`
  over a whole run.

**1. After GPU init, before any job.** Every register in `0x0200–0x03FC` and `0x2000–0x20FC`
reads 0, except:

| Register | Value after init |
|---|---|
| `0x0358` | `0x5` |
| `0x0388` | `0x1` |
| `0x03C0`, `0x03C8` | `0x7F` |
| `0x03E0` | `0x1` |
| `0x03F8` | `0xFF` |

They keep these values after the job. The host cannot tell whether they are reset defaults
or values the firmware writes at init. An implementation can compare them with its own idle read.

**The first geometry load after boot** changed values in this order (samples ~12 µs apart;
order of changes, not a list of writes):

| Sample | Change | Note |
|---|---|---|
| 0 | **`0x20F8` 0 → 1** | changes **only here**; stays 1 for the rest of the boot (also 1 after the job) |
| +56 µs | `0x0240/0x0244` 0 → `0x80_0000_0000` | |
| +69 µs | **`0x03D0` 0 → `0x100`, `0x03D4` 0 → `0x1`** | |
| +82 µs | `0x0210` → local base, `0x0224` 0 → `0x1F`, **live `0x0348` 0 → `0x1F`** | same sample |
| +93 µs | `0x20C0` 0 → `0x1BF`, **live `0x20C8` 0 → `0x1BF`** | same sample |
| +105 µs | `0x02B0` 0 → 3 | |

The three registers in bold that are not freelist registers, **`0x20F8`, `0x03D0` and
`0x03D4`**, are not in the per-kick list given earlier, and they are 0 after init. They are
the most likely missing setup (`TO CONFIRM` by writing them): they all change before the first
live copy.

They behave as follows over the rest of the run:
- **`0x20F8`:** 1 from the first load on; never changes again.
- **`0x03D0`:** bit 8 set at every geometry load. Bit 16 is set at every fragment load
  (`0x100` → `0x10100`) and cleared at the next geometry load (`0x10100` → `0x100`).
- **`0x03D4`:** the same pattern one byte lower: bit 0 at the geometry load, bit 8 added at
  the fragment load (`0x1` ↔ `0x101`).

  The pattern fits "PM context 0 / PM context 1 loaded" flags. Whether the firmware writes
  them or the PM sets them is not visible from the host.
- **`0x03F0` and `0x20F0`:** 0 → 3 some time after each fragment kick, and 3 → 0 at the next
  geometry load. They are status-like and change only after the fragment starts, so they are
  not a precondition for the geometry.
- **`0x02F8`:** 0 until the geometry runs, then counts up with the ALIST stack pointer. It is
  a consequence of allocation, not a precondition.

**2. The copy into the live registers is a separate step.** Over a whole run with ~6 µs
sampling, `0x20C0` changed 136 times:
- in 88 cases `0x20C8` took the new value in the same sample;
- in 48 cases one sample shows the new `+0x08` with the **old** live value, and the live
  register follows 5–13 µs later.

So writing `+0x08` does not by itself update the live register; a later step does, within
~13 µs. Which step it is was not identified.
- At freelist updates the whole sequence sits inside a `PM_PAGE_MANAGEOP` pause: `0x02A0` 0 →
  1 with `0x02A8` 0 → 1 in the same sample, then base, `+0x08` and live, then both back to 0.
- At the first load there is no pause visible, but `0x20F8` and `0x03D0`/`0x03D4` change
  first.

**3. Handshakes and the fault requester.**
- `0x0250` and `0x0258` read **0 in all ~296 000 samples** of the 6 µs run, across about 140
  loads and updates. If the original uses them, each pulse lasts less than one sample.
  Nothing here supports a protocol that waits several polls on them.
- The `TAG_SB`/`TAG_ID` requester encoding of `BIF_FAULT_BANK*_REQ_STATUS` is not in the
  public sources (`pvr_rogue_cr_defs.h` gives only the field positions), and no fault with
  that tag was observed with the original firmware. The unit is not identified (`PENDING`).

#### What happens around the live copy (answers Q16i, O)

More `bigpb` runs on the open v1.1 image, each started right after a reboot so that the
**first load of the boot** is captured. Register reads were done from the host only, in
three forms:
- a diff of every register in `0x0000–0x09FC`, `0x0C00–0x40FC` between the last idle read
  and the first read after the load (one full read takes ~0.5 ms);
- 5–37 µs sampling of 64-register blocks: `0x2000–0x20FC`, `0x0300–0x03FC`, and every
  register outside the PM ranges that the diff showed changing;
- 5–11 µs sampling of `0x20B8`/`0x20BC`/`0x20C0`/`0x20C8` with the public BIF, SLC, event
  and multicore status registers.

Not covered: `0x0A00–0x0BFF`, and offsets above `0x40FC` other than `0xF078–0xF338`. A
write that does not change a value, or a pulse shorter than one sample, is invisible.

**First load after boot, all ranges** (one sample ≈ 37 µs; time from the `0x20F8` change):

| Time | Changes |
|---|---|
| −40 µs | `0x18C0`/`0x18C8`/`0x18D0` `0x7FFFFF` → `0x7FC000` |
| 0 | `0x20F8` 0 → 1, together with `0x1498` → 1, `0x18E8` → `0xF`, `0x17B8` → `0xA`, `0x1500` → `0x80`, `0x1780` `0x40` → `0x21`, `0x1788` → `0x20`, `0x178C` → `0x20`, `0x0FA0` → `0x88`, `0x0C78` → `0x88` |
| +37 µs | **`ROGUE_CR_BIF_CAT_BASE1` (`0x1208`) → the application's page catalogue, `ROGUE_CR_BIF_CAT_BASE_INDEX` (`0x1240`) → `0x1`**, and the local live `0x0348` 0 → `0x1F` |
| +74 µs | global live `0x20C8` 0 → `0x1BF` |
| +111 µs | `0x0CE0` → 1, `0x17A0`/`0x17A4` (TPU border), `0x0C80`/`0x0C88`, `0x3840` |
| +172 µs | the geometry starts: `0x0420` → 1, `0x0408` (VDM stream base), `0x0D00` `0x3F` → `0x05`, `PM_CAT_BASE_VCE0` valid, `0x2000` 0 → 3, the unit clocks in `ROGUE_CR_CLK_STATUS` turn on |

How to read this:
- **`0x20F8` belongs to the GPU unit initialisation, not to the freelist load.** It changes
  in the same sample as the unit defaults of [boot-gpu-init](boot-gpu-init.md) (`0x1498`,
  `0x18E8`, `0x18C0…`, `0x17B8`).
  - On the original, that initialisation runs at the first kick, not at module load. The
    public trace shows the GPU being idled after boot (`GPU idle (might be powered
    down)`), and `GPU Units init` printed again right before the first geometry load.
  - Writing `0x20F8` at init time, as the spec's init table says, is therefore equivalent.
  - `0x1500`, `0x1780`, `0x1788`, `0x178C` and `0x0FA0` also change in that sample and are
    not in the init table (`TO CONFIRM` whether they belong to the initialisation or to the
    kick).
- **The memory context is bound no later than the local copy.** `CAT_BASE1` and
  `CAT_BASE_INDEX` change in the **same** sample as `0x0348`, and before `0x20C8`. The public
  trace gives the order at a first kick:
  1. memory-context activation and register-set setup;
  2. then the two `Load Freelist` lines;
  3. then `CONTEXT_PB_BASE`.

  Since a freelist base is a GPU virtual address in the application's memory context, an
  implementation should bind `CAT_BASE{n}` and `CAT_BASE_INDEX` (`TA` field) **before** it
  writes the PM freelist registers. `TO CONFIRM`: whether the live copy depends on it.
- `ROGUE_CR_SOFT_RESET` (`0x0100`/`0x0104`) and `ROGUE_CR_CLK_CTRL` (`0x0000`/`0x0004`) did
  not change during the load. In `ROGUE_CR_CLK_STATUS`, the PM clock (bit 4) still reads
  gated when `0x20C8` is copied; it turns on later, with the geometry.
- `ROGUE_CR_MULTICORE_GEOMETRY_CTRL_COMMON` (`0xF320`) stays **0** for the whole run
  (`MULTICORE_FRAGMENT_CTRL_COMMON` = 1, `MULTICORE_COMPUTE_CTRL_COMMON` = `0x101`).

**1. What changes with the late copies.** With 5 µs sampling of the geometry bank (136 bank
updates in one run):
- **The base (`+0x00`) always changes first**, then `+0x08`. The live `+0x10` takes the
  `+0x08` value in the same sample as `+0x08` or one sample (5 µs) later.
- When `+0x08` keeps its value, the live register still reloads to it in the same sample
  as the base change or one sample later (91 + 34 cases).
- With `0x02A0`/`0x02A8`, `0x12A0`, `0x3820`, `0x1350`, `0x1358`, `0x1330`, `0x1338`,
  `0x0130`, `0x2000` and `0xF310–0xF330` sampled every ~11 µs, **none of them changed between
  the `+0x08` write and the late live copy**. At the first load of that run the gap was 61 µs,
  with no change in any of them.
- At freelist updates the copy happens while `0x02A0` = 1 and `0x02A8` = 1; at the first load
  there is no pause.

The copy is visible only as the live value changing. No other state change goes with it in
the ranges read.

**2. `0x03D0`/`0x03D4` vs `0x0348`.**
- In both first loads captured with these registers, `0x03D0`/`0x03D4` changed **before**
  `0x0348` (13 µs and 90 µs earlier), never in the same sample.
- `0x0224` changed in the same sample as `0x0348`.
- After the first load the local live value is not reloaded (see the correction in Q16g),
  so later loads give no further comparison.

The order fits `0x03D0`/`0x03D4` being written before the freelist registers, not a status
set after the copy, but the host cannot tell a write from a hardware update.

**3. Invalidations and flushes.**
- `ROGUE_CR_BIF_CTRL_INVAL` (`0x12A0`) was non-zero in **one** sample of a whole run, not
  near any load.
- `ROGUE_CR_SLC_STATUS0` (`0x3820`) showed `FLUSH_INVAL_PENDING` in 110 samples. None fell
  between a `+0x08` write and its live copy.
- `ROGUE_CR_BIFPM_STATUS_MMU` (`0x1350`, outstanding PM MMU requests) stayed 0 until
  **after** the live copy. Its first non-zero values come when the PM starts taking pages.

So the copy needs no PM memory access and no flush or invalidate visible at this
resolution.

**4. Trace at the first load of a boot** (public strings; `group id` from
`pvr_rogue_fwif_sf.h`), in order, between the KCCB kick and `Kick TA`:

| Trace line | Group, id |
|---|---|
| `cCCB Woff update = 200, DM = 2, …` | — |
| `Ready-to-run debug OSid = 0, DM = 2, …`, `Client command header DM = 2, …` | — |
| `PM running primary config (Core 0)` | PM 30 |
| `GPU Units init (# mask: 0x1)` | MAIN 69 |
| `Cancel power off request …` | POW 18 |
| `Activate MemCtx=… DM=2 secure=0` | BIF 1 |
| `Setup register set=-1 DM=2, PC address=…, NewPCRegRequired=1` | BIF 19 |
| `Alloc PC set 0 as register range [1 - 1]` | BIF 20 |
| `Grab reg set 0 refcount now 1` | BIF 4 |
| `Load  Freelist … type: 0 … stacktop = 0x1f …` (local) | RTD 17 |
| `Load  Freelist … type: 1 … stacktop = 0x1bf …` (global) | RTD 17 |
| `CONTEXT_PB_BASE set to 0x3, FL different between TA/3D: local: 1, global: 1` | RTD 3 |
| `TA RTData … loaded on HW context 0` | RTD 15 |
| `TA Buffers: …`, `TE Region headers base … (RGNHDR Init: 1)` | RTD 25, RTD 24 |
| `Is TA: 1, finished: 0 on HW 0 …` | PM 2 |
| `UFL-TA-Base: … (SP = 447 …), FL-TA-Base: … (SP = 31 …)` | PM 4 |
| `ALIST0 SP = 0, MLIST0 SP = 0 (VCE 0x0, TE 0x0, ALIST 0x0)` | — |
| `Kick TA: … First kick:1 …` | MAIN 7 |

`PM running primary config (Core %d)` is printed before **every** PM operation: here,
before each OOM handling, and between `PM pause TA ALLOC` and `PM unpause TA ALLOC` at every
freelist update. Its sibling is id 31, `PM running secondary config (Core %d)`. This points
to a "select which PM configuration is being programmed" step before PM registers are
touched. No register change goes with it in the ranges read (`PENDING`: it may be a write
of an unchanged value, or a register in an unsampled range).

**Summary for an implementation still stuck at live = 0:**
1. Bind the memory context (`CAT_BASE{n}` = page catalogue, `CAT_BASE_INDEX`) before the PM
   writes.
2. Write `0x03D0`/`0x03D4` before the local freelist.
3. Write each bank **base first, then `+0x08`**. Write the local base and `0x0224`.
4. Then look for the copy within ~60 µs.

If it still does not copy, the remaining unobserved candidates are:
- the "PM configuration select" step behind trace PM 30;
- the `0x0A00–0x0BFF` range;
- writes of unchanged values.

#### Q16j: what the observation can and cannot give (O, P)

**The ordered write list is not provided.** Q16j asks for the original's sequence of register
writes and polls between trace PM 30 and `CONTEXT_PB_BASE`, including writes of unchanged
values. Such a list can only come from the original's code and would be a transcription of
its routine, which the clean-room rules exclude (see Q16e, item 4). What follows is what
black-box work and public sources give.

**1. Decoding the fault at address 0 (P).** The request-status layout and tag tables are in
the public vendor kernel driver for this SoC: the SpacemiT `linux-6.6` tree, `img-rogue`,
dual MIT/GPLv2, files `km/rgx_cr_defs_km.h`, `km/rgxmhdefs_km.h` and the fault decoder in
`rgxdebug.c`.
- Its build configuration for BVNC `36.V.52.182` enables the "XE memory hierarchy" feature,
  so the `__XE_MEM__` field layout of `BIF_FAULT_BANK*_REQ_STATUS` applies: `TAG_SB` = bits
  51:46, `TAG_ID` = bits 45:40.
- `0x0011B000_00000000` therefore decodes as `TAG_ID` = `0x30` and `TAG_SB` = `0x6`. In that
  driver's tables this is the **PM allocation unit** (`0x30`; `0x31` is the PM deallocation
  unit), side-band `0x6`, labelled `MAVP`. The other PM allocation side-bands in the same
  table are: 0 TA free stack, 1 TA MList, 2 3D free stack, 3 3D MList, 4 PM context 0,
  7 UF stack, 8 TA MMU stack.
- So the read at address 0 is the **PM allocator's first memory access**, of the `MAVP`
  kind, made with a zero base. It is not a freelist (free-stack) read.

**2. A hardware probe on the original's own state (O).** The original firmware was loaded and
the GPU powered, with no job submitted yet. The host then wrote PM registers directly:
1. `0x20B8`/`0x20BC` (base) and `0x20C0`: the live `0x20C8` **stayed 0**, immediately and
   1 ms later.
2. `0x0210`/`0x0214`/`0x0224`: the live `0x0348` **stayed 0**.
3. The same writes after `0x03D0 = 0x100`, `0x03D4 = 1`: still 0.
4. The same writes inside a `PM_PAGE_MANAGEOP` pause (`0x02A0` 1 … 0): still 0.

So the hardware behaves exactly as the clean implementation reports. Writing the freelist
registers does not load the live stack pointers, even on a GPU the original firmware has
initialised. The missing action is real; it is not a difference in idle state.

This probe also produced **two MMU faults with the same request status as the one reported for the handshake-around variant**
(`0x0001B000…`/`0x0011B000…`, PM allocator, `MAVP`). The firmware ran its recovery each
time.
- `ROGUE_CR_BIF_FAULT_BANK0_MMU_STATUS` read `0x21`: fault bit set, catalogue field 0. The
  access went through catalogue 0, because no application context was bound yet, at
  address 0.
- So **some write in steps 1–4 started the PM allocator**. Its first access is the `MAVP`
  read, and it used a zero base because the probe had not written VHeap (`0x0248`) or MLIST
  (`0x02D8`).
- Which single write started it was not isolated. The probes were stopped there, so as not
  to cause more recoveries.

This narrows the problem:
- The PM allocator **can be started by register writes alone**. The fault reported for the handshake-around variant shows the
  clean implementation also starts it, at least in the handshake-around variant.
- When it starts, it reads `MAVP` data first. With a valid base and a bound memory context
  that read succeeds, and only then would it pop the free stacks.
- An implementation where the allocator starts but the live stack pointers stay 0 and
  **no fault occurs** has started it with a valid base. The copy into the live registers
  happens at a different point.

Also seen in the probe: `0x0250` written with 1 reads back 1 and returns to 0 by itself
within 1 ms, with no job present. The original never showed it non-zero at 5 µs sampling
(Q16h).

**3. What remains `PENDING`.** Which action makes the PM copy `+0x08`/`0x0224` into the live
stack pointers. Observation cannot see it: it leaves no value change, and probing it by
writes causes faults. The spec maintainer will not answer it with a write list from the
original's code. A functional statement of the trigger, without a sequence, is a decision
for the project owner.

#### Experiments on PM loading, freeing and storing (answers Q16k, O)

Eight `bigpb` runs and one `xfer` run on the open v1.1 image, after a fresh reboot. All reads were done from the host, with **no register writes** (the write probes of
Q16j caused recoveries and were not repeated). No run had a recovery. Each experiment tested
one expected behaviour.

**Experiment A: load-command pulses.** Purpose: catch the command registers that would
make the PM copy loaded values into its live state.
- Candidates: `0x0190`, `0x0200`, `0x0208`, `0x0288`, `0x20A8`, `0x20B0`.
- Method: sampled together over a whole run, then `0x20B0` and `0x0190` each alone in the
  tightest host loop (≤ 3 µs per read, an upper bound measured with printing on).
- **Result:** all read **0 in every sample**. Any write of 1 to them clears in less than one
  host read. This is consistent with self-clearing commands, but the observation cannot
  confirm their role (`TO CONFIRM` by implementation).

**Experiment B: ALIST/MLIST stack pointers of PM context 1** (`0x0268`–`0x02D0`, `0x03D0`,
`0x03D4`, `0x0388`, `EVENT_STATUS`, `0x0348`, `0x0350`; ~10 µs per sample).

At every fragment load, in this order:
1. `0x03D0` gets bit 16 (`0x100` → `0x10100`) and `0x03D4` gets bit 8 (`0x1` → `0x101`).
   At every geometry load both bits are cleared again.
2. In the same or the next sample, `0x0278` (ALIST, 64-bit with `0x027C`) and `0x02C8` (MLIST) take the
   values printed by the trace line `Loading stack-pointers for 1 (0:MidTA,1:3D) on context
   1, MLIST = …, ALIST = …` (for example `0x5F14`/`0x68`). In the **same** sample `0x0280`
   and `0x02D0` take the same values.

So `0x0278`/`0x02C8` are the values to load for context 1, and `0x0280`/`0x02D0` are the live
copies.

Context 0 works the same way:
- At every new geometry load, the live `0x0270` (ALIST) and `0x02C0` (MLIST) drop to **0**,
  in the sample after the `0x03D0`/`0x03D4` bits are cleared.
- `0x0268`/`0x026C` and `0x02B8` read 0 for the whole run.

This fits a context-0 load of zero stack pointers at each first geometry kick. The value
registers hold 0, so their write is invisible.

**Experiment C: what the store reads** (`0x0348`, `0x0350`, `0x03A0`, `0x03A8`, `0x2000`,
`0x2008`, the global banks, `0x0390–0x039C`, `0x0220–0x0234`).
- **Live local counts:**
  - `0x03A0` (context 0) and `0x03A8` (context 1) rise to **20**;
  - `0x2000` (context 0) and `0x2008` (context 1) rise to **11**;
  - these are the trace's local `4PB = 20`, `4PT = 11`.
  - `0x2000` is the register seen going `0 → 3 → 9` at the first kick (Q16i): it is the live
    local MMU-page count of context 0.
- **Load registers:** they stay constant over the run, as load registers should:
  - `0x0390`/`0x0394` = 0/0 (context-0 counts at a first kick);
  - `0x0398`/`0x039C` = 20/11 (context-1 counts loaded at the fragment kick);
  - `0x0220`/`0x0224` = 0/`0x1F` (context-0 local top, in the high word);
  - `0x0230`/`0x0234` = 0/0. This matches the fragment loads in this workload, which carry a
    local top of 0; a 64-bit layout with the top from bit 22 is `TO CONFIRM`.
- **End of the run:** the four final `Store Freelist` trace lines (local and global, PM
  contexts 0 and 1) printed stack tops `0x1F`/`0xFFFF` with counts 0. These equal the live
  registers at that time:
  - local: `0x0348`/`0x0350` with `0x03A0`/`0x03A8` and `0x2000`/`0x2008`;
  - global: `0x20C8`/`0x2088` with `0x20D8`/`0x2098` and `0x20E0`/`0x20A0`.

  No register outside these changed at the stores, so no separate store command was seen.

**Experiment D: `0x0388` around a transfer** (`xfer`, then `bigpb`).
- `0x0388` stays **1** for a whole `bigpb` run.
- It goes 1 → 0 when the transfer job runs, and 0 → 1 at the next fragment kick of a render.

So the fragment kick sets `0x0388` back to 1 after a transfer cleared it.

**Experiment E: the end-of-render events** (`EVENT_STATUS` alone, tight loop). For every
render, 13 out of 13:

| Time | `EVENT_STATUS` |
|---|---|
| t | `PIXELBE_END_RENDER` (bit 3) set |
| t + 0.4 … 2.5 ms | **`PM_3D_MEM_FREE` (bit 8)** set |
| + 3–7 µs | bit 8 cleared |
| + ~35 µs | bit 3 cleared |

`ZLS_FINISHED` (bit 14) was also set in 6 of the 13 renders.

**Experiment F: what happens between the two events** (`EVENT_STATUS`, `0x0350`, `0x2088`).
At `PIXELBE_END_RENDER`:
- the local live stack pointer of context 1 still reads **`0x10`–`0x12`**;
- the global one reads the render's level (for example `0x2818`).

Both then **climb back to the full stack**: `0x0350` → `0x1F`, and `0x2088` → `0x2BFF` /
`0xFFF7` / `0xFFFF`, which is total − 1, minus the pages still held. They finish **just
before** `PM_3D_MEM_FREE`, 0.44–2.5 ms after the end of the render; the time grows with the
number of pages to return.

So, after the pixel back end finishes:
- the **PM returns the render's pages to both freelists by itself**;
- it signals completion with `PM_3D_MEM_FREE`;
- the firmware clears that event and only then the end-of-render event.

No register write by the firmware was visible during the freeing.

**What this means for an implementation where `0x0350` stops at `0x10` after the render.** The
PM did not free the pages. Experiments B and F together show that freeing follows from the
context-1 ALIST/MLIST stack pointers being **live** (`0x0280`/`0x02D0` equal to the loaded
values) when the render ends. Writing `0x0278`/`0x02C8` alone is not enough. Experiment A
cannot see the step that makes them live.

- **PM handshakes** (**D**, not observed directly):
  - `CR[0x250] = 1`, then **poll** `CR[0x250]` until bit 0 = 1;
  - `CR[0x258] = 1`, then **poll** `CR[0x258]` until bit 0 = 0.

  The polls must complete before phase C.

### C. Program the geometry registers (command data)

**Observed (O)**, for a first kick. Each command field is matched by value with the register
that held the same value:

| Register (geometry core bank) | Observed value | Source |
|---|---|---|
| `0x0C88` | `0x202` | `rogue_fwif_geom_regs.ppp_ctrl` |
| `0x0C28` | `0x0002_0000` | `rogue_fwif_geom_regs.te_psg` |
| `0x0C38`/`0x0C3C` | `0x80_0013_E000` | `rogue_fwif_hwrtdata.rgn_header_dev_addr` (trace "TE Region headers base") |
| `0x0C40`/`0x0C44` | `0x80_0013_B000` | `rogue_fwif_hwrtdata.tail_ptrs_dev_addr` (trace "TPC") |
| `0x0408`/`0x040C` | `0x80_0013_F000` | `rogue_fwif_geom_regs.vdm_ctrl_stream_base` |
| `0x17A0`/`0x17A4` | `0x20_0001_C400` | `rogue_fwif_geom_regs.tpu_border_colour_table` (the fragment kick writes the same register) |

The other non-zero values in the geometry bank are analysed in "Unmatched geometry
registers" below. `0x0410`/`0x0414` held `vdm_ctrl_stream_base` plus a small offset (`+0x40`,
`+0x3C`) after each job, which reads like the VDM's current position in the control stream
(`TO CONFIRM`).

#### Unmatched geometry registers (answers Q16c, O)

The `bigpb` run renders to a 1024×1024 target, against 256×256 for the draw. Comparing the
two separates the values that depend on the target size from the constant ones:

| Register | 256×256 | 1024×1024 | Reading |
|---|---|---|---|
| `0x0C98` | `0x00FF_00FF` | `0x03FF_03FF` | **x/y maximum in pixels** (width − 1, height − 1) |
| `0x0C18` | `0x0000_F00F` | `0x0003_F03F` | **x/y maximum in 16×16 tiles** (tiles − 1) |
| `0x0C08`, `0x0C10` | `0x0010_0000` | `0x0040_0000` | scales ×4 when each side grows ×4 (a per-row size or stride; `TO CONFIRM` which `hwrtdata_common` field) |
| `0x0C20` | `0x10` | `0x100` | scales ×16, with the area (`TO CONFIRM`) |
| `0x0C30`, `0x0C34` | `2`, `3` | same | constant |
| `0x0C48` | `8` | same | constant |
| `0x0C70` | `3` | same | constant |
| `0x0C78`, `0x0C80` | `0x88` | same | constant |
| `0x0C90`/`0x0C94` | `0x3089_705F` ×2 | same | constant 64-bit value (candidate: `multi_sample_ctl`, which is 64-bit and fixed per sample count; `TO CONFIRM`) |
| `0x0CA0` | `0x10` | same | constant |
| `0x0CE0` | `1` | same | constant |
| `0x0D00` | `0x3F` | 0…`0x3F`, varying | hardware status that changes during the job, not configuration |
| `0x0420` | `1` | 2, 3, … 9 | increments by one at each geometry kick (a running count) |
| `0x0418`/`0x041C` | `0x80_0006_D000` | same | **`rogue_fwif_geom_ctx_state_per_geom.geom_reg_vdm_call_stack_pointer_init`** of the context's `context_state_addr` buffer (equal by value) |

"Constant" means the value did not change across the two targets. The analysis does not
show whether the firmware writes it as a constant or copies it from a field that happened
to be equal in both runs.

`vdm_context_resume_task0_size` (`0x2001_0000` in this job) was **not** found in any
sampled register. It is probably used only when resuming a stored context (`TO CONFIRM`).
`tpu`, `pds_ctrl` and `view_idx` were 0 in this job, so their registers could not be
identified by value.

> **Correction.** Earlier versions of this phase had a table derived from firmware analysis
> that placed `te_psg` at `0xC18`, `tpu` at `0xC20`, `pds_ctrl` at `0xC10`, the resume sizes
> at `0xC00`/`0xC08`, `tpu_border_colour_table` at `0xC80`, the VDM control-stream base at
> `0xC88…0xC98`, and listed `0xC28`, `0xC38` and `0xC40` as context state or polls. Where
> that table and the observation disagree, the **observation above wins**: `0xC28` is
> `te_psg`, `0xC38`/`0xC40` are the region-header and TPC addresses, `0xC88` is `ppp_ctrl`,
> and the VDM control-stream base is at `0x0408`. Rows that are neither confirmed nor
> contradicted are dropped. Trace: BIF id 19 `Setup register set=… DM=…`, RTD id 25/33
> `TA Buffers: …`, RTD id 24 `TE Region headers base …`.

### D. Context-switch state and lists

- A **new kick** and a **resume** (after preemption) program the TA *state buffer* and the
  stack pointers differently: `CR[set+0xCE0]`, `CR[set+0xCB0]` (poll),
  `CR[set+0xCB8] = 1`, `CR[set+0xC48]`, and `CR[0xD20]` = state buffer address (0 if
  none).
- On **resume**, the TA state buffer is **flipped** relative to the last store.
- After an **HWR**, the RTC cache and the TA caches are cleared before the kick
  (`rogue_fwif_hwrtdata.geom_caches_need_zeroing`).
- `CR[0x40A8] = (primary core) & 1` (core/TE selection).

**Context-state registers (Q16d, O — partial).** For the draw and `bigpb` runs, the
render context's `static_render_context_state.ctxswitch_regs[0]` (at render-context offset
`0x130`, public layout) held:
- `geom_reg_vdm_context_state_base_addr = 0x80_0006_E000`;
- `geom_reg_ta_context_state_base_addr = 0x80_0006_F000`;
- the `geom_state[]` store/resume task words.

**None of these values was found in any register sampled after a job.** `0x0CB0`, `0x0CB8`,
`0x0428` and `0x0D20` read 0, and `0x0CE0` read 1. So the observation neither confirms nor
contradicts `0x0CB0 = geom_reg_ta_context_state_base_addr` or `0x0428 =
geom_reg_vdm_context_state_base_addr`: either these registers are only meaningful during the
job, or they are cleared at its end. Settling it needs a view of the writes during the kick
(`PENDING`).

### E. Enable the context and broadcast the configuration

- `CR[set+0xC68] = 1`, then **poll** `CR[set+0xC68]` until bit 0 = 1 (context
  acceptance). Must complete before the trigger.
- `0x0408`/`0x040C` receive the VDM control-stream base (`vdm_ctrl_stream_base`, observed
  in phase C), not a freelist/ALIST configuration as earlier versions said. The pair
  `0x0418`/`0x041C` holds `geom_reg_vdm_call_stack_pointer_init` (see Q16c above).

### F. Trace, timing and watchdog (before the trigger)

- Trace entry MAIN id 119: `Kick TA: FWCtx … @ …, RTD …, First kick:…, Last kick:…, CSW
  resume:…. (PID:…, prio:…, frame:…, ext:…, int:…)`.
- Read `ROGUE_CR_TIMER` (`0x0160`), arm the DM's **deadline watchdog** (see
  [hardware-recovery](hardware-recovery.md)) and record the DM's execution state
  (context, offset, kick time, deadline).

### G. Trigger

- **`CR[set+0x400] = 1`**: the TA *run* register. It is **the last write**: everything
  above must be programmed and accepted before it.

## Completion

The GPU signals in `ROGUE_CR_EVENT_STATUS` (HW2/IRQ). Geometry completion handling (trace
MAIN id 8, `TA finished`) updates the HWRTData and freelist, flushes the TPC, retires the
command and processes the updates (see [client-ccb](client-ccb.md) and
[scheduler](scheduler.md)).

**Which events reach the firmware (part of Q19).** The boot routing for DM 2
(`ROGUE_CR_MTS_DM2_INTERRUPT_ENABLE = 0xA0`, see [boot-gpu-init](boot-gpu-init.md)) enables
only `TA_FINISHED` (bit 5) and `PM_OUT_OF_MEMORY` (bit 7). `TA_TERMINATE` (bit 6, public
name) is **not** routed to the IRQ task, so a firmware that follows this routing completes
geometry on `TA_FINISHED` alone. When the hardware raises `TA_TERMINATE` is not
established by this spec.

**Freelist write-back (part of Q19, O).** The freelist state reached by the geometry is
captured when the freelists are **stored from PM context 0 at the fragment kick** of the same
HWRTData (phase B, step 3), not at `TA finished`. The fields involved are the ones the trace
prints: stack top, current pages, allocated page count. Geometry completion itself is traced
as `TA finished` then `TA RTData … finished on HW context 0`.

`PENDING`: the registers the PM state is read back from, the HWRTData fields updated after
the TA, the context registers of phase D, and the full geometry variant of the completion
cache flush.
## Confidence and pending items

- **High confidence:** the phases and their dependencies (preconditions → PM → register
  set → CSW → enable → watchdog → **trigger last**), the listed offsets and the mandatory
  polls. The `rogue_fwif_geom_regs` field → register mapping in Phase C (Q4) is resolved
  above.
- The VDM control pair (`CR[set+0xC88]`…`+0xC98`) mixes the host `vdm_ctrl_stream_base`
  with firmware context state; the exact split of the 64-bit base across `+0xC90`/`+0xC98`
  is a detail the implementer can finish from the public field widths.

The 3D, transfer and compute kicks follow the same skeleton. See
[kick-compute-and-3d](kick-compute-and-3d.md).
