#!/usr/bin/env python3
"""Which of the port's draws carry a given light (dynamic-light passes), from a scene-feed draw dump.

UE3 draws each dynamic light that affects a primitive as an extra additive pass over that primitive. Its shaders get
the light's position (and 1 / radius) as constants. This finds the feed's point and spot lights that are not part of
a light environment, then every draw in a draw dump (version 4: with pixel shader constants) whose vertex or pixel
constants hold that light's position, and prints the draws with their constants near the match, the blend state and
the primitive the draw's world matrix belongs to (when the feed JSON has one at that position).

  python3 tools/light_draws.py <feed dir> [--json frame_N.json] [--draws draws_N.bin] [--light <substring>]
      [--consts] [--min-luma 0.01]

The feed dir is a tour's <out>/feed (parity_tour.py --feed --draws) or any BOTS_SCENEFEED_DIR.
"""
import argparse
import glob
import json
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scenefeed_coverage import load_draws


def lights_of(frame):
    out = []
    for l in frame.get("lights", []):
        p = l.get("props", {})
        if l.get("class") not in ("PointLightComponent", "SpotLightComponent"):
            continue
        if p.get("LightEnvironment"):
            continue
        m = p.get("LightToWorld")
        if not m or len(m) != 16:
            continue
        c = p.get("LightColor", [0, 0, 0, 0])
        b = p.get("Brightness", 1.0)
        col = [c[0] / 255 * b, c[1] / 255 * b, c[2] / 255 * b]
        ue3 = [(c[k] / 255) ** 2.2 * b for k in range(3)]  # FLinearColor(FColor) * Brightness
        out.append({"name": l.get("name", ""), "cls": l["class"], "pos": np.array(m[12:15], dtype=np.float32),
                    "radius": p.get("Radius", 0.0), "falloff": p.get("FalloffExponent", 2.0), "color": col, "ue3": ue3, "props": p})
    return out


def luma(c):
    return 0.3 * c[0] + 0.59 * c[1] + 0.11 * c[2]


def find_rows(consts, pos, tol):
    """Register rows whose xyz is within tol of pos."""
    d = np.abs(consts[:, :3] - pos[None, :]).max(axis=1)
    return [int(i) for i in np.nonzero(d < tol)[0]]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("feed")
    ap.add_argument("--json")
    ap.add_argument("--draws")
    ap.add_argument("--light", default="")
    ap.add_argument("--consts", action="store_true", help="print every non-zero pixel constant of matching draws")
    ap.add_argument("--min-luma", type=float, default=0.01)
    ap.add_argument("--tol", type=float, default=0.05)
    a = ap.parse_args()
    js = a.json or max(glob.glob(os.path.join(a.feed, "frame_*.json")), key=os.path.getmtime)
    dr = a.draws or max(glob.glob(os.path.join(a.feed, "draws_*.bin")), key=os.path.getmtime)
    with open(js) as f:
        frame = json.load(f)
    _, draws = load_draws(dr)
    print(f"feed {os.path.basename(js)} (time {frame.get('time')}), draws {os.path.basename(dr)}: {len(draws)} draws")
    for L in lights_of(frame):
        if a.light not in L["name"] and a.light not in L["cls"]:
            continue
        p = L["props"]
        flag = "" if luma(L["color"]) >= a.min_luma else " (dim)"
        print(f"\nlight {L['name'] or '?'} {L['cls']} pos {L['pos'].round(1).tolist()} radius {L['radius']} "
              f"falloff {L['falloff']} colour*brightness {np.round(L['color'], 3).tolist()} "
              f"UE3 pow(colour, 2.2)*brightness {np.round(L['ue3'], 3).tolist()}{flag}")
        for k in ("bEnabled", "LightingChannels", "LightAffectsClassification", "UseDirectLightMap", "CastShadows",
                  "CastDynamicShadows", "CastStaticShadows", "bCastCompositeShadow", "LightShadowMode", "Function",
                  "bForceDynamicLight", "ShadowProjectionTechnique", "bAffectCompositeShadowDirection",
                  "bOnlyAffectSameAndSpecifiedLevels", "OtherLevelsToAffect", "Inclusion", "Exclusion",
                  "InclusionConvexVolumes", "ExclusionConvexVolumes", "bUseVolumes", "MinShadowResolution"):
            if k in p:
                print(f"    {k}: {json.dumps(p[k])[:300]}")
        if luma(L["color"]) < a.min_luma:
            continue
        hits = []
        for d in draws:
            vr = find_rows(d["c"], L["pos"], a.tol)
            pr = find_rows(d["pixel"]["c"], L["pos"], a.tol) if d.get("pixel") else []
            if vr or pr:
                hits.append((d, vr, pr))
        print(f"    draws carrying its position: {len(hits)}")
        for d, vr, pr in hits:
            px = d.get("pixel") or {}
            print(f"    draw {d['idx']:5d} vs {d['vs']:016x} ps {d['ps']:016x} {d['w']}x{d['h']} flags {d['flags']} "
                  f"prim {d['prim']} count {d['count']} blend0 {px.get('blend0', 0):08x} depth {px.get('depth', 0):08x} "
                  f"stencil {px.get('stencil', 0):08x} vs rows {vr} ps rows {pr}")
            for r in vr:
                print(f"        vc{r} = {np.round(d['c'][r], 5).tolist()}")
            for r in pr:
                print(f"        pc{r} = {np.round(px['c'][r], 5).tolist()}")
            if a.consts and px:
                for r in range(256):
                    v = px["c"][r]
                    if np.any(v != 0):
                        print(f"        pc{r} = {np.round(v, 5).tolist()}")


if __name__ == "__main__":
    main()
