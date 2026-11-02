# ADR-0048: Semantic versioning from v0.4.0, with 1.0.0 the finished reader

- **Status:** Accepted.
- **Date:** 2026-11-02.
- **Builds on:** [ADR-0001](0001-qt-free-core-c-abi.md) (the C ABI's
  promises), [ADR-0031](0031-command-line-client.md) (`havne-cli`'s
  schema), [ADR-0035](0035-linux-downloads-sbom-reproducible.md) (the
  core packages).

## Context

Releases v0.3.0 to v0.3.6 each added features under a PATCH bump, so
the version said nothing about what changed: v0.3.5 added a whole
diagnostics service and a transport's requests. The plan ahead runs to
a finished reading tool and then writes, and the numbers should say
where on that road a release is.

Havne has four things a version speaks for: the app, the C ABI (core's
public headers, checked by CI's `abi` job against the newest release),
`havne-cli`'s output (JSON lines with a `schema` number, ADR-0031
section 3) and, from v0.5.0, the plugin formats.

## Decision

1. **Semantic versioning ([semver.org](https://semver.org/)) from
   v0.4.0.** v0.3.0 to v0.3.6 keep their numbers.
2. **While 0.x:** each feature release bumps MINOR (v0.4.0, v0.5.0,
   ...). PATCH is for fixes alone, a `hotfix/*` branch's release. A
   MINOR release may break the C ABI, as semver allows before 1.0, but
   only on purpose: the break goes in `abi/suppressions.abignore` with
   its reason (docs/development.md, "The C ABI check") and in the
   CHANGELOG. The CMake package keeps `SameMinorVersion`, so
   `find_package(ecuio 0.4)` takes any 0.4.x and no other.
3. **1.0.0 is the finished reader:** the read side complete and
   hardened, full featured on Windows, Linux and Android (macOS and iOS
   as far as is manageable). After 1.0, MINOR adds and PATCH fixes; a
   break in the C ABI, `havne-cli`'s schema or a plugin format is a
   MAJOR release, and the CMake package moves to `SameMajorVersion`.
4. **2.0 is the write path** (security access, flashing): whatever
   else it breaks, it changes what Havne can do to a car, and the
   major number says so.
5. **Alpha and beta are informal names, not version labels.** Alpha is
   the 0.x feature releases; beta is the freeze after the last of them,
   its fix releases (0.9.x), before 1.0.0. No `-alpha` or `-beta`
   suffixes: a pre-release suffix is the release dry run's (`-rcN`).
6. **The ceiling is 0.99.99.** The Android version code is
   major x 10000 + minor x 100 + patch (`app/CMakeLists.txt`), so MINOR
   and PATCH each stay under 100. A long alpha costs nothing; if 1.0's
   conditions wait on something outside the code (a signing
   certificate), the freeze runs on as 0.9.x releases.
7. **`havne-cli`'s `schema` and the C ABI version separately** from the
   product: `schema` rises when a reader would break (ADR-0031), and
   the ABI's promises are ADR-0001's. The product's version is the one
   a user sees.

## Alternatives considered

- **Keep bumping PATCH (v0.3.7, v0.3.8, ...).** It needs no change, but
  the numbers run out of meaning: nothing tells a fix release from a
  feature release, and the plan would reach v0.3.20 before 1.0.
- **`AnyNewerVersion` for the CMake package while 0.x.** A consumer
  built against 0.4 would accept 0.5 without a rebuild, which holds
  only while the ABI never breaks. Semver lets a 0.x MINOR break it, the
  `abi` job already makes a break a named, deliberate act, and Cargo
  treats `^0.4` the same way (0.5 doesn't match), so the package says
  what semver says.
- **Alpha and beta as suffixes (`v0.5.0-alpha`).** They would collide
  with the `-rcN` dry runs the release workflow keys on and say nothing
  the MINOR number doesn't.

## Consequences

- The next release is v0.4.0, not v0.3.7; STATUS's and README's
  roadmaps carry the new numbers. Earlier ADRs name planned versions
  in the old numbering: their v0.4.0 Python bindings are v0.6.0's,
  their v0.4.0 discovery v0.8.0's; the v0.5.0 plugin framework keeps
  its number.
- Every feature release asks a CMake consumer to re-request the new
  MINOR; a fix release doesn't.
- 1.0.0 is a promise about the read side: after it, the C ABI,
  `havne-cli`'s schema and the plugin formats break only with a MAJOR
  release.
