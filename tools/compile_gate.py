#!/usr/bin/env python3
"""Fails when a run compiled shaders or pipelines in live play (B-198: all shader work happens in loading screens).

    tools/compile_gate.py GAME.log [--allow N]   exit 1 if the log has more than N `compile-in-play` lines
    tools/compile_gate.py --self-test

Run it on a warm-cache session (the second boot after a cold one): the n1 log carries one `compile-in-play` line per
compile that happened while scene frames were being drawn, and a `summary: n1: compiles ...` line at shutdown.
"""
import re
import sys

PLAY = re.compile(r"compile-in-play t=([\d.]+)s kind=(\S+) (.*?) ([\d.]+) ms \((render thread|worker)\)")
SUMMARY = re.compile(r"compiles startup=(\d+)/(\d+)ms loading=(\d+)/(\d+)ms play=(\d+)/(\d+)ms")


def scan(text):
    """Returns (in-play compile lines as tuples, last summary as a tuple of ints or None)."""
    plays = [(float(m[1]), m[2], m[3], float(m[4]), m[5]) for m in PLAY.finditer(text)]
    sums = SUMMARY.findall(text)
    return plays, (tuple(int(x) for x in sums[-1]) if sums else None)


def main(argv):
    if argv[:1] == ["--self-test"]:
        return self_test()
    allow = int(argv[argv.index("--allow") + 1]) if "--allow" in argv else 0
    path = next((a for a in argv if not a.startswith("--") and not a.isdigit()), None)
    if not path:
        print(__doc__)
        return 2
    plays, summ = scan(open(path, errors="replace").read())
    for t, kind, label, ms, where in plays:
        print(f"in play: t={t:.1f}s {kind} {label} {ms:.1f} ms ({where})")
    if summ:
        print(f"startup {summ[0]} ({summ[1]} ms), loading {summ[2]} ({summ[3]} ms), play {summ[4]} ({summ[5]} ms)")
    else:
        print("no summary line (the game did not shut down cleanly, or the log is not an n1 run)")
    n = max(len(plays), summ[4] if summ else 0)
    print(f"{'FAIL' if n > allow else 'ok'}: {n} compile(s) in play (allowed {allow})")
    return 1 if n > allow else 0


def self_test():
    ok = "n1ffi: summary: n1: compiles startup=900/180000ms loading=12/900ms play=0/0ms play-render-thread=0/0ms play-max=0.0ms"
    bad = (
        "n1ffi: compile-in-play t=61.250s kind=aa_pipe fs_cas/50 38.0 ms (render thread)\n"
        "n1ffi: compile-in-play t=70.500s kind=scene_pipe Scene(SceneKey { shader: 3 }) 12.5 ms (worker)\n"
        "n1ffi: summary: n1: compiles startup=900/180000ms loading=12/900ms play=2/51ms play-render-thread=1/38ms play-max=38.0ms"
    )
    assert scan(ok) == ([], (900, 180000, 12, 900, 0, 0))
    plays, summ = scan(bad)
    assert [p[1] for p in plays] == ["aa_pipe", "scene_pipe"] and plays[0][4] == "render thread", plays
    assert summ[4] == 2
    import os, tempfile
    for text, want in ((ok, 0), (bad, 1)):
        with tempfile.NamedTemporaryFile("w", suffix=".log", delete=False) as f:
            f.write(text)
        try:
            assert main([f.name]) == want
        finally:
            os.unlink(f.name)
    print("self-test ok")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
