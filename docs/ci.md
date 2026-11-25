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
releases. `codeql.yml` follows the same PRs and pushes. `repro-windows.yml`
builds the Windows release targets twice, the second copy in another folder
with every file a day newer, and fails when any `.exe` or `.dll` differs
(`packaging/windows/repro-compare.ps1`; the zips are reported). It runs on a
push to `build/**` or `dev` that changes `CMakeLists.txt`, `packaging/windows`
or the workflow, on demand, or called by another workflow (`release.yml`'s `repro` job, which `publish` needs), takes about 30
minutes, and is not part of `ci.yml`'s run
([ADR-0035](adr/0035-linux-downloads-sbom-reproducible.md)). A manual run
leaves out the iOS simulator
smoke unless asked: `-f ios_smoke=true` adds it to the whole run, and
`-f smoke_only=true` runs it with `core-and-tests` alone.

**A change to docs alone runs only the `scope` job.** `scope` diffs a
pull request from its merge-base, and a push from the commit it
replaced, and asks `scripts/docs_only.py` whether every changed path is
on its closed list: the top-level `README.md`, `STATUS.md`,
`CHANGELOG.md`, `CONTRIBUTING.md`, `SECURITY.md` and
`CODE_OF_CONDUCT.md`; Markdown anywhere under `docs/` (the release notes
in `docs/releases/` included); and png, gif or jpg files under
`docs/images/`. Nothing else is docs: not `DISCLAIMER.md` (the Windows
zip ships it), a `.md` under `tests/`, `schema/`, `plugins/` or
`packaging/`, a fixture, a log or recording under `docs/`, code, CMake,
a script or a workflow. The path and its mode decide, never a commit
message: a symlink, an executable, a submodule or a type change is not
docs, and neither is an empty diff, a diff git can't give (a forced
push's old tip is gone), a manual run or a commit whose message asks
for the iOS smoke. The list is narrower than `scripts/verify_scope.py`'s
table for `verify.sh --changed`, which also takes any `.md` outside
`tests/` and all of `docs/`; `DocsOnlyTest.SelfTest` holds everything
`docs_only.py` accepts inside that table. When the diff is docs alone,
`scope` also checks that every relative link in the docs resolves (no
committed docs lint runs in CI: the full checker is local), and every
other job is skipped by its `if`, so the run ends green: a skipped job
reports as passed to branch protection, which requires no check today.
A job that is a matrix and is skipped appears under its unexpanded name
(`win-build (${{ matrix.shard }}/4)`), so a rule that requires a shard
by name would wait forever on a docs-only run; require `scope` instead,
or add a job that always runs.
`-f macos_repeat='<ctest regex>'` (and
`-f macos_repeat_count=<n>`, 200 by default) has `macos-build` run the
matching tests again until one fails, for a failure seen once on macOS
(its timeout is then 60 minutes). Every job waits for `scope` (a few
seconds), and beyond that none waits for another but `android-build`,
which starts when `core-and-tests` ends, whatever its result: a manual
run has more jobs than the account's concurrent runners, and the short
job frees its runner first. So a run takes as long as its longest job. The long suites are shared between runners,
each with a build of its own: `win-build` runs every fourth case of ctest
(`-I k,,4`, every case once across the four), `macos-build` and
`linux-build` alternate halves (`-I k,,2`; `linux-build`'s halves are of the Qt tests, `-L qt`), `clang-tidy` three shares of the files, and
`win-coverage` three legs. The steps that aren't the suite (the caches'
saves, the probes) run on the first shard. The Windows load test is a job of its own, `win-load`, beside the shards. Every build and every ctest run
(`coverage.sh`'s too) runs four jobs at once, the hosted runners' core
count (`CMAKE_BUILD_PARALLEL_LEVEL` and `CTEST_PARALLEL_LEVEL` in the
workflow's `env`); `win-build` and `linux-build` run ctest eight wide
(`CTEST_PARALLEL_LEVEL` at job level), and the CPU-bound cases (every
`QmlTest.*`, the bridge's dense recording cases) carry `PROCESSORS 2`, so
they still share the four cores. The cases that share a fixed resource are
`RUN_SERIAL`, and the cases that use `vcan0` take `RESOURCE_LOCK vcan0`
(apart from each other, beside the rest). Every job that uses Qt uses 6.12.0, and each kit comes
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
`win-coverage`, `macos-build`, `linux-build`, `core-and-tests`,
`core-sanitizers` and `fuzz`; the Windows jobs and `linux-build` build with
Ninja for it) save the same way, except the last three:

- **Each job's first shard** (`win-coverage`'s gtest leg) saves its
  cache right after the build, before the tests, under the run's own
  key (`sccache-<job>-2-<run id>`; `win-build`'s is
  `sccache-win-build-3-<ref slug>-<run id>`), on every ref, when the
  build compiled 20 or more objects: a warm rerun adds no entry.
- **A run restores the newest entry it may read:** its branch's own,
  else `main`'s, since GitHub lets a branch read only its own caches and
  the default branch's. A lane branch's first run therefore compiles
  against `main`'s last entry, and its reruns and later commits against
  its own. `win-build` is the exception, below.
- **`win-build`'s three tiers.** `main`'s entry is as old as the last
  release, and dev gets no run of its own that a lane branch could read,
  so a lane's first run restoring `main`'s compiled most of the tree
  again (about a quarter of its compiles hit). `win-build` therefore
  restores only its own branch's newest entry (the key's slug is the
  ref name with every character outside letters and digits as `_`, so
  `main`'s entries never match another branch's prefix). With none, each
  shard downloads dev's newest seed: the `sccache-win-seed` artifact a
  run on `dev` uploads after its first shard's build (stored without
  compression, kept 5 days, uploaded only when the build compiled 20 or
  more objects), unpacked into `SCCACHE_DIR`. With no seed either it
  builds cold. The step "Seed the compiler cache" prints the tier used
  (own branch, dev seed, none) and the build step writes it to the run's
  summary above sccache's statistics. The seed is trusted only from a
  `ci.yml` run of this repository on `dev`; the dispatch input `seed_run`
  (a run id) pins another run's, which only someone who can dispatch can
  set, and `seed_upload=true` makes a run on any ref upload a seed. The
  job's token has `actions: read` for the download and `actions: write`
  for the next point.
