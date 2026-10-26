# Changelog

What changed in each release, for people using QtTune. The format
follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and
versions follow [Semantic Versioning](https://semver.org/) (before
1.0, a minor version may break the C ABI). A release's full notes,
the text of its GitHub release, are in [docs/releases/](docs/releases/).
How each change was built, with its commits and the lessons along the
way, is in [docs/engineering-log.md](docs/engineering-log.md).

## [Unreleased]

### Added
- **Replay** ([ADR-0023](docs/adr/0023-replay-transport.md)):
  `replay://<recording>` plays a recording's `frames.csv` back on every
  platform at its own pace, at `?speed=` 0.1 to 100 or `max`, and with
  `?loop=1` round again. Its default profile is `raw`; a J2534
  recording decodes under `?profile=obd2`. The activity log says what's
  playing and when it ends, and the session stays open at the end. If
  the recording changes or disappears while it plays, playback stops
  and every page says "Replay stopped"; playing a recording never locks
  the file.
- Connect-failure text names a vendor's status in hex as well as
  decimal ("status 34 (0x22, vendor-specific)").

### Changed
- Every J2534 driver runs in a helper process of its own architecture
  ([ADR-0025](docs/adr/0025-one-j2534-backend.md)): 64-bit drivers in
  the new `ecuio-j2534-surrogate-x64.exe`, which ships beside
  `qttune.exe`, as 32-bit ones already did in
  `ecuio-j2534-surrogate.exe`. A driver that crashes ends its helper,
  not QtTune, and the session reports an adapter fault; every
  session's driver calls are in the surrogate's session log. A file
  that isn't a Windows DLL is refused with a plainer message.
- Decoding runs in core on the transport's thread, not in the UI
  ([ADR-0024](docs/adr/0024-decode-on-the-worker.md)): a frame dropped
  under load still delivers its values to the pages and the recording,
  and the UI's work per frame no longer grows with the profile.
- A raw session on an adapter listens to the engine ECU without
  `?filter=`, and `?profile=raw&poll=` asks it; Settings adds no
  `filter=` of its own.
- Live's strip chart places each value at the time its frame was
  received (a replay's own time), and keeps the highest and lowest
  value of every 100 ms window, so a short peak stays on the chart.

### Fixed
- Choosing raw for a replay in Settings no longer gives a URI the
  replay refuses.
- A replay that can't play its recording no longer says it failed to
  load a transport library.
- The dropped-frame count includes frames a fast replay drops while
  the connect completes.
- A URI with spaces around it, as pasted, connects.
- The loader refuses a relative DLL path, from the registry or a
  direct caller, before anything loads it.
- The strip chart stays sharp under zoom, and its labels fit when many
  traces meet a short plot.
- Live's signal list gives up rows on a short page, so the chart stays
  on screen.
- The Log page holds long replies without keeping their memory after
  short frames replace them.
- The first frame of a session is never lost to the connect.

## [0.3.1] - 2026-10-25

The first release with a Windows download
([full notes](docs/releases/v0.3.1.md)).

### Added
- A request engine that paces requests by the ECU's replies
  ([ADR-0015](docs/adr/0015-request-engine.md)), and a PID-support
  probe that stops asking for what the ECU says it lacks.
- Vehicle identification at connect: VIN, calibration IDs and the
  ECU's identification DIDs
  ([ADR-0021](docs/adr/0021-vehicle-identification.md)).
- The `uds-generic` profile: poll the DIDs a URI lists, or scan a range
  for the ones that answer ([ADR-0020](docs/adr/0020-uds-generic.md)).
- Frames carry a whole ISO 15765 message, up to 4,096 bytes.
- A failed connect says why, with the driver's own error text
  ([ADR-0018](docs/adr/0018-connect-failure-reporting.md)); a
  successful one names the adapter, its battery reading and versions
  ([ADR-0019](docs/adr/0019-session-info.md)).
- Metric or imperial display, chosen in Settings and remembered
  ([ADR-0022](docs/adr/0022-display-units.md)).
- Settings composes the URI from a preset, a profile row and a DID
  field.
- The simulated car: a scripted loop of pulls, a cruise and a quarter
  mile, warming over its first loops.

## [0.3.0] - 2026-10-22

### Added
- Signal decoding in core: linear scaling of unsigned, signed and
  float fields in either byte order, gated on message ID and mux key
  ([ADR-0004](docs/adr/0004-signal-decoding-model.md),
  [ADR-0016](docs/adr/0016-signal-definition-layout.md)).
- Profiles chosen by the URI: `mock`, `raw` (log only) and `obd2`
  (Service 01 PIDs on the engine ECU's channel)
  ([ADR-0013](docs/adr/0013-protocol-profiles.md)).
- LiveView with a strip chart, updated at 10 Hz whatever the bus rate
  ([ADR-0005](docs/adr/0005-coalesced-signal-updates.md)), and a
  Dashboard of gauges and shift lights.
- The J2534 transport on Windows: discovery of installed adapters,
  32-bit drivers through a surrogate process, and link health that
  tells a quiet bus, failing requests and a dead adapter apart
  ([ADR-0008](docs/adr/0008-x86-surrogate-host.md),
  [ADR-0012](docs/adr/0012-j2534-transport-adoption.md),
  [ADR-0014](docs/adr/0014-single-threaded-surrogate.md)).
- Session recording: every frame, decoded value and activity line per
  connect ([ADR-0017](docs/adr/0017-session-recording.md)).
- UI zoom, kept between runs.

### Changed
- The C ABI's names carry the `ecuio_` prefix; the frame struct and the
  close and destroy calls were redesigned
  ([ADR-0006](docs/adr/0006-frame-abi-v0.3.md)).

## [0.2.0] - 2026-10-05

### Added
- A session-scoped frame callback API with snapshot dispatch
  ([ADR-0002](docs/adr/0002-callback-pair-identity-snapshot-dispatch.md)).
- The mock transport, streaming synthetic CAN frames.
- A live frame log, fed through the bridge with bounded backpressure
  ([ADR-0003](docs/adr/0003-backpressure-bounds-in-flight-frames.md)).
- CI builds the app on Qt 6.12 LTS.

## [0.1.0] - 2026-09-30

### Added
- The skeleton: a Qt-free C++17 core behind a C ABI
  ([ADR-0001](docs/adr/0001-qt-free-core-c-abi.md)) and a Qt 6 / QML
  shell with a responsive layout.
- CI on Windows, macOS and Linux, running the core's tests.

[Unreleased]: https://github.com/erickramer42/qttune/compare/v0.3.1...HEAD
[0.3.1]: https://github.com/erickramer42/qttune/compare/v0.3.0...v0.3.1
[0.3.0]: https://github.com/erickramer42/qttune/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/erickramer42/qttune/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/erickramer42/qttune/releases/tag/v0.1.0
