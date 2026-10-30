#!/usr/bin/env python3
"""Drive and measure a running game through its test instrumentation (BOTS_INSTR, src/instr.cpp).

The game must run with BOTS_INSTR=1 (control channel on 127.0.0.1:47100). For another machine, forward the port
(ssh -L 47100:127.0.0.1:47100 host) or start the game with BOTS_INSTR_BIND=0.0.0.0.

  botsctl.py cmd status                     one command, print the JSON reply
  botsctl.py cmd wait -t 600 phase==in_play
  botsctl.py watch                          print the status stream and events
  botsctl.py run SCENARIO --out DIR         run a scenario, record the stream, write DIR/summary.json
  botsctl.py summary DIR                    print a run's summary again

Scenario files: one step per line, # comments. Steps the driver handles itself:
  sample SECS [shot_every=S] [name=NAME]    measure SECS seconds of play (see "Validity"); shots every S seconds
  dismiss [BTN] [timeout=S]                 while a dialog is up, press BTN (default B: cancel); state-driven
  press_until COND BTN [every=MS] [timeout=S]  press BTN every MS (default 700) until COND holds
  until COND [btn=B] [timeout=S]            wait for COND (default 600 s), dismissing dialogs with BTN meanwhile
  expect COND                               fail the run (INVALID) unless COND holds now
  shot NAME                                 DIR/shots/NAME.png (+ .json sidecar with the status at that moment)
  burst NAME COUNT [MS]                     COUNT consecutive frames into DIR/shots/NAME/; with MS a trace: every
                                            present in the TSV, a frame copied every MS (and after any 50 ms gap)
  dualshot NAME                             hybrid dual render (port + n1) into the game's BOTS_HYBRID_DUAL_DIR
  sleep SECS                                (only for settling; never to wait for a state: use wait)
  mark NAME                                 a timestamp in the summary
  default VAR=VALUE                         ${VAR} when --set doesn't give it
Anything else goes to the game unchanged (wait, exec, press, hold, release, stick, trigger, macro, ...); a reply with
"ok": false fails the run, except for steps prefixed with `try ` (e.g. `try wait -t 5 phase==dialog`).

Validity: a sample window is INVALID if, during it, the phase was ever not in_play, a frame took longer than
--stall-ms to arrive (presentation stopped: GPU hang, lost device), the presenter logged a failure, pipelines were
built in play (port or n1 late pipelines), n1 failed, or no frames arrived. It is CONTAMINATED if --procs-cmd
reports a game process other than this one (two runs sharing a machine), or the game saw keyboard / mouse input this
script didn't send (status input.human rose: someone used the machine). Exit codes: 0 valid, 4 INVALID,
5 CONTAMINATED, 2 the scenario failed.
"""
import argparse
import json
import math
import os
import shlex
import socket
import subprocess
import sys
import threading
import time

BUCKET_MS = 0.5


class Conn:
    """One line-based connection: send a command, read its one-line JSON reply."""

    def __init__(self, host, port, timeout=30.0):
        # Retries until the game listens. Through an ssh tunnel the connect itself succeeds while the game is still
        # starting and the tunnel then drops it, so a connection only counts once it answers a "status".
        deadline = time.time() + timeout
        while True:
            try:
                self.sock = socket.create_connection((host, port), timeout=5)
                self.buf = b""
                self.sock.sendall(b"status\n")
                self.readline(10)
                break
            except (OSError, ConnectionError):
                if time.time() > deadline:
                    raise
                time.sleep(1)
        self.sock.settimeout(None)

    def readline(self, timeout=None):
        self.sock.settimeout(timeout)
        while b"\n" not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("game closed the connection")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return line.decode("utf-8", "replace")

    def cmd(self, text, timeout=None):
        self.sock.sendall((text + "\n").encode())
        return json.loads(self.readline(timeout))

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


