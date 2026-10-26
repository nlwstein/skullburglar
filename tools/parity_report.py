#!/usr/bin/env python3
"""Parity report for a dual-render tour (tools/parity_tour.py): the port's picture against n1's, per pair.

  uv run --with numpy --with pillow --with flip-evaluator tools/parity_report.py <tour dir>

Per pair (<tour>/shots/NNNN-<tag>.{port,n1}.png + .json): PSNR (RGB), SSIM (luma, Gaussian window), mean FLIP (NVIDIA
FLIP, LDR, 67 ppd, the port as reference), a FLIP heatmap, a side-by-side, and a mask of pixels the port drew and n1
left empty (n1 clears to a sentinel colour, BOTS_N1_CLEAR; the tour's default is magenta). Plus a few signals for
sorting gaps: luma ratio and colour cast over pixels both drew, how much of the error sits in the top third (sky),
and the worst 8x8 tiles.

Writes <tour>/report/index.html (per chapter, worst first), report/metrics.json and report/metrics.csv, and per pair
report/<stem>.sbs.jpg (port | n1 | FLIP) and report/<stem>.mask.jpg.
"""
import argparse
import csv
import html
import json
import os
import sys

import numpy as np
from PIL import Image

try:
    import flip_evaluator as flip
except ImportError:  # FLIP is optional; SSIM and PSNR still work
    flip = None


def load(path):
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.float32) / 255.0


def psnr(a, b):
    mse = float(np.mean((a - b) ** 2))
    return 99.0 if mse == 0 else 10 * np.log10(1.0 / mse)


def luma(x):
    return x[..., 0] * 0.2126 + x[..., 1] * 0.7152 + x[..., 2] * 0.0722


def gaussian_filter(x, sigma):
    # Separable Gaussian, edges clamped (numpy only: no SciPy).
    r = int(3 * sigma + 0.5)
    k = np.exp(-0.5 * (np.arange(-r, r + 1) / sigma) ** 2)
    k /= k.sum()
    x = np.pad(x, r, mode="edge")
    x = sum(k[i] * x[i:i + x.shape[0] - 2 * r, :] for i in range(2 * r + 1))
    return sum(k[i] * x[:, i:i + x.shape[1] - 2 * r] for i in range(2 * r + 1))


def ssim(a, b, sigma=1.5):
    a, b = luma(a).astype(np.float64), luma(b).astype(np.float64)
    c1, c2 = 0.01 ** 2, 0.03 ** 2
    mu_a, mu_b = gaussian_filter(a, sigma), gaussian_filter(b, sigma)
    saa = gaussian_filter(a * a, sigma) - mu_a ** 2
    sbb = gaussian_filter(b * b, sigma) - mu_b ** 2
    sab = gaussian_filter(a * b, sigma) - mu_a * mu_b
    m = ((2 * mu_a * mu_b + c1) * (2 * sab + c2)) / ((mu_a ** 2 + mu_b ** 2 + c1) * (saa + sbb + c2))
    return float(m.mean()), m


def srgb_to_lin(x):
    return np.where(x <= 0.04045, x / 12.92, ((x + 0.055) / 1.055) ** 2.4).astype(np.float32)


def lin_to_srgb(x):
    return np.where(x <= 0.0031308, x * 12.92, 1.055 * np.power(x, 1 / 2.4) - 0.055).astype(np.float32)


def sentinel(x):
    """Pixels in n1's clear colour after post: strongly magenta (red and blue high and alike, green low)."""
    r, g, b = x[..., 0], x[..., 1], x[..., 2]
    lo = np.minimum(r, b)
    return (lo > 0.25) & (g < 0.45 * lo) & (np.abs(r - b) < 0.3 * np.maximum(r, b))


# Magma-like colour map for error in [0, 1].
_STOPS = np.array([[0, 0, 4], [40, 11, 84], [101, 21, 110], [159, 42, 99], [212, 72, 66], [245, 125, 21],
                   [250, 193, 39], [252, 255, 164]], dtype=np.float32) / 255.0


