#!/usr/bin/env python3
"""Dump the joint skeleton of an RE1 PC .emd model, as the renderer walks it.

This mirrors exactly what StickFigure_Finish (src/game/TmdRenderer.cpp) and
rotate_entity (src/game/GteMatrix.cpp) do at runtime:

    animHeader = puVar2[1] & ~3            (puVar2 = dwords at (size & ~3) - 0x14)
    animBase   = animHeader + (*(u16*)animHeader & ~3)
    per joint i, at animBase + i*4:
        +0  signed byte  child count
        +2  word         offset of the child list, relative to animBase

Walking the child lists from joint 0 gives each joint's parent. That is the
ground truth for where a hand, a foot and a head are, so the stick figure does
not have to guess at joint indices.

Verified against the running game: the player model (style 1) reports
joints=15 and parents 0<--1 1<-0 2<-0 3<-2 4<-3 5<-4 6<-2 7<-6 8<-7 9<-0
10<-9 11<-10 12<-0 13<-12 14<-13, which this tool reproduces.

Usage: python3 tools/dump_emd_skeleton.py <file.emd> [...]
"""
import struct
import sys


def u16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def dump(path):
    with open(path, "rb") as f:
        d = f.read()
    size = len(d)
    tbl = (size & ~3) - 0x14
    anim_header_off = u32(d, tbl + 4) & ~3
    anim_base_off = anim_header_off + (u16(d, anim_header_off) & ~3)
    joint_count = d[anim_header_off + 4]

    print(f"== {path}")
    print(f"   size={size} animHeader=0x{anim_header_off:x} "
          f"animBase=0x{anim_base_off:x} jointCount={joint_count}")

    parent = [-1] * joint_count
    seen = {0}
    queue = [0]
    while queue:
        idx = queue.pop(0)
        if idx >= joint_count:
            continue
        cnt = struct.unpack_from("<b", d, anim_base_off + idx * 4)[0]
        clist = anim_base_off + u16(d, anim_base_off + idx * 4 + 2)
        for c in range(cnt):
            ch = d[clist + c]
            if ch == 0 or ch >= joint_count or ch in seen:
                continue
            seen.add(ch)
            parent[ch] = idx
            queue.append(ch)

    kids = {}
    for j in range(1, joint_count):
        if parent[j] >= 0:
            kids.setdefault(parent[j], []).append(j)

    print("   " + " ".join(f"{i}<-{parent[i]}" for i in range(joint_count)))
    leaves = [i for i in range(1, joint_count) if i not in kids]
    print(f"   leaves (hands / feet / head tips): {leaves}")
    un = [i for i in range(joint_count) if i not in seen]
    if un:
        print(f"   unreached joints: {un}")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    for p in sys.argv[1:]:
        dump(p)


if __name__ == "__main__":
    main()
