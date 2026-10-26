#!/usr/bin/env python3
"""Match the native renderer's draws to scene-feed primitives (research spike, src/scene_feed.cpp).

  python3 tools/scenefeed_coverage.py <dir> [--detail] [--show N]   (needs numpy)

<dir> holds frame_*.json (the feed) and draws_*.bin (src/gfx/vk/draw_dump.h), written in the same window of frames.
UE3 uploads each mesh's LocalToWorld into vertex-shader constants, so a draw is explained by a primitive when four
consecutive constant registers hold that primitive's matrix (as stored, transposed, or the 3-register affine forms).
Each draw file is matched against the feed frame it shares the most exact translations with, plus that frame's
neighbours, so moving objects match the frame the renderer was actually drawing. Prints, per draw file, draws explained by primitive kind and the unexplained
draws grouped by pass (target size, depth-only, multisampled, rectangle list) and by what their constants look like
(2D, camera-facing particle sprite, world space inside a particle system, identity, other), plus how the feed's
visibility flags compare with what was drawn. --detail splits the unexplained draws by shader pair; --show N prints
c4..c13 of the first N unexplained draws per vertex shader.
"""
import collections
import glob
import json
import os
import struct
import sys

import numpy as np

REC = struct.Struct("<4sIQQIIIIII")
NCONST = 1024


def load_draws(path):
    data = open(path, "rb").read()
    if data[:4] != b"DFR1":
        raise SystemExit(f"{path}: not a draw dump")
    (stamp,) = struct.unpack_from("<q", data, 8)
    off, draws = 16, []
    while off + REC.size + 4 * NCONST <= len(data):
        magic, idx, vs, ps, w, h, flags, prim, count, _ = REC.unpack_from(data, off)
        off += REC.size
        consts = np.frombuffer(data, dtype="<f4", count=NCONST, offset=off).reshape(256, 4)
        off += 4 * NCONST
        draws.append(dict(idx=idx, vs=vs, ps=ps, w=w, h=h, flags=flags, prim=prim, count=count, c=consts))
    return stamp, draws


def keys(m):
    """The register rows a matrix shows up as: (encoding, first register row, all rows)."""
    m = np.asarray(m, dtype=np.float32).reshape(4, 4)
    t = m.T
    return [("rows", m), ("cols", t), ("cols3", t[:3]), ("rows3", m[:3])]


