#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Thalles Campagnani
# runs the copy_and_blit subgroups one by one; summary in /tmp/cab/summary.txt
mkdir -p /tmp/cab; : > /tmp/cab/summary.txt
for g in buffer_to_buffer_with_offset buffer_to_image buffer_to_image_general_layout image_to_buffer image_to_buffer_general_layout image_to_buffer_indirect buffer_to_depthstencil depthstencil_to_buffer memory_to_image_indirect memory_to_depthstencil_indirect resolve_image depth_stencil_msaa_copy image_to_image_general_layout image_to_image.simple_tests image_to_image.3d_images image_to_image.array image_to_image.cube blit_image.simple_tests use_after_copy; do
  bash /tmp/deqp.sh "dEQP-VK.api.copy_and_blit.core.$g.*" 3000 100000 > /tmp/cab/$g.txt 2>&1
  p=$(grep -oP 'Passed:\s+\K[0-9]+/[0-9]+' /tmp/cab/$g.txt); f=$(grep -oP 'Failed:\s+\K[0-9]+' /tmp/cab/$g.txt); n=$(grep -oP 'Not supported:\s+\K[0-9]+' /tmp/cab/$g.txt)
  echo "$g pass=$p fail=$f ns=$n" >> /tmp/cab/summary.txt
done
echo DONE >> /tmp/cab/summary.txt
