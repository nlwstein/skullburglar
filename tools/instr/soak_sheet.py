#!/usr/bin/env python3
"""Contact sheet of dual-shot pairs from a soak run (port | n1, two pairs per row, labelled with the shot name).
  soak_sheet.py DIR_WITH_JPGS OUT_PREFIX [--per-page 12] [--pane-w 400]
DIR holds <seq>-<tag>.port.jpg / .n1.jpg (the game's dual shots, converted: ffmpeg -q:v 3). Writes OUT_PREFIX-1.jpg, -2.jpg, ...
"""
import argparse
import glob
import os

from PIL import Image, ImageDraw

ap = argparse.ArgumentParser()
ap.add_argument("dir")
ap.add_argument("out")
ap.add_argument("--per-page", type=int, default=12)
ap.add_argument("--pane-w", type=int, default=400)
ap.add_argument("--match", default="")
a = ap.parse_args()
ports = sorted(p for p in glob.glob(os.path.join(a.dir, "*.port.jpg")) if a.match in p)
W = a.pane_w
pairs = []
for p in ports:
    n = p.replace(".port.jpg", ".n1.jpg")
    if os.path.exists(n):
        pairs.append((os.path.basename(p).replace(".port.jpg", "")[5:], Image.open(p), Image.open(n)))
H = None
for page in range(0, len(pairs), a.per_page):
    chunk = pairs[page:page + a.per_page]
    h = int(W * chunk[0][1].height / chunk[0][1].width)
    rows = (len(chunk) + 1) // 2
    sheet = Image.new("RGB", (4 * W + 30, rows * (h + 18)), (20, 20, 20))
    d = ImageDraw.Draw(sheet)
    for i, (name, po, n1) in enumerate(chunk):
        r, c = divmod(i, 2)
        x, y = c * (2 * W + 30), r * (h + 18)
        d.text((x + 4, y + 3), f"{name}   port | n1", fill=(255, 255, 0))
        sheet.paste(po.resize((W, h)), (x, y + 16))
        sheet.paste(n1.resize((W, h)), (x + W, y + 16))
    path = f"{a.out}-{page // a.per_page + 1}.jpg"
    sheet.save(path, quality=85)
    print(path)
