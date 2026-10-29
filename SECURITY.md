# Security policy

## Supported versions

Havne is pre-release software. Security fixes go into the latest
release only.

| Version | Supported |
|---|---|
| Latest v0.3.x release | ✅ |
| Anything older | ❌ |

A release is what the
[releases page](https://github.com/erickramer42/havne/releases) offers:
the Windows zip and, from v0.3.3, the Android APK signed with the
release key. The APK that CI builds on each push is a debug-signed
development build, not a release; report what you find in it all the
same.

Once v1.0.0 is released, this table will give each release line's
support window.

## Reporting a vulnerability

Report a suspected vulnerability privately through **GitHub Security
Advisories**, not in a public issue:

1. Go to https://github.com/erickramer42/havne/security/advisories/new.
2. Describe the issue, with steps to reproduce it and the version or
   commit, the platform, and the adapter or connection involved.

Leave vehicle data out of the report: VINs, calibration IDs, ECU and
adapter serial numbers, adapter Bluetooth and MAC addresses, and local
paths ([CONTRIBUTING.md, "Handling vehicle data"](CONTRIBUTING.md#handling-vehicle-data)).
A reply that reproduces the problem can be described by its shape, or
made up, rather than copied from a car.

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
| Memory safety | A frame, a recording, a vendor reply, an adapter's reply (over serial, WiFi or Bluetooth) or a URI that makes Havne read or write out of bounds, hang, or crash |
| Host integrity | A way for another process or user to reach the surrogate's pipe, for a DLL to load from somewhere it shouldn't, or for another app on a phone to drive Havne's session or its background service |
| Reach | Havne talking to anything but the adapter the user chose: a host-name lookup, a listening port, a connection the URI didn't name |
| Requests | A path that sends the vehicle anything but the read-only services (0x01, 0x09, 0x22), whatever an adapter or ECU replies |
| Privacy | Vehicle identity or local paths written anywhere the user didn't choose to record them, or sent off the device by Havne |
| Supply chain | An unpinned action or dependency, or a release whose provenance doesn't verify |

Out of scope:
- **Vulnerabilities in vendor J2534 drivers or adapter firmware.**
  Report those to their manufacturers. Havne isolates each driver in
  a process of its own so that a faulty driver can't take the app
  down, but it doesn't vouch for the driver.
- **An adapter's own exposure.** A WiFi adapter's access point
  usually has no password, so anyone in range can join it, and the
  ELM327 command set has no authentication over any link. That is the
  adapters' design; Havne parses whatever they send as untrusted, and
  the fix on the user's side is to unplug an adapter that isn't in
  use.
- **Vehicle-side security** (security access, gateways, emissions
  controls). Havne sends read-only diagnostic requests and has no
  write path; reports about bypassing a manufacturer's protections
  aren't handled as Havne vulnerabilities. See
  [DISCLAIMER.md](DISCLAIMER.md).

## Security practices

- **CI on every push:** AddressSanitizer and UndefinedBehaviorSanitizer
  over the core; libFuzzer over the decoder, the validators, the URI
  parser, the ELM327 reply parsers and the replay reader; clang-tidy's
  analyzer checks; and CodeQL ([docs/testing.md](docs/testing.md)).
- **Supply chain:** `main` is protected; workflows run with
  least-privilege permissions; every action is pinned to a full commit
  SHA, and Dependabot proposes updates; OpenSSF Scorecard runs weekly.
- **Releases:** each download carries a signed SLSA build provenance
  from the release workflow, attached to the release as
  `<download>.intoto.jsonl`, and a SHA-256 checksum. The APK is signed
  with the release key, which only the release workflow's Android job
  can read, from an environment that only release tags and `main`
  deploy to, and whose certificate fingerprint is pinned in the
  repository and given in each release's notes
  ([ADR-0030](docs/adr/0030-signed-android-releases.md),
  [docs/development.md](docs/development.md#releases)).
- **Android:** the app asks only for the permissions its features use
  (Bluetooth declared "never for location", notifications, a
  connected-device foreground service and a wake lock for background
  sessions, and internet for a WiFi adapter's socket), its service
  isn't exported to other apps, and its files stay out of the phone's
  backup and device-to-device transfer.
- **Not yet:** the Windows executables aren't code-signed, so
  SmartScreen warns on first run.
