#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Thalles Campagnani
"""Pack the firmware ELF into the `.fw` container read by the mainline
powervr driver (format: public pvr_fw_info.h / pvr_fw.c):

    [ firmware ELF ][ padding ][ device info ][ 4 KiB block: header + layout table ]

The device info block (quirks, enhancements, features and feature values)
describes the BXE-2-32 (BVNC 36.29.52.182) to the host driver.
"""
import argparse
import re
import struct

FW_BLOCK = 4096
INFO_VERSION = 3
FLAGS_OPEN_SOURCE = 1 << 0
FW_VERSION = (1, 0, 1)  # major, minor, build

BVNC = (36, 29, 52, 182)

# enum pvr_fw_section_id / pvr_fw_section_type (pvr_fw_info.h)
SEC = {"MIPS_CODE": 4, "MIPS_EXCEPTIONS_CODE": 5, "MIPS_BOOT_CODE": 6,
       "MIPS_PRIVATE_DATA": 7, "MIPS_BOOT_DATA": 8, "MIPS_STACK": 9}
FW_CODE, FW_DATA = 1, 2

# Order of the bits of the device-info masks (pvr_rogue_fwif_dev_info.h).
BRNS = [44079, 47217, 48492, 48545, 49927, 50767, 51764, 62269, 63142, 63553,
        66011, 71242]
ERNS = [35421, 38020, 38748, 42064, 42290, 42606, 47025, 57596]
FEATURES = """AXI_ACELITE CDM_CONTROL_STREAM_FORMAT CLUSTER_GROUPING
COMMON_STORE_SIZE_IN_DWORDS COMPUTE COMPUTE_MORTON_CAPABLE COMPUTE_OVERLAP
COREID_PER_OS DYNAMIC_DUST_POWER ECC_RAMS FBCDC FBCDC_ALGORITHM FBCDC_ARCHITECTURE
FBC_MAX_DEFAULT_DESCRIPTORS FBC_MAX_LARGE_DESCRIPTORS FB_CDC_V4
GPU_MULTICORE_SUPPORT GPU_VIRTUALISATION GS_RTA_SUPPORT IRQ_PER_OS
ISP_MAX_TILES_IN_FLIGHT ISP_SAMPLES_PER_PIXEL ISP_ZLS_D24_S8_PACKING_OGL_MODE
LAYOUT_MARS MAX_PARTITIONS META META_COREMEM_SIZE MIPS NUM_CLUSTERS
NUM_ISP_IPP_PIPES NUM_OSIDS NUM_RASTER_PIPES PBE2_IN_XE PBVNC_COREID_REG PERFBUS
PERF_COUNTER_BATCH PHYS_BUS_WIDTH RISCV_FW_PROCESSOR ROGUEXE
S7_TOP_INFRASTRUCTURE SIMPLE_INTERNAL_PARAMETER_FORMAT
SIMPLE_INTERNAL_PARAMETER_FORMAT_V2 SIMPLE_PARAMETER_FORMAT_VERSION SLC_BANKS
SLC_CACHE_LINE_SIZE_BITS SLC_SIZE_CONFIGURABLE SLC_SIZE_IN_KILOBYTES SOC_TIMER
SYS_BUS_SECURE_RESET TESSELLATION TILE_REGION_PROTECTION TILE_SIZE_X TILE_SIZE_Y
TLA TPU_CEM_DATAMASTER_GLOBAL_REGISTERS TPU_DM_GLOBAL_REGISTERS
TPU_FILTERING_MODE_CONTROL USC_MIN_OUTPUT_REGISTERS_PER_PIX VDM_DRAWINDIRECT
VDM_OBJECT_LEVEL_LLS VIRTUAL_ADDRESS_SPACE_BITS WATCHDOG_TIMER
WORKGROUP_PROTECTION XE_ARCHITECTURE XE_MEMORY_HIERARCHY XE_TPU2
XPU_MAX_REGBANKS_ADDR_WIDTH XPU_MAX_SLAVES XPU_REGISTER_BROADCAST
XT_TOP_INFRASTRUCTURE ZLS_SUBTILE""".split()
VALUE_FEATURES = set("""CDM_CONTROL_STREAM_FORMAT COMMON_STORE_SIZE_IN_DWORDS ECC_RAMS
FBCDC FBCDC_ALGORITHM FBCDC_ARCHITECTURE FBC_MAX_DEFAULT_DESCRIPTORS
FBC_MAX_LARGE_DESCRIPTORS ISP_MAX_TILES_IN_FLIGHT ISP_SAMPLES_PER_PIXEL LAYOUT_MARS
MAX_PARTITIONS META META_COREMEM_SIZE NUM_CLUSTERS NUM_ISP_IPP_PIPES NUM_OSIDS
NUM_RASTER_PIPES PHYS_BUS_WIDTH SIMPLE_PARAMETER_FORMAT_VERSION SLC_BANKS
SLC_CACHE_LINE_SIZE_BITS SLC_SIZE_IN_KILOBYTES TILE_SIZE_X TILE_SIZE_Y
USC_MIN_OUTPUT_REGISTERS_PER_PIX VIRTUAL_ADDRESS_SPACE_BITS XE_ARCHITECTURE
XPU_MAX_REGBANKS_ADDR_WIDTH XPU_MAX_SLAVES XPU_REGISTER_BROADCAST""".split())

