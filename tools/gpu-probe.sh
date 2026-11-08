#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Thalles Campagnani
# Run ON THE BOARD: load powervr while sampling GPU registers, then report.
# Refuses to keep running if the kernel loaded any firmware other than ours
# (the clean-room build reports "FW version v1.0 (build 1 OS)").
sudo dmesg -C
sudo modprobe -r powervr 2>/dev/null
( sleep 0.3; sudo python3 /tmp/gpu-regs.py ${SAMPLE_S:-4.5} 0.25 ) &
sudo timeout 60 modprobe powervr exp_hw_support=1 ${PVR_ARGS:-}
echo "modprobe exit: $?"
if sudo dmesg | grep -q "FW version" && ! sudo dmesg | grep -q "FW version v1.0 (build 1 OS)"; then
	echo "!!! foreign firmware loaded, unloading powervr"
	sudo modprobe -r powervr
	sudo dmesg | grep -i firmware
	exit 1
fi
wait
sudo dmesg | tail -30
