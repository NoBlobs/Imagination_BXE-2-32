#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Thalles Campagnani
# Run ON THE BOARD: install /tmp/rogue_36.29.52.182_v1.fw, reload powervr, run one draw
# case while sampling registers, then print the samples and the key trace lines.
# env: REGS (hex offsets), SAMPLE (seconds), ROWS, CASE
sudo install -m644 /tmp/rogue_36.29.52.182_v1.fw /lib/firmware/updates/powervr/ && sync
sudo modprobe -r powervr; bash /tmp/gpu-test.sh 2>&1 | grep Initialized; sudo dmesg -C
D=/sys/kernel/debug/dri/cac00000.gpu/pvr_fw; echo 0x80007ffe | sudo tee $D/trace_mask >/dev/null
(sudo env ONLY_EXTRA=1 EXTRA_REGS="${REGS:-6020 0410 0224 0270 02c0 0d00 2078 2080 2090 02b0 0130 12b8}" \
	timeout ${SAMPLE:-10} python3 /tmp/gpu-regs.py ${SAMPLE:-10} 0.2 > /tmp/ta-regs.txt 2>&1 &)
sleep 1; bash /tmp/deqp.sh "${CASE:-dEQP-VK.draw.renderpass.simple_draw.simple_draw_triangle_list}" 60 3 | head -1
cut -c1-260 /tmp/ta-regs.txt | head -${ROWS:-6}
sudo cat $D/trace_0 | grep -E "Kick TA|TA finished|0xe028|page fault|Lockup detected" -A1 | grep -v '^--' | head -${TRACE_ROWS:-8}
sudo dmesg | grep -iE "fault address" | head -2
