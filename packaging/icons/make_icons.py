"""QtTune's app icon, from one geometry: the Home page's tachometer,
simplified to read at launcher sizes, on the accent color's tile.

    python packaging/icons/make_icons.py [--qt <Qt kit, e.g. C:/Qt/6.12.0/msvc2022_64>]

Writes, from the repository root:
  packaging/icons/qttune.svg        the master: the tile with rounded corners
  packaging/icons/qttune-ios.svg    full bleed, square (iOS masks its own)
  app/android/res/...               the adaptive launcher icon as vector
                                    drawables (Android 8+; sharp at every density)
and, with --qt, renders qttune-ios.svg through the kit's qml runtime and
SVG plugin into app/ios/Assets.xcassets/AppIcon.appiconset/AppIcon.png
(1024 px, the alpha channel dropped: iOS wants it opaque).

The geometry lives on Android's 108-unit adaptive canvas: launchers show
its middle 72 units, and the face fits the 66-unit safe circle.
"""
import argparse
import math
import os
import struct
import subprocess
import zlib

BG, ELEVATED = "#14141c", "#2a2a3e"
TEXT, DANGER, SUCCESS = "#e8e8f0", "#f87171", "#4ade80"
TILE_FROM, TILE_TO = "#8a6dff", "#3b1fb8"   # Theme.primary (#6d4aff), lit from the top left

CX = CY = 54.0
R = 33.0                        # the face
MAX_RPM, REDLINE, NEEDLE_RPM = 8000, 6500, 4400

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))


def ang(v):
    """Degrees for v r/min: 0 = 3 o'clock, clockwise, as the Home gauge"""
    return 135 + v / MAX_RPM * 270


def pt(a, r):
    t = math.radians(a)
    return CX + r * math.cos(t), CY + r * math.sin(t)


def n(v):
    return f"{v:.3f}".rstrip("0").rstrip(".")


def arc(a0, a1, r):
    (x0, y0), (x1, y1) = pt(a0, r), pt(a1, r)
    large = 1 if (a1 - a0) % 360 > 180 else 0
    return f"M{n(x0)},{n(y0)} A{n(r)},{n(r)} 0 {large} 1 {n(x1)},{n(y1)}"


def circle(r):
    return f"M{n(CX - r)},{n(CY)} a{n(r)},{n(r)} 0 1,0 {n(2 * r)},0 a{n(r)},{n(r)} 0 1,0 {n(-2 * r)},0 Z"


# The face's light, from above
FACE = {"type": "radial", "cx": CX, "cy": CY - R * 0.35, "r": R * 1.25,
        "stops": [(0, ELEVATED), (1, BG)]}


def gauge():
    """The gauge as paths: dicts of d, fill or stroke (color or gradient),
    width, alpha and cap"""
    tick_outer = R * 0.80
    ring = R * 0.90
    out = [
        {"d": circle(R), "fill": FACE, "stroke": TEXT, "stroke_alpha": 0.14, "width": 0.8},
        {"d": arc(ang(REDLINE), ang(MAX_RPM), tick_outer - 3.2),
         "stroke": DANGER, "stroke_alpha": 0.35, "width": 5},
        {"d": arc(ang(0), ang(MAX_RPM), ring), "stroke": TEXT, "stroke_alpha": 0.10, "width": 3.4},
        {"d": arc(ang(0), ang(NEEDLE_RPM), ring), "stroke": SUCCESS, "width": 3.4, "cap": "round"},
    ]
    for k in range(9):          # a major tick every 1000 r/min, red past the redline
        v = k * 1000
        (x0, y0), (x1, y1) = pt(ang(v), tick_outer), pt(ang(v), tick_outer - 5.5)
        out.append({"d": f"M{n(x0)},{n(y0)} L{n(x1)},{n(y1)}",
                    "stroke": DANGER if v >= REDLINE else TEXT, "width": 2.2, "cap": "round"})
    # The needle, a tapered blade through the hub
    a = ang(NEEDLE_RPM)
    normal = math.radians(a + 90)

    def side(r, w):
        x, y = pt(a, r)
        dx, dy = w * math.cos(normal), w * math.sin(normal)
        return (x + dx, y + dy), (x - dx, y - dy)

    (t1, t2), (h1, h2), (p1, p2) = side(-R * 0.20, 1.4), side(0, 2.2), side(R * 0.80, 0.5)
    blade = [t1, h1, p1, p2, h2, t2]
    out.append({"d": "M" + " L".join(f"{n(x)},{n(y)}" for x, y in blade) + " Z", "fill": DANGER})
    out.append({"d": circle(3.6), "fill": ELEVATED, "stroke": DANGER, "width": 1.4})
    return out