# BXE-2-32 description, from docsClean/memory-model.md ("Device information
# block"): the values carried by the redistributable open firmware image for
# this BVNC, as documented by the spec maintainer (answers question Q2).
DEV_QUIRKS = [63553]
DEV_ERNS = [35421, 38748, 42290, 42606, 47025, 57596]
DEV_FEATURES = {
    # value features
    "CDM_CONTROL_STREAM_FORMAT": 1,
    "COMMON_STORE_SIZE_IN_DWORDS": 13312,
    "FBCDC": 50,
    "FBCDC_ALGORITHM": 50,
    "FBCDC_ARCHITECTURE": 7,
    "ISP_MAX_TILES_IN_FLIGHT": 2,
    "ISP_SAMPLES_PER_PIXEL": 1,
    "LAYOUT_MARS": 1,
    "MAX_PARTITIONS": 8,
    "NUM_CLUSTERS": 1,
    "NUM_ISP_IPP_PIPES": 2,
    "NUM_OSIDS": 8,
    "NUM_RASTER_PIPES": 1,
    "PHYS_BUS_WIDTH": 36,
    "SIMPLE_PARAMETER_FORMAT_VERSION": 2,
    "SLC_BANKS": 1,
    "SLC_CACHE_LINE_SIZE_BITS": 512,
    "SLC_SIZE_IN_KILOBYTES": 16,
    "TILE_SIZE_X": 16,
    "TILE_SIZE_Y": 16,
    "USC_MIN_OUTPUT_REGISTERS_PER_PIX": 2,
    "VIRTUAL_ADDRESS_SPACE_BITS": 40,
    "XE_ARCHITECTURE": 1,
    "XPU_MAX_REGBANKS_ADDR_WIDTH": 19,
    "XPU_MAX_SLAVES": 3,
    "XPU_REGISTER_BROADCAST": 1,
    # boolean features
    "AXI_ACELITE": True,
    "COMPUTE": True,
    "COMPUTE_OVERLAP": True,
    "COREID_PER_OS": True,
    "GPU_MULTICORE_SUPPORT": True,
    "GPU_VIRTUALISATION": True,
    "GS_RTA_SUPPORT": True,
    "IRQ_PER_OS": True,
    "MIPS": True,
    "PBE2_IN_XE": True,
    "PBVNC_COREID_REG": True,
    "PERFBUS": True,
    "ROGUEXE": True,
    "SIMPLE_INTERNAL_PARAMETER_FORMAT": True,
    "SIMPLE_INTERNAL_PARAMETER_FORMAT_V2": True,
    "SLC_SIZE_CONFIGURABLE": True,
    "SOC_TIMER": True,
    "SYS_BUS_SECURE_RESET": True,    # required: E001
    "XE_MEMORY_HIERARCHY": True,
    "XE_TPU2": True,
}


def parse_layout(path):
    vals = {}
    for line in open(path, encoding="utf-8"):
        m = re.match(r"#define\s+(FWL?_\w+)\s+(0x[0-9A-Fa-f]+|\d+)", line)
        if m:
            vals[m.group(1)] = int(m.group(2), 0)
    return vals