def main():
    d = sys.argv[1]
    detail = "--detail" in sys.argv
    show = int(sys.argv[sys.argv.index("--show") + 1]) if "--show" in sys.argv else 0
    shown = {}
    frames = sorted(glob.glob(os.path.join(d, "frame_*.json")))
    mats, info, when, frame_ids = [], [], [], []
    total_prims = collections.Counter()
    nframes = 0
    for f in frames:
        fr = json.load(open(f))
        nframes += 1
        for p in fr["primitives"]:
            total_prims[p["kind"]] += 1
            mats.append(np.asarray(p["transform"], dtype=np.float32).reshape(4, 4))
            info.append(p)
            when.append(fr.get("host_us", 0))
            frame_ids.append(fr["frame"])
    all_mats, all_when = np.array(mats), np.array(when, dtype=np.int64)
    mats = all_mats
    ident_all = np.all(np.abs(all_mats - np.eye(4, dtype=np.float32)) < 1e-5, axis=(1, 2))
    ident, trans = ident_all, all_mats[:, 3, :3]
    print(f"{nframes} feed frames; primitives per kind (summed over frames): {dict(total_prims)}")
    print(f"identity transforms: {collections.Counter(info[i]['kind'] for i in np.nonzero(ident_all)[0])}")

    frame_of = np.array(frame_ids)
    # Particle components' bounds, indexed like info (zero for everything else).
    part_idx = np.array([i for i, p in enumerate(info) if p["kind"] == "particle"], dtype=np.int64)
    part_origin = np.zeros((len(info), 3), np.float32)
    part_extent = np.zeros((len(info), 3), np.float32)
    for i in part_idx:
        part_origin[i] = info[i]["bounds"]["origin"]
        part_extent[i] = info[i]["bounds"]["extent"]

    def window(draws):
        """Restrict matching to the feed frame the renderer is drawing, plus its neighbours. The two clocks' stamps
        are taken at different points of a frame, so align by content: the feed frame sharing the most exact
        translations with the draws' c5..c12 rows."""
        nonlocal mats, ident, trans
        rows = {tuple(r) for dr in draws for r in np.round(dr["c"][5:13, :3], 2).tolist()}
        score = collections.Counter()
        for i in np.nonzero(~ident_all)[0]:
            if tuple(np.round(all_mats[i, 3, :3], 2).tolist()) in rows:
                score[frame_of[i]] += 1
        if score:
            best = score.most_common(1)[0][0]
            sel = np.abs(frame_of - best) <= 1
        else:
            sel = np.ones(len(frame_of), bool)
        mats = np.where(sel[:, None, None], all_mats, np.float32(1e9))
        ident = ident_all | ~sel
        trans = mats[:, 3, :3]
        return len(set(frame_of[sel].tolist()))

    def in_particle_bounds(point):
        sel = part_idx[~ident[part_idx]] if len(part_idx) else part_idx
        if not len(sel):
            return False
        o, e = part_origin[sel], part_extent[sel]
        return bool(np.any(np.all(np.abs(point - o) <= e, axis=1)))

    def match(block):
        """Primitive index for a 4-register row-major affine block: nearest translation, then rotation check."""
        dist = np.linalg.norm(trans - block[3, :3], axis=1)
        dist[ident] = np.inf
        best = np.argsort(dist)[:8]
        for i in best:
            if dist[i] > 100:  # cm; one frame of a fast vehicle
                break
            if np.max(np.abs(mats[i, :3, :3] - block[:3, :3])) < 0.02:
                return i, ("exact" if dist[i] < 0.01 else "close" if dist[i] < 2 else "moving")
        return None, None

    for path in sorted(glob.glob(os.path.join(d, "draws_*.bin"))):
        stamp, draws = load_draws(path)
        if not draws:
            continue
        nwin = window(draws)
        by_kind, quality, regs = collections.Counter(), collections.Counter(), collections.Counter()
        unexplained = collections.Counter()
        unexplained_vs = collections.Counter()
        matched = set()
        identity_draws = 0
        category = collections.Counter()
        for dr in draws:
            c = dr["c"]
            hit = None
            for r in range(5, 253):
                blk = c[r : r + 4]
                if blk[3, 3] != 1.0 or np.any(blk[:3, 3] != 0):
                    continue
                if np.all(np.abs(blk - np.eye(4)) < 1e-6):
                    continue
                i, q = match(blk)
                if i is not None:
                    hit = (i, q, r)
                    break
            if not hit:
                # SpeedTree and a few others pack scale and rotation apart: any w=1 row (but the camera at c4) on a
                # primitive's translation.
                for r in range(5, 256):
                    if c[r, 3] != 1.0 or not np.any(c[r, :3]):
                        continue
                    dist = np.linalg.norm(trans - c[r, :3], axis=1)
                    dist[ident] = np.inf
                    i = int(np.argmin(dist))
                    if dist[i] < 0.05:
                        hit = (i, "translation", r)
                        break
            pas = "{}x{}{}{}{}".format(dr["w"], dr["h"], " depth" if dr["flags"] & 1 else "",
                                         " ms" if dr["flags"] & 2 else "", " rect" if dr["flags"] & 4 else "")
            if hit:
                i, q, r = hit
                by_kind[info[i]["kind"]] += 1
                quality[q] += 1
                regs[r] += 1
                matched.add(info[i]["name"])
            else:
                is_id = np.all(np.abs(c[5:9] - np.eye(4)) < 1e-6)
                identity_draws += is_id
                if np.array_equal(c[4], np.array([0, 0, -1, 0], np.float32)):
                    what = "2D (canvas/UI/full-screen pass)"
                elif np.allclose(c[5, :3], c[4, :3]) and c[5, 3] == 0:
                    what = "camera-facing (particle sprite/beam)"
                elif is_id:
                    what = "world, LocalToWorld=identity"
                elif in_particle_bounds(c[8, :3]):
                    # Mesh-emitter particles (debris, casings): one draw per particle with its own matrix, inside a
                    # ParticleSystemComponent's bounds. The particles live in the native emitter instance.
                    what = "world, inside a particle system (mesh-emitter particle)"
                else:
                    what = "world, other"
                category[what] += 1
                key = pas + " | " + what
                unexplained[key] += 1
                unexplained_vs[(key, f"{dr['vs']:016X}", f"{dr['ps']:016X}")] += 1
                if show and shown.get(dr["vs"], 0) < show:
                    shown[dr["vs"]] = shown.get(dr["vs"], 0) + 1
                    print(f"  unexplained draw {dr['idx']} {key} vs {dr['vs']:016X} ps {dr['ps']:016X} prim {dr['prim']} "
                          f"count {dr['count']}\n{np.array2string(c[4:14], precision=4, suppress_small=True)}")
        n = len(draws)
        exp = sum(by_kind.values())
        print(f"\n{os.path.basename(path)}: {n} draws; explained {exp} ({100 * exp / n:.1f}%) by "
              f"{len(matched)} primitives: {dict(by_kind)}; match quality {dict(quality)}; registers {dict(regs)}; "
              f"{nwin} feed frames in the window")
        print(f"  unexplained {n - exp}: {identity_draws} with LocalToWorld = identity at c5 (BSP / world-space)")
        print(f"  unexplained by category: {dict(category)}")
        for k, v in unexplained.most_common(20):
            print(f"    {v:5d}  {k}")
        # How well the feed's own visibility flags predict what was drawn (by component, over all feed frames).
        last = {}
        for p in info:
            last[p["name"]] = p
        flags = collections.Counter()
        for name, p in last.items():
            flags[("drawn" if name in matched else "not drawn", "hidden" if p.get("hidden") else "visible",
                   "rendered" if p.get("rendered") else "not rendered")] += 1
        print("  visibility flags vs drawn (components):")
        for k, v in sorted(flags.items()):
            print(f"    {v:5d}  {' / '.join(k)}")
        if detail:
            for (k, vs, ps), v in unexplained_vs.most_common(30):
                print(f"    {v:5d}  {k}  vs {vs} ps {ps}")


if __name__ == "__main__":
    main()
