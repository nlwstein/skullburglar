#!/usr/bin/env python3
"""The public release archive: every release's player packages also live in the package registry of the GitLab.com
project skullburglar/releases (which doesn't count toward GitLab.com storage), so versions that leave the download
bucket stay downloadable. Used by publish_downloads.py.

    python3 tools/ci/release_archive.py check   # before tagging: GITLAB_COM_TOKEN is usable (required on the default branch)
    python3 tools/ci/release_archive.py release ENTRY.json [--dry-run]
        # make or refresh the archive's Releases-page entry for one version.json entry; --dry-run only prints the plan

Each release also gets an entry on the archive project's Releases page (tag v<version> on the archive repo, the notes,
and one link per package). Entries hold no files, so they cost no storage; an existing entry is updated, never deleted.

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

ARCHIVE_PROJECT = "https://gitlab.com/api/v4/projects/skullburglar%2Freleases"
ARCHIVE_API = ARCHIVE_PROJECT + "/packages/generic/skullburglar"
LABELS = (("windows", "Windows installer"), ("macos", "macOS"), ("AppImage", "Linux AppImage"),
          ("tar.xz", "Linux .tar.xz"), ("ios", "iPhone/iPad (.ipa)"), ("tvos", "Apple TV (.ipa)"),
          ("android", "Android (.apk)"))


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


def release_description(entry):
    """The Releases-page text for a version.json entry: its date and notes."""
    notes = "\n".join(f"- {n}" for n in entry.get("notes", [])) or "_No notes recorded for this version._"
    return f"Released {entry.get('date', 'unknown date')}.\n\n## What changed\n\n{notes}\n"


def release_links(entry):
    """One package link per download, pointing at the archive copy."""
    version = entry["version"]
    out = []
    for platform, d in entry.get("downloads", {}).items():
        label = next((text for key, text in LABELS if key in platform or key in d["file"]), platform)
        out.append({"name": f"{label}: {d['file']}", "url": archive_url(version, d["file"]), "link_type": "package"})
    return out


def release_plan(entry, existing_links=None):
    """What to do for this version: ("create", body) or ("update", body, missing links). existing_links is None when
    there's no entry yet, else the URLs it already links."""
    version = entry["version"]
    links = release_links(entry)
    desc = release_description(entry)
    if existing_links is None:
        body = {"tag_name": f"v{version}", "name": f"Skullburglar {version}", "description": desc,
                "assets": {"links": links}}
        if entry.get("date"):
            body["released_at"] = f"{entry['date']}T12:00:00Z"
        return ("create", body)
    return ("update", {"name": f"Skullburglar {version}", "description": desc},
            [link for link in links if link["url"] not in existing_links])


def _api(token, method, path, body=None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(ARCHIVE_PROJECT + path, data=data, method=method,
                                 headers={"PRIVATE-TOKEN": token, "Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.load(r)


def archive_release(token, entry, dry_run=False):
    """Create (or refresh) the archive's Releases-page entry for one version. Never deletes anything. Exits on failure."""
    tag = urllib.parse.quote(f"v{entry['version']}", safe="")
    try:
        existing = [link["url"] for link in _api(token, "GET", f"/releases/{tag}/assets/links")]
    except urllib.error.HTTPError as e:
        if e.code != 404:
            sys.exit(f"archive release lookup failed: HTTP {e.code}")
        existing = None
    plan = release_plan(entry, existing)
    print(f"archive release v{entry['version']}: {plan[0]}"
          + (f", {len(plan[2])} new links" if plan[0] == "update" else f", {len(plan[1]['assets']['links'])} links")
          + (" (dry run)" if dry_run else ""))
    if dry_run:
        return plan
    try:
        if plan[0] == "create":
            # The tag is created on the archive repo's default branch.
            plan[1]["ref"] = _api(token, "GET", "")["default_branch"]
            _api(token, "POST", "/releases", plan[1])
        else:
            _api(token, "PUT", f"/releases/{tag}", plan[1])
            for link in plan[2]:
                _api(token, "POST", f"/releases/{tag}/assets/links", link)
    except urllib.error.HTTPError as e:
        sys.exit(f"archive release v{entry['version']} failed: HTTP {e.code}")
    return plan


if __name__ == "__main__":
    if sys.argv[1:2] == ["release"] and len(sys.argv) in (3, 4) and sys.argv[3:] in ([], ["--dry-run"]):
        archive_release(archive_token(), json.load(open(sys.argv[2])), dry_run=bool(sys.argv[3:]))
        sys.exit(0)
    if sys.argv[1:] != ["check"]:
        sys.exit(__doc__)
    print("release archive: token ok" if archive_token() else "release archive: no GITLAB_COM_TOKEN, archive copy will be skipped")
