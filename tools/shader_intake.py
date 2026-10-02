"""Shader intake (B-053): bring pipelines captured by play sessions into the precompile master set.

    uv run --with xxhash python tools/shader_intake.py pull local|<target>|<dir> [--label NAME]
    uv run --with xxhash python tools/shader_intake.py merge
    uv run --with xxhash python tools/shader_intake.py push local|<target>
    uv run --with xxhash python tools/shader_intake.py status

The flow after someone plays (RUNBOOK "Shader intake"):
  1. pull   copies the machine's cache/shaders/shareable into shaders/captures/<source>-<stamp>-<label>/
            and prints what it adds over the current master (new pipelines per backend/path).
  2. merge  rebuilds shaders/master/ from every capture (dedupe by hash, oldest capture first, so
            the order pipelines were first seen is kept: the SDK creates them in file order).
  3. push   copies shaders/master/ over the machine's cache, so its next launch precompiles all of
            it in the background. Refuses while the game is running there (it appends to the files),
            or if the target holds records master lacks (pull + merge first).
shaders/ is gitignored: the .xsh holds the game's shader microcode (derived game data).
Every capture gets a manifest.json (source, time, record counts, file headers), and merge writes
shaders/master/manifest.json listing the captures it was built from.
"""
import argparse
import datetime
import json
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shader_cache_merge as scm  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STORE = os.path.join(REPO, "shaders")
CAPTURES = os.path.join(STORE, "captures")
MASTER = os.path.join(STORE, "master")

# Where each machine keeps its shader storage, and how to tell whether the game runs there. Built in:
# this checkout's own run/ ("local", alias "mac"). More machines come from a JSON file of the same shape,
# its path in $SHADER_TARGETS: {"name": {"remote": "user@host:path", "running": [argv...],
# "running_match": "text or null", "mkdir": true}}. "mkdir" creates the remote dir before a push.
_LOCAL = {
    "local": os.path.join(REPO, "run", "cache", "shaders", "shareable"),
    "running": ["sh", "-c", "pgrep -x bots || true"],
    "running_match": None,  # any pid output
}
TARGETS = {"local": _LOCAL, "mac": _LOCAL}
if os.environ.get("SHADER_TARGETS"):
    with open(os.environ["SHADER_TARGETS"]) as f:
        TARGETS.update(json.load(f))


def counts(directory):
    out = {}
    for name in sorted(os.listdir(directory)):
        path = os.path.join(directory, name)
        if name.endswith(".xsh"):
            header, records = scm.read_xsh(path)
        elif name.endswith(".xpso"):
            header, _, records = scm.read_xpso(path)
        else:
            continue
        out[name] = {"records": len(records), "header": header.hex(), "keys": [k for k, _ in records]}
    return out


def keyset(entry):
    # xsh keys are u64 hashes, xpso keys are whole records (bytes): both hashable.
    return set(entry["keys"])


def report_new(capture_dir):
    cap = counts(capture_dir)
    master = counts(MASTER) if os.path.isdir(MASTER) else {}
    total_new = 0
    for name, entry in cap.items():
        have = keyset(master[name]) if name in master else set()
        new = len(keyset(entry) - have)
        total_new += new
        hdr = ""
        if name in master and master[name]["header"] != entry["header"]:
            hdr = "  ! header differs from master (other SDK version?): merge will skip it"
        print(f"  {name}: {entry['records']} records, {new} not in master{hdr}")
    return total_new


def game_running(target):
    t = TARGETS[target]
    out = subprocess.run(t["running"], capture_output=True, text=True).stdout
    return (t["running_match"] in out) if t["running_match"] else bool(out.strip())


