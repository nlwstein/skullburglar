# /// script
# requires-python = ">=3.11"
# dependencies = ["lzallright>=0.2", "xxhash>=3"]
# ///
"""Pipeline masters without shader microcode: describe each shader by where it is on the game disc.

    uv run tools/shader_refs.py index MASTER OUT.refs --game game --image dumps/default_image.bin [--decompressed DIR]
    uv run tools/shader_refs.py rebuild IN.refs OUT.master.bin --game game --image dumps/default_image.bin
    uv run tools/shader_refs.py stats IN.refs

A master (tools/pipeline_manifest.py) carries every shader's microcode, which is game data. A refs file
keeps the master's header and pipeline records and replaces each shader chunk with a reference:

  header  8-byte magic 'BOTSPR01', then the master's 16-byte header
  'R'     u64 shader hash, u32 type, u32 dword count, u16 source length, source (UTF-8), u32 byte
          offset, u16 patch count, patch count x (u16 dword index, u32 dword)
  'P'     one pipeline record, as in the master

The source is `image` (the loaded XEX image, e.g. a BOTS_DUMP_IMAGE dump) or a path under the game root
(`C1Game/CookedXenon/Engine.xxx`); offsets into a cooked package are into its decompressed body
(tools/upk_decompress.py). Patches are the dwords that differ from the disc copy: the XDK rewrites a
vertex shader's vfetch block from the bound vertex declaration, so 2-35 dwords of a vertex shader are
usually patched. Shaders found nowhere are left out, and `rebuild` drops pipelines whose shaders it
cannot rebuild. Every rebuilt shader is checked against its hash (XXH3-64 of the microcode ^ type).

Patch dwords are a few instruction words of the game's microcode: `--no-patches` leaves out every shader
that needs them, for a refs file with nothing copied from the disc.
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

import xxhash

sys.path.insert(0, str(Path(__file__).resolve().parent))
from upk_decompress import decompress_package  # noqa: E402

MAGIC = b"BOTSPR01"
MASTER_HEADER = 16
HEAD = 32  # bytes of a shader's start that a near match must share (control-flow header)


def shader_hash(code: bytes, type_: int) -> int:
    return xxhash.xxh3_64_intdigest(code) ^ type_


def read_master(path):
    data = Path(path).read_bytes()
    if not data.startswith(b"BOTSPM"):
        raise SystemExit(f"{path}: not a pipeline manifest")
    record_size = struct.unpack_from("<I", data, 8)[0]
    shaders, pipelines, pos = {}, [], MASTER_HEADER
    while pos < len(data):
        tag = data[pos:pos + 1]
        if tag == b"S" and pos + 17 <= len(data):
            hash_, type_, dwords = struct.unpack_from("<QII", data, pos + 1)
            end = pos + 17 + dwords * 4
            if end > len(data):
                break
            shaders.setdefault(hash_, (type_, data[pos + 17:end]))
            pos = end
        elif tag == b"P" and pos + 1 + record_size <= len(data):
            pipelines.append(data[pos:pos + 1 + record_size])
            pos += 1 + record_size
        else:
            break
    return data[:MASTER_HEADER], shaders, pipelines


class Sources:
    """The files shaders come from, loaded on demand (cooked packages decompressed)."""

    def __init__(self, game, image, decompressed=None):
        self.game, self.image = Path(game), image and Path(image)
        self.decompressed = decompressed and Path(decompressed)
        self.cache = {}

    def names(self):
        cooked = sorted(p.relative_to(self.game).as_posix()
                        for p in (self.game / "C1Game" / "CookedXenon").glob("*")
                        if p.suffix in (".xxx", ".bin"))
        # Always-loaded sources first, so references cluster in a few files.
        first = ["C1Game/CookedXenon/GlobalShaders-Xbox360.bin", "C1Game/CookedXenon/Engine.xxx"]
        return (["image"] if self.image else []) + [n for n in first if n in cooked] + \
            [n for n in cooked if n not in first]

    def get(self, name):
        if name not in self.cache:
            if len(self.cache) > 8:
                self.cache.pop(next(iter(self.cache)))
            self.cache[name] = self._load(name)
        return self.cache[name]

    def _load(self, name):
        if name == "image":
            if not self.image:
                raise FileNotFoundError("a shader comes from the XEX image: pass --image")
            return self.image.read_bytes()
        path = self.game / name
        if path.suffix != ".xxx":
            return path.read_bytes()
        if self.decompressed and (self.decompressed / path.name).exists():
            return (self.decompressed / path.name).read_bytes()
        return decompress_package(path.read_bytes())


def find(code, data):
    """(offset, patches) of `code` in `data`: exact, else the same-length blob sharing its head with the
    fewest differing dwords (at most a third)."""
    at = data.find(code)
    if at >= 0:
        return at, []
    n = len(code) // 4
    if n <= HEAD // 4:
        return None
    best, at, tries = None, data.find(code[:HEAD]), 0
    while at >= 0 and tries < 64 and at + len(code) <= len(data):
        diff = [i for i in range(n) if data[at + i * 4:at + i * 4 + 4] != code[i * 4:i * 4 + 4]]
        if len(diff) <= n // 3 and (best is None or len(diff) < len(best[1])):
            best = (at, [(i, code[i * 4:i * 4 + 4]) for i in diff])
        at, tries = data.find(code[:HEAD], at + 1), tries + 1
    return best


def index(args):
    header, shaders, pipelines = read_master(args.master)
    sources = Sources(args.game, args.image, args.decompressed)
    found = {}  # hash -> (source, offset, patches); exact matches replace near ones
    for name in sources.names():
        todo = [h for h in shaders if h not in found or found[h][2]]
        if not todo:
            break
        data = sources.get(name)
        for h in todo:
            hit = find(shaders[h][1], data)
            if hit and (h not in found or len(hit[1]) < len(found[h][2])):
                found[h] = (name, hit[0], hit[1])
    with open(args.out, "wb") as f:
        f.write(MAGIC + header)
        kept = 0
        for h, (type_, code) in shaders.items():
            if h not in found or (args.no_patches and found[h][2]):
                continue
            name, offset, patches = found[h]
            src = name.encode()
            f.write(b"R" + struct.pack("<QIIH", h, type_, len(code) // 4, len(src)) + src)
            f.write(struct.pack("<IH", offset, len(patches)))
            for i, dword in patches:
                f.write(struct.pack("<H", i) + dword)
            kept += 1
        for record in pipelines:
            f.write(record)
    near = sum(1 for v in found.values() if v[2])
    missing = [f"{h:016X}" for h in shaders if h not in found]
    print(f"{args.out}: {kept}/{len(shaders)} shaders referenced ({len(found) - near} exact, {near} patched"
          f"{', patched left out' if args.no_patches else ''}), {len(pipelines)} pipelines")
    if missing:
        print("not found:", " ".join(missing))


def read_refs(path):
    data = Path(path).read_bytes()
    if not data.startswith(MAGIC):
        raise SystemExit(f"{path}: not a refs file")
    header = data[len(MAGIC):len(MAGIC) + MASTER_HEADER]
    record_size = struct.unpack_from("<I", header, 8)[0]
    refs, pipelines, pos = [], [], len(MAGIC) + MASTER_HEADER
    while pos < len(data):
        tag = data[pos:pos + 1]
        if tag == b"R":
            h, type_, dwords, n = struct.unpack_from("<QIIH", data, pos + 1)
            pos += 19
            name = data[pos:pos + n].decode()
            offset, count = struct.unpack_from("<IH", data, pos + n)
            pos += n + 6
            patches = [(struct.unpack_from("<H", data, pos + i * 6)[0], data[pos + i * 6 + 2:pos + i * 6 + 6])
                       for i in range(count)]
            pos += count * 6
            refs.append((h, type_, dwords, name, offset, patches))
        elif tag == b"P":
            pipelines.append(data[pos:pos + 1 + record_size])
            pos += 1 + record_size
        else:
            raise SystemExit(f"{path}: bad chunk at {pos:#x}")
    return header, refs, pipelines


def rebuild(args):
    header, refs, pipelines = read_refs(args.refs)
    sources = Sources(args.game, args.image, args.decompressed)
    chunks, bad = {}, []
    for h, type_, dwords, name, offset, patches in sorted(refs, key=lambda r: r[3]):
        try:
            code = bytearray(sources.get(name)[offset:offset + dwords * 4])
        except OSError as e:
            bad.append(f"{h:016X} ({e})")
            continue
        for i, dword in patches:
            code[i * 4:i * 4 + 4] = dword
        if len(code) != dwords * 4 or shader_hash(bytes(code), type_) != h:
            bad.append(f"{h:016X} ({name}: hash mismatch)")
            continue
        chunks[h] = b"S" + struct.pack("<QII", h, type_, dwords) + bytes(code)
    with open(args.out, "wb") as f:
        f.write(header)
        written, kept = set(), 0
        for record in pipelines:
            vs, _, ps, _ = struct.unpack_from("<QQQQ", record, 1)
            hashes = [vs] + ([ps] if ps else [])
            if any(x not in chunks for x in hashes):
                continue
            for x in hashes:
                if x not in written:
                    f.write(chunks[x])
                    written.add(x)
            f.write(record)
            kept += 1
    print(f"{args.out}: {len(chunks)}/{len(refs)} shaders rebuilt, {kept}/{len(pipelines)} pipelines")
    if bad:
        print("failed:", "; ".join(bad))
        sys.exit(1)


def stats(args):
    for path in args.refs:
        header, refs, pipelines = read_refs(path)
        files = {}
        for r in refs:
            files[r[3]] = files.get(r[3], 0) + 1
        patched = sum(1 for r in refs if r[5])
        top = ", ".join(f"{k.rsplit('/', 1)[-1]} {v}" for k, v in sorted(files.items(), key=lambda kv: -kv[1])[:6])
        print(f"{path}: {len(refs)} shaders ({patched} patched, {sum(len(r[5]) for r in refs)} dwords) from "
              f"{len(files)} files [{top}], {len(pipelines)} pipelines, class {struct.unpack_from('<I', header, 12)[0]}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name, fn in (("index", index), ("rebuild", rebuild)):
        p = sub.add_parser(name)
        p.add_argument("master" if name == "index" else "refs")
        p.add_argument("out")
        p.add_argument("--game", required=True, help="game root (the extracted disc)")
        p.add_argument("--image", help="the loaded XEX image (BOTS_DUMP_IMAGE)")
        p.add_argument("--decompressed", help="directory of already-decompressed packages (same names)")
        if name == "index":
            p.add_argument("--no-patches", action="store_true", help="leave out shaders that need patch dwords")
        p.set_defaults(fn=fn)
    p = sub.add_parser("stats")
    p.add_argument("refs", nargs="+")
    p.set_defaults(fn=stats)
    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
