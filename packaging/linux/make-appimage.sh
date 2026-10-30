#!/usr/bin/env bash
# File/layer: packaging/linux/make-appimage.sh | release packaging, Linux x86_64
# Testing: run by .github/workflows/ci.yml's linux-appimage job, and by
#          hand in WSL (docs/development.md, "Linux: the AppImage")
#
# Packs a built Release tree into an AppImage and checks it:
#   dist/havne-<version>-linux-x86_64.AppImage
#   dist/havne-<version>-linux-x86_64.AppImage.components.json, what it
#     bundles, for the release's SBOM (packaging/sbom.py, ADR-0035 §5)
# linuxdeploy and its Qt plugin (pinned releases, checked against their
# SHA-256) gather the app, the Qt libraries, plugins and QML modules it
# loads, and the desktop entry and icon. The checks:
#   - no QML debugging ships (qmltooling plugins: a Debug build's);
#   - the AppImage starts and is still running after SMOKE_SECONDS (10)
#     on the offscreen platform, which a missing Qt plugin or QML module
#     would stop at once.
# The AppImage runs on distributions whose glibc is at least the build
# machine's: build it on the oldest one to support (CI: Ubuntu 24.04).
# Two builds of one commit make the same bytes when both carry its time
# (ADR-0035 §4): rcc records each embedded file's modification time,
# which a checkout sets, unless QT_RCC_SOURCE_DATE_OVERRIDE is set when
# the app is built; the squashfs records times unless SOURCE_DATE_EPOCH
# is set when it's packed.
#
# Usage: packaging/linux/make-appimage.sh <version> [build dir] [out dir]
#   build dir  default build, configured with CMAKE_BUILD_TYPE=Release
#   out dir    default dist
# Environment:
#   QMAKE            the Qt kit's qmake (default: on PATH, or $QT_ROOT_DIR/bin)
#   LINUXDEPLOY_DIR  where the tools are kept (default ~/.cache/havne-linuxdeploy)
#   SMOKE_SECONDS    0 skips the smoke
#   SOURCE_DATE_EPOCH  the squashfs's times (default: git log -1 --format=%ct)
# Run from the repository root.

set -euo pipefail

die() { echo "make-appimage: $*" >&2; exit 1; }

[ $# -ge 1 ] && [ $# -le 3 ] || die "usage: make-appimage.sh <version> [build dir] [out dir]"
version=$1 build=${2:-build} out=${3:-dist}
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z]+)?$ ]] || die "$version is not X.Y.Z or X.Y.Z-<suffix>"
app=$build/app/havne
[ -x "$app" ] || die "no $app: build the app first (cmake --build $build)"
grep -qx "CMAKE_BUILD_TYPE:STRING=Release" "$build/CMakeCache.txt" \
    || die "$build isn't a Release tree (-DCMAKE_BUILD_TYPE=Release)"

if [ -z "${QMAKE:-}" ]; then
    QMAKE=$(command -v qmake6 || command -v qmake || true)
    [ -z "$QMAKE" ] && [ -n "${QT_ROOT_DIR:-}" ] && QMAKE=$QT_ROOT_DIR/bin/qmake
fi
[ -n "${QMAKE:-}" ] && [ -x "$QMAKE" ] || die "no qmake: set QMAKE to the Qt kit's bin/qmake"
export QMAKE

# The tools: pinned releases, checked before they run
tools=${LINUXDEPLOY_DIR:-$HOME/.cache/havne-linuxdeploy}
mkdir -p "$tools"
fetch() {
    local name=$1 url=$2 sum=$3
    if [ ! -f "$tools/$name" ] || ! echo "$sum  $tools/$name" | sha256sum -c --status; then
        curl -fsSL -o "$tools/$name.part" "$url" || die "couldn't download $url"
        echo "$sum  $tools/$name.part" | sha256sum -c --status \
            || { rm -f "$tools/$name.part"; die "$name's SHA-256 isn't the pinned one"; }
        mv "$tools/$name.part" "$tools/$name"
        chmod +x "$tools/$name"
    fi
}
fetch linuxdeploy-x86_64.AppImage \
    https://github.com/linuxdeploy/linuxdeploy/releases/download/1-alpha-20251107-1/linuxdeploy-x86_64.AppImage \
    c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d
fetch linuxdeploy-plugin-qt-x86_64.AppImage \
    https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/1-alpha-20250213-1/linuxdeploy-plugin-qt-x86_64.AppImage \
    15106be885c1c48a021198e7e1e9a48ce9d02a86dd0a1848f00bdbf3c1c92724
export PATH="$tools:$PATH"
# The tools are AppImages themselves: run them extracted, so the build
# machine needs no FUSE
export APPIMAGE_EXTRACT_AND_RUN=1
if [ -z "${SOURCE_DATE_EPOCH:-}" ]; then
    SOURCE_DATE_EPOCH=$(git log -1 --format=%ct) || die "no SOURCE_DATE_EPOCH and no git history"
fi
export SOURCE_DATE_EPOCH
[ -n "${QT_RCC_SOURCE_DATE_OVERRIDE:-}" ]     || echo "make-appimage: QT_RCC_SOURCE_DATE_OVERRIDE wasn't set: the app embeds its files' checkout times" >&2

