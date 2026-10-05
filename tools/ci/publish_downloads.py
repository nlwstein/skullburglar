#!/usr/bin/env python3
"""Publish a release's player packages to the download site's bucket and add it to the releases file there.

    VERSION=0.4.0 LAST=0.3.2 python3 tools/ci/publish_downloads.py [--notes-file notes.json]

Run by the release job after gitlab_release.py, or by hand to add or redo a release (it replaces that version's entry).
The bucket is a Cloudflare R2 bucket behind the private download site, which serves the packages to signed-in players
and the releases file (version.json) to anyone; the launcher reads that file (src/setup/update_check.h).

The packages come from this project's package registry (skullburglar/<VERSION>). Only the newest KEEP versions stay
downloadable: older packages are deleted from the bucket, but every version stays in the file's history.

version.json:
    {"latest": "0.4.0", "page": "https://<site>/", "updated": "...",
     "versions": [{"version": "0.4.0", "date": "2026-10-05", "notes": ["..."], "available": true,
                   "downloads": {"windows": {"file": "...", "size": 123, "sha256": "...", "url": "/dl/0.4.0/..."}}}]}

Release notes are the feat/fix/perf commits since LAST, newest first. A commit's `Release-note:` trailer is its note
(written for players); `Release-note: none` leaves it out, as do commits scoped `ci` without one. Otherwise the note is
the subject without its type. --notes-file (a JSON list of strings) replaces them.

Env: VERSION, LAST (optional), DOWNLOADS_BUCKET, DOWNLOADS_PAGE (the site's address), CF_API_TOKEN (with R2 edit),
CF_ACCOUNT_ID; the registry is read with CI_JOB_TOKEN in CI, else GITLAB_TOKEN; CI_API_V4_URL and CI_PROJECT_ID.
"""
import argparse
import datetime
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
import urllib.error
import urllib.parse
import urllib.request

import boto3
from botocore.config import Config

VERSION = os.environ["VERSION"]
LAST = os.environ.get("LAST", "")
BUCKET = os.environ["DOWNLOADS_BUCKET"]
PAGE = os.environ["DOWNLOADS_PAGE"]
KEEP = 3

# Player packages (tools/ci/package.sh names); debug symbols stay in the registry.
PACKAGES = {
    "windows": f"skullburglar-{VERSION}-windows-x86_64-setup.exe",
    "macos": f"skullburglar-{VERSION}-macos-arm64.dmg",
    "linux_appimage": f"skullburglar-{VERSION}-linux-x86_64.AppImage",
    "linux_tar": f"skullburglar-{VERSION}-linux-x86_64.tar.xz",
}
TYPES = ("feat", "fix", "perf")


def registry_get(name, dest):
    api, project = os.environ["CI_API_V4_URL"], os.environ["CI_PROJECT_ID"]
    url = f"{api}/projects/{project}/packages/generic/skullburglar/{urllib.parse.quote(VERSION)}/{name}"
    auth = ({"JOB-TOKEN": os.environ["CI_JOB_TOKEN"]} if os.environ.get("CI_JOB_TOKEN")
            else {"PRIVATE-TOKEN": os.environ["GITLAB_TOKEN"]})
    digest, size = hashlib.sha256(), 0
    with urllib.request.urlopen(urllib.request.Request(url, headers=auth)) as r, open(dest, "wb") as f:
        while chunk := r.read(1 << 20):
            digest.update(chunk)
            size += len(chunk)
            f.write(chunk)
    return size, digest.hexdigest()


