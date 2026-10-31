#!/usr/bin/env python3
"""Per-chapter load counters from a soak run's game log (B-287): lazy loads, late skips (instances not drawn because their
asset was still loading), the level-start preload hold, plus the footprint from the sampler's mem.tsv.
  soak_loads.py RUN.log
One row per `LoadChapter` (a chapter visited twice gets two rows). `late` counts from the chapter's load to the next one;
`late<12s` only the first 12 s after the load (what a player sees pop in); `settle` is the last second (log time since the
load) at which the cumulative count still rose. `hold_ms` is the level-start preload hold; `repreload_assets` the assets of
the level's last visit requested again at its start (B-287 re-preload).
"""
import re
import sys
from datetime import datetime

TS = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\]")
LOADS = re.compile(r"hybrid n1 loads: t=[\d.]+ s: lazy (\d+) meshes (\d+) materials (\d+) lights (\d+) skeletal \d+ particle \d+ tree; "
                   r"(\d+) late skips")
HOLD = re.compile(r"level resumed: preload hold (\d+) ms \((\d+) passes\)")
EXEC = re.compile(r"BOTS_EXEC: LoadChapter (\d+)")
REPRE = re.compile(r"level start (\S+): (\d+) assets")


def ts(s):
    return datetime.strptime(s, "%Y-%m-%d %H:%M:%S.%f").timestamp()


def main():
    a = sys.argv[1:]
    chaps = []  # dicts
    cur = None
    late = lazy = 0
    for line in open(a[0], errors="replace"):
        m = TS.match(line)
        if not m:
            continue
        t = ts(m.group(1))
        e = EXEC.search(line)
        if e:
            cur = dict(chap=e.group(1), t0=t, late0=late, lazy0=lazy, late12=None, last_rise=t, hold=0, passes=0, repre=0, late=0, lazy=0)
            chaps.append(cur)
            continue
        h = HOLD.search(line)
        if h and cur:
            cur["hold"] = max(cur["hold"], int(h.group(1)))
            cur["passes"] += int(h.group(2))
        r = REPRE.search(line)
        if r and cur:
            cur["repre"] += int(r.group(2))
        m = LOADS.search(line)
        if m:
            nl = int(m.group(5))
            nz = sum(int(m.group(i)) for i in (1, 2, 3, 4))
            if cur:
                if nl > late:
                    cur["last_rise"] = t
                if t - cur["t0"] <= 12:
                    cur["late12"] = nl - cur["late0"]
                cur["late"], cur["lazy"] = nl - cur["late0"], nz - cur["lazy0"]
            late, lazy = nl, nz
    print("chapter\tlate\tlate<12s\tsettle_s\tlazy\thold_ms\trepreload_assets")
    for c in chaps:
        print(f"{c['chap']}\t{c['late']}\t{c['late12'] if c['late12'] is not None else 0}\t{c['last_rise'] - c['t0']:.0f}\t"
              f"{c['lazy']}\t{c['hold']}\t{c['repre']}")


main()
