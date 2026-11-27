#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Thalles Campagnani
#
# Run ON THE BOARD: the acceptance oracle of docsClean/board-testing-oracle.md.
#
#   oracle.sh [A] [B] [C] [D] [E] [F]  (default: A B C D; E hangs the GPU on purpose)
#
#   A  cold boot: reload powervr, check dmesg
#   B  functional sanity: vkfwtest multiframe (pre-3d), pre-ta, pre-cdm, xfer,
#      bigpb; each checks its output on the CPU (rc 0) and needs trace proof
#      (kicks seen, no discard, no fault, no HWR)
#   C  pixel-checked dEQP-VK suites with the reference pass counts
#   D  runtime-PM suspend, then vkfwtest xfer from the suspended state
#   E  vkfwtest hwr-lockup, hwr-fault: lockup / page fault -> device lost -> clean renders
#   F  extended (optional): the other vkfwtest tests (texture, zs-load, msaa, mrt,
#      sizes, vertex, indirect, queries, formats, stress, soak, churn) and prio-*
#
# Every stage also fails on the hard-failure patterns of the oracle. Output:
# one PASS/FAIL line per check, a summary in ~/oracle/summary.txt (on disk, synced
# after every line, so it survives a board hang).
# Environment: VKFWTEST (default ~/vkfwtest-clean/build/vkfwtest, built by
# tools/deploy.sh vkfwtest), VKFWTEST_ARGS (extra options), CTS (deqp-vk directory).
set -u
OUT=${OUT:-$HOME/oracle}; mkdir -p $OUT; : > $OUT/summary.txt
D=/sys/kernel/debug/dri/cac00000.gpu/pvr_fw
DEV=/sys/bus/platform/devices/cac00000.gpu
VKFWTEST=${VKFWTEST:-$HOME/vkfwtest-clean/build/vkfwtest}
VKFWTEST_ARGS=${VKFWTEST_ARGS:-}
CTS=${CTS:-/home/thalles/cts/build/external/vulkancts/modules/vulkan}
export VK_ICD_FILENAMES=/opt/mesa-pvr/share/vulkan/icd.d/powervr_mesa_icd.riscv64.json
export PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1
HARD='HW poll .* failed|BIF.*fault|MMU.*fault|FW fault|ASSERT|FWCCB.*112|type=112|device lost|error -22|Lockup|HWRNeeded=1|Discarded cmd|Page fault|Job timeout|context reset'
fails=0

say() { echo "$1  $2" | tee -a $OUT/summary.txt; sync; [ "$1" = FAIL ] && fails=$((fails + 1)); return 0; }
trace() { sudo cat $D/trace_0 2>/dev/null; }
# The firmware trace buffer cannot be cleared: remember what it held, and look
# only at the lines added since (an earlier failure must not fail a later check).
reset_logs() {
	sudo dmesg -C; echo 0x80007ffe | sudo tee $D/trace_mask >/dev/null 2>&1
	trace > $OUT/.trace_before
}
new_trace() { trace | grep -vxF -f $OUT/.trace_before; }
# hard failures in dmesg and in the firmware trace since the last reset_logs
clean() {
	local n
	n=$( (sudo dmesg; new_trace) | grep -E "$HARD" | grep -vE 'HWRNeeded=0' | wc -l)
	[ "$n" -eq 0 ]
}
kicks() { new_trace | grep -c "$1"; }

stage_A() {
	sudo modprobe -r powervr 2>/dev/null; sync
	sudo dmesg -C
	sudo modprobe powervr exp_hw_support=1 || { say FAIL "A: modprobe"; return; }
	sleep 2
	local m; m=$(sudo dmesg)
	echo "$m" | grep -q "FW version v1\." && say PASS "A: $(echo "$m" | grep -o 'FW version v[0-9.]*')" \
		|| say FAIL "A: no FW version line"
	echo "$m" | grep -q "Initialized powervr" && say PASS "A: driver initialised" || say FAIL "A: not initialised"
	echo "$m" | grep -qE "$HARD" && say FAIL "A: hard failure in dmesg" || say PASS "A: no hard failure"
}