def r2():
    # R2's S3 credentials for an API token: its id, and the SHA-256 of its value.
    token, account = os.environ["CF_API_TOKEN"], os.environ["CF_ACCOUNT_ID"]
    for path in ("/user/tokens/verify", f"/accounts/{account}/tokens/verify"):
        req = urllib.request.Request(f"https://api.cloudflare.com/client/v4{path}",
                                     headers={"Authorization": f"Bearer {token}"})
        try:
            with urllib.request.urlopen(req) as r:
                token_id = json.load(r)["result"]["id"]
                break
        except urllib.error.HTTPError:
            continue
    else:
        sys.exit("CF_API_TOKEN doesn't verify")
    return boto3.client("s3", endpoint_url=f"https://{account}.r2.cloudflarestorage.com", region_name="auto",
                        aws_access_key_id=token_id, aws_secret_access_key=hashlib.sha256(token.encode()).hexdigest(),
                        config=Config(retries={"max_attempts": 5}))


def git(*args):
    return subprocess.run(["git", *args], capture_output=True, text=True, check=True).stdout


def release_ref():
    # The tag once it exists (by hand, later), else this commit (in the release job, before the tag is fetched).
    try:
        return git("rev-parse", "--verify", "-q", f"v{VERSION}^{{commit}}").strip()
    except subprocess.CalledProcessError:
        return os.environ.get("CI_COMMIT_SHA", "HEAD")


def notes(ref):
    rng = f"v{LAST}..{ref}" if LAST else ref
    out = []
    for entry in git("log", "--no-merges", "--format=%s%x1f%(trailers:key=Release-note,valueonly,unfold)%x1e",
                     rng).split("\x1e"):
        if not entry.strip():
            continue
        subject, trailer = (entry.strip("\n").split("\x1f") + [""])[:2]
        m = re.match(r"(\w+)(?:\(([^)]*)\))?!?:\s*(.*)", subject)
        if not m or m.group(1) not in TYPES:
            continue
        trailers = [t.strip() for t in trailer.splitlines() if t.strip()]  # one note per Release-note: trailer
        if [t.lower() for t in trailers] == ["none"] or (not trailers and m.group(2) == "ci"):
            continue
        for text in trailers or [m.group(3)]:
            out.append(text[0].upper() + text[1:])
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--notes-file")
    args = ap.parse_args()
    s3 = r2()
    ref = release_ref()

    downloads = {}
    with tempfile.TemporaryDirectory() as tmp:
        for platform, name in PACKAGES.items():
            path = os.path.join(tmp, name)
            size, sha = registry_get(name, path)
            key = f"releases/{VERSION}/{name}"
            s3.upload_file(path, BUCKET, key, ExtraArgs={"ContentType": "application/octet-stream"})
            downloads[platform] = {"file": name, "size": size, "sha256": sha, "url": f"/dl/{VERSION}/{name}"}
            print(f"uploaded {key} ({size / 1e6:.1f} MB)")

    try:
        doc = json.load(s3.get_object(Bucket=BUCKET, Key="version.json")["Body"])
    except s3.exceptions.NoSuchKey:
        doc = {"versions": []}
    entry = {
        "version": VERSION,
        "date": git("show", "-s", "--format=%cs", ref).strip(),
        "notes": json.load(open(args.notes_file)) if args.notes_file else notes(ref),
        "available": True,
        "downloads": downloads,
    }
    versions = [v for v in doc["versions"] if v["version"] != VERSION] + [entry]
    key = lambda v: [int(n) for n in re.findall(r"\d+", v["version"].split("-")[0])]
    versions.sort(key=key, reverse=True)
    for i, v in enumerate(versions):
        if i >= KEEP and v.get("available"):
            for d in v["downloads"].values():
                s3.delete_object(Bucket=BUCKET, Key=f"releases/{v['version']}/{d['file']}")
            v["available"] = False
            print(f"no longer downloadable: {v['version']}")
    doc.update({
        "latest": versions[0]["version"],
        "page": PAGE,
        "updated": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "versions": versions,
    })
    s3.put_object(Bucket=BUCKET, Key="version.json", Body=json.dumps(doc, indent=1).encode(),
                  ContentType="application/json")
    print(f"version.json: latest {doc['latest']}, {len(versions)} versions, notes: {len(entry['notes'])}")


if __name__ == "__main__":
    main()
