"""Map the statically linked Xbox D3D runtime in the dumped image (native renderer groundwork).

The runtime is the code range D3D_LO..D3D_HI. For every function there it reports its size, the PM4 type-3
packets it builds (directly and through its callees in the range), the globals it loads, and its callers
outside the range — the API surface the game calls.

    python3 tools/d3d_scan.py summary            # entry points (called from outside), most-called first
    python3 tools/d3d_scan.py func 0x82315650    # one function: packets, callees, globals, callers
    python3 tools/d3d_scan.py callers 0x82315650 # every outside call site (caller function + address)
    python3 tools/d3d_scan.py packets            # PM4 packet builders by opcode
    python3 tools/d3d_scan.py entries > src/gfx/d3d_entries.inc   # hook list for the census

Needs dumps/default_image.bin (tools/ppcdis.py) and generated/default/codegen.partition.json (codegen).
"""
import bisect
import collections
import json
import struct
import sys

BASE = 0x82000000
CODE_BEGIN, CODE_END = 0x82310000, 0x83754C14
D3D_LO, D3D_HI = 0x82310000, 0x8234A000

PM4 = {0x10: "NOP", 0x21: "op21", 0x22: "DRAW_INDX", 0x23: "VIZ_QUERY", 0x25: "INVALIDATE_STATE",
       0x27: "IM_LOAD", 0x2B: "IM_LOAD_IMMEDIATE", 0x2D: "SET_CONSTANT", 0x2E: "LOAD_CONSTANT_CONTEXT",
       0x2F: "LOAD_ALU_CONSTANT", 0x36: "DRAW_INDX_2", 0x3B: "SET_BIN_MASK_LO", 0x3C: "WAIT_REG_MEM",
       0x3D: "REG_RMW", 0x3E: "COND_WRITE", 0x3F: "INDIRECT_BUFFER_PFD", 0x45: "op45", 0x46: "EVENT_WRITE",
       0x48: "ME_INIT", 0x58: "EVENT_WRITE_SHD", 0x5A: "op5A", 0x5B: "EVENT_WRITE_ZPD", 0x60: "SET_BIN_MASK_HI?",
       0x61: "SET_BIN_SELECT_LO?", 0x62: "SET_BIN_SELECT_HI?", 0x63: "op63"}


