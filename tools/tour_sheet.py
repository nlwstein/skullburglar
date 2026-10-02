"""Contact sheet for tools/chapter_tour.sh: the last frame captured before each next LoadChapter,
labelled with the chapter id and the median guest fps while it was active.

    uv run --with pillow python tools/tour_sheet.py logs/run-N.log [out.jpg] [--frames DIR] [--list]

Logs from another machine (a test Mac, a Windows PC, the Linux container) name frames by its paths:
--list prints the frames the sheet needs (one per chapter) so only those get copied, and
--frames DIR looks each one up by file name in DIR instead.
"""
import os
import re
import statistics
import sys

TS = r"^\[(\d+-\d+-\d+ \d+:\d+:\d+\.\d+)\]"
args = [a for a in sys.argv[1:] if not a.startswith("--")]
frames_dir = sys.argv[sys.argv.index("--frames") + 1] if "--frames" in sys.argv else None
if frames_dir in args:
    args.remove(frames_dir)
log = open(args[0], errors="replace").read().splitlines()
out = args[1] if len(args) > 1 else "run/tour/sheet.jpg"

# (chapter, [frame paths], [fps samples]) in log order
spans = []
for line in log:
    if m := re.search(r"BOTS_EXEC: LoadChapter (\d+)", line):
        spans.append((m.group(1), [], []))
    elif spans and (m := re.search(r"capture \d+: \d+x\d+ -> (\S+)", line)):
        spans[-1][1].append(m.group(1))
    elif spans and (m := re.search(r"guest fps ([0-9.]+)", line)):
        spans[-1][2].append(float(m.group(1)))

if "--list" in sys.argv:
    print("\n".join(f[-1] for _, f, _ in spans if f))
    sys.exit()
if frames_dir:
    # Only frames that made it over (a killed run can log a capture whose file never landed).
    spans = [(c, [p for f in fr if os.path.exists(p := os.path.join(frames_dir, re.split(r"[\\/]", f)[-1]))],
              fps) for c, fr, fps in spans]

from PIL import Image, ImageDraw  # noqa: E402 (not needed for --list)

cols, w, h = 6, 320, 180
rows = (len(spans) + cols - 1) // cols
sheet = Image.new("RGB", (cols * w, max(rows, 1) * h))
draw = ImageDraw.Draw(sheet)
for i, (chapter, frames, fps) in enumerate(spans):
    x, y = (i % cols) * w, (i // cols) * h
    if frames:
        sheet.paste(Image.open(frames[-1]).resize((w, h)), (x, y))
    med = f"{statistics.median(fps[3:] or fps):.0f}" if fps else "-"
    label = f"{chapter}  {med} fps" + ("" if frames else "  NO FRAME")
    draw.rectangle((x, y, x + 150, y + 16), fill=(0, 0, 0))
    draw.text((x + 3, y + 2), label, fill=(255, 255, 0))
    print(f"{chapter}\tframes={len(frames)}\tfps_med={med}")
sheet.save(out, quality=80)
print(f"{len(spans)} chapters -> {out}")
