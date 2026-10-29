#!/usr/bin/env python3
"""Moving-camera series for the hybrid renderer: port and n1 pairs while the player walks / turns.

Attaches to a running game started with BOTS_HYBRID_DUAL=1 and BOTS_INSTR (control channel, src/instr.cpp), loads a
checkpoint, then takes `--shots` dual shots (hybrid dualshot) with the camera still (`--mode still`), turning the
right stick (`turn`), walking forward (`walk`) or both (`walkturn`). Shots land in BOTS_HYBRID_DUAL_DIR as
NNNN-<tag>.{port,n1}.png; make a sheet with tools/parity_sheet.py or tools/contact_sheet.py.

  python3 tools/camera_series.py --chapter 11010 --tag a --mode walkturn --shots 24 [--port 47100] [--gap 0.7]
        [--settle 6] [--no-load] [--turn 0.35] [--walk 0.8]

Decal and z-fighting checks need the camera moving, because a fight depends on the exact depth of every pixel and
changes with each camera step: a still shot shows only one outcome. --jitter N adds N extra shots per step with a
tiny stick nudge in between (pairs of almost identical frames, so a flicker shows as a difference between neighbours).
"""
import argparse
import glob
import json
import os
import socket
import time


class Ctl:
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=10)
        self.f = self.s.makefile("rb")

    def cmd(self, line, timeout=30):
        self.s.settimeout(timeout)
        self.s.sendall((line + "\n").encode())
        while True:
            j = json.loads(self.f.readline())
            if j.get("type") not in ("status", "event") or line.startswith("status"):
                if line.startswith("status") and j.get("type") != "status":
                    continue
                return j

    def status(self):
        return self.cmd("status")

    def dualshot(self, tag, timeout=60):
        """One pair; returns once both PNGs are on disk (a second dualshot while one is pending is dropped)."""
        d = self.cmd(f"dualshot {tag}").get("dir", "dual")
        end = time.time() + timeout
        while time.time() < end:
            if glob.glob(os.path.join(d, f"*-{tag}.n1.png")) and glob.glob(os.path.join(d, f"*-{tag}.port.png")):
                time.sleep(0.2)
                return True
            time.sleep(0.1)
        print("dualshot", tag, "timed out")
        return False


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=47100)
    ap.add_argument("--chapter", default="")
    ap.add_argument("--no-load", action="store_true", help="shoot in the current level")
    ap.add_argument("--tag", default="s")
    ap.add_argument("--mode", choices=["still", "turn", "walk", "walkturn"], default="walkturn")
    ap.add_argument("--shots", type=int, default=24)
    ap.add_argument("--gap", type=float, default=0.7, help="seconds between shots (wall)")
    ap.add_argument("--settle", type=float, default=6.0, help="game seconds after the load before the first shot")
    ap.add_argument("--turn", type=float, default=0.35, help="right-stick x while turning (-1..1)")
    ap.add_argument("--walk", type=float, default=0.8, help="left-stick y while walking (-1..1)")
    ap.add_argument("--strafe", type=float, default=0.0, help="left-stick x (-1..1) while walking: slide along a wall")
    ap.add_argument("--jitter", type=int, default=0)
    a = ap.parse_args()
    c = Ctl(a.port)
    if not a.no_load:
        r = c.cmd(f"exec LoadChapter {a.chapter}")
        print(r)
        time.sleep(2)
        r = c.cmd("wait -t 300 phase==in_play", timeout=320)
        print("in play:", r.get("ok"))
    t0 = None
    while True:
        st = c.status()
        t = st["game"]["world_time"]
        t0 = t0 if t0 is not None else t
        if st["phase"] == "dialog":  # a message box blocks input; B cancels it (never A: see the B-069 dialog)
            c.cmd("press B")
            t0 = None
        elif st["phase"] == "in_play" and t - t0 >= a.settle:
            break
        time.sleep(0.5)
    sx = a.turn if a.mode in ("turn", "walkturn") else 0.0
    ly = a.walk if a.mode in ("walk", "walkturn") else 0.0
    if a.mode == "walk" and a.strafe:
        ly = 0.0
    if sx or ly:
        c.cmd(f"stick R {sx} 0")
        c.cmd(f"stick L {a.strafe} {ly}")
    for i in range(a.shots):
        if a.mode == "walkturn" and i % 8 == 7:  # reverse the turn so the player doesn't spin forever
            sx = -sx
            c.cmd(f"stick R {sx} 0")
        c.dualshot(f"{a.tag}{i:02d}")
        time.sleep(a.gap)
        for j in range(a.jitter):
            c.dualshot(f"{a.tag}{i:02d}j{j}")
            time.sleep(a.gap / 2)
    c.cmd("stick R 0 0")
    c.cmd("stick L 0 0")
    st = c.status()
    print("camera", st["camera"], "player", st.get("player", {}).get("x"), st.get("player", {}).get("y"))


if __name__ == "__main__":
    main()
