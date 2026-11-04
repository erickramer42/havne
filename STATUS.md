# Havne: Project Status

**Updated:** 2026-11-04, **Version:** v0.4.0,
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

**Current phase: v0.5.0, in progress.** v0.4.0 is out: recordings in
core, written on a thread of their own by the app and `havne-cli`
alike; the Recordings page's viewer, which overlays two runs, and
`havne-cli view` and `info` offline; a check of a recording's frames;
finding a signal among many; and SocketCAN through a real controller,
which has listened on a desk bus and on the test car. Changes since then go under
[CHANGELOG.md's "Unreleased"](CHANGELOG.md#unreleased). The J2534 stack is verified
end to end against a fake vendor DLL, through the x64 and the x86
surrogate. One real J2534 adapter, the OBDX Pro VX, has run on a quiet
bench bus and on one car, from
[2026-10-22](docs/bench-sessions/2026-10-22-vehicle.md) to
[2026-11-03](docs/bench-sessions/2026-11-03-socketcan-car.md): the values
decoded and tracked the cluster, and the request engine, the probe,
identification, `uds-generic`, the trouble codes, several PIDs per
request, more than one ECU and a DID scan ran there. The [ELM327 transport](docs/elm327.md)
([ADR-0026](docs/adr/0026-elm327-transport.md)) is built and has read
the same car through the OBDLink MX+ over Bluetooth and the VX in ELM
mode over USB
([2026-10-27](docs/bench-sessions/2026-10-27-elm327-vehicle.md)), and
the Android app reaches the car through the phone's Bluetooth, in the
background too. v0.5.0 brings your own signals: PIDs and DIDs defined
or imported in bulk, and computed signals such as boost
([roadmap](#roadmap)).

- **Works:**
  - **Mock transport,** on every platform: a scripted drive cycle (15 s
    with the key on and the engine stopped, a cold start, two pulls onto
    the fuel-cut limiter, a cruise, a quarter mile off launch control)
    whose later loops start warm, each after its own engine-off spell.
  - **Replay** ([ADR-0023](docs/adr/0023-replay-transport.md)), on every
    platform: `replay://<recording>` plays a recording's `frames.csv`
    back at its own pace or up to 100x faster. `?faults=` injects faults
    as it plays ([ADR-0041](docs/adr/0041-replay-fault-injection.md),
    proposed): frames dropped, lost to an overflow, corrupted, cut short
    or held back, the same each time from `?seed=`. `?from=` and `?to=`
    play a stretch alone, in the seconds the viewer shows.
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
    The surrogate holds Windows' 1 ms timer, so a held read ends at
    its slice: through the fake as the VX a 4 ms reply reads 11.3 ms
    instead of 16.4, and on the test car 28 % more replies arrived in
    the same time (ADR-0042's appendix of 2026-11-01,
    [2026-11-01-vehicle](docs/bench-sessions/2026-11-01-vehicle.md)).
  - **Requests follow the replies**
    ([ADR-0015](docs/adr/0015-request-engine.md)): one out per channel,
    the next when the reply is in, a 100 ms wait and a 20 ms floor;
    the services 01, 02, 03, 07, 09, 0A and 0x22. About 26 a
    second on the car.
  - **The PID-support probe** (ADR-0015 section 4) drops only what an ECU says
    it lacks, never on silence; on the car it walked six bitmaps and
    dropped `01 5C`.
  - **Every linear Service 01 reading** ([ADR-0039](docs/adr/0039-full-service01-coverage.md)):
    the `obd2` set holds a def for every linear field of SAE J1979's
    Service 01 table, and the probe keeps only the PIDs a car lists. Its
    requests run in two rate classes, two fast (rpm, speed, load, MAF,
    throttle, fuel pressure, lambda) to one slow (temperatures,
    pressures, counters); Live hides the signals the car's bitmaps
    leave out and the fields its replies come without (core's absent
    signals), and says how many; imperial units cover the new units
    (`inH2O`, `gal/h`, `lb·ft`). Verified against the fakes' ECU, which
    answers every linear PID, through J2534 and ELM327, and the car's
    recordings replay to their own values through the grown set. On
    the test car through the MX+ the probe keeps exactly its bitmaps'
    43 PIDs, Live hides the rest, and the classes refresh every 0.87 s
    and 3.53 s at 26 requests a second
    ([2026-10-31-elm327-vehicle](docs/bench-sessions/2026-10-31-elm327-vehicle.md),
    BENCH prep #30). Several PIDs per request (ADR-0039's Appendix A):
    up to six of a class in one request, one per request again for a
    car that mishandles them, and packed again after an ignition cycle
    once the car answers; on the test car each signal refreshed four
    to five times as often through the MX+, and through the VX 2.7
    times as many replies arrived a minute and RPM 2.8 times as often
    (BENCH prep #31;
    [2026-10-31-elm327-vehicle-2](docs/bench-sessions/2026-10-31-elm327-vehicle-2.md),
    [2026-11-01-vehicle](docs/bench-sessions/2026-11-01-vehicle.md),
    [2026-11-02-vehicle](docs/bench-sessions/2026-11-02-vehicle.md)).
  - **Vehicle identification** at connect
    ([ADR-0021](docs/adr/0021-vehicle-identification.md)), and
    **`uds-generic`**, polling or scanning the DIDs a URI names
    ([ADR-0020](docs/adr/0020-uds-generic.md)).
  - **Trouble codes, readiness and the freeze frame** at connect
    ([ADR-0037](docs/adr/0037-reading-trouble-codes.md)): each ECU's
    check-engine light, stored, pending and permanent codes, readiness
    monitors (since the last clear and for this drive cycle) and freeze
    frame, read once between the live requests and
    again on request, on the Diagnostics page, in the activity log and
    in `havne-cli`'s output; the simulated car has codes of its own.
    Verified against the fakes' ECU through J2534 (both surrogates) and
    ELM327, and on the test car through the VX and the MX+, which has
    no codes stored (BENCH prep #28).
  - **Clearing trouble codes**
    ([ADR-0038](docs/adr/0038-clearing-trouble-codes.md)): the page's
    Clear codes... asks first, and the app saves the report to a file of
    its own before anything is sent; core asks each ECU its engine and
    vehicle speeds and, on the user's word, sends `04` with the engine
    running, by the ECU's own ID, but asks once more while the vehicle
    moves (ADR-0038's appendix of 2026-11-01), then reads the report
    again; each outcome on the page and in the activity log. Verified
    against the fakes through J2534 and ELM327 (running, standing,
    moving, refused), on the simulated car, and on the test car through
    the MX+ with the engine off and running, a car with no codes
    ([2026-11-01-android](docs/bench-sessions/2026-11-01-android.md),
    BENCH prep #29).
  - **Profiles** chosen by the URI
    ([ADR-0013](docs/adr/0013-protocol-profiles.md)): `mock`, `raw`,
    `obd2` and `uds-generic`. Each signal def states its kind and byte
    order ([ADR-0016](docs/adr/0016-signal-definition-layout.md)).
    `uds-generic` scans a `?did=` range with `?scan=first` back to
    back before polling what it found: about 30 DIDs a second on the
    test car through both adapters, a 1,024-DID block in about 30 s
    ([ADR-0020](docs/adr/0020-uds-generic.md)'s appendices).
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
    Live's table and strip chart, the Log page's activity log, the
    Recordings page's viewer (pan, zoom, a cursor, the frames check's
    findings, two runs overlaid and aligned, and a Mark while
    recording), one signal filter on Live and the viewer (words, plotted
    only, the part of the car), and Settings composing the URI; developer mode (off until switched on:
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
    reader closing the pipe (`| head`). `stream --record <folder>`
    records the session through core's recorder
    ([ADR-0049](docs/adr/0049-recordings-in-core.md)) into a folder
    of the app's files, each line typed a marker; a golden folder
    pins the replay fixture's.
  - **SocketCAN** ([ADR-0032](docs/adr/0032-socketcan-listening.md),
    [ADR-0040](docs/adr/0040-socketcan-requests-isotp.md)), on Linux.
    Core reads the interface and never configures it, and each refusal
    names the `ip link` commands that fix it. Settings and `havne-cli
    devices` list the interfaces.
    - **Listening:** `socketcan://can0` reads a CAN interface's whole
      bus (or `?filter=`'s IDs) with the kernel's receive times, CAN FD
      and error frames included, through a controller in listen-only
      mode, and sends nothing. `havne-cli play mock://demo --to vcan0`
      drives the simulated car on a virtual bus.
    - **The simulated ECU** ([ADR-0046](docs/adr/0046-simulated-ecu.md)):
      `havne-cli sim mock://sim --on vcan0` answers OBD-II requests on a
      virtual bus as the simulated car's engine ECU, through core's
      ISO-TP (`ecuio/sim.h`): the drive cycle's values or the fakes'
      canned ones, the mock's diagnostics, a VIN, and periodic faults;
      or `replay://<recording>`, the replies the car gave, each keyed by
      the request it answers, at the sim's time since it started.
      Verified on `vcan0` (WSL): an obd2 session reads its values,
      identification, diagnostics and inspection line, clears it, and
      stays in step under every fault; against the test car's engine
      start, it walks the car's bitmaps, keeps exactly the recorded
      PIDs, packs, and decodes only engine speeds the car sent.
      `--desk-bus` lets it answer on a real controller too, one-shot
      mode or not, on a desk bus with no car (ADR-0046's appendix
      "the desk bus"); on any bus it goes silent for good once
      another node sends on 0x7E8. Verified on `vcan0`, and on the
      desk bus through the USB2CAN, read by the VX over J2534
      ([2026-11-03-socketcan-desk-sim](docs/bench-sessions/2026-11-03-socketcan-desk-sim.md)).
    - **Requests:** `obd2` and `uds-generic` run through ISO-TP in core
      (`core/src/isotp.h`, the reassembly ELM327 uses too), with the
      request keys of the other two transports. A real controller must
      be in normal mode at the rate `?baud=` states, never restart
      itself, be in one-shot mode and be error-active; until its first
      request is echoed and then answered, or 25 ms pass after the echo
      with no error frame, a CAN error ends the sending (an adapter may
      echo a frame before any node acknowledged it).
    - Verified on `vcan0` (on CI too): listening, and requests against
      the fakes' ECU answering through the kernel's ISO-TP, core's
      ISO-TP held to the kernel's as well. A real controller, an
      Innomaker USB2CAN, has listened on a desk bus beside the VX
      (BENCH prep #26: every frame the kernel counted, nothing sent, a
      wrong rate's errors, an unplug and its recovery) and on the test
      car beside it ([2026-11-03-socketcan-car](docs/bench-sessions/2026-11-03-socketcan-car.md)); it offers no
      one-shot mode, so it can't ask, and on that bus `cansend` showed
      why one-shot stays required: a frame nobody acknowledges is sent
      again forever, error-passive, never bus-off (BENCH prep #32,
      [2026-11-02-socketcan-desk](docs/bench-sessions/2026-11-02-socketcan-desk.md)).
      A real controller has sent through Havne only as the simulated
      ECU on the desk bus; no request has gone out through one.
  - **Linux** ([docs/linux.md](docs/linux.md)): the app as an
    AppImage, and core alone as a deb and a tarball (`havne-cli`, the
    static library, its C headers, a CMake package and a pkg-config
    file, with `havne-cli`'s debug info by build ID), built on Ubuntu
    24.04 for glibc 2.39 and later. CI builds the core packages twice
    and requires the same bytes, then installs and builds against
    them; the AppImage is built by the commit's time, and two builds of
    one commit match. The AppImage ran with a window under WSLg on
    Ubuntu 26.04, Arch and Kali, and v0.3.4's on a Linux machine's own
    desktop (Xubuntu 26.10, X11, `mock://demo` with recording); not yet
    with an adapter on Linux.
  - **Reply timing** ([ADR-0042](docs/adr/0042-reply-timing.md)): every
    requesting session measures, per request in its rotation, its
    outcomes, its reply latency (host clock) and its refresh interval
    on the frames' own stamps (the adapter's for J2534, the kernel's
    for SocketCAN, the host's for ELM327) at P50, P99 and P99.9, into a
    recording's `latency.csv`, `havne-cli`'s `timing` lines and the
    activity log's summary at the disconnect; `bench_timing` measures
    what the stack adds through each transport against the fakes
    ([docs/testing.md](docs/testing.md#reply-timing-against-the-fakes)).
    On the test car through the VX, a single PID's reply at P50
    15.4 ms (the median across the rotation) and a pack of several at
    73.7 ms ([2026-11-01-vehicle](docs/bench-sessions/2026-11-01-vehicle.md));
    through the MX+ relayed by the test phone, `01 00` at P50 18 ms and
    P99 114 ms ([2026-11-01-android](docs/bench-sessions/2026-11-01-android.md)).
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
  - **VIN decode** ([ADR-0043](docs/adr/0043-vin-decode.md)): the
    model year, the make and the check digit, on the device, by 49 CFR
    565.15 and the manufacturer codes of NHTSA vPIC's standalone
    database (a table committed from it); the activity log's `VIN:`
    line and Settings' lookup in the app, `havne-cli vin` and
    `stream`'s `identity` line. Verified at UNIT (the regulation's
    sample VIN, each year rule) and in the bridge, QML and CLI tests,
    and on the test car through the VX: its VIN decoded, the year and
    the make found and the check digit matching
    ([2026-11-02-vehicle](docs/bench-sessions/2026-11-02-vehicle.md)).
  - **Trouble-code titles** ([ADR-0044](docs/adr/0044-trouble-code-titles.md)):
    SAE J2012's names for the generic powertrain and network codes, from
    Wal33D's MIT list (a table committed from it, its notice in every
    download), on the Diagnostics page, in its lookup, in the CLI's
    `diagnostics` line and `havne-cli dtc`. Checked against J2012's
    2002 pages on 110 codes and against the other open lists; verified at
    UNIT and in the bridge, QML and CLI tests.
  - **Inspection readiness** ([ADR-0045](docs/adr/0045-inspection-readiness.md)):
    every ECU's monitors and check-engine light judged as one car by the
    federal OBD inspection rule (40 CFR 85.2222: two incomplete for
    1996-2000, one from 2001, a commanded light fails) for the VIN's
    model year; Diagnostics' card and `stream`'s `inspection` line.
    Verified at UNIT and in the bridge, QML and CLI tests, and on the
    test car through the VX: "ready", one monitor incomplete where its
    model year allows one
    ([2026-11-02-vehicle](docs/bench-sessions/2026-11-02-vehicle.md)).
  - **Evidence for field sessions:** opt-in recording of every frame,
    decoded value and activity line per connect
    ([ADR-0017](docs/adr/0017-session-recording.md)), written by core's
    recorder on a thread of its own, so the frames the Log page drops
    under load are recorded too
    ([ADR-0049](docs/adr/0049-recordings-in-core.md)), and read back
    offline by core's reader, the Recordings page and `havne-cli
    view`, and a surrogate
    session log of every vendor call, with every request and reply
    while recording. Session time counts the machine's sleep on every
    platform, so a sleep is a gap on Live and in a recording, and the
    activity log and `havne-cli` say how long it was
    ([ADR-0033](docs/adr/0033-session-time-counts-sleep.md); the test
    phone's sleeps, [2026-10-29-android-2](docs/bench-sessions/2026-10-29-android-2.md);
    a Linux machine's suspend is left, BENCH prep #27).
- **Not yet:** real hardware beyond two adapters and one car: 29-bit
  addressing, more than one ECU answering in one session, a real x64 driver,
  and other vendors ([BENCH prep](docs/bench-sessions/bench-prep.md),
  [testing.md](docs/testing.md#gaps-not-verified)). The run sheets are
  [bench-runsheet.md](docs/bench-sessions/bench-runsheet.md),
  [vehicle-runsheet.md](docs/bench-sessions/vehicle-runsheet.md), the
  ELM327 [bench](docs/bench-sessions/elm327-bench-runsheet.md) and
  [vehicle](docs/bench-sessions/elm327-vehicle-runsheet.md) sheets, and
  the [phone's](docs/bench-sessions/android-phone-runsheet.md).
- **Tests:** 999 ctest cases on Windows (both trees rebuilt,
  2026-11-04), all passing but two env-gated skips (the cross-arch
  case, and the real-car recordings' diff, run locally), plus the
  cross-arch subset (87/87); 0 MSVC warnings, and a GCC or Clang
  warning fails CI. CI runs ctest on Windows,
  macOS and Linux, with SocketCAN's suites on Linux's `vcan0`; core
  under ASan and UBSan, `vcan0` included; 60 s of each libFuzzer
  harness ([fuzz/README.md](fuzz/README.md)); clang-tidy over core,
  the surrogate, `havne-cli` and the app, and CodeQL; the C ABI
  and its headers' declarations against the last release's; the load test on Windows and Linux,
  gated on two ratios rather than the runner's speed; line coverage on Windows; the AppImage, built and started
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
| Bridge and models | Worker to GUI marshaling with bounded backpressure, 100 ms coalescing, a 10k-row frame log, a 360-point history per signal (each 100 ms window's lowest, highest and newest), link-health polling, recording; QML reaches it as the `Bridge` singleton | [ADR-0003](docs/adr/0003-backpressure-bounds-in-flight-frames.md), [ADR-0005](docs/adr/0005-coalesced-signal-updates.md) |
| QML pages | Home, Dashboard, Live, Diagnostics, Log, Recordings, Settings; `test_qml` runs them against a real bridge, offscreen | [docs/architecture.md](docs/architecture.md#the-ui-layer) |
| J2534 | HARNESS-verified through the surrogate: x64 in CI, x86 cross-arch. BENCH: one adapter, quiet bus. Vehicle: one car, through the surrogate, from 2026-10-22 to 2026-11-03 | [docs/j2534.md](docs/j2534.md) |
| ELM327 | HARNESS-verified against a fake ELM327 on every platform (serial, TCP, app stream). BENCH: the VX on USB and WiFi, no bus. Vehicle: one car, through the MX+ (Bluetooth) and the VX (USB) | [docs/elm327.md](docs/elm327.md) |
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
part), a BLE adapter (#25), a real CAN controller that sends with
one-shot mode (#32), and a sleep on a Linux
machine (#27). Any other adapter starts the list again.

## Known issues and technical debt

These numbers are stable IDs, and code and docs refer to them. When
an issue is resolved it is removed, and its number is not reused.

- **#2. The iOS app is unvalidated, and no tests run on a phone.** CI
   cross-builds an unsigned iOS arm64 device build and, on request,
   launches the app on the Simulator and screenshots it
   (`ios-simulator-smoke`); it has never run on a device. Each run's
   `havne-ios-arm64-unsigned` artifact can be sideloaded with a
   tester's own Apple ID ([docs/ios.md](docs/ios.md)). The Android app has run on one phone (Android 16) and one
   car, with the MX+ over classic Bluetooth, in the background and with
   the screen off ([2026-10-28-android-11](docs/bench-sessions/2026-10-28-android-11.md),
   [2026-10-29-android](docs/bench-sessions/2026-10-29-android.md));
   no BLE adapter has. Neither platform runs the suite: cross builds
   set `BUILD_TESTING=OFF`, so the phone is checked by its run sheet
   ([android-phone-runsheet.md](docs/bench-sessions/android-phone-runsheet.md)).
   The app needs Android 12 or later; CI uploads the Android build's
   debug APK, and each release carries one signed with the release key
   ([ADR-0030](docs/adr/0030-signed-android-releases.md)).
- **#6. The session API is single-caller by contract.** Each session has
   one caller: the bridge in the app, `havne-cli` in its own process
   ([ADR-0031](docs/adr/0031-command-line-client.md)). Two front ends
   sharing one session would need more.
- **#13. The picker treats one registry entry as one device.** Multiple
    adapters behind one vendor DLL can't be told apart. They would be
    selected through `PassThruOpen`'s `pName` argument, which the
    loader passes as `nullptr`, meaning the default device.
- **#20. Pointer-bearing ioctls (`FAST_INIT`, `FIVE_BAUD_INIT`) are out of
    contract** and refused at both ends. That is fine for ISO 15765,
    since those are ISO 9141/KWP init calls. If a BENCH device needs
    them, give them a dedicated wire form rather than relaxing the
    whitelist.
- **#29. The iOS simulator smoke depends on Intel macOS runners.** Qt
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
| v0.3.6 | Proven and finished: every car run the adapters on hand allow (packing through the VX, more than one ECU captured); Windows timer resolution; trouble-code descriptions, VIN decode and a readiness summary; the simulated ECU as `havne-cli sim`; the phone as a relay; clearing with the engine running; a performance gate in CI; clang-tidy tightened | Released |
| v0.4.0 | Recordings in core and a log viewer, in the app and `havne-cli`; finding a signal among many; SocketCAN through a real controller, on a desk bus and then the test car | Released |
| v0.5.0 | Your own signals: PIDs and DIDs defined or imported in bulk, computed signals such as boost, and the plugin framework's data tier | In progress |
| v0.6.0-v0.9.0 | v0.6.0, sharing and scripting: exports, imports, Python. v0.7.0, conditions: triggered logging and alerts. v0.8.0, the whole vehicle: discovery and extended sessions. v0.9.0, the scan tool, the last feature release | Planned |
| 1.0.0 | The finished reader: the features above frozen and hardened, full featured on Windows, Linux and Android | Planned |

Versions follow semantic versioning
([ADR-0048](docs/adr/0048-semantic-versioning.md)): each feature
release raises the minor number, fixes alone the patch number. Earlier
releases are in [CHANGELOG.md](CHANGELOG.md). The planned releases are
in order and hold about three features each; a release's detail comes
in here when its work starts.

### v0.4.0: recordings in core, the log viewer, SocketCAN on a real controller

**Recordings in core, and the log viewer**
([ADR-0049](docs/adr/0049-recordings-in-core.md), accepted; its
section 10 orders the work).
- **The writer** is in core, on a thread the worker feeds: frames the
  app drops under load are recorded too, and a failed write or a full
  disk is an error the recording reports. Built (steps 1 and 2): core's
  recorder (`ecuio/recording.h`), `havne-cli stream --record <dir>`,
  and the app recording through it. It costs core's own worker 30 to 50 ns a
  frame, and the bridge's looping run keeps 0.96 of its rate, inside
  the 10% budget (`bench_bridge --record`, the ADR's appendices of
  2026-11-02 and 2026-11-03); CI's load test gates the recorded run at
  0.7 of the unrecorded one, the faster of two runs each.
- **The reader** opens a recording offline. One pass on open builds a
  sparse index (`host_us` only increases) and each signal's lowest and
  highest per second, so memory follows the recording's seconds, not
  its size. A torn last row is dropped and counted, by the reader and
  by `replay://`. It is built (step 3), with a fuzz target over every
  parser and whole folders: the replay fixture's hour opens in about
  9 ms on the laptop and 10 ms on the test phone, against bounds of
  2 s and 10 s, and a dense hour of 50 signals at 10 Hz in about
  0.2 s on either (`bench_recording`, the ADR's appendix of
  2026-11-03).
- **The viewer**, in the app and as `havne-cli view` (JSON lines,
  goldens), both built (step 5): the Recordings page pans, zooms
  (pinch on the phone) and reads the values at a cursor, with the
  frames check's findings on the time axis, and overlays a second run
  dashed, aligned as `havne-cli view --align-on` aligns two: on their
  starts, their UTC starts, a marker, or a rise or fall through a
  level. Live's strip chart is the shared chart, and draws a dense hour
  at 62 charts a second on the test phone and 124 on the laptop, past
  the 60 asked (the ADR's appendix "the viewer"). Marks are written
  while recording: the header's Mark in the app, a line typed into
  `havne-cli stream --record`.
- **`havne-cli info <recording>`:** duration, each signal's count and
  rate, the link events and sleeps, and what's wrong offline: gaps,
  rate drops, an ID first seen late, overflow flags, broken ISO-TP
  sequences. It is built (step 4), on core's frames check
  (`ecuio_recording_check`), whose defaults are set against the car
  recordings on hand: a rate drop at 10x an ID's median interval and
  100 ms at least (on the wire, between its messages), a gap
  of 1 s (the ADR's appendix of 2026-11-03). On the SocketCAN car
  run's listener recording, the first car recording of wire frames, it
  checked every ISO-TP sequence and found none broken
  ([2026-11-03-socketcan-car](docs/bench-sessions/2026-11-03-socketcan-car.md)).
- **A stretch replayed**, built: `replay://<recording>?from=<s>&to=<s>`
  in the viewer's seconds, on Live or through `havne-cli sim` on a
  virtual bus, and the viewer's Replay view hands on the part in view
  ([ADR-0023](docs/adr/0023-replay-transport.md)'s appendix "a stretch
  replayed").

**Finding a signal among many**, built
([ADR-0050](docs/adr/0050-finding-a-signal.md)). Each signal def
carries a group, the part of the car it reads (Engine, Air, Fuel,
Oxygen sensors, Emissions, Vehicle, Electrical, Diagnostics, else
Other), which recordings and `havne-cli`'s lines carry too. Live's list
and the Recordings page's rows share one filter, a row high: words
over the name and the unit as shown, and a popup with Plotted only and
a box per group the list holds. On Live it sits in the title's row, so
a short page loses no height to it.

**SocketCAN through a real controller.** The desk run
([2026-11-02-socketcan-desk](docs/bench-sessions/2026-11-02-socketcan-desk.md))
had the Innomaker USB2CAN listen (BENCH prep #26) and kept
[ADR-0040](docs/adr/0040-socketcan-requests-isotp.md) section 2's
one-shot rule: without it, a frame nobody acknowledges is sent again
forever. The USB2CAN offers no one-shot mode, so it listens only.
- The refusal names only modes the controller offers, and
  `havne-cli devices` and Settings show each CAN interface's bit rate,
  controller state and supported modes, so a refusal is explained
  before a run: built (ADR-0040's appendix "the desk bus", amended
  2026-11-03); the supported modes need Linux 6.0.
- The simulated ECU on the desk bus, behind an explicit flag, silent
  once anything else answers on its reply ID: built
  (`havne-cli sim --desk-bus`, ADR-0046's appendix "the desk bus": a
  controller without one-shot mode answers too, a desk bus having
  no car to harm). The VX read it through J2534 over real wire
  ([2026-11-03-socketcan-desk-sim](docs/bench-sessions/2026-11-03-socketcan-desk-sim.md)).
- Stateful fuzzing of ISO-TP and UDS, before the car: built
  (`fuzz_request_bus`, [fuzz/README.md](fuzz/README.md)): a requesting
  session's engine and ISO-TP against the fakes' ECU on a simulated
  bus, each reply broken as the input says, then honest replies that
  must bring every channel's one-shots to an end.
- SocketCAN on the test car, listen-only (BENCH prep #26): done
  ([2026-11-03-socketcan-car](docs/bench-sessions/2026-11-03-socketcan-car.md)),
  beside the VX reading the car. Requests only through an adapter
  whose driver offers one-shot mode, on the desk bus first (BENCH
  prep #32).
- A reply's time on the bus against the host's clock: measured beside
  the VX on the car (ADR-0042's appendix of 2026-11-03): about 52 ms
  of a segmented reply's time is the adapter's delivery after the
  last frame. A latency on the adapter's clock and a J2534 read that
  ends at its reply wait for a reason to build them.

**Done when:** two recordings of the mock's pulls, recorded by
`havne-cli`, overlay aligned on the pull's start in the app and in
`havne-cli view` (met: [testing.md](docs/testing.md)'s overlay row);
an hour-long recording opens and scrubs within a stated memory bound
(met: [testing.md](docs/testing.md)'s "Reading a recording" and
"Drawing a recording"); the SocketCAN car run is filed (met:
[2026-11-03-socketcan-car](docs/bench-sessions/2026-11-03-socketcan-car.md)).

### Waiting on a tester: an iPhone download

No version until a tester has run CI's unsigned `.ipa` on an iPhone
and the run is filed as a session sheet. Then the release workflow
builds and publishes `havne-X.Y.Z-ios-arm64-unsigned.ipa` with the
other downloads (provenance, the SBOM and the checksums), the release
checks read its version and that it carries no signature, and the
notes' Try it points at [docs/ios.md](docs/ios.md).

### 1.0.0: the finished reader

After v0.9.0 the formats freeze (the C ABI, `havne-cli`'s schema, the
plugin formats, the recording format) and the releases are fixes
alone, as 0.9.x: a second car and testers other than the developer,
every adapter class on a car, the release on a PC without Visual
Studio, Linux on real hardware, accessibility and a user guide, and
hour-long runs on every transport. 1.0.0 follows once those runs are
filed and the 0.9.x releases have gone a stated stretch with no
high-severity issue: full featured on Windows, Linux and Android, as
much macOS and iOS as is manageable, with a signed Windows executable
and an installer that checks for updates. Writing to a car (security
access, flashing) is 2.0.

## Backlog

Work not placed in a release, one line each. An item moves into the
roadmap when it's placed. Defects and debt live under
[Known issues](#known-issues-and-technical-debt). **S** is a day or
less, **M** a few days, **L** a week or more or an ADR first.

### User interface

- **Per-signal unit overrides (S):** one quantity in the other system
  (deg F with km/h); whole-UI units are built
  ([ADR-0022](docs/adr/0022-display-units.md)).
- **A phone's density (M):** control heights, spacing and the default
  zoom on a phone; the test phone's run of the viewer read best at 75 %,
  and on its side the viewer's controls fill the screen above the chart.
- **The strip chart maximized (S):** a toggle (a button, a double-click,
  `F`) that gives Live's and the viewer's chart the whole page, the
  cursor's values in a strip beside it, and a splitter between the
  signal list and the chart, remembered per page. On a phone on its
  side the chart takes the page by itself.
- **Chart panes, two cursors and the span's statistics (L, an ADR
  first):** traces in stacked panes on one time axis, each pane with a
  y-axis in its units and its own vertical zoom; a second cursor, with
  the time between the two and each signal's change, lowest, highest
  and mean over the span; a drag that boxes a span to zoom to, and the
  arrow keys stepping the cursor a sample at a time; on Live, a pause
  that scrolls back through the buffer while the session runs. v0.5.0's
  computed signals want panes too.
- **Low-glare and night themes (M):** Theme roles for the analog gauge
  first; warning and danger told apart by more than hue.
- **User-configurable dashboards (L):** pages built from the gauge
  library; layout persistence is the new ground.
- **In-vehicle displays, Android Auto and CarPlay (L, research
  first):** a native front end over the C ABI, only if a live-gauge
  app qualifies.

### Vehicle coverage

- **Custom derived channels (M):** a user's expression per channel, in
  core, on v0.5.0's derived-signal stage.
- **A scanned DID answered "response pending" on a real adapter (S):**
  tested against the fake only; needs an ECU that sends it (BENCH prep
  #17).

### Performance

- **Full signal sets at full rate (L):** left is the bridge's 4 KB copy
  per frame ([docs/testing.md](docs/testing.md#performance)).
- **A frame-rate readout in developer mode (S):** the window's drawn
  frames per second, beside the benchmark's frame and value rates.

### Toolchain

- **C++20 baseline (M, an ADR first):** `std::span` and `std::format`
  against older toolchains dropping out; low priority.
