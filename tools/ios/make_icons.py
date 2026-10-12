#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = ["cairosvg", "pillow"]
# ///
"""Draw the iOS and tvOS app icons from the skull in packaging/linux/skullburglar.svg.

    tools/ios/make_icons.py        # uv; cairosvg needs the cairo library (Homebrew: brew install cairo)

Writes tools/ios/Assets.ios.xcassets (one 1024 px icon; iOS rounds the corners) and tools/ios/Assets.tvos.xcassets
(the layered Apple TV icon, a background layer and the skull on its own layer for the focus parallax, plus the Top
Shelf banners). tools/ios/build.sh compiles them with actool. Rerun after changing the SVG and commit the output.
"""
import io
import json
import os
import re
import shutil
import sys

if sys.platform == "darwin":  # cairocffi finds Homebrew's cairo only through the fallback path
    os.environ.setdefault("DYLD_FALLBACK_LIBRARY_PATH", "/opt/homebrew/lib:/usr/local/lib")
import cairosvg  # noqa: E402
from PIL import Image  # noqa: E402

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
SVG = os.path.join(ROOT, "packaging", "linux", "skullburglar.svg")
OUT = os.path.join(ROOT, "tools", "ios")
INK = "#17130f"  # the SVG's background colour

# The skull: everything in the SVG but its rounded background square.
_svg = open(SVG).read()
SKULL = re.sub(r"<rect[^>]*/>", "", _svg[_svg.index(">") + 1:_svg.rindex("</svg>")]).strip()
SKULL_BOX = (44, 32, 168, 184)  # x, y, width, height of the skull and crowbar in the SVG's 256 units

BACKGROUND = f"""<defs><radialGradient id="g" cx="50%" cy="45%" r="75%">
<stop offset="0" stop-color="#2a2119"/><stop offset="1" stop-color="{INK}"/></radialGradient></defs>
<rect width="100%" height="100%" fill="url(#g)"/>"""


def skull(cx, cy, height):
    """The skull scaled to height, centred on (cx, cy)."""
    s = height / SKULL_BOX[3]
    x = cx - (SKULL_BOX[0] + SKULL_BOX[2] / 2) * s
    y = cy - (SKULL_BOX[1] + SKULL_BOX[3] / 2) * s
    return f'<g transform="translate({x:.2f} {y:.2f}) scale({s:.4f})">{SKULL}</g>'


def render(path, w, h, body, opaque):
    svg = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" viewBox="0 0 {w} {h}">'
           f"{body}</svg>")
    image = Image.open(io.BytesIO(cairosvg.svg2png(bytestring=svg.encode()))).convert("RGBA")
    # Images that fill their frame are stored without alpha, as the App Store asks for icons.
    (image.convert("RGB") if opaque else image).save(path, optimize=True)


def contents(path, data):
    os.makedirs(path, exist_ok=True)
    data.setdefault("info", {"author": "xcode", "version": 1})
    with open(os.path.join(path, "Contents.json"), "w") as f:
        json.dump(data, f, indent=2)
        f.write("\n")


def ios():
    cat = os.path.join(OUT, "Assets.ios.xcassets")
    shutil.rmtree(cat, ignore_errors=True)
    contents(cat, {})
    icon = os.path.join(cat, "AppIcon.appiconset")
    contents(icon, {"images": [{"idiom": "universal", "platform": "ios", "size": "1024x1024",
                                "filename": "AppIcon.png"}]})
    # Full bleed: iOS masks the corners itself.
    render(os.path.join(icon, "AppIcon.png"), 1024, 1024,
           f'<rect width="1024" height="1024" fill="{INK}"/>' + skull(512, 530, 760), opaque=True)


def tvos():
    cat = os.path.join(OUT, "Assets.tvos.xcassets")
    shutil.rmtree(cat, ignore_errors=True)
    contents(cat, {})
    brand = os.path.join(cat, "App Icon & Top Shelf Image.brandassets")
    contents(brand, {"assets": [
        {"idiom": "tv", "role": "primary-app-icon", "size": "400x240", "filename": "App Icon.imagestack"},
        {"idiom": "tv-marketing", "role": "primary-app-icon", "size": "1280x768",
         "filename": "App Icon - App Store.imagestack"},
        {"idiom": "tv", "role": "top-shelf-image", "size": "1920x720", "filename": "Top Shelf Image.imageset"},
        {"idiom": "tv", "role": "top-shelf-image-wide", "size": "2320x720",
         "filename": "Top Shelf Image Wide.imageset"},
    ]})

    # Front to back; the back layer is opaque, the skull's layer transparent around it.
    layers = {"Front": lambda w, h: skull(w / 2, h / 2, h * 0.72), "Back": lambda w, h: BACKGROUND}

    def stack(name, w, h, scales, idiom):
        path = os.path.join(brand, name)
        contents(path, {"layers": [{"filename": f"{layer}.imagestacklayer"} for layer in layers]})
        for layer, draw in layers.items():
            lp = os.path.join(path, f"{layer}.imagestacklayer")
            contents(lp, {})
            ip = os.path.join(lp, "Content.imageset")
            os.makedirs(ip, exist_ok=True)
            images = []
            for s in scales:
                file = f"Content@{s}x.png"
                render(os.path.join(ip, file), w * s, h * s, draw(w * s, h * s), opaque=layer == "Back")
                images.append({"idiom": idiom, "scale": f"{s}x", "filename": file})
            contents(ip, {"images": images})

    stack("App Icon.imagestack", 400, 240, (1, 2), "tv")
    stack("App Icon - App Store.imagestack", 1280, 768, (1,), "tv")

    def shelf(name, w, h):
        path = os.path.join(brand, name)
        os.makedirs(path, exist_ok=True)
        images = []
        for s in (1, 2):
            file = f"TopShelf@{s}x.png"
            render(os.path.join(path, file), w * s, h * s,
                   BACKGROUND + skull(w * s / 2, h * s / 2, h * s * 0.62), opaque=True)
            images.append({"idiom": "tv", "scale": f"{s}x", "filename": file})
        contents(path, {"images": images})

    shelf("Top Shelf Image.imageset", 1920, 720)
    shelf("Top Shelf Image Wide.imageset", 2320, 720)


if __name__ == "__main__":
    ios()
    tvos()
    print("wrote tools/ios/Assets.ios.xcassets and tools/ios/Assets.tvos.xcassets")
