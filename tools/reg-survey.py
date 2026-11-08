#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Thalles Campagnani
"""Run ON THE BOARD as root with powervr loaded: black-box survey of the RGX
register bank through /dev/mem.

  reg-survey.py read  <start> <end>          print every non-zero 64-bit register
  reg-survey.py probe <start> <end> [skip..] write a pattern, read back, restore;
                                             prints the writable mask of each register

The GPU power domain is forced on first (runtime PM control = on): reading the
bank with the domain off hangs the SoC.  `probe` writes only bits 63:4 of each
register (bit 0..3 untouched) to avoid hitting trigger bits, and always restores
the original value.  Offsets in `skip` (hex) and the built-in DENY list are never
written.
"""
import mmap
import os
import struct
import sys
import time

BASE = 0xCAC00000
SIZE = 0x80000
DEV = "/sys/devices/platform/soc/soc:multimedia-bus/cac00000.gpu/power"

# Registers that must never be written by the probe: resets, MIPS wrapper,
# MTS, BIF/MMU, IRQ, firmware scratch, SLC control, known triggers.
DENY_RANGES = [
    (0x0000, 0x0200),   # clocks, IDs, soft reset, events, timer
    (0x0470, 0x0480),   # CDM kick trigger area
    (0x0800, 0x0C00),   # MIPS wrapper, META slave port, MTS, IRQ
    (0x0F00, 0x0F10),   # ISP_RENDER trigger
    (0x1200, 0x1440),   # BIF / MMU
    (0x1A00, 0x1AA0),   # scratch (firmware debug + OS scratch)
    (0x3000, 0x3800),   # FWCORE
    (0x3800, 0x3A00),   # SLC control
    (0xA000, 0xA200),   # BIF_TRUST, SYS_BUS_SECURE
    (0xF000, 0x10000),  # broadcast / multicore
]


def ensure_powered():
    if open(DEV + "/control").read().strip() != "on":
        open(DEV + "/control", "w").write("on")
    for _ in range(50):
        if open(DEV + "/runtime_status").read().strip() == "active":
            return
        time.sleep(0.1)
    sys.exit("GPU not powered (runtime_status != active), refusing to touch registers")


def denied(off, extra):
    return off in extra or any(a <= off < b for a, b in DENY_RANGES)


def main():
    mode, start, end = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 16)
    extra = {int(x, 16) for x in sys.argv[4:]}
    ensure_powered()
    rw = mode == "probe"
    fd = os.open("/dev/mem", (os.O_RDWR if rw else os.O_RDONLY) | os.O_SYNC)
    prot = mmap.PROT_READ | (mmap.PROT_WRITE if rw else 0)
    m = mmap.mmap(fd, SIZE, mmap.MAP_SHARED, prot, offset=BASE)

    def r64(o):
        lo, hi = struct.unpack_from("<II", m, o)
        return hi << 32 | lo

    def w64(o, v):
        struct.pack_into("<I", m, o, v & 0xFFFFFFFF)
        struct.pack_into("<I", m, o + 4, v >> 32)

    for off in range(start, end, 8):
        if mode == "read":
            v = r64(off)
            if v:
                print(f"{off:05x} {v:016x}", flush=True)
            continue
        if denied(off, extra):
            continue
        orig = r64(off)
        pat = 0xFFFFFFFFFFFFFFF0 | (orig & 0xF)
        w64(off, pat)
        got = r64(off)
        w64(off, orig)
        back = r64(off)
        mask = got & 0xFFFFFFFFFFFFFFF0
        if mask or orig:
            flag = "" if back == orig else f" RESTORE-MISMATCH {back:016x}"
            print(f"{off:05x} orig={orig:016x} mask={mask:016x}{flag}", flush=True)


if __name__ == "__main__":
    main()