def device_info():
    assert len(FEATURES) == 71
    brn = sum(1 << BRNS.index(b) for b in DEV_QUIRKS)
    ern = sum(1 << ERNS.index(e) for e in DEV_ERNS)
    nwords = (len(FEATURES) + 63) // 64
    masks = [0] * nwords
    params = []
    for i, name in enumerate(FEATURES):
        if name not in DEV_FEATURES:
            continue
        masks[i // 64] |= 1 << (i % 64)
        if name in VALUE_FEATURES:
            v = DEV_FEATURES[name]
            params.append(int(v))
    for name in DEV_FEATURES:
        assert name in FEATURES, name
    hdr = struct.pack("<4Q", 1, 1, nwords, len(params))
    body = struct.pack("<Q", brn) + struct.pack("<Q", ern)
    body += b"".join(struct.pack("<Q", m) for m in masks)
    body += b"".join(struct.pack("<Q", p) for p in params)
    return hdr + body


def layout_table(L):
    code, data = FW_CODE, FW_DATA
    ents = [
        # id, type, base_addr, max_size, alloc_size, alloc_offset
        (SEC["MIPS_CODE"], code, L["FWL_CODE_BASE"], L["FWL_CODE_SIZE"], L["FWL_CODE_SIZE"], 0),
        (SEC["MIPS_BOOT_CODE"], code, L["FW_BOOT_CODE_VA"], 0x1000, 0x1000, L["FWL_BOOT_OFFSET"]),
        (SEC["MIPS_EXCEPTIONS_CODE"], code, L["FW_EXC_VA"], 0x1000, 0x1000, L["FWL_EXC_OFFSET"]),
        (SEC["MIPS_PRIVATE_DATA"], data, L["FWL_DATA_BASE"], L["FWL_DATA_SIZE"], L["FWL_DATA_SIZE"], 0),
        (SEC["MIPS_BOOT_DATA"], data, L["FW_BOOT_DATA_VA"], 0x1000, 0x1000, L["FWL_BOOTDATA_OFFSET"]),
        (SEC["MIPS_STACK"], data, L["FW_STACK_VA"], L["FWL_STACK_SIZE"], L["FWL_STACK_SIZE"],
         L["FWL_STACK_OFFSET"]),
    ]
    assert sum(e[4] for e in ents if e[1] == code) == L["FWL_CODE_ALLOC"]
    assert sum(e[4] for e in ents if e[1] == data) == L["FWL_DATA_ALLOC"]
    return ents


def check_elf(elf, ents):
    assert elf[:4] == b"\x7fELF" and elf[4] == 1 and elf[5] == 1, "need ELF32 LE"
    e_phoff, = struct.unpack_from("<I", elf, 28)
    e_phentsize, e_phnum = struct.unpack_from("<HH", elf, 42)
    for i in range(e_phnum):
        p_type, p_off, p_vaddr, _, p_filesz, p_memsz, _, _ = struct.unpack_from(
            "<8I", elf, e_phoff + i * e_phentsize)
        if p_type != 1:
            continue
        ok = any(e[2] <= p_vaddr and p_vaddr + p_memsz <= e[2] + e[4] for e in ents)
        if not ok:
            raise SystemExit(f"segment 0x{p_vaddr:08x}+0x{p_memsz:x} outside the layout table")
        print(f"  PT_LOAD vaddr=0x{p_vaddr:08x} filesz=0x{p_filesz:x} memsz=0x{p_memsz:x}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--layout", required=True)
    ap.add_argument("elf")
    ap.add_argument("out")
    a = ap.parse_args()

    L = parse_layout(a.layout)
    ents = layout_table(L)
    elf = open(a.elf, "rb").read()
    check_elf(elf, ents)

    dinfo = device_info()
    body = elf
    pad = (-(len(body) + len(dinfo))) % FW_BLOCK
    body += b"\0" * pad + dinfo

    b, v, n, c = BVNC
    bvnc = (b << 48) | (v << 32) | (n << 16) | c
    hdr = struct.pack("<4IQ2I2H3I", INFO_VERSION, 48, len(ents), 24, bvnc, 4096,
                      FLAGS_OPEN_SOURCE, FW_VERSION[0], FW_VERSION[1], FW_VERSION[2],
                      len(dinfo), 0)
    assert len(hdr) == 48
    tbl = b"".join(struct.pack("<6I", *e) for e in ents)
    block = hdr + tbl
    block += b"\0" * (FW_BLOCK - len(block))
    out = body + block
    assert len(out) % FW_BLOCK == 0
    open(a.out, "wb").write(out)
    print(f"{a.out}: {len(out)} bytes, device info {len(dinfo)} bytes, {len(ents)} sections")


if __name__ == "__main__":
    main()
