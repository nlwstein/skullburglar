#!/usr/bin/env python3
"""Parity tour for the hybrid renderer (n1) against the port's own renderer.

Launches the game once in dual-render mode (BOTS_HYBRID_DUAL=1: the port draws its scene as usual and n1 draws the
same frame offscreen, src/gfx/vk/hybrid_fill.cpp), loads each checkpoint with LoadChapter, lets it settle, and takes
N pairs of pictures per checkpoint through the console ("hybrid dualshot <tag>"). Then run tools/parity_report.py on
the output directory.

  python3 tools/parity_tour.py --out out/parity/run1 [--checkpoints 11010,61465] [--shots 3] [--user-template DIR]

Times: commands go through BOTS_EXEC_FILE (polled every 0.5 s by the game); the driver follows the log and waits
for each pair to be written before going on, so slow loads and cold pipeline caches only make the tour longer.

Clock (--clock): by default the waits after a load are in GAME time, so a slow machine and a fast one shoot the same
moment of the scene. The driver starts the game with the instrumentation channel (BOTS_INSTR, a free localhost port,
src/instr.cpp) and reads game.world_time (the level's TimeSeconds) from its status line. --load-secs is then
"seconds of game time after the load" and --gap "game seconds between shots". If the channel is not there (an older
build) the driver says so and falls back to --clock wall: fixed wall-clock sleeps, as before.

Series: --series 71515:8 loads that checkpoint 8 times (with --shots 3, 24 shots), each load shot at the same game
times. Round 0 keeps the tags <id>_a.. and later rounds are <id>_r<N>a.. so parity_report.py + parity_sheet.py --chapter 71515 pool them.

Test runs never use a player's save: the user-data root is a fresh copy of --user-template (or empty) inside --out.
"""
import argparse
import json
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import time

