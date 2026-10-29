# Changelog

What changed in each release, for people using Havne. The format
follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and
versions follow [Semantic Versioning](https://semver.org/) (before
1.0, a minor version may break the C ABI). A release's full notes,
the text of its GitHub release, are in [docs/releases/](docs/releases/).
How each change was built, with its commits and the lessons along the
way, is in [docs/engineering-log.md](docs/engineering-log.md).

## [Unreleased]

### Added
- **`havne-cli`**, a command-line client
  ([ADR-0031](docs/adr/0031-command-line-client.md)):
  `havne-cli stream <uri>` opens any session the app can (`mock://`,
  `replay://`, `elm327://`, `j2534://` on Windows) and writes its
  frames to standard output as JSON lines, one object per line, for
  `jq`, scripts and logging without a desktop; `--values` adds the
  values the profile decodes. It stops at Ctrl+C, after `--count`
  frames or `--seconds`, or at a replay's last frame, and always ends
  with an `end` line. `havne-cli devices` lists the serial ports and
  J2534 adapters a URI can name. On Windows it ships beside
  `havne.exe` in the zip.
- **SocketCAN, listening, on Linux**
  ([ADR-0032](docs/adr/0032-socketcan-listening.md)):
  `socketcan://can0` reads a native CAN interface (a candleLight or
  CANable adapter, PEAK, Kvaser, a Raspberry Pi's CAN hat) and sends
  nothing: every frame on the bus, or the IDs `?filter=` names, CAN FD
  and error frames included, timed by the kernel. Bring the interface
  up first, at the bus's bit rate and in listen-only mode
  (`sudo ip link set can0 up type can bitrate 500000 listen-only on`);
  Havne refuses one that could transmit and says how to fix it.
  Settings and `havne-cli devices` list the CAN interfaces. Requesting
  profiles (`obd2`, `uds-generic`) don't run on SocketCAN yet.
- **A simulated car on a virtual CAN bus:** `havne-cli play mock://demo
  --to vcan0` puts the simulated car's frames on Linux's `vcan0`, where
  `socketcan://vcan0?profile=mock` (or any SocketCAN tool) can listen,
  with no hardware. It writes only to virtual interfaces, never to a
  real bus.
- **A sleep says so:** when the computer or phone sleeps during a
  session, the activity log (and a recording's `events.log`) says
  "Asleep for N s: nothing was read meanwhile", and `havne-cli` writes
  a `slept` line.

### Fixed
- **A sleep on Linux or Android is a gap,** on Live, in a recording's
  times and in `havne-cli`'s stream, instead of the values on either
  side being joined as if nothing had been missed; values read before
  the sleep dim as stale at the wake
  ([ADR-0033](docs/adr/0033-session-time-counts-sleep.md)). Windows,
  macOS and iOS already counted a sleep.

## [0.3.3] - 2026-10-29

### Added
- An app icon on Windows, Android and iOS: the Home page's tachometer
  on the app's purple. On Windows it's `havne.exe`'s icon, in Explorer,
  the taskbar and the title bar; on Android it's an adaptive icon,
  shaped to fit whichever icon style the launcher uses.
- **ELM327 adapters** ([ADR-0026](docs/adr/0026-elm327-transport.md)):
  `elm327://<port>` reads a car through an ELM327-compatible adapter,
  such as an OBDLink or an OBDX Pro in its ELM mode, on a serial port:
  USB, or Bluetooth paired as a port (`COM5` on Windows, a `/dev/` path
  on macOS and Linux). It takes the `obd2` profile by default and the
  same query keys as `j2534://`, plus `?serial=` for a wired adapter's
  rate. A dropped Bluetooth link recovers on its own. Settings lists
  the machine's serial ports beside the J2534 interfaces, without
  opening any, and the activity log's "Session:" line names the
  adapter's firmware and the battery voltage. Each connect writes a
  wire log to the temporary folder (`ecuio-elm327.<date-time>.log`):
  every command and what the adapter answered, and, while recording,
  every request and reply with its timing. It has read a car through
  an OBDLink MX+ over Bluetooth and an OBDX Pro VX over USB.
- `elm327://` reaches a WiFi adapter by its address and port, for
  example `elm327://192.168.4.1:23` (an OBDX Pro) or
  `elm327://192.168.0.10:35000` (most clones), once the computer has
  joined the adapter's network. The address is typed: Havne doesn't
  search the network. A connection that drops is tried again on its
  own, as a serial port is.
- An `elm327://` session that asks nothing (`?profile=raw` without
  `?poll=`) listens: the adapter monitors the bus for the engine ECU's
  messages (or `?filter=`'s ID), silently, so another tool's
  conversation can be logged and recorded.
- When an adapter's buffer overflows and frames are lost (an ELM327's
  `BUFFER FULL`, a J2534 driver's `ERR_BUFFER_OVERFLOW`), the next frame
  says so in the recording's flags, the activity log says so once, and
  the Log page's frame view counts them.
- Settings remembers the last URI that connected, with its profile and
  DIDs, and the Record sessions switch between runs. A URI that failed
  to connect isn't kept.
- Settings' adapter list shows each device's firmware versions as last
  seen at a connect ("last seen: …"), without opening the adapter. A
  serial port shows whatever adapter answered on it last.
- Vehicles ([ADR-0027](docs/adr/0027-vehicle-profiles.md)). Settings'
  Vehicle row keeps named vehicles: pick one before connecting and it
  fills in that car's adapter, profile and DIDs, restores the signals
  plotted on Live, and names the session's recording folder after it.
  The VIN recognizes a saved car on the next connect, and an unknown
  one is saved as "Vehicle N". Only a salted hash of the VIN is stored.
  New, Duplicate, Rename, Forget the VIN, Forget and Forget all are
  buttons there, so nothing needs a file edited.
- Developer mode, a switch in Settings that's remembered between runs.
  It adds the `mock://demo` preset and the Log page's raw frame table,
  with its delivery counters; with it off, the app offers only real
  adapters.
- On Android (12 or later), Settings lists the phone's paired
  Bluetooth adapters, and a **Scan for BLE** button finds BLE ones.
  Selecting one puts `elm327://bt:<address>` or `elm327://ble:<address>`
  in the field, and the session runs over the phone's Bluetooth as it
  would over a serial port, reconnecting on its own when the link
  drops. Pairing stays in Android's Bluetooth settings, and Havne asks
  for the Nearby devices permission when the adapter list first opens.
  It has read a car through an OBDLink MX+.
- On Android, an adapter session keeps running in the background and
  with the screen off, recording included. While it's connected a
  notification says so ("Connected to …", "· recording"), with a
  small gauge in the status bar and no sound; a tap opens the app. Havne asks once, at the first adapter connect, to
  show notifications, and the phone stays awake (not its screen) for
  as long as the session lasts. A dropped classic Bluetooth link
  reconnects with one connection attempt at a time. It has held an
  OBDLink MX+ on a car through the Home screen and the screen off. A
  `mock://` or `replay://` session may pause while the app is in the
  background.
- **An Android download** beside the Windows one: each release carries
  `havne-<version>-android-arm64-v8a.apk`, for Android 12 or later,
  signed with Havne's release key
  ([ADR-0030](docs/adr/0030-signed-android-releases.md)), so each
  release installs over the one before and keeps the app's data. The
  notes give the key's certificate fingerprint, and the APK has its
  own build provenance, as the zip does.
- Settings lists the recent recordings, newest first. Selecting one
  puts its `replay://` address in the field, with the profile and DIDs
  it was recorded with, so a recording replays without typing its path
  (on a phone, the only way).

### Changed
- **QtTune is now Havne**: the program is `havne.exe`, the release
  zip `havne-<version>-windows-x64.zip`, and the repository
  `erickramer42/havne` (the old address redirects). The first
  launch copies QtTune's preferences, vehicles included, and
  Settings' recent recordings list the sessions in
  `Documents\QtTune\recordings` beside the new ones in
  `Documents\Havne\recordings`; neither is moved or deleted. On
  Android Havne is a new app (`io.github.erickramer42.havne`) that
  starts empty. The build option `QTTUNE_BUILD_CORE_ONLY` is
  `HAVNE_BUILD_CORE_ONLY`; the old name still works, with a warning,
  until v0.3.4.
- Settings opens on the connect bar and five folded sections, each
  heading saying what it holds now: Adapter (the devices found, then
  the WiFi addresses and other choices the list doesn't show), Vehicle,
  What to read, Recording (with the recent recordings to replay) and
  Display. Connect… on Home, Live or the header opens Settings with
  Adapter unfolded. A button too long for a phone's width wraps its
  text.
- Selecting a device, a recording or a preset in Settings scrolls back
  to the URI field and Connect when they're out of view, as on a phone,
  and the chosen row is outlined.
- With **Record sessions** on, a replay isn't recorded: it would copy
  the recording it plays. Settings' Record line and the activity log
  say so; the switch keeps its setting for the next live session.
- Live's chart draws a crowded trace's back-and-forth stretches as a
  faint band, the range it swings over edged by thin lines, so a signal
  that swings every moment (lambda through the rev limiter, on a
  phone's narrow chart) reads as one shape rather than a hatch; its
  ramps, steps and small wiggles stay a line. Ten signals get a color
  of their own before any repeats dashed.
- Settings' presets include the two usual WiFi adapter addresses,
  `elm327://192.168.4.1:23` (an OBDX Pro) and
  `elm327://192.168.0.10:35000` (most others), and the Log page has a
  Copy button for the whole activity log.
- The window's title bar is dark on Windows, to match the app; on
  Windows 11 it takes the header's color, so the two read as one bar.
- The Log page shows the activity log (every connect, link change and
  ECU line, time-stamped), which sat at the bottom of Settings. The raw
  frame table is behind its Activity | Frames switch in developer mode.
- Home's logo is a small tachometer whose needle idles and blips into
  the red.

### Fixed
- An ELM327 adapter whose reset answered twice, the second time late
  (an OBDLink MX+ over a new Bluetooth link), no longer fails the
  connect: the setup command that drew the late answer is sent again.
- A supported-PID query answered with another PID's reply is asked
  once more, instead of the session polling without knowing which
  PIDs the car supports.
- Choosing a preset or a device of another kind in Settings no longer
  carries options it can't use into the URI, which failed the next
  connect (a replay's `profile=mock` or `speed=` on an adapter): keys
  that kind doesn't take are dropped, and the profile row shows Default
  in place of the mock's. One of the same kind keeps them all.
- On a phone, the app fills the screen after the phone turns, where
  bands showed at the right and the bottom.
- On a phone, connecting a long URI no longer pushes Settings past the
  screen's right edge: the "Connected:" line wraps.
- A WiFi adapter that can't be reached because this computer left its
  network says so ("the connection was cut off on this computer's
  side"), instead of a Windows sockets error number.
- Settings and Home fit a narrow window: Settings' rows of buttons wrap
  instead of pushing Connect off the right edge, and Home scrolls when
  the window is shorter than it. On a short window the Dashboard's
  dials fit the page.
- On a phone, the URI field no longer capitalizes or predicts what's
  typed into it, and Settings no longer offers an Open folder button
  that no app can open.
- A J2534 driver reporting a buffer overflow on a read no longer reads
  as "Adapter fault" with the read's messages dropped: the messages
  arrive, and the session carries on.
- Over an ELM327, a Service 01 PID whose reply spans several CAN frames
  (engine run time, 0x7F, for one) is read: it was asked with a
  response count that ended the reply after its first frame, so every
  request timed out. Only a profile you register can ask one; the
  built-in `obd2` doesn't.
- On Android, nothing of Havne's goes into the phone's backup or a
  transfer to a new phone: its preferences and recordings can hold the
  vehicle's VIN.
- The phone's session notification, and the header's connection label,
  show a Bluetooth adapter's address only by its last two parts
  (`elm327://bt:…:44:55`), so a screenshot or the lock screen doesn't
  carry the whole address.

## [0.3.2] - 2026-10-26

Replay, decoding in core, and every driver in a process of its own
([full notes](docs/releases/v0.3.2.md)).

### Added
- **Replay** ([ADR-0023](docs/adr/0023-replay-transport.md)):
  `replay://<recording>` plays a recording's `frames.csv` back on every
  platform at its own pace, at `?speed=` 0.1 to 100 or `max`, and with
  `?loop=1` round again (a recording whose frames all share one time
  plays once). Its default profile is `raw`; a J2534
  recording decodes under `?profile=obd2`. The activity log says what's
  playing and when it ends, and the session stays open at the end. If
  the recording changes or disappears while it plays, playback stops
  and every page says "Replay stopped"; playing a recording never locks
  the file.
- Connect-failure text names a vendor's status in hex as well as
  decimal ("status 34 (0x22, vendor-specific)").
- The Windows executables carry a version resource, so Explorer's
  Details tab names QtTune and its version.
- The Windows download's signed build provenance is a release file
  too (`<zip>.intoto.jsonl`), so the zip can be checked with
  `gh attestation verify <zip> --bundle <zip>.intoto.jsonl -R erickramer42/qttune`.

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
- At disconnect the activity log, and a recording's `events.log`, give
  the session's delivery: frames received and dropped, and how long
  frames took to reach the app and values to reach its pages (median,
  99th percentile and maximum).
- Live's strip chart places each value at the time its frame was
  received (a replay's own time), and keeps the highest and lowest
  value of every 100 ms window, so a short peak stays on the chart.

### Fixed
- A link failure's activity-log line always counts its own episode:
  one could read "adapter fault (0 episode(s))" and repeat with 1.
- The Dashboard's held peaks and lows catch a value between two
  refreshes, as the strip chart does: the engine start replayed fast
  had shown a peak of 1,195 rpm against the recording's 1,261.
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

[Unreleased]: https://github.com/erickramer42/havne/compare/v0.3.3...HEAD
[0.3.3]: https://github.com/erickramer42/havne/compare/v0.3.2...v0.3.3
[0.3.2]: https://github.com/erickramer42/havne/compare/v0.3.1...v0.3.2
[0.3.1]: https://github.com/erickramer42/havne/compare/v0.3.0...v0.3.1
[0.3.0]: https://github.com/erickramer42/havne/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/erickramer42/havne/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/erickramer42/havne/releases/tag/v0.1.0
