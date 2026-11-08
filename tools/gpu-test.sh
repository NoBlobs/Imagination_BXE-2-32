#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Thalles Campagnani
# Run ON THE BOARD: load powervr by hand (it is blacklisted at boot) and show
# what happened (kernel log, firmware trace).
set -u
sudo dmesg -C
sudo timeout 60 modprobe powervr exp_hw_support=1 ${PVR_ARGS:-}
echo "modprobe exit: $?"
sleep 1
sudo dmesg | grep -v "^$" | tail -60
D=$(ls -d /sys/kernel/debug/dri/*/ 2>/dev/null | while read d; do [ -e "$d/trace_mask" ] && echo "$d"; done | head -1)
if [ -n "$D" ]; then
	echo "--- debugfs: $D"
	sudo cat "$D/trace_mask" 2>/dev/null
	sudo cat "$D/trace_0" 2>/dev/null | tail -60
fi
