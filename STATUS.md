# QtTune — Project Status

**Updated:** 2026-10-20 · **Version:** v0.3.0-dev (last tag v0.2.0) ·
[![CI](https://github.com/erickramer42/qttune/actions/workflows/ci.yml/badge.svg)](https://github.com/erickramer42/qttune/actions/workflows/ci.yml)

This file covers the project as it stands: what works, what comes
next, and what is known to be wrong. Why it is built this way is in
[docs/adr/](docs/adr/README.md). How it got here, with lessons
learned, is in [docs/engineering-log.md](docs/engineering-log.md).
Safety, regulatory and warranty caveats are in
[DISCLAIMER.md](DISCLAIMER.md).

## Where things stand

**Current phase: v0.3.0 Phase 3 (J2534).** The HARNESS tier is complete,
including transport adoption (Part 5). The BENCH tier has covered a real
adapter on a quiet bus (no vehicle attached). The first real vehicle comes
next.

- **Works:**
  - The mock transport drives the live frame log, LiveView and the
    strip chart.
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
    with no `?filter=` required (`?filter=`/`?txid=` remain overrides).
  - Discovery finds the real installed drivers, and the Settings page
    can pick one and connect.
  - Link health (#11): the J2534 worker tells a quiet bus from a dead
    adapter, the bridge logs transitions to the Settings activity
    log, and LiveView, the Home status card and the Settings
    connection label all show the state. All states observed
    2026-10-20, including the fault state on the VX itself (quiet bus,
    no vehicle).
- **Not yet:**
  - BENCH coverage beyond the quiet-bus tier. The formal first BENCH
    session (2026-10-16, [docs/bench-sessions/2026-10-16.md](docs/bench-sessions/2026-10-16.md))
    succeeded: the OBDX Pro VX loaded through the surrogate, opened with
    USB power (vendor status 0), accepted the 8-byte flow-control filter
    shape (behaviorally —     see #23; the 4-byte shape that replaced it was 
    accepted with logged vendor status 0 on 2026-10-20, 
    [bench-sessions/2026-10-20.md](docs/bench-sessions/2026-10-20.md)), and 
    survived a 2-minute quiet-bus soak at <1% CPU with clean teardown. 
    Talking-bus behaviors, VBATT, and timestamp units remain untested. The OBDX 
    Pro VT still refuses open (status 34) with its adapter absent. The run sheet
    template is [docs/bench-sessions/bench-runsheet.md](docs/bench-sessions/bench-runsheet.md).
  - Any live vehicle.
  - J2534 decode on real hardware: the `obd2` set, scheduler and
    channel derivation are HARNESS-verified against the fake; a
    live-vehicle session (real ECU responses, talking-bus items in
    the BENCH prep list) is still ahead.
  - **Tests:** 139 ctest cases, all passing locally on 2026-10-20
    (Windows/MSVC 2022, both trees rebuilt), plus the cross-arch
    subset (16/16). CI was green on all seven jobs at `923f479`
    (2026-10-20, run #73, link-health changes included). That run
    covers ctest on Windows, macOS and Linux, plus the Android and iOS
    device cross-builds. `ios-simulator-smoke` passed too, but its
    screenshot was blank, so the job is now manual-only (#30). The
    cross-arch routing case
    skips unless its environment variables are set. J2534 suites
    carry a 30 s per-test timeout — a hang fails bounded instead of
    wedging the runner (2026-10-17 F1 fallout). Tiers, suites and
    gaps are in [docs/testing.md](docs/testing.md).

## Architecture

```
+--------------------------------+
| Qt Quick UI (QML)              |  Home · Dash · Live · Log · Setup
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
| Mock transport | Simulates idle RPM with rev blips, coolant warm-up, trailing intake temperature, and coupled throttle/load. Every frame decodes against the mock set. | — |
| Bridge and models | Frame log with sort toggle; coalesced signal model with a 120-sample history per signal; device discovery for the picker; link-health polling (#11) | [ADR-0003](docs/adr/0003-backpressure-bounds-in-flight-frames.md), [ADR-0005](docs/adr/0005-coalesced-signal-updates.md) |
| QML pages | Home, Live (table and strip chart), Log, Settings (device picker plus manual URI). Dashboard is a placeholder. | — |
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
    2026-10-16 (#1's original 8-byte shape accepted behaviorally, #9
    resolved, #6 partially informed); #1's current 4-byte shape and
    the talking-bus items (#2, #3, #4, #5, #7) still open, 2026-10-16 
    (#9 resolved, #6 partially informed); #1 resolved for
    both shapes (8-byte behaviorally 2026-10-16, 4-byte with logged
    vendor status 0 2026-10-20); the talking-bus items (#2, #3, #4,
    #5, #7) still open
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
     2026-10-16 session accepted. The 4-byte shape has never met real
     hardware. If the VX rejects it, the child log now carries the
     vendor's filter status verbatim — capture it.
   - CONFIRMED 2026-10-20 (no vehicle): the VX accepted the 4-byte
     shape through a bare `j2534://` URI (obd2-derived channel),
     `OP_START_FILTER` vendor status 0, filter_id 1
     ([session note](bench-sessions/2026-10-20.md)).
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
   frame", and anything else is a hard failure (#11). Frames are
   delivered whatever status they arrive with, so a vendor that pairs
   `ERR_TIMEOUT` with a partial batch loses nothing. The same rule
   also trusts the count under a hard error (#11).    Observed 2026-10-20 
   (no vehicle): the VX returns `STATUS_NOERROR` with zero messages for an 
   empty read — a third flavor, neither of the two the split names; it behaves 
   as "no frame", and the session's fault state came from the writes, not the 
   reads ([session note](bench-sessions/2026-10-20.md)). Still to observe on
   a talking bus: whether it pairs `ERR_TIMEOUT` with data at all.
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

1. **Windows CI installs aqtinstall from git master.**
   - A sporadic py7zr `Bad7zFile` failure has been seen once, and a
     re-run passed.
   - If it recurs, pin a known-good post-#1000 commit plus
     `py7zr==1.1.0`.
   - Go back to the PyPI release once aqtinstall 3.4+ ships the
     layout fix.
2. **The mobile apps are unvalidated.** Since 2026-10-20, CI
   cross-builds an Android arm64-v8a APK and an unsigned iOS arm64
   device build. A launch smoke for the iOS Simulator
   (`ios-simulator-smoke`) exists but is manual-only for now (#30).
   Neither app has run on a device, the
   Android app has never been launched, and no tests run on either
   (cross builds set `BUILD_TESTING=OFF`). Off Windows the only
   transport is `mock://`, so a mobile session can't reach a vehicle
   yet.
4. **Decoding ignores subscription state.** Every signal is decoded
   for every frame and filtered at the model. Filter at decode time
   once signal counts grow.
5. **Oldest-first log mode has no auto-follow.**
6. **The session API is single-caller by contract.** The bridge is the
   only caller.
7. **`notifyDropped()` runs on every frame** in `handleFrameInternal`.
   It only emits when the count changes, but it should move into the
   coalesce batch before real-data rates arrive.
8. **`CoreTest.SessionStartWithoutTransportFails` is a `SUCCEED()`
   stub.** Implement it or delete it.
9. **The mock has no scripted drive cycle.** One candidate is a
   2nd-gear WOT pull with a 2→3 shift. It needs `SimState` physics
   changes and updated ranges in
   `MockFramesDecodeAgainstMockSignalSet`. The preferred end goal is a
   2nd-gear WOT pull, slow down, a 3rd-gear WOT pull, slow down, then a
   60–130 mph pull.
10. **Strip-chart x-axis resolution is one coalesce window, about
    100 ms.** Per-sample timestamps need decoding to move onto the
    worker.
11. **J2534 link health is HARNESS-tested, surfaced in the UI, and
    BENCH-observed once.** Since 2026-10-19 both backends split
    `ERR_TIMEOUT` (vehicle timeout), `ERR_BUFFER_EMPTY` (no frame) and
    other statuses (hard failure), and the worker debounces timeouts,
    counts hard-failure episodes and backs off
    ([j2534.md](docs/j2534.md#session-lifecycle)). Both request modes
    keep sending (each request is the recovery probe), a failure always
    records `QT_ERR_TRANSPORT_HARD_FAILURE`, and the backoff grows from
    100 ms to a 5 s cap. Since 2026-10-20, in-proc HARNESS tests cover
    the canonical code, one episode per outage, the quiet debounce and
    recovery (`HardFailureSetsCanonicalErrorAndRecovers`,
    `QuietBusDebouncesToVehicleTimeoutWithoutEpisode`); the bridge polls
    the state on its 100 ms tick, logs transitions to the activity
    log, and publishes `linkState`, which LiveView (banner), the Home
    status card and the Settings connection label show. Never-arrived
    signals render "—" (`hasValue`, notified per batch), and stale
    values dim (the QML computes age from a page clock; the `ageMs`
    role stays unnotified and unused — deliberate, explained in-file).
    Smoke recipe: [testing.md](docs/testing.md#manual-smoke--link-health-ui).
    Still open:
    - Both backends trust the vendor's message count under a
      hard-error status, so a vendor that leaves `pNumMsgs` untouched
      produces phantom frames that read as healthy (code review
      2026-10-20, finding 1). The VX was observed returning
      `written 1` under a failed write, so the write side of this class
      is real.
    - The tests are in-proc only and don't pin backoff growth.
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
22. **J2534 connect blocked the GUI thread. Fixed 2026-10-16.**
    Observed with the OBDX Pro VT/VX drivers installed and no adapter:
    a picker row click and the Connect button both called the
    synchronous connect, freezing the UI about 8 s per attempt. It was
    blamed at the time on a vendor blocked in `PassThruOpen`, but the
    installed OBDX drivers turned out to refuse open within seconds
    (VT: status 34). The freeze more likely came from the old startup
    order, vendor open before the pipe connect, which left the parent
    waiting out the handshake window on a child that had already
    exited. Fixes:
    - Picker rows fill the URI field only.
    - `connectSessionAsync` runs create and start on a worker thread
      with a queued completion.
    - The child connects the pipe first, then loads the DLL, resolves
      its exports and calls `PassThruOpen` when `OP_OPEN` arrives. A
      connected pipe proves only that the child started. An unanswered
      `OP_OPEN` with the child alive means the vendor is blocked in
      `DllMain` or `PassThruOpen`; the client then terminates the
      child, which can't answer `OP_SHUTDOWN` from inside vendor code.
      A refused open comes back as the vendor status.
    - Test knobs: `QTTUNE_SUR_HANDSHAKE_MS` (handshake window) and
      `QTTUNE_FAKE_OPEN_DELAY_MS` (fake open sleep; it arms the x86
      child from an x64 test through environment inheritance).
23. **Surrogate tracing: the parent's half is still weak.** Since
    2026-10-19 the child's log (`%TEMP%\qttune-surrogate.log`) carries
    millisecond timestamps and the vendor status of every `OPEN`,
    `CONNECT`, `DISCONNECT`, `IOCTL`, `START_FILTER` and
    `STOP_FILTER`. `OP_WRITE` is logged when its status changes, and
    `OP_READ` when its status changes or frames arrive (so about
    10 lines/s while an ECU answers). The client logs startup failures:
    CreateProcess, handshake timeout, the child's decoded exit code, and
    refused `PassThruOpen`/`PassThruConnect`. Still open:
    - The parent's `SUR_LOG` lines go to stderr, which the GUI app only
      has when launched with a redirect (`2> sur.log`).
    - Parent lines carry no timestamps, and the child's clock counts
      from its pipe connect, so matching the two logs still takes care.
    - A blocked `OP_OPEN` is logged only as a generic transaction
      timeout, the handshake-timeout line wrongly suggests a blocked
      `PassThruOpen`, and the exit-5 text says the missing exports are
      "named above" when they're in the child's log (code review
      2026-10-20, finding 3).
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
30. **The iOS simulator smoke can't yet show the app drawing.** Its
    first run (2026-10-20, run #73) passed: the app installed,
    launched, loaded QML and was still running 15 s later. But the
    screenshot was blank. The whole artifact zipped to 6.8 KB, which
    fits a single flat frame. The app's own background (`#14141c`) is
    nearly black, so this may be that background with nothing drawn
    on it, or nothing at all. The job also took 19.5 min, against
    about 11 min for the rest of CI. It's manual-only
    (`workflow_dispatch`) until a run with the new diagnostics settles
    the cause:
    - `home.png` is blank too: the runner's simulator display is at
      fault, not the app.
    - The home screen draws but `launch-15s.png` and `launch-45s.png`
      are blank, and stderr has no `QSG_INFO` lines: the window was
      never exposed, so the scene graph never started.
    - `QSG_INFO` lines are present and both shots are still blank: the
      scene graph started and its frames aren't reaching the screen.
      Try another `QSG_RHI_BACKEND`.
    - Only the 15 s shot is blank: the first frame is slow on the
      Intel VM. Re-enable the job with a longer wait.

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

**Backlog (unscheduled):**

- Hide mock connections behind a developer-mode setting on the
  Settings page
- A basic dashboard with RPM and speed gauges, maybe one more,
  customizable later
- Change the Log page to show application log messages instead of
  mock frame data
- Save data logs to a file, once live vehicle data shows on LiveView
- Run the mobile apps on a real device and the Android emulator (#2)
- A scripted mock drive cycle (#9)
- Distinguish refused vendor open (a status code) from load failure in
  the URI-facing error text — today both surface as
  "failed to load transport library"
- Plumb `PassThruGetLastError` for vendor status text (the VT's
  status 34 is unlabeled)
- Picker row URIs templated with profile parameters — less urgent
  now that channel derivation makes the natural flow produce a
  working `obd2` session; the remaining case worth exposing is
  `?profile=raw` (log-only) and explicit ID overrides for oddball
  vehicles
- update code base, docs, compilation floors to C++20(gives std::span, 
  coroutines, ranges, std::format, etc) but removes older compiler support 
  (MSVC 2019, gcc 11, clang 14 all missing full support) - low priority / debatable 
- android auto / apple car play target for the dashboard of live values to use
  infotainment as a potential display. More invasive options: direct usage including
  overwrite or extension of vehicles infotainment system (highly vehicle dependent)
- expand logging of connections and bus state to the activity log on settings page
  (link-health transitions are logged there since 2026-10-20)