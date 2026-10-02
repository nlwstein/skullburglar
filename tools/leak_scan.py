"""Keep things that must not be published out of this repo's history (used by .githooks/).

    python3 tools/leak_scan.py staged            pre-commit: staged changes, file types and sizes
    python3 tools/leak_scan.py message FILE      commit-msg: the commit message
    python3 tools/leak_scan.py range A..B        pre-push: every commit in the range, changes + messages
    python3 tools/leak_scan.py tree [PATH...]    files as they are on disk (default: everything tracked)

Blocks: patterns from .githooks/blocklist.txt (plus .githooks/blocklist.local if present; one
case-insensitive regex per line), secret-looking strings, game data file types, and files over 1 MB.
History is never rewritten here, so a leak has to be stopped before the commit exists.
Exit 1 with a report on any finding. gitleaks, when installed, also runs on staged changes.
"""
import os
import re
import shutil
import subprocess
import sys

ROOT = subprocess.run(["git", "rev-parse", "--show-toplevel"], capture_output=True, text=True,
                      check=True).stdout.strip()
HOOKS = os.path.join(ROOT, ".githooks")
SELF_EXEMPT = {".githooks/blocklist.txt", ".githooks/blocklist.local"}
MAX_BYTES = 1 << 20
GAME_EXT = (".xex", ".xexp", ".xxx", ".upk", ".tfc", ".bik", ".iso", ".xsh", ".xpso", ".bin", ".xma")
SECRETS = [
    r"glpat-[0-9A-Za-z_\-]{20,}",
    r"gh[pousr]_[0-9A-Za-z]{30,}",
    r"github_pat_[0-9A-Za-z_]{30,}",
    r"AKIA[0-9A-Z]{16}",
    r"xox[abprs]-[0-9A-Za-z\-]{10,}",
    r"discord(app)?\.com/api/webhooks/\d+/",
    r"-----BEGIN [A-Z ]*PRIVATE KEY-----",
]


def git(*args):
    return subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True, check=True).stdout


def patterns():
    pats = []
    for name in ("blocklist.txt", "blocklist.local"):
        path = os.path.join(HOOKS, name)
        if os.path.exists(path):
            for line in open(path, encoding="utf-8"):
                line = line.strip()
                if line and not line.startswith("#"):
                    pats.append((f"blocklist: {line}", re.compile(line, re.I)))
    pats += [(f"secret: {p}", re.compile(p)) for p in SECRETS]
    return pats


PATS = patterns()


def scan_text(text, where, out):
    for n, line in enumerate(text.splitlines(), 1):
        for label, rx in PATS:
            if rx.search(line):
                out.append(f"{where}:{n}: {label}\n    {line.strip()[:160]}")


def scan_diff(diff, where, out):
    """Added lines of a unified diff, reported per file."""
    path = None
    for line in diff.splitlines():
        if line.startswith("+++ "):
            path = line[6:] if line.startswith("+++ b/") else None
        elif line.startswith("+") and path and path not in SELF_EXEMPT:
            for label, rx in PATS:
                if rx.search(line[1:]):
                    out.append(f"{where}{path}: {label}\n    {line[1:].strip()[:160]}")


def check_file(path, size, where, out):
    if path.lower().endswith(GAME_EXT):
        out.append(f"{where}{path}: game data file type")
    if size > MAX_BYTES:
        out.append(f"{where}{path}: {size} bytes (limit {MAX_BYTES})")


def cmd_staged(out):
    for line in git("diff", "--cached", "--name-only", "--diff-filter=AM", "-z").split("\0"):
        if line:
            size = int(git("cat-file", "-s", f":{line}").strip())
            check_file(line, size, "", out)
    scan_diff(git("diff", "--cached", "-U0", "--no-color"), "", out)
    if shutil.which("gitleaks"):
        if subprocess.run(["gitleaks", "protect", "--staged", "--no-banner", "-v"], cwd=ROOT).returncode:
            out.append("gitleaks reported a secret (above)")


def cmd_message(path, out):
    scan_text(open(path, encoding="utf-8").read(), "commit message", out)


def cmd_range(spec, out):
    for sha in git("rev-list", "--reverse", *spec.split()).split():
        where = f"{sha[:10]} "
        scan_text(git("log", "-1", "--format=%B", sha), f"{where}message", out)
        for row in git("diff-tree", "-r", "--root", "--no-commit-id", "--diff-filter=AM", sha).splitlines():
            meta, path = row.split("\t", 1)
            blob = meta.split()[3]
            check_file(path, int(git("cat-file", "-s", blob).strip()), where, out)
        scan_diff(git("show", "--format=", "-U0", "--no-color", sha), where, out)


def cmd_tree(paths, out):
    files = git("ls-files", "-z", "--", *paths).split("\0")
    for path in filter(None, files):
        full = os.path.join(ROOT, path)
        if not os.path.isfile(full) or path in SELF_EXEMPT:
            continue
        check_file(path, os.path.getsize(full), "", out)
        try:
            text = open(full, encoding="utf-8").read()
        except UnicodeDecodeError:
            continue
        scan_text(text, path, out)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    mode, rest, out = sys.argv[1], sys.argv[2:], []
    if mode == "staged":
        cmd_staged(out)
    elif mode == "message":
        cmd_message(rest[0], out)
    elif mode == "range":
        cmd_range(" ".join(rest), out)
    elif mode == "tree":
        cmd_tree(rest, out)
    else:
        sys.exit(__doc__)
    if out:
        print("leak_scan: blocked. This repo's history is public and never rewritten:", file=sys.stderr)
        print("\n".join(out), file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
