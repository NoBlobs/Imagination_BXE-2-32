#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Thalles Campagnani
# Stability: (1) N repetitions of compute basic; (2) M runtime-PM cycles, each:
# wait until the GPU is runtime-suspended, then wake it with one compute job.
D=/sys/devices/platform/soc/soc:multimedia-bus/cac00000.gpu/power
mkdir -p /tmp/stab; : > /tmp/stab/summary.txt
echo auto | sudo tee $D/control >/dev/null
for i in $(seq 1 ${REPS:-5}); do
  bash /tmp/deqp.sh "dEQP-VK.compute.pipeline.basic.*" 900 9000 > /tmp/stab/rep$i.txt 2>&1
  echo "rep $i $(grep -oP 'Passed:\s+\K[0-9]+/[0-9]+' /tmp/stab/rep$i.txt) fail=$(grep -oP 'Failed:\s+\K[0-9]+' /tmp/stab/rep$i.txt)" >> /tmp/stab/summary.txt
done
ok=0; bad=0; susp=0
for i in $(seq 1 ${CYCLES:-100}); do
  for t in $(seq 1 50); do [ "$(cat $D/runtime_status)" = suspended ] && { susp=$((susp+1)); break; }; sleep 0.1; done
  if bash /tmp/deqp.sh "dEQP-VK.compute.pipeline.basic.copy_ssbo_single_invocation" 60 3 | grep -q "Failed:        0/1"; then ok=$((ok+1)); else bad=$((bad+1)); fi
done
echo "pm-cycles=${CYCLES:-100} suspended-before=$susp ok=$ok bad=$bad" >> /tmp/stab/summary.txt
echo "runtime_suspended_time=$(cat $D/runtime_suspended_time) active_time=$(cat $D/runtime_active_time)" >> /tmp/stab/summary.txt
echo DONE >> /tmp/stab/summary.txt
