#!/usr/bin/env python3
"""Write THIRD-PARTY-NOTICES.txt for a release package: this repo's license, the SDK's, and every SDK dependency's.

    python3 tools/ci/third_party_notices.py <sdk dir> <out file> [<Cargo.toml of the n1 workspace>]

The texts come from the SDK checkout the release was built from (tools/ci/sdk.sh), so they match what was compiled.
With the n1 workspace's manifest, the license texts of its Rust crates (from the cargo registry, so they must have been
fetched: build first) are added, grouped by identical text.
Over-inclusion is harmless; a missing notice is not, so a thirdparty/ directory with no license text that isn't in
NOTES or SKIP fails the build.
"""
from __future__ import annotations

import json
import re
import subprocess
import sys
from pathlib import Path

# Not in anything we ship.
SKIP = {
    "disasm": "the recompiler's PowerPC disassembler (GPL); linked only into the codegen tool",
    "catch2": "SDK tests only",
    "ffmpeg-overlay": "build lists for FFmpeg, covered by FFmpeg below",
}
# Vendored copies that carry no license text of their own.
NOTES = {
    "tiny-aes-c": "Public domain (The Unlicense). Upstream: https://github.com/kokke/tiny-AES-c",
    "disruptorplus": "MIT License. Upstream: https://github.com/lewissbaker/disruptorplus",
    "dxc": "Headers only. University of Illinois/NCSA Open Source License: "
    "https://github.com/microsoft/DirectXShaderCompiler/blob/main/LICENSE.TXT",
    "pe": "Headers only: PE image structure definitions from the Windows SDK.",
}
# Statically linked LGPL code: say where its exact source is, and that the rest is open for relinking.
LGPL = {"FFmpeg", "libmspack"}
# FFmpeg ships the GPL texts too; the SDK builds it without --enable-gpl (XMA/WMA/MP3 audio decoders only).
ONLY = {"FFmpeg": ["LICENSE.md", "COPYING.LGPLv2.1"]}

LICENSE_NAME = re.compile(r"^(licen[cs]e|copying|unlicense|notice)", re.IGNORECASE)
SOURCE = re.compile(r"\.(h|hpp|hh|c|cc|cpp)$")


def license_files(d: Path) -> list[Path]:
    if d.name in ONLY:
        return [d / n for n in ONLY[d.name] if (d / n).is_file()]
    top = sorted(p for p in d.iterdir() if p.is_file() and LICENSE_NAME.match(p.name))
    if top:
        return top
    nested = sorted(p for p in d.rglob("*") if p.is_file() and LICENSE_NAME.match(p.name) and len(p.relative_to(d).parts) <= 3)
    return nested


def comment_notice(d: Path) -> str | None:
    """The license comment of a header-only library: stb's trailing block, or the first comment naming a copyright."""
    for f in sorted(p for p in d.rglob("*") if p.is_file() and SOURCE.search(p.name)):
        text = f.read_text(errors="replace")
        i = text.find("This software is available under 2 licenses")
        if i >= 0:
            return text[i : text.find("*/", i)].strip()
        for m in re.finditer(r"/\*.*?\*/", text, re.DOTALL):
            block = m.group(0)
            if re.search(r"copyright", block, re.IGNORECASE) and re.search(r"licen[cs]e|permission", block, re.IGNORECASE):
                return block
    return None


def submodule(sdk: Path, name: str) -> tuple[str, str] | None:
    url = None
    gm = sdk / ".gitmodules"
    if gm.is_file():
        m = re.search(rf"path = thirdparty/{re.escape(name)}\s*\n\s*url = (\S+)", gm.read_text())
        url = m and m.group(1)
    try:
        sha = subprocess.run(["git", "-C", str(sdk), "ls-tree", "HEAD", f"thirdparty/{name}"],
                             capture_output=True, text=True, check=True).stdout.split()[2]
    except (subprocess.CalledProcessError, IndexError, FileNotFoundError):
        sha = None
    return (url, sha) if url and sha else None


