#!/usr/bin/env python3
"""The public release archive: every release's player packages also live in the package registry of the GitLab.com
project skullburglar/releases (which doesn't count toward GitLab.com storage), so versions that leave the download
bucket stay downloadable. Used by publish_downloads.py.

    python3 tools/ci/release_archive.py check   # before tagging: GITLAB_COM_TOKEN is usable (required on the default branch)

GITLAB_COM_TOKEN: an api-scope token of the archive project's account (a masked, hidden, protected CI variable). It is
never printed.
"""
import hashlib
import json
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

ARCHIVE_API = "https://gitlab.com/api/v4/projects/skullburglar%2Freleases/packages/generic/skullburglar"


def archive_url(version, name):
    return f"{ARCHIVE_API}/{urllib.parse.quote(version)}/{urllib.parse.quote(name)}"


def archive_token(environ=None):
    """GITLAB_COM_TOKEN, checked against GitLab.com; None when unset off the default branch. Exits (without the
    token in the message) when it's missing on the default branch or GitLab.com refuses it."""
    env = os.environ if environ is None else environ
    token = env.get("GITLAB_COM_TOKEN", "").strip()
    on_default_branch = bool(env.get("CI")) and env.get("CI_COMMIT_BRANCH") == env.get("CI_DEFAULT_BRANCH")
    if not token:
        if on_default_branch:
            sys.exit("GITLAB_COM_TOKEN is not set: refusing to publish a release the archive won't have")
        return None
    req = urllib.request.Request(ARCHIVE_API.split("/packages/")[0], headers={"PRIVATE-TOKEN": token})
    try:
        with urllib.request.urlopen(req) as r:
            perms = json.load(r).get("permissions") or {}
    except urllib.error.HTTPError as e:
        sys.exit(f"GITLAB_COM_TOKEN was refused by GitLab.com (HTTP {e.code}): renew it")
    level = max((p or {}).get("access_level", 0) for p in perms.values()) if perms else 0
    if level < 30:
        sys.exit("GITLAB_COM_TOKEN can't upload packages to the archive project (needs Developer or higher)")
    return token


def archive_put(token, version, path, name, sha):
    """Copy one package to the archive, then check anyone can download it with the same sha256. Exits on failure."""
    url = archive_url(version, name)
    with open(path, "rb") as f:
        req = urllib.request.Request(url, data=f, method="PUT",
                                     headers={"PRIVATE-TOKEN": token, "Content-Length": str(os.path.getsize(path))})
        try:
            urllib.request.urlopen(req).close()
        except urllib.error.HTTPError as e:
            sys.exit(f"archive upload of {name} failed: HTTP {e.code}")
    for wait in (0, 5, 15, 30, 60):  # a fresh upload can 404 for a few seconds
        time.sleep(wait)
        digest = hashlib.sha256()
        try:
            with urllib.request.urlopen(url) as r:
                while chunk := r.read(1 << 20):
                    digest.update(chunk)
        except urllib.error.HTTPError as e:
            if e.code == 404:
                continue
            sys.exit(f"archive copy of {name} isn't downloadable: HTTP {e.code}")
        if digest.hexdigest() != sha:
            sys.exit(f"archive copy of {name} doesn't match: sha256 differs")
        return
    sys.exit(f"archive copy of {name} still isn't downloadable after upload")


def archive_has(version, name):
    """Whether anyone can download this package from the archive (one byte, no token)."""
    req = urllib.request.Request(archive_url(version, name), headers={"Range": "bytes=0-0"})
    try:
        with urllib.request.urlopen(req) as r:
            return r.status in (200, 206)
    except urllib.error.HTTPError:
        return False


def archive_entry(v):
    """Point a version that left the bucket at its archive copies, if every package is there."""
    if v.get("archived") or not v.get("downloads"):
        return
    if all(archive_has(v["version"], d["file"]) for d in v["downloads"].values()):
        for d in v["downloads"].values():
            d["url"] = archive_url(v["version"], d["file"])
        v["archived"] = True
        print(f"archived: {v['version']}")


if __name__ == "__main__":
    if sys.argv[1:] != ["check"]:
        sys.exit(__doc__)
    print("release archive: token ok" if archive_token() else "release archive: no GITLAB_COM_TOKEN, archive copy will be skipped")
