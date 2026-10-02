"""Merge ReXGlue persistent shader caches (B-053): union of several runs' / machines' storage files.

    uv run --with xxhash python tools/shader_cache_merge.py OUT_DIR IN_DIR [IN_DIR...]
    uv run --with xxhash python tools/shader_cache_merge.py --stats IN_DIR [IN_DIR...]

Each IN_DIR is a `cache/shaders/shareable` directory (or any directory holding the files):
  <title>.xsh                    guest (Xbox 360) shader microcode, shared by every backend:
                                 8-byte header ('XESH', version) + records
                                 [u64 ucode hash][u32 dword count:31 | type:1][ucode dwords]
  <title>.<path>.<api>.xpso      pipeline descriptions for one backend + render path
                                 (rov.d3d12, rtv.d3d12, fsi.vk, fbo.vk): 12-byte header
                                 ('XEPS', api magic, version) + fixed-size records
                                 [u64 XXH3 of description][description]
Records are deduplicated by hash, first occurrence wins, order kept (the SDK creates pipelines in
file order at launch). The xpso record size is found from the hash itself, so the tool doesn't
depend on the SDK's struct layout. Files whose headers (version / api) differ are not mixed: the
first input's header wins and mismatching inputs are reported and skipped.
"""
import argparse
import os
import struct
import sys
from collections import defaultdict

import xxhash


def read_xsh(path):
    data = open(path, "rb").read()
    header, pos, records = data[:8], 8, []
    while pos + 12 <= len(data):
        h, packed = struct.unpack_from("<QI", data, pos)
        n = (packed & 0x7FFFFFFF) * 4
        if pos + 12 + n > len(data):
            break
        body = data[pos + 12 : pos + 12 + n]
        if xxhash.xxh3_64_intdigest(body) != h:
            break  # corrupt tail, same rule as the SDK loader
        records.append((h, data[pos : pos + 12 + n]))
        pos += 12 + n
    return header, records


def xpso_record_size(data):
    for size in range(9, 4096):  # packed structs: any size
        if 12 + size > len(data) or (len(data) - 12) % size:
            continue
        h = struct.unpack_from("<Q", data, 12)[0]
        if xxhash.xxh3_64_intdigest(data[20 : 12 + size]) == h:
            return size
    return None


def read_xpso(path):
    data = open(path, "rb").read()
    header = data[:12]
    if len(data) <= 12:
        return header, None, []
    size = xpso_record_size(data)
    if size is None:
        print(f"  ! {path}: can't determine record size, skipped", file=sys.stderr)
        return header, None, []
    records = []
    for pos in range(12, len(data) - size + 1, size):
        h = struct.unpack_from("<Q", data, pos)[0]
        if xxhash.xxh3_64_intdigest(data[pos + 8 : pos + size]) != h:
            break
        records.append((data[pos : pos + size], data[pos : pos + size]))
    return header, size, records


def collect(dirs):
    files = defaultdict(list)  # name -> [path]
    for d in dirs:
        for name in sorted(os.listdir(d)):
            if name.endswith((".xsh", ".xpso")):
                files[name].append(os.path.join(d, name))
    return files


def merge(name, paths):
    reader = read_xsh if name.endswith(".xsh") else read_xpso
    out_header, seen, out, sizes = None, set(), [], set()
    for p in paths:
        parsed = reader(p)
        header, records = parsed[0], parsed[-1]
        if name.endswith(".xpso") and parsed[1]:
            sizes.add(parsed[1])
        if out_header is None:
            out_header = header
        elif header != out_header:
            print(f"  ! {p}: header {header.hex()} != {out_header.hex()} (other SDK version?), "
                  f"skipped", file=sys.stderr)
            continue
        new = 0
        for key, blob in records:
            if key not in seen:
                seen.add(key)
                out.append(blob)
                new += 1
        print(f"  {p}: {len(records)} records, {new} new")
    if len(sizes) > 1:
        sys.exit(f"{name}: inputs disagree on record size {sizes}")
    return out_header, out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("--stats", action="store_true", help="only report counts, write nothing")
    ap.add_argument("dirs", nargs="+")
    a = ap.parse_args()
    out_dir, in_dirs = (None, a.dirs) if a.stats else (a.dirs[0], a.dirs[1:])
    if not in_dirs:
        ap.error("need at least one input directory")
    for name, paths in sorted(collect(in_dirs).items()):
        print(name)
        header, records = merge(name, paths)
        print(f"  => {len(records)} unique")
        if out_dir:
            os.makedirs(out_dir, exist_ok=True)
            tmp = os.path.join(out_dir, name + ".tmp")
            with open(tmp, "wb") as f:
                f.write(header)
                for blob in records:
                    f.write(blob)
            os.replace(tmp, os.path.join(out_dir, name))


if __name__ == "__main__":
    main()
