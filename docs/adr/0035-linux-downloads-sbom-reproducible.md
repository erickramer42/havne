# ADR-0035: Linux downloads, an SBOM with each release, and reproducible Linux builds

- **Status:** Accepted.
- **Date:** 2026-10-29.
- **Builds on:** the release workflow (`.github/workflows/release.yml`;
  `docs/development.md`, "Releases");
  [ADR-0001](0001-qt-free-core-c-abi.md) (the C ABI the core package
  carries); [ADR-0031](0031-command-line-client.md) (`havne-cli`);
  [ADR-0034](0034-debug-symbols-and-probes.md) §2 (Linux debug info, which
  this record builds).

## Context

A release publishes a Windows zip, its symbols and an Android APK. Linux
has nothing to download, though every Linux CI job builds and tests the
app and core:

- **The app:** `packaging/linux/make-appimage.sh` packs and smokes an
  AppImage on CI (`linux-appimage`, Ubuntu 24.04), kept as a 14-day
  artifact; no release carries it.
- **Core alone:** `havne-cli`, the static `ecuio-core` and its public
  headers are what another program builds against (ADR-0001, ADR-0031).
  Nothing installs them: CMake has no `install()` rules, so a user
  builds the tree and copies files by hand.

Two questions about a download have no answer today:

- **What's inside it.** The Windows zip and the AppImage bundle Qt's
  libraries, and the AppImage some of the build machine's. A user who
  checks a download against a vulnerability feed has to unpack it and
  guess versions from file names.
- **Whether it's the source's.** The provenance (`.intoto.jsonl`) says
  which workflow built a file from which commit. It doesn't say the
  same source builds the same file, which is what lets anyone rebuild a
  download and compare.

## Decision

### 1. Four Linux downloads, built on Ubuntu 24.04

The release workflow gains a `linux` job on Ubuntu 24.04, the oldest
distribution supported, since a binary runs only where glibc is at least
its build machine's (2.39). It publishes:

| File | Holds |
|---|---|
| `havne-X.Y.Z-linux-x86_64.AppImage` | the app, as `make-appimage.sh` packs it |
| `havne-core_X.Y.Z_amd64.deb` | `havne-cli`, the static library, the public headers, a pkg-config file and a CMake package |
| `havne-core-X.Y.Z-linux-x86_64.tar.gz` | the same tree, relocatable, for any glibc 2.39+ distribution |
| `havne-core-dbgsym_X.Y.Z_amd64.deb` and `havne-core-X.Y.Z-linux-x86_64-debug.tar.gz` | `havne-cli`'s debug info (§3) |

Each goes into `SHA256SUMS.txt` and gets a provenance attestation, like
the Windows files.

### 2. What the core package installs

CMake gains `install()` rules, so `cmake --install` lays out the same
tree the packages hold (under `/usr` in the deb, the tarball's own
folder otherwise):

- `bin/havne-cli`
- `include/ecuio/`: the headers whose `File/layer` line says public C
  ABI (`core.h`, `j2534.h`, `profile.h`, `serial.h`, `signals.h`,
  `socketcan.h`, `stream.h`) and the generated `version.h`. The internal
  ones (`internal.h`, `j2534_ptypes.h`, `j2534_surrogate.h`,
  `surrogate_protocol.h`, `signal_sets.h`) stay out.
- `lib/libecuio-core.a`
- `lib/pkgconfig/ecuio.pc`. Its `Libs` name `-lstdc++ -lm -pthread`
  beside the archive, since a static library is all there is, and a C
  program linking it needs the C++ runtime. Its prefix is
  `${pcfiledir}/../..`.
- `lib/cmake/ecuio/`: `find_package(ecuio)` gives the imported target
  `ecuio::core`. A minor version accepts only its own minor
  (`SameMinorVersion`), since before 1.0 a minor may break the ABI. A
  project that enables only C links with the C driver, which leaves out
  the C++ runtime, so on Linux the package names it there too.
- `share/doc/havne-core/copyright` (the MIT license)

`lib/`, not Debian's multiarch `lib/x86_64-linux-gnu/`: one layout
serves the deb and the tarball, and `pkg-config` and CMake search
`/usr/lib` too. Every path in the CMake and pkg-config files is relative
to the file, so the tarball works wherever it's unpacked.

