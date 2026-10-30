# File/layer: packaging/sbom.py | release packaging, Python 3.8+, no packages
# Testing: UNIT, its own self-test (ctest SbomTest.SelfTest); CI's
#          linux-packages validates its self-test document with
#          pyspdxtools, and release.yml's publish job the release's
#
# The release's SBOM (ADR-0035 §5): one SPDX 2.3 JSON document naming
# each download, by file name and SHA-256, and what it contains. Each
# build job writes a component list beside its download,
# <download>.components.json, from what it actually packed:
#
#   {"download": "havne-0.3.4-windows-x64.zip",
#    "purpose": "APPLICATION",            # SPDX primaryPackagePurpose
#    "components": [{"name": "Qt", "version": "6.12.0",
#                    "supplier": "Organization: The Qt Company",
#                    "license": "LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only",
#                    "purl": "pkg:generic/qt@6.12.0",
#                    "files": ["Qt6Core.dll", ...],
#                    "comment": "anything else worth saying"}],
#    "depends": [{"name": "libc6", "version": ">= 2.39",
#                 "purl": "pkg:deb/ubuntu/libc6?arch=amd64"}]}
#
# Only name is required in a component or dependency; what's unknown is
# NOASSERTION, never guessed. Havne itself is added to every download.
# The output is the same bytes for the same inputs: sorted, and created
# at SOURCE_DATE_EPOCH (the commit's time) when it's set.
#
#   python3 packaging/sbom.py build --version X.Y.Z --tag vX.Y.Z --out <file> <download>...
#   python3 packaging/sbom.py apk --apk <file> --qt 6.12.0 --ndk <version> --gradle <deps.txt>
#       (an APK's component list, from the APK and Gradle's resolved tree)
#   python3 packaging/sbom.py selftest
#
# Exit 0 on success, 1 on a failed self-test, 2 on an error (a download
# or its component list missing, a list that names another file).

import argparse
import hashlib
import json
import os
import re
import sys
import tempfile
import time
from pathlib import Path

REPO = "erickramer42/havne"
PURPOSES = {"APPLICATION", "FRAMEWORK", "LIBRARY", "ARCHIVE", "INSTALL", "FILE", "OTHER"}
NOASSERTION = "NOASSERTION"


class SbomError(Exception):
    pass


def spdx_id(*parts):
    """An SPDX element ID from parts: letters, digits, '.' and '-' only."""
    text = "-".join(parts)
    return "SPDXRef-" + re.sub(r"[^A-Za-z0-9.-]+", "-", text).strip("-")


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def purl_ref(purl):
    return [{"referenceCategory": "PACKAGE-MANAGER", "referenceType": "purl",
             "referenceLocator": purl}] if purl else []


def package(ident, name, version, supplier, license_declared, purl=None,
            purpose=None, location=NOASSERTION, comment=None, checksum=None):
    p = {
        "SPDXID": ident,
        "name": name,
        "versionInfo": version or NOASSERTION,
        "supplier": supplier or NOASSERTION,
        "downloadLocation": location,
        "filesAnalyzed": False,
        "licenseConcluded": NOASSERTION,
        "licenseDeclared": license_declared or NOASSERTION,
        "copyrightText": NOASSERTION,
    }
    if purl:
        p["externalRefs"] = purl_ref(purl)
    if purpose:
        p["primaryPackagePurpose"] = purpose
    if comment:
        p["comment"] = comment
    if checksum:
        p["checksums"] = [{"algorithm": "SHA256", "checksumValue": checksum}]
    return p


def load_components(download):
    listing = Path(str(download) + ".components.json")
    if not download.is_file():
        raise SbomError(f"no download {download}")
    if not listing.is_file():
        raise SbomError(f"no component list {listing}")
    try:
        data = json.loads(listing.read_text(encoding="utf-8"))
    except ValueError as e:
        raise SbomError(f"{listing}: {e}") from e
    if data.get("download") != download.name:
        raise SbomError(f"{listing} describes {data.get('download')!r}, not {download.name}")
    if data.get("purpose", "APPLICATION") not in PURPOSES:
        raise SbomError(f"{listing}: purpose {data.get('purpose')!r}")
    for entry in data.get("components", []) + data.get("depends", []):
        if not entry.get("name"):
            raise SbomError(f"{listing}: an entry without a name")
    return data


