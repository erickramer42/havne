# Contributing to Havne

Thanks for your interest in improving Havne. This document explains how to
report problems, propose changes, and get a contribution accepted.

Havne is a one-maintainer project built deliberately: an append-only C ABI, a
Qt-free core, ADRs for every architectural decision, and a tiered test suite.
Contributions that respect those constraints land quickly. Contributions that
fight them won't land at all.

## Reporting a bug

Open a [GitHub Issue](https://github.com/erickramer42/havne/issues) with:

1. The Havne version (or commit) and your OS.
2. What you expected, and what happened instead.
3. How you connected: the adapter and vehicle, if a session is involved.

**Never paste raw session recordings, surrogate logs, or screenshots that
contain them.** Havne logs can carry VINs, adapter serial numbers and
account-bearing file paths. Scrub them (see *Handling vehicle data* below) or
describe the contents instead.

Feature requests are welcome too — but see *Scope* first.

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
service ([ADR-0015](docs/adr/0015-request-engine.md)). There is no code
clearing DTCs, no writes, no flashing (see [DISCLAIMER.md](DISCLAIMER.md)). Tuning-application features are on the
[roadmap](STATUS.md), not yet built. Proposals in these directions are worth
discussing in an issue before any code is written — they carry regulatory
weight (emissions law, warranty, effects on safety systems) that must be
designed for, not bolted on.

## Building

Requires a C++17 compiler, CMake 3.21+, and Qt 6.12, the version CI and the
maintainer build (compiler minimums follow Qt's — MSVC 2022+, GCC 10+,
Xcode 16+).

```sh
cmake -B build -DCMAKE_PREFIX_PATH=<your Qt>
cmake --build build
ctest --test-dir build
```

On Windows the Visual Studio generator is multi-config, so add
`--config Release` to the build and `-C Release` to ctest. x86 vendor drivers
also need the Win32 surrogate from a second build tree; the README's Build
section and [docs/development.md](docs/development.md) cover both.

The build validates that the core never links Qt; the CI matrix runs ctest on
Windows, macOS and Linux, core under ASan/UBSan, the libFuzzer harnesses,
clang-tidy and CodeQL, the load test, and the Android/iOS cross-builds.
**A pull request that isn't green on CI won't be merged.**
If CI fails on your change, check whether it's the code or the toolchain before
assuming either.

## Conventions your change must respect

The project's architecture is enforced, not aspirational. The full rules live
in [docs/architecture.md](docs/architecture.md), with the J2534 specifics in
[docs/j2534.md](docs/j2534.md) and the build and test workflow in
[docs/development.md](docs/development.md); the ones that reject most outside
contributions:

- **The C ABI is the only boundary.** Public headers in `core/include/ecuio/`
  compile as strict C. Enum values, status codes and opcodes are append-only —
  never renumber, reuse, or remove one. Breaking changes need `feat!:`; a
  change to the signal-definition layout also bumps `ECUIO_SIGNAL_SET_LAYOUT`
  ([ADR-0016](docs/adr/0016-signal-definition-layout.md)).
- **Core never includes or links Qt.** Decoding, scaling and units live in
  core; the UI receives final display values. A Qt include in `core/` fails CI.
- **Know your thread.** Frame callbacks run on the transport's worker thread:
  they must not block, and must never close or destroy their session (both
  join that thread). The bridge's frame callback touches only atomics and
  posts the frame to the GUI thread, and its values callback holds one short
  lock while it stages values for the GUI thread's 100 ms tick; the rest of
  the bridge runs on the GUI thread. Slow vendor calls (connect, disconnect)
  run on threads of their own, so the GUI never waits on them.
- **Transports encode, never decode.** The URI selects the transport
  (`mock://`, `replay://`, `elm327://`, `j2534://` on Windows);
  everything else is rejected.
- **ABI-breaking or architectural changes need an ADR** under `docs/adr/`
  before the code. Look at the existing ones first; your change may already
  have one.

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
  for the strict-C headers — fixed-width types, no dynamic allocation, no
  undefined behavior.
- **QML and JavaScript** (`app/qml/`): [Qt's QML Coding
  Conventions](https://doc.qt.io/qt-6/qml-codingconventions.html).
- **CMake**: the conventions already in the tree — lower-case commands,
  target-scoped properties, comments explaining why rather than what.

Beyond formatting: every first-party target builds with `/W4` or
`-Wall -Wextra` at zero warnings on MSVC; `core/` and `surrogate/` pass
clang-tidy with the checks in [.clang-tidy](.clang-tidy) (`scripts/tidy.sh`,
and CI fails on any warning; [docs/development.md](docs/development.md)
says how to run it); and comments are present tense and cite open Known
Issues by number.

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
- **Serial ports and sockets**: an `elm327://` port is `COM1` to `COM255`
  on Windows and a `/dev/` path elsewhere, or a WiFi adapter's dotted
  IPv4 address and port (never a host name, so nothing is looked up);
  an adapter's answers are parsed by a pure
  layer (`core/src/elm327_protocol.h`) that bounds every line, and a
  setup answer that isn't the expected one refuses the connect.
- **Vendor DLLs**: the path must be absolute, and the file must be a
  Windows DLL by its PE header, before any surrogate starts.
- **Frames**: a frame holds up to 4,096 payload bytes (`ECUIO_FRAME_DATA_MAX`,
  a whole ISO 15765 message); a longer message is cut there and flagged
  `ECUIO_FRAME_FLAG_TRUNCATED`, and a replayed row longer than that is
  refused.
- **Signal definitions**: `ecuio_profile_register` runs the set validator,
  which accepts exactly the defs the decoder can decode, requires reserved
  bytes to be zero and rejects duplicate IDs
  ([ADR-0016](docs/adr/0016-signal-definition-layout.md)).

## Testing

Tests are tiered (docs/testing.md); a change is only done when tested at the
right tier:

| Tier | Covers |
|---|---|
| Unit | Core, signal and ABI logic, no I/O |
| Bridge | The Qt bridge and models, headless |
| QML | The app's QML pages against a real bridge, offscreen (Qt Quick Test) |
| HARNESS | The J2534 stack against the in-tree fake vendor DLL (Windows) |
| Cross-arch | The HARNESS tier through the x86 surrogate (Windows; maintainer-run, CI never runs it) |
| Fuzzing | The signal decoder, the validators and the `uds-generic` query parser under libFuzzer, in CI ([fuzz/README.md](fuzz/README.md)) |
| Load test | A recorded engine start replayed through the bridge, in CI on Windows and Linux |
| Simulator/Manual smoke | The running app's layout and paint |
| BENCH / Vehicle | Real hardware — **maintainer-run only** |

You can run the Unit, Bridge, QML and HARNESS tiers yourself (all through
ctest), and the fuzzers by following fuzz/README.md; smoke is a recipe in
docs/testing.md; BENCH and Vehicle sessions need real adapters and a car, so
don't attempt them in a PR — describe what needs hardware verification instead.

Changes to timing, threading, callbacks or streaming state need at least a
Bridge- or HARNESS-tier test even if a unit test passes: the unit test proves
the logic, not the timing.

## Commit messages

Conventional Commits, with a scope. In use: `core`, `bridge`, `j2534`,
`j2534-surrogate`, `obd2`, `ui`, `app`, `fuzz`, `windows`; `ci:` and `docs:`
usually go without one. Examples:

```
feat(core): mux keys up to 3 bytes
fix(bridge): Log table sorts by arrival, clears on connect
docs: accept ADR-0016
```

Keep commits terse — subject line says what, a short body says why, and
`BREAKING CHANGE:` footers where applicable. ABI-breaking changes use `feat!:`.

## Pull requests

1. Discuss anything non-trivial in an issue first.
2. Branch from `main` (or `dev` if it has moved ahead), keep the diff small —
   a PR that mixes refactors with behavior changes will be asked to split.
3. Every changed file needs the `// File/layer:` and `// Testing:` headers the
   rest of the code carries, describing how your change is tested and at what
   tier.
4. Update what the change touches: STATUS.md test counts, docs/testing.md,
   a line under CHANGELOG.md's "Unreleased" if a user would notice the
   change, and an engineering-log row if the change taught a lesson. Files that must
   change together are named in docs/architecture.md (the mock encoder and
   its signal defs; the `obd2` defs and the fake DLL's canned replies).
5. Rebase onto the target branch before requesting review; merge commits are
   squashed unless the history itself is the point.

PRs from forks get a human review — there is one maintainer, so allow time.

## Handling vehicle data

Havne's domain produces data that never belongs in a public repository:
VINs, calibration IDs, adapter serial numbers, absolute local paths, user and
machine names. Before committing anything captured from a session — logs,
recordings, bench notes — scrub it or use placeholders (`<VIN>`,
`D:/…/build`). When in doubt, describe the data in the issue instead of
attaching it.

## Licensing

By contributing, you agree your contribution is licensed under the project's
license (see [LICENSE](LICENSE)). Keep copyright headers consistent with the
existing files.
