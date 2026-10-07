"""Read a D3D frame capture (BOTS_GFX_CAPTURE, src/gfx/capture.cpp).

    python3 tools/gfxcap.py summary CAPDIR        # calls per entry point, draws, memory
    python3 tools/gfxcap.py calls CAPDIR [N]      # first N calls (default all), with arguments
    python3 tools/gfxcap.py pm4 CAPDIR [FIRST [N]] # calls with the PM4 packets each emitted (GFXCAP_FULL=1: every word)
    python3 tools/gfxcap.py mem CAPDIR ADDR [LEN] # guest bytes at a virtual address, or pADDR for physical

Names come from docs/knowledge/d3d-runtime.md (outside this repo); NAMES below keeps the ones the
replayer depends on.
"""
import collections
import json
import os
import struct
import sys

CALL = struct.Struct("<6IQ8Q8d8IQdQQ")
REGION = struct.Struct("<4IQ")

NAMES = {
    0x82317110: "Swap",
    0x82317108: "SyncPresent",
    0x8231D5B8: "Resolve",
    0x82333720: "BeginTiling",
    0x82333AB8: "EndTiling",
    0x823335A8: "SetPredication",
    0x8231C9C0: "Clear?",
    0x82335478: "DrawIndexedVertices",
    0x82335088: "DrawVertices",
    0x82334A70: "DrawVerticesUP",
    0x82334FE8: "DrawIndexedVerticesUP",
    0x823345C8: "BeginVertices",
    0x82334AB8: "BeginIndexedVertices",
    0x82334A60: "EndVertices",
    0x823136F8: "SetIndices",
    0x8231B270: "SetFetchConstant",
    0x82313550: "SetTexture",
    0x823216B0: "SetVSConstantF",
    0x82321790: "SetPSConstantF",
    0x823222C0: "SetVertexShader?",
    0x82321E58: "SetPixelShader?",
    0x82313448: "SetViewport",
    0x823134E8: "GetViewport",
    0x823143F8: "GetRenderTarget",
    0x82314438: "GetDepthStencilSurface",
    0x8232BF00: "Query_GetData",
    0x8232BC78: "Query_Issue",
    0x82315650: "Release",
    0x823155D8: "AddRef",
    0x82310E60: "RS_FillMode",
    0x82310E30: "RS_CullMode",
    0x82311A80: "RS_DepthBias",
    0x823119B8: "RS_SlopeScaleDepthBias",
}


# Type-3 opcodes (the SDK's xenos.h).
PM4_OPS = {
    0x10: "NOP", 0x21: "REG_RMW", 0x22: "DRAW_INDX", 0x23: "VIZ_QUERY", 0x26: "WAIT_FOR_IDLE", 0x27: "IM_LOAD",
    0x2B: "IM_LOAD_IMMEDIATE", 0x2D: "SET_CONSTANT", 0x2E: "LOAD_CONSTANT_CONTEXT", 0x2F: "LOAD_ALU_CONSTANT",
    0x36: "DRAW_INDX_2", 0x37: "INDIRECT_BUFFER_PFD", 0x3B: "INVALIDATE_STATE", 0x3C: "WAIT_REG_MEM",
    0x3D: "MEM_WRITE", 0x3E: "REG_TO_MEM", 0x3F: "INDIRECT_BUFFER", 0x44: "COND_EXEC", 0x45: "COND_WRITE",
    0x46: "EVENT_WRITE", 0x48: "ME_INIT", 0x4A: "SET_SHADER_BASES", 0x50: "SET_BIN_MASK", 0x51: "SET_BIN_SELECT",
    0x52: "WAIT_REG_EQ", 0x53: "WAIT_REG_GTE", 0x54: "INTERRUPT", 0x55: "SET_CONSTANT2",
    0x56: "SET_SHADER_CONSTANTS", 0x58: "EVENT_WRITE_SHD", 0x59: "EVENT_WRITE_CFL", 0x5A: "EVENT_WRITE_EXT",
    0x5B: "EVENT_WRITE_ZPD", 0x5E: "CONTEXT_UPDATE", 0x60: "SET_BIN_MASK_LO", 0x61: "SET_BIN_MASK_HI",
    0x62: "SET_BIN_SELECT_LO", 0x63: "SET_BIN_SELECT_HI",
}
# SET_CONSTANT type -> register file base (Xenos): ALU, fetch, bool, loop, registers.
CONST_BASE = {0: 0x4000, 1: 0x4800, 2: 0x4900, 3: 0x4908, 4: 0x2000}


def pm4_packets(data):
    """Yields (dword offset, text) for big-endian PM4 dwords."""
    full = bool(os.environ.get("GFXCAP_FULL"))  # whole register / constant blocks, not the first 8 words
    n = len(data) // 4
    d = struct.unpack(f">{n}I", data[: n * 4])
    i = 0
    while i < n:
        h = d[i]
        t = h >> 30
        if t == 0:
            count = ((h >> 16) & 0x3FFF) + 1
            reg, one = h & 0x7FFF, (h >> 15) & 1
            vals = d[i + 1 : i + 1 + count]
            yield i, f"type0 {reg:04X}{'' if one else '+'} = " + " ".join(f"{v:08X}" for v in (vals if full else vals[:8])) + \
                (" …" if count > 8 and not full else "")
            i += 1 + count
        elif t == 1:
            yield i, f"type1 {h & 0x7FF:04X}={d[i + 1]:08X} {(h >> 11) & 0x7FF:04X}={d[i + 2]:08X}"
            i += 3
        elif t == 2:
            i += 1
        else:
            op, count = (h >> 8) & 0x7F, ((h >> 16) & 0x3FFF) + 1
            body = d[i + 1 : i + 1 + count]
            name = PM4_OPS.get(op, f"op{op:02X}")
            if op == 0x2D and body:
                kind, index = (body[0] >> 16) & 0xFF, body[0] & 0x7FF
                reg = CONST_BASE.get(kind, 0) + index
                text = f"SET_CONSTANT {reg:04X}+{count - 1}: " + " ".join(f"{v:08X}" for v in (body[1:] if full else body[1:7])) + \
                    (" …" if count > 7 and not full else "")
            else:
                text = f"{name} " + " ".join(f"{v:08X}" for v in (body if full else body[:8])) + \
                    (" …" if count > 8 and not full else "")
            yield i, text
            i += 1 + count


