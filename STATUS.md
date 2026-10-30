# Havne — Project Status

**Updated:** 2026-10-29 · **Version:** v0.3.3 ·
[![CI](https://github.com/erickramer42/havne/actions/workflows/ci.yml/badge.svg)](https://github.com/erickramer42/havne/actions/workflows/ci.yml)

This file covers the project as it stands: what works, what comes
next, and what is known to be wrong. For how the parts fit together,
start with [docs/architecture.md](docs/architecture.md). Why it is
built this way is in [docs/adr/](docs/adr/README.md). How it got
here, with lessons learned, is in
[docs/engineering-log.md](docs/engineering-log.md).
Safety, regulatory and warranty caveats are in
[DISCLAIMER.md](DISCLAIMER.md).

## Where things stand

**Current phase: v0.3.4, under way.** v0.3.3 is out: ELM327
adapters, persisted preferences and vehicles, the Android app as a
signed download, and the name Havne; changes since then go under
[CHANGELOG.md's "Unreleased"](CHANGELOG.md#unreleased). The J2534 stack is verified
end to end against a fake vendor DLL, through the x64 and the x86
surrogate. One real J2534 adapter, the OBDX Pro VX, has run on a quiet
bench bus and in two read-only sessions on one car
([2026-10-22](docs/bench-sessions/2026-10-22-vehicle.md),
[2026-10-25](docs/bench-sessions/2026-10-25-vehicle.md)): the engine
ECU answered, the values decoded and tracked the cluster, and the
second session ran the request engine, the probe, identification and
`uds-generic` there. The [ELM327 transport](docs/elm327.md)
([ADR-0026](docs/adr/0026-elm327-transport.md)) is built and has read
the same car through the OBDLink MX+ over Bluetooth and the VX in ELM
mode over USB
([2026-10-27](docs/bench-sessions/2026-10-27-elm327-vehicle.md)), and
the Android app reaches the car through the phone's Bluetooth, in the
background too. Next come v0.3.4 and v0.3.5 ([roadmap](#roadmap)).

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
    its channels (on the VX's bench; a car with a second tester is to
    come). How it works: [docs/elm327.md](docs/elm327.md).
  - **Android** ([ADR-0028](docs/adr/0028-app-fed-streams.md),
    [ADR-0029](docs/adr/0029-android-background-sessions.md)): the app
    on Android 12 and later reaches a Bluetooth adapter through the
    phone (classic SPP, or the BLE service it picks), and an adapter
    session keeps running in the background and with the screen off,
    under a foreground service with a wake lock and a notification. On
    the car through the OBDLink MX+
    ([2026-10-28-android-8](docs/bench-sessions/2026-10-28-android-8.md),
    [2026-10-28-android-11](docs/bench-sessions/2026-10-28-android-11.md),
    [2026-10-29-android](docs/bench-sessions/2026-10-29-android.md)); no
    BLE adapter has run.
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
  - **`havne-cli`** ([ADR-0031](docs/adr/0031-command-line-client.md)),
    on every desktop platform: a session's frames, and with `--values`
    its decoded values, as JSON lines on standard output, through the
    public headers alone. Golden runs pin its output: the replay
    fixture through `obd2` frame for frame, the mock's cycle and the
    refusals; a stop from outside ends it cleanly (SIGINT and SIGTERM
    tested on Linux and macOS, Ctrl+Break on Windows), and so does a
    reader closing the pipe (`| head`).
  - **SocketCAN, listening** ([ADR-0032](docs/adr/0032-socketcan-listening.md)),
    on Linux: `socketcan://can0` reads a CAN interface's whole bus (or
    `?filter=`'s IDs) with the kernel's receive times, CAN FD and error
    frames included, and sends nothing. Core reads the interface and
    never configures it: a controller that isn't in listen-only mode is
    refused, with the `ip link` commands that make it so. Settings and
    `havne-cli devices` list the interfaces. `havne-cli play mock://demo
    --to vcan0` drives the simulated car on a virtual bus, so the whole
    path runs with no hardware: verified on `vcan0` (on CI too); no real
    controller has run yet (BENCH prep #26). Profiles that request
    (`obd2`, `uds-generic`) wait for ISO-TP.
  - **Vehicles** ([ADR-0027](docs/adr/0027-vehicle-profiles.md)): a
    named vehicle, picked in Settings before a connect, remembers its
    adapter URI and Live's plotted signals and names its recordings'
    folders; a salted hash of the VIN recognizes it at the next
    connect. Made, copied, renamed and forgotten in the app.
  - **Evidence for field sessions:** opt-in recording of every frame,
    decoded value and activity line per connect
    ([ADR-0017](docs/adr/0017-session-recording.md)), and a surrogate
    session log of every vendor call, with every request and reply
    while recording. Session time counts the machine's sleep on every
    platform, so a sleep is a gap on Live and in a recording, and the
    activity log and `havne-cli` say how long it was
    ([ADR-0033](docs/adr/0033-session-time-counts-sleep.md); the test
    phone's sleeps, [2026-10-29-android-2](docs/bench-sessions/2026-10-29-android-2.md)).
- **Not yet:** real hardware beyond two adapters and one car: 29-bit
  addressing, more than one ECU answering, a real x64 driver,
  and other vendors ([BENCH prep](docs/bench-sessions/bench-prep.md),
  [testing.md](docs/testing.md#gaps-not-verified)). The run sheets are
  [bench-runsheet.md](docs/bench-sessions/bench-runsheet.md),
  [vehicle-runsheet.md](docs/bench-sessions/vehicle-runsheet.md), the
  ELM327 [bench](docs/bench-sessions/elm327-bench-runsheet.md) and
  [vehicle](docs/bench-sessions/elm327-vehicle-runsheet.md) sheets, and
  the [phone's](docs/bench-sessions/android-phone-runsheet.md).
- **Tests:** 537 ctest cases on Windows (both trees rebuilt,
  2026-10-29), all passing but two env-gated skips (the cross-arch
  case, and the real-car recordings' diff, run locally), plus the
  cross-arch subset (82/82); 0 MSVC warnings. CI runs ctest on Windows,
  macOS and Linux; core under ASan and UBSan; 60 s of each libFuzzer
  harness ([fuzz/README.md](fuzz/README.md)); clang-tidy and CodeQL;
  the load test on Windows and Linux; line coverage on Windows; the
  Android and iOS cross-builds; and, on request, an iOS
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
| ELM327 | HARNESS-verified against a fake ELM327 on every platform (serial, TCP, app stream). BENCH: the VX on USB and WiFi, no bus. Vehicle: one car, read-only, through the MX+ (Bluetooth) and the VX (USB) | [docs/elm327.md](docs/elm327.md) |
| Android app | Android 12 and later; Bluetooth through the phone, background sessions. On one phone and one car, with the MX+; built in CI, no tests run on the phone | [ADR-0028](docs/adr/0028-app-fed-streams.md), [ADR-0029](docs/adr/0029-android-background-sessions.md) |

## The ELM327 transport

Built and read on one car; how it works, and what each adapter on hand
does, is in [docs/elm327.md](docs/elm327.md), and why in
[ADR-0026](docs/adr/0026-elm327-transport.md). Bluetooth on Android
goes through the app ([ADR-0028](docs/adr/0028-app-fed-streams.md)),
with background sessions ([ADR-0029](docs/adr/0029-android-background-sessions.md)).
Open, all needing hardware not on hand:
- **Response pending** (BENCH prep #17): whether an adapter waits past
  `7F xx 78`, and whether it counts that line toward the
  response-count digit. The car never sent one.
- **Listening to another tester** (#19): `AT MA` on a car, through an
  OBD Y-splitter.
- **A BLE adapter** (#25): the service the app picks, on a real one.
- Later, with records of their own: Bluetooth LE on iOS and the
  desktop (ADR-0028 step 5), and listening to the whole bus (raw
  frames, for a car whose OBD port carries more than diagnostics).

## BENCH prep

The assumptions in the J2534 and ELM327 code that only real hardware
can confirm, each with its evidence, are in
[docs/bench-sessions/bench-prep.md](docs/bench-sessions/bench-prep.md);
every session walks it. Every item is settled for the adapters on
hand but these: 29-bit addressing (#4, the car is 11-bit), what sets
the VX clock's zero (#5), the VX's serial port on a machine without
OBDX's software (#11), response pending (#17), listening to another
tester (#19), the VX's WiFi while its USB port is held (#20, in
part), and a BLE adapter (#25). Any other adapter starts the list
again.

## Known issues and technical debt

These numbers are stable IDs, and code and docs refer to them. When
an issue is resolved it is removed, and its number is not reused.

2. **The iOS app is unvalidated, and no tests run on a phone.** CI
   cross-builds an unsigned iOS arm64 device build and, on request,
   launches the app on the Simulator and screenshots it
   (`ios-simulator-smoke`); it has never run on a device, which needs
   signing. The Android app has run on one phone (Android 16) and one
   car, with the MX+ over classic Bluetooth, in the background and with
   the screen off ([2026-10-28-android-11](docs/bench-sessions/2026-10-28-android-11.md),
   [2026-10-29-android](docs/bench-sessions/2026-10-29-android.md));
   no BLE adapter has. Neither platform runs the suite: cross builds
   set `BUILD_TESTING=OFF`, so the phone is checked by its run sheet
   ([android-phone-runsheet.md](docs/bench-sessions/android-phone-runsheet.md)).
   The app needs Android 12 or later; CI uploads the Android build's
   debug APK, and each release carries one signed with the release key
   ([ADR-0030](docs/adr/0030-signed-android-releases.md)).
4. **Every def is matched against every frame, on the transport's
   worker** ([ADR-0024](docs/adr/0024-decode-on-the-worker.md)). A
   subscription can't trim that work: it is the chart's filter only,
   because the Dashboard reads every signal. Measured, it costs about
   1.4 to 2.2 µs a frame at 1000 defs (the figure varies by session),
   well inside the J2534 worker's 10 ms read slice, and the GUI
   thread's cost per frame no longer depends on it
   ([docs/testing.md](docs/testing.md#performance)). A per-signal enable
   mask can trim it if a CAN FD rate makes it matter.
6. **The session API is single-caller by contract.** Each session has
   one caller: the bridge in the app, `havne-cli` in its own process
   ([ADR-0031](docs/adr/0031-command-line-client.md)). Two front ends
   sharing one session would need more.
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

## Roadmap

| Version | Scope | Status |
|---|---|---|
| v0.1.0 | Skeleton: core ABI, Qt shell, responsive layout | ✅ Released |
| v0.2.0 | Callback API, mock transport, live frame log, CI | ✅ Released |
| v0.3.0 | Signal decoding, LiveView, J2534 transport, live vehicle read and logging | ✅ Released |
| v0.3.1 | Request engine, PID-support probe, `uds-generic`, vehicle identification, imperial units; the first Windows download | ✅ Released |
| v0.3.2 | Record-replay transport, decoding on the transport's thread, every driver in its own process | ✅ Released |
| v0.3.3 | ELM327 adapters, preferences and vehicles, Android with Bluetooth and background sessions and a signed APK, the rename to Havne | ✅ Released |
| v0.3.4 | A command-line client over the C ABI; Linux as a supported desktop; a SocketCAN transport that listens | In progress: the CLI, SocketCAN listening, a session clock that counts sleep, debug symbols and probes, the Linux downloads with an SBOM, and the Linux guide built |
| v0.3.5 | Full OBD-II diagnostics (Services 01–0A, clearing codes); SocketCAN requests through ISO-TP | Planned |
| v0.4.0 | Python bindings; PCAP export and candump import; ECU, DID and service discovery (needs requests to every ECU and listening to the whole bus first) | Proposed |
| v0.5.0 | The plugin framework (ADR-0013's profile API), with no manufacturer plugin yet; legacy buses (ISO 9141-2, KWP2000, J1850) | Proposed |
| v1.0+ | The first manufacturer plugins; the write path, with security access (0x27) and flashing through each plugin after the gateway research | Proposed |

The planned releases are in order; the proposed ones are not
scheduled. What each holds, past its row:

### v0.3.4: a command-line client, Linux, SocketCAN listening

- **`havne-cli`:** built ([ADR-0031](docs/adr/0031-command-line-client.md)):
  a client over the Qt-free core that starts a session, streams frames
  as JSON lines and exits cleanly, with golden runs that hold what the
  C ABI shows a caller. The Linux deb and tarball carry it (below).
- **Linux as a supported desktop:** CI builds and tests the app on
  Ubuntu, ELM327 runs over termios, and the AppImage runs on Ubuntu,
  Arch and Kali; [docs/linux.md](docs/linux.md) is the guide: the
  downloads, adapters and their permissions, SocketCAN, tracing, and
  what has run where. Not yet run: the app's window on a Linux desktop
  and an adapter on a Linux machine.
- **Linux downloads, an SBOM, reproducible builds:** built
  ([ADR-0035](docs/adr/0035-linux-downloads-sbom-reproducible.md)):
  each release adds the AppImage and core alone as a deb and a tarball
  (`havne-cli`, the static library, its headers, a CMake package and a
  pkg-config file), with `havne-cli`'s debug info by build ID; one SPDX
  SBOM lists every download's components, validated and attested. CI
  builds the core packages twice under `reprotest` and requires the same
  bytes, then installs and uses them. Left: the first release's run of
  the `linux` and `publish` jobs. The AppImage is built by the commit's
  time, and two builds of one commit match, but CI doesn't require it.
- **SocketCAN, listening:** built
  ([ADR-0032](docs/adr/0032-socketcan-listening.md)): raw frames from a
  native CAN interface, listen-only, and `havne-cli play`, a simulated
  ECU on Linux's virtual CAN bus that runs it with no car. Left: a real
  controller on the bench and a car (BENCH prep #26).
- **Debug symbols and probes:** built
  ([ADR-0034](docs/adr/0034-debug-symbols-and-probes.md)): every MSVC
  Release build writes PDBs, and each release ships the executables'
  PDBs in a zip of their own, checked against the binaries; three USDT
  probes in core on Linux (a session's start and close, each frame),
  checked in the ELF notes and traced with `bpftrace` on CI. Linux
  debug info ships with the deb and tarball (ADR-0035).
- **The session clock counts sleep:** built
  ([ADR-0033](docs/adr/0033-session-time-counts-sleep.md)): one host
  clock for core and the app, counting the time the machine sleeps on
  every platform (`CLOCK_BOOTTIME` on Linux and Android), so a sleep is
  a gap on Live and in a recording, and the activity log says how long
  it was. Confirmed on the test phone
  ([2026-10-29-android-2](docs/bench-sessions/2026-10-29-android-2.md):
  thirteen sleeps, each a gap of its length); a Linux machine's suspend
  is left (BENCH prep #27).

### v0.3.5: full OBD-II diagnostics, SocketCAN requests

- **Full standard OBD-II coverage.** Grow the `obd2` set toward
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
- **Trouble codes:** read stored, pending and permanent codes and the
  freeze frame (Services 03, 07, 0A, 02), and clear them (Service 04),
  the first request that changes anything in the car. It gets its own
  gate: the codes and freeze frame read and saved first, ignition on
  with the engine off, and a confirmation that says what clearing
  resets (the readiness monitors).
- **SocketCAN requests:** ISO-TP segmentation, which J2534 and ELM327
  adapters do themselves, so the request engine and the profiles run on
  a native CAN interface. An ADR first, which also revisits
  [ADR-0032](docs/adr/0032-socketcan-listening.md)'s listen-only rule:
  a session that sends needs the controller in normal mode, so it says
  when that's allowed and what then guards a car's bus against a wrong
  bit rate.

### v0.4.0 and v0.5.0 (proposed)

- **v0.4.0:** Python bindings over the C ABI; PCAP export and candump
  import; discovering which ECUs answer and what they offer, which
  needs requests to more than one ECU and listening to the whole bus
  first.
- **v0.5.0:** a plugin framework (ADR-0013's profile API made
  general); the pre-2008 buses (ISO 9141-2, KWP2000, J1850), through
  ELM327 and J2534.

### 1.0 and later: make-specific data and the write path (proposed)

- **A first manufacturer signal set,** the pilot for the first
  plugins: one make's enhanced Service 22 data, such as transmission
  temperature, which the standard set doesn't include. A def keyed
  `62 <DID>` already enrols its request and decodes the reply
  (ADR-0015, ADR-0016), so what's missing is the data: the DIDs and
  their formulas, which `uds-generic`'s polling and range scan are for
  ([ADR-0020](docs/adr/0020-uds-generic.md)).
- **The write path.** Security access (0x27) and flashing go through
  each manufacturer's plugin and depend on its secure-gateway
  authentication, researched outside this repository. Every item needs
  its own ADR first, and the UDS services it raises (0x27, 0x2E, 0x31,
  0x34/0x36/0x37, 0x3D) carry emissions-law, warranty and
  ABS/traction/stability exposure that the record has to cover. Design
  order matters: the original copy's format constrains everything
  after it.
  - **The original copy (L).** Read the calibration as it stands,
    store it as a versioned, checksum-addressed artifact, and keep it
    immutable. Every edited value shows against this shadow, so a
    restore path always exists. Session recordings are frame logs, not
    a calibration image.
  - **Table and curve editors (L).** VE and spark tables: grid cells,
    2D curves with draggable points, a 3D heatmap view. The signal
    model describes a single scalar, so core needs a richer def type,
    the biggest schema change since the signal set, and QML needs
    editors.
  - **Diff and undo (M).** Changed values highlighted against the
    original, per-value revert, global rollback; cheap if the original
    copy exists, painful to retrofit if it doesn't.
  - **Write-flow safety (M).** During any write: battery voltage
    visible, a do-not-power-off state, and a resumable or recoverable
    transfer whose recovery story is stated before the write starts.
    The surrogate's wedged-child latch bounds a hung read; a wedged
    write is a worse failure and must never be silent.

## Backlog

Work not placed in a release, grouped by theme. An item moves into
the roadmap above when it is. Defects and debt live under
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