def tile(rounded):
    rx = 16 if rounded else 0
    d = (f"M{18 + rx},18 H{90 - rx} A{rx},{rx} 0 0 1 90,{18 + rx} V{90 - rx} "
         f"A{rx},{rx} 0 0 1 {90 - rx},90 H{18 + rx} A{rx},{rx} 0 0 1 18,{90 - rx} "
         f"V{18 + rx} A{rx},{rx} 0 0 1 {18 + rx},18 Z") if rounded else "M0,0 H108 V108 H0 Z"
    return {"d": d, "fill": {"type": "linear", "x1": 18, "y1": 18, "x2": 90, "y2": 90,
                             "stops": [(0, TILE_FROM), (1, TILE_TO)]}}


# ---- SVG

def svg(paths, view):
    defs, body = [], []
    for i, p in enumerate(paths):
        attrs = [f'd="{p["d"]}"']
        fill = p.get("fill")
        if isinstance(fill, dict):
            gid = f"g{i}"
            stops = "".join(f'<stop offset="{o}" stop-color="{c}"/>' for o, c in fill["stops"])
            if fill["type"] == "radial":
                defs.append(f'<radialGradient id="{gid}" cx="{n(fill["cx"])}" cy="{n(fill["cy"])}" '
                            f'r="{n(fill["r"])}" gradientUnits="userSpaceOnUse">{stops}</radialGradient>')
            else:
                defs.append(f'<linearGradient id="{gid}" x1="{fill["x1"]}" y1="{fill["y1"]}" '
                            f'x2="{fill["x2"]}" y2="{fill["y2"]}" gradientUnits="userSpaceOnUse">'
                            f'{stops}</linearGradient>')
            attrs.append(f'fill="url(#{gid})"')
        else:
            attrs.append(f'fill="{fill or "none"}"')
        if "stroke" in p:
            attrs.append(f'stroke="{p["stroke"]}" stroke-width="{n(p["width"])}"')
            if "stroke_alpha" in p:
                attrs.append(f'stroke-opacity="{p["stroke_alpha"]}"')
            if "cap" in p:
                attrs.append(f'stroke-linecap="{p["cap"]}"')
        body.append(f'  <path {" ".join(attrs)}/>')
    return ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="' + view + '" width="1024" height="1024">\n'
            "  <!-- Generated by packaging/icons/make_icons.py -->\n"
            f'  <defs>{"".join(defs)}</defs>\n' + "\n".join(body) + "\n</svg>\n")


# ---- Android vector drawables

def argb(color):
    return "#FF" + color[1:].upper()


def vector(paths):
    body = []
    for p in paths:
        attrs = [f'android:pathData="{p["d"]}"']
        inner = ""
        fill = p.get("fill")
        if isinstance(fill, dict):
            items = "".join(f'\n                <item android:offset="{o}" android:color="{argb(c)}"/>'
                            for o, c in fill["stops"])
            if fill["type"] == "radial":
                g = (f'android:type="radial" android:centerX="{n(fill["cx"])}" '
                     f'android:centerY="{n(fill["cy"])}" android:gradientRadius="{n(fill["r"])}"')
            else:
                g = (f'android:type="linear" android:startX="{fill["x1"]}" android:startY="{fill["y1"]}" '
                     f'android:endX="{fill["x2"]}" android:endY="{fill["y2"]}"')
            inner = (f'\n        <aapt:attr name="android:fillColor">\n            <gradient {g}>'
                     f'{items}\n            </gradient>\n        </aapt:attr>\n    ')
        elif fill:
            attrs.append(f'android:fillColor="{argb(fill)}"')
        if "stroke" in p:
            attrs.append(f'android:strokeColor="{argb(p["stroke"])}"')
            attrs.append(f'android:strokeWidth="{n(p["width"])}"')
            if "stroke_alpha" in p:
                attrs.append(f'android:strokeAlpha="{p["stroke_alpha"]}"')
            if "cap" in p:
                attrs.append(f'android:strokeLineCap="{p["cap"]}"')
        sep = "\n        "
        body.append(f"    <path{sep}{sep.join(attrs)}>{inner}</path>" if inner
                    else f"    <path{sep}{sep.join(attrs)}/>")
    return ('<?xml version="1.0" encoding="utf-8"?>\n'
            "<!-- Generated by packaging/icons/make_icons.py -->\n"
            '<vector xmlns:android="http://schemas.android.com/apk/res/android"\n'
            '    xmlns:aapt="http://schemas.android.com/aapt"\n'
            '    android:width="108dp" android:height="108dp"\n'
            '    android:viewportWidth="108" android:viewportHeight="108">\n'
            + "\n".join(body) + "\n</vector>\n")


