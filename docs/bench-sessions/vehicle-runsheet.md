# VEHICLE run sheet — first read-only vehicle session

**Date:** <date>  **Second person present, or procedure posted:** <yes, or where posted>
**Adapter:** OBDX Pro VX (USB)  **Vehicle:** <year make model>; CAN-era OBD-II, expected 11-bit IDs at 500 kbit/s
**QtTune build:** <commit hash>  **Trees:** build, build-win32 both fresh

Fill in a copy of this sheet as `docs/bench-sessions/<date>.md`. The
no-vehicle BENCH tier is [bench-runsheet.md](bench-runsheet.md); this
sheet picks up at its Phase 4 stop point.

## Purpose

First session against a talking bus. Goals, in order:

1. Requests leave the adapter and an ECU answers them (BENCH prep #2,
   request framing)
2. Replies decode to plausible values through the `obd2` profile
3. Link health behaves as designed on a real bus, ignition off and on
4. Validate or invalidate the talking-bus items in the
   [BENCH prep list](../../STATUS.md#bench-prep) (#2, #3, #4, #5, #6,
   #7, #8)
5. Record each phase: the recordings are the first real-bus replay
   fixtures ([ADR-0017](../adr/0017-session-recording.md)), and the
   bitmaps and reply timings are what
   [ADR-0015](../adr/0015-request-engine.md)'s open questions wait on

Record everything, including every connect's session log. A session
that stops at phase 2 with evidence is worth more than one that runs
to the end undocumented.

## Safety and scope

- **Read-only.** QtTune sends only Mode-01 ("show current data")
  requests, the same read-only service a generic OBD-II scanner uses.
  It has no code path that writes, clears codes or flashes. Read
  [DISCLAIMER.md](../../DISCLAIMER.md) before starting.
- **Parked:** transmission in Park (or neutral), parking brake set.
  Run any engine-on phase outdoors or with ventilation.
- **Laptop power:** on its own battery, not an inverter on the
  vehicle.
- **Abort at once if:** a new warning lamp lights, a gauge or the
  cluster misbehaves, relays click repeatedly, the adapter gets hot,
  or the app crashes. **Abort order:** Disconnect in the app (or close
  it), unplug the adapter, then key off. Write down what you saw before
  anything else.

## Pre-flight (before going to the vehicle)

- [ ] Fresh build of both trees, **Win32 first**, and the app build
      prints "Deploying Win32 J2534 surrogate next to qttune.exe"
      (see [bench-runsheet.md](bench-runsheet.md) pre-flight)
- [ ] Full suite green: every case passes except the env-gated skip
      (current count in [STATUS.md](../../STATUS.md)), plus the
      cross-arch subset
- [ ] The same build passes the quiet-bus BENCH run with the VX
      ([bench-runsheet.md](bench-runsheet.md) Phases 1–3): it loads
      through the surrogate, opens, the filter is accepted, requests
      fail on the no-ACK bus, and teardown is clean. There's no ECU
      simulator, so the vehicle is the first talking bus; this is the
      risk that can be retired at the desk
- [ ] Launch the app and tick **Record sessions** in Settings before
      the first connect (it can't change during a session). Each
      connect then writes a folder under `Documents\QtTune\recordings`
      with every frame (`frames.csv`, host and vendor time side by
      side), every decoded value (`signals.csv`) and the activity lines
      (`events.log`) ([ADR-0017](../adr/0017-session-recording.md)).
      The Activity log names the folder at each connect
- [ ] Recording also turns on the surrogate's payload logging. After
      the first connect, the session log at `%TEMP%\ecuio-surrogate.log`
      holds both `[surrogate HH:MM:SS.mmm]` and `[sur …]` lines, and a
      child line reads "frame logging on". Every request and reply
      appears as an `OP_WRITE frame` / `OP_READ frame` line with its
      bytes and statuses ([j2534.md](../j2534.md#surrogate)), which the
      recording can't see. Each connect takes the fixed name and keeps
      the previous log as `ecuio-surrogate.<YYYYMMDD-HHMMSS-mmm>.log`
      beside it, the newest 50, so copy them all at teardown and don't
      run the test suite before you have
- [ ] Know the four link states before you need them (see
      [j2534.md](../j2534.md#session-lifecycle)):
      - no banner: ok;
      - amber "Vehicle not responding": quiet;
      - amber "Requests failing — check the vehicle connection and
        ignition": writes fail while reads are clean, the no-ECU or
        ignition-off case;
      - red "Adapter fault": reads fail.
      The Settings activity log records each transition; the header
      pill, Home and the Dashboard header show the same state in short
      form.
- [ ] Settings picker lists the VX as "x86". Every connect below goes
      through Settings, the only page that connects: type the phase's
      URI into the URI field (the VX row fills the bare one), then
      Connect
- [ ] Camera or notebook ready
- [ ] A reference for the values: the cluster's gauges (tachometer,
      coolant, fuel), and a second scan tool if you have one. Use the
      scan tool between phases, with QtTune disconnected, **never on
      the bus at the same time**: its replies also arrive on 0x7E8, so
      QtTune would decode and record them as its own, which skews the
      refresh and timing numbers, and both adapters may answer a
      multi-frame reply with flow control
- [ ] Rehearse at the desk with the x86 fake in place of the VX, with
      recording on, so the evidence pipeline has run once before it
      matters. Its URI is `j2534://` plus the full path to
      `build-win32\tests\Release\fake_j2534.dll`, then each phase's
      query:
      - Phase 0: launch with `ECUIO_FAKE_WRITE_STATUS=7` in the
        environment, then the bare URI: "Requests failing";
      - Phase 1: relaunch without it, then `?profile=raw&filter=0x7E8`:
        "Vehicle not responding";
      - Phase 2: `?poll=0x0C`, then the 0x00 bitmap read: the fake's
        bitmap is all zeros, so its last bit says to skip the other
        two;
      - Phase 3: the bare URI: every `obd2` signal shows the fake's
        canned value.

      Then do the teardown and the post-session scrub on what it
      produced: the recordings and the kept session logs. Phase 5's
      recovery can't be rehearsed this way, because the surrogate
      reads the fake's settings once, at spawn. The knob recipes are
      in [testing.md](../testing.md)'s "Manual smoke — link-health UI"
      table

Abort condition: the suite isn't green, or the surrogate isn't
deployed. Stop and fix it at the desk; the vehicle adds nothing.

## Phase 0 — ignition off

**Action:** adapter plugged into the vehicle, ignition **off**. Connect:

    j2534://OBDX Pro - VX

**Expected** (the no-ACK bus seen on the 2026-10-20 bench):
- [ ] Connect succeeds, the filter is accepted (session log
      `OP_START_FILTER … status 0`)
- [ ] The first few writes succeed, then writes fail (`OP_WRITE: status 7`)
      once nothing ACKs them. At the first failed write the link latches
      `ECUIO_ERR_REQUEST_FAILED`, one episode, and stays there: amber
      "Requests failing" banner, and "Link: requests failing
      (1 episode(s))" in the activity log
- [ ] Disconnect cleanly

This state is correct with nothing awake on the bus. Don't debug it.
If an ECU does answer with the ignition off (some stay awake briefly
after key-off), record that instead.

**Evidence:** the session log; the UI state; how many writes succeeded
before the first failure.

## Phase 1 — ignition on, engine off, listen only

**Action:** ignition **on**, engine off. Connect without requests:

    j2534://OBDX Pro - VX?profile=raw&filter=0x7E8

**Expected:**
- [ ] Connect succeeds on a powered bus
- [ ] No writes in the session log (passive mode)
- [ ] Usually no frames: ECUs don't send on 0x7E8 unasked. The link
      goes quiet ("Vehicle not responding") after about 1.2 s, which is
      correct here

**Evidence:** quiet or unsolicited frames, and if frames, their IDs
and payloads verbatim (the session log's `OP_READ frame` lines).

## Phase 2 — one PID

**Action:** same ignition state. Connect:

    j2534://OBDX Pro - VX?poll=0x0C

(the `obd2` profile stays in force, so the reply decodes; only PID 0x0C
is requested)

**Expected:**
- [ ] Log page: frames with ID `0x7E8`, DLC 4, payload `41 0C` + two
      data bytes; engine off, RPM reads 0 or close to it. On the wire
      the reply is an 8-byte CAN frame with a PCI byte and padding, but
      the adapter reassembles ISO 15765 and hands over only the data,
      so DLC 4 is what to expect. Record the DLC either way (in the
      session log's `OP_READ frame` lines, `len` includes the 4-byte
      CAN ID, so DLC 4 is `len 8`):
      - DLC 8 starting `41 0C`: this adapter passes the padding through.
        Decoding still works; note the pad bytes;
      - a payload that doesn't start with `41` (for example
        `04 41 0C …`): a PCI byte left in-band (see "If it fails")
- [ ] LiveView: RPM populated; every other signal shows "—"
- [ ] No banner (link ok)
- [ ] The supported-PID bitmaps, same ignition state, one connect
      each:

          j2534://OBDX Pro - VX?profile=raw&filter=0x7E8&poll=0x00
          j2534://OBDX Pro - VX?profile=raw&filter=0x7E8&poll=0x20
          j2534://OBDX Pro - VX?profile=raw&filter=0x7E8&poll=0x40

      Each reply is `41 <PID>` plus four bitmap bytes, one bit per
      PID, most significant bit first: 0x00's bits name PIDs
      0x01–0x20, 0x20's name 0x21–0x40, and 0x40's 0x41–0x60 (SAE J1979
      Service 01; checked against Wikipedia's "OBD-II PIDs" table).
      Each bitmap's last bit says whether the next bitmap PID is
      supported, so if it's clear, skip the reads after it. A request
      with no reply, or a reply starting `7F`, is recorded as it is.
      Together they cover PIDs 0x01–0x60, which predicts the Phase 3
      signals that stay "—"; if the `obd2` set gains a PID past 0x60,
      add the next bitmap

**Evidence:** the recording's `frames.csv` (reply bytes, vendor
timestamps) and `signals.csv` (what they decoded to), and the session
log's `OP_WRITE frame` and `OP_READ frame` lines (request and reply
bytes, statuses, RxStatus). To compare a few payloads on screen,
Pause holds the Log page's rows still; the recording keeps every
frame either way.

## Phase 3 — full scheduler

**Action:** same ignition state. Connect with the bare URI:

    j2534://OBDX Pro - VX

**Expected:**
- [ ] Every `obd2` signal populates within about one rotation (one
      request per 100 ms, so N PIDs take about N × 100 ms; about 1 s
      for the current set); a PID the ECU doesn't support stays "—"
- [ ] Plausible engine-off values:

| Signal | Expect (engine off) |
|---|---|
| RPM | 0 |
| Vehicle Speed | 0 km/h |
| Engine Load | 0 % or near it |
| Throttle Position | small, steady |
| Coolant Temp, Intake Air Temp | near ambient on a cold engine |
| Fuel Level | roughly matches the gauge |
| Battery Voltage | ~12.0–12.6 V |
| MAF | ~0 g/s |
| Engine Oil Temp | near ambient on a cold engine, or "—": many vehicles don't support PID 0x5C |

- [ ] Values stay bright while they keep arriving; nothing dims
- [ ] Every row is reachable on Live (the list scrolls once it fills
      its share of the page); rows past `Theme.seriesColors` reuse
      its colours, with dashed chart traces

**Evidence:** a screenshot of LiveView, and a minute or more of
recording: each signal's refresh interval comes from its `host_us`
in `signals.csv` (expect about one rotation; replies a cycle late are
a known item, not a failure), and `frames.csv`'s `timestamp_us`
against `host_us` gives the vendor timestamp's unit (BENCH prep #5).

## Phase 4 — engine idling (the decode check)

Only if phases 0–3 were clean, and with the second person present.
With the engine off, most values are 0 or ambient, which a wrong scale
or byte order can still produce; Fuel Level and Battery Voltage are the
exceptions. Idle is where RPM's two-byte value and the charging
voltage move, so this phase is the strongest check of the decode.

**Action:** start the engine with the app connected on the bare URI.
The connection may drop during cranking; if so, reconnect once the
engine is idling.

**Expected:**
- [ ] RPM ~600–900, matching the tachometer to within its needle;
      Battery Voltage ~13.5–14.5 V; Coolant Temp rising over minutes,
      moving with the cluster's gauge if it has one; Fuel Level
      roughly matching the gauge

**Evidence:** a photo of the cluster and LiveView together, and the
recording. If a second scan tool is on hand, disconnect QtTune, read
RPM, coolant and voltage with it at idle, and note them beside
QtTune's.

## Phase 5 — ignition cycle (optional)

**Action:** with the bare URI connected and the engine off, turn the
ignition off, wait 10 s, then turn it back on.

**Expected:**
- [ ] Ignition off: the "Requests failing" banner at the first failed
      write (writes may succeed for a few cycles first, as in Phase 0)
- [ ] Ignition on: values resume within about 7 s, and the activity
      log shows "Link: recovered". While requests fail, the worker
      waits between probes, doubling from 100 ms to a 5 s cap, so
      after a 10 s outage the next probe can be up to 5 s away, and
      the ECU needs a moment to wake. Record the time from ignition-on
      to the first value; longer than ~10 s is a finding

## If it fails, suspect

| Symptom | Suspect |
|---|---|
| Connect fails at once, before any write ("failed to load transport library") | The session log's `open:` lines name the stage: the surrogate missing next to `qttune.exe` (pre-flight), a surrogate from a stale `build-win32` ("built from other shared headers": rebuild both trees), the DLL not loading, or the vendor refusing `PassThruOpen` with its status (the VT refuses with status 34 when its adapter is absent). Check the URI names the VX |
| Writes fail with the ignition on (`OP_WRITE: status` nonzero) | BENCH prep #2, request framing (6-byte logical message with `ISO15765_FRAME_PAD`); then baud (500 k assumed) or a 29-bit vehicle (prep #4). The bare URI is 11-bit only. 29-bit IDs given with `?filter=`/`?txid=` connect with `CAN_29BIT_ID` automatically, but no 29-bit OBD IDs are prepared: record and stop rather than improvise |
| Writes succeed but no frames arrive | The ECU doesn't answer physical 0x7E0: retry Phase 2 with `?poll=0x0C&txid=0x7DF` (functional request, replies still on 0x7E8); then the flow-control filter (prep #1) |
| `0x7E8` frames on the Log page, but LiveView shows "—" everywhere | Payload shape: stamping already proves the channel matched, so suspect a PCI byte left in-band shifting `[41][PID]`. The `OP_READ frame` lines show the bytes as the vendor returned them |
| Values implausible (−40 °C, huge RPM) | Scaling or byte order: temperatures are unsigned 8-bit with a −40 offset; 2-byte PIDs are big-endian |
| Link flips between ok and quiet while frames arrive | Unmatched frames interleaving with replies: a single unmatched frame reads as quiet with no debounce, by design. Note the frame IDs |
| Slow refresh, replies a cycle late | Known refresh-rate item (20 ms first read vs up to 50 ms ECU response). Measure; don't fix in the session |
| "Disconnecting…" lasts seconds | A wedged adapter. The teardown runs off the GUI thread and gives up after about 6 s; Connect stays disabled until it ends. Note how long (the activity log's `Disconnecting:` and `Disconnected:` lines bracket it) and whether the session log says `child unresponsive`. The window itself shouldn't freeze on Disconnect; if it does, that's a bug: capture the session log |
| App crashes | Abort. Capture the session log and `sur.log` before relaunching |

## Evidence against the BENCH prep list

| Prep item | What settles it |
|---|---|
| #2 Request framing | Phase 2 replies arrive |
| #3 No checksum byte | Replies decode without a trailing byte |
| #4 29-bit addressing | 11-bit IDs expected; any 29-bit traffic is news |
| #5 Timestamp units | Phase 3: in `frames.csv`, `timestamp_us` deltas against `host_us` deltas (µs if they match) |
| #6 ReadMsgs statuses | Session log: does any `OP_READ frame: status 9` line appear (`ERR_TIMEOUT` with data)? |
| #7 Struct layout | A correct decode in phases 2–3 implies the layout is right |
| #8 Vendor timeouts | Read timing visible in the session log stamps |

## Teardown

1. Disconnect in the app and wait for `Disconnected:` in the activity
   log (the teardown ends there); the session log shows
   `OP_DISCONNECT: vendor status 0` exactly once
2. Copy this session's recording folders (the Activity log named
   each) and its session logs: `%TEMP%\ecuio-surrogate.log` and the
   earlier ones kept beside it, this session's by their stamps
3. Unplug the adapter, then key off

## Post-session actions

1. Copy this sheet, filled in, to `docs/bench-sessions/<date>.md`, with
   the session logs and recordings alongside (the 2026-10-16 session's
   `surrogate-phase*.log` files are the precedent). Before committing,
   scrub the sheet, the logs, the recordings' `events.log` (it holds
   the URI) and any screenshots: no VIN or calibration IDs, no names
   or initials, and no local paths (`%TEMP%` and `C:\Users\…` paths
   carry the account name)
2. Walk the [BENCH prep list](../../STATUS.md#bench-prep): mark each
   item CONFIRMED / REFUTED (with evidence) / STILL UNKNOWN
3. Update STATUS.md (phase checklist: "first live, read-only vehicle
   session"), the testing.md tiers and gaps, and the open items the
   session settles: the surrogate health path, unmatched-frame
   flapping, and the refresh rate
4. Record what the request engine's design waits on, in
   [ADR-0015](../adr/0015-request-engine.md)'s open questions:
   request-to-reply times per PID (from the `OP_WRITE frame` /
   `OP_READ frame` stamps), whether any reply was "response pending"
   (`7F 01 78`), and Phase 2's supported-PID bitmaps
5. Engineering-log entry if a lesson emerged