def cmd_pull(args):
    source = args.source
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    name = f"{os.path.basename(os.path.normpath(source)) if source not in TARGETS else source}-{stamp}"
    if args.label:
        name += f"-{args.label}"
    dest = os.path.join(CAPTURES, name)
    os.makedirs(dest)
    if source in TARGETS and "remote" in TARGETS[source]:
        if game_running(source):
            print(f"note: the game is running on {source}; pulling a snapshot of what's written so far")
        rc = subprocess.run(["scp", "-q", "-o", "LogLevel=ERROR", TARGETS[source]["remote"] + "/*", dest]).returncode
        if rc:
            shutil.rmtree(dest)
            sys.exit(f"scp from {source} failed")
    else:
        src = TARGETS[source]["local"] if source in TARGETS else source
        for f in os.listdir(src):
            if f.endswith((".xsh", ".xpso")):
                shutil.copy2(os.path.join(src, f), dest)
    entries = counts(dest)
    if not entries:
        shutil.rmtree(dest)
        sys.exit("no .xsh/.xpso files found")
    manifest = {
        "source": source, "label": args.label, "pulled": stamp,
        "files": {n: {"records": e["records"], "header": e["header"]} for n, e in entries.items()},
    }
    with open(os.path.join(dest, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2)
    print(f"capture {name}:")
    new = report_new(dest)
    print(f"=> {new} new records vs master. Next: `merge`, then `push <target>`.")


def cmd_merge(args):
    caps = sorted(d for d in os.listdir(CAPTURES) if os.path.isdir(os.path.join(CAPTURES, d))) \
        if os.path.isdir(CAPTURES) else []
    if not caps:
        sys.exit("no captures yet: run `pull` first")
    # Oldest first (by pull stamp in the manifest), so first-seen order is kept.
    def pulled(d):
        try:
            return json.load(open(os.path.join(CAPTURES, d, "manifest.json")))["pulled"]
        except (OSError, KeyError, ValueError):
            return ""
    caps.sort(key=pulled)
    dirs = [os.path.join(CAPTURES, d) for d in caps]
    tmp = MASTER + ".new"
    shutil.rmtree(tmp, ignore_errors=True)
    os.makedirs(tmp)
    summary = {}
    for name, paths in sorted(scm.collect(dirs).items()):
        print(name)
        header, records = scm.merge(name, paths)
        with open(os.path.join(tmp, name), "wb") as f:
            f.write(header)
            for blob in records:
                f.write(blob)
        summary[name] = len(records)
        print(f"  => {len(records)} unique")
    with open(os.path.join(tmp, "manifest.json"), "w") as f:
        json.dump({"built": datetime.datetime.now().isoformat(timespec="seconds"),
                   "captures": caps, "records": summary}, f, indent=2)
    shutil.rmtree(MASTER, ignore_errors=True)
    os.replace(tmp, MASTER)
    print(f"master: {summary}")


def unpulled_records(target):
    t = TARGETS[target]
    tmp = os.path.join(STORE, ".push-check")
    shutil.rmtree(tmp, ignore_errors=True)
    os.makedirs(tmp)
    try:
        if "remote" in t:
            subprocess.run(["scp", "-q", "-o", "LogLevel=ERROR", t["remote"] + "/*", tmp],
                           capture_output=True)
        elif os.path.isdir(t["local"]):
            for f in os.listdir(t["local"]):
                if f.endswith((".xsh", ".xpso")):
                    shutil.copy2(os.path.join(t["local"], f), tmp)
        there, master = counts(tmp), counts(MASTER)
        return sum(len(keyset(e) - (keyset(master[n]) if n in master else set()))
                   for n, e in there.items())
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def cmd_push(args):
    target = args.target
    if not os.path.isdir(MASTER):
        sys.exit("no master yet: run `merge` first")
    if game_running(target):
        sys.exit(f"the game is running on {target}: it appends to these files, so push after it exits")
    files = [os.path.join(MASTER, f) for f in os.listdir(MASTER) if f.endswith((".xsh", ".xpso"))]
    t = TARGETS[target]
    # Pushing replaces the target's files: refuse if it holds records master lacks (played since
    # the last pull), so nothing captured is lost. `pull` + `merge` first, or --force.
    if not args.force:
        lost = unpulled_records(target)
        if lost:
            sys.exit(f"{target} has {lost} records not in master (captured since the last pull): "
                     f"run `pull {target}` and `merge` first, or push --force")
    if "remote" in t:
        host, path = t["remote"].split(":", 1)
        if t.get("mkdir"):
            subprocess.run(["ssh", host, f"mkdir -p {path}"], check=True)
        rc = subprocess.run(["scp", "-q", "-o", "LogLevel=ERROR", *files, t["remote"] + "/"]).returncode
        if rc:
            sys.exit(f"scp to {target} failed")
    else:
        os.makedirs(t["local"], exist_ok=True)
        for f in files:
            shutil.copy2(f, t["local"])
    print(f"pushed {len(files)} files to {target}; its next launch precompiles them in the background")


def cmd_status(args):
    if os.path.isdir(MASTER):
        m = json.load(open(os.path.join(MASTER, "manifest.json")))
        print(f"master built {m['built']} from {len(m['captures'])} captures: {m['records']}")
    else:
        print("no master yet")
    if os.path.isdir(CAPTURES):
        for d in sorted(os.listdir(CAPTURES)):
            try:
                man = json.load(open(os.path.join(CAPTURES, d, "manifest.json")))
                print(f"  {d}: " + ", ".join(f"{n} {v['records']}" for n, v in man["files"].items()))
            except (OSError, ValueError):
                print(f"  {d}: (no manifest)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("pull")
    p.add_argument("source", help="local, a $SHADER_TARGETS name, or a directory")
    p.add_argument("--label", help="what was played, e.g. mission1")
    sub.add_parser("merge")
    p = sub.add_parser("push")
    p.add_argument("target", choices=sorted(TARGETS))
    p.add_argument("--force", action="store_true", help="push even if the target has unpulled records")
    sub.add_parser("status")
    a = ap.parse_args()
    {"pull": cmd_pull, "merge": cmd_merge, "push": cmd_push, "status": cmd_status}[a.cmd](a)


if __name__ == "__main__":
    main()