def rust_notices(manifest: Path) -> str:
    """License texts of the crates.io dependencies in a Cargo workspace, one section per distinct text."""
    meta = json.loads(subprocess.run(
        ["cargo", "metadata", "--format-version", "1", "--locked", "--manifest-path", str(manifest)],
        capture_output=True, text=True, check=True).stdout)
    by_text: dict[str, list[str]] = {}
    nolicense = []
    for pkg in sorted(meta["packages"], key=lambda p: p["name"]):
        if not pkg["source"]:  # our own crates: covered by the Skullburglar license
            continue
        d = Path(pkg["manifest_path"]).parent
        files = sorted(p for p in d.iterdir() if p.is_file() and LICENSE_NAME.match(p.name))
        if not files:
            nolicense.append(f"{pkg['name']} {pkg['version']} ({pkg['license']})")
            continue
        text = "\n\n".join(f"--- {f.name} ---\n\n{f.read_text(errors='replace').strip()}" for f in files)
        by_text.setdefault(text, []).append(f"{pkg['name']} {pkg['version']}")
    out = []
    for text, crates in sorted(by_text.items(), key=lambda kv: kv[1][0]):
        out.append(section("Rust crates: " + ", ".join(crates), text))
    if nolicense:
        # Crates whose package has no license file still name their SPDX license (MIT/Apache-2.0 family: the
        # copyright line is the author's; the text is the standard one).
        out.append(section("Rust crates without a license file in their package", "\n".join(nolicense)))
    return "".join(out)


def section(title: str, body: str) -> str:
    return f"{'=' * 78}\n{title}\n{'=' * 78}\n\n{body.strip()}\n\n"


def main() -> int:
    if len(sys.argv) not in (3, 4):
        print(__doc__.strip().splitlines()[2].strip(), file=sys.stderr)
        return 2
    sdk, out = Path(sys.argv[1]), Path(sys.argv[2])
    repo = Path(__file__).resolve().parents[2]
    parts = [
        (
            "Skullburglar includes software written by others. Their licenses follow.\n"
            "Source: https://github.com/nlwstein/skullburglar and https://github.com/rexglue/rexglue-sdk\n\n"
        ),
        section("Skullburglar", (repo / "LICENSE").read_text()),
        section("ReXGlue SDK (includes code derived from Xenia)", (sdk / "LICENSE").read_text()),
    ]
    missing = []
    for d in sorted((sdk / "thirdparty").iterdir(), key=lambda p: p.name.lower()):
        if not d.is_dir() or d.name in SKIP:
            continue
        files = license_files(d)
        if files:
            body = "\n\n".join(f"--- {f.relative_to(d)} ---\n\n{f.read_text(errors='replace').strip()}" for f in files)
        elif d.name in NOTES:
            body = NOTES[d.name]
        elif notice := comment_notice(d):
            body = notice
        else:
            missing.append(d.name)
            continue
        if d.name in LGPL:
            sub = submodule(sdk, d.name)
            where = f"{sub[0]} at commit {sub[1]}" if sub else "the ReXGlue SDK's thirdparty/" + d.name
            body = (f"Statically linked into the runtime library. Its source: {where}. The rest of the program's "
                    "source is public too (above), so you can modify this library and rebuild or relink.\n\n" + body)
        parts.append(section(d.name, body))
    kenney = repo / "assets/prompts/LICENSE-kenney.txt"
    if kenney.is_file():
        parts.append(section("Kenney Input Prompts (button prompt icons)", kenney.read_text()))
    if len(sys.argv) == 4:
        parts.append(rust_notices(Path(sys.argv[3])))
    if missing:
        print(f"no license text found for thirdparty/{', thirdparty/'.join(missing)}: add it to NOTES or SKIP in "
              f"{Path(__file__).name}", file=sys.stderr)
        return 1
    out.write_text("".join(parts))
    print(f"{out}: {len(parts) - 1} notices, {out.stat().st_size // 1024} KiB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
