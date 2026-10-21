# QtTune — Project Status

**Updated:** 2026-10-21 · **Version:** v0.3.0-dev (last tag v0.2.0) ·
[![CI](https://github.com/erickramer42/qttune/actions/workflows/ci.yml/badge.svg)](https://github.com/erickramer42/qttune/actions/workflows/ci.yml)

This file covers the project as it stands: what works, what comes
next, and what is known to be wrong. For how the parts fit together,
start with [docs/architecture.md](docs/architecture.md). Why it is
built this way is in [docs/adr/](docs/adr/README.md). How it got here, with lessons
learned, is in [docs/engineering-log.md](docs/engineering-log.md).
Safety, regulatory and warranty caveats are in
[DISCLAIMER.md](DISCLAIMER.md).

## Where things stand

**Current phase: v0.3.0 Phase 3 (J2534).** The HARNESS tier is complete,
including transport adoption (ADR-0012). The BENCH tier has covered a real
adapter on a quiet bus (no vehicle attached). The first real vehicle comes
next.

- **Works:**
  - The mock transport drives the live frame log, LiveView and the
    strip chart with a scripted drive cycle: 2nd- and 3rd-gear pulls
    to the rev limiter, then a quarter-mile run.
  - J2534 runs end to end against the fake vendor DLL, both in-proc
    and through the x86 surrogate. That includes write/ioctl/filter
    relay on both backends and transport adoption: filters, the Mode-01
    request loop, and frame identity via `?filter=`/`?txid=`/`?poll=`
    (ADR-0012).
  - The surrogate child is single-threaded and reads travel over the
    pipe ([ADR-0014](docs/adr/0014-single-threaded-surrogate.md),
    2026-10-19): vendor calls are serialized by construction, the
    shared-memory ring is gone, and the vendor's read status now
    reaches the transport on both backends (timeout, empty and hard
    failure are told apart; see #11 for what remains).
  - Profile selection (ADR-0013, slices 1–3, 2026-10-17 → 2026-10-19):
    a string-keyed registry selects the session's signal set —
    `mock://` defaults to the mock set, `j2534://` defaults to
    `obd2`, and `?profile=<name>` overrides either. Unknown names are
    rejected at session_create, before any transport attach or DLL
    load. The bridge decodes the session's active set instead of the
    hard-wired mock set (#21 closed). The `obd2` set carries the
    multi-PID round-robin scheduler and declares its CAN channel
    (7E0/7E8), so the transport derives the receive filter and request
    target from the profile: a bare `j2534://` URI starts and streams
    with no `?filter=` required (`?filter=`/`?txid=` remain overrides,
    and since `90020bd` `?poll=` swaps the rotation for one PID under
    any profile instead of being ignored).
  - Discovery finds the real installed drivers, and the Settings page
    can pick one and connect.
  - Link health (#11): the J2534 worker tells a quiet bus, a dead
    adapter and failing requests apart, the bridge logs transitions to
    the Settings activity log, and LiveView, the Home status card and
    the Settings connection label all show the state. The
    ok/quiet/fault/disconnected states were observed 2026-10-20,
    including a fault on the VX itself (quiet bus, no vehicle). That
    VX fault was under the rules before `946bdf5`; the same no-ACK
    setup now latches `QT_ERR_REQUEST_FAILED`, which the UI shows as
    "Requests failing" in amber.
  - Dashboard: analog, ring, digital and bar gauges plus shift lights,
    fed by name through `SignalHub` so one layout serves both the mock
    and `obd2` profiles. Gauges show peak hold, dim stale values, and
    say "not in profile" instead of showing zero (PR #12).
  - Surrogate diagnostics: client and child write one session log,
    both stamped `HH:MM:SS.mmm`, so a GUI launch keeps the whole story
    without a stderr redirect; a second concurrent session gets its own
    log (`995519b`, PR #11).
- **Not yet:**
  - BENCH coverage beyond the quiet-bus tier. The formal first BENCH
    session (2026-10-16, [docs/bench-sessions/2026-10-16.md](docs/bench-sessions/2026-10-16.md))
    succeeded: the OBDX Pro VX loaded through the surrogate, opened with
    USB power (vendor status 0), accepted the 8-byte flow-control filter
    shape (behaviorally, since the child didn't yet log filter ops),
    and survived a 2-minute quiet-bus
    soak at <1% CPU with clean teardown. The 4-byte shape that replaced
    it was accepted with logged vendor status 0 on 2026-10-20
    ([bench-sessions/2026-10-20.md](docs/bench-sessions/2026-10-20.md)).
    Talking-bus behaviors, VBATT, and timestamp units remain untested.
    The OBDX Pro VT still refuses open (status 34) with its adapter
    absent. The run sheet template is
    [docs/bench-sessions/bench-runsheet.md](docs/bench-sessions/bench-runsheet.md).
  - Any live vehicle. The session's run sheet is
    [docs/bench-sessions/vehicle-runsheet.md](docs/bench-sessions/vehicle-runsheet.md).
  - J2534 decode on real hardware: the `obd2` set, scheduler and
    channel derivation are HARNESS-verified against the fake; a
    live-vehicle session (real ECU responses, talking-bus items in
    the BENCH prep list) is still ahead.
  - **Tests:** 158 ctest cases on `668ba25` plus the MAF and TFT
    additions to the `obd2` set (Windows/MSVC 2022, both trees rebuilt,
    2026-10-21), plus the cross-arch subset (19/19). All passed except
    the env-gated cross-arch skip. CI was green on all eight jobs
    on PR #11 (merged as `a96892b`), including the simulator smoke. That covers
    ctest on Windows, macOS and Linux, Core under ASan and UBSan, the
    Android and iOS device cross-builds, and the simulator smoke. The
    smoke's first run (`923f479`) gave a blank frame. Its second
    (`4be8ba5`) drew and showed two layout bugs, and `a0f5c0f`
    confirmed both fixes. The smoke runs on pull requests and on
    request, not on every push, because it roughly doubles CI time. The cross-arch routing case
    skips unless its environment variables are set. J2534 suites
    carry a 30 s per-test timeout — a hang fails bounded instead of
    wedging the runner. Tiers, suites and
    gaps are in [docs/testing.md](docs/testing.md).

## Architecture

```
+--------------------------------+
| Qt Quick UI (QML)              |  Home · Dash · Live · Log · Settings
|                                |  sidebar at >= 720 px, TabBar below
+---------------+----------------+
                | QtTuneBridge: worker -> GUI marshaling, 100 ms coalescing
+---------------v----------------+
| qttune-core (C++17, C ABI)     |  no Qt (CI-enforced)
|  sessions + frame callbacks    |
|  signal decode                 |
|  transports: mock | j2534      |  selected by URI
|  j2534 loader: in-proc | x86   |  routed by the DLL's PE machine type
+---------------+----------------+
                | named pipe (control + batched reads, ADR-0014)
                | + kill-on-close job object
+---------------v----------------+
| qttune-j2534-surrogate (x86)   |  hosts 32-bit vendor DLLs
+--------------------------------+
```

Three rules ([ADR-0001](docs/adr/0001-qt-free-core-c-abi.md)):

1. **Core never links Qt.**
2. **UI layers receive final display values, never raw frames.**
3. **The C ABI is the FFI boundary.** That means opaque handles, flat
   functions, POD structs and append-only enums.

## Data path

```
mock worker (50 ms tick)  -or-  J2534 worker (ReadMsgs poll; in-proc,
      |                         or batched over the surrogate's pipe)
      | qttune_frame_t
      v
core: snapshot dispatch to registered callbacks ............ ADR-0002
      | QtTuneBridge::onFrame (worker thread): copy, in-flight gate, post
      v
GUI thread: handleFrameInternal ............................ ADR-0003
      |- FrameListModel (10k ring) -> FrameSortProxy -> LogPage
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
| Core sessions and callbacks | Stable. The frame ABI and close/destroy split changed in v0.3.0-dev. | [ADR-0002](docs/adr/0002-callback-pair-identity-snapshot-dispatch.md), [ADR-0006](docs/adr/0006-frame-abi-v0.3.md) |
| Signal decode | Linear scaling; Intel byte order by default, big-endian for byte-aligned fields; gated on message ID plus mux key. Two signal sets, `mock` and `obd2`, chosen by profile. | [ADR-0004](docs/adr/0004-signal-decoding-model.md), [ADR-0013](docs/adr/0013-protocol-profiles.md) |
| Mock transport | A scripted ~2-minute drive cycle on a small vehicle model (`core/src/mock_drive_cycle.h`): cold-start warm-up, a 2nd-gear WOT pull from ~1550 rpm to the 7000 rpm limiter with a 5 s hold, a longer and hotter 3rd-gear pull with the same hold, then a quarter-mile run from a standstill (~13.4 s, ~176 km/h trap). Every tick of the cycle round-trips through the mock set in `CoreTest.MockDriveCycle*`. | — |
| Bridge and models | Frame log with sort toggle; coalesced signal model with a 120-sample history per signal; device discovery for the picker; link-health polling (#11) | [ADR-0003](docs/adr/0003-backpressure-bounds-in-flight-frames.md), [ADR-0005](docs/adr/0005-coalesced-signal-updates.md) |
| QML pages | Home, Dashboard (gauges and shift lights through `SignalHub`), Live (table and strip chart), Log, Settings (device picker plus manual URI). `Theme` holds the palette and the link-state color and label functions. | [docs/architecture.md](docs/architecture.md#the-ui-layer) |
| J2534 | HARNESS-verified, same-arch and cross-arch, including transport adoption (ADR-0012) and profile-derived channels (ADR-0013). BENCH: one adapter, quiet bus. | [docs/j2534.md](docs/j2534.md) |

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
- [ ] **Phase 3 close, BENCH tier:**
  - [ ] first real vendor DLL load and connect: the OBDX Pro VX through
    the surrogate — done, formal session 2026-10-16
    ([session sheet](docs/bench-sessions/2026-10-16.md)); the OBDLink
    MX+ in-proc if its DLL is x64 — pending
  - [x] **Resolved surrogate serialization bug (#24):** serializes vendor DLL calls, clears `g_channel` on OP_DISCONNECT, prevents double disconnect and unsafe teardown; BENCH gate cleared.
  - [x] **Single-threaded surrogate, reads over the pipe**
        ([ADR-0014](docs/adr/0014-single-threaded-surrogate.md),
        2026-10-19). Read batches that arrive with `ERR_TIMEOUT` are
        delivered (formerly #26, fixed the same day).
  - work through [BENCH prep](#bench-prep) — quiet-bus subset walked
    2026-10-16 (#9 resolved) and 2026-10-20 (#6 observed for empty
    reads); #1 resolved for both shapes (8-byte behaviorally
    2026-10-16, 4-byte with logged vendor status 0 2026-10-20); the
    talking-bus items (#2, #3, #4, #5, #7) still open
  - first live, read-only vehicle session
- [ ] **Protocol profiles** (ADR-0013):
  - [x] slice 1: profile registry, `raw` profile, `?profile=`
        override, bridge reads the session's active signal set
        instead of the hard-wired mock set (#21 closed 2026-10-17)
  - [x] slice 2: the `obd2` signal set and the multi-PID scheduler
        replacing the single-poll loop (paired with #25, the
        read-rate cap — one workstream per ADR-0013)
  - [x] slice 3: channel declarations (§4) — profiles declare rx/tx
        CAN channel pairs; the transport registers one flow-control
        filter per channel and routes scheduler requests per PID; a
        bare `j2534://` URI under `obd2` starts with no `?filter=`
  - [ ] `obd2` PID probe phase and `uds-generic`
- [ ] **Phase 4, record-replay transport:** capture fixtures, then play
      them back deterministically through the same frame API
- [ ] **Tag v0.3.0** and bump the version string (#16)

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
     ([session note](docs/bench-sessions/2026-10-20.md)).
2. **Request framing.** The transport writes the LOGICAL message —
   `[4-byte tx ID][01][PID]`, 6 bytes — with ISO15765_FRAME_PAD, so the
   adapter adds the ISO-TP PCI byte and zero-pads the CAN frame. The
   shape used before 2026-10-18 supplied both in-band (12 bytes). Some drivers expect
   the in-band form; if the VX transmits nothing, this is the first
   suspect.
3. **No checksum byte.** This is ISO 15765-4 over CAN, so the K-line
   checksum convention doesn't apply.
4. **29-bit addressing** is assumed whenever either ID is above
   `0x7FF`.
5. **Timestamps** are passed through as microseconds, as the spec
   says (#12).
6. **ReadMsgs statuses.** Split in both backends since 2026-10-19:
   `ERR_TIMEOUT` is a vehicle timeout, `ERR_BUFFER_EMPTY` is "no
   frame", and anything else is a hard failure (#11). Frames that
   arrive with `STATUS_NOERROR` or `ERR_TIMEOUT` are delivered, so a
   vendor that pairs `ERR_TIMEOUT` with a partial batch loses nothing.
   Under any other status the count is ignored, because a vendor may
   leave it untouched on error (`6420f09`). Observed 2026-10-20
   (no vehicle): the VX returns `STATUS_NOERROR` with zero messages
   for an empty read — a third flavor, neither of the two the split
   names; it behaves as "no frame", and the session's fault state came
   from the writes, not the reads
   ([session note](docs/bench-sessions/2026-10-20.md)). Still to
   observe on a talking bus: whether it pairs `ERR_TIMEOUT` with data
   at all.
7. **Struct-layout conformance to v04.04** cannot be confirmed without
   a device ([ADR-0010](docs/adr/0010-j2534-v0404-shared-types.md)).
8. **Vendor ioctl and filter behavior** and timeout semantics.
9. **Export-name conformance.** RESOLVED 2026-10-16, contrary to
   expectations: the OBDX Pro VX exports the SPEC spelling
   (`PassThruIoctl`, J2534-1_0500 §7.3.19; all v04.04-era references
   agree) — this project's `PassThruIoCtl` string was the divergence.
   Both backends now try spec-first with the legacy spelling as
   fallback, pinned in both directions by the two fakes.

## Known issues and technical debt

These numbers are stable IDs, and code and docs refer to them. When
an issue is resolved it is removed, and its number is not reused.

2. **The mobile apps are unvalidated.** Since 2026-10-20, CI
   cross-builds an Android arm64-v8a APK and an unsigned iOS arm64
   device build. On pull requests and on request, it also launches
   the iOS app on the Simulator and screenshots it
   (`ios-simulator-smoke`). Neither app has run on a device, the
   Android app has never been launched, and no tests run on either
   (cross builds set `BUILD_TESTING=OFF`). Off Windows the only
   transport is `mock://`, so a mobile session can't reach a vehicle
   yet.
4. **Decoding ignores subscription state.** Every signal is decoded
   for every frame and filtered at the model. Filter at decode time
   once signal counts grow.
6. **The session API is single-caller by contract.** The bridge is the
   only caller.
8. **`CoreTest.SessionStartWithoutTransportFails` is a `SUCCEED()`
   stub.** Implement it or delete it.
10. **Strip-chart x-axis resolution is one coalesce window, about
    100 ms.** Per-sample timestamps need decoding to move onto the
    worker.
11. **J2534 link health: real-bus coverage remains.**
    The worker's health machine is done and HARNESS-tested in-proc:
    hard read failures, a quiet debounce, a write latch with
    `QT_ERR_REQUEST_FAILED`, unmatched frames read as quiet, and a
    growing backoff ([j2534.md](docs/j2534.md#session-lifecycle)). The
    bridge publishes it as `linkState` (`ok`, `quiet`, `requests`,
    `fault`, `disconnected`) and logs transitions, and LiveView, Home
    and Settings show it; stale values dim from the model's `ageMs`.
    A request failure (no ECU awake, adapter fine) reads "Requests
    failing — check the vehicle connection and ignition" in amber;
    `BridgeTest.LinkStateMapsEveryPublishedCode` pins the mapping. The
    smoke recipe passed on 2026-10-20; its request-failure row hasn't
    been run yet
    ([testing.md](docs/testing.md#manual-smoke--link-health-ui)).
    Still open:
    - No surrogate session drives the health machine in the suite.
      Accepted until the first vehicle session, which runs through the
      surrogate and records it in the session log.
    - If reads recover while writes keep failing, the state stays
      "adapter fault" instead of stepping down to request failure.
      Accepted for now: it needs a transient read error on a no-ACK
      bus, and the session log tells the two apart (clean `OP_READ`
      lines next to failing `OP_WRITE` lines).
    - Real-vendor coverage is one quiet-bus observation (2026-10-20:
      empty reads are `NOERROR` with zero messages; writes fail after
      ~8 cycles with no ACK); talking-bus flavors remain unknown
      (prep item 6).
12. **J2534 timestamps pass through untranslated.** The spec says
    microseconds, but vendor conformance is unverified.
13. **The picker treats one registry entry as one device.** Multiple
    adapters behind one vendor DLL can't be told apart. They would be
    selected through `PassThruOpen`'s `pName` argument, which is
    currently `nullptr`, meaning the default device.
14. **Discovery tests only check shape,** so they pass vacuously on a
    machine with no vendors installed. Candidate fixes are a
    seeded-registry fixture behind an admin-elevation guard, or
    count-positive assertions on the known dev machine.
16. **The version string still says 0.2.0** even though the frame ABI
    has changed ([ADR-0006](docs/adr/0006-frame-abi-v0.3.md)). It gets
    bumped at the v0.3.0 tag. Bumping mid-cycle only causes rebuild
    churn.
19. **Shared-header constants are ABI across two build trees.**
    - `j2534_ptypes.h` and `surrogate_protocol.h` are compiled into
      both the x64 tree and `build-win32`.
    - A struct-size mismatch fails loudly at the pipe. A changed
      constant value inside a same-sized struct is silent.
    - One possible mitigation is a version or magic hash in the
      `OP_OPEN` handshake. Until then, rebuild both trees together.
20. **Pointer-bearing ioctls (`FAST_INIT`, `FIVE_BAUD_INIT`) are out of
    contract** and refused at both ends. That is fine for ISO 15765,
    since those are ISO 9141/KWP init calls. If a BENCH device needs
    them, give them a dedicated wire form rather than relaxing the
    whitelist.
28. **In-proc, a dropped indication ends the drain early.** The loader
    drops `TX_INDICATION`/`TX_MSG_TYPE` messages by reporting "no
    frame", and the drain stops at the first "no frame". After each
    write the vendor's TX-done indication therefore ends the drain,
    and the ECU's reply is read on the next cycle. The surrogate drops
    indications inside the child, so its drain continues: the two
    backends now drain differently. `START_OF_MESSAGE` (first-frame)
    indications are not handled by either backend.
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
| v0.3.0 | Signal decoding, LiveView, J2534 transport, live vehicle read and logging | 🚧 [Checklist above](#v030-checklist) |
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
release: an item moves into the [v0.3.0 checklist](#v030-checklist) or
the roadmap above when it is scheduled. Defects and debt live under
[Known issues](#known-issues-and-technical-debt), not here.

Sizes are rough: **S** is a day or less, **M** a few days, **L** a
week or more or a design that needs an ADR first.

### Diagnostics and error reporting

These make a failed field session explainable, so they come first
after the first vehicle session.

- **Say why a connect failed (M).** `qttune_session_create` folds
  distinct causes into two codes. A refused `PassThruOpen` (a vendor
  status) and a DLL that won't load both read "failed to load
  transport library" (`QT_ERR_LOAD_FAILED`); a malformed URI and an
  unknown profile both read "no transport available"
  (`QT_ERR_NO_TRANSPORT`). Add status codes (the enum is append-only)
  or a last-error string on the session API, and show the specific
  cause and vendor status in the Settings error line.
- **Vendor error text (S).** After a failed vendor call, fetch
  `PassThruGetLastError` and put its text in the session log and the
  error line; the OBDX Pro VT's status 34 has no label without it.
  Needs a loader helper and a matching surrogate op, tested on both
  backends.
- **Session details in the activity log (S).** Connects,
  disconnects, failed connects and link-health transitions are
  logged. Add what a support question needs:
  - the backend (in-proc or surrogate) and the DLL's architecture;
  - protocol and baud;
  - the profile and its derived channel IDs;
  - the adapter's battery voltage (`READ_VBATT`, already relayed on
    both backends but never requested);
  - its firmware and DLL versions (`PassThruReadVersion`, not yet
    wrapped).

### Data capture

- **Record a session to disk (M).** Two outputs from one recording:
  - decoded signals as CSV (time, signal, value, unit) for
    spreadsheets and plotting;
  - raw frames in the format the Phase 4 replay transport reads, so
    a capture from the road doubles as a regression fixture.

  Start and stop from the UI, one file per session, named by date and
  vehicle. Define the raw format together with Phase 4, so there is
  only one. Most useful once live vehicle data reaches LiveView.

### User interface

- **Persisted preferences (S).** Nothing survives a restart: there is
  no `QSettings` or QML `Settings` use yet. Developer mode, units,
  theme and the last-used device all need it, so it comes before
  them. It belongs in the app layer, not core.
- **Developer mode (S).** A persisted Settings toggle, off by default.
  With it off the app offers only real devices. With it on, the app
  also shows:
  - the mock transport (the header's "Connect Mock", the `mock://demo`
    preset, and Home's "Start session", which connects mock);
  - the raw frame log;
  - the header's frame, notify, batch and drop counters.
- **Log page as the application log (S).** Show the application's
  event log on the Log page (it sits at the bottom of Settings) and
  move the raw frame table into developer mode. Pairs with
  developer mode.
- **Units of measure (M).** Metric or imperial globally, with
  per-signal overrides (°C or °F, km/h or mph, kPa or psi, g/s or
  lb/min). The conversion belongs in core, because the UI receives
  final display values (rule 2 above). Display names and decimal
  places are core's to own for the same reason: `degC` and raw def
  names such as `Engine_RPM` reach the screen.
- **Low-glare and night themes (M).** Amber and red-on-black schemes
  for driving at night.
  - `Theme` already centralizes the palette, but the gauges hard-code
    their face, bezel and needle colours, which need Theme roles
    first.
  - A near-monochrome scheme must keep warning and danger
    distinguishable by more than hue, for example with a label or
    icon.
- **Picker presets with profiles (S).** Channel derivation already
  makes the plain flow produce a working `obd2` session. What's left
  is picker rows for `?profile=raw` (log only) and explicit ID
  overrides for vehicles that don't use the standard 7E0/7E8 pair.
- **In-vehicle displays: Android Auto and CarPlay (L, research
  first).** A second front end over the same C ABI, which is what
  replaceable UI layers are for. Both platforms admit third-party apps
  only in approved categories with template-based screens, and Qt
  targets neither, so the front end would be native (Kotlin, Swift).
  Confirm that a live-gauge app qualifies before building anything.
  Out of scope: modifying or replacing a vehicle's own infotainment
  system.

### Vehicle coverage

- **Full standard OBD-II coverage (L).** Grow the `obd2` set toward
  the whole SAE J1979 Service 01 table, including PIDs that carry two
  values (oxygen sensors with fuel trims), which need two defs on one
  PID.
  - Depends on the PID-support probe (v0.3.0 checklist): until the
    ECU's support bitmaps prune the rotation, every PID in a set costs
    a 100 ms request slot whether the ECU answers or not.
  - Depends on faster request pacing (Performance, below).
  - Verify each PID's meaning and formula against a public J1979
    table. The fake's canned replies share the def's assumptions, so
    the tests can't catch a wrong PID.
- **Vehicle identification (M).** Read the VIN and calibration IDs
  (Service 09) at connect, for the activity log and session
  recordings. These responses are multi-frame and textual: the adapter
  reassembles ISO-TP, but the signal model decodes numbers only
  (`STRING` is rejected), so this needs a string path.
- **A first manufacturer signal set (L).** One make's enhanced
  Service 22 data, such as transmission temperature, which the
  standard set doesn't include. Choose the make from the target
  vehicles. Needs Service 22 requests (the scheduler builds Service 01
  only) and the profile API from ADR-0013, and serves as the pilot
  for the v0.5.0 plugin modules.

### Transports and platforms

- **ELM327 transport (L).** ELM327-compatible adapters over USB serial,
  Bluetooth and BLE (the OBDLink MX+ and CX among them). This is the
  path to a vehicle on macOS, Linux, iOS and Android, where only
  `mock://` exists (Known Issue #2).
  - It's a new transport, not a J2534 shim: ELM327 is a text
    AT-command protocol with its own framing and flow control.
  - Settle in an ADR first: core can't link Qt, so Bluetooth needs
    either per-platform native code in core (CoreBluetooth, WinRT,
    BlueZ, the Android SDK) or a byte-stream transport that the app
    layer feeds.
- **Legacy buses:** see the roadmap above.

### Performance

- **Full signal sets at full rate (L).** The goal is a per-PID refresh
  limited by the ECU's response time rather than by a fixed 100 ms
  slot, with no per-frame GUI-thread cost. The parts:
  - response-driven request pacing: send the next request when the
    reply arrives, bounded by the ISO 15765-4 response timeout (P2,
    50 ms);
  - decoding on the worker thread, which also brings per-sample
    timestamps (Known Issue #10) and decode-time subscription filtering
    (#4);
  - a fixed ring of plain frame structs for the log, replacing
    `QVariantMap` rows, whose eviction costs an O(n) erase per frame
    at the 10k cap.

  Set a measured target before starting: sustained frames per second,
  and per-PID refresh at N PIDs, recorded in
  [docs/testing.md](docs/testing.md).

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