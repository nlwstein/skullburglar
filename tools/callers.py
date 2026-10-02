"""List `bl`/`b` call sites targeting the given addresses in the dumped image.

    python3 tools/callers.py 0x8253D7A0 [0x...]
Prints: target  site  kind  containing_function
"""
import bisect
import json
import struct
import sys

BASE = 0x82000000
CODE_BEGIN, CODE_END = 0x82310000, 0x83754C14
img = open("dumps/default_image.bin", "rb").read()
starts = sorted(int(k, 16) for k in json.load(open("generated/default/codegen.partition.json"))["assignments"])
targets = {int(a, 16) for a in sys.argv[1:]}

lo, hi = CODE_BEGIN - BASE, CODE_END - BASE
words = struct.unpack(f">{(hi - lo) // 4}I", img[lo:hi])
for i, w in enumerate(words):
    if (w >> 26) != 18 or w & 2:  # I-form branch, relative only
        continue
    site = CODE_BEGIN + i * 4
    off = w & 0x03FFFFFC
    if off & 0x02000000:
        off -= 0x04000000
    t = (site + off) & 0xFFFFFFFF
    if t in targets:
        fn = starts[bisect.bisect_right(starts, site) - 1]
        print(f"{t:08X}\t{site:08X}\t{'bl' if w & 1 else 'b'}\tsub_{fn:08X}")