**Static only.** A shared `libecuio.so` is what an FFI caller (v0.4.0's
Python bindings) loads, and it needs a soname policy: before 1.0 a
minor version may break the ABI (CHANGELOG.md), so the soname would
carry the minor version. That decision belongs with the bindings. The
archive ships with its debug info, since whoever links it debugs their
own program with it.

`packaging/linux/make-core-packages.sh <version> [build dir] [out dir]`
configures and builds a core-only tree of its own, like
`make-appimage.sh` packs one: it installs into a staging folder, splits
the debug info, then writes the debs with `dpkg-deb --root-owner-group
-Zxz --build` (the control file's `Depends` from `dpkg-shlibdeps`) and
the tarballs with GNU tar. No
`debian/` directory and no debhelper: the packages are built from the
tree CMake installs, and the same script makes the tarball.

### 3. Debug info (ADR-0034 §2)

The Linux packages build in `Release`, the configuration CI tests,
with `-g` added (`CMAKE_CXX_FLAGS`; it doesn't change the code), and
then, for `havne-cli`:

1. `objcopy --only-keep-debug` writes the debug file.
2. `strip --strip-debug --strip-unneeded`, then
   `objcopy --add-gnu-debuglink`, link the binary to it.
3. The debug file goes to `usr/lib/debug/.build-id/<xx>/<rest>.debug`,
   named by the binary's GNU build ID (`readelf -n`).

The `-dbgsym` deb holds that tree (Debian's layout: `Section: debug`,
`Depends: havne-core (= X.Y.Z)`). The `-debug` tarball holds the same
tree to unpack under `/`. `gdb`, `perf report` and `debuginfod` clients
find the file by build ID. The binaries keep the USDT probes
(ADR-0034 §4): the build installs `systemtap-sdt-dev`.

### 4. Reproducible: the core packages, byte for byte

The core packages build the same bytes from the same commit on any
machine with the same toolchain:

- **Paths:** `-ffile-prefix-map=<source>=.` and the same for the build
  folder, so neither debug info nor `__FILE__` records where it was
  built. (`/PDBALTPATH` is the Windows half of the same rule, ADR-0034.)
- **Time:** `SOURCE_DATE_EPOCH` is the commit's time (`git log -1
  --format=%ct`). `dpkg-deb` clamps file times to it, and tar writes
  `--sort=name --mtime=@$SOURCE_DATE_EPOCH --owner=0 --group=0
  --numeric-owner --format=gnu`, piped through `gzip -9n`.
- **Modes and order:** every file and folder gets a fixed mode, and the
  `umask` is set, so neither comes from the build machine. The static
  archive relies on `ar`'s deterministic mode (`D`, Ubuntu's default);
  reprotest would catch it otherwise.

CI's new `linux-packages` job runs `reprotest` over
`make-core-packages.sh`. It builds twice, the second time with reprotest's
variations (build path, environment, locale, time zone, umask, file
order, CPU count, user's home, kernel personality, ASLR, the clock), and
requires every package to match. `user_group` is left out: it needs
other accounts on the runner. On a mismatch, `diffoscope` prints the
difference.

**The AppImage is measured, not gated.** Its contents come from aqt's
Qt and Ubuntu's packages at fixed versions, but linuxdeploy rewrites
each library's run path and the squashfs records times, and neither is
under this project's control. Two builds of one commit are compared
once, and the result goes into `docs/testing.md`. A fix that makes them
match is a later item. **Windows and Android stay
out:** MSVC needs `/Brepro` and more, and Gradle its own settings.

> **Amended 2026-10-29:** the AppImage is built and packed with the
> commit's time, `QT_RCC_SOURCE_DATE_OVERRIDE` for the files `rcc`
> embeds in the app and `SOURCE_DATE_EPOCH` for the squashfs, and two
> builds of one commit then match (`docs/testing.md`). It stays
> measured, not gated: CI builds it once per run, and its bundled
> libraries follow Ubuntu's updates between runs.

### 5. An SBOM with each release

The release publishes `havne-X.Y.Z.spdx.json`, one SPDX 2.3 document
with each download as a package, by file name and SHA-256, and what it
contains:

| Download | Components |
|---|---|
| Windows zip | Havne, Qt (each `Qt6*.dll`, plugin and QML module windeployqt deployed), the Visual C++ runtime, Direct3D's and DirectX's shader compilers, Mesa's software OpenGL; each at its DLL's version resource. A DLL that matches none fails `stage.ps1` |
| AppImage | Havne, Qt and its ICU (what came from the Qt kit), the AppImage runtime, each other bundled library by its Ubuntu package and version (`dpkg -S` on the build runner), as `pkg:deb/ubuntu/<name>@<version>` |
| APK | Havne, Qt for Android (each `libQt6*`, `libplugins_*` and `libqml_*` in `lib/arm64-v8a`), the NDK's libc++, and the Java libraries Gradle resolved for the Release build (`gradlew dependencies`). A native library from anywhere else fails the job |
| Core deb and tarball | Havne core; it bundles nothing, so the libraries its `Depends` names (`libc6`, `libgcc-s1`, `libstdc++6`) are listed as what it depends on |

Each build job writes a component list of what it actually packed, so
nothing is listed from memory. `packaging/sbom.py` (Python's standard
library only) merges them in the `publish` job, with each download's
checksum and license (Havne MIT; Qt as its sources declare their
open-source terms; anything else `NOASSERTION`, never guessed). The
symbols and debug files carry no component of their own and are left
out. `pyspdxtools` (hash-pinned, `.github/spdx-requirements.txt`)
validates the document before anything is published, and it gets an
SBOM attestation (`actions/attest`) over the downloads it describes,
verified with `gh attestation verify --predicate-type
https://spdx.dev/Document/v2.3`.

SPDX rather than CycloneDX: GitHub's dependency graph exports SPDX, and
the Linux Foundation's `spdx-tools` validate it. Either would serve.

### 6. Tests

- **HARNESS, CI's `linux-packages` (Ubuntu 24.04, every push):**
  - builds the packages with `make-core-packages.sh`
  - runs reprotest over `make-core-packages.sh` (§4). The script
    itself fails a package without the USDT probes, with a build ID or
    debug link that doesn't match, with an internal header, or with a
    file that names the build's folders.
  - `tests/package_check.sh` installs the deb and its `-dbgsym` with
    `apt-get`, runs `havne-cli stream mock://` from `PATH`, and builds
    `tests/package/consumer.c` (a C program running a mock session)
    through `find_package` and through `pkg-config`, and runs each
  - unpacks the tarball elsewhere and builds the same consumer against
    it by `CMAKE_PREFIX_PATH`, proving it relocatable
  - checks `gdb` finds `havne-cli`'s debug info by build ID
  - builds an SBOM from these packages' component lists and validates it
- **UNIT:** `sbom.py`'s self-test (ctest `SbomTest.SelfTest`): the
  merge, the Gradle tree's parse and the APK's classification on
  fixtures, and its refusals. CI's `linux-packages` validates the
  self-test's document with `pyspdxtools`.
- **Release:** the `linux` job runs `make-appimage.sh` and
  `make-core-packages.sh` with their own checks; the `rc` dry run checks
  the attestations, and the AppImage on the distributions (the `linux`
  skill's `distros` run).

## Alternatives considered

- **CPack's DEB and TGZ generators.** They come from the same
  `install()` rules, and CPack can split a `-dbgsym` package. But they
  don't build the `-debug` tarball, and their archives' timestamps and
  ordering aren't reproducible without the same tar flags passed through
  CPack's variables. One script controls both packages directly.
- **A `debian/` directory with debhelper.** It's Debian's own recipe,
  and `dh_strip` makes the `-dbgsym` package. It brings a second build
  description beside CMake's, and builds only debs.
- **A shared library now.** See §2: the soname policy belongs with the
  bindings that need it.
- **One SPDX document per download.** That's more files on the release
  page for the same content. One document names every download, and its
  attestation covers each.
- **`syft` to generate the SBOM.** It identifies software by its package
  manager's metadata, which a windeployqt folder and an AppImage
  don't carry. The build jobs know exactly what they packed.

## Consequences

- **Good:**
  - Linux users get the app and, separately, core and `havne-cli` for
    their own programs, installed and found the usual ways.
  - A crash or profile of `havne-cli` reads against its source.
  - Anyone can rebuild the core packages from a tag and compare bytes,
    and every download lists what it contains.
- **Bad:**
  - The release grows by six files (eleven with their provenance) and a
    job, and the Linux and SBOM steps can fail a release.
  - The deb is built on Ubuntu 24.04 for Debian-family systems with
    glibc 2.39 or later; it isn't in any distribution's archive or a
    PPA. Other distributions use the tarball.
  - The AppImage's reproducibility is reported, not promised.