- **One entry for each branch:** after its save, the first shard deletes
  the same ref's older `sccache-win-build-` entries (`gh cache list
  --ref`, then `gh cache delete`), never another ref's, so the branch
  holds one entry of about the cache's cap and the shared 10 GB is not
  filled by reruns. A failed delete does not fail the run.
- **`core-and-tests`, `core-sanitizers` and `fuzz`** key their entries
  `sccache-<job>-3-<ref slug>-<ISO week>-<run id>` (the slug is the ref
  name with every character outside letters and digits as `_`, so no slug
  is the prefix of another). `main` and `dev` save on every run that
  compiled 20 or more objects. A lane branch saves only when its restore
  found no entry under its own slug (the restored key tells its own from
  `main`'s), so its second run builds warm and its later runs add nothing
  to the shared 10 GB. A pull request only restores. A run restores its
  own newest entry, else `main`'s of this week, else `main`'s newest.
- **Each cache is capped** at 500 MB (1 GB for `win-build`: a warm hit
  rate of about 93% needs 700-900 MB of objects).
- **Why:** an entry for each job and week, frozen once saved and read
  by a lane branch only as `main`'s, left the hit rate at 0% on Linux
  and 15-18% on Windows and macOS. A save on every ref would put up to
  about 1.5 GB per lane run into the shared 10 GB, where eviction reaches
  the Qt kits' entries: one save for each lane keeps the warm rerun and
  bounds the cost.

GitHub drops an entry unread for 7 days, and evicts the least recently
used past 10 GB. `win-build`, `macos-build` and `linux-build` also cache
ctest's cost data (`CTestCostData.txt`, for each shard), so the cases
that took longest last time start first.

Every job has a `timeout-minutes` of roughly 3-5x its usual duration
(`linux-build`'s and `macos-build`'s are 25: each ran up to 13-14
minutes cold on a busy runner, and less with a warm cache):

| Job | Runner | Does |
|---|---|---|
| `scope` | Ubuntu | Judges whether the change is docs alone (above), and when it is checks the docs' relative links. Every other job needs it and skips on a docs-only change. |
| `core-and-tests` | Ubuntu, GCC, no Qt | Builds core and the Qt-free tests, then runs ctest (jsonschema and pytest from the hash-pinned `.github/*-requirements.txt` in a venv, the tests' Python, so `SchemaFixturesTest` and `PythonTest.*` run, not skip). `WheelTest.Installed` builds a wheel from the tree's library and installs it in a venv. This enforces "core never links Qt." Then, whatever the tests did, it installs libabigail and runs `scripts/abi-check.sh` against the newest release tag ([The C ABI check](#the-c-abi-check)); the result goes to the run's summary. Builds under sccache (restored on every ref, saved on `main`, `dev` and once for each lane branch). |
| `core-sanitizers` | Ubuntu, GCC, no Qt | The same build and tests in Debug under AddressSanitizer and UndefinedBehaviorSanitizer. LeakSanitizer runs too, so a test that closes a session without destroying it fails here. It brings up `vcan0` and requires the vcan suites (`ECUIO_VCAN_REQUIRED`), so the SocketCAN worker runs under the sanitizers, and it runs `SocketCanBusTest.TheInterfaceGoingDownIsAHardFailureThatRecovers` under sudo (CI's one run of it) and `CliTransferRunTest.candump` (`can-utils` gives it `candump`). Blocking since its first green run (2026-10-20). Builds under sccache (restored on every ref, saved on `main`, `dev` and once for each lane branch). |
| `fuzz` | Ubuntu, Clang, no Qt | Installs the image's clang with its `libclang-rt` runtimes, checks that `-fsanitize=fuzzer` links, builds `fuzz/` (`-DECUIO_BUILD_FUZZERS=ON`, RelWithDebInfo), runs each harness for 60 s from its folder of `fuzz/seeds.py`'s seeds (built from the committed fixtures for the harnesses that read a structured format: JSON, signal files, plugins, OBDb sets, recordings and exports, candump logs, CSV imports and the ELM327 emulator's hosts; empty for the rest), as many at once as the runner has cores, each log in a group of its own, and uploads any crashing input with the logs ([fuzz/README.md](../fuzz/README.md)) Builds under sccache (restored on every ref, saved on `main`, `dev` and once for each lane branch). |
| `clang-tidy` (1/3 to 3/3) | Windows, MSVC's headers | Installs the hash-pinned clang-tidy wheel, installs Qt as `win-build` does, and in a Developer shell runs `scripts/tidy.sh --qt` (a Ninja tree with Qt) over core, the J2534 layer, the surrogate, `havne-cli` and the desktop app, each runner a third of the files (`HAVNE_TIDY_SHARD=<k>/3`). Any warning fails it, and so does a baselined function whose lines or complexity grew. Shard 1 also runs `scripts/filelength_check.py` through `scripts/tidy.sh`: a new source file past 1,000 lines, or a listed file that grew, fails. Only the app's moc output is built. Each runner starts from the result cache (`scripts/tidy_cache.py`) of the newest earlier run on its branch, else on dev: all three shards' `tidy-cache-<k>` artifacts, kept 5 days, holding only the entries that run used; the tier goes to the job summary. |
| `win-build` (1/4 to 4/4) | Windows, MSVC 2022 | Builds the full app with Ninja and sccache, then runs its quarter of ctest (the bridge, J2534 and QML suites included). sccache caches the build because the reproducible compile options are spelled `-Brepro` and `-d1trimfile:` (a `/` spelling reads as a second input file and nothing is cached). Its ctest's Python is a venv with pytest, so `PythonTest.*` run. `WheelTest.Installed` builds a wheel from the tree's library and installs it in a venv. |
| `win-load` | Windows, MSVC 2022 | Builds only `bench_bridge` (`--target bench_bridge`) in the same Ninja tree settings as `win-build`, under the same compiler cache tiers (it restores the cache and never saves it), and runs the load test (`bench_bridge --load`): its table goes to the run's summary, and the step fails on a broken invariant, a bound far below the dev machine's figures, or one of two ratios that compare the run with itself (decode at 1,000 defs against 9, looping with decoding against without) ([performance.md](performance.md#load-test-the-replay-fixture-through-the-bridge)). It installs no pytest: no ctest case runs there. |
| `build-vs` | Windows, MSVC 2022 | Builds core, the shared library, the surrogate and the app with the Visual Studio generator, which `release.yml` builds with (no sccache, Qt restored as `win-build` does and never saved). It needs the scope job and skips a docs-only change. Runs on pushes to `main`, the pull request to `main` and `dev`'s runs, and on another branch only with `-f vs_build=true`: its uncached build is CI's longest job, and a lane merges into `dev`, whose run builds it. |
| `macos-build` (1/2, 2/2) | macOS (arm64), AppleClang | Builds the full app, then runs its half of ctest. The QML suites' link-state cases skip without the fake. A manual run's `macos_repeat` then runs the matching tests again until one fails (above). Its ctest's Python is a venv with pytest, so `PythonTest.*` run. `WheelTest.Installed` builds a wheel from the tree's library and installs it in a venv. |
| `win-coverage` (gtest, ctest-1, ctest-2) | Windows, MSVC 2022 | On pushes to `main` and on `dev`'s runs, and on another branch only with `-f coverage=true`, never pull requests: installs Qt as `win-build` does and OpenCppCoverage from its GitHub release (SHA-256 pinned), registers the fake DLL as a Pass-Thru device with three broken entries beside it, runs its leg of `scripts/coverage.sh` (`--part`: the gtest binaries, or one of two slices of ctest's other cases) and uploads the report to Codecov, which merges the three ([testing.md](testing.md#coverage)). |
| `linux-build` (1/2, 2/2) | Ubuntu, GCC | Builds the app, the bridge's and QML's tests (the `qt` label), `havne-cli` and the shared core, with Ninja and the mold linker, and runs its half of the Qt tests. The second shard brings up `vcan0` (the `vcan` and `can-isotp` modules load as installed on the hosted runners) and runs the Python package's vcan0 cases (`PythonTest.test_vcan`, `test_did_scan`, `test_isotp_fuzz`, selected by the `vcan` label) with `ECUIO_VCAN_REQUIRED=1` and `ECUIO_PYTEST_REQUIRED=1`, which no other job has both pytest and `vcan0` for; the first runs core's USDT probes under bpftrace (`tests/probe_trace.sh`) and the load test as on Windows. Installs QtGui's EGL and OpenGL runtime libraries, which `test_qml` loads. The Qt-free suites run in `core-and-tests` and `core-sanitizers`. |
| `linux-appimage` | Ubuntu 24.04, GCC, on x86_64 and on the ARM runner (`ubuntu-24.04-arm`) | Builds the app in Release, dated by the commit, and packs it with `packaging/linux/make-appimage.sh` ([Linux: the AppImage](packaging.md#linux-the-appimage)), printing its SHA-256, then uploads `havne-linux-x86_64` or `havne-linux-aarch64` (kept 14 days). On 24.04 because an AppImage runs only where glibc is at least its build machine's. |
| `linux-packages` | Ubuntu 24.04, GCC, on x86_64 and on the ARM runner | Builds the core packages twice under `reprotest` and fails on any difference, then installs and uses them (`tests/package_check.sh`), builds an SBOM from their component lists and validates it and `sbom.py`'s self-test document with `pyspdxtools`, and uploads `havne-core-linux-x86_64` or `havne-core-linux-aarch64` (kept 14 days) ([Linux: the core packages](packaging.md#linux-the-core-packages)). |
| `android-build` | Ubuntu to Android arm64-v8a | Starts when `core-and-tests` ends (`needs`, for a runner slot, not its result). Selects NDK r27 and JDK 21 by version, failing with a clear error if the image lacks either. Then cross-builds and packages the APK. Build only, no tests. |
| `ios-build` | macOS (arm64) to iOS device (arm64) | Calls `ios-ipa.yml`, which `release.yml`'s `ios` job calls too. Unsigned device build with the Xcode generator. Checks the bundle's Info.plist keys (`app/ios/Info.plist.in`), its versions against `project()`'s and that it holds no signature or provisioning profile, then uploads it as an unsigned `.ipa`, `havne-ios-arm64-unsigned` (kept 14 days), for sideloading ([ios.md](ios.md)). Build only, no tests. |
| `macos-app` | macOS (arm64) | Calls `macos-app.yml`, which `release.yml`'s `macos-app` job calls too. Builds the app in Release, deploys Qt into the bundle with `macdeployqt`, signs it ad hoc and checks it: versions, `Info.plist` keys, `Signature=adhoc` with no Team ID, arm64 alone, no library outside the bundle and the system. Uploads `havne-macos-arm64` (kept 14 days), the zip [macos.md](macos.md) describes. Reads the Qt kit `macos-build` saves; saves none. |
| `macos-smoke` | macOS (arm64) | Starts when `macos-app` ends. Unpacks its zip, opens the app through Launch Services and checks it is still running after 15 s and 30 s; fails on `QML FAILED TO LOAD` or a missing process. Uploads `macos-smoke`: screenshots of the runner's display, the app's stderr (with `QSG_INFO`) and the host's display facts, plus crash reports on failure. The app has no option to connect at start, so a synthetic click picks Home's `mock://demo` row and presses Connect (a refused click is a warning and leaves the shots on Home); the later shots show the session active. About two minutes. |
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
