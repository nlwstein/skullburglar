#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["pillow", "numpy"]
# ///
"""Flag test-run pictures that show the engine's stand-in look (GitHub #32): a large flat white grid (the default
material's checker, overexposed) or a large pure-black silhouette in an otherwise lit frame (a skinned mesh lit by
nothing). Two modes.

  shot_check.py files PATH...      check PNG/JPG files (or folders of them); exit 1 when any is flagged
  shot_check.py live --out DIR     drive a running game (BOTS_INSTR=1, BOTS_HYBRID_DUAL=1, BOTS_SCENEFEED_DIR set):
                                   every --every seconds take a dual shot, check the n1 and port pictures, and for a
                                   flagged one keep the frame (+ its JSON sidecar) and ask the game for a scene-feed
                                   dump (`scenefeed dump 2`) into DIR

Detectors (both work on a copy downsampled to about 480 px wide):
  white-grid   at least --grid-area of the frame is near white (luma >= 225) and, inside that area, the thin dark
               lines form a regular lattice: the autocorrelation of the column and row line profiles has a strong
               peak (>= --grid-peak) at a lag of 6..120 px. A plain bright sky or a lit wall has no lattice.
  black-blob   a connected region of near-black pixels (all channels < 10) of at least --blob-area of the frame that
               does not touch the picture's edge, whose surround is lit (mean luma >= 55), and that is a solid shape
               (fills >= 30% of its bounding box). Letterbox bars, fades and dark rooms touch the edge or have no lit
               surround, so they don't count.
Thresholds were set on the #32 report's four pictures (positive) and on tutorial counter-kill frames (negative).
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import time

import numpy as np
from PIL import Image


def load(path, width=480):
    im = Image.open(path).convert("RGB")
    h = max(1, round(im.height * width / im.width))
    return np.asarray(im.resize((width, h), Image.BILINEAR), dtype=np.float32)


def luma(a):
    return a[..., 0] * 0.299 + a[..., 1] * 0.587 + a[..., 2] * 0.114


def _peak(profile, lo=6, hi=120):
    p = profile - profile.mean()
    if not p.any():
        return 0.0
    ac = np.correlate(p, p, mode="full")[len(p) - 1:]
    if ac[0] <= 0:
        return 0.0
    hi = min(hi, len(ac) - 1)
    return float(ac[lo:hi + 1].max() / ac[0]) if hi > lo else 0.0


def white_grid(a, area=0.06, peak=0.4):
    y = luma(a)
    bright = y >= 225
    frac = float(bright.mean())
    if frac < area:
        return None
    # thin dark lines inside bright territory: dark pixels with a bright pixel 2 px away on both sides
    dark = (y < 222) & (y > 110)
    near_h = np.zeros_like(bright)
    near_h[:, 2:-2] = bright[:, :-4] & bright[:, 4:]
    near_v = np.zeros_like(bright)
    near_v[2:-2, :] = bright[:-4, :] & bright[4:, :]
    line = dark & (near_h | near_v)
    if line.sum() < 0.002 * y.size:
        return None
    best = max(_peak(line.sum(axis=0).astype(np.float32)), _peak(line.sum(axis=1).astype(np.float32)))
    dens = float(line.sum() / max(1, bright.sum()))
    if best >= peak:
        return {"kind": "white-grid", "bright": round(frac, 3), "lattice": round(best, 2), "line_density": round(dens, 3)}
    return None


def _components(mask):
    """Connected components of a boolean mask (4-neighbour), as (label image, count). Plain numpy flood fill by rows
    (union-find on run labels) keeps this free of scipy."""
    h, w = mask.shape
    lab = np.zeros((h, w), np.int32)
    parent = [0]

    def find(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i

    nxt = 1
    for yy in range(h):
        row = mask[yy]
        xx = 0
        while xx < w:
            if not row[xx]:
                xx += 1
                continue
            x0 = xx
            while xx < w and row[xx]:
                xx += 1
            ids = set()
            if yy:
                ids = {int(v) for v in lab[yy - 1, x0:xx] if v}
            if not ids:
                parent.append(nxt)
                lab[yy, x0:xx] = nxt
                nxt += 1
            else:
                r = min(find(i) for i in ids)
                for i in ids:
                    parent[find(i)] = r
                lab[yy, x0:xx] = r
    roots = np.array([find(i) for i in range(len(parent))], np.int32)
    return roots[lab], roots


def black_blob(a, area=0.012):
    y = luma(a)
    black = a.max(axis=2) < 10
    if black.mean() < area:
        return None
    lab, roots = _components(black)
    h, w = black.shape
    counts = np.bincount(lab.ravel())
    counts[0] = 0
    for label in np.argsort(counts)[::-1][:3]:
        n = int(counts[label])
        if n < area * h * w:
            break
        ys, xs = np.nonzero(lab == label)
        y0, y1, x0, x1 = ys.min(), ys.max(), xs.min(), xs.max()
        if y0 <= 0 or x0 <= 0 or y1 >= h - 1 or x1 >= w - 1:
            continue  # touches the edge: letterbox, fade, a dark room
        fill = n / float((y1 - y0 + 1) * (x1 - x0 + 1))
        pad = max(4, int(0.06 * w))
        ring = y[max(0, y0 - pad):y1 + pad + 1, max(0, x0 - pad):x1 + pad + 1].copy()
        ring_mask = ~(lab[max(0, y0 - pad):y1 + pad + 1, max(0, x0 - pad):x1 + pad + 1] == label)
        surround = float(ring[ring_mask].mean()) if ring_mask.any() else 0.0
        if fill >= 0.30 and surround >= 55:
            return {"kind": "black-blob", "area": round(n / (h * w), 3), "fill": round(fill, 2), "surround_luma": round(surround, 1)}
    return None


def check(path, **kw):
    a = load(path)
    return [r for r in (white_grid(a, kw.get("grid_area", 0.06), kw.get("grid_peak", 0.4)), black_blob(a, kw.get("blob_area", 0.012))) if r]


def expand(paths):
    for p in paths:
        if os.path.isdir(p):
            for f in sorted(os.listdir(p)):
                if f.lower().endswith((".png", ".jpg", ".jpeg")) and "-rays" not in f and "-mask" not in f:
                    yield os.path.join(p, f)
        else:
            yield p


def run_files(args):
    bad = 0
    for f in expand(args.paths):
        res = check(f, grid_area=args.grid_area, grid_peak=args.grid_peak, blob_area=args.blob_area)
        if res:
            bad += 1
        print(f"{'FLAG' if res else 'ok  '} {f} {json.dumps(res) if res else ''}")
    print(f"{bad} flagged")
    return 1 if bad else 0


def ctl(args, *cmd):
    base = [sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), "botsctl.py"), "--port", str(args.port), "cmd"]
    r = subprocess.run(base + list(cmd), capture_output=True, text=True, timeout=60)
    try:
        return json.loads(r.stdout)
    except Exception:
        return {"ok": False, "raw": r.stdout[-200:]}


def run_live(args):
    os.makedirs(args.out, exist_ok=True)
    n = flagged = 0
    end = time.time() + args.duration
    while time.time() < end:
        tag = f"chk{n:04d}"
        r = ctl(args, "dualshot", tag)
        n += 1
        if not r.get("ok"):
            print("dualshot failed:", r)
            time.sleep(args.every)
            continue
        d = r.get("dir", "")
        time.sleep(2.0)  # the pair is written once its GPU copy completes
        found = [f for f in (os.listdir(d) if os.path.isdir(d) else []) if tag in f and f.endswith(".png") and "-rays" not in f and "-mask" not in f]
        for f in found:
            res = check(os.path.join(d, f))
            if res:
                flagged += 1
                keep = os.path.join(args.out, f)
                shutil.copy(os.path.join(d, f), keep)
                side = os.path.join(d, f.split(".")[0] + ".json")
                if os.path.exists(side):
                    shutil.copy(side, os.path.join(args.out, os.path.basename(side)))
                ctl(args, "exec", "scenefeed", "dump", "2")
                print(f"FLAG {f} {json.dumps(res)} (frame and sidecar kept in {args.out}; scene-feed dump asked for)")
        time.sleep(args.every)
    print(f"{n} shots, {flagged} flagged")
    return 1 if flagged else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--grid-area", type=float, default=0.06)
    ap.add_argument("--grid-peak", type=float, default=0.4)
    ap.add_argument("--blob-area", type=float, default=0.012)
    sub = ap.add_subparsers(dest="mode", required=True)
    f = sub.add_parser("files")
    f.add_argument("paths", nargs="+")
    lv = sub.add_parser("live")
    lv.add_argument("--out", required=True)
    lv.add_argument("--port", type=int, default=47100)
    lv.add_argument("--every", type=float, default=3.0)
    lv.add_argument("--duration", type=float, default=600.0)
    a = ap.parse_args()
    sys.exit(run_files(a) if a.mode == "files" else run_live(a))


if __name__ == "__main__":
    main()
