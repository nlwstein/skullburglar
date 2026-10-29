#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = ["pillow"]
# ///
"""Contact sheet of dual-render pairs (port | n1 per cell), for a moving-camera series (tools/camera_series.py).

    uv run tools/dual_sheet.py <shots dir> -o sheet.jpg [--cols 2] [--width 640] [--match PAT] [--crop x0,y0,x1,y1]
        [--only n1|port]

Cells are labelled with the shot tag. --crop (fractions 0..1 of the frame) zooms into the region where a decal sits;
--only draws a single pane per shot (denser grids: --cols 4).
"""
import argparse
import glob
import os
import re

from PIL import Image, ImageDraw

ap = argparse.ArgumentParser()
ap.add_argument("dir")
ap.add_argument("-o", "--out", default="sheet.jpg")
ap.add_argument("--cols", type=int, default=2)
ap.add_argument("--width", type=int, default=640, help="width of one pane")
ap.add_argument("--match", default="")
ap.add_argument("--crop", default="")
ap.add_argument("--only", choices=["n1", "port"], default="")
a = ap.parse_args()

tags = []
for f in sorted(glob.glob(os.path.join(a.dir, "*.n1.png"))):
    m = re.match(r"(\d+)-(.*)\.n1\.png", os.path.basename(f))
    if m and a.match in m.group(2):
        tags.append((m.group(1), m.group(2)))
crop = [float(v) for v in a.crop.split(",")] if a.crop else None


def pane(idx, tag, kind):
    im = Image.open(os.path.join(a.dir, f"{idx}-{tag}.{kind}.png")).convert("RGB")
    if crop:
        w, h = im.size
        im = im.crop((int(crop[0] * w), int(crop[1] * h), int(crop[2] * w), int(crop[3] * h)))
    h = round(im.height * a.width / im.width)
    im = im.resize((a.width, h), Image.LANCZOS)
    ImageDraw.Draw(im).text((4, 2), f"{tag} {kind}", fill=(255, 255, 0))
    return im


cells = []
for idx, tag in tags:
    ps = [pane(idx, tag, k) for k in ([a.only] if a.only else ["port", "n1"])]
    cell = Image.new("RGB", (sum(p.width for p in ps), ps[0].height))
    x = 0
    for p in ps:
        cell.paste(p, (x, 0))
        x += p.width
    cells.append(cell)
if not cells:
    raise SystemExit("no pairs found")
cw, ch = cells[0].size
rows = (len(cells) + a.cols - 1) // a.cols
sheet = Image.new("RGB", (cw * a.cols, ch * rows))
for i, c in enumerate(cells):
    sheet.paste(c, ((i % a.cols) * cw, (i // a.cols) * ch))
sheet.save(a.out, quality=85)
print(a.out, sheet.size, len(cells), "pairs")