class Recorder(threading.Thread):
    """A second connection in watch mode: every status line and event, to DIR/stream.jsonl and memory."""

    def __init__(self, host, port, path, hz):
        super().__init__(daemon=True)
        self.conn = Conn(host, port)
        self.file = open(path, "a", encoding="utf-8")
        self.lines = []
        self.lock = threading.Lock()
        self.hz = hz
        self.alive = True

    def run(self):
        try:
            self.conn.sock.sendall(f"watch {self.hz}\n".encode())
            while self.alive:
                line = self.conn.readline()
                try:
                    obj = json.loads(line)
                except ValueError:
                    continue
                obj.setdefault("host_t", time.time())
                with self.lock:
                    self.lines.append(obj)
                self.file.write(json.dumps(obj) + "\n")
                self.file.flush()
        except (OSError, ConnectionError):
            pass
        self.alive = False

    def since(self, t0):
        with self.lock:
            return [o for o in self.lines if o.get("host_t", 0) >= t0]

    def stop(self):
        self.alive = False
        self.conn.close()
        self.file.close()


def hist_delta(a, b):
    """Two `hist` replies -> {bucket: count} of the frames between them."""
    first = dict((i, n) for i, n in a.get("buckets", []))
    out = {}
    for i, n in b.get("buckets", []):
        d = n - first.get(i, 0)
        if d > 0:
            out[i] = d
    return out


def percentile_ms(hist, q):
    total = sum(hist.values())
    if not total:
        return 0.0
    want = math.ceil(q * total)
    seen = 0
    for i in sorted(hist):
        seen += hist[i]
        if seen >= want:
            return (i + 1) * BUCKET_MS
    return 0.0


def frame_stats(hist, secs):
    frames = sum(hist.values())
    total_ms = sum((i + 0.5) * BUCKET_MS * n for i, n in hist.items())
    p50 = percentile_ms(hist, 0.5)
    p90 = percentile_ms(hist, 0.9)
    return {
        "frames": frames,
        "secs": round(secs, 2),
        "fps_mean": round(frames / secs, 1) if secs else 0,
        "fps_mean_frametime": round(1000 * frames / total_ms, 1) if total_ms else 0,
        "fps_median": round(1000 / p50, 1) if p50 else 0,
        "fps_p10": round(1000 / p90, 1) if p90 else 0,
        "frame_ms_p50": p50,
        "frame_ms_p90": p90,
        "frame_ms_p99": percentile_ms(hist, 0.99),
        "frame_ms_max": (max(hist) + 1) * BUCKET_MS if hist else 0,
        "over_33ms": sum(n for i, n in hist.items() if i * BUCKET_MS >= 33.5),
        "over_50ms": sum(n for i, n in hist.items() if i * BUCKET_MS >= 50),
    }


def get(d, dotted, default=None):
    for part in dotted.split("."):
        if not isinstance(d, dict) or part not in d:
            return default
        d = d[part]
    return d