def build(version, tag, downloads, created=None):
    """The SPDX document for the downloads, as a dict."""
    if created is None:
        epoch = os.environ.get("SOURCE_DATE_EPOCH")
        created = int(epoch) if epoch else int(time.time())
    release = f"https://github.com/{REPO}/releases/download/{tag}"
    doc = {
        "spdxVersion": "SPDX-2.3",
        "dataLicense": "CC0-1.0",
        "SPDXID": "SPDXRef-DOCUMENT",
        "name": f"havne-{version}",
        "documentNamespace": f"{release}/havne-{version}.spdx.json",
        "creationInfo": {
            "created": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(created)),
            "creators": ["Tool: havne-packaging-sbom.py", "Organization: Havne"],
        },
        "packages": [],
        "relationships": [],
    }
    havne = spdx_id("havne", version)
    doc["packages"].append(package(
        havne, "Havne", version, "Organization: Havne", "MIT",
        purl=f"pkg:github/{REPO}@{tag}",
        location=f"https://github.com/{REPO}/archive/refs/tags/{tag}.tar.gz"))
    seen = {havne}

    for path in sorted(downloads, key=lambda p: p.name):
        data = load_components(path)
        dl = spdx_id("download", path.name)
        if dl in seen:
            raise SbomError(f"{path.name} listed twice")
        seen.add(dl)
        doc["packages"].append(package(
            dl, path.name, version, "Organization: Havne", NOASSERTION,
            purpose=data.get("purpose", "APPLICATION"),
            location=f"{release}/{path.name}", checksum=sha256(path)))
        rel = doc["relationships"]
        rel.append({"spdxElementId": "SPDXRef-DOCUMENT", "relationshipType": "DESCRIBES",
                    "relatedSpdxElement": dl})
        rel.append({"spdxElementId": dl, "relationshipType": "CONTAINS",
                    "relatedSpdxElement": havne})
        for kind, entries in (("CONTAINS", data.get("components", [])),
                              ("DEPENDS_ON", data.get("depends", []))):
            for c in sorted(entries, key=lambda c: (c["name"], c.get("version", ""))):
                ident = spdx_id(path.name, c["name"], c.get("version", ""))
                if ident in seen:
                    raise SbomError(f"{path.name}: {c['name']} {c.get('version', '')} listed twice")
                seen.add(ident)
                files = sorted(c.get("files", []))
                notes = [c["comment"]] if c.get("comment") else []
                if files:
                    notes.append("Files: " + ", ".join(files))
                doc["packages"].append(package(
                    ident, c["name"], c.get("version"), c.get("supplier"),
                    c.get("license"), purl=c.get("purl"),
                    comment="; ".join(notes) or None))
                rel.append({"spdxElementId": dl, "relationshipType": kind,
                            "relatedSpdxElement": ident})
    return doc


GRADLE_DEP = re.compile(r"^[|\\+ ]*[+\\]--- (\S+)(?: -> (\S+))?(?: \(([*cn])\))?$")


def gradle_components(text):
    """The libraries in a Gradle `dependencies` tree (one configuration),
    each at the version Gradle resolved: constraints (c), unresolved (n)
    and BOMs, which hold no code, are left out."""
    found = set()
    for line in text.splitlines():
        m = GRADLE_DEP.match(line.rstrip())
        if not m or m.group(3) in ("c", "n"):
            continue
        parts = m.group(1).split(":")
        if len(parts) < 2:
            continue
        version = m.group(2) or (parts[2] if len(parts) > 2 else None)
        if parts[1].endswith("-bom") or not version:
            continue
        found.add((parts[0], parts[1], version))
    return [{"name": f"{g}:{n}", "version": v, "purl": f"pkg:maven/{g}/{n}@{v}"}
            for g, n, v in sorted(found)]


def apk_components(apk, qt, ndk, gradle_text, app_lib):
    """The component list of an APK: its native libraries by origin (Qt,
    the NDK's libc++, the app's own) and the Java libraries Gradle
    resolved. A native library from anywhere else is an error."""
    import zipfile
    qt_files = []
    with zipfile.ZipFile(apk) as z:
        for name in sorted(z.namelist()):
            if not (name.startswith("lib/") and name.endswith(".so")):
                continue
            base = name.rsplit("/", 1)[1]
            if re.match(r"lib(Qt6|plugins_|qml_)", base):
                qt_files.append(name)
            elif base not in ("libc++_shared.so", app_lib):
                raise SbomError(f"{name} in {apk.name} belongs to no component")
    components = [
        {"name": "Qt", "version": qt, "supplier": "Organization: The Qt Company",
         "license": "LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only",
         "purl": f"pkg:generic/qt@{qt}", "files": qt_files,
         "comment": "with Qt's Java classes (Qt6Android*.jar) in classes.dex"},
        {"name": "LLVM libc++ (Android NDK)", "version": ndk,
         "supplier": "Organization: Google", "license": "Apache-2.0 WITH LLVM-exception",
         "files": [f for f in ("lib/arm64-v8a/libc++_shared.so",)]},
    ]
    return {"download": apk.name, "components": components + gradle_components(gradle_text)}


