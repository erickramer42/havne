#!/usr/bin/env bash
# File/layer: packaging/linux/make-appimage.sh | release packaging, Linux x86_64 and aarch64
# Testing: run by .github/workflows/ci.yml's linux-appimage job, and by
#          hand in WSL (docs/development.md, "Linux: the AppImage")
#
# Packs a built Release tree into an AppImage and checks it:
#   dist/havne-<version>-linux-<arch>.AppImage, <arch> the build
#     machine's: x86_64 or aarch64
#   dist/havne-<version>-linux-<arch>.AppImage.components.json, what it
#     bundles, for the release's SBOM (packaging/sbom.py, ADR-0035 section 5)
# linuxdeploy and its Qt plugin (pinned releases, the build machine's
# architecture's, checked against their SHA-256) gather the app, the Qt libraries, plugins and QML modules it
# loads, and the desktop entry and icon; the notices go in
# usr/share/doc/havne/ (packaging/notices/, and the common licenses the
# bundled packages' copyright files name). The AppImage starts with
# type2-runtime's runtime from a pinned release, checked the same way:
# with no runtime named, linuxdeploy's appimagetool downloads the newest
# one at every pack. The checks:
#   - no QML debugging ships (qmltooling plugins: a Debug build's);
#   - the AppImage's runtime names the pinned commit;
#   - every notice is there (linuxdeploy's usr/share/doc/<package>/copyright
#     for each bundled package too), and the notice names the Qt and ICU
#     packed;
#   - the AppImage starts and is still running after SMOKE_SECONDS (10)
#     on the offscreen platform, which a missing Qt plugin or QML module
#     would stop at once.
# The AppImage runs on distributions whose glibc is at least the build
# machine's: build it on the oldest one to support (CI: Ubuntu 24.04,
# on x86_64 and on ARM runners).
# Two builds of one commit make the same bytes when both carry its time
# (ADR-0035 section 4): rcc records each embedded file's modification time,
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

# The build machine's architecture names the tools, the AppImage and
# the libraries' folder
arch=$(uname -m)
case $arch in
    x86_64)  multiarch=x86_64-linux-gnu debarch=amd64
             linuxdeploy_sum=c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d
             plugin_sum=15106be885c1c48a021198e7e1e9a48ce9d02a86dd0a1848f00bdbf3c1c92724
             runtime_sum=2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d ;;
    aarch64) multiarch=aarch64-linux-gnu debarch=arm64
             linuxdeploy_sum=620095110d693282b8ebeb244a95b5e911cf8f65f76c88b4b47d16ae6346fcff
             plugin_sum=bf1c24aff6d749b5cf423afad6f15abd4440f81dec1aab95706b25f6667cdcf1
             runtime_sum=00cbdfcf917cc6c0ff6d3347d59e0ca1f7f45a6df1a428a0d6d8a78664d87444 ;;
    *) die "no AppImage for $arch: x86_64 and aarch64 only" ;;
esac