ADAPTIVE = """<?xml version="1.0" encoding="utf-8"?>
<!-- Generated by packaging/icons/make_icons.py -->
<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">
    <background android:drawable="@drawable/ic_launcher_background"/>
    <foreground android:drawable="@drawable/ic_launcher_foreground"/>
</adaptive-icon>
"""

IOS_CONTENTS = """{
  "images" : [
    {
      "filename" : "AppIcon.png",
      "idiom" : "universal",
      "platform" : "ios",
      "size" : "1024x1024"
    }
  ],
  "info" : {
    "author" : "xcode",
    "version" : 1
  }
}
"""

CATALOG_CONTENTS = """{
  "info" : {
    "author" : "xcode",
    "version" : 1
  }
}
"""

RENDER_QML = """import QtQuick
// Renders one SVG at its size, saves it and quits (make_icons.py)
Window {
    id: win
    width: %(size)d; height: %(size)d
    visible: true
    color: "black"
    Image { anchors.fill: parent; source: "file:///%(svg)s"; sourceSize: Qt.size(%(size)d, %(size)d) }
    Timer {
        interval: 500; running: true
        onTriggered: win.contentItem.grabToImage(r => { r.saveToFile("%(png)s"); Qt.quit() },
                                                 Qt.size(%(size)d, %(size)d))
    }
}
"""


def write(rel, text):
    path = os.path.join(ROOT, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)


def png_without_alpha(path):
    """Rewrites an 8-bit RGBA PNG as RGB (stdlib only)"""
    with open(path, "rb") as fh:
        data = fh.read()
    pos, idat, ihdr = 8, b"", None
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        chunk = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            ihdr = chunk
        elif kind == b"IDAT":
            idat += chunk
        pos += 12 + length
    w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", ihdr)
    if (depth, ctype) == (8, 2):
        return              # RGB already: Qt writes an opaque image without alpha
    if (depth, ctype, interlace) != (8, 6, 0):
        raise SystemExit(f"{path}: expected 8-bit RGBA, not interlaced")
    raw, bpp, stride = zlib.decompress(idat), 4, w * 4
    prev, rows, i = bytearray(stride), [], 0
    for _ in range(h):
        ftype, line = raw[i], bytearray(raw[i + 1:i + 1 + stride])
        i += 1 + stride
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if ftype == 1:
                line[x] = (line[x] + a) & 255
            elif ftype == 2:
                line[x] = (line[x] + b) & 255
            elif ftype == 3:
                line[x] = (line[x] + (a + b) // 2) & 255
            elif ftype == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(b"\x00" + bytes(v for k, v in enumerate(line) if k % 4 != 3))
        prev = line

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    out = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(b"".join(rows), 9)) + chunk(b"IEND", b""))
    with open(path, "wb") as fh:
        fh.write(out)


def render(qt, svg_rel, png_rel, size, scratch):
    svg_path = os.path.join(ROOT, svg_rel).replace("\\", "/")
    png_path = os.path.join(ROOT, png_rel).replace("\\", "/")
    os.makedirs(os.path.dirname(png_path), exist_ok=True)
    qml = os.path.join(scratch, "render.qml")
    with open(qml, "w", encoding="utf-8") as fh:
        fh.write(RENDER_QML % {"size": size, "svg": svg_path, "png": png_path})
    # Offscreen, on the software renderer: at the size asked, whatever the
    # screen's scaling, and no window opens
    env = dict(os.environ, PATH=os.path.join(qt, "bin") + os.pathsep + os.environ["PATH"],
               QT_QPA_PLATFORM="offscreen", QT_QUICK_BACKEND="software")
    subprocess.run([os.path.join(qt, "bin", "qml"), qml], check=True, env=env, timeout=60)
    png_without_alpha(png_path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qt", help="a desktop Qt kit, to render the iOS PNG")
    ap.add_argument("--scratch", default=os.environ.get("TEMP", "/tmp"))
    args = ap.parse_args()

    g = gauge()
    write("packaging/icons/qttune.svg", svg([tile(True)] + g, "18 18 72 72"))
    write("packaging/icons/qttune-ios.svg", svg([tile(False)] + g, "18 18 72 72"))
    write("app/android/res/drawable/ic_launcher_foreground.xml", vector(g))
    write("app/android/res/drawable/ic_launcher_background.xml", vector([tile(False)]))
    write("app/android/res/mipmap-anydpi-v26/ic_launcher.xml", ADAPTIVE)
    write("app/ios/Assets.xcassets/Contents.json", CATALOG_CONTENTS)
    write("app/ios/Assets.xcassets/AppIcon.appiconset/Contents.json", IOS_CONTENTS)
    if args.qt:
        render(args.qt, "packaging/icons/qttune-ios.svg",
               "app/ios/Assets.xcassets/AppIcon.appiconset/AppIcon.png", 1024, args.scratch)
    print("ok")


if __name__ == "__main__":
    main()
