# CI

`ci.yml` runs on every pull request to `main`, on every push to
`main`, and on demand (Actions > CI > Run workflow, on any branch, or
`gh workflow run ci.yml --ref <branch>`). The PR is the gate: the one
PR per release from `dev`, or a `hotfix/*` PR, and a push to its
branch cancels the PR's run it supersedes. The push its merge makes to
`main` runs again, and that run is the one the release job reads.
Pushes to `dev` and feature branches get no run: a feature branch is
checked locally (`scripts/verify.sh`, the cross-arch subset, WSL where
it applies) and merged into `dev`, and a manual run on `dev` checks the
platforms only CI has (macOS, iOS, Android, the sanitizers) between
releases. `codeql.yml` follows the same PRs and pushes. A manual run
leaves out the iOS simulator
smoke unless asked: `-f ios_smoke=true` adds it to the whole run, and
`-f smoke_only=true` runs it with `core-and-tests` alone.
`-f macos_repeat='<ctest regex>'` (and
`-f macos_repeat_count=<n>`, 200 by default) has `macos-build` run the
matching tests again until one fails, for a failure seen once on macOS
(its timeout is then 60 minutes). No job waits for another but
`android-build`, which starts when `core-and-tests` ends, whatever its
result: a manual run has more jobs than the account's concurrent
runners, and the short job frees its runner first. So a run takes as
long as its longest job. The long suites are shared between runners,
each with a build of its own: `win-build`, `macos-build` and
`linux-build` run alternate halves of ctest (`-I k,,2`, every case once
across the two), `clang-tidy` three shares of the files, and
`win-coverage` three legs. The steps that aren't the suite (the caches'
saves, the load test, the probes) run on the first shard. Every build and every ctest run
(`coverage.sh`'s too) runs four jobs at once, the hosted runners' core
count (`CMAKE_BUILD_PARALLEL_LEVEL` and `CTEST_PARALLEL_LEVEL` in the
workflow's `env`); the cases that share a fixed resource are
`RUN_SERIAL`. Every job that uses Qt uses 6.12.0, and each kit comes
from the Actions cache under a key of the workflow's own, which names
what decides the install's bytes (the version, the arch, any module,
`--autodesktop`, and aqtinstall's version or, for the Windows kit, its
commit): one copy of each, saved by `main`'s pushes only and read by
every other ref and by `release.yml`'s tags, so the repository stays
under the cache's 10 GB (the iOS kit alone is about 3.6 GB). A miss
installs from the mirrors. The jobs don't use install-qt-action's own
cache, whose key holds the runner's kernel version and which saves on
any ref that misses (the on-request iOS simulator smoke aside, which
caches only on `main`). The other downloads that have failed a run
with a 504 come from the cache the same way: Gradle's distribution and
the Android build's dependencies (keyed on the Qt version, whose
template pins them) and linuxdeploy's pinned files (keyed on
`make-appimage.sh`). The compiler caches (sccache in `win-build`,
`win-coverage`, `macos-build` and `linux-build`; the two Windows jobs
build with Ninja for it) are keyed by the ISO week and saved on `dev`
and `main`. A
week's first run saves its entry, and each later run that week restores
that entry and saves nothing, so a commit's objects reach the cache the
next week and the week's later commits compile cold from the week's
start. That costs build time only, and keeps the entries (one for each
job and week) inside the 10 GB; GitHub drops an entry unread for 7
days.
Every job has a `timeout-minutes` of roughly 3-5x its usual duration:

