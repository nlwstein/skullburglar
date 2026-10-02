# /// script
# requires-python = ">=3.11"
# dependencies = ["pillow"]
# ///
"""Tile captured guest frames (BOTS_CAPTURE_DIR) into one labelled PNG.

    uv run tools/contact_sheet.py run/shots [out.png] [cols=6]
"""
import glob
import sys

from PIL import Image, ImageDraw

src = sys.argv[1]
out = sys.argv[2] if len(sys.argv) > 2 else f"{src}/sheet.png"
cols = int(sys.argv[3]) if len(sys.argv) > 3 else 6
files = sorted(glob.glob(f"{src}/frame-*.ppm"))
w, h = 256, 144
rows = (len(files) + cols - 1) // cols
sheet = Image.new("RGB", (cols * w, rows * h), "black")
for i, f in enumerate(files):
    im = Image.open(f).resize((w, h))
    ImageDraw.Draw(im).text((4, 2), f.split("-")[-1].split(".")[0], fill=(255, 255, 0))
    sheet.paste(im, ((i % cols) * w, (i // cols) * h))
sheet.save(out)
print(out, len(files), "frames")