# The tools: pinned releases, checked before they run
tools=${LINUXDEPLOY_DIR:-$HOME/.cache/havne-linuxdeploy}
mkdir -p "$tools"
# Downloads $2 to $1: curl with retries, then, for a GitHub release asset
# URL, the API through gh (the asset URLs redirect to a host that answers
# 504 for long stretches while the API path keeps working). The caller
# checks the SHA-256 either way.
download() {
    local dest=$1 url=$2 owner repo tag name
    curl -fsSL --retry 3 --retry-delay 5 --retry-all-errors -o "$dest" "$url" && return 0
    if command -v gh >/dev/null 2>&1 \
        && [[ $url =~ ^https://github\.com/([^/]+)/([^/]+)/releases/download/([^/]+)/([^/]+)$ ]]; then
        owner=${BASH_REMATCH[1]} repo=${BASH_REMATCH[2]} tag=${BASH_REMATCH[3]} name=${BASH_REMATCH[4]}
        echo "$0: curl failed for $url; trying gh release download" >&2
        gh release download "$tag" -R "$owner/$repo" -p "$name" -O "$dest" --clobber && return 0
    fi
    die "couldn't download $url"
}
fetch() {
    local name=$1 url=$2 sum=$3
    if [ ! -f "$tools/$name" ] || ! echo "$sum  $tools/$name" | sha256sum -c --status; then
        download "$tools/$name.part" "$url"
        echo "$sum  $tools/$name.part" | sha256sum -c --status \
            || { rm -f "$tools/$name.part"; die "$name's SHA-256 isn't the pinned one"; }
        mv "$tools/$name.part" "$tools/$name"
        chmod +x "$tools/$name"
    fi
}
fetch "linuxdeploy-$arch.AppImage" \
    "https://github.com/linuxdeploy/linuxdeploy/releases/download/1-alpha-20251107-1/linuxdeploy-$arch.AppImage" \
    "$linuxdeploy_sum"
fetch "linuxdeploy-plugin-qt-$arch.AppImage" \
    "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/1-alpha-20250213-1/linuxdeploy-plugin-qt-$arch.AppImage" \
    "$plugin_sum"
# The runtime: type2-runtime's release, whose commit the AppImage reports
# (--appimage-version). A new pin changes the release, both SHA-256s and
# the commit together, and THIRD_PARTY_NOTICES.txt's runtime section with
# the versions of what it links statically (its build scripts at that
# commit; docs/development.md, "Linux: the AppImage")
runtime_release=20251108 runtime_commit=dd6cebe
# The release attaches this runtime's source (runtime-sources.sh)
grep -qx "runtime_release=$runtime_release" packaging/linux/runtime-sources.sh \
    && grep -q "^runtime_full=$runtime_commit" packaging/linux/runtime-sources.sh \
    || die "packaging/linux/runtime-sources.sh doesn't fetch type2-runtime $runtime_release ($runtime_commit)'s source"
fetch "type2-runtime-$runtime_release-$arch" \
    "https://github.com/AppImage/type2-runtime/releases/download/$runtime_release/runtime-$arch" \
    "$runtime_sum"
export LDAI_RUNTIME_FILE=$tools/type2-runtime-$runtime_release-$arch
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
name="havne-$version-linux-$arch.AppImage"
# The QML the app loads, for the plugin's import scan; offscreen for
# the smoke below, xcb (X11 and XWayland) for everyone
export QML_SOURCES_PATHS=$PWD/app/qml
export EXTRA_PLATFORM_PLUGINS=libqoffscreen.so
export OUTPUT=$out/$name
# Two runs: the first fills the AppDir, so the notices below can name
# the libraries it took; the second packs it
"linuxdeploy-$arch.AppImage" --appdir "$appdir" \
    --executable "$app" \
    --desktop-file packaging/linux/havne.desktop \
    --icon-file packaging/icons/app.svg --icon-filename havne \
    --plugin qt > "$build/appimage.log" 2>&1 \
    || { tail -n 30 "$build/appimage.log"; die "linuxdeploy failed (log: $build/appimage.log)"; }

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
    pkg=$(owner "/usr/lib/$multiarch/$base") \
        || die "$base isn't from the Qt kit or a system package"
    printf '%s\t%s\t%s\n' "${pkg%%:*}" "$(dpkg-query -W -f='${Version}' "$pkg")" "$base"
done > "$build/appimage-system-libs.tsv"
QT_VERSION=$("$QMAKE" -query QT_VERSION) APPDIR=$appdir NAME=$name \
SYSTEM_LIBS=$build/appimage-system-libs.tsv LINUXDEPLOY=1-alpha-20251107-1 DEBARCH=$debarch \
RUNTIME_RELEASE=$runtime_release RUNTIME_COMMIT=$runtime_commit \
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
              {"name": "AppImage type 2 runtime", "version": os.environ["RUNTIME_RELEASE"],
               "supplier": "Organization: AppImage", "license": "MIT",
               "purl": "pkg:github/AppImage/type2-runtime@" + os.environ["RUNTIME_RELEASE"],
               "comment": "the AppImage's first part, commit " + os.environ["RUNTIME_COMMIT"]
                          + ", packed by linuxdeploy " + os.environ["LINUXDEPLOY"]}]
# Linked statically into the runtime: its build scripts at that commit
# pin libfuse and squashfuse, and take the rest from Alpine 3.21
for name, ver, lic in (("libfuse", "3.15.0", "LGPL-2.1-only"),
                       ("squashfuse", "0.5.2", "BSD-2-Clause"),
                       ("musl", "1.2.5", "MIT"),
                       ("mimalloc", "2.1.7", "MIT"),
                       ("zstd", "1.5.6", "BSD-3-Clause OR GPL-2.0-or-later"),
                       ("zlib", "1.3.1", "Zlib")):
    components.append({"name": name, "version": ver, "license": lic,
                       "comment": "linked statically into the AppImage runtime"})
if icu:
    major = sorted({m.group(1) for f in icu if (m := re.search(r"\.so\.(\d+)", f))})
    components.append({"name": "ICU", "version": ",".join(major), "files": icu,
                       "comment": "from the Qt kit"})
for (pkg, ver), v in sorted(system.items()):
    components.append({"name": pkg, "version": ver, "files": v["files"],
                       "purl": f"pkg:deb/ubuntu/{pkg}@{ver}?arch={os.environ['DEBARCH']}"})
print(json.dumps({"download": os.environ["NAME"], "components": components}, indent=1, sort_keys=True))
EOF
echo "components: $(grep -c '"name"' "$out/$name.components.json") listed"

# ---- The notices (LGPL-3.0 section 4 wants Qt's with each copy, and the
# other licenses their own): Havne's license, the third-party notice and
# the license texts it names, and the common licenses that the system
# packages' copyright files point to. linuxdeploy puts each bundled
# package's copyright file in usr/share/doc/<package>/ itself
doc=$appdir/usr/share/doc/havne
notice=packaging/notices/THIRD_PARTY_NOTICES.txt
mkdir -p "$doc/licenses" "$doc/common-licenses"
cp LICENSE "$doc/LICENSE.txt"
cp "$notice" "$doc/THIRD_PARTY_NOTICES.txt"
cp packaging/windows/licenses/LGPL-3.0.txt packaging/windows/licenses/GPL-3.0.txt \
   packaging/windows/licenses/Qt-third-party.txt \
   packaging/notices/licenses/ICU.txt "$doc/licenses/"
# The runtime's license, and those of what it links statically
runtime_licenses="AppImage-runtime.txt LGPL-2.1.txt musl.txt mimalloc.txt squashfuse.txt zstd.txt zlib.txt"
for f in $runtime_licenses; do cp "packaging/notices/licenses/$f" "$doc/licenses/"; done
packages=$(cut -f1 "$build/appimage-system-libs.tsv" | sort -u)
named() {   # the common licenses a copyright file points to
    grep -o '/usr/share/common-licenses/[A-Za-z0-9.+_-]*' "$1" | sed 's/\.*$//' | sort -u
}
for pkg in $packages; do
    shipped=$appdir/usr/share/doc/$pkg/copyright
    [ -f "$shipped" ] || continue   # the check below names it
    for lic in $(named "$shipped"); do
        [ -s "$lic" ] || die "$pkg's copyright names $lic, which isn't there"
        cp -L "$lic" "$doc/common-licenses/"
    done
done

# ---- Check: the notices are there and name what was packed
for f in LICENSE.txt THIRD_PARTY_NOTICES.txt licenses/LGPL-3.0.txt licenses/GPL-3.0.txt \
         licenses/Qt-third-party.txt licenses/ICU.txt \
         $(printf 'licenses/%s ' $runtime_licenses); do
    [ -s "$doc/$f" ] || die "the AppDir has no usr/share/doc/havne/$f"
done
for pkg in $packages; do
    shipped=$appdir/usr/share/doc/$pkg/copyright
    [ -s "$shipped" ] || die "the AppDir has no usr/share/doc/$pkg/copyright, $pkg's license"
    for lic in $(named "$shipped"); do
        [ -s "$doc/common-licenses/${lic##*/}" ] \
            || die "the AppDir has no usr/share/doc/havne/common-licenses/${lic##*/}, which $pkg's copyright names"
    done
done
qt_version=$("$QMAKE" -query QT_VERSION)
grep -qx "Qt $qt_version" "$notice" || die "$notice doesn't name Qt $qt_version: update it"
icu=$(find "$appdir/usr/lib" -maxdepth 1 -name 'libicuuc.so.*' -printf '%f\n' \
      | sed -n 's/^libicuuc\.so\.\([0-9]*\)$/\1/p' | head -n 1)
[ -n "$icu" ] || die "the AppDir has no libicuuc.so.<major>, which $notice names"
grep -q "^ICU $icu\." "$notice" || die "$notice doesn't name ICU $icu: update it and licenses/ICU.txt"
grep -q "type2-runtime release $runtime_release (commit $runtime_commit)" "$notice" \
    || die "$notice doesn't name type2-runtime $runtime_release ($runtime_commit): update its runtime section"
echo "notices: $(find "$doc" -type f | wc -l) files, $(echo "$packages" | wc -w) system packages' copyrights in usr/share/doc"

# ---- The AppImage, packed from the AppDir as it stands
"linuxdeploy-$arch.AppImage" --appdir "$appdir" --output appimage >> "$build/appimage.log" 2>&1 \
    || { tail -n 30 "$build/appimage.log"; die "linuxdeploy's packing failed (log: $build/appimage.log)"; }
[ -f "$out/$name" ] || die "linuxdeploy wrote no $out/$name (log: $build/appimage.log)"

# ---- Check: the runtime is the pinned one (a linuxdeploy that ignored
# LDAI_RUNTIME_FILE would have downloaded another); it prints its
# version and exits, with no FUSE
got=$("$out/$name" --appimage-version 2>&1 | tr -d '\r') || true
grep -q "type2-runtime/commit/$runtime_commit\b" <<< "$got" \
    || die "the AppImage's runtime isn't type2-runtime $runtime_commit: $got"
echo "runtime: type2-runtime $runtime_release ($runtime_commit)"

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
