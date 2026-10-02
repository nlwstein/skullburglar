# /// script
# requires-python = ">=3.11"
# dependencies = ["lzallright>=0.2"]
# ///
"""Decompress BotS (UE3 v538/lic 73, Xbox 360) cooked packages.

Writes the package body fully uncompressed at its logical offsets, for our own parsers.
The summary is copied verbatim (still lists compression chunks) — UModel reads the
original LZO packages natively, so use those for UModel. Big-endian; flag 2 = LZO.

    uv run tools/upk_decompress.py game/C1Game/CookedXenon/C1Game.xxx -o dumps/upk/
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

from lzallright import LZOCompressor

PACKAGE_TAG = 0x9E2A83C1
COMPRESS_ZLIB, COMPRESS_LZO, COMPRESS_LZX = 1, 2, 4


class Reader:
    def __init__(self, data: bytes, off: int = 0):
        self.d, self.o = data, off

    def u32(self) -> int:
        v = struct.unpack_from(">I", self.d, self.o)[0]
        self.o += 4
        return v

    def i32(self) -> int:
        v = struct.unpack_from(">i", self.d, self.o)[0]
        self.o += 4
        return v

    def fstring(self) -> None:
        n = self.i32()
        self.o += n if n >= 0 else -n * 2


def parse_header(data: bytes) -> dict:
    r = Reader(data)
    if r.u32() != PACKAGE_TAG:
        raise ValueError("not a UE3 package (bad tag)")
    r.u32()  # version: licensee<<16 | engine
    r.i32()  # total header size
    r.fstring()  # folder name
    r.u32()  # package flags
    r.o += 7 * 4  # name/export/import counts+offsets, depends offset
    r.o += 16  # guid
    gens = r.i32()
    r.o += gens * 12
    r.i32()  # engine build
    r.i32()  # cooker version
    cflags_off = r.o
    cflags = r.u32()
    nchunks = r.i32()
    chunks = [tuple(r.i32() for _ in range(4)) for _ in range(nchunks)]
    return dict(cflags_off=cflags_off, cflags=cflags, chunks=chunks, after_chunks=r.o)


def decompress_chunk(data: bytes, off: int, lzo: LZOCompressor) -> bytes:
    r = Reader(data, off)
    if r.u32() != PACKAGE_TAG:
        raise ValueError(f"bad chunk tag at {off:#x}")
    r.u32()  # block size
    r.i32()  # total compressed
    total = r.i32()
    blocks = []
    got = 0
    while got < total:
        csz, usz = r.i32(), r.i32()
        blocks.append((csz, usz))
        got += usz
    out = bytearray()
    for csz, usz in blocks:
        out += lzo.decompress(data[r.o : r.o + csz], output_size_hint=usz)
        r.o += csz
    if len(out) != total:
        raise ValueError(f"chunk at {off:#x}: got {len(out)} bytes, expected {total}")
    return bytes(out)


def decompress_package(data: bytes) -> bytes:
    h = parse_header(data)
    if not h["chunks"]:
        return data
    if h["cflags"] != COMPRESS_LZO:
        raise ValueError(f"unsupported compression flags {h['cflags']}")
    lzo = LZOCompressor()
    first_uoff = min(c[0] for c in h["chunks"])
    end = max(c[0] + c[1] for c in h["chunks"])
    out = bytearray(end)
    # Summary kept verbatim (incl. the now-stale chunk table) so every absolute offset in
    # it stays valid; parsers must skip the chunk table rather than act on it.
    out[:first_uoff] = data[:first_uoff]
    for uoff, usz, coff, _csz in h["chunks"]:
        blob = decompress_chunk(data, coff, lzo)
        if len(blob) != usz:
            raise ValueError(f"chunk @{coff:#x}: size {len(blob)} != {usz}")
        out[uoff : uoff + usz] = blob
    return bytes(out)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("packages", nargs="+", type=Path)
    ap.add_argument("-o", "--out", type=Path, required=True)
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    rc = 0
    for p in args.packages:
        try:
            raw = p.read_bytes()
            dec = decompress_package(raw)
            (args.out / p.name).write_bytes(dec)
            print(f"{p.name}: {len(raw):,} -> {len(dec):,} bytes")
        except Exception as e:  # keep going across a batch
            print(f"{p.name}: FAILED: {e}", file=sys.stderr)
            rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
