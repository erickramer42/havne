# Packaging and releases

How the Linux downloads are packed, the SBOM, and how a version tag
becomes a release. The desktop builds and tests are in
[development.md](development.md); the mobile builds in [mobile-build.md](mobile-build.md).

## Linux: the AppImage

`packaging/linux/make-appimage.sh <version> [build dir] [out dir]`
packs a Release tree into `dist/havne-<version>-linux-<arch>.AppImage`,
`<arch>` the build machine's (`x86_64` or `aarch64`, from `uname -m`):
linuxdeploy and its Qt plugin, pinned releases for each architecture
checked against their SHA-256 and kept in `~/.cache/havne-linuxdeploy`,
gather the app, the
Qt libraries, plugins and QML modules it loads (the plugin scans
`app/qml`), the xcb platform plugin and the offscreen one, and
`packaging/linux/havne.desktop` with the icon. The AppImage starts
with type2-runtime's runtime from a pinned release (20251108, commit
`dd6cebe`), checked against its SHA-256 and handed to linuxdeploy as
`LDAI_RUNTIME_FILE`: with none named, linuxdeploy's appimagetool
downloads type2-runtime's newest `continuous` build at every pack.
Before a second
linuxdeploy run packs the AppDir, the script puts the notices in
`usr/share/doc/havne/`: `LICENSE`, `packaging/notices/`'s
`THIRD_PARTY_NOTICES.txt`, the LGPL-3.0, GPL-3.0 and ICU texts
(LGPL-3.0 section 4 wants Qt's notice and both texts with each copy)
and `Qt-third-party.txt` (the credits and license texts of the code
Qt's own libraries contain, from Qt's SBOM for the modules shipped),
the runtime's MIT text and those of what it links statically
(libfuse's LGPL-2.1, squashfuse, musl, mimalloc, zstd, zlib),
and under `common-licenses/` the `/usr/share/common-licenses` texts
that the bundled Ubuntu packages' copyright files name (linuxdeploy
puts each in `usr/share/doc/<package>/`). It then fails a package that
holds `qmltooling` (a Debug build's QML debugger), one missing a notice
or a bundled package's copyright file, one whose notice names another
Qt, ICU or runtime than it packed, one whose runtime reports another
commit (`--appimage-version`), or one that isn't still running 10 s
after it starts on the offscreen platform.

- **The runtime's sources:** it links libfuse statically, so LGPL-2.1
  section 6 wants libfuse's source and the runtime's offered beside the
  AppImage. `packaging/linux/runtime-sources.sh [out dir]` fetches
  `fuse-3.15.0.tar.xz` and `type2-runtime-20251108.tar.gz` (GitHub's
  archive of the commit), each checked against its SHA-256, and
  `release.yml`'s `publish` attaches both to each release.
- **A new runtime pin** changes, together: `make-appimage.sh`'s
  release, commit and both SHA-256s (GitHub's digests of
  `runtime-x86_64` and `runtime-aarch64`); `runtime-sources.sh`'s
  release, commit and the runtime tarball's SHA-256, and libfuse's
  version and SHA-256 when the runtime's
  `scripts/common/install-dependencies.sh` names another; the notice's
  runtime section and the component list's versions in
  `make-appimage.sh` (libfuse and squashfuse from that script, musl,
  mimalloc, zstd and zlib from Alpine's packages for the release its
  `scripts/chroot/chroot_build.sh` names, as of the runtime's build
  date); and the license texts under `packaging/notices/licenses/`.
  Release 20251108 lacks one later change, commit `8f39b89`, which
  makes the folder `--appimage-extract-and-run` unpacks into private
  (0700, not 0755); the AppImage's files are public, so that discloses
  nothing.

- **The build machine** needs Qt's xcb plugin libraries for linuxdeploy
  to bundle: `libxcb-cursor0 libxcb-icccm4 libxcb-image0
  libxcb-keysyms1 libxcb-randr0 libxcb-render-util0 libxcb-shape0
  libxcb-sync1 libxcb-xfixes0 libxcb-xinerama0 libxcb-xkb1
  libxkbcommon-x11-0` on Ubuntu.
- **The same bytes from one commit:** two builds match when both carry
  the commit's time. Set `QT_RCC_SOURCE_DATE_OVERRIDE` to it before
  `cmake --build`, since `rcc` otherwise records each embedded file's
  modification time, which is when it was checked out. The script sets
  `SOURCE_DATE_EPOCH` for the squashfs's times (default
  `git log -1 --format=%ct`), and warns when the first variable is
  missing. CI and the release set both:
  `export SOURCE_DATE_EPOCH=$(git log -1 --format=%ct) QT_RCC_SOURCE_DATE_OVERRIDE=$SOURCE_DATE_EPOCH`.
- **glibc:** an AppImage runs only where glibc is at least its build
  machine's, so the one to hand out is CI's (`linux-appimage`, Ubuntu
  24.04, glibc 2.39, on x86_64 and on the ARM runner). One built on a
  newer distribution is for checking the packing.
- **Some libraries come from the system:** linuxdeploy's exclude list
  leaves out what every desktop has. Those are OpenGL (libGL, libGLX
  and libEGL, which belong to the machine's graphics driver),
  fontconfig, freetype and libgpg-error. A minimal install (WSL's Arch
  and Kali images, for example) lacks some of them, and the app then stops at once
  with "error while loading shared libraries: libGLX.so.0" (or
  `libfreetype.so.6`, or another). Mesa and the font libraries fix it:
  `pacman -S --needed libglvnd mesa fontconfig freetype2` on Arch, and
  `apt-get install libgl1 libegl1 libopengl0 libfontconfig1 libfreetype6 libgpg-error0`
  on Debian and its kin.

## Linux: the core packages

`packaging/linux/make-core-packages.sh <version> [build dir] [out dir]`
builds core and `havne-cli` in a core-only tree of its own and packs
them ([ADR-0035](adr/0035-linux-downloads-sbom-reproducible.md)):

for the build machine's architecture (`dpkg --print-architecture`:
`amd64` or `arm64`, the tarballs naming it `x86_64` or `aarch64`):

| File | Holds |
|---|---|
| `havne-core_<version>_amd64.deb` | `/usr/bin/havne-cli`, `/usr/lib/libecuio-core.a`, the public headers in `/usr/include/ecuio/`, a CMake package (`find_package(ecuio)`, target `ecuio::core`) and `ecuio.pc` |
| `havne-core-<version>-linux-x86_64.tar.gz` | the same tree under one folder, relocatable |
| `havne-core-dbgsym_<version>_amd64.deb`, `…-debug.tar.gz` | `havne-cli`'s debug info, `/usr/lib/debug/.build-id/<xx>/<rest>.debug` |

`cmake --install <tree> --prefix <dir>` lays out the same tree from any
core build (`install()` rules in `core/` and `cli/`; the internal
headers stay out).

- **The build machine** needs `cmake ninja-build dpkg-dev
  systemtap-sdt-dev` (Ubuntu); the script refuses a build without the
  USDT probes.
- **Reproducible:** the same commit and toolchain give the same bytes
  anywhere. `-ffile-prefix-map` maps the source and build folders away,
  `SOURCE_DATE_EPOCH` (default: the commit's time) dates every file,
  and modes, owners and order are fixed. CI's `linux-packages` builds
  twice under `reprotest` and fails on any difference;
  `reprotest 'bash packaging/linux/make-core-packages.sh 0.3.4-x build-packages dist' 'dist/*'`
  does the same by hand (Ubuntu: `sudo apt install reprotest diffoscope`).
- **The checks** in the script: the stripped `havne-cli` keeps its
  build ID and the three probes, has no debug info left and a debug link;
  no internal header ships; no file names the build's folders.
  `tests/package_check.sh <out dir> <version>` (needs `sudo`, `gdb` and
  `pkg-config`) installs both debs, runs `havne-cli`, builds
  `tests/package/consumer.c` through `find_package`, `pkg-config` and
  the unpacked tarball, and checks `gdb` finds the debug info.

To read a crash or a profile of the packaged `havne-cli`, install
`havne-core-dbgsym` (or unpack the `-debug` tarball under `/`): `gdb`
and `perf report` find the debug file by the binary's build ID.

## The SBOM

`packaging/sbom.py` writes the release's SPDX 2.3 document
(ADR-0035 section 5) from the component lists the build jobs write beside each
download (`<download>.components.json`): `stage.ps1` (every DLL in the
Windows folder, by its version resource; an unknown one fails the
stage), `make-appimage.sh` (Qt and ICU from the kit, every other
library by its Ubuntu package), `make-core-packages.sh` (the deb's
`Depends`) and `sbom.py apk` (the APK's native libraries and
`gradlew dependencies`). `python3 packaging/sbom.py selftest` pins its
rules (ctest `SbomTest.SelfTest`). Validate a document with
`pyspdxtools -i <file>` from `.github/spdx-requirements.txt` (hash-pinned;
bump by hand, as its comment says).

## Releases

A version tag publishes these downloads through
`.github/workflows/release.yml`:

- **Windows x64,** a zip that runs on a PC with neither Qt nor Visual
  Studio: the app, the x86 and x64 J2534 surrogates, the Qt runtime
  that windeployqt deploys, and the app-local Visual C++ runtime.
- **Android arm64-v8a,** an APK for Android 12 or later, signed with
  the release key ([ADR-0030](adr/0030-signed-android-releases.md)),
  so each release installs over the one before.
- **Linux x86_64 and 64-bit ARM (aarch64),** for each the AppImage
  ([Linux: the AppImage](#linux-the-appimage)), and core alone as a deb
  and a tarball ([Linux: the core packages](#linux-the-core-packages)),
  built on Ubuntu 24.04, so they run where glibc is 2.39 or later
  (on a Raspberry Pi, the 64-bit Raspberry Pi OS 13).
- **The Python wheel,** one for Windows x64
  (`havne-<version>-py3-none-win_amd64.whl`), Linux x86_64 and aarch64
  (`manylinux_2_39`: glibc 2.39 or later) and macOS arm64 (13.0 or
  later): the `havne` package with core's shared library, the Visual
  C++ runtime linked into it, and on Windows both J2534 surrogates
  ([ADR-0053](adr/0053-python-bindings.md)). An `-rcN` tag's wheel is
  `<version>rcN` (PEP 440).

Beside them: the Windows PDBs and `havne-cli`'s Linux debug info, an
SPDX SBOM of every download, `SHA256SUMS.txt`, and one build
provenance over every file.

**To release:**

1. In one commit, set `project(Havne VERSION X.Y.Z)` in the root
   `CMakeLists.txt` and `__version__` in `python/havne/_version.py`
   (`PythonTest.test_version` holds them together; `make_wheel.py`
   refuses a release whose two differ), move CHANGELOG.md's "Unreleased" entries under
   `## [X.Y.Z] - <date>` (and add its compare link at the foot), and
   add `docs/releases/vX.Y.Z.md`, the release notes, starting from
   those entries. Open with a Downloads table linking the file each
   platform wants, since the release's own list is flat and sorted by
   name. Say what's new, and what's verified at which tier
   ([testing.md](testing.md)), including what hasn't run on hardware,
   and give the release key's certificate fingerprint.
2. Make that commit on `dev`, after the review's fixes and the docs,
   and push it; open the PR from `dev` to `main` (or push to the open
   one). Once the PR's CI run passes, merge it with a merge commit, and
   wait for the merge's CI run on `main` to pass: the release job
   refuses a commit whose CI hasn't. A tag pushed first fails the job
   at its CI gate; once CI on the commit passes, rerun the failed job
   (`gh run rerun <run id>`), which keeps the tag's ref and so the
   provenance's signer.
   For a dry run, tag that commit `vX.Y.Z-rc1` and push the tag. The
   workflow publishes a pre-release. Download its zip, run it on a PC
   without Visual Studio if one is available, install its APK on a
   phone, run its AppImage on the distributions (the `linux` skill's
   `distros`), and check the provenances with
   `gh attestation verify <file> -R erickramer42/havne` and the SBOM's
   with `--predicate-type https://spdx.dev/Document/v2.3` added. Then
   delete the pre-release and its tag.
3. Tag `vX.Y.Z` and push it.

**What the jobs do,** in order:

- **`gate`: the version.** The tag must be `vX.Y.Z` or `vX.Y.Z-rcN`,
  its `X.Y.Z` must equal the CMake project version, and
  `docs/releases/vX.Y.Z.md` must exist, or the run fails before
  anything builds. An `-rcN` tag publishes as a pre-release with the
  same notes.
- **`gate`: CI.** ci.yml never runs on tags, so the job reads the newest
  `push` run of ci.yml on the tagged commit (`gh run list --commit`),
  the merge's run on `main`,
  and fails unless it has completed with `success`: a tag pushed
  before CI finished, or on a commit whose CI failed, publishes
  nothing.
- **`windows`: build.** Qt 6.12.0 is installed as `win-build` does it,
  then `build-win32` (the surrogate only) and `build` (the app only),
  both with `BUILD_TESTING=OFF`, in the usual order so the app's build
  copies the x86 surrogate beside `havne.exe`, with its own x64 one.
  CI has already run the suite on the commit; the cross-arch suite is
  `scripts/verify.sh`'s.
- **`windows`: stage, check and zip:** `packaging/windows/stage.ps1`,
  which also runs locally against a built tree
  (`pwsh packaging/windows/stage.ps1 -Version X.Y.Z-local -OutDir Testing/dist`).
  It copies the deployed app without its `.pdb`, `.lib`, `.exp` and
  `.ilk` files, adds the x64 CRT from Visual Studio's redistributable folder
  (found with `vswhere`; windeployqt runs with `--no-compiler-runtime`),
  `LICENSE`, `DISCLAIMER.md` and `packaging/windows/`'s README,
  third-party notices and license texts. It then fails the release if
  either surrogate imports a CRT DLL (each links the CRT statically, `/MT`,
  because an x86 `vcruntime140.dll` can't sit beside the app's x64
  one), if `havne.exe` imports a DLL that is neither in the folder nor
  part of Windows, if `havne.exe` or either surrogate carries a version
  resource other than "Havne X.Y.Z" (`packaging/windows/version.rc.in`,
  filled from the CMake project version; a stale tree fails it), if it
  carries QML debugging (an import of
  `QQmlDebuggingEnabler`, or `qmltooling` plugins in the folder: a
  build with `QT_QML_DEBUG` opens an unauthenticated debug server when
  launched with `-qmljsdebugger`, so `app/CMakeLists.txt` defines it
  and windeployqt deploys the plugins in Debug only), or if the staged
  `havne.exe` isn't still running
  10 s after it starts (QML that fails to load exits at once; the
  check was confirmed against a package with `qml/QtQuick/Controls`
  removed, 2026-10-25). It writes the zip to `dist/` (git-ignored).
  Beside it goes `havne-X.Y.Z-windows-x64-symbols.zip`, the four
  executables' PDBs from the trees that built them
  ([ADR-0034](adr/0034-debug-symbols-and-probes.md)). The release fails
  if one is missing, if an exe records anything but its PDB's file name
  (the job builds with `LINK=/PDBALTPATH:%_PDB%` in the environment,
  since CMake's Visual Studio generator can't pass the `%` through; a
  local stage needs the same), or if `dumpbin /pdbpath:verbose` doesn't
  find the PDB beside its exe, which it does only when the signature and
  age match (a PDB from another build failed it, 2026-10-29). A local
  dry run of trees built without `LINK` (`scripts/verify.sh`'s) passes
  `-AllowBuildPath`: the recorded path may be the build's, the PDB must
  still match.
- **`android`: build and sign.** The kit, NDK and JDK as
  `android-build`, but a Release build: optimized, not debuggable, and
  unsigned. The job is the only one that names the `release`
  environment, whose secrets hold the release key; the keystore exists
  on the runner for the signing step only.
  `packaging/android/sign-apk.sh` checks the unsigned APK (package
  `io.github.erickramer42.havne`, the version name and code, not
  debuggable, arm64-v8a only, no `qmltooling` plugins, an app built
  without `QT_QML_DEBUG`), aligns it, signs it with APK Signature
  Scheme v3, and fails unless the signer's certificate SHA-256 is the
  one pinned in `packaging/android/release-cert.sha256`. Locally,
  `HAVNE_CERT_SHA256` overrides the pin, so a throwaway key runs the
  same checks. A last step writes the APK's component list
  (`sbom.py apk`).
- **`linux`: build and pack** (Ubuntu 24.04, once on x86_64 and once on
  the ARM runner). Qt as `linux-appimage`, the app built and packed by
  `make-appimage.sh`, dated by the commit, then
  `make-core-packages.sh`: the same scripts and checks CI ran on the
  commit.
- **`windows`, `linux`, `macos`: the wheel.** `packaging/python/make_wheel.py`
  packs each platform's wheel under its tag (`win_amd64` with both
  surrogates; `manylinux_2_39_<arch>` from `make-core-packages.sh`'s
  tree, its debug info stripped; `macosx_13_0_arm64`) and refuses a
  library that doesn't fit it. `check_wheel.py` then installs it into a
  fresh venv and runs `read_pids.py mock://` through it, under
  `ECUIO_PYTEST_REQUIRED=1`, so it can't skip. Each wheel's
  `.components.json` is a job artifact for the SBOM, never a release
  file.
- **`macos`: the shared core** (macOS arm64): a core-only tree with
  `CMAKE_OSX_DEPLOYMENT_TARGET=13.0` (Qt 6.12 needs 14.4, core no more
  than 13.0); the only build at 13.0, so a release candidate is its
  first run.
- **`publish`: the SBOM.** `sbom.py build` over the zip, the APK,
  each architecture's AppImage, deb and tarball, and the four wheels (the symbols and debug files carry
  no component of their own), dated by the commit, then validated by
  `pyspdxtools` before anything is published.
- **`publish`: checksums and provenance.** `SHA256SUMS.txt` lists every
  download, the debug files and the SBOM, with LF line ends.
  `actions/attest-build-provenance` signs one SLSA build provenance
  over every download, tied to this repository's workflow, and stores
  it in GitHub's attestation store; `actions/attest` attests the SBOM
  over the downloads it describes. The job also copies the Sigstore
  bundle into `dist/` as `havne-X.Y.Z.intoto.jsonl`, one file for all
  the downloads, because OpenSSF Scorecard's Signed-Releases check
  reads only release files (it counts `*.intoto.jsonl` as provenance
  and `*.sig`, `*.asc` or `*.sigstore.json` as signatures), and so a
  download can be checked with
  `gh attestation verify <file> --bundle havne-X.Y.Z.intoto.jsonl`.
- **`publish`: the release.** `gh release create` with the downloads,
  the provenance bundle, the SBOM, `SHA256SUMS.txt` and the notes
  file. A
  manual run for an existing tag
  (`gh workflow run release.yml -f tag=vX.Y.Z`) rebuilds it and
  replaces the release's files.

The exe isn't code-signed, so SmartScreen warns on first run; the
zip's `README.txt` and the release notes say how to get past it.

### The Android release key

Android installs an update only when it carries the same certificate
as the app it replaces, so the release key is the app's identity for
good ([ADR-0030](adr/0030-signed-android-releases.md)). It is made
once:

1. **The key:** a PKCS12 keystore with one RSA 4096 key, alias
   `havne`, one password for store and key, validity 10,000 days, and
   the certificate name `CN=Havne` only (anyone with the APK can read
   it). Pass the password through an environment variable, never on
   the command line:

   ```sh
   read -rs -p 'release key password: ' HAVNE_KEYSTORE_PASS; echo; export HAVNE_KEYSTORE_PASS
   keytool -genkeypair -keystore havne-release.p12 -storetype PKCS12 -alias havne \
       -keyalg RSA -keysize 4096 -validity 10000 -dname "CN=Havne" \
       -storepass:env HAVNE_KEYSTORE_PASS -keypass:env HAVNE_KEYSTORE_PASS
   ```

2. **The pin:** the certificate's SHA-256, lowercase hex without
   colons, in `packaging/android/release-cert.sha256`, committed:

   ```sh
   keytool -list -v -keystore havne-release.p12 -alias havne -storepass:env HAVNE_KEYSTORE_PASS \
       | tr -d '\r' | sed -n 's/^[[:space:]]*SHA256: //p' | tr -d ':' | tr 'A-F' 'a-f' \
       > packaging/android/release-cert.sha256
   ```

3. **The environment:** a GitHub environment named `release` that only
   `v*` tags and `main` may deploy to (Settings to Environments, or
   `gh api -X PUT repos/<repo>/environments/release` with a custom
   deployment policy, then one tag rule and one branch rule).
4. **Its secrets,** from stdin, so neither lands in the shell's
   history:

   ```sh
   base64 -w0 havne-release.p12 | gh secret set ANDROID_KEYSTORE_B64 --env release
   printf '%s' "$HAVNE_KEYSTORE_PASS" | gh secret set ANDROID_KEYSTORE_PASS --env release
   ```

5. **A local signing** with the real key, before any tag: a Release
   tree built with the Android kit (as in [mobile-build.md](mobile-build.md), with
   `-DCMAKE_BUILD_TYPE=Release`), then
   `HAVNE_KEYSTORE=havne-release.p12 ANDROID_SDK_ROOT=<sdk> packaging/android/sign-apk.sh <tree>/app/android-build/build/outputs/apk/release/android-build-release-unsigned.apk X.Y.Z-local Testing/dist`.

Keep the keystore and its password in two places off the development
machine, a password manager and an offline copy. A lost key can't be
replaced: every phone would have to uninstall the app, and its data
with it, to take a build signed by another.
