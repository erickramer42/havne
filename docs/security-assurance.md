# Security assurance case

Why Havne's design and checks are enough for what it does, and where
they stop. How to report a vulnerability is in
[SECURITY.md](../SECURITY.md).

## Threat model

Havne is a desktop and Android app that loads third-party J2534
drivers on Windows, talks to ELM327-compatible adapters over a serial
port, a WiFi adapter's TCP socket or (on a phone) Bluetooth, reads a
vehicle's diagnostic replies, and replays recordings from disk. It
listens on no port and looks up no host name: its only network
traffic is a TCP connection out to the IPv4 address and port a URI
names, a WiFi adapter's.

- **Assets:** the integrity of the host and the user's account; the
  vehicle, which must never be sent anything but a read request; and
  the user's vehicle data (VIN, calibration IDs) and local paths.
- **Adversaries:**
  - a faulty or malicious vendor DLL;
  - a misbehaving adapter or ECU, sending malformed or oversized
    replies;
  - something posing as the adapter: a device on a WiFi adapter's
    network (whose access point usually has no password), or a
    Bluetooth device the user paired or picked from a scan;
  - a crafted recording or URI;
  - another local process or user trying to reach the surrogate's
    pipe, or another app on the phone trying to reach the session;
  - a compromised build dependency or CI action.
- **Out of scope:** writing to an ECU (there is no write path),
  attackers beyond the adapter's own link (Havne serves nothing and
  reaches nothing but the adapter), and an attacker who already runs
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
| User → session | A URI | One strict query grammar for every scheme: an unknown scheme, key or profile is refused at create, before any DLL loads or any port opens; `uds-generic`'s `?did=` parser is fuzzed |
| User → ELM327 port | A port name or address | `COM1` to `COM255` on Windows, a `/dev/` path elsewhere, a dotted IPv4 address with a port (no host names, so nothing is looked up), or the name of a stream the app registered; anything else is refused at create ([ADR-0026](adr/0026-elm327-transport.md)). The socket and the serial handle aren't inherited by the surrogate |
| Adapter → ELM327 transport | Text lines over serial, TCP or Bluetooth | A pure parser (`core/src/elm327_protocol.h`, no allocation) keeps each line to a fixed length and flags a cut one, counts a frame line's digits before indexing it, and reassembles ISO-TP within what its First Frame said and 4,095 bytes; a setup answer that isn't the expected one refuses the connect. Fuzzed (`fuzz_elm327_reply`). Whatever comes back, the requests stay reads |
| App's Bluetooth → core | Bytes from the phone's link, through `ecuio/stream.h` | The same parser behind a fixed 64 KB buffer; bytes past it fail the link rather than resynchronize mid-reply, and nothing calls back into the app under a core lock ([ADR-0028](adr/0028-app-fed-streams.md)) |
| Kernel → SocketCAN transport | Frames, and an interface's netlink description | Listening only: a session refuses an interface that could transmit (a controller not in listen-only mode) before it opens a socket, and never writes to one ([ADR-0032](adr/0032-socketcan-listening.md)); `havne-cli play` writes only to a virtual interface. Pure parsers (`core/src/socketcan_wire.h`) read the netlink attributes within their stated lengths and keep the name and kind terminated; `?filter=` takes at most 8 IDs, each within its width. An error frame is reported and never decoded as values. Fuzzed (`fuzz_socketcan_wire`) |
| Android → app | Other apps, the system | The background session's service isn't exported; the permissions are those the features use: Bluetooth connect and scan declared "never for location", notifications, a connected-device foreground service, a wake lock, and internet for the WiFi socket ([ADR-0029](adr/0029-android-background-sessions.md)). The app's files, which can hold the VIN, are kept out of the system's backup and device-to-device transfer (`allowBackup` off and data-extraction rules that exclude every domain; CI checks the manifest for both) |
| Disk → replay | A recording's `frames.csv` | The whole file is checked at connect, and a row that doesn't parse or is longer than a frame holds is refused, naming its line but never the path; a file that changes during playback (a row that no longer parses, fewer rows) stops it, and rows added since are never played ([ADR-0023](adr/0023-replay-transport.md)). Fuzzed (`fuzz_replay_file`) |
| App → vehicle | Requests | The request engine sends services 0x01, 0x09 and 0x22 (reads) only, and refuses any other service even if one reaches it ([ADR-0015](adr/0015-request-engine.md) §3) |
| Source → release | The downloads: the Windows zip and its symbols, the Android APK, the Linux AppImage and the core packages | Actions pinned to commit SHAs, a hash-pinned clang-tidy wheel, a release workflow that requires the commit's CI to have passed, checks the zip's imports and version resources and the APK's package, version and debuggability, and starts the AppImage offscreen, a signed SLSA build provenance on every download, and an SPDX SBOM of what each download contains, attested over them ([development.md](development.md#releases)). The core packages build to the same bytes in CI under varied folders, clocks and locales ([ADR-0035](adr/0035-linux-downloads-sbom-reproducible.md)). The APK is signed with the release key, which only the Android job reads, from a `release` environment that only `v*` tags and `main` deploy to; the signing step fails unless the certificate is the one pinned in the repository ([ADR-0030](adr/0030-signed-android-releases.md)) |

