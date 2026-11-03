#!/usr/bin/env python3
"""Summarise test-range damage logs (src/test_range.cpp, BOTS_RANGE_DIR/<name>.jsonl, scenarios/range.txt).

  range_report.py LOG.jsonl [LOG.jsonl ...]     one table per log, then one per group of logs named by prefix

A trial runs from its `trial <weapon> <aim> <dist> <rep>` mark to the next `end`. Per trial: hits, hits to kill,
head hits, damage. Per (weapon, aim, dist): the mean over trials and the bone mix.
"""
import collections
import json
import sys


def load(path):
    trials, cur = [], None
    for line in open(path, encoding="utf-8", errors="replace"):
        line = line.strip()
        if not line.startswith("{"):
            continue
        e = json.loads(line)
        if e["ev"] == "mark":
            t = e["text"].split()
            if t and t[0] == "trial" and len(t) >= 5:
                cur = {"weapon": t[1], "aim": t[2], "dist": t[3], "rep": t[4], "hits": [], "shots": 0}
                trials.append(cur)
            elif t and t[0] == "end":
                cur = None
        elif cur is not None and e["ev"] == "shot":
            cur["shots"] += 1
        elif cur is not None and e["ev"] == "hit":
            cur["hits"].append(e)
    return trials


def summarise(trials):
    rows = collections.OrderedDict()
    for t in trials:
        hits = t["hits"]
        killed = next((i + 1 for i, h in enumerate(hits) if h["dead"]), None)
        r = rows.setdefault((t["weapon"], t["aim"], t["dist"]), {"n": 0, "hits": 0, "head": 0, "kills": [], "dmg": 0, "bones": collections.Counter(), "lod": collections.Counter(), "age": []})
        r["n"] += 1
        r["hits"] += len(hits)
        r["head"] += sum(h["head"] for h in hits)
        r["dmg"] += sum(h["dmg"] for h in hits)
        if killed:
            r["kills"].append(killed)
        for h in hits:
            r["bones"][h["bone"]] += 1
            r["lod"][int(h["lod"])] += 1
            r["age"].append(h["now"] - h["lrt"])
    return rows


def victims(trials):
    """Per victim over the whole trial (real encounters): hits, hits to the kill, head hits."""
    rows = collections.OrderedDict()
    for t in trials:
        for h in t["hits"]:
            v = rows.setdefault(h["victim"], {"cls": h["cls"], "hp": h["hp0"], "hits": 0, "head": 0, "dead": False, "dmg": 0, "age": [], "lod": collections.Counter()})
            if v["dead"]:
                continue
            v["hits"] += 1
            v["head"] += h["head"]
            v["dmg"] += h["dmg"]
            v["dead"] = bool(h["dead"])
            v["age"].append(h["now"] - h["lrt"])
            v["lod"][int(h["lod"])] += 1
    return rows


def show_victims(name, trials):
    rows = victims([t for t in trials if t["weapon"] == "real"])
    if not rows:
        return
    kills = [v for v in rows.values() if v["dead"]]
    print(f"== {name}: {len(rows)} victims hit, {len(kills)} killed; hits per kill {sum(v['hits'] for v in kills) / max(1, len(kills)):.2f}; "
          f"head hits {sum(v['head'] for v in rows.values())} of {sum(v['hits'] for v in rows.values())}; "
          f"one-hit kills {sum(1 for v in kills if v['hits'] == 1)}")
    print(f"{'victim':28}{'class':26}{'hp':>5}{'hits':>6}{'head':>6}{'dead':>6}{'dmg':>7}  lod")
    for n, v in rows.items():
        print(f"{n:28}{v['cls']:26}{v['hp']:5.0f}{v['hits']:6}{v['head']:6}{int(v['dead']):6}{v['dmg']:7.0f}  {dict(v['lod'])}")
    print()


def show(name, trials):
    show_victims(name, trials)
    if all(t["weapon"] == "real" for t in trials):
        return
    print(f"== {name}: {len(trials)} trials, {sum(len(t['hits']) for t in trials)} hits")
    print(f"{'weapon':18}{'aim':7}{'dist':6}{'n':>3}{'hits':>6}{'kills':>6}{'hits/kill':>10}{'head%':>7}{'avg dmg':>9}  bones  |  lod  |  render age s")
    for (w, a, d), r in summarise(trials).items():
        hk = sum(r["kills"]) / len(r["kills"]) if r["kills"] else float("nan")
        head = 100.0 * r["head"] / r["hits"] if r["hits"] else float("nan")
        dmg = r["dmg"] / r["hits"] if r["hits"] else float("nan")
        bones = " ".join(f"{b.replace('C1_', '')}:{c}" for b, c in r["bones"].most_common(4))
        lod = " ".join(f"{k}:{v}" for k, v in sorted(r["lod"].items()))
        age = sorted(r["age"])
        med = age[len(age) // 2] if age else float("nan")
        print(f"{w:18}{a:7}{d:6}{r['n']:3}{r['hits']:6}{len(r['kills']):6}{hk:10.2f}{head:7.1f}{dmg:9.1f}  {bones}  |  {lod}  |  {med:.3f}")
    print()


if __name__ == "__main__":
    groups = collections.OrderedDict()
    for path in sys.argv[1:]:
        trials = load(path)
        show(path.rsplit("/", 1)[-1], trials)
        stem = path.rsplit("/", 1)[-1].split(".")[0]
        groups.setdefault(stem.rstrip("abcdefgh") if stem[-1] in "abcdefgh" else stem, []).extend(trials)  # n1a, n1b -> n1
    if len(sys.argv) > 2:
        for g, trials in groups.items():
            show(f"all {g}", trials)
