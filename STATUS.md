# Havne: Project Status

**Updated:** 2026-10-30 · **Version:** v0.3.4 ·
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

**Current phase: v0.3.5, in progress.** v0.3.4 is out: `havne-cli`, a
command-line client over the C ABI; Linux as a supported desktop,
with the AppImage, the core as a deb and a tarball, and SocketCAN
listening; debug symbols, an SBOM and reproducible Linux downloads
with each release. Changes since then go under
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
background too. v0.3.5 has begun with reading trouble codes; clearing
them and SocketCAN requests come next ([roadmap](#roadmap)).

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
    reads only (Services 01, 02, 03, 07, 09, 0A and 0x22). About 26 a
    second on the car.
  - **The PID-support probe** (ADR-0015 §4) drops only what an ECU says
    it lacks, never on silence; on the car it walked six bitmaps and
    dropped `01 5C`.
  - **Vehicle identification** at connect
    ([ADR-0021](docs/adr/0021-vehicle-identification.md)), and
    **`uds-generic`**, polling or scanning the DIDs a URI names
    ([ADR-0020](docs/adr/0020-uds-generic.md)).
  - **Trouble codes, readiness and the freeze frame** at connect
    ([ADR-0037](docs/adr/0037-reading-trouble-codes.md)): each ECU's
    check-engine light, stored, pending and permanent codes, readiness
    monitors and freeze frame, read once between the live requests and
    again on request, on the Diagnostics page, in the activity log and
    in `havne-cli`'s output; the simulated car has codes of its own.
    Verified against the fakes' ECU through J2534 (both surrogates) and
    ELM327; not yet on a car (BENCH prep #28). Reading only: clearing
    is the rest of v0.3.5.
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
  - **Linux** ([docs/linux.md](docs/linux.md)): the app as an
    AppImage, and core alone as a deb and a tarball (`havne-cli`, the
    static library, its C headers, a CMake package and a pkg-config
    file, with `havne-cli`'s debug info by build ID), built on Ubuntu
    24.04 for glibc 2.39 and later. CI builds the core packages twice
    and requires the same bytes, then installs and builds against
    them; the AppImage is built by the commit's time, and two builds of
    one commit match. The AppImage ran with a window under WSLg on
    Ubuntu 26.04, Arch and Kali; not yet on a Linux machine's own
    desktop, nor with an adapter on Linux.
  - **Every release** carries the Windows zip and a zip of its PDBs,
    the signed APK, the Linux files, one SPDX SBOM over them, and
    checksums and one signed provenance over every download
    ([ADR-0034](docs/adr/0034-debug-symbols-and-probes.md),
    [ADR-0035](docs/adr/0035-linux-downloads-sbom-reproducible.md)),
    each checked from the published release (v0.3.4's dry run, its
    APK updating the previous release on the test phone). Core carries
    three USDT probes on Linux (a session's start and close, each
    frame), traced with `bpftrace` on CI.
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
    phone's sleeps, [2026-10-29-android-2](docs/bench-sessions/2026-10-29-android-2.md);
    a Linux machine's suspend is left, BENCH prep #27).
- **Not yet:** real hardware beyond two adapters and one car: 29-bit
  addressing, more than one ECU answering, a real x64 driver,
  and other vendors ([BENCH prep](docs/bench-sessions/bench-prep.md),
  [testing.md](docs/testing.md#gaps-not-verified)). The run sheets are
  [bench-runsheet.md](docs/bench-sessions/bench-runsheet.md),
  [vehicle-runsheet.md](docs/bench-sessions/vehicle-runsheet.md), the
  ELM327 [bench](docs/bench-sessions/elm327-bench-runsheet.md) and
  [vehicle](docs/bench-sessions/elm327-vehicle-runsheet.md) sheets, and
  the [phone's](docs/bench-sessions/android-phone-runsheet.md).
- **Tests:** 624 ctest cases on Windows (both trees rebuilt,
  2026-10-30), all passing but two env-gated skips (the cross-arch
  case, and the real-car recordings' diff, run locally), plus the
  cross-arch subset (84/84); 0 MSVC warnings. CI runs ctest on Windows,
  macOS and Linux, with SocketCAN's suites on Linux's `vcan0`; core
  under ASan and UBSan, `vcan0` included; 60 s of each libFuzzer
  harness ([fuzz/README.md](fuzz/README.md)); clang-tidy and CodeQL;
  the C ABI against the last release's; the load test on Windows and
  Linux; line coverage on Windows; the AppImage, built and started
  offscreen; the core packages, built twice to the same bytes, then
  installed and built against; the probes under `bpftrace`; the
  Android and iOS cross-builds; and, on request, an iOS Simulator
  smoke. The badge above shows its state. Tiers, suites and
  gaps are in [docs/testing.md](docs/testing.md).

## Components

| Component | State | Reference |
|---|---|---|
| Core sessions and callbacks | Stable: snapshot dispatch, the 4,120-byte frame, close and destroy apart | [ADR-0002](docs/adr/0002-callback-pair-identity-snapshot-dispatch.md), [ADR-0006](docs/adr/0006-frame-abi-v0.3.md) |
| Signal decode | Linear scaling in `double` of unsigned, signed and binary32 fields in either byte order, gated on message ID and a mux key of up to 3 bytes; text and hex fields; a validator that accepts exactly what the decoder decodes | [ADR-0004](docs/adr/0004-signal-decoding-model.md), [ADR-0016](docs/adr/0016-signal-definition-layout.md) |
| Mock transport | A ~2-minute drive cycle on a vehicle model (`core/src/mock_drive_cycle.h`) whose readings stay consistent with each other, every tick round-tripped through the mock set (`CoreTest.MockDriveCycle*`) | none |
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
part), a BLE adapter (#25), a real CAN controller (#26) and a sleep
on a Linux machine (#27). Any other adapter starts the list again.

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
| v0.3.4 | A command-line client over the C ABI; Linux as a supported desktop; a SocketCAN transport that listens | ✅ Released |
| v0.3.5 | Full OBD-II diagnostics (Services 01–0A, clearing codes); SocketCAN requests through ISO-TP; fault injection on a replay; a performance benchmark with latency per PID | In progress: reading codes built |
| v0.3.6–v0.3.9 | Recordings you can open, compare and share: a log viewer with run overlay, CSV import and export with scrubbing, a health report, triggered logging and alerts | Planned |
| v0.4.0 | Python bindings; PCAP export and candump import; ECU, DID and service discovery (needs requests to every ECU and listening to the whole bus first) | Proposed |
| v0.5.0 | The plugin framework (ADR-0013's profile API), with no manufacturer plugin yet; legacy buses (ISO 9141-2, KWP2000, J1850) | Proposed |

The planned releases are in order; the proposed ones are not
scheduled. What each holds, past its row:

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
- **Trouble codes:** reading is built
  ([ADR-0037](docs/adr/0037-reading-trouble-codes.md)): stored, pending
  and permanent codes, readiness and the freeze frame (Services 03, 07,
  0A, 01 and 02), on the Diagnostics page, in the activity log and in
  `havne-cli`; on the test car through both adapters, beside a second
  scan tool, to come (BENCH prep #28). Next, clearing them (Service
  04), the first request that changes anything in the car, on its own
  branch and record. It gets its own gate: the codes and freeze frame
  read and saved first, ignition on with the engine off, and a
  confirmation that says what clearing resets (the readiness
  monitors).
- **SocketCAN requests:** ISO-TP segmentation, which J2534 and ELM327
  adapters do themselves, so the request engine and the profiles run on
  a native CAN interface. An ADR first, which also revisits
  [ADR-0032](docs/adr/0032-socketcan-listening.md)'s listen-only rule:
  a session that sends needs the controller in normal mode, so it says
  when that's allowed and what then guards a car's bus against a wrong
  bit rate.
  - The simulated ECU on `vcan0` answers requests through ISO-TP (the
    fakes' ECU, `tests/fake_ecu.h`), so all of it runs with no car.
  - Conformance tests against it: STmin, block sizes, wait frames,
    flow-control overflow, the N_Bs and N_Cr timeouts, out-of-order
    sequence numbers. Adapters hide these; on SocketCAN they're core's
    to get right.
  - A fuzz target for the ISO-TP reassembly.
- **Fault injection on a replay:** frames dropped, delayed or corrupted
  (a bad ISO-TP sequence, a late reply) in a replayed session, so the
  error handling is proven against real traffic with no vehicle.
  `replay://` already plays byte for byte and the fakes inject faults;
  injecting into a replay is the new part.
- **A performance benchmark:** frames per second, throughput and zero
  dropped as published numbers, building on CI's load test and
  `bench_bridge` ([docs/testing.md](docs/testing.md#performance)); and
  per PID, reply latency and the spread of its refresh interval (P50,
  P99, P99.9), as a `latency.csv` per session. A J2534 frame carries
  the adapter's time and an ELM327's the host's, whose jitter includes
  the serial link's, so the output says which.
- **Done when:** a script reads a live car through SocketCAN at a
  documented rate. Python bindings come in v0.4.0, so the script is
  `havne-cli`. The car run needs a SocketCAN adapter and an OBD-to-DB9
  cable (BENCH prep #26).

### v0.3.6 to v0.3.9: recordings you can open, compare and share

A recording becomes something to open offline, line up against
another, bring in from another logger and post safely; the split
across the four releases is decided as they come.

- **A log viewer:** a recording opened offline with seeking, zoom and
  markers, and two runs overlaid on a common zero (a threshold
  crossing or a marker). Seeking needs no new format, since `host_us`
  only increases; the recording container waits for whole-bus
  listening (v0.4.0).
  - Drawn at scale: each pixel column's lowest and highest, with memory
    bounded on an hour-long recording.
  - With it, the same chart code on Live: the chart as a time window
    (the last N seconds, the same span on any adapter, instead of the
    last 360 points per signal), and on the phone pinch zoom and
    smaller Dashboard gauges when it's upright.
  - Done when two recordings of the mock's pulls overlay aligned on the
    pull's start, and an hour-long recording opens and scrubs within a
    stated memory bound.
- **In and out:** CSV import of another logger's timed signals through
  a column map, with a fuzz target for its parser; CSV export; every
  export scrubbed of VINs, calibration IDs, serials and paths; a
  health report (trouble codes, readiness, identification with the
  VIN decoded to make, model and year, and key values) as one scrubbed
  export, with readiness for the current drive cycle (PID 41) and a
  summary of whether the car would pass an inspection.
  - The Log page's frames: a filter by the columns an engineer reads a
    bus by (the ID, one or a list, a range or an ID and mask; the
    length; the payload, a byte at a position or a masked hex sequence
    such as `41 0C`; the flags; a time range), a per-ID view (each
    ID's newest frame, count and rate, the bytes that changed
    highlighted), an export of what the filter shows, and copied rows
    for quick notes.
- **Conditions:** one condition model in core, data rather than code,
  for triggered logging (record while a condition holds, or a window
  around a link fault), alerts (color, sound or a banner), and markers
  the viewer shows; computed signals beside it, in a derived-signal
  stage in core that keeps the UI on final display values: boost is
  intake manifold pressure (PID 0x0B) minus barometric pressure (PID
  0x33), neither yet in the `obd2` set, so both are checked against a
  public J1979 table first.

### v0.4.0 and v0.5.0 (proposed)

- **v0.4.0:** Python bindings over the C ABI; PCAP export and candump
  import (a fuzz target for the candump parser); discovering which
  ECUs answer and what they offer, which needs requests to more than
  one ECU and listening to the whole bus first, with the simulated ECU
  grown to several modules (engine, transmission, body) to test them
  with no car. Done when a Python script opens SocketCAN, decodes PIDs
  and prints the values.
- **v0.5.0:** a plugin framework (ADR-0013's profile API made
  general); the pre-2008 buses (ISO 9141-2, KWP2000, J1850), through
  ELM327 and J2534.

## Backlog

Work not placed in a release, grouped by theme. An item moves into
the roadmap above when it is. Defects and debt live under
[Known issues](#known-issues-and-technical-debt), not here.

Sizes are rough: **S** is a day or less, **M** a few days, **L** a
week or more or a design that needs an ADR first.

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

- **Custom derived channels (M).** Beyond the computed signals planned
  for v0.3.6–v0.3.9: a user-facing expression per channel
  (differences, unit conversions, smoothing). Stays in core, so the UI
  still gets final display values, and is the same
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