#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Thalles Campagnani
"""Pre-flight check of a .fw container before it is copied to the board.

Checks of docsClean/board-testing-oracle.md section 2, against the public
pvr_fw_info.h layout:
  1. size is a multiple of 4096 (FW_BLOCK_SIZE);
  2. the info header in the last block: info_version 3, BVNC 36.29.52.182,
     PVR_FW_FLAGS_OPEN_SOURCE, fw_version_major >= 1, layout_entry_num <= 8,
     a device-info block that fits;
  3. layout entries of each kind (code / data) do not overlap;
  4. every ELF PT_LOAD segment fits in a layout entry, and the ELF flags are
     microMIPS o32 (0x72001001).

usage: fw-preflight.py <file.fw>   (exit status 0 = pass)
"""
import struct
import sys

FW_BLOCK = 4096
BVNC = (36 << 48) | (29 << 32) | (52 << 16) | 182
SEC_NAMES = {4: "MIPS_CODE", 5: "MIPS_EXCEPTIONS_CODE", 6: "MIPS_BOOT_CODE",
             7: "MIPS_PRIVATE_DATA", 8: "MIPS_BOOT_DATA", 9: "MIPS_STACK"}
TYPE_NAMES = {0: "NONE", 1: "CODE", 2: "DATA", 3: "COREMEM_CODE", 4: "COREMEM_DATA"}
ELF_FLAGS = 0x72001001


def main(path):
    data = open(path, "rb").read()
    errors = []

    def check(cond, msg):
        print(("ok    " if cond else "FAIL  ") + msg)
        if not cond:
            errors.append(msg)

    size = len(data)
    check(size >= FW_BLOCK and size % FW_BLOCK == 0, f"size {size} is a multiple of 4096")
    if errors:
        return 1

    hdr_off = size - FW_BLOCK
    (info_version, header_len, n_ent, ent_size, bvnc, page_size, flags,
     vmaj, vmin, vbuild, devinfo_size, _pad) = struct.unpack_from("<IIIIQIIHHIII", data, hdr_off)
    check(info_version == 3, f"info_version {info_version} == 3")
    check(bvnc == BVNC, f"bvnc {bvnc:#018x} == 36.29.52.182")
    check(flags & 1, f"flags {flags:#x} has PVR_FW_FLAGS_OPEN_SOURCE")
    check(vmaj >= 1, f"fw version v{vmaj}.{vmin} (build {vbuild}) has major >= 1")
    check(n_ent <= 8, f"layout_entry_num {n_ent} <= 8")
    check(ent_size == 24, f"layout_entry_size {ent_size} == 24")
    check(header_len + n_ent * ent_size <= FW_BLOCK, "header and layout table fit in the block")
    check(devinfo_size <= hdr_off, f"device_info_size {devinfo_size} fits before the header")

    entries = []
    for i in range(n_ent):
        sid, stype, base, max_size, alloc_size, alloc_off = struct.unpack_from(
            "<IIIIII", data, hdr_off + header_len + i * ent_size)
        entries.append((sid, stype, base, max_size, alloc_size, alloc_off))
        print(f"      entry {SEC_NAMES.get(sid, sid):22} {TYPE_NAMES.get(stype, stype):5} "
              f"base {base:#010x} max {max_size:#07x} alloc {alloc_size:#07x} @ {alloc_off:#07x}")
    check({e[0] for e in entries} >= set(SEC_NAMES), "all six MIPS sections are present")

    for kind in (1, 2):
        ranges = sorted((e[5], e[5] + e[4], e[0]) for e in entries if e[1] == kind)
        ok = all(a[1] <= b[0] for a, b in zip(ranges, ranges[1:]))
        check(ok, f"{TYPE_NAMES[kind]} allocations do not overlap")
        for e in entries:
            if e[1] == kind:
                check(e[4] <= e[3], f"{SEC_NAMES.get(e[0], e[0])} alloc_size <= max_size")

    # ELF at the start of the file
    if data[:4] != b"\x7fELF":
        check(False, "ELF image at offset 0")
        return 1
    e_flags = struct.unpack_from("<I", data, 0x24)[0]
    check(e_flags == ELF_FLAGS, f"ELF flags {e_flags:#x} == {ELF_FLAGS:#x} (microMIPS o32)")
    phoff, = struct.unpack_from("<I", data, 0x1C)
    phentsize, phnum = struct.unpack_from("<HH", data, 0x2A)
    for i in range(phnum):
        p_type, _off, vaddr, _paddr, filesz, memsz, _fl, _al = struct.unpack_from(
            "<IIIIIIII", data, phoff + i * phentsize)
        if p_type != 1 or memsz == 0:
            continue
        home = [e for e in entries
                if e[2] <= vaddr and vaddr + memsz <= e[2] + e[3]]
        check(bool(home), f"PT_LOAD {vaddr:#010x}+{memsz:#x} fits in a layout entry"
              + (f" ({SEC_NAMES.get(home[0][0], home[0][0])})" if home else ""))

    print("PASS" if not errors else f"FAILED: {len(errors)} check(s)")
    return 0 if not errors else 1


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
