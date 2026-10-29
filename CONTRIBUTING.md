# Contributing to Havne

Thanks for your interest in improving Havne. This document explains where
to start, how to report problems, propose changes, and get a contribution
accepted.

Havne is a one-maintainer project built deliberately: an append-only C ABI, a
Qt-free core, ADRs for every architectural decision, and a tiered test suite.
Contributions that respect those constraints land quickly. Contributions that
fight them won't land at all.

By taking part you agree to the [Code of Conduct](CODE_OF_CONDUCT.md).

## Where to start

Read [docs/architecture.md](docs/architecture.md) first: it is a short map
of how the parts fit together and why, and each section points to the
document that holds the detail. Then, by what you want to do:

| You want to… | Read |
|---|---|
| Know what works, what's broken and what's planned | [STATUS.md](STATUS.md): "Where things stand", "Known issues", the roadmap |
| Find something to work on | STATUS.md's [backlog](STATUS.md#backlog) (sized **S**, **M** or **L**; an **S** item is the gentlest start) and [docs/testing.md's gaps](docs/testing.md#gaps-not-verified) |
| Build it and run the tests | [docs/development.md](docs/development.md): "Toolchain", "Build configurations", "Running tests", "Troubleshooting" |
| Understand why something is built the way it is | The [ADR index](docs/adr/README.md). Search it before proposing a change: the decision, and the alternatives it turned down, may already be written up |
| Change the core, a transport or the C ABI | architecture.md's "Sessions, profiles and decode", the public headers in `core/include/ecuio/` (each documents its contract), [ADR-0007](docs/adr/0007-uri-selected-transports.md) (transports by URI) and [ADR-0016](docs/adr/0016-signal-definition-layout.md) (the def layout) |
| Add a PID or a profile | [ADR-0013](docs/adr/0013-protocol-profiles.md) (profiles), [ADR-0015](docs/adr/0015-request-engine.md) (how a def's mux key becomes a request), and architecture.md's note on the pairs that must change together. Check a PID's formula against a public SAE J1979 table, never from memory: the fakes answer from the same assumption, so the tests can't catch a wrong one |
| Work on J2534 | [docs/j2534.md](docs/j2534.md), then ADRs [0010](docs/adr/0010-j2534-v0404-shared-types.md), [0014](docs/adr/0014-single-threaded-surrogate.md) and [0025](docs/adr/0025-one-j2534-backend.md) |
| Work on ELM327 adapters or Bluetooth | [docs/elm327.md](docs/elm327.md), then [ADR-0026](docs/adr/0026-elm327-transport.md) and its appendices and [ADR-0028](docs/adr/0028-app-fed-streams.md) (Bluetooth through the app) |
| Work on the UI | architecture.md's "The UI layer", [ADR-0005](docs/adr/0005-coalesced-signal-updates.md) (10 Hz updates), the QML tests in `tests/qml/` |
| Work on Android | [ADR-0028](docs/adr/0028-app-fed-streams.md), [ADR-0029](docs/adr/0029-android-background-sessions.md), development.md's "Build configurations" (Mobile, then *Android*); the signed release APK: [ADR-0030](docs/adr/0030-signed-android-releases.md) and development.md's "Releases" |
| Know how a change is verified | [docs/testing.md](docs/testing.md): the tiers, every verified behavior with the test that pins it, and the gaps |
| Fuzz a parser | [fuzz/README.md](fuzz/README.md) |
| Report a security problem | [SECURITY.md](SECURITY.md), privately; the threat model is [docs/security-assurance.md](docs/security-assurance.md) |
| See how it got here, and the lessons learned | [CHANGELOG.md](CHANGELOG.md) and [docs/engineering-log.md](docs/engineering-log.md) |
| See what real hardware has confirmed | [docs/bench-sessions/bench-prep.md](docs/bench-sessions/bench-prep.md) and the session sheets beside it |

## Reporting a bug

Open a [GitHub Issue](https://github.com/erickramer42/havne/issues) with:

1. The Havne version (or commit) and your OS (and Android version, on a
   phone).
2. What you expected, and what happened instead.
3. How you connected: the URI (Settings shows it), the adapter, and the
   vehicle, if a session is involved.
4. The activity log's lines around the problem (the Log page). A failed
   connect's line says why it failed, and the "Session:" line says what
   the session ran on.

Two logs help with adapter problems: the ELM327 wire log
(`ecuio-elm327.<date>.log`) and the J2534 surrogate's session log
(`ecuio-surrogate.log`), both in the system's temporary folder.

**Never paste raw session recordings, logs, or screenshots without
scrubbing them first.** They can carry your VIN, your ECU's calibration
IDs and serial numbers, an adapter's serial number or Bluetooth address,
and file paths with your account name in them. See
[Handling vehicle data](#handling-vehicle-data), or describe the contents
instead.

A security problem goes through [SECURITY.md](SECURITY.md), not an issue.

Feature requests are welcome too, but see [Scope](#scope) first.

## Reporting on hardware

Havne has run on one car with two adapters. A report from another
vehicle or adapter is useful even when everything works: open an issue
with the adapter, the vehicle's protocol (CAN, 11-bit or 29-bit, its bit
rate), what you ran, and what the activity log said, scrubbed as below.
The run sheets in [docs/bench-sessions/](docs/bench-sessions/bench-runsheet.md)
show what a session checks, and [bench-prep.md](docs/bench-sessions/bench-prep.md)
lists the assumptions only hardware can confirm. Session sheets in the
repository are filed by the maintainer, so attach nothing raw to a pull
request. Read-only sessions only: keep to Havne's own requests, and stop
if a module behaves unusually ([DISCLAIMER.md](DISCLAIMER.md)).

## Governance

Havne uses a single-maintainer model (benevolent dictator). Eric Kramer is
the project owner and lead developer; he makes all final decisions on scope,
architecture and merges. Proposed changes are discussed in GitHub issues,
reviewed through pull requests, and decided by the maintainer. Architectural
decisions are recorded as ADRs in docs/adr/.

## Roles and Responsibilities

Havne currently has one maintainer. The roles below describe the functions
that exist in the project and who performs them.

| Role | Holder | Responsibilities |
|------|--------|------------------|
| Project owner / Lead developer | Eric Kramer (@erickramer42) | Sets scope, approves architectural decisions, final merge authority |
| Release manager | Eric Kramer (@erickramer42) | Tags releases, publishes release notes, maintains version compatibility |
| Security contact | Eric Kramer (@erickramer42) | Receives vulnerability reports per SECURITY.md, coordinates remediation |
| CI/infrastructure owner | Eric Kramer (@erickramer42) | Maintains GitHub Actions workflows, branch protection, and repository settings |

Architectural decisions are recorded as ADRs in `docs/adr/`. Changes follow the
convention outlined in this document; all pull requests are reviewed by the
maintainer before merging.

## Scope

Havne is **read-only today**: its request engine sends OBD-II Services 01
and 09 and UDS Service 0x22 (ReadDataByIdentifier), and refuses any other
service ([ADR-0015](docs/adr/0015-request-engine.md)). There is no code that
clears trouble codes, writes or flashes (see [DISCLAIMER.md](DISCLAIMER.md)).
The [roadmap](STATUS.md#roadmap) plans full OBD-II diagnostics, clearing
codes included, for v0.3.5, and puts the write path (security access,
flashing) at 1.0 and later, through manufacturer plugins. Each of those
needs an ADR before any code. Proposals in these directions are worth
discussing in an issue first: they carry regulatory weight (emissions law,
warranty, effects on safety systems) that must be designed for, not bolted
on.

## Building

Requires a C++17 compiler, CMake 3.21+, and Qt 6.12, the version CI and the
maintainer build (compiler minimums follow Qt's: MSVC 2022+, GCC 10+,
Xcode 16+). [docs/development.md](docs/development.md) has the toolchain
table, Qt's installation and every build configuration.

```sh
cmake -B build -DCMAKE_PREFIX_PATH=<your Qt>
cmake --build build
ctest --test-dir build
```

- **The core alone needs no Qt:** `-DHAVNE_BUILD_CORE_ONLY=ON` builds
  `ecuio-core` and its Qt-free tests, as CI's Ubuntu job does. Most core
  and transport work can be done and tested this way.
- **On Windows** the Visual Studio generator is multi-config, so add
  `--config Release` to the build and `-C Release` to ctest. x86 vendor
  drivers also need the x86 surrogate from a Win32 tree, configured and
  built before the app's; `scripts/verify.sh` (Git Bash) builds both trees,
  runs the suite and the cross-arch subset, and prints one line per step.
- **Android** builds from the same sources with Qt's Android kit
  (development.md's "Build configurations", *Android*); CI cross-builds it
  and uploads a debug APK.

The build validates that the core never links Qt; the CI matrix runs ctest on
Windows, macOS and Linux, core under ASan/UBSan, the libFuzzer harnesses,
clang-tidy and CodeQL, the load test, and the Android/iOS cross-builds.
**A pull request that isn't green on CI won't be merged.**
If CI fails on your change, check whether it's the code or the toolchain before
assuming either.

## Conventions your change must respect

The project's architecture is enforced, not aspirational. The full rules live
in [docs/architecture.md](docs/architecture.md), with the transports'
specifics in [docs/j2534.md](docs/j2534.md) and
[docs/elm327.md](docs/elm327.md), and the build and test workflow in
[docs/development.md](docs/development.md); the ones that reject most outside
contributions:

- **The C ABI is the only boundary.** Public headers in `core/include/ecuio/`
  compile as strict C. Enum values, status codes and opcodes are append-only:
  never renumber, reuse, or remove one. Breaking changes need `feat!:`; a
  change to the signal-definition layout also bumps `ECUIO_SIGNAL_SET_LAYOUT`
  ([ADR-0016](docs/adr/0016-signal-definition-layout.md)). Every public name
  carries the core's prefix (`ecuio_`, `ECUIO_`); the product's name appears
  only where a user or builder sees it.
- **Core never includes or links Qt.** Decoding, scaling and units live in
  core; the UI receives final display values. A Qt include in `core/` fails CI.
- **Read-only by construction.** Every request enters the engine through one
  function that refuses any service but 0x01, 0x09 and 0x22. A change that
  sends anything else is a scope change (above), not a patch.
- **Know your thread.** Frame callbacks run on the transport's worker thread:
  they must not block, and must never close or destroy their session (both
  join that thread). No worker allocates per frame, and tests count it. The
  bridge's frame callback touches only atomics and posts the frame to the GUI
  thread, and its values callback holds one short lock while it stages values
  for the GUI thread's 100 ms tick; the rest of the bridge runs on the GUI
  thread. Slow calls (a driver's connect and disconnect, a port's open) run on
  threads of their own, so the GUI never waits on them.
- **Transports encode, never decode.** The URI selects the transport
  (`mock://`, `replay://`, `elm327://`, `j2534://` on Windows);
  everything else is rejected.
- **ABI-breaking or architectural changes need an ADR** under `docs/adr/`
  before the code ([the index](docs/adr/README.md) says how to add one). Look
  at the existing ones first; your change may already have one.

## Coding standards

Each primary language has a named guide. Contributions must generally comply
with the guide for the language they touch.

- **C and C++** (`core/`, `surrogate/`, `app/`, `tests/`): the LLVM style as
  configured in [.clang-format](.clang-format), which records the project's
  deviations from LLVM (4-space indent, a function's brace on its own line,
  pointer-left, includes left in written order, comments not reflowed).
  Format the lines you change with `git clang-format`; the tree predates the
  file and is not being reformatted wholesale.
- **C ABI headers** (`core/include/ecuio/`): the [SEI CERT C Coding
  Standard](https://wiki.sei.cmu.edu/confluence/display/c/) as the reference
  for the strict-C headers: fixed-width types, no dynamic allocation, no
  undefined behavior.
- **QML and JavaScript** (`app/qml/`): [Qt's QML Coding
  Conventions](https://doc.qt.io/qt-6/qml-codingconventions.html).
- **CMake**: the conventions already in the tree: lower-case commands,
  target-scoped properties, comments explaining why rather than what.
- **Portability:** everything is C++17, and CI compiles it with GCC, MSVC and
  AppleClang. Code that builds on one isn't necessarily portable:
  development.md's "Portability traps" lists the ones that have bitten
  (designated initializers, Annex K functions, implicit int-to-enum
  conversions).

Beyond formatting: every first-party target builds with `/W4` or
`-Wall -Wextra` at zero warnings on MSVC; `core/` and `surrogate/` pass
clang-tidy with the checks in [.clang-tidy](.clang-tidy) (`scripts/tidy.sh`,
and CI fails on any warning; [docs/development.md](docs/development.md)
says how to run it). Comments and committed docs describe the present and
explain why: no change history ("used to", "now"), which belongs in the
commit message, and a Known Issue cited by number only while it's open in
STATUS.md.

## Input validation

Anything a session takes from outside is checked before it is used, and
anything unrecognized is refused, never guessed at. A change that adds an
input adds its check, a test that the check refuses, and, for parsers of
arbitrary bytes or text, a fuzz harness ([fuzz/README.md](fuzz/README.md)).
The threat model is [docs/security-assurance.md](docs/security-assurance.md).

- **URIs**: `mock://`, `replay://`, `elm327://` and `j2534://` only. Every
  scheme shares one strict query grammar (`core/src/uri_query.h`): an
  unknown key, a malformed value or an unknown profile is refused at
  session create, before any DLL loads or any port opens.
- **Serial ports, sockets and app streams**: an `elm327://` port is `COM1`
  to `COM255` on Windows and a `/dev/` path elsewhere, a WiFi adapter's
  dotted IPv4 address and port (never a host name, so nothing is looked
  up), or the name of a stream the app registered (a Bluetooth link on a
  phone, ADR-0028). An adapter's answers are parsed by a pure layer
  (`core/src/elm327_protocol.h`, fuzzed) that bounds every line, and a
  setup answer that isn't the expected one refuses the connect.
- **Vendor DLLs**: the path must be absolute, and the file must be a
  Windows DLL by its PE header, before any surrogate starts.
- **Frames**: a frame holds up to 4,096 payload bytes (`ECUIO_FRAME_DATA_MAX`,
  a whole ISO 15765 message); a longer message is cut there and flagged
  `ECUIO_FRAME_FLAG_TRUNCATED`, and a replayed row longer than that is
  refused.
- **Recordings**: a replay reads the whole `frames.csv` at connect (fuzzed),
  and a row that doesn't parse refuses it, naming the line but never the
  path.
- **Signal definitions**: `ecuio_profile_register` runs the set validator,
  which accepts exactly the defs the decoder can decode, requires reserved
  bytes to be zero and rejects duplicate IDs
  ([ADR-0016](docs/adr/0016-signal-definition-layout.md)).

## Testing

Tests are tiered ([docs/testing.md](docs/testing.md)); a change is only done
when tested at the right tier:

| Tier | Covers |
|---|---|
| Unit | Core, signal and ABI logic, no I/O |
| Bridge | The Qt bridge and models, headless |
| QML | The app's QML pages against a real bridge, offscreen (Qt Quick Test) |
| HARNESS | The J2534 stack against the in-tree fake vendor DLL (Windows); the ELM327 transport against the in-tree fake ELM327, behind its port, a loopback socket, an app stream and, on Linux and macOS, a pseudo-terminal (every platform) |
| Cross-arch | The J2534 HARNESS tier through the x86 surrogate (Windows; maintainer-run, CI never runs it) |
| Fuzzing | The signal decoder, the validators, the `uds-generic` query parser, the ELM327 reply parsers and the replay reader under libFuzzer, in CI ([fuzz/README.md](fuzz/README.md)) |
| Load test | A recorded engine start replayed through the bridge, in CI on Windows and Linux |
| Simulator/Manual smoke | The running app's layout and paint |
| BENCH / Vehicle | Real hardware: **maintainer-run only** |

You can run the Unit, Bridge, QML and HARNESS tiers yourself (all through
ctest), and the fuzzers by following fuzz/README.md; smoke is a recipe in
docs/testing.md. BENCH and Vehicle sessions need real adapters and a car, so
don't attempt them in a PR: describe what needs hardware verification
instead (and see [Reporting on hardware](#reporting-on-hardware) for a report
of your own).

Changes to timing, threading, callbacks or streaming state need at least a
Bridge- or HARNESS-tier test even if a unit test passes: the unit test proves
the logic, not the timing. A new test should fail without the change it
pins; try it against the old code before relying on it.

## Commit messages

Conventional Commits, with a scope. In use: `core`, `bridge`, `j2534`,
`j2534-surrogate`, `elm327`, `obd2`, `ui`, `app`, `android`, `fuzz`,
`windows`; `ci:` goes without one, and `docs:` takes one where it helps
(`docs(adr)`, `docs(bench)`). Examples:

```
feat(core): mux keys up to 3 bytes
fix(bridge): Log table sorts by arrival, clears on connect
docs: accept ADR-0016
```

Keep commits terse: the subject line says what, a short body says why, and
`BREAKING CHANGE:` footers where applicable. ABI-breaking changes use `feat!:`.

## Pull requests

1. Discuss anything non-trivial in an issue first.
2. Branch from `main` (or `dev` if it has moved ahead), keep the diff small:
   a PR that mixes refactors with behavior changes will be asked to split.
3. Every changed source file needs the `// File/layer:` and `// Testing:`
   headers the rest of the code carries, describing how your change is
   tested and at what tier.
4. Update what the change touches: STATUS.md test counts, docs/testing.md,
   a line under CHANGELOG.md's "Unreleased" if a user would notice the
   change, an ADR for a new decision, and an engineering-log row if the
   change taught a lesson. Files that must change together are named in
   docs/architecture.md (the mock encoder and its signal defs; the `obd2`
   defs and the fakes' canned replies in `tests/fake_ecu.h`).
5. Rebase onto the target branch before requesting review; merge commits are
   squashed unless the history itself is the point.

PRs from forks get a human review; there is one maintainer, so allow time.

## Handling vehicle data

Havne's domain produces data that never belongs in a public repository:
VINs, calibration IDs, ECU and adapter serial numbers, adapter Bluetooth and
MAC addresses, absolute local paths, user and machine names. Before
committing or attaching anything captured from a session (logs, recordings,
screenshots, bench notes), scrub it or use placeholders (`<VIN>`, `<CALID>`,
`<serial>`, `<BT address>`, `D:/…/build`), keeping the rest of each line so
it still reads as evidence. Where they hide:

- **The activity log's identity line** (`ECU 0x7E8 identity: VIN …`) and a
  recording's `events.log`, which holds the same lines.
- **Hex dumps:** an ELM327 wire log or a frame log holds the identification
  replies as hex (`49 02` is the VIN, `49 04` the calibration ID, `62 F1 xx`
  the ECU's serials), which a search for a VIN's shape won't find.
- **URIs:** a Bluetooth adapter's URI (`elm327://bt:<address>`) carries its
  address; Havne's own labels shorten it, but Settings and the logs keep it
  whole.
- **Screenshots:** check the Log page and Live for a VIN, and strip a
  phone screenshot's metadata (EXIF names the phone's maker and model).
- **Recording folders** end with the vehicle's label, if you named one.

When in doubt, describe the data in the issue instead of attaching it.

## Licensing

By contributing, you agree your contribution is licensed under the project's
license (see [LICENSE](LICENSE)). Keep copyright headers consistent with the
existing files.
