# QtTune — Project Status

**Updated:** 2026-10-26 · **Version:** v0.3.2 ·
[![CI](https://github.com/erickramer42/qttune/actions/workflows/ci.yml/badge.svg)](https://github.com/erickramer42/qttune/actions/workflows/ci.yml)

This file covers the project as it stands: what works, what comes
next, and what is known to be wrong. For how the parts fit together,
start with [docs/architecture.md](docs/architecture.md). Why it is
built this way is in [docs/adr/](docs/adr/README.md). How it got
here, with lessons learned, is in
[docs/engineering-log.md](docs/engineering-log.md).
Safety, regulatory and warranty caveats are in
[DISCLAIMER.md](DISCLAIMER.md).

## Where things stand

**Current phase: v0.3.x, its plan complete.** v0.3.2 is out: replay,
decoding on the transport's thread, and every driver in a process of
its own; changes since then go under
[CHANGELOG.md's "Unreleased"](CHANGELOG.md#unreleased). The J2534 stack is verified
end to end against a fake vendor DLL, through the x64 and the x86
surrogate. One real J2534 adapter, the OBDX Pro VX, has run on a quiet
bench bus and in two read-only sessions on one car
([2026-10-22](docs/bench-sessions/2026-10-22-vehicle.md),
[2026-10-25](docs/bench-sessions/2026-10-25-vehicle.md)): the engine
ECU answered, the values decoded and tracked the cluster, and the
second session ran the request engine, the probe, identification and
`uds-generic` there. Every item in [After v0.3.0](#after-v030) has
shipped. The [ELM327 transport](#the-elm327-transport)
([ADR-0026](docs/adr/0026-elm327-transport.md)) is built and has read
the same car through the OBDLink MX+ over Bluetooth and the VX in ELM
mode over USB
([2026-10-27](docs/bench-sessions/2026-10-27-elm327-vehicle.md)); it
goes out in the next release. The milestone after it is chosen from the
[backlog](#backlog).

- **Works:**
  - **Mock transport,** on every platform: a scripted drive cycle (a
    cold start, two pulls onto the fuel-cut limiter, a cruise, a quarter
    mile off launch control) whose later loops run warm.
  - **Replay** ([ADR-0023](docs/adr/0023-replay-transport.md)), on every
    platform: `replay://<recording>` plays a recording's `frames.csv`
    back at its own pace or up to 100× faster.
  - **ELM327 adapters** in core, on every platform
    ([ADR-0026](docs/adr/0026-elm327-transport.md)): `elm327://<port>`
    on a serial port, USB or Bluetooth SPP, or at a WiFi adapter's
    address (`elm327://192.168.4.1:23`), with the same request
    engine, probe, identification and link health as J2534, and
    Settings lists the serial ports. Verified against a fake ELM327, on the bench with the OBDX VX, and
    on a car through the OBDLink MX+ (Bluetooth) and the VX in ELM mode
    (2026-10-27). A session that asks nothing listens with `AT MA` on
    its channels (fake-tested; the bench and a car with a second tester
    are to come).
  - **J2534 on Windows:** discovery of installed drivers, each driver
    loaded in a single-threaded surrogate process of its own
    architecture, x86 or x64, so a crashing driver ends its process,
    not the app ([ADR-0025](docs/adr/0025-one-j2534-backend.md),
    [ADR-0014](docs/adr/0014-single-threaded-surrogate.md)), and
    channels and filters derived from the profile, so a bare
    `j2534://` streams ([ADR-0012](docs/adr/0012-j2534-transport-adoption.md)).
  - **Requests follow the replies**
    ([ADR-0015](docs/adr/0015-request-engine.md)): one out per channel,
    the next when the reply is in, a 100 ms wait and a 20 ms floor;
    reads only (Service 01, 09 and 0x22). About 26 a second on the car.
  - **The PID-support probe** (ADR-0015 §4) drops only what an ECU says
    it lacks, never on silence; on the car it walked six bitmaps and
    dropped `01 5C`.
  - **Vehicle identification** at connect
    ([ADR-0021](docs/adr/0021-vehicle-identification.md)), and
    **`uds-generic`**, polling or scanning the DIDs a URI names
    ([ADR-0020](docs/adr/0020-uds-generic.md)).
  - **Profiles** chosen by the URI
    ([ADR-0013](docs/adr/0013-protocol-profiles.md)): `mock`, `raw`,
    `obd2` and `uds-generic`. Each signal def states its kind and byte
    order ([ADR-0016](docs/adr/0016-signal-definition-layout.md)).
  - **Connects explain themselves:** a failure says why, with the
    vendor's own text
    ([ADR-0018](docs/adr/0018-connect-failure-reporting.md)); a success
    logs a "Session:" line with the adapter's battery and versions
    ([ADR-0019](docs/adr/0019-session-info.md)). Connect and disconnect
    run off the GUI thread.
  - **Link health** tells a quiet bus, failing requests and a dead
    adapter apart on every connection surface
    ([docs/j2534.md](docs/j2534.md#session-lifecycle)); all four states
    have shown on the VX.
  - **The UI:** Dashboard gauges with peak hold and stale dimming,
    Live's table and strip chart, the Log page's activity log, and
    Settings composing the URI; developer mode (off until switched on:
    the `mock://` preset and the Log page's raw frame table with its
    bridge counters), zoom, metric or imperial units
    ([ADR-0022](docs/adr/0022-display-units.md)), the last URI that
    connected, the Record sessions switch and each device's versions as
    last seen at a connect (shown on its row in Settings, since the list
    opens no adapter), kept between runs.
  - **Vehicles** ([ADR-0027](docs/adr/0027-vehicle-profiles.md)): a
    named vehicle, picked in Settings before a connect, remembers its
    adapter URI and Live's plotted signals and names its recordings'
    folders; a salted hash of the VIN recognizes it at the next
    connect. Made, copied, renamed and forgotten in the app.
  - **Evidence for field sessions:** opt-in recording of every frame,
    decoded value and activity line per connect
    ([ADR-0017](docs/adr/0017-session-recording.md)), and a surrogate
    session log of every vendor call, with every request and reply
    while recording.
- **Not yet:** real hardware beyond two adapters and one car: 29-bit
  addressing, more than one ECU answering, a real x64 driver,
  and other vendors ([BENCH prep](docs/bench-sessions/bench-prep.md),
  [testing.md](docs/testing.md#gaps-not-verified)). The run sheets are
  [bench-runsheet.md](docs/bench-sessions/bench-runsheet.md) and
  [vehicle-runsheet.md](docs/bench-sessions/vehicle-runsheet.md).
- **Tests:** 468 ctest cases on Windows (both trees rebuilt,
  2026-10-27), all passing but two env-gated skips (the cross-arch
  case, and the real-car recordings' diff, run locally), plus the
  cross-arch subset (76/76); 0 MSVC warnings. CI runs ctest on Windows,
  macOS and Linux; core under ASan and UBSan; 60 s of each libFuzzer
  harness ([fuzz/README.md](fuzz/README.md)); clang-tidy and CodeQL;
  the load test on Windows and Linux; line coverage on Windows; the
  Android and iOS cross-builds; and, on pull requests, an iOS
  Simulator smoke. The badge above shows its state. Tiers, suites and
  gaps are in [docs/testing.md](docs/testing.md).

## Components

| Component | State | Reference |
|---|---|---|
| Core sessions and callbacks | Stable: snapshot dispatch, the 4,120-byte frame, close and destroy apart | [ADR-0002](docs/adr/0002-callback-pair-identity-snapshot-dispatch.md), [ADR-0006](docs/adr/0006-frame-abi-v0.3.md) |
| Signal decode | Linear scaling in `double` of unsigned, signed and binary32 fields in either byte order, gated on message ID and a mux key of up to 3 bytes; text and hex fields; a validator that accepts exactly what the decoder decodes | [ADR-0004](docs/adr/0004-signal-decoding-model.md), [ADR-0016](docs/adr/0016-signal-definition-layout.md) |
| Mock transport | A ~2-minute drive cycle on a vehicle model (`core/src/mock_drive_cycle.h`) whose readings stay consistent with each other, every tick round-tripped through the mock set (`CoreTest.MockDriveCycle*`) | — |
| Replay transport | UNIT-tested, against a scrubbed real-bus fixture too; passive, default profile `raw` | [ADR-0023](docs/adr/0023-replay-transport.md) |
| Bridge and models | Worker → GUI marshaling with bounded backpressure, 100 ms coalescing, a 10k-row frame log, a 360-point history per signal (each 100 ms window's lowest, highest and newest), link-health polling, recording; QML reaches it as the `Bridge` singleton | [ADR-0003](docs/adr/0003-backpressure-bounds-in-flight-frames.md), [ADR-0005](docs/adr/0005-coalesced-signal-updates.md) |
| QML pages | Home, Dashboard, Live, Log, Settings; `test_qml` runs them against a real bridge, offscreen | [docs/architecture.md](docs/architecture.md#the-ui-layer) |
| J2534 | HARNESS-verified through the surrogate: x64 in CI, x86 cross-arch. BENCH: one adapter, quiet bus. Vehicle: one car, read-only, through the surrogate, two sessions | [docs/j2534.md](docs/j2534.md) |

## After v0.3.0

The v0.3.x plan, in order. Items 1–4 shipped in v0.3.1, items 5 and
6 in v0.3.2; each ADR holds the design and the car's measurements.

1. **Request engine** ([ADR-0015](docs/adr/0015-request-engine.md)):
   built; on the car, about 26 requests a second, replies 19 ms after
   their request (median).
2. **The `obd2` PID-support probe** (ADR-0015 §4): built; on the car,
   six bitmaps, 53 PIDs, only `01 5C` dropped.
3. **`uds-generic`** ([ADR-0020](docs/adr/0020-uds-generic.md)):
   built; on the car, the `F180-F1FF` scan settled in 14.3 s.
4. **Vehicle identification**
   ([ADR-0021](docs/adr/0021-vehicle-identification.md)): built; on
   the car, every item settled.
5. **Phase 4, record-replay transport**
   ([ADR-0023](docs/adr/0023-replay-transport.md)): built and
   UNIT-tested, against a real-bus fixture too.
6. **Decoding on the worker thread**
   ([ADR-0024](docs/adr/0024-decode-on-the-worker.md)): built. Core
   decodes on the transport's thread, the bridge stages its values (a
   dropped frame keeps them), and the chart shows each value at its
   frame's time with each window's peak (ADR-0005's appendix); checked
   against the vehicle sessions' recordings, value for value.

## The ELM327 transport

`elm327://` reads a car through an ELM327-compatible adapter on a
serial port: USB, or classic Bluetooth paired as a port. That is the
route to a car on macOS and Linux, and on Windows without J2534
([ADR-0026](docs/adr/0026-elm327-transport.md), which holds the
design and the order). The adapters on hand are the OBDLink MX+
(MX201, an STN chip, powered only from the OBD connector) and the
OBDX Pro VX in its ELM mode (USB-powered). In order:

1. **Link health in a shared header** (`core/src/link_health.h`):
   built. The J2534 worker uses it unchanged, and its rules have
   session-free unit cases (`CoreTest.Link*`).
2. **The protocol layer** (`core/src/elm327_protocol.h`, no I/O, no
   clock): built, with its unit tests (`Elm327ProtocolTest`,
   `test_elm327`, Qt-free, so in every CI job).
3. **The transport** (`core/src/elm327_transport.cpp`,
   `elm327_port.cpp`): built. The worker drives the request engine with
   one command out for the whole adapter, sharing the channel and
   request setup with J2534 (`core/src/request_setup.h`); verified
   against a fake ELM327 behind the port (`Elm327SessionTest`, with
   the same ECU as the fake J2534 DLL, `tests/fake_ecu.h`) and, on
   Linux and macOS, the termios port over a pseudo-terminal
   (`Elm327PortTest`). The Win32 port has carried the VX's bench
   session, a cable pull included.
4. **The app:** built. Settings lists the serial ports
   (`ecuio_serial_ports`, `serial.h`: the registry or `/dev`, nothing
   opened) beside the J2534 interfaces and has an `elm327://` preset;
   the "Session:" line has an ELM327 form (`SerialPortsTest`,
   `BridgeTest`, `QmlTest.settings`). Each connect writes a wire log
   (`ecuio-elm327.<stamp>.log` in the temporary directory: the setup's
   answers always, every request and reply while recording;
   `Elm327SessionTest.TheWireLog*`), the bench's evidence.
5. **BENCH** with the VX on USB, then a read-only vehicle session.
   The VX's bench is done
   ([2026-10-27-elm327-bench](docs/bench-sessions/2026-10-27-elm327-bench.md),
   [2026-10-27-elm327-bench-2](docs/bench-sessions/2026-10-27-elm327-bench-2.md)):
   its `AT Z` answers `OK` alone, which the setup takes, asking `AT I`
   for the version; with no bus it answers `NO DATA`, so the bench
   reads quiet; it connects at both CAN rates and recovers from a
   cable pull with no reconnect. The vehicle session is done
   ([2026-10-27-elm327-vehicle](docs/bench-sessions/2026-10-27-elm327-vehicle.md)):
   the MX+ (Bluetooth, an STN2255) and the VX in ELM mode both read the
   car, decoding the J2534 sessions' values through a cold start and
   idle; the MX+ sustained ~32 requests/s over Bluetooth (the VX read
   26/s through J2534), and a Bluetooth drop recovered with no
   reconnect, the first value within ~6.5 s of Bluetooth coming back. The MX+ is vehicle-only (no 12 V supply), so its bench
   items ran on the car (ADR-0026's appendix of 2026-10-27). The run sheets are
   [elm327-bench-runsheet.md](docs/bench-sessions/elm327-bench-runsheet.md)
   and [elm327-vehicle-runsheet.md](docs/bench-sessions/elm327-vehicle-runsheet.md);
   the items they settle are BENCH prep #11–#17.

6. **Passive listening** (ADR-0026's appendix "passive listening"):
   built and fake-tested. A session with nothing to ask monitors with
   `AT MA`, which keeps the receive filter, so it hears its channels'
   IDs; it is checked every 5 s of silence, restarted when it ends, and
   stopped with a space at close. `BUFFER FULL` flags the next frame
   `ECUIO_FRAME_FLAG_OVERFLOW`, as a J2534 driver's
   `ERR_BUFFER_OVERFLOW` does (`Elm327SessionTest.Listening*` and
   neighbors, `J2534HealthTest.*Overflow*`). The VX ran it on the bench
   with no bus (BENCH prep #18,
   [2026-10-27-elm327-bench-3](docs/bench-sessions/2026-10-27-elm327-bench-3.md)):
   silent, checked every 5 s, stopped at close. Still to come: a car
   with a second tester on an OBD Y-splitter (#19).
7. **WiFi over TCP** (ADR-0026's appendix "WiFi over TCP",
   `core/src/elm327_tcp.cpp`): built. `elm327://<IPv4>:<port>` opens a
   socket where a serial port would be: a 2 s connect, Nagle off, no
   host names; the far side closing is a hard failure that reconnects
   on the backoff. Verified against the fake ELM327 behind a loopback
   socket on every platform (`Elm327TcpTest`). The VX's WiFi carried a
   session on the bench with no bus, its answers and timing those of
   its USB port plus a few ms
   ([2026-10-27-elm327-bench-4](docs/bench-sessions/2026-10-27-elm327-bench-4.md)).
   Leaving its network is a fault at once that recovers on the backoff
   with no reconnect, though the VX's network side hangs after a client
   leaves with its socket open, until a power cycle
   ([2026-10-27-elm327-bench-5](docs/bench-sessions/2026-10-27-elm327-bench-5.md)).
   A session on its USB port, J2534 or ELM, shuts its WiFi out
   ([2026-10-27-elm327-bench-6](docs/bench-sessions/2026-10-27-elm327-bench-6.md)):
   one link at a time, a power cycle before WiFi after USB.

The app-fed stream for Bluetooth on Android and BLE is
[ADR-0028](docs/adr/0028-app-fed-streams.md) (accepted; step 1, the
Android baseline, under way: the app runs on a phone, reaches the
VX over WiFi and replays a recording by a tap; the connected status
line and the window after a turn to confirm on a phone next). Later,
with its own record: listening to the whole bus (raw frames, for a
car whose OBD port carries more than diagnostics).

## BENCH prep

The assumptions in the J2534 code that only real hardware can confirm,
each with its evidence, are in
[docs/bench-sessions/bench-prep.md](docs/bench-sessions/bench-prep.md);
every session walks it. Items 1–3 and 5–10 are settled for the OBDX Pro VX.
Open: #4 (29-bit addressing, unexercised on the 11-bit car), what
sets the VX clock's zero (#5), and every item for any other adapter.

## Known issues and technical debt

These numbers are stable IDs, and code and docs refer to them. When
an issue is resolved it is removed, and its number is not reused.

2. **The mobile apps are unvalidated.** CI
   cross-builds an Android arm64-v8a APK and an unsigned iOS arm64
   device build. On pull requests and on request, it also launches
   the iOS app on the Simulator and screenshots it
   (`ios-simulator-smoke`). The Android app runs on a phone: the
   pages fit, `mock://` streams, `elm327://` reaches the VX over
   WiFi, and a recording replays by a tap on its row in Settings
   ([2026-10-28-android-3](docs/bench-sessions/2026-10-28-android-3.md)).
   Two phone fixes wait for a phone run: the connected status line
   wraps (a long URI ran Settings past the right edge upright), and
   the window follows the screen when the phone turns (it kept a
   size short of the screen, leaving bands,
   [2026-10-28-android-4](docs/bench-sessions/2026-10-28-android-4.md)).
   The iOS app hasn't
   run on a device, and no tests run on either (cross builds set
   `BUILD_TESTING=OFF`). Bluetooth on Android is
   [ADR-0028](docs/adr/0028-app-fed-streams.md). CI uploads the
   Android build's debug APK.
4. **Every def is matched against every frame, on the transport's
   worker** ([ADR-0024](docs/adr/0024-decode-on-the-worker.md)). A
   subscription can't trim that work: it is the chart's filter only,
   because the Dashboard reads every signal. Measured, it costs about
   1.4 to 2.2 µs a frame at 1000 defs (the figure varies by session),
   well inside the J2534 worker's 10 ms read slice, and the GUI
   thread's cost per frame no longer depends on it
   ([docs/testing.md](docs/testing.md#performance)). A per-signal enable
   mask can trim it if a CAN FD rate makes it matter.
6. **The session API is single-caller by contract.** The bridge is the
   only caller.
13. **The picker treats one registry entry as one device.** Multiple
    adapters behind one vendor DLL can't be told apart. They would be
    selected through `PassThruOpen`'s `pName` argument, which the
    loader passes as `nullptr`, meaning the default device.
20. **Pointer-bearing ioctls (`FAST_INIT`, `FIVE_BAUD_INIT`) are out of
    contract** and refused at both ends. That is fine for ISO 15765,
    since those are ISO 9141/KWP init calls. If a BENCH device needs
    them, give them a dedicated wire form rather than relaxing the
    whitelist.
29. **The iOS simulator smoke depends on Intel macOS runners.** Qt
    6.12's iOS simulator libraries are x86_64 only, so
    `ios-simulator-smoke` runs on `macos-15-intel`, where the simulator
    runs natively. GitHub has announced the end of Intel macOS runners
    for Fall 2027. The options after that are an arm64 runner with
    Rosetta plus the universal iOS platform (a large download on every
    run), or Qt's arm64 simulator libraries if Qt ships them. Apple is
    also winding down Rosetta, so the first option has its own end
    date.
37. **On Android, the app in the background leaves a gap.** Android
    pauses the app's GUI thread while it's in the background or the
    screen is off. The session stays connected and the transport's
    worker keeps reading, but the bridge's 100 ms windows stop, so
    Live's chart has a gap for that time and the activity log says
    nothing ([2026-10-28-android-2](docs/bench-sessions/2026-10-28-android-2.md)).
    The frames wait for the GUI thread: a recording through 30 s in
    the background and 30 s with the screen off held every frame and
    value ([2026-10-28-android-3](docs/bench-sessions/2026-10-28-android-3.md)).
    A longer pause drops them past the bridge's in-flight cap
    (ADR-0003), which `mock://` reaches in 50 s. A logger in
    a car runs with the screen off, so this needs a decision of its
    own (a foreground service, or recording off the GUI thread).

## Roadmap

| Version | Scope | Status |
|---|---|---|
| v0.1.0 | Skeleton: core ABI, Qt shell, responsive layout | ✅ Released |
| v0.2.0 | Callback API, mock transport, live frame log, CI | ✅ Released |
| v0.3.0 | Signal decoding, LiveView, J2534 transport, live vehicle read and logging | ✅ Released |
| v0.3.1 | Request engine, PID-support probe, `uds-generic`, vehicle identification, imperial units; the first Windows download | ✅ Released |
| v0.3.2 | Record-replay transport, decoding on the transport's thread, every driver in its own process | ✅ Released |
| v0.4.0 | Security access discovery, flash capability | Proposed |
| v0.5.0+ | Manufacturer plugin modules (ADR-0013's profile API) | Proposed |

Neither proposed milestone is scheduled; the next one is chosen from
the [backlog](#backlog). Flash support depends on discovering each
manufacturer's secure-gateway authentication, research tracked
outside this repository.

Legacy-bus support (ISO 9141-2/KWP, J1850 VPW/PWM) is a missing
workstream between the profile work and the manufacturer plugins:
pre-2008 cars (older Mazda, Ford and GM targets) need transport-layer
protocol support before any profile can serve them. Whether and when
it is built depends on which vehicles matter most; CAN-era cars need
none of it.

## Backlog

Unscheduled work, grouped by theme. Nothing here is committed to a
release: an item moves into [After v0.3.0](#after-v030) or the
roadmap above when it is scheduled. Defects and debt live under
[Known issues](#known-issues-and-technical-debt), not here.

Sizes are rough: **S** is a day or less, **M** a few days, **L** a
week or more or a design that needs an ADR first.

### Data capture

- **Recording, what's left (S).** ADR-0017's recorder writes a whole
  session per connect, and its `events.log` names the vehicle through
  the identity line ([ADR-0021](docs/adr/0021-vehicle-identification.md)).
  The folder is named by the picked vehicle as well as the date
  ([ADR-0027](docs/adr/0027-vehicle-profiles.md)). Still to do: copy
  selected Log rows for quick notes.
- **Triggered logging (M).** Record only while a condition holds
  (RPM over a threshold) or capture a window around a link fault.
  Needs a trigger model in core, similar in shape to the probe: data
  describing a condition, not code on the worker thread.

### User interface

- **Units of measure, per-signal overrides (S).** Metric or imperial
  for the whole UI is built ([ADR-0022](docs/adr/0022-display-units.md):
  converted in core by unit string, chosen on Settings, kept between
  runs). Still to do: one quantity in the other system (degrees F with
  km/h), which passes a unit system per row instead of one for the
  model.
- **Low-glare and night themes (M).** Amber and red-on-black schemes
  for driving at night.
  - `Theme` already centralizes the palette, but the analog gauge
    hard-codes its face, bezel and needle colors; they need Theme roles
    first.
  - A near-monochrome scheme must keep warning and danger
    distinguishable by more than hue, for example with a label or
    icon.
- **In-vehicle displays: Android Auto and CarPlay (L, research
  first).** A second front end over the same C ABI, which is what
  replaceable UI layers are for. Both platforms admit third-party apps
  only in approved categories with template-based screens, and Qt
  targets neither, so the front end would be native (Kotlin, Swift).
  Confirm that a live-gauge app qualifies before building anything.
  Out of scope: modifying or replacing a vehicle's own infotainment
  system.
- **User-configurable dashboards (L).** User-authored pages choosing
  from the gauge library. The gauges are already modular, but layout
  persistence is new ground.

### Vehicle coverage

- **Full standard OBD-II coverage (L).** Grow the `obd2` set toward
  the whole SAE J1979 Service 01 table, including PIDs that carry two
  values (oxygen sensors with fuel trims), which need two defs on one
  PID.
  - The probe drops the PIDs a car's bitmaps mark unsupported, so a
    bigger set costs only the ones the car has. Each kept PID still
    costs a request per pass, so the refresh policy ADR-0015 leaves
    open (rate classes, or a curated set beside a full one) comes with
    it.
  - Verify each PID's meaning and formula against a public J1979
    table. The fake's canned replies share the def's assumptions, so
    the tests can't catch a wrong PID.
- **A first manufacturer signal set (L).** One make's enhanced
  Service 22 data, such as transmission temperature, which the
  standard set doesn't include. Choose the make from the target
  vehicles. A def keyed `62 <DID>` already enrols its request and
  decodes the reply (ADR-0015, ADR-0016), so what's missing is the
  data: the DIDs and their formulas, which `uds-generic`'s `?did=`
  polling and range scan are for ([ADR-0020](docs/adr/0020-uds-generic.md)).
  It serves as the pilot for the v0.5.0 plugin modules.
- **Computed signals (M).** Values derived from other signals, such
  as boost pressure: intake manifold absolute pressure (PID 0x0B)
  minus barometric pressure (PID 0x33). Neither PID is in the `obd2`
  set yet. A def decodes one field of one frame, so this needs a
  derived-signal stage in core, which keeps the UI on final display
  values (rule 2 above). Verify both PIDs against a public J1979
  table before adding them.
- **Custom derived channels (M).** Beyond the fixed computed signals
  above: a user-facing expression per channel (differences, unit
  conversions, smoothing). Stays in core per rule 2, and is the same
  data-describing-arithmetic shape ADR-0015 uses for requests.

### Editing and write path (v0.4+; nothing below is scheduled)

Every item here needs its own ADR first, and the UDS services it raises
(0x27, 0x2E, 0x31, 0x34/0x36/0x37, 0x3D) carry emissions-law, warranty and
ABS/traction/stability exposure that the record has to cover. Design order
matters: the original-copy file format constrains everything after it.

- **The original copy (L; ADR first).** Read the calibration as it
  stands, store it as a versioned, checksum-addressed artifact, and
  keep it immutable. Every edited value shows against this shadow, so
  a restore path always exists. The recording is of data, not frames:
  session recordings today are frame logs, not a calibration image.
- **Table and curve editors (L; ADR first).** Editing ECU data means
  editing arrays: VE and spark tables (grid cells, 2D curves with
  draggable points, a 3D heatmap view). The signal model describes a
  single scalar; core needs a richer def type, the biggest schema
  change since the signal set, and the QML side needs editors the
  current page set has no gesture at.
- **Diff and undo (M).** Changed values highlighted against the
  original, per-value revert, global rollback. Cheap if the original
  copy exists; painful to retrofit if it doesn't.
- **Write-flow safety UX (M).** During any write: battery voltage
  visible, a do-not-power-off state, and a resumable or recoverable
  transfer whose recovery story is stated before the write starts.
  The surrogate's wedged-child latch bounds a hung read; a wedged
  write is a worse failure class and must never be silent.

### Transports and platforms

- **ELM327 adapters:** built; see [The ELM327 transport](#the-elm327-transport),
  which lists what comes later (BLE, the whole bus).
- **Legacy buses:** see the roadmap above.

### Performance

- **Full signal sets at full rate (L).** The goal is a per-PID refresh
  limited by the ECU's response time, with no per-frame GUI-thread
  cost. Requests already follow the replies
  ([ADR-0015](docs/adr/0015-request-engine.md)), and the frame log is
  a fixed ring of plain rows, about 2 µs a frame at its cap. Core
  decodes on the worker and the bridge stages its values
  ([ADR-0024](docs/adr/0024-decode-on-the-worker.md)), so the GUI
  thread's cost per frame doesn't grow with the profile and values
  survive frames dropped under load; the chart shows each value at its
  frame's time and keeps a window's peak. Left: the bridge's 4 KB copy
  per frame, the worker's largest per-frame cost up to about 500 defs
  (`bench_bridge`,
  [docs/testing.md](docs/testing.md#performance)).

### Toolchain

- **C++20 baseline (M; decide in an ADR first).** Low priority.
  - **Gains:** `std::span` for the buffer-and-length pairs at the
    loader and transport seams; `std::format` for log lines; and
    designated initializers, which GCC rejects under C++17.
  - **Costs:** older toolchains drop out (`std::format` needs GCC 13
    or a recent libc++), and every compiler CI uses (MSVC, GCC,
    AppleClang, the NDK's clang) has to be checked against the
    features adopted.
  - The public headers are C, so the ABI doesn't change.