# B: <test> <trace pattern that must appear>...
run_vkfwtest() {
	local sc=$1 rc; shift
	reset_logs
	echo "RUN   $STAGE: $sc" >> $OUT/summary.txt; sync
	timeout 900 "$VKFWTEST" -j $VKFWTEST_ARGS "$sc" > $OUT/vkfwtest-$sc.json 2> $OUT/vkfwtest-$sc.txt
	rc=$?
	new_trace > $OUT/trace-$sc.txt
	if [ $rc -ne 0 ]; then
		say FAIL "$STAGE: vkfwtest $sc rc=$rc $(grep -o '"detail":"[^"]*"' $OUT/vkfwtest-$sc.json | head -1)"
		return
	fi
	for p in "$@"; do
		[ "$(grep -c "$p" $OUT/trace-$sc.txt)" -ge 1 ] || { say FAIL "$STAGE: $sc: no '$p' in the trace"; return; }
	done
	clean && say PASS "$STAGE: vkfwtest $sc" || say FAIL "$STAGE: $sc: hard failure in dmesg/trace"
}

stage_B() {
	[ -x "$VKFWTEST" ] || { say FAIL "B: $VKFWTEST not found (tools/deploy.sh vkfwtest)"; return; }
	STAGE=B
	local only=${B_ONLY:-multiframe pre-ta pre-cdm xfer bigpb input-attach rta}
	for sc in $only; do
		case $sc in
		multiframe|pre-3d|pre-ta|bigpb|input-attach) run_vkfwtest $sc "Kick TA" "Kick 3D:" ;;
		rta) run_vkfwtest $sc "Kick 3D:" ;;
		pre-cdm) run_vkfwtest $sc "Kick Compute" ;;
		xfer) run_vkfwtest $sc "Kick 3D TQ" ;;
		*) run_vkfwtest $sc ;;
		esac
	done
}

# C: <case pattern> <expected passed> <expected not supported>
deqp() {
	local pat=$1 pass=$2 ns=$3 log=$OUT/$(echo "$1" | tr -c 'a-zA-Z0-9.\n' _).txt
	reset_logs
	(cd "$CTS" && timeout 1800 ./deqp-vk --deqp-case="$pat" --deqp-log-filename=$OUT/deqp.qpa \
		--deqp-log-images=disable --deqp-log-shader-sources=disable) > "$log" 2>&1
	echo "RUN   C: $pat" >> $OUT/summary.txt; sync
	local p f n
	p=$(grep -oP 'Passed:\s+\K[0-9]+' "$log"); f=$(grep -oP 'Failed:\s+\K[0-9]+' "$log")
	n=$(grep -oP 'Not supported:\s+\K[0-9]+' "$log")
	if [ "${f:-x}" = 0 ] && [ "${p:-0}" -ge "$pass" ] && clean; then
		say PASS "C: $pat  passed $p (>= $pass), not supported $n (ref $ns)"
	else
		say FAIL "C: $pat  passed ${p:-?} failed ${f:-?} (ref $pass/$ns)"
	fi
}

stage_C() {
	deqp 'dEQP-VK.api.smoke.*' 8 0
	deqp 'dEQP-VK.draw.renderpass.simple_draw.*' 4 0
	deqp 'dEQP-VK.compute.pipeline.basic.*' 73 9
	deqp 'dEQP-VK.api.copy_and_blit.core.image_to_image.simple_tests.*' 23 0
	deqp 'dEQP-VK.synchronization.basic.*' 24 5
	deqp 'dEQP-VK.api.buffer.*' 1030 2046
}

stage_D() {
	echo auto | sudo tee $DEV/power/control >/dev/null
	local i st
	for i in $(seq 1 50); do
		st=$(cat $DEV/power/runtime_status); [ "$st" = suspended ] && break; sleep 0.2
	done
	[ "$st" = suspended ] && say PASS "D: GPU suspended" || { say FAIL "D: GPU not suspended ($st)"; return; }
	reset_logs
	timeout 300 "$VKFWTEST" -q xfer > $OUT/stage-d.txt 2>&1
	local rc=$?
	[ $rc -eq 0 ] && [ "$(kicks 'Kick 3D TQ')" -ge 1 ] && clean \
		&& say PASS "D: vkfwtest xfer after warm boot" || say FAIL "D: vkfwtest xfer after warm boot (rc=$rc)"
}

# E: the lockup itself is expected (HWR lines in the trace); only the harness's
# verdict counts: device lost for the guilty device, then clean renders.
stage_E() {
	[ -x "$VKFWTEST" ] || { say FAIL "E: $VKFWTEST not found"; return; }
	local t rc
	for t in hwr-lockup hwr-fault; do
		reset_logs
		timeout 900 "$VKFWTEST" -j $t > $OUT/vkfwtest-$t.json 2> $OUT/vkfwtest-$t.txt
		rc=$?
		new_trace > $OUT/trace-$t.txt
		[ $rc -eq 0 ] && say PASS "E: vkfwtest $t" ||
			say FAIL "E: vkfwtest $t rc=$rc $(grep -o '"detail":"[^"]*"' $OUT/vkfwtest-$t.json | head -1)"
	done
}

# F: the rest of vkfwtest, with the same proofs as stage B (rc 0, no hard failure)
stage_F() {
	[ -x "$VKFWTEST" ] || { say FAIL "F: $VKFWTEST not found"; return; }
	STAGE=F
	local only=${F_ONLY:-texture zs-load msaa mrt sizes vertex indirect queries formats stress soak churn prio-3d prio-ta prio-cdm}
	for sc in $only; do
		run_vkfwtest $sc
	done
}

stages=${*:-A B C D}
for s in $stages; do "stage_$s"; done
echo "== $fails failure(s)" | tee -a $OUT/summary.txt
[ $fails -eq 0 ]
