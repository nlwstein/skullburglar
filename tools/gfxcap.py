"""Read a D3D frame capture (BOTS_GFX_CAPTURE, src/gfx/capture.cpp).

    python3 tools/gfxcap.py summary CAPDIR        # calls per entry point, draws, memory
    python3 tools/gfxcap.py calls CAPDIR [N]      # first N calls (default all), with arguments
    python3 tools/gfxcap.py mem CAPDIR ADDR [LEN] # guest bytes at a virtual address (snapshot, then blobs)

Names come from docs/knowledge/d3d-runtime.md (outside this repo); NAMES below keeps the ones the
replayer depends on.
"""
import collections
import json
import os
import struct
import sys

CALL = struct.Struct("<4IQ8Q8d8IQd")
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


def load(capdir):
    meta = json.load(open(os.path.join(capdir, "meta.json")))
    raw = open(os.path.join(capdir, "calls.bin"), "rb").read()
    assert meta["record_bytes"] == CALL.size, (meta["record_bytes"], CALL.size)
    calls = []
    for i in range(0, len(raw), CALL.size):
        v = CALL.unpack_from(raw, i)
        calls.append({
            "entry": v[0], "lr": v[1], "thread": v[2], "blob_count": v[3], "blob_offset": v[4],
            "r": v[5:13], "f": v[13:21], "stack": v[21:29], "ret_r3": v[29], "ret_f1": v[30],
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


def mem(capdir, addr, length):
    idx = open(os.path.join(capdir, "mem.idx"), "rb").read()
    with open(os.path.join(capdir, "mem.bin"), "rb") as f:
        for i in range(0, len(idx), REGION.size):
            phys, a, size, _, off = REGION.unpack_from(idx, i)
            if not phys and a <= addr < a + size:
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
    elif cmd == "mem":
        mem(capdir, int(sys.argv[3], 16), int(sys.argv[4], 0) if len(sys.argv) > 4 else 64)


if __name__ == "__main__":
    main()
