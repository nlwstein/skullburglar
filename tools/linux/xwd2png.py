#!/usr/bin/env python3
"""Convert an Xvfb framebuffer file (Xvfb -fbdir DIR writes DIR/Xvfb_screen0, an XWD) to PNG, optionally cropped.

    tools/linux/xwd2png.py Xvfb_screen0 out.png [x y w h]

Headless Linux runs have no xwd/import in the container, but Xvfb can keep the screen in a file: start it with
`Xvfb :99 -screen 0 2560x1440x24 -fbdir /w/xfb`, run the game in a window of that size, and copy the file out. Unlike
BOTS_CAPTURE_DIR frames (what the game presented), this shows the window as a player sees it, ImGui overlays included.
Pure standard library: no PIL or numpy needed.
"""
import struct
import sys
import zlib


def main():
    src, dst = sys.argv[1], sys.argv[2]
    data = open(src, "rb").read()
    (hdr, _ver, _fmt, _depth, w, h, _xoff, byteorder, _bu, _bbo, _bp, bpp, bpl, _vc,
     rmask, gmask, bmask, _bpr, _cme, ncolors) = struct.unpack(">20I", data[:80])
    off = hdr + ncolors * 12
    if bpp != 32:
        sys.exit(f"unsupported depth: {bpp} bits per pixel")
    big = byteorder == 1
    rshift, gshift, bshift = (m.bit_length() - 8 for m in (rmask, gmask, bmask))
    x0, y0, cw, ch = (int(a) for a in sys.argv[3:7]) if len(sys.argv) >= 7 else (0, 0, w, h)
    rows = []
    for y in range(y0, y0 + ch):
        line = data[off + y * bpl: off + y * bpl + w * 4]
        px = memoryview(line).cast("I") if not big else None
        out = bytearray()
        for x in range(x0, x0 + cw):
            v = px[x] if px is not None else struct.unpack(">I", line[x * 4:x * 4 + 4])[0]
            out += bytes(((v & rmask) >> rshift, (v & gmask) >> gshift, (v & bmask) >> bshift))
        rows.append(b"\0" + bytes(out))

    def chunk(tag, body):
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body))

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", cw, ch, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(b"".join(rows), 6)) + chunk(b"IEND", b"")
    open(dst, "wb").write(png)


if __name__ == "__main__":
    main()