| Job | Runner | Does |
|---|---|---|
| `core-and-tests` | Ubuntu, GCC, no Qt | Builds core and the Qt-free tests, then runs ctest (jsonschema and pytest from the hash-pinned `.github/*-requirements.txt` in a venv, the tests' Python, so `SchemaFixturesTest` and `PythonTest.*` run, not skip). `WheelTest.Installed` builds a wheel from the tree's library and installs it in a venv. This enforces "core never links Qt." Then, whatever the tests did, it installs libabigail and runs `scripts/abi-check.sh` against the newest release tag ([The C ABI check](#the-c-abi-check)); the result goes to the run's summary. |
| `core-sanitizers` | Ubuntu, GCC, no Qt | The same build and tests in Debug under AddressSanitizer and UndefinedBehaviorSanitizer. LeakSanitizer runs too, so a test that closes a session without destroying it fails here. It brings up `vcan0` as `linux-build` does and requires the vcan suites (`ECUIO_VCAN_REQUIRED`), so the SocketCAN worker runs under the sanitizers, the interface's down-and-up case as root. Blocking since its first green run (2026-10-20). |
| `fuzz` | Ubuntu, Clang, no Qt | Installs the image's clang with its `libclang-rt` runtimes, checks that `-fsanitize=fuzzer` links, builds `fuzz/` (`-DECUIO_BUILD_FUZZERS=ON`, RelWithDebInfo), runs each harness for 60 s from its folder of `fuzz/seeds.py`'s seeds (built from the committed fixtures for the harnesses that read a structured format: JSON, signal files, plugins, OBDb sets, recordings and exports, candump logs, CSV imports and the ELM327 emulator's hosts; empty for the rest), as many at once as the runner has cores, each log in a group of its own, and uploads any crashing input with the logs ([fuzz/README.md](../fuzz/README.md)) |
| `clang-tidy` (1/3 to 3/3) | Windows, MSVC's headers | Installs the hash-pinned clang-tidy wheel, installs Qt as `win-build` does, and in a Developer shell runs `scripts/tidy.sh --qt` (a Ninja tree with Qt) over core, the J2534 layer, the surrogate, `havne-cli` and the desktop app, each runner a third of the files (`HAVNE_TIDY_SHARD=<k>/3`). Any warning fails it. Only the app's moc output is built. |
| `win-build` (1/2, 2/2) | Windows, MSVC 2022 | Builds the full app with Ninja and sccache, then runs its half of ctest (the bridge, J2534 and QML suites included). The second shard also builds core, the shared library and the surrogate with the Visual Studio generator, which `release.yml` builds with. The first runs the load test (`bench_bridge --load`): its table goes to the run's summary, and the step fails on a broken invariant, a bound far below the dev machine's figures, or one of two ratios that compare the run with itself (decode at 1,000 defs against 9, looping with decoding against without) ([testing.md](performance.md#load-test-the-replay-fixture-through-the-bridge)) Its ctest's Python is a venv with pytest, so `PythonTest.*` run. `WheelTest.Installed` builds a wheel from the tree's library and installs it in a venv. |
| `macos-build` (1/2, 2/2) | macOS (arm64), AppleClang | Builds the full app, then runs its half of ctest. The QML suites' link-state cases skip without the fake. A manual run's `macos_repeat` then runs the matching tests again until one fails (above). Its ctest's Python is a venv with pytest, so `PythonTest.*` run. `WheelTest.Installed` builds a wheel from the tree's library and installs it in a venv. |
| `win-coverage` (gtest, ctest-1, ctest-2) | Windows, MSVC 2022 | On pushes to `main` and manual runs, never pull requests: installs Qt as `win-build` does and OpenCppCoverage from its GitHub release (SHA-256 pinned), registers the fake DLL as a Pass-Thru device with three broken entries beside it, runs its leg of `scripts/coverage.sh` (`--part`: the gtest binaries, or one of two slices of ctest's other cases) and uploads the report to Codecov, which merges the three ([testing.md](testing.md#coverage)). |
| `linux-build` (1/2, 2/2) | Ubuntu, GCC | Builds the full app, brings up `vcan0` (the `vcan` and `can-isotp` modules load as installed on the hosted runners), then runs its half of ctest with `ECUIO_VCAN_REQUIRED=1`, so the simulated-bus suite fails rather than skips without the bus, then `SocketCanBusTest.TheInterfaceGoingDownIsAHardFailureThatRecovers` under sudo, then core's USDT probes under bpftrace (`tests/probe_trace.sh`), then the load test as on Windows. `can-utils` gives `CliTransferRunTest.candump` its `candump`. Installs QtGui's EGL and OpenGL runtime libraries, which `test_qml` loads. Its ctest's Python is a venv with pytest, so `PythonTest.*` run (`vcan0` runs `PythonTest.test_vcan`). `WheelTest.Installed` builds a wheel from the tree's library and installs it in a venv. |
| `linux-appimage` | Ubuntu 24.04, GCC, on x86_64 and on the ARM runner (`ubuntu-24.04-arm`) | Builds the app in Release, dated by the commit, and packs it with `packaging/linux/make-appimage.sh` ([Linux: the AppImage](packaging.md#linux-the-appimage)), printing its SHA-256, then uploads `havne-linux-x86_64` or `havne-linux-aarch64` (kept 14 days). On 24.04 because an AppImage runs only where glibc is at least its build machine's. |
| `linux-packages` | Ubuntu 24.04, GCC, on x86_64 and on the ARM runner | Builds the core packages twice under `reprotest` and fails on any difference, then installs and uses them (`tests/package_check.sh`), builds an SBOM from their component lists and validates it and `sbom.py`'s self-test document with `pyspdxtools`, and uploads `havne-core-linux-x86_64` or `havne-core-linux-aarch64` (kept 14 days) ([Linux: the core packages](packaging.md#linux-the-core-packages)). |
| `android-build` | Ubuntu to Android arm64-v8a | Starts when `core-and-tests` ends (`needs`, for a runner slot, not its result). Selects NDK r27 and JDK 21 by version, failing with a clear error if the image lacks either. Then cross-builds and packages the APK. Build only, no tests. |
| `ios-build` | macOS (arm64) to iOS device (arm64) | Unsigned device build with the Xcode generator. Checks the bundle's Info.plist keys (`app/ios/Info.plist.in`) and that it holds no signature or provisioning profile, then uploads it as an unsigned `.ipa`, `havne-ios-arm64-unsigned` (kept 14 days), for sideloading ([ios.md](ios.md)). Build only, no tests. |
| `ios-simulator-smoke` | macOS 15 Intel to iOS Simulator (x86_64) | Runs on a manual run with `ios_smoke` or `smoke_only` ticked, or when the pushed commit's message contains `[ios-smoke]`; skipped on pull requests, other pushes and other manual runs (about 20 minutes on the Intel runner). Boots the newest iOS 18+ iPhone while the app builds, then installs and launches the app. Passes if QML loaded and the app is still running 45 s later. Uploads the `ios-simulator-smoke` artifact: a home-screen control shot, app shots at 15 s and 45 s, the app's stdout/stderr (with `QSG_INFO`), plus crash reports on failure. The job log also prints the stderr. |

The smoke job's pass/fail proves the app starts and stays up. Only
the screenshots show that it drew anything. It doesn't tap through
the UI or connect a session. It runs on an Intel runner because Qt's
simulator libraries are x86_64 only (Known Issue #29). Its first three
runs took 19.5 to 24 min, against about 11 min for the rest of CI, so
ordinary pushes skip it.

**"Core never links Qt" has two checks.** `core-and-tests` builds core
on a runner with no Qt installed. And every configure, local or CI,
ends with a check in the root `CMakeLists.txt` that fails if
`ecuio-core`'s link lists name any `Qt*::` target. That check catches
a stray link in a full build, where Qt is present and the build would
otherwise succeed. It runs after every subdirectory, so it sees a link
added from anywhere, and it reads the link lists by name rather than
testing `if(TARGET Qt6::Core)`, because Qt's imported targets are
visible only in the directory that found them.

**Kit names that moved:** since Qt 6.7 the Linux desktop arch is
`linux_gcc_64`, and `gcc_64` no longer resolves for 6.12.0. Android
kits live under the `all_os` host. The mobile jobs install the host's
desktop Qt with `--autodesktop`.

`scorecard.yml` runs the OpenSSF Scorecard. Actions are pinned to
full commit SHAs with a `# vX.Y.Z` comment, and Dependabot bumps them
weekly. `codeql.yml` runs CodeQL on pull requests to `main`, pushes to
`main` and a weekly schedule: C++ from a Windows core-only build
(core, the J2534 layer and the surrogate; not the app or QML) and the
workflows. Results land in the repository's code-scanning dashboard,
which needs CodeQL's default setup off (Settings to Code security),
because GitHub refuses advanced-setup uploads while default setup is
on.

**Temporary workarounds**, to revert once upstream ships tagged fixes:

- **Windows: Qt is installed with aqtinstall from a pinned master
  commit.** The PyPI release (3.3.0) builds doubled
  `qt6_xxxx/qt6_xxxx/` paths for the Qt 6.11+ Windows repository
  layout; the fix ([aqtinstall #1000](https://github.com/miurahr/aqtinstall/pull/1000))
  is on master only. Go back to PyPI once aqtinstall 3.4.0 ships it.
  Dependabot doesn't track a git pin, so check for the release by
  hand. The job extracts with the runner's 7-Zip (`--external 7z`)
  rather than py7zr, which fails sporadically on Windows with
  `Bad7zFile`. aqtinstall verifies each archive's SHA-256 before
  extracting, so that error is py7zr's, not the download's. 7-Zip
  extracts the archives in parallel, and the extracts race to create
  the install root ([aqtinstall #1002](https://github.com/miurahr/aqtinstall/issues/1002)),
  so the step creates the directory before installing. Drop that
  line with the pin.
- **macOS needs Qt 6.12.** Qt 6.8 and earlier referenced the AGL
  framework, which Apple removed from the Xcode 26 SDK.
- **The iOS simulator smoke runs on `macos-15-intel`.** GitHub has
  announced the end of Intel macOS runners for Fall 2027. Before then,
  move the job to an arm64 runner with Rosetta plus the universal iOS
  platform, or to Qt's arm64 simulator libraries if Qt ships them
  (Known Issue #29).

**Ignorable CI noise:** `Qt6TaskTree` and `QmlAssetDownloaderPrivate`
not-found warnings, and missing Vulkan headers. These come from
optional Quick plugins this app never uses.

### The C ABI check

`scripts/abi-check.sh` holds the C ABI to its promises
([ADR-0001](adr/0001-qt-free-core-c-abi.md)): the core at HEAD against
the newest `vX.Y.Z` tag, or a tag, commit or source folder named as
its argument. It runs on Linux, with libabigail (`abigail-tools` on
Ubuntu): each tree's core-only build has its static archive linked
into a shared object that exports what the shared core `ecuio` exports
([ADR-0053](adr/0053-python-bindings.md)'s appendix), as an FFI
consumer would load it: the functions the tree's public headers
declare, listed by `scripts/ecuio_exports.py` (none that `internal.h`
declares: core uses those across its own files and never installs
them); `abidw`
dumps the functions and the types the public headers declare, and
`abidiff` compares the two. Before the builds, `scripts/api_check.py`
compares the installed headers themselves (core/CMakeLists.txt's
`ECUIO_PUBLIC_HEADERS`), as declarations: comments and formatting never count.
Output goes to `build-abi/` (`report.txt`, and `api.txt` for the
headers); either half failing fails the check.

- **Fails:** a function removed or its signature changed; a struct
  field moved, resized or retyped; a struct's size changed; an enum
  value renumbered.
- **Passes, reported:** a new function or variable; an enum value
  appended; a struct growing into its reserved room, with its size the
  same and every field outside the room where it was.
  `scripts/abi_report.py` tells that growth from a break by reading the
  report, since a libabigail suppression of the type would hide every
  other change to the struct with it.
- **Fails in the headers, keeping the layout:** a struct member renamed
  or retyped at the same size (`uint32_t` becoming `int32_t`) or gone,
  outside the reserved room; a `#define` or an enumerator gone or with
  another value; a function's, a callback's or a typedef's types
  changed, or one gone; an installed header gone. None changes the
  binary, so libabigail sees none, but each changes what a consumer's
  source means. A parameter renamed passes.

A deliberate break (before 1.0, a minor version may make one) goes in
`abi/suppressions.abignore` (libabigail's) or `abi/api_changes.txt`
(the headers', one name a line) with its reason and its release, and
comes out once that release is tagged and becomes the base. The rules
were checked against mutants of the headers (2026-10-29; the headers'
half 2026-11-04, `ApiCheckTest.SelfTest` pins its rules): every break
above fails, and the passes pass.
