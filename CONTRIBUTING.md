# Contributing to QtTune

Thanks for your interest in improving QtTune. This document explains how to
report problems, propose changes, and get a contribution accepted.

QtTune is a one-maintainer project built deliberately: an append-only C ABI, a
Qt-free core, ADRs for every architectural decision, and a tiered test suite.
Contributions that respect those constraints land quickly. Contributions that
fight them won't land at all.

## Reporting a bug

Open a [GitHub Issue](https://github.com/erickramer42/qttune/issues) with:

1. The QtTune version (or commit) and your OS.
2. What you expected, and what happened instead.
3. How you connected: the adapter and vehicle, if a session is involved.

**Never paste raw session recordings, surrogate logs, or screenshots that
contain them.** QtTune logs can carry VINs, adapter serial numbers and
account-bearing file paths. Scrub them (see *Handling vehicle data* below) or
describe the contents instead.

Feature requests are welcome too — but see *Scope* first.

## Scope

QtTune is **read-only today**: it sends OBD-II Mode-01 requests and nothing
else. There is no code clearing DTCs, no writes, no flashing (see
[DISCLAIMER.md](DISCLAIMER.md)). Tuning-application features are on the
[roadmap](STATUS.md), not yet built. Proposals in these directions are worth
discussing in an issue before any code is written — they carry regulatory
weight (emissions law, warranty, effects on safety systems) that must be
designed for, not bolted on.

## Building

Requires a C++17 compiler, CMake, and Qt 6.5+ (Qt 6.12 recommended; compiler
minimums follow Qt's — MSVC 2022+, GCC 10+, Xcode 16+).

```sh
cmake -B build -DCMAKE_PREFIX_PATH=<your Qt>
cmake --build build
ctest --test-dir build
```

The build validates that the core never links Qt; the CI matrix runs ctest on
Windows, macOS and Linux, core under ASan/UBSan, Android/iOS cross-builds, and
libFuzzer harnesses. **A pull request that isn't green on CI won't be merged.**
If CI fails on your change, check whether it's the code or the toolchain before
assuming either.

## Conventions your change must respect

The project's architecture is enforced, not aspirational. The full rules live
in CLAUDE.md and docs/architecture.md; the ones that reject most outside
contributions:

- **The C ABI is the only boundary.** Public headers in `core/include/ecuio/`
  compile as strict C. Enum values, status codes and opcodes are append-only —
  never renumber, reuse, or remove one. Breaking changes need `feat!:` and a
  layout-version bump.
- **Core never includes or links Qt.** Decoding, scaling and units live in
  core; the UI receives final display values. A Qt include in `core/` fails CI.
- **Know your thread.** Bridge code on the GUI thread blocks nothing; code on
  a transport worker thread touches only atomics. Frame callbacks must not
  block or close the session.
- **Transports encode, never decode.** The URI selects the transport
  (`mock://`, `j2534://` on Windows); everything else is rejected.
- **ABI-breaking or architectural changes need an ADR** under `docs/adr/`
  before the code. Look at the existing ones first; your change may already
  have one.

Coding style: match the surrounding file. Comments are present tense and cite
open Known Issues by number. Warnings are treated as errors in practice —
first-party code builds clean at warning level 0 on MSVC.

## Testing

Tests are tiered (docs/testing.md); a change is only done when tested at the
right tier:

| Tier | Covers |
|---|---|
| Unit | Core, signal and ABI logic, no I/O |
| Bridge | The Qt bridge and models, headless |
| HARNESS | The J2534 stack against the in-tree fake vendor DLL |
| Simulator/Manual smoke | The running app's layout and paint |
| BENCH / Vehicle | Real hardware — **maintainer-run only** |

You can run Unit, Bridge and HARNESS tiers yourself; smoke is a recipe in
docs/testing.md; BENCH and Vehicle sessions need real adapters and a car, so
don't attempt them in a PR — describe what needs hardware verification instead.

Changes to timing, threading, callbacks or streaming state need at least a
Bridge- or HARNESS-tier test even if a unit test passes: the unit test proves
the logic, not the timing.

## Commit messages

Conventional Commits, with a scope. In use: `core`, `bridge`, `j2534`,
`j2534-surrogate`, `obd2`, `ui`, `windows`, `ci`. Examples:

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
   and an engineering-log row if the change taught a lesson. Files that must
   move together are listed in CLAUDE.md.
5. Rebase onto the target branch before requesting review; merge commits are
   squashed unless the history itself is the point.

PRs from forks get a human review — there is one maintainer, so allow time.

## Handling vehicle data

QtTune's domain produces data that never belongs in a public repository:
VINs, calibration IDs, adapter serial numbers, absolute local paths, user and
machine names. Before committing anything captured from a session — logs,
recordings, bench notes — scrub it or use placeholders (`<VIN>`,
`D:/…/build`). When in doubt, describe the data in the issue instead of
attaching it.

## Licensing

By contributing, you agree your contribution is licensed under the project's
license (see [LICENSE](LICENSE)). Keep copyright headers consistent with the
existing files.