def write(doc, out):
    Path(out).write_text(json.dumps(doc, indent=2, sort_keys=True) + "\n",
                         encoding="utf-8", newline="\n")


def selftest():
    failures = []

    def check(cond, what):
        if not cond:
            failures.append(what)

    with tempfile.TemporaryDirectory() as tmp:
        d = Path(tmp)
        zip_ = d / "havne-1.2.3-windows-x64.zip"
        deb = d / "havne-core_1.2.3_amd64.deb"
        zip_.write_bytes(b"zip")
        deb.write_bytes(b"deb")
        (d / (zip_.name + ".components.json")).write_text(json.dumps({
            "download": zip_.name,
            "components": [
                {"name": "Qt", "version": "6.12.0", "supplier": "Organization: The Qt Company",
                 "license": "LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only",
                 "purl": "pkg:generic/qt@6.12.0", "files": ["Qt6Quick.dll", "Qt6Core.dll"]},
                {"name": "Microsoft Visual C++ runtime", "version": "14.44.35211.0",
                 "comment": "app-local"}]}))
        (d / (deb.name + ".components.json")).write_text(json.dumps({
            "download": deb.name, "purpose": "INSTALL",
            "depends": [{"name": "libc6", "version": ">= 2.39",
                         "purl": "pkg:deb/ubuntu/libc6?arch=amd64"}]}))

        doc = build("1.2.3", "v1.2.3", [zip_, deb], created=0)
        ids = [p["SPDXID"] for p in doc["packages"]]
        check(len(ids) == len(set(ids)), "element IDs are unique")
        check(all(re.fullmatch(r"SPDXRef-[A-Za-z0-9.-]+", i) for i in ids), "element IDs are SPDX's form")
        check(doc["creationInfo"]["created"] == "1970-01-01T00:00:00Z", "created is the given time")
        by_name = {p["name"]: p for p in doc["packages"]}
        check(by_name[zip_.name]["checksums"][0]["checksumValue"] == hashlib.sha256(b"zip").hexdigest(),
              "a download carries its SHA-256")
        check(by_name[deb.name]["primaryPackagePurpose"] == "INSTALL", "the purpose is kept")
        check(by_name["Qt"]["comment"] == "Files: Qt6Core.dll, Qt6Quick.dll", "files are listed, sorted")
        check(by_name["Microsoft Visual C++ runtime"]["licenseDeclared"] == NOASSERTION,
              "an unknown license is NOASSERTION")
        check(by_name["Microsoft Visual C++ runtime"]["comment"] == "app-local", "a comment is kept")
        rels = {(r["spdxElementId"], r["relationshipType"], r["relatedSpdxElement"])
                for r in doc["relationships"]}
        for dl in (zip_, deb):
            i = spdx_id("download", dl.name)
            check(("SPDXRef-DOCUMENT", "DESCRIBES", i) in rels, f"the document describes {dl.name}")
            check((i, "CONTAINS", spdx_id("havne", "1.2.3")) in rels, f"{dl.name} contains Havne")
        check((spdx_id("download", deb.name), "DEPENDS_ON", spdx_id(deb.name, "libc6", ">= 2.39")) in rels,
              "a dependency is DEPENDS_ON")
        check(build("1.2.3", "v1.2.3", [deb, zip_], created=0) == doc, "the order of the downloads doesn't matter")

        out = d / "a.spdx.json"
        write(doc, out)
        check(out.read_bytes().endswith(b"}\n") and b"\r" not in out.read_bytes(), "LF line endings")

        tree = r"""releaseRuntimeClasspath - Runtime classpath of '/release'.
+--- org.jetbrains.kotlin:kotlin-stdlib:2.2.10
|    \--- org.jetbrains:annotations:13.0 -> 23.0.0
\--- androidx.core:core:1.17.0
     +--- androidx.annotation:annotation:1.1.0 -> 1.8.1 (*)
     +--- org.jetbrains.kotlin:kotlin-stdlib -> 2.2.10 (*)
     +--- org.jetbrains.kotlin:kotlin-stdlib:1.8.22 -> 2.2.10 (c)
     +--- org.jetbrains.kotlinx:kotlinx-coroutines-bom:1.8.1
     \--- com.example:missing:1.0 (n)
"""
        got = {(c["name"], c["version"]) for c in gradle_components(tree)}
        check(got == {("org.jetbrains.kotlin:kotlin-stdlib", "2.2.10"),
                      ("org.jetbrains:annotations", "23.0.0"),
                      ("androidx.core:core", "1.17.0"),
                      ("androidx.annotation:annotation", "1.8.1")},
              f"Gradle's tree gives each library at its resolved version: {sorted(got)}")

        import zipfile
        apk = d / "havne-1.2.3-android-arm64-v8a.apk"
        with zipfile.ZipFile(apk, "w") as z:
            for name in ("lib/arm64-v8a/libQt6Core_arm64-v8a.so", "lib/arm64-v8a/libplugins_platforms_qtforandroid_arm64-v8a.so",
                         "lib/arm64-v8a/libc++_shared.so", "lib/arm64-v8a/libhavne_arm64-v8a.so", "classes.dex"):
                z.writestr(name, b"x")
        listing = apk_components(apk, "6.12.0", "27.3.1", tree, "libhavne_arm64-v8a.so")
        qt = [c for c in listing["components"] if c["name"] == "Qt"][0]
        check(len(qt["files"]) == 2, "the APK's Qt libraries are Qt's")
        with zipfile.ZipFile(apk, "a") as z:
            z.writestr("lib/arm64-v8a/libqttune_arm64-v8a.so", b"x")
        try:
            apk_components(apk, "6.12.0", "27.3.1", tree, "libhavne_arm64-v8a.so")
            failures.append("a native library of no known origin is refused")
        except SbomError:
            pass

        stray = d / "other.zip"
        stray.write_bytes(b"x")
        (d / "other.zip.components.json").write_text(json.dumps({"download": zip_.name}))
        for downloads, why in (([d / "missing.zip"], "a missing download"),
                               ([stray], "a list naming another file"),
                               ([zip_, zip_], "a download listed twice")):
            try:
                build("1.2.3", "v1.2.3", downloads, created=0)
                failures.append(f"{why} is refused")
            except SbomError:
                pass

    for f in failures:
        print(f"FAIL: {f}", file=sys.stderr)
    print(f"sbom selftest: {'ok' if not failures else f'{len(failures)} failed'}")
    return 1 if failures else 0


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build")
    b.add_argument("--version", required=True)
    b.add_argument("--tag", required=True)
    b.add_argument("--out", required=True)
    b.add_argument("downloads", nargs="+", type=Path)
    a = sub.add_parser("apk", help="write an APK's component list")
    a.add_argument("--apk", required=True, type=Path)
    a.add_argument("--qt", required=True, help="the Qt kit's version")
    a.add_argument("--ndk", required=True, help="the NDK's version")
    a.add_argument("--gradle", required=True, type=Path,
                   help="gradlew dependencies --configuration releaseRuntimeClasspath, as text")
    a.add_argument("--app-lib", default="libhavne_arm64-v8a.so")
    s = sub.add_parser("selftest")
    s.add_argument("--out", help="also write the self-test's document here (for a validator)")
    args = parser.parse_args(argv)
    try:
        if args.cmd == "selftest":
            code = selftest()
            if args.out:
                with tempfile.TemporaryDirectory() as tmp:
                    deb = Path(tmp) / "havne-core_1.2.3_amd64.deb"
                    deb.write_bytes(b"deb")
                    Path(str(deb) + ".components.json").write_text(json.dumps({
                        "download": deb.name, "purpose": "INSTALL",
                        "components": [{"name": "Qt", "version": "6.12.0",
                                        "license": "LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only",
                                        "purl": "pkg:generic/qt@6.12.0", "files": ["libQt6Core.so.6"]}],
                        "depends": [{"name": "libc6", "version": ">= 2.39",
                                     "purl": "pkg:deb/ubuntu/libc6?arch=amd64"}]}))
                    write(build("1.2.3", "v1.2.3", [deb], created=0), args.out)
            return code
        if args.cmd == "apk":
            listing = apk_components(args.apk, args.qt, args.ndk,
                                     args.gradle.read_text(encoding="utf-8"), args.app_lib)
            out = Path(str(args.apk) + ".components.json")
            out.write_text(json.dumps(listing, indent=1, sort_keys=True) + "\n",
                           encoding="utf-8", newline="\n")
            print(f"sbom: {out}, {len(listing['components'])} components")
            return 0
        write(build(args.version, args.tag, args.downloads), args.out)
        print(f"sbom: {args.out}, {len(args.downloads)} downloads")
        return 0
    except SbomError as e:
        print(f"sbom: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
