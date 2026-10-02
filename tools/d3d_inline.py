"""Inline D3D device accesses in game code (native renderer groundwork).

UE3's Xenon RHI keeps the device pointer in a global and inlines part of the XDK API: shader constants and
fetch constants are written straight into the device structure, dirty bits are or-ed in. A native renderer
that supplies its own device must know every field that code touches. For each function that loads the
device global this prints the D3D runtime functions it calls and the device offsets it reads (r) and
writes (w), found by tracking the registers that hold the device pointer through the function.

    python3 tools/d3d_inline.py               # every function loading the device global
    python3 tools/d3d_inline.py 0x827BAE40    # one function, with the access sites

Linear scan, no control flow: a register is "the device" from its load until it is overwritten, which
is right for the short RHI functions this targets. Offsets are byte offsets into D3DDevice.
"""
import bisect
import json
import struct
import sys

BASE = 0x82000000
CODE_BEGIN, CODE_END = 0x82310000, 0x83754C14
D3D_LO, D3D_HI = 0x82310000, 0x8234A000
DEVICE_GLOBAL = 0x838191B0

LOADS = {32: ("lwz", 4), 34: ("lbz", 1), 40: ("lhz", 2), 42: ("lha", 2), 48: ("lfs", 4), 50: ("lfd", 8),
         58: ("ld", 8)}
STORES = {36: ("stw", 4), 38: ("stb", 1), 44: ("sth", 2), 52: ("stfs", 4), 54: ("stfd", 8), 62: ("std", 8)}


def simm(v):
    return v - 0x10000 if v & 0x8000 else v


def load_image():
    img = open("dumps/default_image.bin", "rb").read()
    n = len(img) // 4
    w = struct.unpack(f">{n}I", img[: n * 4])
    starts = sorted(int(k, 16) for k in json.load(open("generated/default/codegen.partition.json"))["assignments"])
    return w, starts


def analyze(w, starts, f):
    i = bisect.bisect_right(starts, f)
    end = starts[i] if i < len(starts) else CODE_END
    hi = (DEVICE_GLOBAL + 0x8000) >> 16
    lo = DEVICE_GLOBAL - (hi << 16)
    lis_regs = {}   # reg -> hi half for a pending lis
    dev = set()     # registers holding the device pointer
    accesses, calls = [], []
    for a in range(f, end, 4):
        x = w[(a - BASE) // 4]
        op, rt, ra, d = x >> 26, (x >> 21) & 31, (x >> 16) & 31, x & 0xFFFF
        if op == 15 and ra == 0:  # lis
            lis_regs[rt] = d
            dev.discard(rt)
            continue
        if op == 18 and (x & 3) == 1:  # bl
            off = x & 0x03FFFFFC
            off = off - 0x04000000 if off & 0x02000000 else off
            t = (a + off) & 0xFFFFFFFF
            if D3D_LO <= t < D3D_HI:
                calls.append((a, t, 3 in dev))
            dev -= set(range(0, 13))  # volatile registers are gone after a call
            continue
        if op == 32 and ra in lis_regs and lis_regs[ra] == hi & 0xFFFF and simm(d) == lo:
            dev.add(rt)
            continue
        if op == 31 and ((x >> 1) & 0x3FF) == 444 and (x >> 11) & 31 == ra:  # mr ra, rs (or ra,rs,rs)
            src, dst = rt, ra
            (dev.add if src in dev else dev.discard)(dst)
            continue
        if op in LOADS and ra in dev:
            accesses.append((a, "r", simm(d), LOADS[op][0]))
            dev.discard(rt) if op not in (48, 50) else None
            continue
        if op in STORES and ra in dev:
            accesses.append((a, "w", simm(d), STORES[op][0]))
            continue
        if op == 14 and ra in dev:  # addi rD, rDev, off: address of a device field
            accesses.append((a, "&", simm(d), "addi"))
        # any other write to rt kills it
        if op not in (36, 38, 44, 52, 54, 62, 18, 16, 19, 11, 10) and rt in dev:
            dev.discard(rt)
        lis_regs.pop(rt, None) if op not in (36, 38, 44, 52, 54, 62) else None
    return accesses, calls


def main():
    w, starts = load_image()
    if len(sys.argv) > 1:
        f = starts[bisect.bisect_right(starts, int(sys.argv[1], 16)) - 1]
        acc, calls = analyze(w, starts, f)
        for a, kind, off, mn in acc:
            print(f"{a:08X}  {kind} {mn:5} +{off:#06x}")
        for a, t, dev_arg in calls:
            print(f"{a:08X}  call {t:08X}{'  (device in r3)' if dev_arg else ''}")
        return
    hi = (DEVICE_GLOBAL + 0x8000) >> 16
    funcs = set()
    for i in range((CODE_BEGIN - BASE) // 4, (CODE_END - BASE) // 4 - 12):
        x = w[i]
        if x >> 26 == 15 and (x >> 16) & 31 == 0 and x & 0xFFFF == hi & 0xFFFF:
            funcs.add(starts[bisect.bisect_right(starts, BASE + i * 4) - 1])
    for f in sorted(funcs):
        acc, calls = analyze(w, starts, f)
        if not acc and not calls:
            continue
        fields = sorted({(off, kind) for _, kind, off, _ in acc})
        print(f"{f:08X}  calls {' '.join(sorted({f'{t:08X}' for _, t, _ in calls})) or '-'}")
        if fields:
            print("          fields " + " ".join(f"{k}{off:#x}" for off, k in fields))


if __name__ == "__main__":
    main()
