#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Thalles Campagnani
"""Check that every TRACE(SFID_x, ...) passes the number of arguments its
format string expects (the count in the "/* N: ..." comment of sfid.h).

A wrong count makes the host's trace parser consume the next entry's words
(the cr_poll32 bug of E037).  SFID expressions that are not a single
identifier (e.g. `c ? SFID_A : SFID_B`) are checked against every SFID they
name.

usage: check-trace-args.py <sfid.h> <file.c>...    (exit 1 on a mismatch)
"""
import re
import sys


def split_args(s):
    depth, cur, out = 0, "", []
    for ch in s:
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def calls(text):
    for m in re.finditer(r"\bTRACE\s*\(", text):
        i, depth = m.end(), 1
        while depth and i < len(text):
            depth += {"(": 1, ")": -1}.get(text[i], 0)
            i += 1
        yield text.count("\n", 0, m.start()) + 1, text[m.end():i - 1]


def main(argv):
    nargs = {}
    for line in open(argv[1]):
        m = re.match(r"#define (SFID_\w+) 0x[0-9A-Fa-f]+u /\* (\d+):", line)
        if m:
            nargs[m.group(1)] = int(m.group(2))
    bad = 0
    for path in argv[2:]:
        text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"),
                      open(path).read(), flags=re.S)
        for line, body in calls(text):
            args = split_args(body)
            if not args:
                continue
            names = re.findall(r"\bSFID_\w+", args[0])
            given = len(args) - 1
            for n in names:
                if n not in nargs:
                    print(f"{path}:{line}: unknown {n}")
                    bad += 1
                elif nargs[n] != given:
                    print(f"{path}:{line}: {n} expects {nargs[n]} argument(s), got {given}")
                    bad += 1
    return 1 if bad else 0


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv))
