#!/usr/bin/env python3
"""Contact sheet for dual-render tours (tools/parity_tour.py + tools/parity_report.py): many pairs in one image.

  uv run --with numpy --with pillow tools/parity_sheet.py <tour dir> [<tour dir> ...] [options]

One row per pair: port | n1 | FLIP heatmap (the report's <stem>.sbs.jpg), with a label giving the run, the shot,
its game time and FLIP. Rows over --bar are flagged in red. Several tour dirs pool into one series, which is how an
effect-heavy checkpoint (an explosion, smoke) is judged: one shot is luck, a series shows the spread.

  --chapter 71515     only that checkpoint; the header then gives the pooled verdict (n, median, p90, % over the bar)
  --sort time|flip    rows by game time (default: watch an effect evolve) or worst first
  --worst N           keep the N worst pairs
  --rows N            rows per page (default 12); more pairs make sheet-2.jpg, sheet-3.jpg, ...
  --grid port|n1|flip one pane per pair as a thumbnail grid (default: rows of all three panes)
  --bar 0.30          the per-shot FLIP bar
  -o out.jpg          default <first tour>/report/sheet.jpg

Run parity_report.py on each tour first. Writes JPEG at quality 85, about 1.5k wide: small enough to commit under
docs and to read in one look.
"""
import argparse
import json
import os

import numpy as np
from PIL import Image, ImageDraw, ImageFont

PANE_W, PANE_H = 480, 270
LABEL_H = 26


def font(size):
    try:
        return ImageFont.load_default(size=size)
    except TypeError:  # Pillow < 10.1
        return ImageFont.load_default()


def pairs(tours, chapter):
    out = []
    for t in tours:
        rep = os.path.join(t, "report")
        for m in json.load(open(os.path.join(rep, "metrics.json"))):
            if chapter and m["chapter"] != chapter:
                continue
            side = os.path.join(t, "shots", m["stem"] + ".json")
            m["time"] = json.load(open(side)).get("world_time") if os.path.exists(side) else None
            m["run"] = os.path.basename(os.path.normpath(t))
            m["sbs"] = os.path.join(rep, m["stem"] + ".sbs.jpg")
            out.append(m)
    return out


def summary(ps, bar, chapter):
    fl = np.array([m["flip"] for m in ps])
    s = (f"{len(fl)} pairs  median {np.median(fl):.3f}  p90 {np.percentile(fl, 90):.3f}  worst {fl.max():.3f}  "
         f"{100 * np.mean(fl > bar):.0f} % over {bar:.2f}")
    if chapter:  # a series: judged on p90 (G1, 71515)
        s += f"  ->  {'PASS' if np.percentile(fl, 90) <= bar else 'FAIL'} (p90 <= {bar:.2f})"
    return s


def label(m):
    t = f"t={m['time']:.1f}s  " if m.get("time") is not None else ""
    return (f"{m['run']} {m['stem']}  {t}FLIP {m['flip']:.3f} (tone-corr. {m['flip_tc']:.3f})  "
            f"luma x{m['luma_ratio']:.2f}  {m.get('hints', '')}")


def page_rows(ps, bar, title):
    w = 3 * PANE_W
    head = 2 * LABEL_H
    img = Image.new("RGB", (w, head + len(ps) * (PANE_H + LABEL_H)), (16, 16, 16))
    d = ImageDraw.Draw(img)
    d.text((6, 4), title, fill=(255, 255, 255), font=font(16))
    for i, name in enumerate(("port (reference)", "n1", "FLIP (bright = worse)")):
        d.text((i * PANE_W + 6, LABEL_H + 4), name, fill=(200, 200, 200), font=font(15))
    for i, m in enumerate(ps):
        y = head + i * (PANE_H + LABEL_H)
        d.text((6, y + 5), label(m), fill=(255, 90, 90) if m["flip"] > bar else (230, 230, 120), font=font(15))
        img.paste(Image.open(m["sbs"]).convert("RGB").resize((w, PANE_H)), (0, y + LABEL_H))
    return img


def page_grid(ps, bar, title, pane, cols=4):
    k = {"port": 0, "n1": 1, "flip": 2}[pane]
    rows = (len(ps) + cols - 1) // cols
    img = Image.new("RGB", (cols * PANE_W, LABEL_H + rows * (PANE_H + LABEL_H)), (16, 16, 16))
    d = ImageDraw.Draw(img)
    d.text((6, 4), f"{title}  [{pane}]", fill=(255, 255, 255), font=font(16))
    for i, m in enumerate(ps):
        x, y = (i % cols) * PANE_W, LABEL_H + (i // cols) * (PANE_H + LABEL_H)
        sbs = Image.open(m["sbs"]).convert("RGB")
        third = sbs.width // 3
        t = f"  t={m['time']:.1f}s" if m.get("time") is not None else ""
        d.text((x + 6, y + 5), f"{m['run']} {m['stem']}{t}  {m['flip']:.3f}",
               fill=(255, 90, 90) if m["flip"] > bar else (230, 230, 120), font=font(14))
        img.paste(sbs.crop((k * third, 0, (k + 1) * third, sbs.height)).resize((PANE_W, PANE_H)), (x, y + LABEL_H))
    return img


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tours", nargs="+")
    ap.add_argument("--chapter")
    ap.add_argument("--sort", choices=("time", "flip"), default="time")
    ap.add_argument("--worst", type=int)
    ap.add_argument("--rows", type=int, default=12)
    ap.add_argument("--grid", choices=("port", "n1", "flip"))
    ap.add_argument("--bar", type=float, default=0.30)
    ap.add_argument("-o", "--out")
    a = ap.parse_args()

    ps = pairs(a.tours, a.chapter)
    if not ps:
        raise SystemExit("no pairs (run parity_report.py first, or check --chapter)")
    title = summary(ps, a.bar, a.chapter)
    if a.worst:
        ps = sorted(ps, key=lambda m: -m["flip"])[:a.worst]
    if a.sort == "flip":
        ps.sort(key=lambda m: -m["flip"])
    else:
        ps.sort(key=lambda m: (m["time"] is None, m["time"] or 0, m["run"], m["stem"]))
    print(title)

    out = a.out or os.path.join(a.tours[0], "report", "sheet.jpg")
    per = a.rows * (4 if a.grid else 1)
    pages = [ps[i:i + per] for i in range(0, len(ps), per)]
    for n, pg in enumerate(pages, 1):
        img = page_grid(pg, a.bar, title, a.grid) if a.grid else page_rows(pg, a.bar, title)
        path = out if n == 1 else f"{os.path.splitext(out)[0]}-{n}{os.path.splitext(out)[1]}"
        img.save(path, quality=85)
        print(f"{path}: {len(pg)} pairs, {img.width}x{img.height}")


if __name__ == "__main__":
    main()