def colormap(e):
    e = np.clip(e, 0, 1) * (len(_STOPS) - 1)
    i = np.minimum(e.astype(int), len(_STOPS) - 2)
    f = (e - i)[..., None]
    return _STOPS[i] * (1 - f) + _STOPS[i + 1] * f


def to_img(x):
    return Image.fromarray((np.clip(x, 0, 1) * 255 + 0.5).astype(np.uint8))


def analyse(port, n1, stem_out):
    h, w, _ = port.shape
    p = psnr(port, n1)
    s, _ = ssim(port, n1)
    if flip is not None:
        err, fl, _ = flip.evaluate(port, n1, "LDR", applyMagma=False)
        err = np.asarray(err, dtype=np.float32)
        if err.ndim == 3:
            err = err[..., 0]
    else:
        err = np.abs(luma(port) - luma(n1))
        fl = float(err.mean())
    empty = sentinel(n1) & ~sentinel(port)
    # Tone-corrected n1: one gain per channel in linear light (least squares over pixels both drew) takes out global
    # exposure and white balance. What error is left is content: missing or different objects, local lighting.
    drew0 = ~sentinel(n1) & ~sentinel(port)
    lp_, ln_ = srgb_to_lin(port), srgb_to_lin(n1)
    gain = np.ones(3, np.float32)
    if drew0.any():
        num, den = (lp_[drew0] * ln_[drew0]).sum(0), (ln_[drew0] ** 2).sum(0)
        gain = np.where(den > 0, num / np.maximum(den, 1e-6), 1.0).astype(np.float32)
    n1c = lin_to_srgb(np.clip(ln_ * gain, 0, 1))
    if flip is not None:
        errc, flc, _ = flip.evaluate(port, n1c, "LDR", applyMagma=False)
        errc = np.asarray(errc, dtype=np.float32)
        if errc.ndim == 3:
            errc = errc[..., 0]
    else:
        errc = np.abs(luma(port) - luma(n1c))
        flc = float(errc.mean())
    # Content gaps: blobs where the tone-corrected error stays high (smoothed over ~8 px).
    content = gaussian_filter(errc.astype(np.float64), 4.0) > 0.35
    drew = ~sentinel(n1) & ~sentinel(port)
    lp, ln = luma(port)[drew], luma(n1)[drew]
    luma_ratio = float(ln.mean() / max(lp.mean(), 1e-4)) if drew.any() else 0.0
    # Colour cast: chromaticity (r, g, b over their sum) of the mean colour each side.
    cp, cn = port[drew].mean(0) if drew.any() else np.zeros(3), n1[drew].mean(0) if drew.any() else np.zeros(3)
    cast = (cn / max(cn.sum(), 1e-4) - cp / max(cp.sum(), 1e-4)).tolist()
    top = err[: h // 3].sum() / max(err.sum(), 1e-6)
    # Worst tiles of an 8x8 grid.
    ty, tx = h // 8, w // 8
    tiles = []
    for j in range(8):
        for i in range(8):
            t = err[j * ty:(j + 1) * ty, i * tx:(i + 1) * tx]
            tiles.append((float(t.mean()), i, j, float(empty[j * ty:(j + 1) * ty, i * tx:(i + 1) * tx].mean())))
    tiles.sort(reverse=True)

    # Pictures: side-by-side (port | n1 | FLIP) at half size, and the empty mask over the port.
    heat = colormap(err)
    sbs = np.concatenate([port, n1, heat], axis=1)
    to_img(colormap(errc)).resize((w // 2, h // 2), Image.LANCZOS).save(stem_out + ".flip_tc.jpg", quality=85)
    to_img(sbs).resize((sbs.shape[1] // 2, sbs.shape[0] // 2), Image.LANCZOS).save(stem_out + ".sbs.jpg", quality=85)
    over = port * 0.45
    over[content] = over[content] * 0.4 + np.array([0.0, 0.9, 1.0]) * 0.6
    over[empty] = [1.0, 0.1, 0.6]
    to_img(over).resize((w // 2, h // 2), Image.LANCZOS).save(stem_out + ".mask.jpg", quality=85)
    return {
        "psnr": round(p, 2), "ssim": round(s, 4), "flip": round(float(fl), 4),
        "flip_tc": round(float(flc), 4), "gain": [round(float(g), 3) for g in gain],
        "content_pct": round(100 * float(content.mean()), 2),
        "empty_pct": round(100 * float(empty.mean()), 2), "high_err_pct": round(100 * float((err > 0.5).mean()), 2),
        "luma_ratio": round(luma_ratio, 3), "cast": [round(c, 4) for c in cast], "err_top_third": round(float(top), 3),
        "worst_tiles": [{"flip": round(t[0], 3), "x": t[1], "y": t[2], "empty": round(t[3], 3)} for t in tiles[:4]],
    }


def hints(m):
    """Rough labels from the numbers; the gap list itself is reviewed by eye."""
    out = []
    if m.get("content_pct", 0) > 2.0:
        out.append(f"content gaps {m['content_pct']:.0f} %")
    if m["flip"] - m.get("flip_tc", m["flip"]) > 0.05:
        out.append("global tone")
    if m["empty_pct"] > 1.0:
        out.append("missing geometry/sky" if m["err_top_third"] > 0.5 else "missing geometry")
    if m["luma_ratio"] and not 0.85 <= m["luma_ratio"] <= 1.15:
        out.append("darker" if m["luma_ratio"] < 1 else "brighter")
    if max(abs(c) for c in m["cast"]) > 0.02:
        r, g, b = m["cast"]
        out.append("cooler" if b - r > 0.02 else "warmer" if r - b > 0.02 else "colour cast")
    return ", ".join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("tour", help="tour directory (with shots/ and tour.json)")
    a = ap.parse_args()
    shots = os.path.join(a.tour, "shots")
    rep = os.path.join(a.tour, "report")
    os.makedirs(rep, exist_ok=True)
    notes = {}
    tj = os.path.join(a.tour, "tour.json")
    if os.path.exists(tj):
        tour = json.load(open(tj))
        notes = {c["chapter"]: c["note"] for c in tour.get("checkpoints", [])}
    rows = []
    for name in sorted(os.listdir(shots)):
        if not name.endswith(".port.png"):
            continue
        stem = name[: -len(".port.png")]
        n1p = os.path.join(shots, stem + ".n1.png")
        if not os.path.exists(n1p):
            continue
        side = {}
        if os.path.exists(os.path.join(shots, stem + ".json")):
            side = json.load(open(os.path.join(shots, stem + ".json")))
        tag = side.get("tag", stem.split("-", 1)[-1])
        chapter = tag.split("_")[0]
        m = analyse(load(os.path.join(shots, name)), load(n1p), os.path.join(rep, stem))
        m.update({"stem": stem, "tag": tag, "chapter": chapter, "mission": chapter[:1], "note": notes.get(chapter, ""),
                  "settled": side.get("settled"), "n1": side.get("n1", {})})
        m["hints"] = hints(m)
        rows.append(m)
        print(f"{stem}: PSNR {m['psnr']:.2f} dB, SSIM {m['ssim']:.3f}, FLIP {m['flip']:.3f} (tone-corrected "
              f"{m['flip_tc']:.3f}, gain {m['gain']}), content {m['content_pct']:.1f} %, empty "
              f"{m['empty_pct']:.1f} %, luma x{m['luma_ratio']:.2f}  {m['hints']}", flush=True)
    if not rows:
        sys.exit("no pairs found")
    json.dump(rows, open(os.path.join(rep, "metrics.json"), "w"), indent=1)
    with open(os.path.join(rep, "metrics.csv"), "w", newline="") as f:
        wr = csv.writer(f)
        wr.writerow(["stem", "chapter", "psnr", "ssim", "flip", "flip_tc", "content_pct", "empty_pct", "high_err_pct", "luma_ratio",
                     "err_top_third", "settled", "hints"])
        for m in rows:
            wr.writerow([m["stem"], m["chapter"], m["psnr"], m["ssim"], m["flip"], m["flip_tc"], m["content_pct"], m["empty_pct"], m["high_err_pct"],
                         m["luma_ratio"], m["err_top_third"], m["settled"], m["hints"]])

    # HTML: summary, then per chapter (worst chapter first), each chapter's pairs worst first.
    by = {}
    for m in rows:
        by.setdefault(m["chapter"], []).append(m)
    chapters = sorted(by, key=lambda c: -max(m["flip"] for m in by[c]))
    fl = np.array([m["flip"] for m in rows])
    ps = np.array([m["psnr"] for m in rows])
    ss = np.array([m["ssim"] for m in rows])
    em = np.array([m["empty_pct"] for m in rows])
    e = html.escape
    out = ["<!doctype html><meta charset=utf-8><title>Parity report</title><style>body{font:14px system-ui;"
           "margin:16px;background:#fff;color:#111}table{border-collapse:collapse}td,th{border:1px solid #ccc;"
           "padding:3px 6px;text-align:right}td.l,th.l{text-align:left}img{max-width:100%}.bad{background:#fdd}"
           "@media(prefers-color-scheme:dark){body{background:#111;color:#eee}td,th{border-color:#444}"
           ".bad{background:#522}}</style>",
           f"<h1>Parity report: {len(rows)} pairs, {len(by)} checkpoints</h1>",
           "<p>Port is the reference. FLIP: 0 = identical, higher = worse. Empty: % of the view n1 left in its clear "
           "colour where the port drew.</p><table><tr><th class=l>metric</th><th>median</th><th>worst</th>"
           "<th>best</th></tr>",
           f"<tr><td class=l>FLIP</td><td>{np.median(fl):.3f}</td><td>{fl.max():.3f}</td><td>{fl.min():.3f}</td></tr>",
           f"<tr><td class=l>PSNR dB</td><td>{np.median(ps):.2f}</td><td>{ps.min():.2f}</td><td>{ps.max():.2f}</td></tr>",
           f"<tr><td class=l>SSIM</td><td>{np.median(ss):.3f}</td><td>{ss.min():.3f}</td><td>{ss.max():.3f}</td></tr>",
           f"<tr><td class=l>empty %</td><td>{np.median(em):.2f}</td><td>{em.max():.2f}</td><td>{em.min():.2f}</td></tr>",
           "</table><h2>Per chapter (worst first)</h2><table><tr><th class=l>chapter</th><th class=l>scene</th>"
           "<th>pairs</th><th>FLIP max</th><th>FLIP mean</th><th>PSNR min</th><th>SSIM min</th><th>empty % max</th>"
           "<th class=l>hints</th></tr>"]
    for c in chapters:
        ms = by[c]
        hs = sorted({h for m in ms for h in m["hints"].split(", ") if h})
        out.append(f"<tr><td class=l><a href='#c{c}'>{c}</a></td><td class=l>{e(ms[0]['note'])}</td><td>{len(ms)}</td>"
                   f"<td>{max(m['flip'] for m in ms):.3f}</td><td>{np.mean([m['flip'] for m in ms]):.3f}</td>"
                   f"<td>{min(m['psnr'] for m in ms):.2f}</td><td>{min(m['ssim'] for m in ms):.3f}</td>"
                   f"<td>{max(m['empty_pct'] for m in ms):.2f}</td><td class=l>{e(', '.join(hs))}</td></tr>")
    out.append("</table>")
    for c in chapters:
        out.append(f"<h2 id='c{c}'>{c}: {e(by[c][0]['note'])}</h2>")
        for m in sorted(by[c], key=lambda m: -m["flip"]):
            unsettled = "" if m["settled"] in (True, None) else " <b class=bad>(n1 not settled)</b>"
            out.append(f"<h3>{e(m['stem'])}{unsettled}</h3><p>FLIP {m['flip']:.3f}, PSNR {m['psnr']:.2f} dB, "
                       f"SSIM {m['ssim']:.3f}, empty {m['empty_pct']:.2f} %, luma ×{m['luma_ratio']:.2f}, "
                       f"error in top third {100 * m['err_top_third']:.0f} %. {e(m['hints'])}</p>"
                       f"<img src='{e(m['stem'])}.sbs.jpg'><br><img width=640 src='{e(m['stem'])}.mask.jpg'>")
    open(os.path.join(rep, "index.html"), "w").write("\n".join(out))
    print(f"report: {os.path.join(rep, 'index.html')}")


if __name__ == "__main__":
    main()
