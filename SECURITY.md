# Security policy

## Supported versions

QtTune is pre-release software. Security fixes go into the latest
release only.

| Version | Supported |
|---|---|
| Latest v0.3.x release | ✅ |
| Anything older | ❌ |

Once v1.0.0 is released, this table will give each release line's
support window.

## Reporting a vulnerability

Report a suspected vulnerability privately through **GitHub Security
Advisories**, not in a public issue:

1. Go to https://github.com/erickramer42/qttune/security/advisories/new.
2. Describe the issue, with steps to reproduce it and the version or
   commit.

Leave vehicle data out of the report: VINs, calibration IDs, adapter
serial numbers and local paths (CONTRIBUTING.md, "Handling vehicle
data").

**What to expect:**
- **Acknowledgment** within 7 days.
- **A status update** on remediation within 30 days.
- **Public disclosure** once a fix is available, unless an ongoing
  investigation needs more time.

## What counts

The threat model, the trust boundaries and the defenses at each are in
[docs/security-assurance.md](docs/security-assurance.md). In scope:

| Category | Examples |
|---|---|
| Memory safety | A frame, a recording, a vendor reply or a URI that makes QtTune read or write out of bounds |
| Host integrity | A way for another process or user to reach the surrogate's pipe, or for a DLL to load from somewhere it shouldn't |
| Supply chain | An unpinned action or dependency, or a release whose provenance doesn't verify |
| Privacy | Vehicle identity or local paths written anywhere the user didn't choose to record them |

Out of scope:
- **Vulnerabilities in vendor J2534 drivers or adapter firmware.**
  Report those to their manufacturers. QtTune isolates each driver in
  a process of its own so that a faulty driver can't take the app
  down, but it doesn't vouch for the driver.
- **Vehicle-side security** (security access, gateways, emissions
  controls). QtTune sends read-only diagnostic requests and has no
  write path; reports about bypassing a manufacturer's protections
  aren't handled as QtTune vulnerabilities. See
  [DISCLAIMER.md](DISCLAIMER.md).

## Security practices

- **CI on every push:** AddressSanitizer and UndefinedBehaviorSanitizer
  over the core, libFuzzer over the decoder, the validators and the
  URI parser, clang-tidy's analyzer checks, and CodeQL
  ([docs/testing.md](docs/testing.md)).
- **Supply chain:** `main` is protected; workflows run with
  least-privilege permissions; every action is pinned to a full commit
  SHA, and Dependabot proposes updates; OpenSSF Scorecard runs weekly.
- **Releases:** each Windows download carries a signed SLSA build
  provenance from the release workflow, attached to the release as
  `<zip>.intoto.jsonl`, and a SHA-256 checksum
  ([docs/development.md](docs/development.md#releases)).
- **Not yet:** the executables aren't code-signed, so Windows
  SmartScreen warns on first run.
