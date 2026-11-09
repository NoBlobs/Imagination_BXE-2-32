#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Thalles Campagnani
# Parse the E027 render-structure dump from a firmware trace (tools/parse-dump.py <trace>).
import re,sys
lines=open(sys.argv[1]).read().splitlines()
blocks={}; order=[]
i=0
while i<len(lines):
    m=re.match(r'\[\d+\] : 0x(d0[0-9a-f]{6})$',lines[i].strip())
    if m and i+1<len(lines):
        tag=int(m.group(1),16); ws=[int(x,16) for x in re.findall(r'0x([0-9a-f]{8})',lines[i+1])]
        blk=tag&0xFFFFF000; off=tag&0xFFF
        b=blocks.setdefault(blk,{})
        if blk not in order: order.append(blk)
        for k,w in enumerate(ws): b[off+4*k]=w
        i+=2; continue
    i+=1
names={0xd0010000:'GEOM cmd',0xd0020000:'FRAG cmd',0xd0030000:'hwrtdata',0xd0040000:'hwrtdata_common',0xd0050000:'freelist0',0xd0051000:'freelist1',0xd0052000:'freelist2',0xd0060000:'static_render_state',0xd0070000:'geom ctx_state'}
for blk in order:
    b=blocks[blk]; print("==",names.get(blk,hex(blk)))
    for off in sorted(b):
        if b[off]: print("  +0x%03x %08x"%(off,b[off]))
