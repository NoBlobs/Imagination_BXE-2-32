#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Thalles Campagnani
# ld.lld range-checks microMIPS J/JAL (R_MICROMIPS_26_S1) as if they were
# PC-relative, which fails for code above 0x80000000.  The encoding it writes
# is the correct region-relative one, so we link with --noinhibit-exec and
# drop only that warning; every other diagnostic is kept.
out=$("$@" 2>&1); rc=$?
printf '%s\n' "$out" | awk '
	/R_MICROMIPS_26_S1 out of range/ { skip = 3; next }
	skip > 0 { skip--; next }
	NF { print }' >&2
exit $rc