def load(capdir):
    meta = json.load(open(os.path.join(capdir, "meta.json")))
    raw = open(os.path.join(capdir, "calls.bin"), "rb").read()
    assert meta["record_bytes"] == CALL.size, (meta["record_bytes"], CALL.size)
    calls = []
    for i in range(0, len(raw), CALL.size):
        v = CALL.unpack_from(raw, i)
        calls.append({
            "entry": v[0], "lr": v[1], "thread": v[2], "blob_count": v[3], "depth": v[4], "blob_offset": v[6],
            "r": v[7:15], "f": v[15:23], "stack": v[23:31], "ret_r3": v[31], "ret_f1": v[32],
            "pm4_begin": v[33], "pm4_end": v[34],
        })
    return meta, calls


def name(entry):
    return NAMES.get(entry, f"sub_{entry:08X}")


def summary(capdir):
    meta, calls = load(capdir)
    by = collections.Counter(c["entry"] for c in calls)
    threads = collections.Counter(c["thread"] for c in calls)
    print(f"frame {meta['frame']}: {len(calls)} calls, threads {dict(threads)}, device {meta['device']:08X}, "
          f"front buffer {meta['front_buffer']:08X}, memory {meta['mem_bytes'] >> 20} MB")
    for e, n in by.most_common():
        print(f"  {e:08X} {name(e):24} {n}")


def calls_cmd(capdir, limit):
    _, calls = load(capdir)
    for i, c in enumerate(calls[:limit]):
        args = " ".join(f"{x & 0xFFFFFFFF:X}" for x in c["r"][1:6])
        blobs = f" blobs={c['blob_count']}" if c["blob_count"] else ""
        print(f"{i:6} t{c['thread']} {name(c['entry']):24} ({args}) -> {c['ret_r3'] & 0xFFFFFFFF:X}{blobs}"
              f"  lr={c['lr']:08X}")


def own_pm4(calls, i):
    """Byte ranges call i emitted itself: its range minus those of the calls nested in it."""
    c = calls[i]
    ranges, pos = [], c["pm4_begin"]
    for j in range(i + 1, len(calls)):
        d = calls[j]
        if d["thread"] != c["thread"]:
            continue
        if d["depth"] <= c["depth"]:
            break
        if d["depth"] == c["depth"] + 1:
            if d["pm4_begin"] > pos:
                ranges.append((pos, d["pm4_begin"]))
            pos = max(pos, d["pm4_end"])
    if c["pm4_end"] > pos:
        ranges.append((pos, c["pm4_end"]))
    return ranges


def pm4_cmd(capdir, first, count):
    """Calls first..first+count with the PM4 each emitted itself, indented by nesting depth."""
    _, calls = load(capdir)
    data = open(os.path.join(capdir, "pm4.bin"), "rb").read()
    for i, c in enumerate(calls[first : first + count], first):
        args = " ".join(f"{x & 0xFFFFFFFF:X}" for x in c["r"][1:6])
        pad = "  " * c["depth"]
        print(f"{i:6} {pad}{name(c['entry'])} ({args})")
        for lo, hi in own_pm4(calls, i):
            for off, text in pm4_packets(data[lo:hi]):
                print(f"         {pad}{lo // 4 + off:5} {text}")


def mem(capdir, addr, length):
    """addr: guest virtual address, or a physical one as pADDR (the 0xA0000000+ views are read as physical)."""
    physical = addr.startswith("p")
    addr = int(addr.lstrip("p"), 16)
    if not physical and addr >= 0xA0000000:
        physical, addr = True, (addr & 0x1FFFFFFF) + (0x1000 if addr >= 0xE0000000 else 0)
    idx = open(os.path.join(capdir, "mem.idx"), "rb").read()
    with open(os.path.join(capdir, "mem.bin"), "rb") as f:
        for i in range(0, len(idx), REGION.size):
            phys, a, size, _, off = REGION.unpack_from(idx, i)
            if bool(phys) == physical and a <= addr < a + size:
                f.seek(off + addr - a)
                print(f.read(min(length, a + size - addr)).hex(" ", 4))
                return
    print("not in the snapshot (zero page or not committed)")


def main():
    cmd, capdir = sys.argv[1], sys.argv[2]
    if cmd == "summary":
        summary(capdir)
    elif cmd == "calls":
        calls_cmd(capdir, int(sys.argv[3]) if len(sys.argv) > 3 else None)
    elif cmd == "pm4":
        pm4_cmd(capdir, int(sys.argv[3]) if len(sys.argv) > 3 else 0, int(sys.argv[4]) if len(sys.argv) > 4 else 50)
    elif cmd == "mem":
        mem(capdir, sys.argv[3], int(sys.argv[4], 0) if len(sys.argv) > 4 else 64)


if __name__ == "__main__":
    main()
