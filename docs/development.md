# Development guide

Building, testing and CI for contributors.

## Start here

**What you need**

| | Windows | Linux | macOS |
|---|---|---|---|
| Qt | 6.12+, kit `msvc2022_64` | 6.12+, kit `linux_gcc_64` | 6.12+, kit `clang_64` |
| Compiler | MSVC 2022+ (Visual Studio) | GCC 10+ | Xcode 16+ |
| CMake | 3.21+ | 3.21+ | 3.21+ |
| Also | Git Bash, for `scripts/verify.sh` | `libgl1-mesa-dev`, `libxkbcommon-dev` (Debian, Ubuntu) | |

GoogleTest is fetched on the first configure. Python 3 is optional (the
pinned clang-tidy wheel, aqtinstall, the Python tests); Qt isn't needed
for a core-only build. Android and iOS: [mobile-build.md](mobile-build.md).
Installing Qt with aqtinstall: [its section](#installing-qt-with-aqtinstall).

**Quickest first build, no Qt:** the core, `havne-cli` and their tests.

```sh
cmake -B build-core -DHAVNE_BUILD_CORE_ONLY=ON
cmake --build build-core --config Release
ctest --test-dir build-core -C Release --output-on-failure
```

**Windows, the app and the tests.** Build the Win32 tree first, so the
x86 J2534 surrogate exists when the app tree is configured and copies it
next to `havne.exe`:

```sh
cmake -B build-win32 -A Win32 -DHAVNE_BUILD_CORE_ONLY=ON
cmake --build build-win32 --config Release --target ecuio-j2534-surrogate fake_j2534 fake_j2534_legacy
cmake -B build -DCMAKE_PREFIX_PATH=<qt-install>/msvc2022_64
cmake --build build --config Release
build/app/Release/havne.exe
ctest --test-dir build -C Release --output-on-failure
```

Once both trees are configured, `scripts/verify.sh` (Git Bash) is the
everyday rebuild-and-test: it builds `build-win32`, then `build`, runs
the suite and the cross-arch subset, and prints one line per step
([Recommended verification sequence](#recommended-verification-sequence)).

**Linux and macOS, the app and the tests.**

```sh
cmake -B build -DCMAKE_PREFIX_PATH=/path/to/qt
cmake --build build
ctest --test-dir build -C Release --output-on-failure
build/app/havne                              # Linux
build/app/Havne.app/Contents/MacOS/Havne    # macOS
```

Showing the window under X11 also needs `libxkbcommon-x11-0` and
`libxcb-cursor0`. A source build on Windows needs a current Microsoft
Visual C++ Redistributable (x64). Linux users who only want to run
Havne: [linux.md](linux.md).

**The rest of this guide**

- Builds: [Build configurations](#build-configurations) (the desktop
  platforms and the surrogate's two trees), [Test fixtures](#test-fixtures-the-two-fake-vendors)
  and [mobile-build.md](mobile-build.md) (Android, iOS, the app icon).
- Tests: [Recommended verification sequence](#recommended-verification-sequence),
  [Running tests](#running-tests) (ctest, QML, cross-arch, fuzzing,
  coverage) and [testing.md](testing.md) for what each suite pins.
- Tools: [The command-line client](#the-command-line-client),
  [Everyday tools](#everyday-tools) (preferences, qmllint, warnings,
  formatting, clang-tidy) and [linux.md](linux.md) (SocketCAN, USDT
  tracing).
- When it breaks: [Troubleshooting](#troubleshooting) (with a crash
  dump on Windows) and [Portability traps](#portability-traps).
- CI and releases: [ci.md](ci.md) (the jobs, the workarounds, the C ABI
  check) and [packaging.md](packaging.md) (the Linux packages, the SBOM,
  releases, the Android release key).

## Toolchain

The table in Start here has Qt, the compiler and CMake. CMake 3.21+ is
required for `qt_standard_project_setup()`; the compiler minimums are
Qt 6.12's (a core-only build needs just C++17 and no Qt); the app needs
Qt's Core, Gui and Quick, under the LGPL. The rest:

| Component | Version | Notes |
|---|---|---|
| GoogleTest | 1.14.0 | Fetched automatically on the first configure if it isn't installed |
| Android NDK, JDK | NDK r27, JDK 21 | Android builds only. Qt 6.12's Android kit expects these versions. |
| Xcode | 16+ (iOS 18 SDK) | iOS builds only, on a macOS host |
| Python (optional) | 3.x, 64-bit | Installs the pinned clang-tidy wheel for `scripts/tidy.sh`, and aqtinstall if you install Qt that way |
| OpenCppCoverage (optional) | any | Windows line coverage, `scripts/coverage.sh` |

### Installing Qt with aqtinstall

Released aqtinstall (3.3.x) can't install Qt 6.11+: download.qt.io
changed its repository layout, and aqt requests a stale
`Updates.xml` path
([aqtinstall #959](https://github.com/miurahr/aqtinstall/issues/959)).
The fix ([aqtinstall #1000](https://github.com/miurahr/aqtinstall/pull/1000))
is not in a release yet, so install aqtinstall from git:

    python -m pip install --force-reinstall --no-cache-dir \
        "git+https://github.com/miurahr/aqtinstall.git"

    python -m aqt install-qt windows desktop 6.12.0 win64_msvc2022_64 -O <your-install-path>

The Qt online installer isn't affected.

Once a release carries the fix, pin it from PyPI instead, here and in
CI (see "Temporary workarounds" in [ci.md](ci.md)), and drop this
section.

## Build configurations

**Full app, Windows: two trees, the Win32 one first.** Every J2534
vendor DLL loads in a surrogate process of its own architecture
([ADR-0025](adr/0025-one-j2534-backend.md)). Most, the VX's among
them, are 32-bit, and their surrogate comes from a core-only Win32
tree, because Qt ships no 32-bit kits; the app tree copies it next to
`havne.exe`, beside its own x64 surrogate. The recipe is in
[Start here](#start-here): its first tree also builds the x86 fakes the
cross-arch tests load, and the app tree's build prints "Deploying x64
J2534 surrogate" and "Deploying Win32 J2534 surrogate" (windeployqt has
run post-build). The Visual Studio generator is multi-config, so
`--config` and `-C` are required. `scripts/verify.sh` (Git Bash) runs
both builds in this order, then the whole suite and the cross-arch
subset ([Running tests](#running-tests)).

**Reproducible Release builds.** `HAVNE_REPRODUCIBLE` (on by default, MSVC
Release) adds `/Brepro` and `/d1trimfile`, so two builds of one commit
match byte for byte ([ADR-0035](adr/0035-linux-downloads-sbom-reproducible.md)).
Turn it off only to compare against a build made without it.

**How the app tree finds the surrogates (automatic).** The x64 one
is the app tree's own `ecuio-j2534-surrogate` target
(`ecuio-j2534-surrogate-x64.exe`), built and copied with the app. For
the x86 one, with the two-tree layout there is nothing to configure:
when
`HAVNE_SURROGATE_SOURCE` is unset (the default), CMake searches the
sibling `build-win32/` tree and copies the surrogate next to
`havne.exe` on every app build. `HAVNE_SURROGATE_SOURCE` is the app
build's CMake setting, for a surrogate built somewhere else; the
runtime variable `ECUIO_SURROGATE_EXE` is separate, and points one run
at a surrogate (the cross-arch tests use it).

Resolution order:

1. `HAVNE_SURROGATE_SOURCE` cache variable, if set. A configured
   path that does not exist is a configure-time `FATAL_ERROR`, not a
   silent no-op.
2. Otherwise, `<repo>/build-win32/surrogate/*/ecuio-j2534-surrogate.exe`,
   `Release/` preferred over any other config.
3. Neither found (no `build-win32` tree, non-standard layout): a
   STATUS message, nothing copied. The app then resolves the
   surrogate through `ECUIO_SURROGATE_EXE` at runtime: see the
   cross-arch run below.

Build order matters, on every build: the Win32 tree **first**, then
configure or reconfigure the x64 tree so auto-detection finds the exe.
Detection runs only at configure time, so an x64 tree configured before
the Win32 surrogate existed has no copy step at all, and building it
again doesn't add one. Check the configure output for `Deploying Win32
J2534 surrogate: <path>`; if it isn't there, re-run `cmake -B build`
after building the Win32 tree. Once the copy step exists, the Visual
Studio generator runs it on every app build (`copy_if_different`), so
building the app before the Win32 tree deploys the previous surrogate.

**Cache caveat.** The cache variable
persists across reconfigures. A tree previously configured with an
explicit `-DHAVNE_SURROGATE_SOURCE=...` keeps that value and
disables auto-detection until it is cleared:

```sh
cmake -B build -U HAVNE_SURROGATE_SOURCE
```

Blowing away `CMakeCache.txt` is the blunt equivalent and equally
safe: regeneration is cheap.

**Full app, Linux and macOS.** The recipe is in [Start here](#start-here).
Single-config generators drop the `Release/` path segment, and on macOS
the app is a bundle.

On Debian or Ubuntu, Qt's desktop kit needs `libgl1-mesa-dev` and
`libxkbcommon-dev` to build (the `linux-build` CI job installs both).
To show the window under X11 you also need `libxkbcommon-x11-0` and
`libxcb-cursor0`. CI never launches the app, so it skips them.

**Running a source build.** On Windows it needs a current Microsoft
Visual C++ Redistributable (x64), at least as new as the MSVC that
built the app and Qt: Visual Studio installs one, and windeployqt
doesn't copy it (the release zip carries its own). Linux and macOS
need the Qt runtime libraries. A J2534 vendor driver is needed only
for a real adapter; the tests use the in-tree fake.

**Core only, no Qt.** The first recipe in [Start here](#start-here); it
mirrors the Ubuntu CI job.

**The cross-arch tree** is the Win32 tree of the Windows recipe
in Start here: besides the surrogate it builds the x86 fake DLLs that test
the x64 to x86 path. Only the surrogate and vendor DLLs are ever x86.

> **Note:** **Rebuild both trees** after any change to `j2534_ptypes.h` or
> `surrogate_protocol.h`. Their constants are ABI between binaries
> built in separate trees. Each tree's configure step hashes the two
> headers into `ECUIO_SUR_FINGERPRINT`, and the surrogate refuses an
> `OP_OPEN` whose fingerprint isn't its own, so a stale tree fails at
> connect: the session log says "built from other shared headers".
> Other sources that compile into both trees, the surrogate's and the
> fakes', carry no fingerprint, and the cross-arch run loads them
> from `build-win32` directly, so rebuild both after those too.

**Mobile builds and the app icon:** Android, iOS (device and Simulator) and
`make_icons.py` are in [mobile-build.md](mobile-build.md).

## Test fixtures: the two fake vendors

Both fakes compile the same source and differ only in their `.def`
export surface. Together they pin the loader's resolution of the ioctl
export, which the spec spells `PassThruIoctl` and some vendors ship as
`PassThruIoCtl` ([engineering log](engineering-log.md), "Lessons
learned"):

| Target | Exports | Pins |
|---|---|---|
| `fake_j2534` | `PassThruIoctl` (spec spelling, directly) | Primary resolution: the conformant-vendor case; exercised by every default test |
| `fake_j2534_legacy` | `PassThruIoCtl` (legacy alias only) | The fallback resolver: what some real vendors still ship |

Do not add `__declspec(dllexport)` back to the fake's vendor
functions: the `.def` files own the export surface, and a stray
dllexport collapses the two fixtures back into one (the spec test's
negative assertion will catch it, but best not to trigger it).

## Recommended verification sequence

`scripts/verify.sh` (Git Bash on Windows; off Windows it builds `build`
and runs ctest alone) rebuilds, runs the tests and prints one line per
step. The trees must already be configured (Start here has the first
configure):

```sh
scripts/verify.sh                         # both trees, full suite, cross-arch
scripts/verify.sh --tests-only -R '^BridgeTest\.'   # a subset, no build
scripts/verify.sh --build-only            # both trees, no tests
scripts/verify.sh --suite Bridge,Qml      # named suites, after the build
scripts/verify.sh --changed               # the steps this branch's changes need
scripts/verify.sh --changed --dry-run     # the plan, nothing run
scripts/verify.sh --cross-only            # win32, build and cross-arch alone
scripts/verify.sh --no-cross              # without the cross-arch step
```

```
win32       ok      <s>  <n> warnings
build       ok      <s>  <n> warnings
ctest       ok      <s>  <passed>/<total> passed, <n> skipped
cross-arch  ok      <s>  <passed>/<total> passed, <n> skipped
```

The times and counts vary with the tree and the machine's load.
`--cross-only` is the half of a branch's verify that CI never runs: it
needs Windows and a configured `build-win32`, takes only `--no-devices`
(which changes nothing there, as no device is read) and `--dry-run`, and
writes no stamp.

Full output goes to `Testing/verify/<step>.log` (git-ignored). On a
failure it prints the compiler errors or the failed test names and
stops. It fails, rather than passing, when `-R` matches no test, and
when the env-gated routing case skips in the cross-arch step, since
that means the x86 paths are wrong. The cross-arch line names each
case that skipped: any other skip is reported, not failed (the
surrogate's session-log case skips while another checkout's run holds
its fixed log name). The Win32 step also builds the x86 `test_signals`,
and the cross-arch step runs its `ComputedTest` in `build-win32`: the
computed machine's results bit for bit under the x86 compiler. Where
`build-win32` has the shared core (`HAVNE_SHARED_CORE`, on by default),
the Win32 step also builds the x86 `ecuio.dll` and `test_c_abi_shared`,
and the cross-arch step adds `c_abi_shared` and
`ExportsTest.TheLibraryExportsTheDeclarations`, since CI never builds
the x86 DLL. Its line adds `x86 core N/N`, the cases it ran in
`build-win32`. Off Windows, or without
`build-win32`, the Win32 and cross-arch steps are left out.

`--suite` and `--changed` are for the edit-and-check loop; a pull request
needs the plain run. `--suite` takes suite names (`Bridge` for
`BridgeTest.*`, and a single test such as `c_abi` by its whole name) and
fails on a name that matches no test. `--changed` compares the working
tree, untracked files included, with its merge-base with `origin/dev`
(or a base named after it) and goes by `scripts/verify_scope.py`'s table:

| What changed | What runs |
|---|---|
| Only `docs/` and markdown | Nothing |
| Only `.qml` files in `app/qml/` and `tests/qml/`, with or without docs | `build`, then `QmlTest.*` |
| Anything else, a build file among them, or a change it can't read | The full run |

Any run less than the full one lists what it skipped, says whether
clang-tidy applies (it doesn't when no C++ or build file changed) and
ends with a `not a full verify` line, a failed run too. A full run that
ran every step writes `Testing/verify/full-run.stamp` with HEAD's SHA,
whether the tree stayed clean, the date and the main test step's
passed, skipped and total counts; a run that fails removes the stamp.

For any change touching the surrogate, the fakes, or the J2534
client/transport: in this order:

1. **Rebuild both trees, Win32 first.** The cross-arch run consumes
   `build-win32` artifacts directly, and the app build copies
   whatever surrogate `build-win32` holds at that moment.
2. **Full suite, bare shell** (no env vars: the cross-arch case
   must *skip*, visibly, never silently fall back).
3. **Cross-arch run** with the variables exported (below).
4. **Session-log check** when tracing changed. The log is
   `%TEMP%\ecuio-surrogate.log`; each spawn keeps the one it replaces
   as `ecuio-surrogate.<YYYYMMDD-HHMMSS-mmm>.log`, the newest 50 of
   them, so a suite run pushes out older ones. It holds the child's lines
   (`[surrogate HH:MM:SS.mmm]`) and the client's (`[sur ...]`) in time
   order. A session started while another holds that file logs to
   `ecuio-surrogate-<pid>-<n>.log` instead ([surrogate.md](surrogate.md)).
   - The child logs one line per vendor call with its status:
     `OP_OPEN`, `OP_CONNECT`, `OP_DISCONNECT`, `OP_WRITE`,
     `OP_IOCTL`, `OP_START_FILTER` and `OP_STOP_FILTER`. Grep for the
     op you changed.
   - After a clean close, `OP_DISCONNECT: vendor status 0` should
     appear exactly once.
   - Tests can read the log through `ecuio_j2534_sur_log_path()`, as
     `ClientAndChildLinesShareTheSessionLog` does. A new trace line is
     still worth reading by eye once.
5. If the app is affected, rebuild the `havne` target (after the
   Win32 tree) so the deployment copy picks up the new surrogate.

## Running tests

```sh
# everything
ctest --test-dir build -C Release --output-on-failure
# one suite (anchor the regex)
ctest --test-dir build -C Release -R '^BridgeTest\.'
build/tests/Release/test_core.exe --gtest_filter='CoreTest.SelfUnregister*'
# specific suites
ctest --test-dir build -C Release -R "J2534Routing|J2534Surrogate|J2534Adoption|J2534HealthTest|J2534ConnectTest" --output-on-failure
```

`gtest_discover_tests` registers each case with ctest as
`Suite.Case`. Anchor `-R` on the suite name, because `-R Signal` also
matches `BridgeTest.SignalBatchesAreCoalesced`. The suites and what
each one covers are listed in [testing.md](testing.md).

**QML tests** (`test_qml`, Qt Quick Test) load the app's QML module,
the `app_ui` library, against a real bridge. Each
`tests/qml/tst_*.qml` file is one ctest case, `QmlTest.<name>`, in a
process of its own, except the slowest (`tst_dashboard.qml`,
`tst_driveview.qml`, `tst_phone.qml`), which run as
several cases each, `QmlTest.<name>-1of<n>` to `-<n>of<n>`, every nth
case (three, five for `tst_phone.qml`);
each carries its measured seconds as a ctest `COST`, so a tree with no
cost history starts the long ones first. A new file is picked up at the
next configure.

```sh
ctest --test-dir build -C Release -R '^QmlTest\.' --output-on-failure
# one file, or one function in it, by hand (from the repo root)
build/tests/qmltest/Release/test_qml.exe -input tests/qml/tst_linkhealth.qml
build/tests/qmltest/Release/test_qml.exe -input tests/qml/tst_linkhealth.qml LinkHealth::test_requests
```

- ctest runs them offscreen on the software renderer
  (`QT_QPA_PLATFORM=offscreen`, `QT_QUICK_BACKEND=software`, and on
  Windows `QT_QPA_FONTDIR` pointing at the system fonts, which the
  offscreen platform can't otherwise find), so no window opens. Run by
  hand without those variables, the windows show.
- On Windows, `test_qml` builds into a directory of its own and is
  `windeployqt`ed there with the offscreen platform plugin, so it runs
  from a shell. The other test binaries' directory holds a copied
  `Qt6Core.dll`, which would point Qt at plugin and QML directories
  that aren't there.
- The harness sets `QT_FORCE_STDERR_LOGGING=1` unless it's already set.
  Without a console (ctest, a pipe, Git Bash), Qt on Windows sends its
  warnings and the test log to the debugger, and a run prints nothing.
- The link-state cases drive the fake through the x64 surrogate
  copied next to `test_qml`, arming its knobs in the environment
  through the harness's `Harness.armFakeKnob` before the connect
  spawns the child; they skip off Windows.
- The page tests fail on any warning from the app's own QML
  (`qrc:/qt/qml/App/…`), but not on Qt's.

**J2534 suites carry a 30 s per-test timeout.** The slowest
legitimate case (the cross-arch timeout-recovery test, ~3.5 s) has
near-10x headroom, so a slow CI machine cannot false-alarm, but a
hang (the IPC deadlock class caught 2026-10-17) fails bounded as
`Failed (Timeout)` instead of wedging the runner.

**Discovery against the machine's drivers.** The discovery test reads
the live registry. With `ECUIO_EXPECT_J2534_DEVICES=<n>` set it fails
unless at least that many devices are found; `scripts/verify.sh` sets
it to 1 on Windows, because the dev machine has a Pass-Thru driver
installed, and takes `--no-devices` on a machine without one. CI's
`win-build` job has no vendors and leaves it unset, so there the test
checks only the shape of an empty result; `win-coverage` registers the
fake DLL as a device first, and `scripts/coverage.sh` demands it.

**Cross-arch run.** Run this from the x64 tree, after building
`build-win32`:

```sh
export ECUIO_SURROGATE_EXE='<repo>/build-win32/surrogate/Release/ecuio-j2534-surrogate.exe'
export ECUIO_FAKE_DLL='<repo>/build-win32/tests/Release/fake_j2534.dll'
ctest --test-dir build -C Release -R "J2534Routing|J2534Surrogate|J2534(Health|Connect|Talk|Car)Test\." --output-on-failure
```

Use Windows-style paths in these variables (forward slashes are
fine). MSYS translates paths in command arguments but not in
environment-variable values, so `/e/...` reaches Windows verbatim and
the open fails. Without these variables,
`J2534RoutingTest.X86FakeSelectsSurrogateBackendAndRoundTrips`
skips.

Every runtime variable carries the core's prefix, `ECUIO_`: these two,
`ECUIO_SURROGATE_X64_EXE` (the x64 surrogate's, which no run needs:
the tests find it next to themselves), the fake's knobs and the
surrogate's. The one `HAVNE_` name is
`HAVNE_SURROGATE_SOURCE`, the app build's CMake cache setting above,
which is read at configure time, never at runtime. A variable under
any other name is ignored without a word: the routing case skips, the
other surrogate cases run against the x64 tree's surrogate and pass,
and the run looks green. Read the skip count, or use
`scripts/verify.sh`, whose cross-arch step fails when that case skips.

**Two ways an env-gated test lies to you** (both hit during the
2026-10-16 bench pre-flight, both operator error):

- A var set to the **wrong value** points the test at the wrong
  arch or a nonexistent path: the failure looks like a routing
  regression (the x64 surrogate where the x86 one belongs) or a fast
  `ECUIO_ERR_LOAD_FAILED` or `ECUIO_ERR_SURROGATE_FAILED`. Echo the
  variable before diagnosing code.
- Each spawn takes `%TEMP%\ecuio-surrogate.log` and keeps the log it
  replaces under a dated name, the newest 50 of them. A suite run
  spawns dozens, so copy a log that's evidence before running the
  tests, and check its stamps: the fixed name always holds the latest
  spawn, which may not be the run you mean. An `elm327://` connect's
  wire log, `%TEMP%\ecuio-elm327.<stamp>.log`, keeps the newest 50 the
  same way; the ELM327 tests write theirs to a directory of their own,
  so a run never pushes those out.

**Fuzzing** isn't part of ctest. The libFuzzer harnesses need a
tree of their own (`-DECUIO_BUILD_FUZZERS=ON` with
`HAVNE_BUILD_CORE_ONLY=ON` and `BUILD_TESTING=OFF`; `build-fuzz-msvc`
on Windows), because nothing else can link the instrumented core.
[fuzz/README.md](../fuzz/README.md) has the MSVC and Clang recipes and
how to replay a crash.

**Coverage** (Windows, [OpenCppCoverage](https://github.com/OpenCppCoverage/OpenCppCoverage)):
`scripts/coverage.sh` configures `build-coverage` on first use
(RelWithDebInfo with `/Od /Ob0`: PDBs, no inlining, release Qt), builds
it, and runs the whole suite under the tool with `--cover_children`,
so the x64 surrogate child counts too. It prints the line rate for
`core/`, `surrogate/` and `app/`, merged across the test binaries, and
writes `Testing/coverage/html/index.html` and
`Testing/coverage/cobertura.xml`. Native code only: QML, the x86
surrogate, the tests and the fake DLLs aren't counted, and `app/main.cpp`
isn't linked into any test, so it isn't in the report. `--tests-only`
and `-R` work as in `verify.sh`. Two cases are left out of its ctest
part, `CliSessionRunTest.TheStreamNamesEachModulesSession` and
`CliSignalTest.CtrlBreakEndsTheStreamCleanly`, with the reason in
`coverage.sh`: they fail under the tool on CI's runners and pass
natively. CI's `win-coverage` job runs the same script on pushes to
`main` and on manual runs, as three legs on a runner each (`--part
gtest`, `1/2` and `2/2`), and each leg uploads its Cobertura file to
Codecov for the README badge ([testing.md](testing.md#coverage)).
Codecov waits for all three uploads before it reports
(`.github/codecov.yml`'s `after_n_builds: 3`), so a partial upload never
shows a drop in coverage that isn't real. The cost: a leg that fails
before its upload leaves the commit with no coverage status until the
whole run is repeated, and a leg run again alone counts as a fourth
upload.

**The tests' Python.** The Python tests (`PythonTest.*`, `SchemaFixturesTest`,
the CLI's script tests) run under the Python the configure found, so
give it one with pytest and jsonschema (`-DPython3_EXECUTABLE=<venv
python>`; CI installs both from `.github/*-requirements.txt` with
`--require-hashes`). A 32-bit Python 3.11 or later with pytest, given to
`build-win32` the same way, has `scripts/verify.sh`'s cross-arch step run
the package's tests against the x86 core.

## The command-line client

`havne-cli` ([ADR-0031](adr/0031-command-line-client.md)) builds in
every desktop tree, the core-only one included: `build-core/cli/Release/`
on Windows, `build-core/cli/` with a single-config generator. The app
build copies it next to `havne.exe`.

```sh
havne-cli stream mock://demo --values --count 20          # the simulated car
havne-cli stream "replay://<folder>?profile=obd2&speed=max" --values > run.jsonl
havne-cli stream elm327://COM5 --seconds 60               # an adapter: as the app would
havne-cli stream "elm327://COM5?profile=uds-generic&did=1000-13FF&scan=first" \
  --until-scanned --seconds 600                           # one block of a DID sweep
havne-cli devices                                         # what a URI can name
havne-cli vin <VIN>                                       # a VIN's year, make and check digit, offline
havne-cli dtc P0301                                       # a trouble code's title, offline
havne-cli sim mock://sim --elm327 tcp:35000               # a simulated car as a WiFi adapter
havne-cli clear mock://demo --yes                         # clear each ECU's codes, engine stopped
havne-cli health mock://demo report.txt --format text     # a car's health as one file
havne-cli export <recording> run.pcapng --format pcapng   # its frames for Wireshark, scrubbed
havne-cli import candump <log> <folder>                   # another tool's log as a recording
havne-cli layout check dashboard-phone.json               # a Dashboard layout file, as the app would load it
```

`havne-cli layout check <file> [--form phone|desktop]` reads a dashboard
layout file as the app's Load layout would and says what it is: one
`layout` line. `ok` is true when the file reads (a file with pages or
gauges that cannot be read still reads, with `problem` "dropped" and the
counts in `dropped`); a refused file has `problem` "damaged", "newer",
"otherform" or "none" and `words`. `needs_files` lists the signal files
the layout names (name and sha256). Without `--form` the file's own form
is used (desktop, with a note, for a file that names none). Exit 0 when it
reads, 1 when it is refused, 2 for a usage error, 3 when the file cannot
be read, is over a layout file's size or is not UTF-8 text
([ADR-0057](adr/0057-dashboards.md)'s appendix on the layout file).

An adapter only the phone reaches: the app relays it to a computer on
its USB cable ([ADR-0047](adr/0047-adapter-relay.md); Settings, Connection,
the adapter's row, then **Relay**, in developer mode), and havne-cli
connects as to a WiFi adapter:

```sh
adb forward tcp:35000 tcp:35000
havne-cli stream "elm327://127.0.0.1:35000?poll=0x0C" --seconds 30
```

Its output is JSON lines, schema 1 (ADR-0031 section 3); a `slept` line marks
a gap the machine's sleep made (ADR-0033 section 6). Its tests are
`test_cli` (the formatting, the ring and the sleep rule) and ctest's
`Cli*` suites, which run it as a child: `CliGoldenTest`, `CliUsageTest`
and `CliSignalTest`, and a run suite per command (`CliReplayTest`,
`CliRecordTest`, `CliViewRunTest`, `CliTransferRunTest`,
`CliHealthRunTest`, `CliClearRunTest`, `CliElm327RunTest` and more). A golden run compares the whole
output with `tests/golden/cli/`; after a deliberate change to what it
prints (or to what core decodes), rewrite the golden file with the
test's own command plus `--update`, then read the diff before
committing it:

```sh
ctest --test-dir build-core -C Release -R '^CliGoldenTest\.' -V   # shows each command
python tests/golden.py run --golden tests/golden/cli/mock.jsonl --ignore core --ignore t_us \
  --update -- "$(pwd -W)/build-core/cli/Release/havne-cli.exe" stream mock://golden --values --count 40
```

On Windows, give `golden.py` the executable's absolute path:
Python's `CreateProcess` doesn't resolve `build-core/cli/...`.


## Everyday tools

**Preferences:** the app keeps the zoom, the units, the last URI that
connected and the Record sessions switch with QML `Settings`; on
Windows that's `HKCU\Software\Havne\Havne`, which every Havne on
the machine shares. `HAVNE_SETTINGS_DIR=<folder>` keeps one
instance's preferences in an ini file under that folder instead, so a
scripted run neither reads nor changes the user's; `test_qml` keeps
its own in a scratch folder the same way.

The preferences also keep the look, the last address that connected,
the splitters' shares and each device's versions as last seen at a
connect (shown on its row in Settings, since the list opens no
adapter). The vehicles are one JSON document among them.

**Live's chart:** each trace's color is keyed to its list row, so the
row's swatch is the legend and a trace keeps its color when another
signal is unsubscribed. The ticks choose what's plotted and nothing
else. Each trace scales on its own, so its value is printed at its
right-hand end; its range is at least a few displayed digits, so a
one-count step stays small, and a gap longer than the stale threshold
breaks the trace instead of drawing a ramp. The layout is arithmetic in
the `ChartLayout` singleton, which the tests run with synthetic series;
the Canvas only strokes what it returns. A Canvas paints an image at its
own size, so this one is sized up by the zoom and scaled back down,
which keeps it as sharp as the rest.

**QML lint:** run qmllint over the rsp the app build writes, alone:
`C:/Qt/6.12.0/msvc2022_64/bin/qmllint.exe @build/app/.rcc/qmllint/app_ui.rsp --json lint.json`
(it lists every file of the module by its absolute path; build the
whole app first, so the module's types file exists). The JSON names
every file linted, warned or not, and holds warnings the text output
leaves out. Don't name a file after the rsp: qmllint 6.12.0 crashes on
a file it gets twice under two spellings (`app/qml/X.qml` beside the
rsp's own). The `all_qmllint` target skips the lint while its stamp is
current, and then prints nothing because nothing ran. Warnings don't
fail the build. The bridge is the registered `Bridge` singleton, so
qmllint checks every `Bridge.*` binding; the baseline is no warnings,
so any warning is new.

**Compiler warnings:** every first-party target builds with `/W4`
(MSVC) or `-Wall -Wextra` (GCC, Clang) through
`app_enable_warnings()` in the root `CMakeLists.txt`; `test_c_abi`
adds `-pedantic` with compiler extensions off. CI's workflow sets
`HAVNE_WARNINGS_AS_ERRORS=ON` in the environment, the CMake option's
default, so a GCC or Clang warning in first-party code fails the job
(`-Werror` on those targets only; GoogleTest and Qt never). Locally it's
off unless the variable is set (`export HAVNE_WARNINGS_AS_ERRORS=ON`
before the first configure, or `-DHAVNE_WARNINGS_AS_ERRORS=ON`). A
warning that is wrong for one line gets a pragma there with the reason,
never a flag for the whole tree. MSVC isn't gated: `scripts/verify.sh`
prints the count per build step; keep it at 0.

**Formatting:** `.clang-format` (clang-format 13 or later) describes
the house style. The tree wasn't reformatted when it was added, so
format only the lines you change: `git clang-format` after staging.

**clang-tidy:** `.clang-tidy` runs the static analyzer and the
bug-finding families (bugprone, concurrency, performance, portability,
a few misc checks) over `core/`, `surrogate/`, `cli/` and `app/`, and
CI's `clang-tidy` job fails on any warning. The style families stay off
but for three checks: `google-explicit-constructor`, braces around a
body on the line after its `if`, `else`, `for` or `while`, and
`google-readability-casting` (no C-style casts in C++; the headers that
compile as C keep theirs behind a `NOLINTBEGIN`). The file says why each
is on and why each excluded check is excluded; `app/.clang-tidy` lets the
app's headers keep `#pragma once`. To run it
locally, install the pinned version once into `build-tidy-venv/` (it
sits with the build trees, so git ignores it), then run
`scripts/tidy.sh` from any shell:

```sh
python -m venv build-tidy-venv
build-tidy-venv/Scripts/pip install --require-hashes -r .github/clang-tidy-requirements.txt
scripts/tidy.sh                  # one line; each file's output in Testing/tidy/
scripts/tidy.sh --qt C:/Qt/6.12.0/msvc2022_64   # app/ too, in build-tidy-qt
```

A function past 80 lines or cognitive complexity 40 fails the run unless
`scripts/tidy-size-baseline.txt` lists it (one `file<TAB>function` line), and
a listed line that no longer applies fails too, so the list only shrinks:
`scripts/tidy.sh --update-size-baseline` drops those lines, and a line is
added by hand only. CI's three shards each judge their own sources.

On Windows outside a Developer prompt, the script enters one (vswhere's
newest VC tools), so clang-tidy and the tree see one MSVC's headers, and
it configures `build-tidy` (Ninja: the Visual Studio generator writes no
compile database) when the tree is missing. Nothing needs building:
with `--qt`, the tree is the whole project against that kit, and the
script generates only the app's moc output before clang-tidy reads it.
The app's Android-only sources aren't in a desktop tree, so they aren't
checked.
Another clang-tidy version finds other things, so the script refuses one
that isn't the pin (an LLVM on PATH, say) unless `CLANG_TIDY_ANY=1`, for
trying a new pin; `$CLANG_TIDY` names another binary. A warning that is wrong at one line
gets a `NOLINTNEXTLINE(<check>)` with the reason beside it.

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| `QmlCacheGeneratedCode` unresolved externals after adding a QML file | Stale QML cache. Do a clean reconfigure (`rm -rf build`). |
| On Windows, every build recompiles the whole QML cache (about a minute) although nothing changed | After an AUTOMOC rerun that leaves the moc list as it was, Qt's metatype step leaves `build/app/meta_types/app_ui_json_file_list.txt` older than `app_ui_autogen/timestamp_<config>`, so the step reruns on every build. Touch the list (`touch build/app/meta_types/app_ui_json_file_list.txt`); the next build is a no-op. |
| A `QmlTest` case fails with "Received a warning that resulted in a failure" | A binding in the app's QML raised a warning: the message names the file and line under `qrc:/qt/qml/App/`. Fix the binding; a common cause is a delegate reading its `index` (-1) while the model resets. |
| `test_qml` exits at once and prints that a QML type isn't available, or `module "App" is not installed` | The module's plugin isn't linked or imported. Whatever links `app_ui` also links `app_uiplugin` and has `Q_IMPORT_QML_PLUGIN(AppPlugin)` in one source file, as `app/main.cpp` and `tests/test_qml.cpp` do. |
| Test discovery fails at build time with `0xc0000135` | A DLL is missing. Discovery runs the test binary during the build, so Qt-linked test targets must copy `$<TARGET_RUNTIME_DLLS>` POST_BUILD, ordered before `gtest_discover_tests`. |
| Surrogate test fails with child exit code 5 | An export can't be found by name, usually because of x86 `__stdcall` decoration. Check the `.def` file. Other exit codes are listed in [surrogate.md](surrogate.md). |
| Surrogate opens fail, in tests or the app, and the session log says "built from other shared headers" | One tree was built from older copies of `j2534_ptypes.h` or `surrogate_protocol.h`, usually a stale `build-win32`. Rebuild both trees, Win32 first. |
| Cross-arch test fails with odd values, but same-arch passes | A stale `build-win32` fake: the fakes carry no build fingerprint. Rebuild both trees. |
| In the app, an x86 adapter (for example the OBDX Pro VX) fails with "surrogate process failed: no surrogate (ecuio-j2534-surrogate.exe)", and `%TEMP%\ecuio-surrogate.log` holds an `open: no surrogate exe` line | No surrogate next to `havne.exe`, so the child never launched. The x64 tree was configured before `build-win32` had a surrogate. Build the Win32 tree, re-run `cmake -B build`, check for `Deploying Win32 J2534 surrogate` in the output, and rebuild. Setting `ECUIO_SURROGATE_EXE` also works. |
| The cross-arch routing case skips although the variables were exported | They're named something else, for example `HAVNE_SURROGATE_EXE`: nothing reads that. The names are `ECUIO_SURROGATE_EXE` and `ECUIO_FAKE_DLL` (Cross-arch run, above). The rest of the run passes on the x64 surrogate, so the skip is the only sign. |
| Cross-arch test fails expecting an x86 DLL, or the session log names `ecuio-j2534-surrogate-x64.exe` | `ECUIO_FAKE_DLL` points at an x64 fake (or the wrong tree). Check the value, not the code: unset skips, wrong-value fails. |
| In the app or a test, an x64 DLL fails with "no surrogate (ecuio-j2534-surrogate-x64.exe): none next to the app" | The x64 surrogate wasn't copied: the app and each J2534 test copy it from their own tree POST_BUILD (`app_deploy_surrogate` in `tests/CMakeLists.txt`). Build the target again. |
| J2534 test fails as `Failed (Timeout)` | A genuine hang, bounded by the 30 s ceiling. Attach to the surviving test and surrogate processes and check thread wait states before killing; the child log is `%TEMP%\ecuio-surrogate.log`, and earlier spawns' logs are kept beside it under dated names. |
| Linker warning LNK4070 on a fake DLL | The `.def` file's `LIBRARY` directive disagrees with the output filename. Make them match. |

Mobile build and iOS Simulator smoke problems: [mobile-build.md](mobile-build.md#troubleshooting).

### A crash dump on Windows


[ADR-0034](adr/0034-debug-symbols-and-probes.md). Every MSVC Release
build writes a PDB beside each binary (`/Zi`, `/DEBUG`, with `/OPT:REF`
and `/OPT:ICF` kept), and each release publishes the shipped
executables' PDBs as `havne-X.Y.Z-windows-x64-symbols.zip`.

A crash dump: Windows keeps one when asked, per program,
in the registry (as administrator):

```bat
reg add "HKLM\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\havne.exe" /v DumpType /t REG_DWORD /d 2
```

The next crash leaves a full dump in `%LOCALAPPDATA%\CrashDumps`. Open
it in WinDbg or Visual Studio with the release's unzipped symbols on the
symbol path (`.sympath+ <folder>`, then `!analyze -v`); the PDB must be
the same release's, as the debugger checks.

On Linux, core's USDT probes: [linux.md](linux.md#probes-in-a-build-tree).

## Portability traps

CI builds core with GCC, MSVC and AppleClang, and cross-compiles it
with the Android NDK's Clang and for iOS, so code that compiles cleanly
on MSVC isn't necessarily portable. Three traps have bitten:
designated initializers are C++20 (MSVC accepts them in C++17 mode, GCC
rejects them; use constructor helpers such as `mk()` in
`signal_sets.cpp`); `strncpy_s` and the rest of Annex K are MSVC-only
(use a bounded copy such as `copy_str()`); and an implicit int to enum
conversion fails on MSVC with C2664.
