# Development guide

Building, testing and CI for contributors. For the short build
instructions, see the [README](../README.md#build).

## Toolchain

| Component | Version | Notes |
|---|---|---|
| CMake | 3.21+ | Required for `qt_standard_project_setup()` |
| Qt | 6.12+ (LGPL) | Core, Gui and Quick. Not needed for core-only builds. |
| C++ compiler | C++17 | MSVC 19.x (2019+), GCC 11+, Clang 14+ |
| GoogleTest | 1.14.0 | Fetched automatically on the first configure if it isn't installed |
| Python (optional) | 3.13+, 64-bit | Reserved for future trace-analysis tooling |

## Build configurations

**Full app, Windows.** The Visual Studio generator is multi-config,
so `--config` and `-C` are required:

```sh
cmake -B build -DCMAKE_PREFIX_PATH=<qt-install>/msvc2022_64
cmake --build build --config Release
# windeployqt runs post-build
build/app/Release/qttune.exe
```

**Optional: deploy the Win32 surrogate next to the app.** By default
the app resolves the surrogate through `QTTUNE_SURROGATE_EXE` (see
the cross-arch run below). Adding this at configure time copies the
Win32-tree surrogate next to `qttune.exe` on every app build, so the
app launches without exporting anything:

```sh
cmake -B build -DCMAKE_PREFIX_PATH=<qt-install>/msvc2022_64 \
      -DQTTUNE_SURROGATE_SOURCE=<repo>/build-win32/surrogate/Release/qttune-j2534-surrogate.exe
```

Opt-in with an empty default — unset, nothing is copied and behavior
is unchanged (contributor machines and CI configure without it). A
configured-but-nonexistent path is a configure-time error, not a
silent no-op. The copy is `copy_if_different`, so rebuilding the
Win32 tree and then the app target propagates a fresh surrogate.

**Full app, Linux and macOS.** Single-config generators drop the
`Release/` path segment:

```sh
cmake -B build -DCMAKE_PREFIX_PATH=/path/to/qt
cmake --build build
build/app/qttune
```

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
cmake --build build-win32 --config Release --target qttune-j2534-surrogate fake_j2534 fake_j2534_legacy
```

> ⚠️ **Rebuild both trees** after any change to `j2534_ptypes.h` or
> `surrogate_protocol.h`. Their constants are ABI between binaries
> built in separate trees. A stale tree with a mismatched struct
> size fails loudly. A stale tree with a changed constant value
> produces silently wrong data (Known Issue #19 in
> [STATUS.md](../STATUS.md)). The same applies to any source that
> compiles into both trees — the surrogate and the fakes — since
> the cross-arch run loads artifacts from `build-win32` directly.

**Mobile (not yet validated).** Open the project in Qt Creator,
select an Android or iOS kit, and Run. The QML, bridge and core are
shared, and only the kit differs.

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

For any change touching the surrogate, the fakes, or the J2534
client/transport — in this order:

1. **Rebuild both trees.** The cross-arch run consumes `build-win32`
   artifacts directly, and the app's deployed surrogate copy
   refreshes on the next app build.
2. **Full suite, bare shell** (no env vars — the cross-arch case
   must *skip*, visibly, never silently fall back).
3. **Cross-arch run** with the variables exported (below).
4. **Child-log check** when tracing changed. The suite can't see the
   child's stderr; new trace lines are verified manually in
   `%TEMP%\qttune-surrogate.log` (copy before respawning — it is
   rewritten per spawn). The 2026-10-18 #24 close is the model:
   grep for `OP_DISCONNECT: vendor status 0` (present after clean
   closes) and `teardown: disconnect` (absent after clean closes).
5. If the app is affected, rebuild the `qttune` target so the
   deployment copy picks up the new surrogate.

## Running tests

```sh
# everything
ctest --test-dir build -C Release --output-on-failure
# one suite (anchor the regex)
ctest --test-dir build -C Release -R '^BridgeTest\.'
build/tests/Release/test_core.exe --gtest_filter='CoreTest.SelfUnregister*'
# specific suites
ctest --test-dir build -C Release -R "J2534Routing|J2534Surrogate|J2534Adoption" --output-on-failure
```

`gtest_discover_tests` registers each case with ctest as
`Suite.Case`. Anchor `-R` on the suite name, because `-R Signal` also
matches `BridgeTest.SignalBatchesAreCoalesced`. The suites and what
each one covers are listed in [testing.md](testing.md).

**J2534 suites carry a 30 s per-test timeout.** The slowest
legitimate case (the cross-arch timeout-recovery test, ~3.5 s) has
near-10x headroom, so a slow CI machine cannot false-alarm — but a
hang (the IPC deadlock class caught 2026-10-17) fails bounded as
`Failed (Timeout)` instead of wedging the runner.

**Cross-arch run.** Run this from the x64 tree, after building
`build-win32`:

```sh
export QTTUNE_SURROGATE_EXE='<repo>/build-win32/surrogate/Release/qttune-j2534-surrogate.exe'
export QTTUNE_FAKE_DLL='<repo>/build-win32/tests/Release/fake_j2534.dll'
ctest --test-dir build -C Release -R "J2534Routing|J2534Surrogate" --output-on-failure
```

Use Windows-style paths in these variables (forward slashes are
fine). MSYS translates paths in command arguments but not in
environment-variable values, so `/e/...` reaches `CreateProcessA`
verbatim and fails. Without these variables,
`J2534RoutingTest.X86FakeSelectsSurrogateBackendAndRoundTrips`
skips.

**Two ways an env-gated test lies to you** (both hit during the
2026-10-16 bench pre-flight, both operator error):

- A var set to the **wrong value** points the test at the wrong
  arch or a nonexistent path — the failure looks like a routing
  regression (`inproc` instead of `surrogate`) or a fast
  `QT_ERR_LOAD_FAILED`. Echo the variable before diagnosing code.
- The surrogate child log is rewritten on every spawn. Copy it
  before the next spawn if it's evidence; a log you didn't capture
  immediately may be describing an older run.

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| `QmlCacheGeneratedCode` unresolved externals after adding a QML file | Stale QML cache. Do a clean reconfigure (`rm -rf build`). |
| Test discovery fails at build time with `0xc0000135` | A DLL is missing. Discovery runs the test binary during the build, so Qt-linked test targets must copy `$<TARGET_RUNTIME_DLLS>` POST_BUILD, ordered before `gtest_discover_tests`. |
| Surrogate test fails with child exit code 5 | An export can't be found by name, usually because of x86 `__stdcall` decoration. Check the `.def` file. Other exit codes are listed in [j2534.md](j2534.md#surrogate). |
| Cross-arch test fails with odd values, but same-arch passes | The `build-win32` tree is stale. Rebuild both trees. |
| Cross-arch test fails expecting backend "surrogate", got "inproc" | `QTTUNE_FAKE_DLL` points at an x64 fake (or the wrong tree). Check the value, not the code — unset skips, wrong-value fails. |
| J2534 test fails as `Failed (Timeout)` | A genuine hang, bounded by the 30 s ceiling. Attach to the surviving test and surrogate processes and check thread wait states before killing; the child log in `%TEMP%\qttune-surrogate.log` is rewritten per spawn — copy before respawning. |
| Linker warning LNK4070 on a fake DLL | The `.def` file's `LIBRARY` directive disagrees with the output filename. Make them match. |

**QML lint:** run `cmake --build build --config Release --target all_qmllint`.
Warnings don't fail the build, and the baseline is noisy. The
`[unqualified]` warnings on `QtTune.*` are expected, because the
bridge is a context property qmllint can't see. Check only for new
warnings in the files you touched.

## Portability traps

CI builds core with GCC, MSVC and AppleClang. Code that compiles
cleanly on MSVC is not necessarily portable:

- Designated initializers are C++20. MSVC accepts them in C++17 mode
  and GCC rejects them. Use constructor helpers such as `mk()` in
  `signal_sets.cpp`.
- `strncpy_s` / Annex K is MSVC-only. Use a bounded copy such as
  `copy_str()`.
- An implicit int→enum conversion fails on MSVC with C2664.

## CI

`ci.yml` runs on every push and pull request to `main`:

| Job | Runner | Does |
|---|---|---|
| `core-and-tests` | Ubuntu, GCC, no Qt | Builds core and the Qt-free tests, then runs ctest. This enforces "core never links Qt." Gates the other two jobs. |
| `full-build` | Windows, MSVC | Builds the full app, then runs ctest (bridge and J2534 suites included) |
| `macos-build` | macOS, AppleClang | Builds the full app, then runs ctest |

`scorecard.yml` runs the OpenSSF Scorecard. Actions are pinned to
full commit SHAs with a `# vX.Y.Z` comment, and Dependabot bumps them
weekly.

**Temporary workarounds**, to revert once upstream ships tagged fixes:

- **Windows: Qt is installed with aqtinstall from git master.** The
  PyPI release builds doubled `qt6_xxxx/qt6_xxxx/` paths for the Qt
  6.11+ repository layout. Go back to PyPI once aqtinstall 3.4+ ships
  the fix. A sporadic py7zr `Bad7zFile` extraction failure has been
  seen once, and a clean re-run passed (Known Issue #1).
- **macOS needs Qt 6.12.** Qt 6.8 and earlier referenced the AGL
  framework, which Apple removed from the Xcode 26 SDK.

**Ignorable CI noise:** `Qt6TaskTree` and `QmlAssetDownloaderPrivate`
not-found warnings, and missing Vulkan headers. These come from
optional Quick plugins this app never uses.
