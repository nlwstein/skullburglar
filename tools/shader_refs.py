# /// script
# requires-python = ">=3.11"
# dependencies = ["lzallright>=0.2", "xxhash>=3"]
# ///
"""Pipeline masters without shader microcode: describe each shader by where it is on the game disc.

    uv run tools/shader_refs.py index MASTER OUT.refs --game game --image dumps/default_image.bin [--decompressed DIR] [--disc]
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

`--disc` also references every other compiled shader on the disc (each cooked package and
GlobalShaders-Xbox360.bin), with no pipeline: the shaders the master hasn't seen, for a renderer that can compile them
without knowing their pairs (pipeline libraries, B-116). Each is referenced at its first location, exact, unpatched.

Disc container (XDK, big-endian): magic 0x102A1100 (pixel) / 0x102A1101 (vertex), virtual (header) size, physical
size; the header holds the constant table and the target string (vs_3_0 / ps_3_0); its dword 6 is the offset of
(literal block size, microcode size). The physical section is the literal block, then the microcode.

Patch dwords are a few instruction words of the game's microcode: `--no-patches` leaves out every shader
that needs them, for a refs file with nothing copied from the disc.
"""
from __future__ import annotations

import argparse
import re
import struct
import sys
from pathlib import Path

import xxhash

sys.path.insert(0, str(Path(__file__).resolve().parent))
from upk_decompress import decompress_package  # noqa: E402

MAGIC = b"BOTSPR01"
MASTER_HEADER = 16
HEAD = 32  # bytes of a shader's start that a near match must share (control-flow header)
CONTAINER = re.compile(rb"\x10\x2a\x11[\x00-\x03]")


def shader_hash(code: bytes, type_: int) -> int:
    return xxhash.xxh3_64_intdigest(code) ^ type_


def disc_shaders(data):
    """(hash, type, offset, microcode) for every well-formed shader container in a decompressed package."""
    for m in CONTAINER.finditer(data):
        at = m.start()
        vsize, psize = struct.unpack_from(">II", data, at + 4)
        if not (0x20 <= vsize < 0x10000 and 0 < psize < 0x40000 and psize % 4 == 0 and at + vsize + psize <= len(data)):
            continue
        head = data[at:at + vsize]
        type_ = 0 if b"vs_3_0" in head else 1 if b"ps_3_0" in head else None
        if type_ is None:
            continue
        info = struct.unpack_from(">I", data, at + 24)[0]
        lit, size = struct.unpack_from(">II", data, at + info) if info + 8 <= vsize else (0, psize)
        if lit + size != psize:
            lit, size = 0, psize
        offset = at + vsize + lit
        code = data[offset:offset + size]
        yield shader_hash(code, type_), type_, offset, code


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
    extra = {}  # --disc: hash -> (type, dwords, source, offset) of disc shaders the master doesn't have
    if args.disc:
        # One pass over the parsed containers: exact matches by hash, patched vertex shaders against the disc vertex
        # shaders of the same length and head. Only what's left gets the byte search below.
        vertex = {}  # (length, head) -> {hash: (source, offset, microcode)}, first location of each
        for name in sources.names():
            if name == "image":
                continue
            for h, type_, offset, code in disc_shaders(sources.get(name)):
                if h in shaders:
                    found.setdefault(h, (name, offset, []))
                elif h not in extra:
                    extra[h] = (type_, len(code) // 4, name, offset)
                if type_ == 0:
                    vertex.setdefault((len(code), code[:HEAD]), {}).setdefault(h, (name, offset, code))
        for h, (type_, code) in shaders.items():
            if h in found or type_ != 0:
                continue
            n = len(code) // 4
            for name, offset, other in vertex.get((len(code), code[:HEAD]), {}).values():
                diff = [i for i in range(n) if other[i * 4:i * 4 + 4] != code[i * 4:i * 4 + 4]]
                if len(diff) <= n // 3 and (h not in found or len(diff) < len(found[h][2])):
                    found[h] = (name, offset, [(i, code[i * 4:i * 4 + 4]) for i in diff])
    for name in sources.names():
        todo = [h for h in shaders if h not in found or (found[h][2] and not args.disc)]
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
        for h, (type_, dwords, name, offset) in extra.items():
            src = name.encode()
            f.write(b"R" + struct.pack("<QIIH", h, type_, dwords, len(src)) + src + struct.pack("<IH", offset, 0))
        for record in pipelines:
            f.write(record)
    near = sum(1 for v in found.values() if v[2])
    missing = [f"{h:016X}" for h in shaders if h not in found]
    print(f"{args.out}: {kept}/{len(shaders)} shaders referenced ({len(found) - near} exact, {near} patched"
          f"{', patched left out' if args.no_patches else ''}), {len(pipelines)} pipelines"
          f"{f', + {len(extra)} more disc shaders' if args.disc else ''}")
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
            p.add_argument("--disc", action="store_true", help="also reference every other shader on the disc")
        p.set_defaults(fn=fn)
    p = sub.add_parser("stats")
    p.add_argument("refs", nargs="+")
    p.set_defaults(fn=stats)
    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