# The AppDir: the app, its desktop entry and icon, then Qt
appdir=$build/AppDir
rm -rf "$appdir"
mkdir -p "$out"
name="havne-$version-linux-x86_64.AppImage"
# The QML the app loads, for the plugin's import scan; offscreen for
# the smoke below, xcb (X11 and XWayland) for everyone
export QML_SOURCES_PATHS=$PWD/app/qml
export EXTRA_PLATFORM_PLUGINS=libqoffscreen.so
export OUTPUT=$out/$name
linuxdeploy-x86_64.AppImage --appdir "$appdir" \
    --executable "$app" \
    --desktop-file packaging/linux/havne.desktop \
    --icon-file packaging/icons/app.svg --icon-filename havne \
    --plugin qt --output appimage > "$build/appimage.log" 2>&1 \
    || { tail -n 30 "$build/appimage.log"; die "linuxdeploy failed (log: $build/appimage.log)"; }
[ -f "$out/$name" ] || die "linuxdeploy wrote no $out/$name (log: $build/appimage.log)"

# ---- Check: no QML debugging
# -quit: find stops at the first, so grep -q never cuts a pipe short (a
# SIGPIPE'd find would fail the pipeline under pipefail and pass this)
if find "$appdir" -path '*plugins/qmltooling*' -print -quit | grep -q .; then
    die "the AppImage carries qmltooling plugins (a Debug build's QML debugger)"
fi
echo "QML debugging: none"

# ---- The component list for the release's SBOM (packaging/sbom.py),
# from the AppDir: what came from the Qt kit is Qt (its ICU apart), and
# every other library is the build machine's, named by its package
command -v dpkg > /dev/null || die "the component list needs dpkg: build on Ubuntu or Debian"
qt_libs=$("$QMAKE" -query QT_INSTALL_LIBS)
owner() {   # the package that installed a library; dpkg knows it by the
            # path the package used, /lib or /usr/lib, link or file
    local path real
    real=$(readlink -f "$1")
    for path in "$1" "${1#/usr}" "$real" "${real#/usr}"; do
        dpkg -S "$path" 2>/dev/null | sed -n "1s/: .*//p" | grep . && return 0
    done
    return 1
}
for lib in "$appdir"/usr/lib/*.so*; do
    base=$(basename "$lib")
    [ -e "$qt_libs/$base" ] && continue
    pkg=$(owner "/usr/lib/x86_64-linux-gnu/$base") \
        || die "$base isn't from the Qt kit or a system package"
    printf '%s\t%s\t%s\n' "${pkg%%:*}" "$(dpkg-query -W -f='${Version}' "$pkg")" "$base"
done > "$build/appimage-system-libs.tsv"
QT_VERSION=$("$QMAKE" -query QT_VERSION) APPDIR=$appdir NAME=$name \
SYSTEM_LIBS=$build/appimage-system-libs.tsv LINUXDEPLOY=1-alpha-20251107-1 \
python3 - > "$out/$name.components.json" <<'EOF'
import json, os, re
from collections import defaultdict
appdir, qt = os.environ["APPDIR"], os.environ["QT_VERSION"]
lib = os.path.join(appdir, "usr/lib")
system = defaultdict(lambda: {"files": []})
for line in open(os.environ["SYSTEM_LIBS"]):
    pkg, ver, base = line.rstrip("\n").split("\t")
    system[(pkg, ver)]["files"].append(base)
sys_files = {f for v in system.values() for f in v["files"]}
kit = sorted(f for f in os.listdir(lib) if ".so" in f and f not in sys_files)
icu = [f for f in kit if f.startswith("libicu")]
qt_files = [f for f in kit if f not in icu]
for sub in ("plugins", "qml"):
    root = os.path.join(appdir, "usr", sub)
    for d, _, files in os.walk(root):
        qt_files += [os.path.relpath(os.path.join(d, f), os.path.join(appdir, "usr"))
                     for f in files if f.endswith(".so")]
components = [{"name": "Qt", "version": qt, "supplier": "Organization: The Qt Company",
               "license": "LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only",
               "purl": f"pkg:generic/qt@{qt}", "files": qt_files},
              {"name": "AppImage type 2 runtime", "license": "MIT",
               "comment": "embedded by linuxdeploy " + os.environ["LINUXDEPLOY"]}]
if icu:
    major = sorted({m.group(1) for f in icu if (m := re.search(r"\.so\.(\d+)", f))})
    components.append({"name": "ICU", "version": ",".join(major), "files": icu,
                       "comment": "from the Qt kit"})
for (pkg, ver), v in sorted(system.items()):
    components.append({"name": pkg, "version": ver, "files": v["files"],
                       "purl": f"pkg:deb/ubuntu/{pkg}@{ver}?arch=amd64"})
print(json.dumps({"download": os.environ["NAME"], "components": components}, indent=1, sort_keys=True))
EOF
echo "components: $(grep -c '"name"' "$out/$name.components.json") listed"

# ---- Check: the AppImage starts and stays up
smoke=${SMOKE_SECONDS:-10}
if [ "$smoke" -gt 0 ]; then
    # A session of its own: the runtime starts the app as its child, and
    # the kill below takes both
    QT_QPA_PLATFORM=offscreen HAVNE_SETTINGS_DIR=$(mktemp -d) \
        setsid "$out/$name" --appimage-extract-and-run > "$build/smoke.log" 2>&1 &
    pid=$!
    sleep "$smoke"
    if ! kill -0 "$pid" 2>/dev/null; then
        wait "$pid" && code=0 || code=$?
        tail -n 20 "$build/smoke.log"
        die "the AppImage exited within $smoke s (code $code; log: $build/smoke.log)"
    fi
    kill -- -"$pid"
    wait "$pid" 2>/dev/null || true
    echo "smoke: the AppImage ran $smoke s"
fi
echo "$name $(du -m "$out/$name" | cut -f1) MB, sha256 $(sha256sum "$out/$name" | cut -d' ' -f1)"