# One or more checkpoints per mission, picked for variety (first digit = mission). Scene notes are what the
# checkpoint starts in; the report groups by mission.
DEFAULT_TOUR = [
    ("11005", "mission 1 start: street, outdoor day, intro"),
    ("11010", "mission 1 corridor: indoor, characters"),
    ("12080", "mission 1 late: courtyard/terrace"),
    ("21115", "mission 2: vehicle (Humvee) through city streets"),
    ("31165", "mission 3 start: mall exterior"),
    ("32225", "mission 3: mall interior"),
    ("33260", "mission 3 late: Evil Bay"),
    ("41270", "mission 4 start: flyover streets"),
    ("43310", "mission 4: middle zone"),
    ("51000", "mission 5 start: theatre plaza"),
    ("52395", "mission 5: theatre interior"),
    ("54450", "mission 5 late: arena/rooftop"),
    ("61465", "mission 6: helicopter minigun, heavy particles"),
    ("61500", "mission 6 late: helicopter flight paths"),
    ("71515", "mission 7 start: mountain city"),
    ("73575", "mission 7: market/medina"),
    ("81610", "mission 8 start: crib entrance"),
    ("83650", "mission 8 late: crib arena"),
    ("91710", "mission 9: desert chase, tank"),
]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", default="out/build/mac-native/bots")
    ap.add_argument("--game", default="game")
    ap.add_argument("--out", required=True, help="output directory (created)")
    ap.add_argument("--checkpoints", help="comma-separated chapter ids (default: the built-in tour)")
    ap.add_argument("--shots", type=int, default=3, help="pairs per checkpoint")
    ap.add_argument("--clock", choices=["auto", "game", "wall"], default="auto",
                    help="what the waits after a load count: game time read from the game (auto: game when the "
                         "instrumentation channel answers, else wall), or wall-clock seconds")
    ap.add_argument("--load-secs", type=float, default=None,
                    help="wait after LoadChapter before the first shot: game seconds since the load in game mode "
                         f"(default {GAME_LOAD_SECS}), wall seconds in wall mode (default {WALL_LOAD_SECS})")
    ap.add_argument("--series", action="append", default=[], metavar="ID:N",
                    help="load checkpoint ID N times in a row, --shots shots each (pooled by the report)")
    ap.add_argument("--n1-ready", type=float, default=0,
                    help="before the first shot, wait up to this many seconds for n1's startup step to finish")
    ap.add_argument("--gap", type=float, default=None,
                    help=f"seconds of play between shots: game seconds between shot times in game mode (default "
                         f"{GAME_GAP}), wall seconds after a shot in wall mode (default {WALL_GAP})")
    ap.add_argument("--shot-timeout", type=float, default=90, help="give up on a pair after this many seconds")
    ap.add_argument("--freeze", choices=["slomo", "pause", "none"], default="slomo",
                    help="hold the world still around each shot: SloMo 0.01 (default), Pause, or nothing")
    ap.add_argument("--random", default="1", help="random play between shots (BOTS_INPUT_RANDOM seed; 0 = off)")
    ap.add_argument("--user-template", help="copied as the run's user-data root (a test root, never a player's)")
    ap.add_argument("--cache", help="port cache root (default: <out>/cache)")
    ap.add_argument("--n1-cache", default="run/n1-cache", help="n1 asset cache (BOTS_N1_CACHE)")
    ap.add_argument("--clear", default="0.5,0,0.5", help="n1 clear colour: marks pixels n1 left empty")
    ap.add_argument("--width", type=int, default=1280)
    ap.add_argument("--height", type=int, default=720)
    ap.add_argument("--env", action="append", default=[], help="extra VAR=value for the game")
    ap.add_argument("--feed", action="store_true",
                    help="also write the scene feed's JSON for the frame of each pair (<out>/feed, scenefeed dump 1)")
    a = ap.parse_args()

    tour = DEFAULT_TOUR
    if a.checkpoints:
        notes = dict(DEFAULT_TOUR)
        tour = [(c, notes.get(c, "")) for c in a.checkpoints.split(",") if c]

    series = {}
    for sv in a.series:
        cid, _, n = sv.partition(":")
        series[cid] = int(n)
    unknown = [c for c in series if c not in dict(tour)]
    if unknown:
        tour = tour + [(c, "") for c in unknown]
    tour = [(c, note, r) for c, note in tour for r in range(series.get(c, 1))]

    out = os.path.abspath(a.out)
    shots = os.path.join(out, "shots")
    os.makedirs(shots, exist_ok=True)
    user = os.path.join(out, "user")
    shutil.rmtree(user, ignore_errors=True)
    if a.user_template:
        shutil.copytree(a.user_template, user)
    else:
        os.makedirs(user)
    cache = os.path.abspath(a.cache) if a.cache else os.path.join(out, "cache")
    os.makedirs(cache, exist_ok=True)
    os.makedirs(a.n1_cache, exist_ok=True)
    exec_file = os.path.join(out, "exec.txt")
    open(exec_file, "w").close()
    log_path = os.path.join(out, "run.log")
    if os.path.exists(log_path):
        os.remove(log_path)

    env = dict(os.environ)
    env.update({
        "BOTS_HYBRID_DUAL": "1",
        "BOTS_HYBRID_DUAL_DIR": shots,
        "BOTS_N1_COOKED": os.path.join(os.path.abspath(a.game), "C1Game", "CookedXenon"),
        "BOTS_N1_CACHE": os.path.abspath(a.n1_cache),
        "BOTS_N1_CLEAR": a.clear,
        "BOTS_EXEC": "30:EnableCheats",
        "BOTS_EXEC_FILE": exec_file,
        "BOTS_INVULNERABLE": "1",
        "BOTS_PRECOMPILE_HOLD": "0",
    })
    instr_port = 0
    if a.clock != "wall":
        with socket.socket() as sk:
            sk.bind(("127.0.0.1", 0))
            instr_port = sk.getsockname()[1]
        env["BOTS_INSTR"] = str(instr_port)
    if a.feed:
        env["BOTS_SCENEFEED_DIR"] = os.path.join(out, "feed")
    if a.random != "0":
        env["BOTS_INPUT_RANDOM"] = f"60:{a.random}"
    for kv in a.env:
        k, _, v = kv.partition("=")
        env[k] = v
    args = [os.path.abspath(a.bin), f"--game_data_root={a.game}", f"--user_data_root={user}",
            f"--cache_root={cache}", f"--log_file={log_path}", "--log_level=info", "--gpu_plugin=native",
            "--fullscreen=false", f"--window_width={a.width}", f"--window_height={a.height}"]
    t0 = time.time()
    proc = subprocess.Popen(args, env=env, stdout=open(os.path.join(out, "stdout.txt"), "w"),
                            stderr=subprocess.STDOUT)
    log = LogFollower(log_path)
    record = {"started": time.strftime("%Y-%m-%d %H:%M:%S"), "args": sys.argv[1:], "freeze": a.freeze,
              "checkpoints": [], "shots": []}

    def send(cmd):
        with open(exec_file, "a") as f:
            f.write(cmd + "\n")

    def alive():
        return proc.poll() is None

    clock = None
    try:
        if not log.wait(lambda l: "BOTS_EXEC: EnableCheats" in l, 240, alive):
            raise RuntimeError("the game never reached its first console command")
        if instr_port:
            clock = GameClock(instr_port)
            if clock.status() is None:
                print("game clock: the instrumentation channel did not answer", flush=True)
                clock = None
            if clock is None and a.clock == "game":
                raise RuntimeError("--clock game, but the game's instrumentation channel is not there")
        mode = "game" if clock else "wall"
        load_secs = a.load_secs if a.load_secs is not None else (GAME_LOAD_SECS if clock else WALL_LOAD_SECS)
        gap = a.gap if a.gap is not None else (GAME_GAP if clock else WALL_GAP)
        record["clock"] = mode
        print(f"clock: {mode} time, load-secs {load_secs:g}, gap {gap:g}", flush=True)
        time.sleep(5)
        for cid, note, rnd in tour:
            if not alive():
                raise RuntimeError("the game exited")
            c0 = time.time()
            print(f"[{c0 - t0:6.0f} s] LoadChapter {cid} ({note})" + (f" round {rnd}" if rnd else ""), flush=True)
            loads0 = clock.loads() if clock else None
            send(f"LoadChapter {cid}")
            if clock:
                if not clock.wait_loaded(loads0, load_secs, alive):
                    print("    game clock: never reached the target; going on", flush=True)
            else:
                time.sleep(load_secs)
            if a.n1_ready > 0 and not record["checkpoints"]:
                w0 = time.time()
                got = log.wait(lambda l: "hybrid n1: startup step done" in l, a.n1_ready, alive)
                print(f"    n1 startup step {'done' if got else 'NOT done'} after {time.time() - w0:.0f} s more",
                      flush=True)
            send("EnableCheats")  # a new level has a new player controller
            entry = {"chapter": cid, "note": note, "shots": [], "load_at": c0 - t0, "round": rnd}
            for k in range(a.shots):
                tag = f"{cid}_{'r%d' % rnd if rnd else ''}{chr(ord('a') + k)}"
                if clock and k:  # shot k is due load_secs + k * gap game seconds after the load
                    clock.wait_time(load_secs + k * gap, loads0 + 1, alive)
                if a.freeze == "slomo":
                    send("SloMo 0.01")
                elif a.freeze == "pause":
                    send("Pause")
                time.sleep(1.0)
                s0 = time.time()
                send(f"hybrid dualshot {tag}")
                rx = re.compile(r"hybrid dual: wrote (\S+-" + re.escape(tag) + r")\.")
                got = log.wait(lambda l: rx.search(l), a.shot_timeout, alive)
                if got and a.feed:  # the world is still held, so this is the pair's scene to within a frame
                    send("scenefeed dump 1")
                    log.wait(lambda l: "scenefeed: wrote" in l or "frame_" in l, 5, alive)
                if a.freeze == "slomo":
                    send("SloMo 1")
                elif a.freeze == "pause":
                    send("Pause")
                if got:
                    stem = rx.search(got).group(1)
                    entry["shots"].append({"tag": tag, "stem": os.path.relpath(stem, out), "secs": time.time() - s0})
                    wt = clock.world_time() if clock else None
                    print(f"    {tag}: {time.time() - s0:.1f} s" + (f", world time {wt:.1f} s" if wt else ""), flush=True)
                else:
                    print(f"    {tag}: no picture within {a.shot_timeout:.0f} s", flush=True)
                if not clock:
                    time.sleep(gap)
            entry["secs"] = time.time() - c0
            record["checkpoints"].append(entry)
    finally:
        record["wall_secs"] = time.time() - t0
        with open(os.path.join(out, "tour.json"), "w") as f:
            json.dump(record, f, indent=1)
        if alive():
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(15)
            except subprocess.TimeoutExpired:
                proc.kill()
    n = sum(len(c["shots"]) for c in record["checkpoints"])
    print(f"tour: {len(record['checkpoints'])} checkpoints, {n} pairs, {record['wall_secs'] / 60:.1f} min -> {out}")


