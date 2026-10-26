# QtTune — Project Status

**Updated:** 2026-10-25 · **Version:** v0.3.1 ·
[![CI](https://github.com/erickramer42/qttune/actions/workflows/ci.yml/badge.svg)](https://github.com/erickramer42/qttune/actions/workflows/ci.yml)

This file covers the project as it stands: what works, what comes
next, and what is known to be wrong. For how the parts fit together,
start with [docs/architecture.md](docs/architecture.md). Why it is
built this way is in [docs/adr/](docs/adr/README.md). How it got here, with lessons
learned, is in [docs/engineering-log.md](docs/engineering-log.md).
Safety, regulatory and warranty caveats are in
[DISCLAIMER.md](DISCLAIMER.md).

## Where things stand

**Current phase: v0.3.x.** v0.3.1 is released, the first with a
Windows download; v0.3.0 was tagged 2026-10-22.
The HARNESS tier is complete, including transport adoption (ADR-0012);
the BENCH tier has covered a real adapter on a quiet bus; and in the
first read-only vehicle session
(2026-10-22, [session sheet](docs/bench-sessions/2026-10-22-vehicle.md))
the engine ECU answered the Mode-01 requests with replies that decode
to plausible values. A second session on the same car (2026-10-25,
[session sheet](docs/bench-sessions/2026-10-25-vehicle.md)) ran the
request engine, the probe, identification and `uds-generic` there for
the first time. Since the tag: the link-health tests run on both
backends against a fake modeled on the VX, a failed connect says why
([ADR-0018](docs/adr/0018-connect-failure-reporting.md)), a session
says what it runs on ([ADR-0019](docs/adr/0019-session-info.md)),
J2534 frames carry session time, the QML pages and the signal decoder
have tests of their own (Qt Quick Test, libFuzzer), requests follow
the ECU's replies instead of a fixed slot
([ADR-0015](docs/adr/0015-request-engine.md)), the probe drops
what each ECU says it doesn't support, a frame carries a whole
ISO 15765 message, `uds-generic` reads and scans the DIDs a URI
names ([ADR-0020](docs/adr/0020-uds-generic.md)), every session
asks its ECU who it is
([ADR-0021](docs/adr/0021-vehicle-identification.md)), and Settings
composes the URI from a preset, a profile row and a DID field. Next
is the plan in [After v0.3.0](#after-v030).

- **Works:**
  - The mock transport drives the live frame log, LiveView and the
    strip chart with a scripted drive cycle: a cold start, 2nd- and
    3rd-gear pulls onto the fuel-cut limiter, a highway cruise, then a
    quarter-mile run off a wheelspin launch. Only the first loop runs
    cold; later loops carry the heat over.
  - J2534 runs end to end against the fake vendor DLL, both in-proc
    and through the x86 surrogate. That includes write/ioctl/filter
    relay on both backends and transport adoption: filters, the Mode-01
    request loop, and frame identity via `?filter=`/`?txid=`/`?poll=`
    (ADR-0012).
  - A live vehicle, read-only, through the OBDX Pro VX and the x86
    surrogate (2026-10-22,
    [session sheet](docs/bench-sessions/2026-10-22-vehicle.md)): the `obd2`
    rotation, `?poll=` and the supported-PID bitmaps were answered by
    the engine ECU on `0x7E8`, and the decoded values tracked the
    cluster through a cold start and at idle.
  - The surrogate child is single-threaded and reads travel over the
    pipe ([ADR-0014](docs/adr/0014-single-threaded-surrogate.md)):
    vendor calls are serialized by construction, and the vendor's
    read status reaches the transport on both backends (timeout, empty
    and hard failure are told apart).
  - Profile selection ([ADR-0013](docs/adr/0013-protocol-profiles.md),
    slices 1–3): a string-keyed registry selects the session's signal
    set. `mock://` defaults to the mock set, `j2534://` defaults to
    `obd2`, and `?profile=<name>` overrides either. Unknown names are
    rejected at session_create, before any transport attach or DLL
    load. The bridge decodes the session's active set. The `obd2` set's
    defs give the transport its Service 01 requests, and the set
    declares its CAN channel (7E0/7E8), so the transport derives the
    receive filter and request target from the profile: a bare
    `j2534://` URI starts and streams with no `?filter=` required.
    `?filter=` and `?txid=` override the derived channel, and `?poll=`
    swaps the rotation for one PID under any profile.
  - Requests ([ADR-0015](docs/adr/0015-request-engine.md)): a def's mux
    key names the reply it reads, and so the request (`41 <PID>` asks
    for `01 <PID>`, `62 <DID>` for `22 <DID>`), so one set can mix
    Service 01 and 0x22, and nothing but reads (0x01, 0x09, 0x22) is
    ever sent. Each channel has one request out at a time. The next
    goes out when the reply is in, positive or negative ("response
    pending" waits up to 5 s), or after a 100 ms wait, and no sooner
    than 20 ms after the last; every mode drains what has arrived.
    Against the fake that's about 30 requests a second, where the fixed
    slot sent 10. No vehicle has run it yet.
  - The PID-support probe (ADR-0015 §4): before its rotation, each
    ECU's supported-PID bitmaps are read, and a request leaves the
    rotation only when the ECU says it isn't supported (a clear bit, or
    a negative reply that can't change in the session), never on
    silence. A connect made with the ignition off reads them once the
    ECU answers. The activity log names each ECU, what it supports and
    what was dropped. `?probe=0` sends the set as written. On the first
    vehicle's recorded bitmaps it drops `0x5C` alone; no vehicle has
    run it yet.
  - Signal definitions
    ([ADR-0016](docs/adr/0016-signal-definition-layout.md)): each def
    states its value kind (unsigned, signed or
    binary32 float) and its byte order, and decodes in `double` with
    16-bit lengths; mux keys run to 3 bytes. A set carries a layout
    version and zeroed reserved room, and a refused registration
    names the failing check and the def or channel it failed on.
  - Discovery finds the real installed drivers, and the Settings page
    can pick one and connect.
  - Connect and disconnect keep the slow vendor calls off the GUI
    thread. A teardown runs on its own thread: a wedged adapter's
    (about 6 s at most) shows "Disconnecting…" on every connection
    surface, and no connect starts until it ends, because the adapter
    may refuse a second open until its close returns.
  - A connect explains itself. A failed one returns a code for its
    cause and says why in words, with the vendor's
    `PassThruGetLastError` text
    ([ADR-0018](docs/adr/0018-connect-failure-reporting.md)). A
    successful one logs a "Session:" line: the backend and the DLL's
    architecture, protocol and baud, the profile and its channels, and
    the adapter's battery voltage and firmware, DLL and API versions
    ([ADR-0019](docs/adr/0019-session-info.md)).
  - Link health: the J2534 worker tells a quiet bus, a dead adapter
    and failing requests apart
    ([docs/j2534.md](docs/j2534.md#session-lifecycle)), the bridge logs
    transitions to
    the Settings activity log, and the header pill, LiveView, the Home
    status card, the Dashboard header and the Settings connection label
    all show the state. On hardware, ok and quiet showed on the
    vehicle, quiet through an ignition cycle and back, and "Requests
    failing" on the VX with no vehicle (all 2026-10-22). The red fault
    state hasn't been seen under the current rules: no vendor read has
    failed.
  - Dashboard: analog, ring, digital and bar gauges plus shift lights,
    fed by name through `SignalHub` so one layout serves both the mock
    and `obd2` profiles. Gauges show peak hold, dim stale values, and
    say "not in profile" instead of showing zero (PR #12).
  - Surrogate diagnostics: client and child write one session log,
    both stamped `HH:MM:SS.mmm`, so a GUI launch keeps the whole story
    without a stderr redirect; a second concurrent session gets its own
    log (`995519b`, PR #11), and each spawn keeps the log it replaces,
    the newest 50. With `ECUIO_SUR_LOG_FRAMES=1`, set at launch or by
    recording, the child also logs every request and reply verbatim,
    so a bench or vehicle session records its payloads without
    screenshots.
  - Session recording
    ([ADR-0017](docs/adr/0017-session-recording.md)): with Record
    sessions ticked in Settings (off by default), each connect writes
    a folder under `Documents/QtTune/recordings` holding every frame
    the bridge receives (`frames.csv`, host and vendor time side by
    side), every decoded value (`signals.csv`) and the activity lines
    (`events.log`). `frames.csv` is the format the record-replay
    transport will read ([After v0.3.0](#after-v030)).
- **Not yet:**
  - Real-hardware coverage beyond one adapter and one vehicle. Every
    hardware session so far ran the OBDX Pro VX through the surrogate:
    quiet-bus BENCH sessions on 2026-10-16, 2026-10-20, 2026-10-22 and
    2026-10-25
    ([2026-10-16](docs/bench-sessions/2026-10-16.md),
    [2026-10-20](docs/bench-sessions/2026-10-20.md),
    [2026-10-22](docs/bench-sessions/2026-10-22.md),
    [2026-10-25](docs/bench-sessions/2026-10-25-bench.md), a laptop),
    and two vehicle sessions on one car. Untested on hardware: 29-bit
    addressing, more than one ECU answering, and the in-proc backend. The VT, and the VX,
    refuse to open with status 34 when their adapter is absent. The
    run sheets are
    [bench-runsheet.md](docs/bench-sessions/bench-runsheet.md) and
    [vehicle-runsheet.md](docs/bench-sessions/vehicle-runsheet.md).
  - **Tests:** 413 ctest cases (Windows, Visual Studio 18 build
    tools, both trees rebuilt, 2026-10-25), the QML suites
    among them, plus the cross-arch subset (75/75). All passed except
    the env-gated cross-arch skip. Every first-party target builds with
    compiler warnings on, at 0 on MSVC. CI was green on all nine jobs on
    PR #17 (`ce8284f`, merged as `1145d11`), the simulator smoke
    included, and on every push run since, the latest at `1ab2dd1`
    (read from the GitHub API on 2026-10-23). That covers ctest on
    Windows, macOS and Linux, core under ASan and UBSan, 60 s of each
    libFuzzer harness ([fuzz/README.md](fuzz/README.md)), the Android
    and iOS device cross-builds, and the simulator smoke. The smoke runs
    on pull requests and on request, not on every push, because it
    roughly doubles CI time. The cross-arch routing case skips unless
    its environment variables are set. J2534 suites carry a 30 s
    per-test timeout, so a hang fails bounded instead of wedging the
    runner. Tiers, suites and gaps are in
    [docs/testing.md](docs/testing.md).

## Architecture

```
+--------------------------------+
| Qt Quick UI (QML)              |  Home · Dash · Live · Log · Settings
|                                |  sidebar at >= 720 px, TabBar below
+---------------+----------------+
                | QtTuneBridge: worker -> GUI marshaling, 100 ms coalescing
+---------------v----------------+
| ecuio-core (C++17, C ABI)      |  no Qt (CI-enforced)
|  sessions + frame callbacks    |
|  signal decode                 |
|  transports: mock | j2534      |  selected by URI
|  j2534 loader: in-proc | x86   |  routed by the DLL's PE machine type
+---------------+----------------+
                | named pipe (control + batched reads, ADR-0014)
                | + kill-on-close job object
+---------------v----------------+
| ecuio-j2534-surrogate (x86)    |  hosts 32-bit vendor DLLs
+--------------------------------+
```

Three rules ([ADR-0001](docs/adr/0001-qt-free-core-c-abi.md)):

1. **Core never links Qt.**
2. **UI layers receive final display values, never raw frames.**
3. **The C ABI is the FFI boundary.** That means opaque handles, flat
   functions, POD structs and append-only enums, every name carrying
   the core's prefix (`ecuio_`, `ECUIO_`).

## Data path

```
mock worker (50 ms tick)  -or-  J2534 worker (request engine + ReadMsgs;
      |                         in-proc, or batched over the surrogate's pipe)
      | ecuio_frame_t
      v
core: snapshot dispatch to registered callbacks ............ ADR-0002
      | QtTuneBridge::onFrame (worker thread): copy, in-flight gate, post
      v
GUI thread: handleFrameInternal ............................ ADR-0003
      |- SessionRecorder (opt-in): frames.csv, signals.csv,
      |    events.log .......................................... ADR-0017
      |- FrameListModel (a 10k ring, newest or oldest first) -> LogPage
      '- decode, gated on ID + mux key -> staging buffer ... ADR-0004
             | 100 ms timer
             v
         SignalListModel::updateValues() -> LiveViewPage
             table + strip chart (pulled via chartData()) .. ADR-0005
```

Contracts along this path:

- **Backpressure** bounds frames that are queued but not yet handled
  (1000), never the cumulative total. The stream never freezes.
- **LiveView** updates at 10 Hz whatever the frame rate. The mock runs
  at 20 frames/s, and J2534 may run far faster.
- **Strip-chart samples are batch-stamped:** one timestamp per 100 ms
  window.
- **J2534 teardown:** the worker is joined before any call that
  unmaps the vendor DLL. See [docs/j2534.md](docs/j2534.md#session-lifecycle).

## Components

| Component | State | Reference |
|---|---|---|
| Core sessions and callbacks | Stable. The frame ABI and close/destroy split changed in v0.3.0. | [ADR-0002](docs/adr/0002-callback-pair-identity-snapshot-dispatch.md), [ADR-0006](docs/adr/0006-frame-abi-v0.3.md) |
| Signal decode | Linear scaling in `double`. Each def states its value kind (unsigned, signed, binary32 float) and byte order (Intel at any bit offset, big-endian in whole bytes), and is gated on message ID plus a mux key of up to 3 bytes. Two signal sets, `mock` and `obd2`, chosen by profile. The validator accepts exactly the defs the decoder decodes, with named return codes; the profile registry runs it at registration and reports the failing check and element. | [ADR-0004](docs/adr/0004-signal-decoding-model.md), [ADR-0013](docs/adr/0013-protocol-profiles.md), [ADR-0016](docs/adr/0016-signal-definition-layout.md) |
| Mock transport | A scripted ~2-minute drive cycle on a vehicle model (`core/src/mock_drive_cycle.h`): a cold start with the first vehicle's fast idle, a 2nd-gear WOT pull from ~1550 rpm onto the 7000 rpm fuel-cut limiter with a 5 s hold, a longer 3rd-gear pull with the same hold, a 100 km/h cruise in 4th, then a quarter mile off launch control with wheelspin in 1st (~13.7 s, ~172 km/h trap). Only the first loop starts cold; each later one starts from where the last ended, so the second warms through the thermostat, from the third the coolant runs 88–102 °C (the fan cutting in on the quarter mile), and by the fifth the oil has built to 108–117 °C under the repeated pulls. The set carries lambda beside the eight shared quantities. Every tick keeps the signals consistent with each other (rpm with wheel speed, MAF with load, lambda with fuel cut) and round-trips through the mock set, in `CoreTest.MockDriveCycle*`. | — |
| Bridge and models | Frame log with sort toggle; coalesced signal model with a 120-sample history per signal; device discovery for the picker; link-health polling. QML reaches it as the `Bridge` singleton. | [ADR-0003](docs/adr/0003-backpressure-bounds-in-flight-frames.md), [ADR-0005](docs/adr/0005-coalesced-signal-updates.md) |
| QML pages | Home, Dashboard (gauges and shift lights through `SignalHub`), Live (table, and a strip chart with each trace's value at its end), Log (with Pause), Settings (presets, the device picker, a profile row and a DID field composing an editable URI, and the one place a session is connected). The Dashboard rearranges to fit a short, wide window, short pages scroll with their scroll bar shown, and the whole UI zooms (Ctrl + / Ctrl − / Ctrl 0, or Settings' Display row), kept between runs. Names, units and decimal places come from core, and Settings' Units row shows every page in metric or imperial, converted in core and kept between runs ([ADR-0022](docs/adr/0022-display-units.md)). `Theme` holds the palette, fonts, page margin and the link-state color and label functions; `PageHeader` gives the pages one title row. `test_qml` runs the pages and the app's window against a real bridge, offscreen: the link-health surfaces, Live's swatches and dashes, `SignalHub` and the gauges' notes. | [docs/architecture.md](docs/architecture.md#the-ui-layer) |
| J2534 | HARNESS-verified, same-arch and cross-arch, including transport adoption (ADR-0012), profile-derived channels (ADR-0013) and the request engine (ADR-0015). BENCH: one adapter, quiet bus. Vehicle: one car, read-only, through the surrogate, before the request engine. | [docs/j2534.md](docs/j2534.md) |

## v0.3.0 checklist

- [x] **Phase 1, signals layer:** POD ABI, scale/offset decode, bit
      extraction, sign extension, validator
- [x] **Phase 1b, mock signal emission:** tick-driven simulation, with
      a round-trip test on real worker frames
- [x] **Phase 2, coalesced LiveView and strip chart**
- [x] **Phase 3, J2534 HARNESS tier:**
  - discovery, loader and session transport
  - URI parameters and the device picker
  - frame ABI redesign and the message-ID gate
  - x86 surrogate host and PE routing
  - v04.04 types verification
  - write/ioctl/filter relay
- [x] **Phase 3 Part 5, transport adoption:**
      `?filter=`/`?txid=`/`?poll=`, header strip and ID stamping, and
      filter teardown ([ADR-0012](docs/adr/0012-j2534-transport-adoption.md)).
- [x] **Phase 3 close, BENCH tier and the first vehicle:**
  - [x] first real vendor DLL load and connect: the OBDX Pro VX through
    the surrogate, formal session 2026-10-16
    ([session sheet](docs/bench-sessions/2026-10-16.md)). No x64
    J2534 adapter is on hand, so the in-proc backend has run only
    against the fake ([testing.md](docs/testing.md#gaps-not-verified))
  - [x] **Surrogate vendor-call serialization:** one vendor call at a
        time, one disconnect per channel, and no unload under a running
        call; this cleared the BENCH gate (2026-10-17)
  - [x] **Single-threaded surrogate, reads over the pipe**
        ([ADR-0014](docs/adr/0014-single-threaded-surrogate.md),
        2026-10-19). Read batches that arrive with `ERR_TIMEOUT` are
        delivered.
  - [x] work through [BENCH prep](#bench-prep): the quiet-bus items
    on the VX (2026-10-16, 2026-10-20, 2026-10-22) and the talking-bus
    items on the first vehicle (2026-10-22); items 1–3 and 5–9
    settled for the VX, item 4 unexercised (the vehicle is 11-bit)
  - [x] first live, read-only vehicle session, 2026-10-22
    ([session sheet](docs/bench-sessions/2026-10-22-vehicle.md))
- [x] **Protocol profiles** (ADR-0013), slices 1–3. The PID-support
      probe came after the tag, and `uds-generic` is next
      ([after v0.3.0](#after-v030)):
  - [x] slice 1: profile registry, `raw` profile, `?profile=`
        override, bridge reads the session's active signal set
  - [x] slice 2: the `obd2` signal set and the multi-PID scheduler
        replacing the single-poll loop (paired with the read-rate
        cap — one workstream per ADR-0013)
  - [x] slice 3: channel declarations (§4) — profiles declare rx/tx
        CAN channel pairs; the transport registers one flow-control
        filter per channel and routes scheduler requests per PID; a
        bare `j2534://` URI under `obd2` starts with no `?filter=`
- [x] **Signal definition layout**
      ([ADR-0016](docs/adr/0016-signal-definition-layout.md)): value
      kinds and explicit byte order, `double` decode, 3-byte mux keys,
      a set layout version, zero-checked reserved room, and validation
      that names the failing element
- [x] **Tagged v0.3.0** (2026-10-22), with the version string at
      0.3.0, once the first vehicle session succeeded: an ECU answered
      the Mode-01 requests and the replies decoded to plausible values
      (the [vehicle run sheet](docs/bench-sessions/vehicle-runsheet.md)'s
      first two goals; [session sheet](docs/bench-sessions/2026-10-22-vehicle.md))

## After v0.3.0

Scheduled for v0.3.x, in this order. The vehicle session's
measurements feed the first item.

1. **Request engine** ([ADR-0015](docs/adr/0015-request-engine.md)):
   built, HARNESS-tested on both backends, and run on a car
   (2026-10-25): about 26 Service 01 requests a second, replies 19 ms
   after their request (median), all but one in 7,196 in before the
   next request, so the 20 ms floor stands.
2. **The `obd2` PID-support probe** (ADR-0015 §4, and its appendix
   of 2026-10-23 on the probe as built): built, HARNESS-tested, and
   run on a car (2026-10-25): it walked six bitmaps, found 53
   supported PIDs and dropped only `01 5C`, and a connect made with
   the ignition off dropped nothing and walked once the ECU woke.
3. **`uds-generic`** ([ADR-0020](docs/adr/0020-uds-generic.md)):
   built, HARNESS-tested. `?profile=uds-generic&did=…` names the DIDs
   (`[<ecu>:]<DID>[-<DID>][:<type>]`, several in a list); single DIDs
   are polled, a range is scanned once beside the rotation and the
   DIDs the ECU answers join it; the mock answers them too, so the app
   shows the profile with no hardware. Settings composes the URI from a
   preset or a device, a profile row (default, `raw`, `uds-generic`)
   and a DID field, so none of it needs typing; the choices last while
   the app runs (persisted preferences, below, will keep them). On a
   car (2026-10-25) the scan of `F180-F1FF` settled all 128 DIDs in
   14.3 s, 2 found and 126 refused with `7F 22 31`; this ECU answers
   Service 0x22 in about 80 ms.
4. **Vehicle identification** ([ADR-0021](docs/adr/0021-vehicle-identification.md)):
   built, HARNESS-tested. Every requesting channel asks its ECU the
   VIN, calibration IDs and identification DIDs once at connect,
   taking turns with the rotation, and the activity log (so a
   recording's `events.log`) gets one line per ECU. `?identify=0`
   turns it off. On a car (2026-10-25) every item settled: the four
   Service 09 items and two DIDs answered, seven DIDs refused, and
   the replies longer than one CAN frame (up to 27 bytes) arrived
   whole.
   Items 1–4 ship in **v0.3.1**, the first release with a Windows
   download ([release notes](docs/releases/v0.3.1.md)): a zip built
   and checked by `.github/workflows/release.yml` that runs without
   Qt or Visual Studio, with a signed build provenance
   ([development.md, "Releases"](docs/development.md#releases)).
5. **Phase 4, record-replay transport** ([ADR-0023](docs/adr/0023-replay-transport.md)):
   built and UNIT-tested, against a real-bus fixture too. `replay://<recording>` plays a
   recording's `frames.csv` ([ADR-0017](docs/adr/0017-session-recording.md))
   back through the frame API on every platform, paced and stamped
   by the recording's session time (`?speed=` 0.1 to 100 or `max`,
   `?loop=1`). A recorded mock session round-trips through the
   recorder and replay value for value, and the second vehicle
   session's engine start, scrubbed of its identification replies,
   is a committed fixture whose replay decodes the sheet's values.
   The bridge accepts frames from the moment a session starts, so a
   replay's first frame isn't lost to the connect.

## BENCH prep

These are assumptions in the code that only a real adapter can
confirm. Each one is a thing to check on the first session with the
OBDX Pro VX, whose registry entry declares `ISO15765=1`, which makes
it a genuine connect candidate.

1. **Flow-control filter shape.**
   - The loader seam requires the flow-control message length to equal
     the mask length.
   - Mask, pattern and flow-control messages are bare 4-byte big-endian
     CAN IDs: the adapter's ISO-TP stack does the framing, so no PCI
     byte belongs in the flow-control message. The loader seam's
     length-equality rule is satisfied trivially at 4 bytes.
   - This SUPERSEDES the 8-byte shape (`[ID][30 00 00][pad]`) the
     2026-10-16 session accepted.
   - CONFIRMED 2026-10-20 (no vehicle): the VX accepted the 4-byte
     shape through a bare `j2534://` URI (obd2-derived channel),
     `OP_START_FILTER` vendor status 0, filter_id 1
     ([session note](docs/bench-sessions/2026-10-20.md)), and carried
     every reply on the first vehicle (2026-10-22).
2. **Request framing.** The transport writes the LOGICAL message —
   `[4-byte tx ID][01][PID]`, 6 bytes — with ISO15765_FRAME_PAD, so the
   adapter adds the ISO-TP PCI byte and zero-pads the CAN frame. Some
   drivers expect both in-band instead (a 12-byte message); if the VX
   transmits nothing, this is the first suspect. CONFIRMED 2026-10-22:
   the first vehicle's engine ECU answered the 6-byte request
   ([session sheet](docs/bench-sessions/2026-10-22-vehicle.md)).
3. **No checksum byte.** This is ISO 15765-4 over CAN, so the K-line
   checksum convention doesn't apply. CONFIRMED 2026-10-22: the VX
   hands over each reply as the CAN ID and the service data, with no
   PCI byte, padding or checksum.
4. **29-bit addressing** is assumed whenever either ID is above
   `0x7FF`: writes carry `CAN_29BIT_ID`, and the channel connects with
   it. Unexercised: the first vehicle uses 11-bit IDs.
5. **Timestamps** are microseconds, as the spec says, and the
   transport turns them into session time, counting the 32-bit wrap
   with the host's clock ([j2534.md](docs/j2534.md#session-lifecycle),
   "Frame time"). CONFIRMED for the VX: on the vehicle, 5,539 stamps
   over 932 s fit the host clock with a slope of 0.9999993, as its
   indications did at the bench. The clock doesn't reset at open or
   connect, wraps 71.6 min after zero, and what sets its zero is
   unknown ([session sheet](docs/bench-sessions/2026-10-22-vehicle.md)).
   The wrap came mid-connect on the second vehicle session, and
   session time ran straight through it, within 15 ms of the host
   clock ([session sheet](docs/bench-sessions/2026-10-25-vehicle.md)).
6. **ReadMsgs statuses.** Split in both backends:
   `ERR_TIMEOUT` is a vehicle timeout, `ERR_BUFFER_EMPTY` is "no
   frame", and anything else is a hard failure. Frames that
   arrive with `STATUS_NOERROR` or `ERR_TIMEOUT` are delivered, so a
   vendor that pairs `ERR_TIMEOUT` with a partial batch loses nothing.
   Under any other status the count is ignored, because a vendor may
   leave it untouched on error (`6420f09`). Observed on the VX
   (2026-10-22, no vehicle): an empty read returns `ERR_BUFFER_EMPTY`
   and a short batch `STATUS_NOERROR`, both after the read's full
   timeout. Its TX indications arrive in those batches and are dropped
   before counting
   ([session note](docs/bench-sessions/2026-10-22.md)). On the
   vehicle every reply came with `STATUS_NOERROR`, and no read
   returned `ERR_TIMEOUT`
   ([session sheet](docs/bench-sessions/2026-10-22-vehicle.md)).
7. **Struct-layout conformance to v04.04**
   ([ADR-0010](docs/adr/0010-j2534-v0404-shared-types.md)). CONFIRMED
   2026-10-22 for the VX: replies decode correctly on a vehicle.
8. **Vendor ioctl and filter behavior** and timeout semantics.
   Observed on the VX (2026-10-22, no vehicle): reads run their full
   timeout unless the batch fills. Writes sent with timeout 0 are
   accepted three deep with nothing on the bus to acknowledge them,
   each with a `TX_INDICATION`; the fourth and every later write
   blocks about 0.5 s and returns `ERR_FAILED`, until a new connect.
   Filter start and stop and disconnect return status 0 at once
   ([session note](docs/bench-sessions/2026-10-22.md)). On the vehicle
   every write returned status 0, with the ignition off too. The one
   ioctl the transport issues, `READ_VBATT` at connect, is item 10.
9. **Export-name conformance.** RESOLVED 2026-10-16, contrary to
   expectations: the OBDX Pro VX exports the SPEC spelling
   (`PassThruIoctl`, J2534-1_0500 §7.3.19; all v04.04-era references
   agree) — this project's `PassThruIoCtl` string was the divergence.
   Both backends try the spec spelling first, with the legacy spelling
   as fallback, pinned in both directions by the two fakes.
10. **The session info's readings**
    ([ADR-0019](docs/adr/0019-session-info.md)). Each connect asks the
    device for `READ_VBATT` and `PassThruReadVersion`, and the
    activity log's "Session:" line shows the answers. CONFIRMED
    2026-10-25 for the VX: it answers both; the voltage is in
    millivolts, 0.0 V on USB power alone and within 0.1 V of the ECU's
    own reading (PID 0x42) on the car; the versions read
    `FW:1.0.1.0 HW:1.0.0.0`, DLL `1.8.9586.12845`, API `04.04`
    ([session sheet](docs/bench-sessions/2026-10-25-vehicle.md)).

## Known issues and technical debt

These numbers are stable IDs, and code and docs refer to them. When
an issue is resolved it is removed, and its number is not reused.

2. **The mobile apps are unvalidated.** Since 2026-10-20, CI
   cross-builds an Android arm64-v8a APK and an unsigned iOS arm64
   device build. On pull requests and on request, it also launches
   the iOS app on the Simulator and screenshots it
   (`ios-simulator-smoke`). Neither app has run on a device, the
   Android app has never been launched, and no tests run on either
   (cross builds set `BUILD_TESTING=OFF`). Off Windows the
   transports are `mock://` and `replay://`, so a mobile session
   can't reach a vehicle yet.
4. **Every def is matched against every frame on the GUI thread.** A
   subscription can't trim that work: it is the chart's filter only,
   because the Dashboard reads every signal. Measured, it costs about
   3 ns a def a frame: 1000 defs at a full 500 kbit/s classic CAN bus
   would take under 2 % of the GUI thread, and about 9 % at a CAN FD
   rate of 25,000 frames a second (worked out, not measured;
   [docs/testing.md](docs/testing.md#performance)). Decoding on the
   worker (Performance, in the backlog) removes it along with #10.
6. **The session API is single-caller by contract.** The bridge is the
   only caller.
10. **Strip-chart x-axis resolution is one coalesce window, about
    100 ms,** and a signal keeps only its latest value per window, so a
    short peak can miss the screen: the fixture's engine start replayed
    at 10× showed an RPM peak of 1,195 on the Dashboard against the
    recording's 1,261 (docs/testing.md). Per-sample timestamps need
    decoding to move onto the
    worker.
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
| v0.2.0 | Callback API, mock transport, live frame log, CI | ✅ Released (tagged) |
| v0.3.0 | Signal decoding, LiveView, J2534 transport, live vehicle read and logging | ✅ Released (tagged) |
| v0.3.x | Request engine, PID-support probe, `uds-generic`, vehicle identification, record-replay transport | Under way: [after v0.3.0](#after-v030) |
| v0.4.0 | Security access discovery, flash capability | Planned |
| v0.5.0+ | Manufacturer plugin modules (ADR-0013's profile API) | Planned |

Flash support depends on discovering each manufacturer's
secure-gateway authentication. That research is tracked outside this
repo for now.
Legacy-bus support (ISO 9141-2/KWP, J1850 VPW/PWM) is a missing
workstream between the profile work and the manufacturer-plugin
milestone: the expanded target list (pre-2008 Mazda/Ford/GM) needs
transport-layer protocol support before any profile can serve those
cars. Whether and when it is built depends on which target vehicles
matter most — CAN-era targets need none of it.

## Backlog

Unscheduled work, grouped by theme. Nothing here is committed to a
release: an item moves into [After v0.3.0](#after-v030) or the
roadmap above when it is scheduled. Defects and debt live under
[Known issues](#known-issues-and-technical-debt), not here.

Sizes are rough: **S** is a day or less, **M** a few days, **L** a
week or more or a design that needs an ADR first.

### Diagnostics and error reporting

These make a failed field session explainable, so they come first
in the backlog.

- **Adapter versions in the interface list (S).** The activity log's
  "Session:" line names a connected adapter's firmware, DLL and API
  versions ([ADR-0019](docs/adr/0019-session-info.md)). The interface
  list can't ask for them without opening each adapter, which the VX
  and VT refuse while unattached; showing the versions last seen for
  each one would need persisted preferences.

### Data capture

- **Recording, what's left (S).** ADR-0017's recorder writes a whole
  session per connect, and its `events.log` names the vehicle through
  the identity line ([ADR-0021](docs/adr/0021-vehicle-identification.md)).
  Still to do: name the folder by vehicle as well as date (the folder
  opens before the VIN arrives, so this waits for persisted
  preferences), keep the Record sessions switch across restarts (with
  the same preferences), and copy selected Log rows for quick notes.
- **Triggered logging (M).** Record only while a condition holds
  (RPM over a threshold) or capture a window around a link fault.
  Needs a trigger model in core, similar in shape to the probe: data
  describing a condition, not code on the worker thread.

### User interface

- **Persisted preferences (S).** The UI zoom and the unit system
  (imperial until changed) survive a restart, through QML `Settings` in `Main.qml` (category
  `ui`). Developer mode, theme, the last-used device, the Record
  sessions switch and Settings' profile choice and DID list need the
  same. It belongs in the app layer, not core.
- **Developer mode (S).** A persisted Settings toggle, off by default.
  With it off the app offers only real devices. With it on, the app
  also shows:
  - the mock transport (the `mock://demo` preset in Settings);
  - the raw frame log;
  - the Log page's notify and batch counters.
- **Log page as the application log (S).** Show the application's
  event log on the Log page (it sits at the bottom of Settings) and
  move the raw frame table into developer mode. Pairs with
  developer mode.
- **Units of measure, per-signal overrides (S).** Metric or imperial
  for the whole UI is built ([ADR-0022](docs/adr/0022-display-units.md):
  converted in core by unit string, chosen on Settings, kept between
  runs). Still to do: one quantity in the other system (degrees F with
  km/h), which passes a unit system per row instead of one for the
  model.
- **Low-glare and night themes (M).** Amber and red-on-black schemes
  for driving at night.
  - `Theme` already centralizes the palette, but the analog gauge
    hard-codes its face, bezel and needle colors, and Home its logo
    gradient; they need Theme roles first.
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
- **Vehicle profiles (M).** Remember per vehicle, keyed by VIN or a
  label the user names: profile, channel overrides, dashboard
  arrangement, last device. Nobody reconfigures the tool each time
  they plug into the same car. The VIN key exists
  ([ADR-0021](docs/adr/0021-vehicle-identification.md)); the rest
  needs persisted preferences.
- **User-configurable dashboards (L).** User-authored pages choosing
  from the gauge library. The gauges are already modular, but layout
  persistence is new ground and needs the preferences item first.

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

- **ELM327 transport (L).** ELM327-compatible adapters over USB serial,
  Bluetooth and BLE (the OBDLink MX+ and CX among them). This is the
  path to a vehicle on macOS, Linux, iOS and Android, where only
  `mock://` and `replay://` exist (Known Issue #2).
  - It's a new transport, not a J2534 shim: ELM327 is a text
    AT-command protocol with its own framing and flow control.
  - Settle in an ADR first: core can't link Qt, so Bluetooth needs
    either per-platform native code in core (CoreBluetooth, WinRT,
    BlueZ, the Android SDK) or a byte-stream transport that the app
    layer feeds.
- **Legacy buses:** see the roadmap above.

### Performance

- **Full signal sets at full rate (L).** The goal is a per-PID refresh
  limited by the ECU's response time, with no per-frame GUI-thread
  cost. Requests already follow the replies
  ([ADR-0015](docs/adr/0015-request-engine.md)), and the frame log is
  a fixed ring of plain rows, about 2 µs a frame at its cap. The part
  left is decoding on the worker thread, which also brings per-sample
  timestamps (Known Issue #10) and takes the per-frame def matching
  off the GUI thread (#4).

  The GUI thread's cost per frame by profile size is measured
  (`bench_bridge`, [docs/testing.md](docs/testing.md#performance)): the
  matching takes under 2 % of the GUI thread at a full classic CAN bus
  with 1000 defs. The reasons to move the decode are #10's per-sample
  time on the chart first, then a decode every front end shares and
  values that survive frames dropped under load; not CPU. It gets its
  own ADR when it's built, citing those numbers. The replay transport
  is its load test: replay faster than real time drives this path with
  recorded traffic whose frames carry their own times, and shows
  whether the worker's per-frame copy or the per-row model update costs
  more than the matching first.

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