class Run:
    def __init__(self, args):
        self.args = args
        self.out = args.out
        os.makedirs(os.path.join(self.out, "shots"), exist_ok=True)
        self.conn = Conn(args.host, args.port, timeout=args.connect_timeout)
        self.rec = Recorder(args.host, args.port, os.path.join(self.out, "stream.jsonl"), args.hz)
        self.rec.start()
        self.t0 = time.time()
        self.samples = []
        self.marks = {}
        self.steps = []
        self.failed = None
        self.contaminated = []
        self.pid = None

    def log(self, msg):
        line = f"[{time.time() - self.t0:7.1f}s] {msg}"
        print(line, flush=True)
        with open(os.path.join(self.out, "driver.log"), "a", encoding="utf-8") as f:
            f.write(line + "\n")

    def game_path(self, rel):
        """A path the game can write to: the remote shot directory when the game runs elsewhere."""
        base = self.args.remote_out or os.path.abspath(self.out)
        return base.rstrip("/\\") + "/" + rel

    def send(self, text, timeout=None):
        reply = self.conn.cmd(text, timeout)
        if self.pid is None:
            st = reply if reply.get("type") == "status" else reply.get("status", {})
            self.pid = st.get("pid", self.pid)
        return reply

    def status(self):
        st = self.send("status")
        self.pid = st.get("pid", self.pid)
        return st

    def check_contamination(self):
        if not self.args.procs_cmd:
            return
        try:
            out = subprocess.run(self.args.procs_cmd, shell=True, capture_output=True, text=True, timeout=60).stdout
        except subprocess.TimeoutExpired:
            return
        pids = [int(w) for w in out.split() if w.isdigit()]
        foreign = [p for p in pids if p != self.pid]
        if foreign:
            self.contaminated.append({"t": round(time.time() - self.t0, 1), "pids": foreign})
            self.log(f"CONTAMINATED: other game processes {foreign}")

    def condition(self, cond):
        st = {}
        reply = self.send(f"wait -t 0 {cond}")
        return reply.get("ok", False), reply

    # ---- steps
    def step_sample(self, words):
        secs = float(words[1])
        opts = dict(w.split("=", 1) for w in words[2:] if "=" in w)
        name = opts.get("name", f"sample{len(self.samples) + 1}")
        shot_every = float(opts.get("shot_every", 0))
        c0 = len(self.contaminated)
        self.check_contamination()
        h0 = self.send("hist")
        s0 = self.status()
        t_start = time.time()
        next_shot = t_start + shot_every if shot_every else None
        n_shot = 0
        next_procs = t_start + 10
        while time.time() - t_start < secs:
            time.sleep(min(1.0, max(0.0, t_start + secs - time.time())))
            if time.time() >= next_procs:  # another game process during the window, not only at its ends
                self.check_contamination()
                next_procs += 10
            if next_shot and time.time() >= next_shot:
                n_shot += 1
                self.do_shot(f"{name}-{n_shot:02d}")
                next_shot += shot_every
        h1 = self.send("hist")
        s1 = self.status()
        wall = time.time() - t_start
        self.check_contamination()
        hist = hist_delta(h0, h1)
        human = (get(s1, "input.human", 0) or 0) - (get(s0, "input.human", 0) or 0)
        if human > 0:
            self.contaminated.append({"t": round(time.time() - self.t0, 1), "human_input": human})
            self.log(f"CONTAMINATED: {human} keyboard/mouse events the script didn't send")
        stream = [o for o in self.rec.since(t_start) if o.get("host_t", 0) <= time.time()]
        statuses = [o for o in stream if o.get("type") == "status"] + [s1]
        events = [o for o in stream if o.get("type") == "event"]
        reasons = []
        phases = sorted(set(o.get("phase") for o in statuses))
        if phases != ["in_play"]:
            reasons.append(f"not in play for the whole window (phases {phases})")
        # A stall shows as a status line with no recent frame, or (when the whole process stalled, so no status line
        # was written meanwhile) as the longest frame of the next line's window.
        worst_age = max([get(o, "render.frame_age_ms", 0) or 0 for o in statuses]
                        + [get(o, "render.worst_ms", 0) or 0 for o in statuses[1:]] + [0])
        if worst_age > self.args.stall_ms:
            reasons.append(f"presentation stalled ({worst_age:.0f} ms without a frame)")
        for key, label in (("health.presenter_failures", "presenter failures"),
                           ("health.builds_in_play", "pipelines built in play (port)"),
                           ("render.n1.late_pipelines", "n1 late pipelines built in play"),
                           ("render.n1.build_failures", "n1 pipeline build failures")):
            d = (get(s1, key, 0) or 0) - (get(s0, key, 0) or 0)
            if d > 0:
                reasons.append(f"{label}: {d}")
        if get(s1, "render.n1.failed"):
            reasons.append("n1 failed (hybrid fell back)")
        stats = frame_stats(hist, wall)
        if stats["frames"] == 0:
            reasons.append("no frames")
        hitches = [e.get("text", "") for e in events if e.get("kind") == "hitch"]
        sample = {
            "name": name,
            "start_s": round(t_start - self.t0, 1),
            "map": get(s1, "game.map"),
            "chapter": get(s1, "game.chapter"),
            "valid": not reasons,
            "contaminated": len(self.contaminated) > c0,
            "invalid_reasons": reasons,
            **stats,
            "hitch_events": len(hitches),
            "hitches": hitches[:20],
            "audio_underruns": (get(s1, "health.audio_underruns", 0) or 0)
            - (get(s0, "health.audio_underruns", 0) or 0),
            "particle_dropped": (get(s1, "render.n1.particle_dropped", 0) or 0)
            - (get(s0, "render.n1.particle_dropped", 0) or 0),
            "errors": (get(s1, "health.errors", 0) or 0) - (get(s0, "health.errors", 0) or 0),
            "render_size": f"{get(s1, 'render.w')}x{get(s1, 'render.h')}",
            "cpu": s1.get("cpu"),
        }
        self.samples.append(sample)
        verdict = ("CONTAMINATED" if sample["contaminated"] else
                   "VALID" if not reasons else "INVALID: " + "; ".join(reasons))
        self.log(f"sample {name}: {verdict}; "
                 f"{stats['fps_mean']} fps mean, median {stats['fps_median']}, p10 {stats['fps_p10']}, "
                 f">33ms {stats['over_33ms']}, >50ms {stats['over_50ms']}")

    def do_shot(self, name):
        path = self.game_path(f"shots/{name}.png")
        r = self.send(f"shot {path}", timeout=60)
        self.log(f"shot {name}: {'ok' if r.get('ok') else r.get('error')}")
        if r.get("ok") and self.args.fetch_cmd:
            subprocess.run(self.args.fetch_cmd.format(src=path, dst=os.path.join(self.out, "shots")), shell=True)
        return r

    def step_dismiss(self, words):
        btn = words[1] if len(words) > 1 and "=" not in words[1] else "B"
        opts = dict(w.split("=", 1) for w in words[1:] if "=" in w)
        deadline = time.time() + float(opts.get("timeout", 30))
        n = 0
        while time.time() < deadline:
            st = self.status()
            if st.get("phase") != "dialog":
                break
            self.log(f"dismiss: dialog {get(st, 'game.dialog')} -> {btn}")
            self.send(f"press {btn}")
            n += 1
            self.send("wait -t 3 phase!=dialog")
        else:
            raise RuntimeError("dialog still up")
        self.log(f"dismiss: {n} dialogs")

    def step_until(self, words):
        """until COND [btn=B] [timeout=S]: wait for COND, dismissing any dialog that comes up meanwhile."""
        cond = words[1]
        opts = dict(w.split("=", 1) for w in words[2:] if "=" in w)
        btn = opts.get("btn", "B")
        deadline = time.time() + float(opts.get("timeout", 600))
        while time.time() < deadline:
            r = self.send(f"wait -t 2 {cond} || phase==dialog")
            st = r.get("status", {})
            if r.get("ok") and st.get("phase") != "dialog":
                self.log(f"  until: {cond} after {time.time() - self.t0:.1f}s, phase {st.get('phase')}")
                return
            if st.get("phase") == "hung":
                raise RuntimeError(f"until: the game thread stopped (phase hung, scene {get(st, 'game.scene')})")
            if st.get("phase") == "dialog":
                self.log(f"  until: dialog {get(st, 'game.dialog')} -> {btn}")
                self.send(f"press {btn}")
                self.send("wait -t 3 phase!=dialog")
        raise RuntimeError(f"until: {cond} never held")

    def step_press_until(self, words):
        cond, btn = words[1], words[2]
        opts = dict(w.split("=", 1) for w in words[3:] if "=" in w)
        every = float(opts.get("every", 700)) / 1000
        deadline = time.time() + float(opts.get("timeout", 60))
        while time.time() < deadline:
            r = self.send(f"wait -t {every} {cond}")
            if r.get("ok"):
                return
            self.send(f"press {btn}")
        raise RuntimeError(f"press_until: {cond} never held")

    def run_step(self, line):
        words = shlex.split(line, comments=False, posix=True)
        op = words[0]
        if op == "try" and words[1] in ("sample", "dismiss", "press_until", "expect", "shot", "burst"):
            try:  # an optional driver step: a failure is logged, not fatal
                return self.run_step(line.split(None, 1)[1])
            except Exception as e:  # noqa: BLE001
                self.log(f"  (optional step failed: {e})")
                return None
        t = time.time()
        self.log(f"> {line}")
        if op == "sample":
            self.step_sample(words)
        elif op == "dismiss":
            self.step_dismiss(words)
        elif op == "press_until":
            self.step_press_until(words)
        elif op == "until":
            self.step_until(words)
        elif op == "expect":
            ok, reply = self.condition(line[len("expect"):].strip())
            if not ok:
                raise RuntimeError(f"expect failed: {line} (phase {get(reply, 'status.phase')})")
        elif op == "shot":
            if not self.do_shot(words[1]).get("ok"):
                raise RuntimeError("shot failed")
        elif op == "burst":
            every = f" {words[3]}" if len(words) > 3 else ""
            r = self.send(f"burst {self.game_path('shots/' + words[1])} {words[2] if len(words) > 2 else 30}{every}")
            if not r.get("ok"):
                raise RuntimeError(r.get("error"))
        elif op == "dualshot":  # the pair goes to the game's BOTS_HYBRID_DUAL_DIR, named after the tag
            r = self.send(f"dualshot {words[1]}")
            self.log(f"dualshot {words[1]}: {'ok, into ' + str(r.get('dir')) if r.get('ok') else r.get('error')}")
        elif op == "sleep":
            time.sleep(float(words[1]))
        elif op == "mark":
            self.marks[words[1]] = round(time.time() - self.t0, 1)
        else:
            optional = op == "try"
            text = line.split(None, 1)[1] if optional else line
            r = self.send(text)
            if op in ("objs", "enemies", "inspect", "elems", "trace"):  # queries: keep the answer in the run log
                self.log("  reply " + json.dumps(r)[:2000])
            if not r.get("ok", True) and not optional:
                raise RuntimeError(f"{text}: {r.get('error')} (phase {get(r, 'status.phase')})")
            if text.startswith("wait"):
                self.log(f"  waited {r.get('waited_s', 0):.1f}s, phase {get(r, 'status.phase')} "
                         f"map {get(r, 'status.game.map')}")
        self.steps.append({"step": line, "secs": round(time.time() - t, 2)})

    def run(self, lines):
        st = self.status()
        self.log(f"connected: pid {st.get('pid')} phase {st.get('phase')}")
        self.check_contamination()
        try:
            for line in lines:
                self.run_step(line)
        except Exception as e:  # noqa: BLE001  any failure ends the scenario with a report
            self.failed = str(e)
            self.log(f"FAILED: {e}")
        return self.finish()

    def finish(self):
        try:
            st = self.status()
        except (OSError, ConnectionError, ValueError):
            st = {}
        events = [o for o in self.rec.since(0) if o.get("type") == "event"]
        loads = [e for e in events if e.get("kind") == "load_end"]
        summary = {
            "verdict": ("CONTAMINATED" if self.contaminated else
                        "FAILED" if self.failed else
                        "VALID" if self.samples and all(s["valid"] for s in self.samples) else
                        "INVALID" if self.samples else "NO_SAMPLES"),
            "failed": self.failed,
            "contaminated": self.contaminated,
            "pid": self.pid,
            "samples": self.samples,
            "phase_first_s": st.get("phase_first_s"),
            "precompile_done_s": get(st, "precompile.done_at_s"),
            "n1_precompile_s": get(st, "precompile.n1_secs"),
            "loads_s": [round(e.get("secs", 0), 2) for e in loads],
            "marks": self.marks,
            "steps": self.steps,
            "final_status": st,
        }
        with open(os.path.join(self.out, "summary.json"), "w", encoding="utf-8") as f:
            json.dump(summary, f, indent=1)
        self.rec.stop()
        print_summary(summary)
        return {"VALID": 0, "INVALID": 4, "CONTAMINATED": 5}.get(summary["verdict"], 2)


