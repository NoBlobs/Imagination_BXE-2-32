<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Thalles Campagnani -->

# E002 — C interrupt handlers must run with `EXL = 0`

**Question.** After boot worked, the firmware died at the first timer interrupt. The
scratch registers showed an exception with a meaningless EPC. Why?

**Set-up.** The first version of `int_common` called `irq_dispatch()` with
`Status.EXL = 1`, as the hardware leaves it on interrupt entry. The general exception
handler stored Cause, EPC and BadVAddr in scratch `0x1A18`/`0x1A20`/`0x1A28`.

**Observation.**
- Cause.ExcCode was **TLBS/TLBL** for an address in the firmware heap (data or code).
- EPC pointed into the interrupted code, not into the handler.
- Background (MIPS32 PRA): when `EXL = 1`, a TLB miss does **not** use the refill
  vector (`+0x000`). It goes to the general vector (`+0x180`), and EPC is not updated.
  Our refill handler, which maps the heap on demand, was never reached.

**Change.**
- `int_common` saves EPC and Status, clears `EXL/ERL/IE`, and only then calls C. It
  restores Status with EXL set before the `ERET`.
- `general_main` does the same before it calls `fatal_exception()`.

**Conclusion.** Every C function may touch unmapped heap pages, so C must never run
with `EXL = 1` on this firmware. This is not stated in the spec. It follows from the
dynamic-TLB design together with the MIPS architecture.

**Clean-room note.** Public MIPS architecture documents and our own firmware only.
