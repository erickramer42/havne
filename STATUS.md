# Havne: Project Status

**Updated:** 2026-11-10, **Version:** v0.7.1,
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

**Current phase: v0.7.1, a more modern look, phone first, is
out** (its section below): Night as its own control, the S2 look, the
phone's five tabs, joined panel and drive view, the viewer's Export, the
log's minimum time between sweeps and reads in the extended session.
v0.7.0, the dashboard and the UI, is out: the Dashboard is built by hand
from eight kinds of gauge and saved per phone and desktop, with Night
off, on or scheduled in an amber or a red style; Settings is a list of
pages; an analysis log
records the signals picked on Live at their own rate, as a wide CSV; the
app imports CSV and candump logs and shows a recording's frames. v0.6.0
is out, sharing and scripting: everything reaches
`havne-cli` and Python as well as the app, a recording exports scrubbed
as pcapng, CSV or a copy of its folder, a car's health goes in one
file, candump logs and other loggers' CSVs import as recordings, and the
simulated car answers as an ELM327 adapter. v0.5.1, a patch, replays a
recording with its own signal file and shows a refused signal as
refused. v0.5.0 brought
your own signals, as signal files that core reads from bytes and
refuses at load where they're wrong, written per vehicle in the app
(with **Read once**) or converted from an OBDb set by `havne-cli`;
enumerations and computed signals such as boost; plugins' data tier, a
package of signal files whose variants the car's VIN proposes, checked
through the simulated ECU in CI; and Linux downloads for 64-bit ARM.
On the test car
([2026-11-06-own-signals-car](docs/bench-sessions/2026-11-06-own-signals-car.md)),
a signal written in the form and checked with **Read once**, a signal
file's DIDs on two modules, a refused DID, an enumeration and boost read
through the VX and the MX+, in the app and `havne-cli`; plugins have run
only against the simulated car. Changes since then go under
[CHANGELOG.md's "Unreleased"](CHANGELOG.md#unreleased). The J2534 stack is verified
end to end against a fake vendor DLL, through the x64 and the x86
surrogate. One real J2534 adapter, the OBDX Pro VX, has run on a quiet
bench bus and on one car, from
[2026-10-22](docs/bench-sessions/2026-10-22-vehicle.md) to
[2026-11-06](docs/bench-sessions/2026-11-06-own-signals-car.md): the values
decoded and tracked the cluster, and the request engine, the probe,
identification, `uds-generic`, the trouble codes, several PIDs per
request, more than one ECU and a DID scan ran there. The [ELM327 transport](docs/elm327.md)
([ADR-0026](docs/adr/0026-elm327-transport.md)) is built and has read
the same car through the OBDLink MX+ over Bluetooth and the VX in ELM
mode over USB
([2026-10-27](docs/bench-sessions/2026-10-27-elm327-vehicle.md)), and
the Android app reaches the car through the phone's Bluetooth, in the
background too ([roadmap](#roadmap)).

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
    its channels: on the VX's bench, and on the test car through the
    MX+ beside the VX asking, where it hears Single Frame replies only
    (Known Issue #41;
    [2026-11-05-two-ecu-car-rerun](docs/bench-sessions/2026-11-05-two-ecu-car-rerun.md)).
    How it works: [docs/elm327.md](docs/elm327.md).
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
    A read asks the vendor for one message and answers with it, so it
    ends at its reply; it never asks the vendor to wait 0 ms and makes
    no call for the rest of a batch (the VX holds a call asked to wait
    0 ms about 50 ms, and one asked to wait under 10 ms about 10 ms:
    [2026-11-05-two-ecu-car-rerun](docs/bench-sessions/2026-11-05-two-ecu-car-rerun.md)).
    The surrogate holds Windows' 1 ms timer, since a vendor's wait for
    its message is timed in the process's timer steps. Through the fake
    as the VX a reply 4 ms after its request reads in 5.1 ms
    (`J2534HealthTest.VxAReplyEndsTheReadThatWaitsForIt`; ADR-0042's
    notes of 2026-11-04 and 2026-11-05); with the timer, on the test
    car 28 % more replies arrived in the same time (ADR-0042's appendix
    of 2026-11-01, [2026-11-01-vehicle](docs/bench-sessions/2026-11-01-vehicle.md)).
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
    BENCH prep #29). `havne-cli clear <uri> --yes [--engine-running
    [--moving]]` clears through the same gate, each ECU's report written
    first (ADR-0038's appendix of 2026-11-06), against the mock, the
    fake DLL and the simulated ECU; not run on a car.
  - **Profiles** chosen by the URI
    ([ADR-0013](docs/adr/0013-protocol-profiles.md)): `mock`, `raw`,
    `obd2` and `uds-generic`, and a signal file's or a plugin's
    (`user.*`, `plugin.*`). Each signal def states its kind and byte
    order ([ADR-0016](docs/adr/0016-signal-definition-layout.md)).
    `uds-generic` scans a `?did=` range with `?scan=first` back to
    back before polling what it found: about 30 DIDs a second on the
    test car through both adapters, a 1,024-DID block in about 30 s
    ([ADR-0020](docs/adr/0020-uds-generic.md)'s appendices).
    With `?session=03` it reads in the extended diagnostic session:
    `10 03` to each module first, tester present in quiet gaps, `10 01`
    at the close; a module that refuses is read in the default session
    with its code named, and each module's report, the Log page,
    `havne-cli`'s `session` line and Python say which session it was
    read in
    ([ADR-0060](docs/adr/0060-extended-session-reads.md)). Run only
    against the fakes and the simulated ECU so far.
  - **Your own signals** ([ADR-0051](docs/adr/0051-signal-files.md)):
    signal files, JSON that core reads from bytes and refuses at load
    where it's wrong, holding Service 01 PIDs and Service 22 DIDs on up
    to four modules, numbers, text, enumerations and computed signals
    such as boost; written per vehicle in Settings' form (with **Read
    once**), converted from an OBDb set by `havne-cli signals import
    obdb`, saved out with **Save a copy**, and read by `havne-cli
    --signals`. Plugins' data tier
    ([ADR-0052](docs/adr/0052-plugin-framework.md)): a package of signal
    files whose variants the car's VIN proposes. A recording holds the
    file it decoded with and replays through it; a signal its module
    refuses shows as refused. Verified on the test car through the VX
    and the MX+
    ([2026-11-06-own-signals-car](docs/bench-sessions/2026-11-06-own-signals-car.md)),
    plugins against the simulated car only. Settings imports an OBDb
    set itself (ADR-0051's appendix "the OBDb import in the app"), the
    report shown before Keep, a re-import replacing the vehicle's
    earlier one; bridge- and QML-tested on a synthetic set, not run on
    the test phone. JSON Schemas in `schema/`
    for editors and other repositories' CI, kept to the loaders' keys by
    a test. Left: naming a scanned DID's refusal and a replay's (the
    backlog), a plugin on a car.
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
  - **The UI:** the Dashboard, built in the app
    ([ADR-0057](docs/adr/0057-dashboards.md)): pages of dials, rings,
    bars, cells, digits, numbers, sparklines and shift lights for any
    numeric signal of the profile, each with its label, range, zones,
    decimals, size and place, saved as made, one set for every vehicle,
    the phone's and the desktop's apart, with peak hold and stale
    dimming; Night (off, on or scheduled, in an amber or a red style)
    beside the Dark look; on the phone, five tabs, a joined panel of
    gauges and a drive view with the bars hidden
    ([ADR-0059](docs/adr/0059-drive-view.md)); Settings as a list of
    pages; Live's table and strip chart, the Log page's activity log, the
    Recordings page's viewer (pan, zoom, a cursor, the frames check's
    findings, two runs overlaid and aligned, a vehicle's baseline
    recording overlaid on a new run aligned on the engine starts, a Mark
    while recording, and Export: the whole recording as a CSV, wide or
    long, pcapng or a folder copy, scrubbed, through the system's
    pickers), one signal filter on Live and the viewer (words, plotted
    only, the part of the car), and Settings composing the URI. Live's
    and the viewer's chart can take the whole page (a button, a
    double-click or `F`; on a phone on its side by default), a splitter
    sets the list's share, and a pinch or the wheel steps Live's
    window; pages are denser on a phone's width or height. Developer
    mode (off until switched on): the `mock://` preset, a frame-rate
    badge, and the Log page's raw frame table with its bridge counters:
    a filter (ID, data, direction, flag), a By ID view (count, rate,
    newest payload), selected rows copied to the clipboard as they are,
    and Export CSV, scrubbed by core (ADR-0054's appendix "frames in
    memory"), by ID or mask, length and time, and the changed bytes
    marked. Kept between runs: the theme, zoom,
    metric or imperial units
    ([ADR-0022](docs/adr/0022-display-units.md)), the last URI that
    connected, the Record sessions switch, the splitters' shares and
    each device's versions as last seen at a connect (shown on its row
    in Settings, since the list opens no adapter).
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
      PIDs, packs, and decodes only engine speeds the car sent. With no
      bus, on every platform, `ecuio_sim_ask` answers one request, and
      `havne-cli sim <uri> --elm327 tcp:[<address>:]<port>` puts an
      ELM327 adapter's text in front of it
      ([ADR-0055](docs/adr/0055-elm327-emulator.md)): core's
      `elm327://` session and python-OBD 0.7.3 read it on Windows
      (python-OBD strips the characters 0, 1, 2, x and backslash from
      both ends of a text item, so the fake VIN reads shorter there).
      A pty on Linux and macOS is to come.
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
    desktop (Xubuntu 26.10, X11, `mock://demo` with recording). The
    app's suite passes on that machine, and the core's on a Raspberry
    Pi 3 (aarch64), with no warnings; on both, the simulated bus's
    cases and `havne-cli` end to end over `vcan0` (the simulated ECU
    answering through ISO-TP, recorded and checked) (2026-11-05,
    [docs/linux.md](docs/linux.md#what-has-run-where)). A laptop on
    native Ubuntu 26.04 has run the MX+ over Bluetooth, the VX in ELM
    mode on USB and WiFi and the USB2CAN listening, on the test car
    ([2026-11-08-linux-car](docs/bench-sessions/2026-11-08-linux-car.md));
    no requesting session has sent through a real CAN controller.
  - **Reply timing** ([ADR-0042](docs/adr/0042-reply-timing.md)): every
    requesting session measures, per request in its rotation, its
    outcomes, its reply latency (host clock) and its refresh interval
    on the frames' own stamps (the adapter's for J2534, the kernel's
    for SocketCAN, the host's for ELM327) at P50, P99 and P99.9, into a
    recording's `latency.csv`, `havne-cli`'s `timing` lines and the
    activity log's summary at the disconnect; `bench_timing` measures
    what the stack adds through each transport against the fakes
    ([docs/testing.md](docs/testing.md#reply-timing-against-the-fakes)).
    On the test car through the VX, the P50 per request (the median
    across the rotation) is 16.4 ms packed, about 2.5 ms past a
    six-PID pack's time on the bus, and 4.6 ms one PID a request
    ([2026-11-05-two-ecu-car-rerun](docs/bench-sessions/2026-11-05-two-ecu-car-rerun.md),
    A2);
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
  - **The analysis log** ([ADR-0056](docs/adr/0056-analysis-log.md)):
    the app's Start log and Stop log, and `havne-cli log`, ask the ECU
    for the signals picked on Live alone, so they refresh at the bus's
    own pace, into a recording that exports as one wide CSV, a row a
    sweep (`ecuio_session_set_focus`, `ecuio_session_record`,
    `ecuio_export_estimate`). Verified at UNIT, in the bridge, QML and CLI
    tests, and on the test car over ELM327 on native Linux: the MX+
    reached 27.45 sweeps a second at one signal and 15.48 at eight, the
    VX on USB 47.38 and 6.63, every reply after the connect carrying
    only the picked PIDs
    ([2026-11-08-linux-car](docs/bench-sessions/2026-11-08-linux-car.md)).
    Left: a log through J2534 and SocketCAN, the viewer's Export.
  - **Evidence for field sessions:** opt-in recording of every frame,
    decoded value and activity line per connect
    ([ADR-0017](docs/adr/0017-session-recording.md)), written by core's
    recorder on a thread of its own, so the frames the Log page drops
    under load are recorded too
    ([ADR-0049](docs/adr/0049-recordings-in-core.md)), and read back
    offline by core's reader, the Recordings page and `havne-cli
    view`, through the values as recorded or through the frames by a
    profile (`ecuio_recording_open_frames`, so a raw recording plots),
    exported by `havne-cli export` as pcapng (its frames), CSV (its
    values, a row a sweep in the wide layout) or a folder (a copy that reads and
    replays as the original), scrubbed by construction and checked
    ([ADR-0054](docs/adr/0054-exports-and-imports.md)), summed up as
    a scrubbed health report by `havne-cli health`, and a
    `candump -L` log or another logger's CSV imported as a recording
    (`havne-cli import`, and the Recordings page's Import, which guesses
    a CSV's map from its header); the app exports the Log page's frames
    and a report, and a surrogate
    session log of every vendor call, with every request and reply
    while recording. Session time counts the machine's sleep on every
    platform, so a sleep is a gap on Live and in a recording, and the
    activity log and `havne-cli` say how long it was
    ([ADR-0033](docs/adr/0033-session-time-counts-sleep.md); the test
    phone's sleeps, [2026-10-29-android-2](docs/bench-sessions/2026-10-29-android-2.md);
    a Linux machine's suspend is left, BENCH prep #27).
- **Not yet:** real hardware beyond three adapters (the VX, the MX+ and
  the USB2CAN) and one car: 29-bit
  addressing, a real x64 driver, and other vendors ([BENCH prep](docs/bench-sessions/bench-prep.md),
  [testing.md](docs/testing.md#gaps-not-verified)). The run sheets are
  [bench-runsheet.md](docs/bench-sessions/bench-runsheet.md),
  [vehicle-runsheet.md](docs/bench-sessions/vehicle-runsheet.md), the
  ELM327 [bench](docs/bench-sessions/elm327-bench-runsheet.md) and
  [vehicle](docs/bench-sessions/elm327-vehicle-runsheet.md) sheets, the
  [phone's](docs/bench-sessions/android-phone-runsheet.md), the SocketCAN
  [desk](docs/bench-sessions/socketcan-desk-runsheet.md) sheet, and
  [two-ecu-car-runsheet.md](docs/bench-sessions/two-ecu-car-runsheet.md)
  (both ECUs in one session, the recorders on a car, a second listener).
- **Tests:** 1931 ctest cases on Windows (both trees rebuilt,
  2026-11-10), all passing but six skips (the cross-arch case and
  the real-car recordings' diff, run locally; three cases that run on
  Linux only, Read once over SocketCAN and the Python package's vcan
  and ISO-TP fuzzing cases; and the Wireshark export's `tshark` run,
  without tshark installed), plus the
  cross-arch subset (103/103); 0 MSVC warnings, and a GCC or Clang
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
| J2534 | HARNESS-verified through the surrogate: x64 in CI, x86 cross-arch. BENCH: one adapter, quiet bus. Vehicle: one car, through the surrogate, from 2026-10-22 to 2026-11-04 | [docs/j2534.md](docs/j2534.md) |
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
the VX clock's zero (#5), the VX's serial port on Windows without
OBDX's INF (#11), response pending (#17), the VX's WiFi while
its USB port is held (#20, in part), a BLE adapter (#25), a real CAN controller that sends with
one-shot mode (#32), a sleep on a Linux
machine (#27), and a busy bus through the surrogate (#34). Any other adapter starts the list again.

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
- **#41. Through the OBDLink MX+, a listening session hears Single
    Frame replies only.** The MX+'s monitor misses every frame that
    follows another within about 1 ms: another tester's Flow Control
    and the ECU's Consecutive Frames after a First Frame. A multi-frame
    reply never completes, so it is dropped and nothing is delivered for
    it. No setup tried changes that (`AT CAF0` or `CAF1`, `AT CFC0`, with
    or without `AT CRA`, `AT MA` or `STMA`, `STCSEGR1`), and the frames
    are on the bus: a SocketCAN listener on the same splitter hears
    them (BENCH prep #19,
    [2026-11-04-two-ecu-car](docs/bench-sessions/2026-11-04-two-ecu-car.md),
    [2026-11-05-two-ecu-car-rerun](docs/bench-sessions/2026-11-05-two-ecu-car-rerun.md)).
    A listening session needs no flow control, so its setup still
    turns it off (`AT CFC0`). Untried: `STCMM`'s other modes
    (some acknowledge on the bus) and `STM`'s filters. To listen to
    multi-frame replies, use a SocketCAN controller in listen-only mode,
    or the VX in ELM mode, which hears them (BENCH prep #19,
    [2026-11-08-linux-car](docs/bench-sessions/2026-11-08-linux-car.md)).
- **#42. Through the OBDX Pro VX, a second module's reply can be lost
    while the first module's segmented reply comes in.** On the test
    car, a session asking the engine (0x7E8) and a second module
    (0x7E9) lost 22% of the second module's single-frame replies with
    the engine's `obd2` requests packed, and 1.1% with `?pack=0`; nearly
    every lost one was due within 30 ms of a 0x7E8 reply of several
    frames. Each write was accepted and no reply came late; with one DID
    asked of each module, and through the MX+, none was lost
    ([2026-11-06-own-signals-car](docs/bench-sessions/2026-11-06-own-signals-car.md),
    BENCH prep #35). The request goes out again on its next turn, so a
    signal still updates, less often. For a session that asks two
    modules through the VX, `?pack=0` keeps the losses near 1%.

## Roadmap

| Version | Scope | Status |
|---|---|---|
| v0.7.0 | The dashboard and the UI: gauges built by hand from a library, an analysis log and a wide CSV, import in the app, themes for the dark, a phone-first Settings | Released |
| v0.7.1 | A more modern look, phone first: Night as its own control, the S2 look and shared controls, the phone's five tabs, joined panel and drive view, the viewer's Export, the log's minimum time between sweeps and reads in the extended session | Released |
| v0.7.2-v0.10.0 | v0.7.2, finishing the Dashboard and signals: the desktop's gauges in the new design, undo in edit mode, a Daylight look, a signal file's dial range and warning levels, Python's exports and health report. v0.7.3, the analysis viewer: the log's navigation aids, chart panes with two cursors, Live's pause. v0.8.0, conditions: triggered logging and alerts. v0.9.0, the whole vehicle: discovery and extended sessions. v0.10.0, the scan tool, the last feature release | Planned |
| 1.0.0 | The finished reader: the features above frozen and hardened, full featured on Windows, Linux and Android | Planned |

Versions follow semantic versioning
([ADR-0048](docs/adr/0048-semantic-versioning.md)): each feature
release raises the minor number; a patch carries fixes and follow-up
work on its minor's theme, and never breaks the C ABI, `havne-cli`'s
schema or a plugin format (ADR-0048's appendix "what a PATCH may
carry"). Earlier releases are in [CHANGELOG.md](CHANGELOG.md). The
planned releases are in order and hold about three features each; a
release's detail comes in here when its work starts.

### v0.7.1: a more modern look, phone first

For people who read the Dashboard in a car, and anyone who wants a
recording's values out of the app. A patch on v0.7.0's theme
([ADR-0048](docs/adr/0048-semantic-versioning.md)'s appendix "what a
PATCH may carry"): the C ABI grows, append-only
(`ecuio_session_set_focus_with`, `ecuio_export_with`,
`ecuio_export_estimate_with`, the session fields of
`ecuio_channel_support_t`).
- **Built:**
  - Night as its own control, off, on or scheduled, in an amber or a
    red style, with a one-tap switch on the Dashboard
    ([ADR-0058](docs/adr/0058-night-mode.md));
  - the S2 look: a graphite palette, Barlow type with fixed-width
    digits for readings, flat cards and gauges, button ranks, one type
    scale, and shared text fields and check boxes;
  - the phone's Dashboard on 6 columns upright and 12 on its side, with
    a stored v0.7.0 layout upgraded in memory, one joined panel, one
    tool row, a gauge's press and hold for its menu or a drag, copy and
    paste, a blank cell, and cells that fill the empty squares under
    them ([ADR-0057](docs/adr/0057-dashboards.md)'s appendices);
  - the phone's five tabs, the address in the top bar's status, and
    Dash's list of recent addresses with no session;
  - the drive view: the gauges to the screen's edges with every bar
    hidden, revealed by a tap, kept awake while connected on external
    power or with Dash showing, entered again at the next connect, and
    Android's Back as one route across the app
    ([ADR-0059](docs/adr/0059-drive-view.md)); on the test phone with
    the mock, three-button and gesture navigation, not on a car;
  - the viewer's Export ([ADR-0056](docs/adr/0056-analysis-log.md)'s
    appendix "the viewer's Export as built") and `havne-cli export
    --estimate`, both stopped cleanly when the app quits or on Ctrl+C
    (ADR-0054's appendix "an export canceled");
  - a focus's minimum time between sweeps, in the app, `havne-cli log
    --min-sweep` and Python (ADR-0056's appendix "the minimum time
    between sweeps as built"); no real adapter has run it;
  - reads in the extended session, `?session=03` on `uds-generic`
    ([ADR-0060](docs/adr/0060-extended-session-reads.md)), with each
    module's session on the Log page, in `havne-cli` and in Python; run
    against the fakes and the simulated ECU only;
  - the Recordings page sized for a phone's touch, and each gauge one
    screen-reader item on Android.
- **Left:** README's screenshots in the new look, and the review and
  docs passes.

**Done when:** the phone's pages use the shared controls in all three
palettes, with no cut text at the test phone's width, and the README
shows them.

### Waiting on a tester: an iPhone download

No version until a tester has run CI's unsigned `.ipa` on an iPhone
and the run is filed as a session sheet. Then the release workflow
builds and publishes `havne-X.Y.Z-ios-arm64-unsigned.ipa` with the
other downloads (provenance, the SBOM and the checksums), the release
checks read its version and that it carries no signature, and the
notes' Try it points at [docs/ios.md](docs/ios.md).

### 1.0.0: the finished reader

After v0.10.0 the formats freeze (the C ABI, `havne-cli`'s schema, the
plugin formats, the recording format) and the releases are fixes
alone, as 0.10.x: a second car and testers other than the developer,
every adapter class on a car, the release on a PC without Visual
Studio, Linux on real hardware, accessibility and a user guide, and
hour-long runs on every transport. 1.0.0 follows once those runs are
filed and the 0.10.x releases have gone a stated stretch with no
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
- **Chart panes, two cursors and the span's statistics (L, an ADR
  first):** traces in stacked panes on one time axis, each pane with a
  y-axis in its units and its own vertical zoom; a second cursor, with
  the time between the two and each signal's change, lowest, highest
  and mean over the span; a drag that boxes a span to zoom to, and the
  arrow keys stepping the cursor a sample at a time; on Live, a pause
  that scrolls back through the buffer while the session runs. v0.5.0's
  computed signals want panes too.
- **In-vehicle displays, Android Auto and CarPlay (L, research
  first):** a native front end over the C ABI, only if a live-gauge
  app qualifies.

### Vehicle coverage

- **Custom derived channels (M):** a user's expression per channel, in
  core, on v0.5.0's derived-signal stage.
- **Every refusal named (M, an ABI addition):** a DID `uds-generic`'s
  scan finds refused, and any refusal in a replay, show no value where
  a live session's polled request reads Refused: the scan's refusals
  are only counted in the support report, and a recording keeps a
  refusal as a note's words
  ([ADR-0051](docs/adr/0051-signal-files.md)'s appendix "a refused
  signal shown as refused").
- **A scanned DID answered "response pending" on a real adapter (S):**
  tested against the fake only; needs an ECU that sends it (BENCH prep
  #17).

### Performance

- **`FrameListModel` inserts a row per frame (S):** one
  `beginInsertRows` for each frame of a delivery; a batch insert would
  signal the view once. Measure first
  ([docs/testing.md](docs/testing.md#performance)).
- **Full signal sets at full rate (L):** left is the bridge's 4 KB copy
  per frame ([docs/testing.md](docs/testing.md#performance)).

### Toolchain

- **A Release iOS build may link Qt's `qmltooling` plugins (S):** check
  whether the `.ipa` carries them and, if so, exclude them with
  `qt_import_plugins(havne EXCLUDE_BY_TYPE qmltooling)`; needs a CI run
  and an Apple kit.
- **The next type2-runtime pin (S):** the steps are in
  [docs/development.md](docs/development.md) ("A new runtime pin"); the
  newer runtime would take the release that unpacks into a 0700 folder.

- **C++20 baseline (M, an ADR first):** `std::span` and `std::format`
  against older toolchains dropping out; low priority.