def print_summary(s):
    print(f"\nverdict: {s['verdict']}" + (f" ({s['failed']})" if s.get("failed") else ""))
    print(f"precompile done at {s.get('precompile_done_s')} s (n1 step {s.get('n1_precompile_s')} s); "
          f"loads {s.get('loads_s')} s; first entered {s.get('phase_first_s')}")
    print(f"{'sample':<12} {'map':<14} {'valid':<6} {'mean':>6} {'median':>7} {'p10':>6} {'>33ms':>6} {'>50ms':>6} "
          f"{'hitch':>6} {'underrun':>8}")
    for x in s.get("samples", []):
        print(f"{x['name']:<12} {str(x.get('map'))[:14]:<14} {str(x['valid']):<6} {x['fps_mean']:>6} "
              f"{x['fps_median']:>7} {x['fps_p10']:>6} {x['over_33ms']:>6} {x['over_50ms']:>6} "
              f"{x['hitch_events']:>6} {x['audio_underruns']:>8}")
        for r in x["invalid_reasons"]:
            print(f"    INVALID: {r}")
        if x.get("contaminated"):
            print("    CONTAMINATED: another game process or keyboard/mouse input during this window")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", default=os.environ.get("BOTS_INSTR_HOST", "127.0.0.1"))
    p.add_argument("--port", type=int, default=int(os.environ.get("BOTS_INSTR_PORT", "47100")))
    p.add_argument("--connect-timeout", type=float, default=120)
    sub = p.add_subparsers(dest="mode", required=True)
    c = sub.add_parser("cmd")
    c.add_argument("text", nargs=argparse.REMAINDER)
    w = sub.add_parser("watch")
    w.add_argument("--hz", type=float, default=2)
    r = sub.add_parser("run")
    r.add_argument("scenario")
    r.add_argument("--out", required=True)
    r.add_argument("--hz", type=float, default=2, help="recorded status lines per second")
    r.add_argument("--stall-ms", type=float, default=1000, help="longest gap between frames before INVALID")
    r.add_argument("--procs-cmd", help="prints the PIDs of every game process on the game's machine")
    r.add_argument("--remote-out", help="the --out directory as the game's machine sees it (shots)")
    r.add_argument("--fetch-cmd", help="copies a shot back: {src} remote path, {dst} local dir")
    r.add_argument("--set", action="append", default=[], help="VAR=value for ${VAR} in the scenario")
    s = sub.add_parser("summary")
    s.add_argument("dir")
    a = p.parse_args()

    if a.mode == "cmd":
        conn = Conn(a.host, a.port, timeout=a.connect_timeout)
        print(json.dumps(conn.cmd(" ".join(a.text)), indent=1))
        return 0
    if a.mode == "watch":
        conn = Conn(a.host, a.port, timeout=a.connect_timeout)
        conn.sock.sendall(f"watch {a.hz}\n".encode())
        while True:
            print(conn.readline(), flush=True)
    if a.mode == "summary":
        with open(os.path.join(a.dir, "summary.json"), encoding="utf-8") as f:
            print_summary(json.load(f))
        return 0
    variables = dict(kv.split("=", 1) for kv in a.set)
    lines = []
    with open(a.scenario, encoding="utf-8") as f:
        for raw in f:
            line = raw.split("#", 1)[0].strip() if not raw.lstrip().startswith("exec") else raw.strip()
            if line.startswith("default "):  # default VAR=value: a --set the command line didn't give
                k, v = line[8:].strip().split("=", 1)
                variables.setdefault(k.strip(), v.strip())
                continue
            for k, v in variables.items():
                line = line.replace("${" + k + "}", v)
            if line:
                lines.append(line)
    return Run(a).run(lines)


if __name__ == "__main__":
    sys.exit(main())
