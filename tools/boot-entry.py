#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Thalles Campagnani
"""Reboot the K1 board and pick a GRUB menu entry through the debug serial.

usage: boot-entry.py <tty> <entry index (0-based)> [ssh target]
"""
import subprocess
import sys
import time

import serial

tty, idx = sys.argv[1], int(sys.argv[2])
target = sys.argv[3] if len(sys.argv) > 3 else None
s = serial.Serial(tty, 115200, timeout=0.1)
if target:
    subprocess.run(["ssh", *sys.argv[4:], target, "sudo systemctl reboot"], check=False)
buf = b""
t0 = time.time()
while time.time() - t0 < 180:
    buf += s.read(4096)
    if b"Debian 13 - Linux" in buf and (b"highlighted" in buf or b"automatically" in buf or b"Use the" in buf):
        break
else:
    sys.exit("GRUB menu not seen")
time.sleep(1.0)
for _ in range(idx):
    s.write(b"\x1b[B")
    time.sleep(0.3)
time.sleep(0.5)
s.write(b"\r")
log = b""
t0 = time.time()
while time.time() - t0 < 120:
    log += s.read(4096)
    if b"login:" in log:
        break
sys.stdout.write(log.decode(errors="replace")[-3000:])
