"""Native renderer pipeline manifests (src/gfx/vk/manifest.cpp): merge into a master, or show stats.

    python3 tools/pipeline_manifest.py merge OUT IN [IN...]
    python3 tools/pipeline_manifest.py stats IN [IN...]

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
    args = ap.parse_args()
    if args.cmd == "merge":
        merge(args.out, args.inputs)
    else:
        stats(args.inputs)


if __name__ == "__main__":
    main()
