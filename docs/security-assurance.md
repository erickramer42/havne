# Security assurance case

Why Havne's design and checks are enough for what it does, and where
they stop. How to report a vulnerability is in
[SECURITY.md](../SECURITY.md).

## Threat model

Havne is a desktop program that loads third-party J2534 drivers,
reads a vehicle's diagnostic replies, and replays recordings from
disk. It opens no network port.

- **Assets:** the integrity of the host and the user's account; the
  vehicle, which must never be sent anything but a read request; and
  the user's vehicle data (VIN, calibration IDs) and local paths.
- **Adversaries:**
  - a faulty or malicious vendor DLL;
  - a misbehaving adapter or ECU, sending malformed or oversized
    replies;
  - a crafted recording or URI;
  - another local process or user trying to reach the surrogate's
    pipe;
  - a compromised build dependency or CI action.
- **Out of scope:** writing to an ECU (there is no write path), network
  attackers (Havne is local only), and an attacker who already runs
  code as the user or as an administrator (they can replace the driver
  or the app outright).

## Trust boundaries

| Boundary | What crosses it | Defense |
|---|---|---|
| Registry or URI → loader | A vendor DLL path | A relative path is refused before anything opens the file; the PE header is read first, and a file that isn't a Windows DLL is refused before any process starts ([ADR-0025](adr/0025-one-j2534-backend.md)). Driver paths come from `HKLM`, which only an administrator can write |
| App → surrogate process | Every vendor call, over a named pipe | The vendor DLL runs in a separate process of its own architecture, so a crash or corruption there ends that process, not the app. The pipe has an unguessable name, is created first-instance-only, rejects remote clients and carries a DACL for the current user alone; the child inherits the session log's handle and no other, and runs in a `KILL_ON_JOB_CLOSE` job object. A build fingerprint of the shared headers is checked at open ([j2534.md](j2534.md#surrogate)) |
| Surrogate ↔ app wire | Ioctls, reads and writes as flat structs | Both ends hold the same ioctl whitelist, and pointer-bearing ioctls are refused at both ([ADR-0011](adr/0011-vendor-call-relay-wire-forms.md)); a batch or length past the wire limit is refused |
| Vendor driver → transport | Message counts and payloads | A count reported under an error status is never trusted (`ecuio_trusted_count`); a message longer than a frame's 4,096 payload bytes is cut there and flagged `ECUIO_FRAME_FLAG_TRUNCATED` |
| Frame → decoder | Arbitrary payload bytes | The decoder reads nothing past `payload_len`, writes nothing unless it succeeds, and returns only defined codes, which libFuzzer checks on every input in CI ([fuzz/README.md](../fuzz/README.md)) |
| Caller → profile registry | Signal definitions | The set validator accepts exactly what the decoder can decode, requires reserved bytes to be zero, checks the layout version and rejects duplicate IDs ([ADR-0016](adr/0016-signal-definition-layout.md)); also fuzzed |
| User → session | A URI | One strict query grammar for every scheme: an unknown scheme, key or profile is refused at create, before any DLL loads; `uds-generic`'s `?did=` parser is fuzzed |
| Disk → replay | A recording's `frames.csv` | A row longer than a frame holds is refused; a file that changes during playback stops it ([ADR-0023](adr/0023-replay-transport.md)) |
| App → vehicle | Requests | The request engine sends services 0x01, 0x09 and 0x22 (reads) only, and refuses any other service even if one reaches it ([ADR-0015](adr/0015-request-engine.md) §3) |
| Source → release | The Windows zip | Actions pinned to commit SHAs, a hash-pinned clang-tidy wheel, a release job that requires the commit's CI to have passed and checks the package's imports and version resources, and a signed SLSA build provenance on every download ([development.md](development.md#releases)) |

## Design principles applied

| Principle | Where it shows |
|---|---|
| Least privilege | Every workflow defaults to read-only permissions, and a job gets more only for its one task: code-scanning uploads, or the release job's release and attestation writes. The surrogate's pipe admits the current user only |
| Isolation | Every vendor DLL in a surrogate process of its own; the core never links Qt, and the UI never parses a frame |
| Fail-safe defaults | Anything unrecognized (scheme, query key, profile, DLL type, ioctl, service) is refused, never guessed at |
| Complete mediation | Every path to a vendor call goes through the loader's checks and the surrogate's whitelist; there is no in-process backend to bypass them |
| Bounded resources | A fixed in-flight frame cap, a fixed-size frame log, and workers that allocate nothing per frame (tested) |
| Stable interfaces | An append-only C ABI: status codes, enum values and opcodes are never renumbered or reused |

## Assurance evidence

- **Tests:** the Unit, Bridge, QML and HARNESS tiers run in CI on
  Windows, macOS and Linux; the HARNESS tier drives the whole J2534
  stack, surrogate included, against a fake vendor DLL
  ([testing.md](testing.md)).
- **Dynamic analysis:** ASan, UBSan and LeakSanitizer over the core
  and its tests; three libFuzzer harnesses, 60 s each, on every push.
- **Static analysis:** clang-tidy's analyzer and bug-finding checks
  over the core and the surrogate, failing CI on any warning; CodeQL
  on pushes, pull requests and weekly.
- **Supply chain:** OpenSSF Scorecard weekly, Dependabot for the
  pinned actions, and build provenance verifiable with
  `gh attestation verify`.

## Known gaps

- **The pipe's protections have no direct test.** Every surrogate test
  exercises them, but nothing tries to connect as another user, squat
  the name or look for a leaked handle ([testing.md](testing.md#gaps-not-verified)).
- **The surrogate isn't sandboxed beyond its process.** A vendor DLL
  runs with the user's rights, as it would in any J2534 tool; the
  surrogate contains its crashes, not its privileges.
- **The client's handling of a misbehaving child is only partly
  tested.** Its checks on oversized batch and ioctl lengths exist, but
  no test child sends one.
- **The executables aren't code-signed,** so their integrity on disk
  rests on the provenance and the checksum, which a user has to check.
