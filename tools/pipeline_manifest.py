"""Native renderer pipeline manifests (src/gfx/vk/manifest.cpp): merge into a master, or show stats.

    python3 tools/pipeline_manifest.py merge OUT IN [IN...]
    python3 tools/pipeline_manifest.py stats IN [IN...]
    python3 tools/pipeline_manifest.py convert OUT IN --class N

`convert` rewrites a manifest for a device class with fewer features (e.g. class 7 -> 3: no cull distance, as on
MoltenVK), so one playthrough on any GPU can feed every class's master.

A manifest is `<cache>/native/<title>.manifest.bin`, written by every native-renderer run. Format:
  header   8-byte magic ('BOTSPM02'), u32 record size, u32 device feature class
  'S'      u64 shader hash, u32 type, u32 dword count, ucode dwords (big-endian guest microcode)
  'P'      one pipeline record (record-size bytes)
Manifests of the same header (magic, record size and feature class) can be merged: shaders by
hash, pipelines by their bytes, first occurrence wins, order kept. Inputs with another header are
reported and skipped. Put the result next to a manifest as `<title>.master.bin`, and runs warm its
pipelines too. A torn last chunk (killed run) is ignored.

Manifests hold the game's shader microcode, so they are game data: never commit or publish them.
"""
import argparse
import struct
import sys

HEADER = 16


def read(path):
    data = open(path, "rb").read()
    if len(data) < HEADER or not data.startswith(b"BOTSPM"):
        raise ValueError(f"{path}: not a pipeline manifest")
    header = data[:HEADER]
    record_size = struct.unpack_from("<I", data, 8)[0]
    shaders, pipelines, pos = {}, [], HEADER
    while pos < len(data):
        tag = data[pos:pos + 1]
        if tag == b"S" and pos + 17 <= len(data):
            hash_, _type, dwords = struct.unpack_from("<QII", data, pos + 1)
            end = pos + 17 + dwords * 4
            if end > len(data):
                break
            shaders.setdefault(hash_, data[pos:end])
            pos = end
        elif tag == b"P" and pos + 1 + record_size <= len(data):
            pipelines.append(data[pos:pos + 1 + record_size])
            pos += 1 + record_size
        else:
            break
    return header, shaders, pipelines


def describe(header):
    record_size, features = struct.unpack_from("<II", header, 8)
    return f"{header[:8].decode(errors='replace')} record {record_size} B, class {features}"


def shader_hashes(record):
    # Record: 'P', then u64 vs hash, u64 vs mod, u64 ps hash, u64 ps mod, ...
    vs, _, ps, _ = struct.unpack_from("<QQQQ", record, 1)
    return [vs] + ([ps] if ps else [])


def merge(out, inputs):
    header, shaders, pipelines, seen = None, {}, [], set()
    for path in inputs:
        h, s, p = read(path)
        if header is None:
            header = h
        elif h != header:
            print(f"skipped {path}: {describe(h)} (merging {describe(header)})", file=sys.stderr)
            continue
        new = [r for r in p if r not in seen]
        seen.update(new)
        pipelines += new
        for k, v in s.items():
            shaders.setdefault(k, v)
        print(f"{path}: {len(p)} pipelines, {len(new)} new")
    if header is None:
        sys.exit("no inputs")
    with open(out, "wb") as f:
        f.write(header)
        written = set()
        for record in pipelines:
            hashes = shader_hashes(record)
            if any(h not in shaders for h in hashes):
                continue  # its shader chunk was lost (torn file): can't be warmed anyway
            for h in hashes:
                if h not in written:
                    f.write(shaders[h])
                    written.add(h)
            f.write(record)
    print(f"{out}: {len(pipelines)} pipelines, {len(written)} shaders ({describe(header)})")


# Device feature class bits (manifest.cpp LoadManifest) and where they show up in a record (after the 'P' tag):
# the vertex shader's translation modification (SpirvShaderTranslator::Modification::vertex: interpolator mask 0-15,
# point parameters 16, dynamic registers 17-24, user clip plane count 25-27, user_clip_plane_cull 28,
# vertex_kill_and 29) and PipelineKey::depth_clamp (the 6th state dword, after the 48-byte record header).
DEPTH_CLAMP, CLIP_DISTANCE, CULL_DISTANCE = 1, 2, 4
VS_MOD, DEPTH_CLAMP_AT = 1 + 8, 1 + 48 + 5 * 4


def convert_record(record, target):
    """A record of a class with more features as draw.cpp would have keyed it on a device of class `target`."""
    r = bytearray(record)
    mod = struct.unpack_from("<Q", r, VS_MOD)[0]
    cull = (mod >> 28) & 1
    if not target & CULL_DISTANCE:
        cull = 0
        mod &= ~((1 << 28) | (1 << 29))
    if not cull and not target & CLIP_DISTANCE:
        mod &= ~(0b111 << 25)
    struct.pack_into("<Q", r, VS_MOD, mod)
    if not target & DEPTH_CLAMP:
        struct.pack_into("<I", r, DEPTH_CLAMP_AT, 0)
    return bytes(r)


def convert(out, path, target):
    header, shaders, pipelines = read(path)
    source = struct.unpack_from("<I", header, 12)[0]
    if target & ~source:
        sys.exit(f"{path}: class {source} lacks features of class {target}; only a superset converts down")
    new_header = header[:12] + struct.pack("<I", target)
    converted = list(dict.fromkeys(convert_record(p, target) for p in pipelines))
    with open(out, "wb") as f:
        f.write(new_header)
        written = set()
        for record in converted:
            for h in shader_hashes(record):
                if h in shaders and h not in written:
                    f.write(shaders[h])
                    written.add(h)
            if all(h in written for h in shader_hashes(record)):
                f.write(record)
    print(f"{out}: {len(converted)} pipelines (from {len(pipelines)}), {len(written)} shaders, class {source} -> {target}")


def stats(inputs):
    for path in inputs:
        h, s, p = read(path)
        print(f"{path}: {len(p)} pipelines, {len(s)} shaders ({describe(h)})")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    m = sub.add_parser("merge")
    m.add_argument("out")
    m.add_argument("inputs", nargs="+")
    st = sub.add_parser("stats")
    st.add_argument("inputs", nargs="+")
    c = sub.add_parser("convert")
    c.add_argument("out")
    c.add_argument("input")
    c.add_argument("--class", dest="target", type=int, required=True)
    args = ap.parse_args()
    if args.cmd == "merge":
        merge(args.out, args.inputs)
    elif args.cmd == "convert":
        convert(args.out, args.input, args.target)
    else:
        stats(args.inputs)


if __name__ == "__main__":
    main()
