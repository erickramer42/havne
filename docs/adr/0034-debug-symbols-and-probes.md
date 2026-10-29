# ADR-0034: Debug symbols with each release, and USDT probes in core

- **Status:** Accepted.
- **Date:** 2026-10-29.
- **Builds on:** the release workflow (`.github/workflows/release.yml`,
  `packaging/windows/stage.ps1`; `docs/development.md`, "Releases");
  [ADR-0025](0025-one-j2534-backend.md) (the two surrogates);
  [ADR-0031](0031-command-line-client.md) (`havne-cli`, which a probe
  test drives).

## Context

A release ships `havne.exe`, `havne-cli.exe` and the two J2534
surrogates in a zip, and the Android APK. None of them can be read
against its source afterwards:

- **No symbols are built.** CMake's MSVC `Release` configuration has no
  `/Zi` and links without `/DEBUG`, so no PDB exists, and `stage.ps1`
  deletes any `*.pdb` that lands in the folder. A crash dump from a
  user's machine shows addresses only, and a profiler shows the same.
- **Nothing in core can be traced without a debugger.** On Linux,
  `perf` and `bpftrace` can attach to a running process at no cost to
  it, but only where the program marks points to attach to. The bridge's
  delivery figures are the one view into the frame path, and they come
  at the end of a session.

Linux has no release download yet: the core deb and tarball are the
next item, so this record decides the rule for Linux debug info and
leaves the packaging to that item.

## Decision

### 1. Windows: PDBs from the release build, in a zip of their own

Every MSVC build in the `Release` configuration compiles with `/Zi`
and links with `/DEBUG /OPT:REF /OPT:ICF` (root `CMakeLists.txt`):

- **`/Zi` and `/DEBUG`** write the PDB beside each binary.
- **`/OPT:REF /OPT:ICF`** keep the optimizations `/DEBUG` turns off by
  default, so the code is what a Release build makes today.

This applies to local builds too. It costs link time, and keeps the
Release tree the one that gets debugged.

