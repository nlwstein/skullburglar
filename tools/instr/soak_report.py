#!/usr/bin/env python3
"""Counter timeline from a long-session run's game log (B-277 soak): one row per 5 s `hybrid n1:` line, with the
chapter (from the `BOTS_EXEC: LoadChapter N` lines), n1 mesh/texture counters and the `gfx native:` memory/texture
counters. Prints a per-chapter summary (first/last/max of each counter) so growth across transitions stands out.
  soak_report.py RUN.log [--rows] [--rss logs/rss.tsv]
"""
import re
import sys
from datetime import datetime

TS = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\]")
N1 = re.compile(r"hybrid n1: ([\d.]+) s, (\d+) frames.*?-> ([\d.]+) draws.*?meshes (\d+) ready (\d+) pending (\d+) missing, "
                r"(\d+) loading, (\d+) textures \((\d+) unsupported\); skinned (\d+) drawn (\d+) culled (\d+) skipped, "
                r"(\d+) meshes ready.*?feed (\d+) lights (\d+) envs \((\d+) instances bound")
MEM = re.compile(r"gfx native: memory: surfaces (\d+) (\d+) MB, resolve targets \d+ (\d+) MB, textures (\d+) (\d+) MB.*?"
                 r"1\(device\) (\d+)/(\d+)/(\d+)")
FRM = re.compile(r"gfx native: frame (\d+):.*?(\d+) unsupported textures, ([\d.]+) MB uploaded, \d+ invalidations, "
                 r"(\d+) texture reloads, (\d+) evictions")
LOAD = re.compile(r"BOTS_EXEC: LoadChapter (\d+)")
COLS = ["t", "chap", "inst", "draws", "mesh_rdy", "mesh_pend", "mesh_miss", "loading", "tex", "tex_unsup", "skin_drawn",
        "skin_rdy", "envs", "env_bound", "heap_ours", "heap_proc", "tex_reloads", "evict", "uploadMB", "port_tex"]


def ts(s):
    return datetime.strptime(s, "%Y-%m-%d %H:%M:%S.%f").timestamp()


def parse(path):
    rows, chap, cur = [], "-", {}
    for line in open(path, errors="replace"):
        m = TS.match(line)
        if not m:
            continue
        t = ts(m.group(1))
        m2 = LOAD.search(line)
        if m2:
            chap = m2.group(1)
        m2 = MEM.search(line)
        if m2:
            cur.update(port_tex=int(m2.group(4)), heap_ours=int(m2.group(6)), heap_proc=int(m2.group(7)))
        m2 = FRM.search(line)
        if m2:
            cur.update(tex_reloads=int(m2.group(4)), evict=int(m2.group(5)), uploadMB=float(m2.group(3)))
        m2 = N1.search(line)
        if m2:
            g = m2.groups()
            rows.append(dict(cur, t=t, chap=chap, draws=float(g[2]), mesh_rdy=int(g[3]), mesh_pend=int(g[4]),
                             mesh_miss=int(g[5]), loading=int(g[6]), tex=int(g[7]), tex_unsup=int(g[8]),
                             skin_drawn=int(g[9]), skin_rdy=int(g[12]), envs=int(g[14]), env_bound=int(g[15]),
                             inst=0))
    return rows


def main():
    a = sys.argv[1:]
    rows = parse(a[0])
    if not rows:
        sys.exit("no `hybrid n1:` lines")
    t0 = rows[0]["t"]
    if "--rows" in a:
        print("\t".join(COLS))
        for r in rows:
            print("\t".join(f"{r.get(c, ''):.0f}" if c == "t" and False else str(round(r.get(c, 0) - t0, 0) if c == "t" else r.get(c, "")) for c in COLS))
    print("chapter\tstart_s\tn\tmesh_rdy(first..max)\tmiss_max\tloading_max\ttex(first..last)\tunsup\tskin_rdy_max\tenv_bound(min..max)\t"
          "heap_ours(first..last)\treloads(first..last)\tevict_last")
    by = {}
    for r in rows:
        by.setdefault(r["chap"], []).append(r)
    for c, rs in by.items():
        g = lambda k: [r.get(k, 0) for r in rs]  # noqa: E731
        print(f"{c}\t{rs[0]['t'] - t0:.0f}\t{len(rs)}\t{g('mesh_rdy')[0]}..{max(g('mesh_rdy'))}\t{max(g('mesh_miss'))}\t"
              f"{max(g('loading'))}\t{g('tex')[0]}..{g('tex')[-1]}\t{max(g('tex_unsup'))}\t{max(g('skin_rdy'))}\t"
              f"{min(g('env_bound'))}..{max(g('env_bound'))}\t{g('heap_ours')[0]}..{g('heap_ours')[-1]}\t"
              f"{g('tex_reloads')[0]}..{g('tex_reloads')[-1]}\t{g('evict')[-1]}")


main()