## Design principles applied

| Principle | Where it shows |
|---|---|
| Least privilege | Every workflow defaults to read-only permissions, and a job gets more only for its one task: code-scanning uploads, or the release workflow's release and attestation writes, in its last job only; only its Android job reads the signing key. The surrogate's pipe admits the current user only. The Android app declares only the permissions its features use |
| Isolation | Every vendor DLL in a surrogate process of its own; the core never links Qt, and the UI never parses a frame |
| Fail-safe defaults | Anything unrecognized (scheme, query key, profile, port name, DLL type, ioctl, service, an adapter's setup answer) is refused, never guessed at |
| Complete mediation | Every path to a vendor call goes through the loader's checks and the surrogate's whitelist; there is no in-process backend to bypass them |
| Bounded resources | A fixed in-flight frame cap, a fixed-size frame log, and workers that allocate nothing per frame (tested) |
| Stable interfaces | An append-only C ABI: status codes, enum values and opcodes are never renumbered or reused |

## Assurance evidence

- **Tests:** the Unit, Bridge, QML and HARNESS tiers run in CI on
  Windows, macOS and Linux; the HARNESS tier drives the whole J2534
  stack, surrogate included, against a fake vendor DLL, and the ELM327
  transport against a fake ELM327 behind its port, a loopback socket
  and an app stream ([testing.md](testing.md)).
- **Dynamic analysis:** ASan, UBSan and LeakSanitizer over the core
  and its tests; libFuzzer over the decoder, the validators, the
  `?did=` parser, the ELM327 reply parsers and the replay reader, 60 s
  each, on every push.
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
  no test child sends one, and no fuzzer reaches them: the client
  compiles only on Windows, against a live pipe, and the fuzz job runs
  on Linux ([testing.md](testing.md#gaps-not-verified)).
- **An adapter can't be authenticated.** Nothing in the ELM327
  command set proves which device answers, and a WiFi adapter's open
  access point lets anyone in range join it. The parser treats every
  reply as untrusted and the requests stay reads, so the exposure is
  the session's data: a device posing as the adapter can feed it false
  values.
- **The Windows executables aren't code-signed,** so their integrity
  on disk rests on the provenance and the checksum, which a user has to
  check.
- **The Android release key is one key, kept offline.** A leaked key
  could sign an update that phones accept; APK Signature Scheme v3 can
  rotate to a new key while the old one exists, but a lost key can't be
  replaced without every phone uninstalling the app
  ([ADR-0030](adr/0030-signed-android-releases.md)).
