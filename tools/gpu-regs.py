#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Thalles Campagnani
"""Run ON THE BOARD as root while the GPU is powered: sample RGX registers
through /dev/mem (debug scratch registers written by the firmware, MIPS
wrapper status).  usage: gpu-regs.py <seconds> [interval]
env: EXTRA_REGS="0610 1a30" (hex offsets), ONLY_EXTRA=1, KMSG=1 (copy each line to
/dev/kmsg so that it reaches the serial console even if the SoC hangs)"""
import mmap
import os
import struct
import sys
import time

BASE = 0xCAC00000
REGS = [("STAGE", 0x1A00), ("REFILLS", 0x1A08), ("LASTVA", 0x1A10), ("EXC_CAUSE", 0x1A18),
        ("EXC_EPC", 0x1A20), ("EXC_BADVA", 0x1A28), ("MIPS_EXC_ST", 0x08D0),
        ("REMAP_UNMAPPED", 0x0868), ("REMAP_UNMAPPED_HI", 0x086C), ("WRAP_IRQ_ST", 0x08A8),
        ("SOFT_RESET", 0x0100), ("EVENT_ST", 0x0130), ("MTS_SCHED_EN", 0x0BC8),
        ("MTS_BGCTX", 0x0BA0), ("MTS_INTCTX", 0x0B98)]

for extra in os.environ.get("EXTRA_REGS", "").split():
    REGS.append((extra, int(extra, 16)))
if os.environ.get("ONLY_EXTRA"):
    REGS = [r for r in REGS if r[0] in os.environ["EXTRA_REGS"].split()]
KMSG = open("/dev/kmsg", "w") if os.environ.get("KMSG") else None
dur = float(sys.argv[1]) if len(sys.argv) > 1 else 5
step = float(sys.argv[2]) if len(sys.argv) > 2 else 0.5
DEV = "/sys/devices/platform/soc/soc:multimedia-bus/cac00000.gpu/power"


def powered():
    """Reading RGX registers with the GPU domain off hangs the SoC bus."""
    try:
        if open(DEV + "/control").read().strip() != "on":
            open(DEV + "/control", "w").write("on")
        return open(DEV + "/runtime_status").read().strip() == "active"
    except OSError:
        return False


fd = os.open("/dev/mem", os.O_RDONLY | os.O_SYNC)
m = mmap.mmap(fd, 0x80000, mmap.MAP_SHARED, mmap.PROT_READ, offset=BASE)
t0 = time.time()
last = None
while time.time() - t0 < dur:
    if not powered():
        time.sleep(step)
        continue
    vals = tuple(struct.unpack_from("<I", m, off)[0] for _, off in REGS)
    if vals != last:
        line = f"{time.time() - t0:6.2f}s " + " ".join(f"{n}={v:08x}" for (n, _), v in zip(REGS, vals))
        print(line, flush=True)
        if KMSG:  # also to the kernel log -> serial console, survives an SoC hang
            KMSG.write("gpuregs: " + line + "\n")
            KMSG.flush()
        last = vals
    time.sleep(step)