class Image:
    def __init__(self):
        img = open("dumps/default_image.bin", "rb").read()
        n = len(img) // 4
        self.w = struct.unpack(f">{n}I", img[: n * 4])
        self.starts = sorted(int(k, 16) for k in json.load(open("generated/default/codegen.partition.json"))["assignments"])
        self.start_set = set(self.starts)

    def word(self, a):
        return self.w[(a - BASE) // 4]

    def func_of(self, a):
        i = bisect.bisect_right(self.starts, a) - 1
        return self.starts[i] if i >= 0 else None

    def func_end(self, f):
        i = bisect.bisect_right(self.starts, f)
        return self.starts[i] if i < len(self.starts) else CODE_END


def simm(v):
    return v - 0x10000 if v & 0x8000 else v


def scan(im):
    """Per-function facts for the D3D range plus all bl edges into it from anywhere."""
    packets = collections.defaultdict(set)   # func -> {opcode}
    callees = collections.defaultdict(set)   # func -> {func in range}
    globals_ = collections.defaultdict(set)  # func -> {absolute address loaded via lis+lwz/addi}
    ext_sites = collections.defaultdict(list)  # target -> [(caller func, site)]
    for i in range((CODE_BEGIN - BASE) // 4, (CODE_END - BASE) // 4):
        x = im.w[i]
        pc = BASE + i * 4
        op = x >> 26
        if op == 18 and (x & 3) == 1:  # bl
            off = x & 0x03FFFFFC
            if off & 0x02000000:
                off -= 0x04000000
            dst = (pc + off) & 0xFFFFFFFF
            if D3D_LO <= dst < D3D_HI:
                if D3D_LO <= pc < D3D_HI:
                    callees[im.func_of(pc)].add(dst)
                else:
                    ext_sites[dst].append((im.func_of(pc), pc))
            continue
        if not (D3D_LO <= pc < D3D_HI) or op != 15 or ((x >> 16) & 31) != 0:
            continue
        rd, hi = (x >> 21) & 31, x & 0xFFFF
        f = im.func_of(pc)
        for k in range(1, 8):
            y = im.w[i + k]
            yop, a, b, lo = y >> 26, (y >> 21) & 31, (y >> 16) & 31, y & 0xFFFF
            if 0xC000 <= hi <= 0xC03F and ((yop == 24 and a == rd) or (yop == 14 and b == rd)):
                hdr = (hi << 16) | lo if yop == 24 else ((hi << 16) + simm(lo)) & 0xFFFFFFFF
                packets[f].add((hdr >> 8) & 0x7F)
                break
            if hi >= 0x8200 and yop in (32, 14) and b == rd:  # lwz / addi from a lis base: a global
                globals_[f].add(((hi << 16) + simm(lo)) & 0xFFFFFFFF)
                break
            if a == rd and yop not in (36, 38, 44, 52, 54):  # rd overwritten (stores don't count)
                break
    return packets, callees, globals_, ext_sites


def closure(f, callees, packets, seen=None):
    seen = set() if seen is None else seen
    if f in seen:
        return set()
    seen.add(f)
    out = set(packets.get(f, ()))
    for c in callees.get(f, ()):
        out |= closure(c, callees, packets, seen)
    return out


def names(ops):
    return ",".join(sorted(PM4.get(o, f"op{o:02X}") for o in ops)) or "-"


def main():
    im = Image()
    packets, callees, globals_, ext = scan(im)
    cmd = sys.argv[1] if len(sys.argv) > 1 else "summary"
    if cmd == "summary":
        funcs = [f for f in im.starts if D3D_LO <= f < D3D_HI]
        print(f"# {len(funcs)} functions in {D3D_LO:08X}-{D3D_HI:08X}; {len(ext)} called from outside")
        print("addr\tsize\text_sites\tcallers\tpackets(direct)\tpackets(transitive)")
        for f in sorted(ext, key=lambda f: -len(ext[f])):
            print(f"{f:08X}\t{im.func_end(f) - f}\t{len(ext[f])}\t{len({c for c, _ in ext[f]})}\t"
                  f"{names(packets.get(f, ()))}\t{names(closure(f, callees, packets))}")
    elif cmd == "func":
        f = im.func_of(int(sys.argv[2], 16))
        print(f"{f:08X} size {im.func_end(f) - f}")
        print("  packets:", names(packets.get(f, ())), "| transitive:", names(closure(f, callees, packets)))
        print("  callees:", " ".join(f"{c:08X}" for c in sorted(callees.get(f, ()))))
        print("  globals:", " ".join(f"{g:08X}" for g in sorted(globals_.get(f, ()))))
        print("  callers:", " ".join(sorted({f"{c:08X}" for c, _ in ext.get(f, [])})))
    elif cmd == "callers":
        for c, site in ext.get(int(sys.argv[2], 16), []):
            print(f"{c:08X}\t{site:08X}")
    elif cmd == "entries":  # the hook list for src/gfx/d3d_census.cpp
        print("// Xbox D3D runtime functions called from outside the runtime (tools/d3d_scan.py entries).")
        for f in sorted(ext):
            print(f"D3D_ENTRY({f:08X})")
    elif cmd == "packets":
        by_op = collections.defaultdict(list)
        for f, ops in packets.items():
            for o in ops:
                by_op[o].append(f)
        for o in sorted(by_op):
            print(f"{PM4.get(o, f'op{o:02X}'):22} " + " ".join(f"{f:08X}" for f in sorted(by_op[o])))


if __name__ == "__main__":
    main()
