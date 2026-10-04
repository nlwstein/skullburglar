# /// script
# requires-python = ">=3.11"
# dependencies = ["lzallright>=0.2", "xxhash>=3"]
# ///
"""How much of the game's shader set the pipeline masters cover.

    uv run tools/shader_coverage.py --game game [--decompressed DIR] [--top N] MASTER_OR_MANIFEST [...]

Enumerates every compiled shader on the disc (each cooked package and GlobalShaders-Xbox360.bin), deduplicated by
the renderer's hash, and reports how many of them the given masters/manifests have seen: in total, by type, and per
package (least covered first). A master's vertex shaders carry the vfetch block the XDK patched in from the vertex
declaration, so they are matched to a disc shader of the same length and head with the fewest differing dwords
(tools/shader_refs.py `find`).

The disc's shader containers are parsed by tools/shader_refs.py `disc_shaders`.

Prints counts and package names only, nothing copied from the disc.
"""
from __future__ import annotations

import argparse
import collections
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from pipeline_manifest import read  # noqa: E402
from shader_refs import HEAD, Sources, disc_shaders  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("inputs", nargs="+", help="pipeline masters or manifests (any class)")
    ap.add_argument("--game", required=True, help="game root (the extracted disc)")
    ap.add_argument("--decompressed", help="directory of already-decompressed packages (same names)")
    ap.add_argument("--top", type=int, default=15, help="packages to list (least covered first)")
    args = ap.parse_args()

    seen = {}  # hash -> (type, microcode) from the inputs
    for path in args.inputs:
        _, shaders, _ = read(path)
        for chunk in shaders.values():
            hash_, type_, dwords = struct.unpack_from("<QII", chunk, 1)
            seen.setdefault(hash_, (type_, chunk[17:17 + dwords * 4]))

    sources = Sources(args.game, None, args.decompressed)
    disc = {}  # hash -> (type, microcode)
    packages = {}  # package name -> set of hashes
    for name in sources.names():
        hashes = set()
        for hash_, type_, _, code in disc_shaders(sources.get(name)):
            disc.setdefault(hash_, (type_, code))
            hashes.add(hash_)
        if hashes:
            packages[Path(name).name] = hashes

    vs_index = collections.defaultdict(list)
    for h, (type_, code) in disc.items():
        if type_ == 0:
            vs_index[(len(code), code[:HEAD])].append(h)
    covered, unmatched = set(), 0
    for h, (type_, code) in seen.items():
        if h in disc:
            covered.add(h)
            continue
        best = None
        if type_ == 0:
            n = len(code) // 4
            for c in vs_index.get((len(code), code[:HEAD]), ()):
                other = disc[c][1]
                diff = sum(1 for i in range(n) if other[i * 4:i * 4 + 4] != code[i * 4:i * 4 + 4])
                if diff <= n // 3 and (best is None or diff < best[0]):
                    best = (diff, c)
        if best:
            covered.add(best[1])
        else:
            unmatched += 1

    by_type = collections.Counter(t for t, _ in disc.values())
    cov_type = collections.Counter(disc[h][0] for h in covered)
    print(f"disc: {len(disc)} distinct shaders ({by_type[0]} vertex, {by_type[1]} pixel) in {len(packages)} files")
    print(f"inputs: {len(seen)} shaders, {unmatched} not on the disc (built at runtime)")
    print(f"covered: {len(covered)} / {len(disc)} = {100 * len(covered) / max(1, len(disc)):.1f}% "
          f"(vertex {cov_type[0]}/{by_type[0]}, pixel {cov_type[1]}/{by_type[1]})")
    rows = sorted(((len(h & covered) / len(h), len(h), n) for n, h in packages.items()))
    print(f"\nleast covered packages (of {len(rows)}):")
    for frac, total, name in rows[:args.top]:
        print(f"  {100 * frac:5.1f}%  {total:5d}  {name}")


if __name__ == "__main__":
    main()