A release's binaries also record only their PDB's file name, not the
build machine's path (a developer's path can hold the account name):
`/PDBALTPATH:%_PDB%`. CMake's Visual Studio generator escapes the `%`
in a linker option, so the binary would record `%%%havne.pdb%%%`
([CMake issue 15865](https://gitlab.kitware.com/cmake/cmake/-/issues/15865)).
The release workflow passes the option through `LINK` in the
environment instead, which `link.exe` reads itself.

The release workflow adds one file, `havne-X.Y.Z-windows-x64-symbols.zip`,
holding:
- `havne.pdb`
- `havne-cli.pdb`
- `ecuio-j2534-surrogate-x64.pdb`
- `ecuio-j2534-surrogate.pdb`, from the Win32 tree

It goes into `SHA256SUMS.txt` and gets a provenance attestation, like
the zip. The application zip stays as it is.

`stage.ps1` checks each binary against its PDB. The debug directory
must name only the file (`dumpbin /headers`), and `dumpbin
/pdbpath:verbose` must find the PDB beside a copy of the shipped exe,
which it does only when the PDB's signature and age are the exe's. A
release whose symbols don't match its binaries fails.

`docs/development.md` explains how to use them:
- turning on Windows Error Reporting's `LocalDumps` for `havne.exe`, so
  a crash leaves a dump
- opening the dump in WinDbg or Visual Studio with the unzipped PDBs on
  the symbol path

### 2. Linux: split debug info, by build ID, with the packages

The Linux packages (the next item) build core and `havne-cli` with
`-g` and optimizations, then split the debug info:
1. `objcopy --only-keep-debug` writes it to a separate file.
2. `strip --strip-debug` and `objcopy --add-gnu-debuglink` link the
   binary to it.

Each binary has a GNU build ID (GCC's and Ubuntu's linker default,
checked with `readelf -n`). The debug files ship beside the packages,
as a `-dbgsym` deb and a `-debug` tarball. `gdb`, `perf report` and
`debuginfod` clients find them by build ID.

This record fixes the rule; the deb and tarball item builds it.

### 3. Android: not yet

The APK's native library is stripped by the Android build. Its symbols
would need the unstripped `.so`, matched by build ID, and a way to read
a tombstone (`ndk-stack`). Nothing reports an Android crash today, so
this waits for a reason.

### 4. USDT probes in core

`core/src/probes.h` defines `ECUIO_PROBE(name, ...)`:

- **Linux, not Android, with `<sys/sdt.h>`:** it expands to `STAP_PROBEn`
  from that header. That's systemtap's header-only macros: no runtime
  library, and no change to the C ABI. CMake checks for the header and
  sets `ECUIO_HAVE_SDT`.
- **Elsewhere:** it expands to nothing.

A probe is one `nop` in the code, plus a note in the ELF file
(`.note.stapsdt`) that tells a tracer where the `nop` is and where its
arguments live. Unused, it costs the `nop`. The arguments are values
already in registers or memory, so no semaphore guards them.

The provider is `ecuio`, the core's prefix:

| Probe | Where | Arguments |
|---|---|---|
| `session_start` | `ecuio_session_start`, once the worker runs | session, backend (`ECUIO_BACKEND_*`) |
| `session_close` | `ecuio_session_close`, after the worker joins | session |
| `frame` | `ecuio_session_dispatch_callbacks`, before the callbacks | session, arbitration ID, flags, data length, `timestamp_us` |

These are the points the roadmap named: connect, disconnect and the
frame callback. Link-health transitions are already in the activity
log and through `ecuio_session_get_error_state`, so they get no probe
until a trace needs them at the moment they happen.

With `bpftrace` and a probe-enabled build:

```sh
sudo bpftrace -e 'usdt:./havne-cli:ecuio:frame { @[arg1] = count(); }' \
    -c './havne-cli stream mock://demo --count 200'
```

That counts the frames per ID. `perf` reads the same notes (its SDT
events); bpftrace is the tool the tests use.

CI installs `systemtap-sdt-dev` in the Linux jobs (`core-and-tests`,
`linux-build`, `core-sanitizers`, `linux-appimage`), so every Linux
binary it builds carries the probes, and so will the packages.

### 5. Tests

- **UNIT, Linux with `ECUIO_HAVE_SDT`:** a ctest case reads
  `readelf --notes` of `havne-cli` and `test_core`. It expects the three
  probes under provider `ecuio` and fails if any is missing. A build
  without the header says why it skips.
- **HARNESS, CI's `linux-build`:** `tests/probe_trace.sh` runs under
  `sudo` with `bpftrace` and attaches to `havne-cli stream
  mock://probes --count 100`. It expects:
  - at least 100 `frame` hits
  - one `session_start` and one `session_close`
- **Release:** `stage.ps1`'s PDB checks, as §1, on every release and
  the `rc` dry run. A PDB from another build under the right name
  fails them.
- **Every other platform** builds the macros to nothing, and every CI
  job compiles them.

## Alternatives considered

- **PDBs inside the application zip.** It would double the download
  for something a user never opens. A separate zip, checksummed and
  attested, serves whoever needs it.
- **A public symbol server.** It needs hosting and an upload step for
  every release. The zip on the release page does the same for a
  project this size.
- **LTTng-UST instead of USDT.** It brings a runtime library and a
  session daemon. USDT is header-only and free when unused, and
  `bpftrace` and `perf` both read it.
- **Probes on Windows (ETW TraceLogging) and macOS (DTrace).** The same
  macro layer could map to either later. Each needs its own tooling
  (TraceLogging's provider registration, DTrace's generated header), and
  nothing asks for them yet.
- **Tracing core's functions by symbol** (uprobes on `pump_one`, etc.).
  That needs the symbols, breaks when the compiler inlines, and moves
  with every refactor. A probe is a named, stable point.

## Consequences

- **Good:**
  - A crash dump or a profile from a release build reads against its
    source on Windows, and on Linux once the packages ship.
  - The frame path can be counted and timed on a running Linux system
    without a debug build or a rebuild.
- **Bad:**
  - Release links are slower, and the release has one more file to
    check.
  - Probe names and arguments are something tracing scripts depend on.
    They're named here and documented, and a change to one goes through
    this record.
  - The bpftrace step needs root on the runner, which `linux-build`
    already uses for the SocketCAN step.
