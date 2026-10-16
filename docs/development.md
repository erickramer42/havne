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
cmake -B build -DCMAKE_PREFIX_PATH=C:/Qt/6.12.0/msvc2022_64
cmake --build build --config Release
# windeployqt runs post-build
build/app/Release/qttune.exe         
```

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
surrogate and fake DLL that test the x64 → x86 path
([ADR-0008](adr/0008-x86-surrogate-host.md)). It is core-only because
modern Qt ships no 32-bit kits, and only the surrogate and vendor
DLLs are ever x86.

```sh
cmake -B build-win32 -A Win32 -DQTTUNE_BUILD_CORE_ONLY=ON
cmake --build build-win32 --config Release --target qttune-j2534-surrogate fake_j2534
```

> ⚠️ **Rebuild both trees** after any change to `j2534_ptypes.h` or
> `surrogate_protocol.h`. Their constants are ABI between binaries
> built in separate trees. A stale tree with a mismatched struct
> size fails loudly. A stale tree with a changed constant value
> produces silently wrong data (Known Issue #19 in
> [STATUS.md](../STATUS.md)).

**Mobile (not yet validated).** Open the project in Qt Creator,
select an Android or iOS kit, and Run. The QML, bridge and core are
shared, and only the kit differs.

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

**Cross-arch run.** Run this from the x64 tree, after building
`build-win32`:

```sh
export QTTUNE_SURROGATE_EXE='.../path/to/build-win32/surrogate/Release/qttune-j2534-surrogate.exe'
export QTTUNE_FAKE_DLL='.../path/to/build-win32/tests/Release/fake_j2534.dll'
ctest --test-dir build -C Release -R "J2534Routing|J2534Surrogate" --output-on-failure
```

Use Windows-style paths in these variables (forward slashes are
fine). MSYS translates paths in command arguments but not in
environment-variable values, so `/e/...` reaches `CreateProcessA`
verbatim and fails. Without these variables,
`J2534RoutingTest.X86FakeSelectsSurrogateBackendAndRoundTrips` skips.

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| `QmlCacheGeneratedCode` unresolved externals after adding a QML file | Stale QML cache. Do a clean reconfigure (`rm -rf build`). |
| Test discovery fails at build time with `0xc0000135` | A DLL is missing. Discovery runs the test binary during the build, so Qt-linked test targets must copy `$<TARGET_RUNTIME_DLLS>` POST_BUILD, ordered before `gtest_discover_tests`. |
| Surrogate test fails with child exit code 5 | An export can't be found by name, usually because of x86 `__stdcall` decoration. Check the `.def` file. Other exit codes are listed in [j2534.md](j2534.md#surrogate). |
| Cross-arch test fails with odd values, but same-arch passes | The `build-win32` tree is stale. Rebuild both trees. |

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
