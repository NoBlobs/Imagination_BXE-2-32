#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Thalles Campagnani
# Run ON THE BOARD: dEQP-VK on the powervr Mesa driver.  usage: deqp.sh <case pattern> [timeout s]
cd /home/thalles/cts/build/external/vulkancts/modules/vulkan || exit 1
export VK_ICD_FILENAMES=/opt/mesa-pvr/share/vulkan/icd.d/powervr_mesa_icd.riscv64.json
export PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1
timeout ${2:-300} ./deqp-vk --deqp-case="$1" --deqp-log-filename=/tmp/deqp.qpa \
	--deqp-log-images=disable --deqp-log-shader-sources=disable 2>&1 | \
	grep -E "^Test case|^  (Pass|Fail|NotSupported|QualityWarning|CompatibilityWarning|InternalError|Crash|Timeout)|Passed:|Failed:|Not supported:|Warnings:|Skipped" | tail -${3:-40}