GAME_LOAD_SECS, GAME_GAP = 20.0, 7.7  # shots overshoot their target by 1-3 s, so they read about 22/30/38 s, like the old 25 s wall waits on the Mac
WALL_LOAD_SECS, WALL_GAP = 25.0, 6.0


class GameClock:
    """Reads the level's game time (game.world_time) over the game's instrumentation channel (src/instr.cpp)."""

    def __init__(self, port):
        self.port, self.sock, self.buf = port, None, b""

    def _connect(self):
        try:
            self.sock = socket.create_connection(("127.0.0.1", self.port), timeout=3)
            self.sock.settimeout(5)
            self.buf = b""
        except OSError:
            self.sock = None

    def status(self):
        """The `game` object of a fresh status line, or None."""
        for _ in range(2):
            if self.sock is None:
                self._connect()
                if self.sock is None:
                    return None
            try:
                self.sock.sendall(b"status\n")
                while True:
                    while b"\n" not in self.buf:
                        d = self.sock.recv(65536)
                        if not d:
                            raise OSError("closed")
                        self.buf += d
                    line, self.buf = self.buf.split(b"\n", 1)
                    j = json.loads(line)
                    if j.get("type") == "status":
                        return j.get("game")
            except (OSError, ValueError):
                self.sock = None
        return None

    def loads(self):
        g = self.status()
        return g["loads"] if g else 0

    def world_time(self):
        g = self.status()
        return g["world_time"] if g else None

    def wait_loaded(self, loads_before, secs, alive, timeout=None):
        """After LoadChapter: the load finished and the new level's clock reads >= secs."""
        return self._wait(loads_before + 1, secs, alive, timeout)

    def wait_time(self, secs, loads_min, alive, timeout=None):
        return self._wait(loads_min, secs, alive, timeout)

    def _wait(self, loads_min, secs, alive, timeout):
        end = time.time() + (timeout if timeout else 240 + 3 * secs)
        while time.time() < end and alive():
            g = self.status()
            # since_load_s >= 1: the status has sampled the new level (world_time is read every 200 ms)
            if (g and g["loads"] >= loads_min and not g["loading"] and g["since_load_s"] >= 1
                    and g["world_time"] >= secs):
                return True
            time.sleep(0.2)
        return False


class LogFollower:
    """Reads new lines of a growing log file."""

    def __init__(self, path):
        self.path, self.pos, self.buf = path, 0, ""

    def lines(self):
        try:
            with open(self.path, "r", errors="replace") as f:
                f.seek(self.pos)
                data = f.read()
                self.pos = f.tell()
        except FileNotFoundError:
            return []
        self.buf += data
        *done, self.buf = self.buf.split("\n")
        return done

    def wait(self, pred, timeout, alive):
        end = time.time() + timeout
        while time.time() < end and alive():
            for line in self.lines():
                if pred(line):
                    return line
            time.sleep(0.25)
        return None


if __name__ == "__main__":
    main()
