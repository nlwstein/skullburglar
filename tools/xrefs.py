"""String cross-references for the dumped image.

Finds `lis rA,hi` followed (within a short window, same rA) by `addi rD,rA,lo` /
`ori rD,rA,lo` / a D-form load-store with base rA, computes the address, and maps
addresses that hold a printable string (ASCII or UTF-16BE) to the containing function.

    python3 tools/xrefs.py build                  # writes dumps/xrefs.tsv
    python3 tools/xrefs.py grep 'ProcessEvent'    # functions referencing matching strings
"""
import bisect
import json
import re
import struct
import sys

BASE = 0x82000000
CODE_BEGIN, CODE_END = 0x82310000, 0x83754C14
WINDOW = 12


def load():
    img = open("dumps/default_image.bin", "rb").read()
    starts = sorted(int(k, 16) for k in json.load(open("generated/default/codegen.partition.json"))["assignments"])
    return img, starts


def string_at(img, a):
    off = a - BASE
    if not (0 <= off < len(img)):
        return None
    m = re.match(rb"[\x20-\x7e\t\n]{4,200}\x00", img[off : off + 201])
    if m:
        return m.group(0)[:-1].decode()
    m = re.match(rb"(?:\x00[\x20-\x7e]){4,200}\x00\x00", img[off : off + 402])
    if m:
        return "L\"" + m.group(0)[:-2].decode("utf-16-be") + "\""
    return None


def build():
    img, starts = load()
    n0, n1 = (CODE_BEGIN - BASE) // 4, (CODE_END - BASE) // 4
    words = struct.unpack(f">{n1 - n0}I", img[n0 * 4 : n1 * 4])
    out = []
    for i, w in enumerate(words):
        if w >> 26 != 15 or (w >> 16) & 31 != 0:  # lis rA,hi  == addis rA,0,hi
            continue
        ra, hi = (w >> 21) & 31, w & 0xFFFF
        for j in range(i + 1, min(i + WINDOW, len(words))):
            v = words[j]
            op, rd, rs = v >> 26, (v >> 21) & 31, (v >> 16) & 31
            lo = v & 0xFFFF
            if op == 14 and rs == ra:  # addi
                a = ((hi << 16) + (lo - 0x10000 if lo & 0x8000 else lo)) & 0xFFFFFFFF
            elif op == 24 and rd == ra:  # ori rA,rS,lo (rS in rd field)
                a = (hi << 16) | lo
            elif op in (32, 34, 36, 38, 40, 44, 48, 50, 52, 54) and rs == ra:  # lwz lbz stw stb lhz sth lfs lfd stfs stfd
                a = ((hi << 16) + (lo - 0x10000 if lo & 0x8000 else lo)) & 0xFFFFFFFF
            else:
                if (rd == ra and op not in (36, 38, 44, 52, 54)) or (v >> 26) == 18:  # clobbered or branch
                    break
                continue
            s = string_at(img, a)
            if s:
                pc = CODE_BEGIN + (i + j - i) * 4 + (i * 4) - (i * 4)
                pc = BASE + (n0 + j) * 4
                fn = starts[bisect.bisect_right(starts, pc) - 1]
                out.append((fn, pc, a, s))
            break
    with open("dumps/xrefs.tsv", "w") as f:
        for fn, pc, a, s in out:
            f.write(f"{fn:08X}\t{pc:08X}\t{a:08X}\t{s.encode('unicode_escape').decode()}\n")
    print(f"{len(out)} string xrefs, {len({o[0] for o in out})} functions", file=sys.stderr)


def grep(pat):
    rx = re.compile(pat)
    for line in open("dumps/xrefs.tsv"):
        if rx.search(line.split("\t", 3)[3]):
            print(line.rstrip())


if __name__ == "__main__":
    build() if sys.argv[1] == "build" else grep(sys.argv[2])
