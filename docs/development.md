# Development guide

Building, testing and CI for contributors. For the short build
instructions, see the [README](../README.md#build).

## Toolchain

| Component | Version | Notes |
|---|---|---|
| CMake | 3.21+ | Required for `qt_standard_project_setup()` |
| Qt | 6.12+ (LGPL) | Core, Gui and Quick. Not needed for core-only builds. |
| C++ compiler | C++17 | MSVC 2022+, GCC 10+, Xcode 16+: Qt 6.12's minimums. A core-only build needs just C++17. |
| GoogleTest | 1.14.0 | Fetched automatically on the first configure if it isn't installed |
| Android NDK, JDK | NDK r27, JDK 21 | Android builds only. Qt 6.12's Android kit expects these versions. |
| Xcode | 16+ (iOS 18 SDK) | iOS builds only, on a macOS host |
| Python (optional) | 3.13+, 64-bit | Reserved for future trace-analysis tooling |

## Installing Qt with aqtinstall

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
CI (see "Temporary workarounds" under CI below), and drop this
section.

## Build configurations

**Full app, Windows.** The Visual Studio generator is multi-config,
so `--config` and `-C` are required:

```sh
cmake -B build -DCMAKE_PREFIX_PATH=<qt-install>/msvc2022_64
cmake --build build --config Release
# windeployqt runs post-build
build/app/Release/qttune.exe
```

**Deploy the Win32 surrogate next to the app (automatic).** With the
two-tree layout there is nothing to configure: when
`QTTUNE_SURROGATE_SOURCE` is unset (the default), CMake searches the
sibling `build-win32/` tree and copies the surrogate next to
`qttune.exe` on every app build.

Resolution order:

1. `QTTUNE_SURROGATE_SOURCE` cache variable, if set. A configured
   path that does not exist is a configure-time `FATAL_ERROR`, not a
   silent no-op.
2. Otherwise, `<repo>/build-win32/surrogate/*/ecuio-j2534-surrogate.exe`,
   `Release/` preferred over any other config.
3. Neither found (no `build-win32` tree, non-standard layout): a
   STATUS message, nothing copied. The app then resolves the
   surrogate through `ECUIO_SURROGATE_EXE` at runtime — see the
   cross-arch run below.

Build order matters: build the Win32 tree **first**, then configure
or reconfigure the x64 tree so auto-detection finds the exe.
Detection runs only at configure time, so an x64 tree configured
before the Win32 surrogate existed has no copy step at all, and
building it again doesn't add one. Check the configure output for
`Deploying Win32 J2534 surrogate: <path>`. If it isn't there, re-run
`cmake -B build` after building the Win32 tree.

Once the copy step exists, the Visual Studio generator runs it on
every app build (`copy_if_different`). Keep the order on every
rebuild too: Win32 tree, then app. Building the app first deploys the
previous surrogate.

**Cache caveat.** The cache variable
persists across reconfigures. A tree previously configured with an
explicit `-DQTTUNE_SURROGATE_SOURCE=...` keeps that value and
disables auto-detection until it is cleared:

```sh
cmake -B build -U QTTUNE_SURROGATE_SOURCE
```

Blowing away `CMakeCache.txt` is the blunt equivalent and equally
safe — regeneration is cheap.

**Full app, Linux and macOS.** Single-config generators drop the
`Release/` path segment. On macOS the app is a bundle:

```sh
cmake -B build -DCMAKE_PREFIX_PATH=/path/to/qt
cmake --build build
build/app/qttune                              # Linux
build/app/qttune.app/Contents/MacOS/qttune    # macOS
```

On Debian or Ubuntu, Qt's desktop kit needs `libgl1-mesa-dev` and
`libxkbcommon-dev` to build (the `linux-build` CI job installs both).
To show the window under X11 you also need `libxkbcommon-x11-0` and
`libxcb-cursor0`. CI never launches the app, so it skips them.

**Core only, no Qt.** This mirrors the Ubuntu CI job:

```sh
cmake -B build-core -DQTTUNE_BUILD_CORE_ONLY=ON
cmake --build build-core --config Release
```

**Cross-arch tree, Windows dev machines.** This builds the x86
surrogate and fake DLLs that test the x64 → x86 path
([ADR-0008](adr/0008-x86-surrogate-host.md)). It is core-only because
modern Qt ships no 32-bit kits, and only the surrogate and vendor
DLLs are ever x86:

```sh
cmake -B build-win32 -A Win32 -DQTTUNE_BUILD_CORE_ONLY=ON
cmake --build build-win32 --config Release --target ecuio-j2534-surrogate fake_j2534 fake_j2534_legacy
```

> ⚠️ **Rebuild both trees** after any change to `j2534_ptypes.h` or
> `surrogate_protocol.h`. Their constants are ABI between binaries
> built in separate trees. Each tree's configure step hashes the two
> headers into `ECUIO_SUR_FINGERPRINT`, and the surrogate refuses an
> `OP_OPEN` whose fingerprint isn't its own, so a stale tree fails at
> connect: the session log says "built from other shared headers".
> Other sources that compile into both trees, the surrogate's and the
> fakes', carry no fingerprint, and the cross-arch run loads them
> from `build-win32` directly, so rebuild both after those too.

**Mobile (builds in CI, not yet run on a device).** The QML, bridge
and core are shared, and only the kit differs. Use `qt-cmake` from
the mobile kit, which applies Qt's toolchain file and finds the host
desktop Qt. Mobile builds set `BUILD_TESTING=OFF`:
`gtest_discover_tests` runs each test binary at build time, and the
build host can't execute an Android or iOS binary. Off Windows the
only transport is `mock://` (Known Issue #2). Qt Creator with an
Android or iOS kit is the interactive alternative.

*Android* (this mirrors the `android-build` CI job). On Windows, add
`-G Ninja`, because the Visual Studio generator can't target Android:

```sh
<qt-install>/android_arm64_v8a/bin/qt-cmake -B build-android \
    -DBUILD_TESTING=OFF \
    -DANDROID_SDK_ROOT=<sdk> -DANDROID_NDK_ROOT=<sdk>/ndk/<27.x>
cmake --build build-android
cmake --build build-android --target apk
```

For an emulator on an x86-64 PC, use the `android_x86_64` kit. Since
Qt 6.7, Qt publishes Android kits under the `all_os` host (the name
aqtinstall and install-qt-action use). They need the build host's
desktop Qt, and `--autodesktop` installs it. A custom
manifest or icons go in `app/android/`. CMake sets
`QT_ANDROID_PACKAGE_SOURCE_DIR` only when that directory exists, so
`androiddeployqt` is never pointed at a missing template. No template
exists yet.

*iOS, device* (macOS host; this mirrors `ios-build`). `qt-cmake` also
applies Qt's minimum deployment target, iOS 18 for Qt 6.12:

```sh
<qt-install>/ios/bin/qt-cmake -B build-ios -G Xcode \
    -DBUILD_TESTING=OFF -DCMAKE_OSX_ARCHITECTURES=arm64
cmake --build build-ios --config Release -- CODE_SIGNING_ALLOWED=NO
```

That build is unsigned and can't be installed. Running it on a phone
needs a development team for signing, and nobody has done that here
yet.

*iOS, Simulator* (this mirrors `ios-simulator-smoke`). Qt's iOS
simulator libraries are x86_64 only. On Apple silicon the simulator
therefore runs under Rosetta. With Xcode 26 and later, first delete
the default iOS platform component in Xcode's settings, then install
the universal one
(`xcodebuild -downloadPlatform iOS -architectureVariant universal`).
Boot a simulator before installing:

```sh
<qt-install>/ios/bin/qt-cmake -B build-iossim -G Xcode \
    -DBUILD_TESTING=OFF \
    -DCMAKE_OSX_SYSROOT=iphonesimulator -DCMAKE_OSX_ARCHITECTURES=x86_64
cmake --build build-iossim --config Release -- CODE_SIGNING_ALLOWED=NO
xcrun simctl install booted build-iossim/app/Release-iphonesimulator/qttune.app
```

Then launch it from the simulator's home screen. The CI job's launch
step shows how to read the bundle ID from `Info.plist`, launch the
app, and send its Qt log output to a file.

## Test fixtures: the two fake vendors

Both fakes compile the same source and differ only in their `.def`
export surface — together they pin the PassThruIoctl export-spelling
resolver (see the 2026-10-16 VX incident):

| Target | Exports | Pins |
|---|---|---|
| `fake_j2534` | `PassThruIoctl` (spec spelling, directly) | Primary resolution — the conformant-vendor case; exercised by every default test |
| `fake_j2534_legacy` | `PassThruIoCtl` (legacy alias only) | The fallback resolver — what some real vendors still ship |

Do not add `__declspec(dllexport)` back to the fake's vendor
functions: the `.def` files own the export surface, and a stray
dllexport collapses the two fixtures back into one (the spec test's
negative assertion will catch it, but best not to trigger it).

## Recommended verification sequence

`scripts/verify.sh` (Git Bash on Windows; any shell elsewhere) runs
steps 1–3 below and prints one line per step (the counts below are an
example):

```sh
scripts/verify.sh                         # both trees, full suite, cross-arch
scripts/verify.sh --tests-only -R '^BridgeTest\.'   # a subset, no build
```

```
win32       ok       1 s  0 warnings
build       ok       6 s  0 warnings
ctest       ok      32 s  171/171 passed, 1 skipped
cross-arch  ok      13 s  21/21 passed, 0 skipped
```

Full output goes to `Testing/verify/<step>.log` (git-ignored). On a
failure it prints the compiler errors or the failed test names and
stops. It fails, rather than passing, when `-R` matches no test, and
when the env-gated routing case skips in the cross-arch step, since
that means the x86 paths are wrong. The trees must already be
configured. Off Windows, or without `build-win32`, the Win32 and
cross-arch steps are left out.

For any change touching the surrogate, the fakes, or the J2534
client/transport — in this order:

1. **Rebuild both trees, Win32 first.** The cross-arch run consumes
   `build-win32` artifacts directly, and the app build copies
   whatever surrogate `build-win32` holds at that moment.
2. **Full suite, bare shell** (no env vars — the cross-arch case
   must *skip*, visibly, never silently fall back).
3. **Cross-arch run** with the variables exported (below).
4. **Session-log check** when tracing changed. The log is
   `%TEMP%\ecuio-surrogate.log`; each spawn keeps the one it replaces
   as `ecuio-surrogate.<YYYYMMDD-HHMMSS-mmm>.log`, the newest 50 of
   them, so a suite run pushes out older ones. It holds the child's lines
   (`[surrogate HH:MM:SS.mmm]`) and the client's (`[sur ...]`) in time
   order. A session started while another holds that file logs to
   `ecuio-surrogate-<pid>-<n>.log` instead ([j2534.md](j2534.md#surrogate)).
   - The child logs one line per vendor call with its status:
     `OP_OPEN`, `OP_CONNECT`, `OP_DISCONNECT`, `OP_WRITE`,
     `OP_IOCTL`, `OP_START_FILTER` and `OP_STOP_FILTER`. Grep for the
     op you changed.
   - After a clean close, `OP_DISCONNECT: vendor status 0` should
     appear exactly once.
   - Tests can read the log through `ecuio_j2534_sur_log_path()`, as
     `ClientAndChildLinesShareTheSessionLog` does. A new trace line is
     still worth reading by eye once.
5. If the app is affected, rebuild the `qttune` target (after the
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
the `qttune_ui` library, against a real bridge. Each
`tests/qml/tst_*.qml` file is one ctest case, `QmlTest.<name>`, in a
process of its own; a new file is picked up at the next configure.

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
- The link-state cases drive the fake in-proc, arming its knobs
  through the harness's `Harness.armFakeKnob`; they skip off Windows.
- The page tests fail on any warning from the app's own QML
  (`qrc:/qt/qml/QtTune/…`), but not on Qt's.

**J2534 suites carry a 30 s per-test timeout.** The slowest
legitimate case (the cross-arch timeout-recovery test, ~3.5 s) has
near-10x headroom, so a slow CI machine cannot false-alarm — but a
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
ctest --test-dir build -C Release -R "J2534Routing|J2534Surrogate|J2534(Health|Connect)Test\..*/Surrogate" --output-on-failure
```

Use Windows-style paths in these variables (forward slashes are
fine). MSYS translates paths in command arguments but not in
environment-variable values, so `/e/...` reaches Windows verbatim and
the open fails. Without these variables,
`J2534RoutingTest.X86FakeSelectsSurrogateBackendAndRoundTrips`
skips.

Every runtime variable carries the core's prefix, `ECUIO_`: these two,
the fake's knobs and the surrogate's. The one `QTTUNE_` name is
`QTTUNE_SURROGATE_SOURCE`, the app build's CMake cache setting above,
which is read at configure time, never at runtime. A variable under
any other name is ignored without a word: the routing case skips, the
other surrogate cases run against the x64 tree's surrogate and pass,
and the run looks green. Read the skip count, or use
`scripts/verify.sh`, whose cross-arch step fails on a skip.

**Two ways an env-gated test lies to you** (both hit during the
2026-10-16 bench pre-flight, both operator error):

- A var set to the **wrong value** points the test at the wrong
  arch or a nonexistent path — the failure looks like a routing
  regression (`inproc` instead of `surrogate`) or a fast
  `ECUIO_ERR_LOAD_FAILED` or `ECUIO_ERR_SURROGATE_FAILED`. Echo the
  variable before diagnosing code.
- Each spawn takes `%TEMP%\ecuio-surrogate.log` and keeps the log it
  replaces under a dated name, the newest 50 of them. A suite run
  spawns dozens, so copy a log that's evidence before running the
  tests, and check its stamps: the fixed name always holds the latest
  spawn, which may not be the run you mean.

**Fuzzing** isn't part of ctest. The two libFuzzer harnesses need a
tree of their own (`-DECUIO_BUILD_FUZZERS=ON` with
`QTTUNE_BUILD_CORE_ONLY=ON` and `BUILD_TESTING=OFF`; `build-fuzz-msvc`
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
and `-R` work as in `verify.sh`. CI's `win-coverage` job runs the same
script on every push and uploads the Cobertura file to Codecov for the
README badge ([testing.md](testing.md#coverage)).

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| `QmlCacheGeneratedCode` unresolved externals after adding a QML file | Stale QML cache. Do a clean reconfigure (`rm -rf build`). |
| A `QmlTest` case fails with "Received a warning that resulted in a failure" | A binding in the app's QML raised a warning: the message names the file and line under `qrc:/qt/qml/QtTune/`. Fix the binding; a common cause is a delegate reading its `index` (-1) while the model resets. |
| `test_qml` exits at once and prints that a QML type isn't available, or `module "QtTune" is not installed` | The module's plugin isn't linked or imported. Whatever links `qttune_ui` also links `qttune_uiplugin` and has `Q_IMPORT_QML_PLUGIN(QtTunePlugin)` in one source file, as `app/main.cpp` and `tests/test_qml.cpp` do. |
| Test discovery fails at build time with `0xc0000135` | A DLL is missing. Discovery runs the test binary during the build, so Qt-linked test targets must copy `$<TARGET_RUNTIME_DLLS>` POST_BUILD, ordered before `gtest_discover_tests`. |
| Surrogate test fails with child exit code 5 | An export can't be found by name, usually because of x86 `__stdcall` decoration. Check the `.def` file. Other exit codes are listed in [j2534.md](j2534.md#surrogate). |
| Surrogate opens fail, in tests or the app, and the session log says "built from other shared headers" | One tree was built from older copies of `j2534_ptypes.h` or `surrogate_protocol.h`, usually a stale `build-win32`. Rebuild both trees, Win32 first. |
| Cross-arch test fails with odd values, but same-arch passes | A stale `build-win32` fake: the fakes carry no build fingerprint. Rebuild both trees. |
| In the app, an x86 adapter (for example the OBDX Pro VX) fails with "surrogate process failed: no surrogate (ecuio-j2534-surrogate.exe)", and `%TEMP%\ecuio-surrogate.log` holds an `open: no surrogate exe` line | No surrogate next to `qttune.exe`, so the child never launched. The x64 tree was configured before `build-win32` had a surrogate. Build the Win32 tree, re-run `cmake -B build`, check for `Deploying Win32 J2534 surrogate` in the output, and rebuild. Setting `ECUIO_SURROGATE_EXE` also works. |
| The cross-arch routing case skips although the variables were exported | They're named something else, for example `QTTUNE_SURROGATE_EXE`: nothing reads that. The names are `ECUIO_SURROGATE_EXE` and `ECUIO_FAKE_DLL` (Cross-arch run, above). The rest of the run passes on the x64 surrogate, so the skip is the only sign. |
| Cross-arch test fails expecting backend "surrogate", got "inproc" | `ECUIO_FAKE_DLL` points at an x64 fake (or the wrong tree). Check the value, not the code — unset skips, wrong-value fails. |
| J2534 test fails as `Failed (Timeout)` | A genuine hang, bounded by the 30 s ceiling. Attach to the surviving test and surrogate processes and check thread wait states before killing; the child log is `%TEMP%\ecuio-surrogate.log`, and earlier spawns' logs are kept beside it under dated names. |
| Linker warning LNK4070 on a fake DLL | The `.def` file's `LIBRARY` directive disagrees with the output filename. Make them match. |
| A mobile build fails during test discovery, or tries to run a test binary | `BUILD_TESTING` is on. Cross builds must pass `-DBUILD_TESTING=OFF` (see Mobile above). |
| Re-running a skipped `ios-simulator-smoke` skips it again | A re-run replays the original event and commit, so the job's condition comes out the same. Start a manual run instead (Actions → CI → Run workflow, or `gh workflow run ci.yml --ref <branch>`); tick `smoke_only` to skip the other platform jobs. A commit whose message contains `[ios-smoke]` also works. |
| `ios-simulator-smoke` passes, but the app screenshots are blank | Seen once, on the first run (2026-10-20). That run launched the app straight after a cold boot, and the frame was blank at 15 s. With the boot overlapping the build, the next run drew by 15 s. If it recurs: a blank `home.png` means the runner's simulator display is at fault. No `QSG_INFO` lines in stderr means the window was never exposed. `QSG_INFO` present with blank shots means frames aren't reaching the screen, so try another `QSG_RHI_BACKEND`. A blank 15 s shot with a drawn 45 s shot means a slow first frame. |
| `ios-simulator-smoke` fails with "qttune exited within 45 s", and the printed stderr says `Failed to initialize graphics backend` | Qt Quick couldn't get a Metal device from the runner VM. Check the boot step's `system_profiler` output. Before suspecting the app, try `SIMCTL_CHILD_QSG_RHI_BACKEND=opengl` on the launch line. |
| `ios-simulator-smoke` fails with "No available iPhone simulator on iOS 18 or later" | The runner image dropped its iOS 18+ runtimes, or the job moved to a runner without them. The step prints the available devices. |

**QML lint:** run `cmake --build build --config Release --target all_qmllint`.
Warnings don't fail the build. The bridge is the registered `Bridge`
singleton, so qmllint checks every `Bridge.*` binding; the baseline is
a few `[unqualified]` warnings on `model.*` roles in LogPage's
delegate. Check for new warnings in the files you touched.

**Compiler warnings:** every first-party target builds with `/W4`
(MSVC) or `-Wall -Wextra` (GCC, Clang) through
`qttune_enable_warnings()` in the root `CMakeLists.txt`; `test_c_abi`
adds `-pedantic` with compiler extensions off. Warnings don't fail the
build. `scripts/verify.sh` prints the count per build step; keep it at
0 on MSVC.

**Formatting:** `.clang-format` (clang-format 13 or later) describes
the house style. The tree wasn't reformatted when it was added, so
format only the lines you change: `git clang-format` after staging.

## Portability traps

CI builds core with GCC, MSVC and AppleClang, and cross-compiles it
with the Android NDK's Clang and for iOS. Code that compiles cleanly
on MSVC is not necessarily portable:

- Designated initializers are C++20. MSVC accepts them in C++17 mode
  and GCC rejects them. Use constructor helpers such as `mk()` in
  `signal_sets.cpp`.
- `strncpy_s` / Annex K is MSVC-only. Use a bounded copy such as
  `copy_str()`.
- An implicit int→enum conversion fails on MSVC with C2664.

## CI

`ci.yml` runs on every push to `main` and `dev`, on every pull
request to `main`, and on demand (Actions → CI → Run workflow, on any
branch, or `gh workflow run ci.yml --ref <branch>`). A manual run
includes the iOS simulator smoke; its `smoke_only` input
(`-f smoke_only=true`) skips the other platform jobs, leaving
`core-and-tests`, which the smoke depends on. Every job that uses Qt uses 6.12.0, and
every job has a `timeout-minutes` of roughly 3-5x its usual duration:

| Job | Runner | Does |
|---|---|---|
| `core-and-tests` | Ubuntu, GCC, no Qt | Builds core and the Qt-free tests, then runs ctest. This enforces "core never links Qt." Gates every other job. |
| `core-sanitizers` | Ubuntu, GCC, no Qt | The same build and tests in Debug under AddressSanitizer and UndefinedBehaviorSanitizer. LeakSanitizer runs too, so a test that closes a session without destroying it fails here. Blocking since its first green run (2026-10-20). |
| `fuzz` | Ubuntu, Clang, no Qt | Installs the image's clang with its `libclang-rt` runtimes, checks that `-fsanitize=fuzzer` links, builds `fuzz/` (`-DECUIO_BUILD_FUZZERS=ON`, RelWithDebInfo), runs each harness for 60 s, and uploads any crashing input ([fuzz/README.md](../fuzz/README.md)) |
| `win-build` | Windows, MSVC 2022 | Builds the full app, then runs ctest (the bridge, J2534 and QML suites included) |
| `macos-build` | macOS (arm64), AppleClang | Builds the full app, then runs ctest. The QML suites' link-state cases skip without the fake. |
| `linux-build` | Ubuntu, GCC | Builds the full app, then runs ctest, as on macOS. Installs QtGui's EGL and OpenGL runtime libraries, which `test_qml` loads. |
| `android-build` | Ubuntu → Android arm64-v8a | Selects NDK r27 and JDK 21 by version, failing with a clear error if the image lacks either. Then cross-builds and packages the APK. Build only, no tests. |
| `ios-build` | macOS (arm64) → iOS device (arm64) | Unsigned device build with the Xcode generator. Build only, no tests. |
| `ios-simulator-smoke` | macOS 15 Intel → iOS Simulator (x86_64) | Runs on pull requests, on a manual run (with or without `smoke_only`), or when the pushed commit's message contains `[ios-smoke]`. Skipped on other pushes. Boots the newest iOS 18+ iPhone while the app builds, then installs and launches the app. Passes if QML loaded and the app is still running 45 s later. Uploads the `ios-simulator-smoke` artifact: a home-screen control shot, app shots at 15 s and 45 s, the app's stdout/stderr (with `QSG_INFO`), plus crash reports on failure. The job log also prints the stderr. |

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
weekly.

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

## Releases

A version tag publishes a Windows x64 download through
`.github/workflows/release.yml`. The zip runs on a PC with neither Qt
nor Visual Studio: it carries the app, the x86 J2534 surrogate, the Qt
runtime that windeployqt deploys, and the app-local Visual C++
runtime.

**To release:**

1. In one commit, set `project(QtTune VERSION X.Y.Z)` in the root
   `CMakeLists.txt` and add `docs/releases/vX.Y.Z.md`, the release
   notes. Say what's new, and what's verified at which tier
   ([testing.md](testing.md)), including what hasn't run on hardware.
2. Push that commit to `main` and wait for its CI run to pass: the
   release job refuses a commit whose CI hasn't.
   For a dry run, tag that commit `vX.Y.Z-rc1` and push the tag. The
   workflow publishes a pre-release. Download its zip, run it on a PC
   without Visual Studio if one is available, and check the
   provenance with
   `gh attestation verify <zip> -R erickramer42/qttune`. Then delete
   the pre-release and its tag.
3. Tag `vX.Y.Z` and push it.

**What the job does,** in order:

- **Version gate.** The tag must be `vX.Y.Z` or `vX.Y.Z-rcN`, its
  `X.Y.Z` must equal the CMake project version, and
  `docs/releases/vX.Y.Z.md` must exist, or the job fails before
  anything builds. An `-rcN` tag publishes as a pre-release with the
  same notes.
- **CI gate.** ci.yml never runs on tags, so the job reads the newest
  `push` run of ci.yml on the tagged commit (`gh run list --commit`)
  and fails unless it has completed with `success`: a tag pushed
  before CI finished, or on a commit whose CI failed, publishes
  nothing.
- **Build.** Qt 6.12.0 is installed as `win-build` does it, then
  `build-win32` (the surrogate only) and `build` (the app only), both
  with `BUILD_TESTING=OFF`, in the usual order so the app's build
  copies the x86 surrogate beside `qttune.exe`. CI has already run the
  suite on the commit; the cross-arch suite is `scripts/verify.sh`'s.
- **Stage, check and zip:** `packaging/windows/stage.ps1`, which also
  runs locally against a built tree
  (`pwsh packaging/windows/stage.ps1 -Version X.Y.Z-local -OutDir Testing/dist`).
  It copies the deployed app without its `.pdb`, `.lib` and `.exp`
  files, adds the x64 CRT from Visual Studio's redistributable folder
  (found with `vswhere`; windeployqt runs with `--no-compiler-runtime`),
  `LICENSE`, `DISCLAIMER.md` and `packaging/windows/`'s README,
  third-party notices and licence texts. It then fails the release if
  the surrogate imports a CRT DLL (it links the CRT statically, `/MT`,
  because an x86 `vcruntime140.dll` can't sit beside the app's x64
  one), if `qttune.exe` imports a DLL that is neither in the folder nor
  part of Windows, if it carries QML debugging (an import of
  `QQmlDebuggingEnabler`, or `qmltooling` plugins in the folder: a
  build with `QT_QML_DEBUG` opens an unauthenticated debug server when
  launched with `-qmljsdebugger`, so `app/CMakeLists.txt` defines it
  and windeployqt deploys the plugins in Debug only), or if the staged
  `qttune.exe` isn't still running
  10 s after it starts (QML that fails to load exits at once; the
  check was confirmed against a package with `qml/QtQuick/Controls`
  removed, 2026-10-25). It writes the zip and `SHA256SUMS.txt` to
  `dist/` (git-ignored).
- **Provenance:** `actions/attest-build-provenance` signs a SLSA build
  provenance for the zip, tied to this repository's workflow.
- **Publish:** `gh release create` with the zip, `SHA256SUMS.txt` and
  the notes file. A manual run for an existing tag
  (`gh workflow run release.yml -f tag=vX.Y.Z`) rebuilds it and
  replaces the release's files.

The exe isn't code-signed, so SmartScreen warns on first run; the
zip's `README.txt` and the release notes say how to get past it.
