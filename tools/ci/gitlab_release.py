#!/usr/bin/env python3
"""Create the GitLab release v<VERSION> for this commit, linking the packages the build jobs uploaded.

    VERSION=0.1.0 LAST=0.0.9 python3 tools/ci/gitlab_release.py

Run by .gitlab-ci.yml's release job with its CI_* variables. GitLab creates the tag from CI_COMMIT_SHA along with the
release. The notes list the releasable commits since LAST (empty for the first release); the packages stay in this
project's package registry (skullburglar/<VERSION>) and are private like the project.
"""
import json
import os
import subprocess
import sys
import urllib.parse
import urllib.request

API = os.environ["CI_API_V4_URL"]
PROJECT = os.environ["CI_PROJECT_ID"]
TOKEN = os.environ["CI_JOB_TOKEN"]
VERSION = os.environ["VERSION"]
LAST = os.environ.get("LAST", "")
SHA = os.environ["CI_COMMIT_SHA"]

# Every package a release must have (tools/ci/package.sh names).
FILES = [
    ("Linux x86_64 (AppImage)", f"skullburglar-{VERSION}-linux-x86_64.AppImage"),
    ("Linux x86_64 (tar.xz)", f"skullburglar-{VERSION}-linux-x86_64.tar.xz"),
    ("Linux x86_64 debug symbols", f"skullburglar-{VERSION}-linux-x86_64-symbols.tar.xz"),
    ("Windows x86_64", f"skullburglar-{VERSION}-windows-x86_64.zip"),
    ("Windows x86_64 debug symbols", f"skullburglar-{VERSION}-windows-x86_64-symbols.zip"),
]
SECTIONS = [("feat", "Features"), ("fix", "Fixes"), ("perf", "Performance")]


def call(method, path, body=None):
    req = urllib.request.Request(f"{API}/projects/{PROJECT}{path}", method=method,
                                 data=json.dumps(body).encode() if body is not None else None,
                                 headers={"JOB-TOKEN": TOKEN, "Content-Type": "application/json"})
    with urllib.request.urlopen(req) as r:
        return json.load(r)


def notes():
    rng = f"v{LAST}..{SHA}" if LAST else SHA
    subjects = subprocess.run(["git", "log", "--no-merges", "--format=%s", rng],
                              capture_output=True, text=True, check=True).stdout.splitlines()
    out = []
    for kind, title in SECTIONS:
        items = [s for s in subjects if s.split(":")[0].split("(")[0].rstrip("!") == kind]
        if items:
            out += [f"### {title}", ""] + [f"- {s}" for s in items] + [""]
    return "\n".join(out) or "Initial release."


def main():
    pkg = f"/packages/generic/skullburglar/{urllib.parse.quote(VERSION)}"
    found = call("GET", "/packages?package_type=generic&package_name=skullburglar&package_version="
                 + urllib.parse.quote(VERSION))
    have = {f["file_name"] for p in found for f in call("GET", f"/packages/{p['id']}/package_files?per_page=100")}
    missing = [name for _, name in FILES if name not in have]
    if missing:  # all there, or no release
        sys.exit(f"missing packages: {', '.join(missing)}")
    release = call("POST", "/releases", {
        "tag_name": f"v{VERSION}",
        "ref": SHA,
        "name": f"v{VERSION}",
        "description": notes(),
        "assets": {"links": [{"name": label, "url": f"{API}/projects/{PROJECT}{pkg}/{name}", "link_type": "package"}
                             for label, name in FILES]},
    })
    print(f"released {release['tag_name']}: {release['_links']['self']}")


if __name__ == "__main__":
    main()
