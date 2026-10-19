# QtTune — Project Status

**Updated:** 2026-10-18 · **Version:** v0.3.0-dev (last tag v0.2.0) ·
[![CI](https://github.com/erickramer42/qttune/actions/workflows/ci.yml/badge.svg)](https://github.com/erickramer42/qttune/actions/workflows/ci.yml)

This file covers the project as it stands: what works, what comes
next, and what is known to be wrong. Why it is built this way is in
[docs/adr/](docs/adr/README.md). How it got here, with lessons
learned, is in [docs/engineering-log.md](docs/engineering-log.md).
Safety, regulatory and warranty caveats are in
[DISCLAIMER.md](DISCLAIMER.md).

## Where things stand

**Current phase: v0.3.0 Phase 3 (J2534).** The HARNESS tier is complete, 
including transport adoption (Part 5).The BENCH tier tested on real adapter with
"quiet bus" meaning not connected to vehicle, the first real vehicle, comes next.

- **Works:**
  - The mock transport drives the live frame log, LiveView and the
    strip chart.
  - J2534 runs end to end against the fake vendor DLL, both in-proc
    and through the x86 surrogate, including write/ioctl/filter relay on both backends, transport adoption: filters, the Mode-01 request loop, and frame identity via ?filter=/?txid=/?poll= (ADR-0012),     and vendor calls through the surrogate are serialized behind one
    lock (review 2026-10-17 finding 2, issue #24 fixed 2026-10-17):
    the child never enters a vendor DLL from two threads, and teardown
    holds the same lock across disconnect/close/FreeLibrary, so a
    stuck vendor call becomes a job-reaped linger, never an unmap
    under live vendor code
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
- **Not yet:**
  - BENCH coverage beyond the quiet-bus tier. The formal first BENCH
    session (2026-10-16, [docs/bench-sessions/2026-10-16.md](docs/bench-sessions/2026-10-16.md))
    succeeded: the OBDX Pro VX loaded through the surrogate, opened with
    USB power (vendor status 0), accepted the 8-byte flow-control filter
    shape (behaviorally — see #23), and survived a 2-minute quiet-bus
    soak at <1% CPU with clean teardown. Talking-bus behaviors, VBATT,
    and timestamp units remain untested. The OBDX Pro VT still refuses
    open (status 34) with its adapter absent. The run sheet template is
    [docs/bench-sessions/bench-runsheet.md](docs/bench-sessions/bench-runsheet.md).
  - Any live vehicle.
  - J2534 decode on real hardware: the `obd2` set, scheduler and
    channel derivation are HARNESS-verified against the fake; a
    live-vehicle session (real ECU responses, talking-bus items in
    the BENCH prep list) is still ahead.
  - **Tests:** 136 ctest cases, passing locally (Windows/MSVC 2022)
    and in CI on all three platforms. The cross-arch routing case
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
                | named pipe (control) + shared-memory ring (data)
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
      |                         or drained from the surrogate's ring)
      | qttune_frame_t
      v
core: snapshot dispatch to registered callbacks ............ ADR-0002
      | QtTuneBridge::onFrame (worker thread): copy, in-flight gate, post
      v
GUI thread: handleFrameInternal ............................ ADR-0003
      |- FrameListModel (10k ring) -> FrameSortProxy -> LogPage
      '- decode, gated on message ID -> staging buffer ..... ADR-0004
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
| Signal decode | Linear scaling, Intel byte order, gated on message ID. Only the mock signal set exists. | [ADR-0004](docs/adr/0004-signal-decoding-model.md) |
| Mock transport | Simulates idle RPM with rev blips, coolant warm-up, trailing intake temperature, and coupled throttle/load. Every frame decodes against the mock set. | — |
| Bridge and models | Frame log with sort toggle; coalesced signal model with a 120-sample history per signal; device discovery for the picker | [ADR-0003](docs/adr/0003-backpressure-bounds-in-flight-frames.md), [ADR-0005](docs/adr/0005-coalesced-signal-updates.md) |
| QML pages | Home, Live (table and strip chart), Log, Settings (device picker plus manual URI). Dashboard is a placeholder. | — |
| J2534 | HARNESS-verified, same-arch and cross-arch, including transport adoption (ADR-0012). | [docs/j2534.md](docs/j2534.md) |

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
  - first real vendor DLL load and connect: the OBDX Pro VX through
    the surrogate — done, formal session 2026-10-16
    ([session sheet](docs/bench-sessions/2026-10-16.md)); the OBDLink
    MX+ in-proc if its DLL is x64 — pending
  - [x] **Resolved surrogate serialization bug (#24):** serializes vendor DLL calls, clears `g_channel` on OP_DISCONNECT, prevents double disconnect and unsafe teardown; BENCH gate cleared.
  - work through [BENCH prep](#bench-prep) — quiet-bus subset walked
    2026-10-16 (#1 confirmed behaviorally, #9 resolved earlier,
    #6 partially informed); talking-bus items (#2, #3, #4, #5, #7)
    still open
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
   - So the transport registers 8-byte mask, pattern and flow-control
     messages: the 4-byte ID, then `30 00 00` (clear-to-send, BS 0,
     STmin 0), then zero padding. The trailing mask bytes are
     don't-care.
   - The equality rule is believed to be stricter than J2534-1
     requires. Relax it if a real vendor rejects this shape.
     (`j2534_transport.cpp` points here.)
2. **Request framing.** The transport supplies the ISO-TP single-frame
   PCI byte and the 8-byte zero padding itself, giving 12 wire bytes
   including the 4-byte ID. Some drivers do the framing and padding
   in-driver instead.
3. **No checksum byte.** This is ISO 15765-4 over CAN, so the K-line
   checksum convention doesn't apply.
4. **29-bit addressing** is assumed whenever either ID is above
   `0x7FF`.
5. **Timestamps** are passed through as microseconds, as the spec
   says (#12).
6. **ReadMsgs statuses.** A timeout and a hard error currently look
   the same (#11).
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
2. **Mobile builds are unvalidated.** No kit build has been attempted.
3. **The Linux full-app build is not in CI.** This is deliberate until
   Linux is a ship target.
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
   `MockFramesDecodeAgainstMockSignalSet`. preferred end goal: 2ng-gear pull WOT ->
    slow down -> 3rd gear pull WOT -> slow down -> 60-130mph pull
10. **Strip-chart x-axis resolution is one coalesce window, about
    100 ms.** Per-sample timestamps need decoding to move onto the
    worker.
11. **The J2534 read path treats every nonzero vendor status as "no
    frame."** Split `ERR_TIMEOUT` from hard errors at BENCH.
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
17. **The surrogate ring drops frames silently when full.** With 4096
    slots, overflow is theoretical at current rates, but a drop would
    be invisible. It needs a producer-side counter.
18. **The bridge's architecture label shows unknown (0) as "x86".**
    This only affects the display. Routing correctly treats 0 as
    in-proc.
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
22. **J2534 connect blocked the GUI thread; the pre-fix surrogate
    startup ran vendor open before the pipe connect. Fixed 2026-10-16.** Observed with the OBDX Pro VT/VX drivers installed and no adapter: the picker
    row click called the synchronous connect slot (selection was an
    unintended connect trigger), and Connect did too, freezing the UI ~8 s per attempt while the synchronous path waited on the child — believed at the 
    time to be a vendor blocked in `PassThruOpen`; post-instrumentation evidence (2026-10-16) shows the installed OBDX drivers REFUSE open in seconds (VT: 
    status 34) rather than block, so the full 8 s freeze was more likely the pre-fix startup order — vendor open before pipe connect — leaving the parent to burn the
    handshake window on an already-exited child.
    Fixes: picker rows fill the URI field only; `connectSessionAsync`
    runs create/start on a worker thread with queued completion;
    the surrogate now connects the pipe FIRST and performs
    `PassThruOpen` inside `OP_OPEN`, so pipe-connected proves
    load+exports and an unanswered `OP_OPEN` with a live child means
    exactly "blocked in vendor open"; a refused open is relayed as a
    FAILED response. Test knobs: `QTTUNE_SUR_HANDSHAKE_MS` (handshake
    window) and `QTTUNE_FAKE_OPEN_DELAY_MS` (fake open sleep, arming
    the x86 child from an x64 test via env inheritance). A vendor that genuinely blocks on absent hardware remains possible elsewhere in the ecosystem — and is now distinguishable: unanswered `OP_OPEN` with a live child means exactly that.
23. **The surrogate traces only startup, OP_OPEN, OP_CONNECT and
    OP_DISCONNECT** (IDs and vendor status; OP_DISCONNECT added
    2026-10-17). Filter starts/stops and ioctls are still silent in
    `%TEMP%\qttune-surrogate.log`, each with its own vendor status. Discovered in the first formal BENCH session (2026-10-16), where filter acceptance had to be inferred from session-start success rather than a logged vendor status. Also: log lines carry no timestamps, so cross-referencing with parent timings remains guesswork. Target: trace all operations with timestamps and add filter/ioctl operation logs in the next iteration.

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
- Mobile builds for Android and iOS (#2)
- macOS and Linux builds of the full desktop app (#3)
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
