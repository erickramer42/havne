# VEHICLE run sheet — read-only vehicle session

**QtTune build:** <commit hash>  **Trees:** build, build-win32 both fresh
**Date:** <date>
**Adapter:** OBDX Pro VX (USB)  **Vehicle:** <year make model>; CAN-era OBD-II, expected 11-bit IDs at 500 kbit/s

Fill in a copy of this sheet as `docs/bench-sessions/<date>-vehicle.md`.
The no-vehicle BENCH tier is [bench-runsheet.md](bench-runsheet.md);
this sheet picks up at its stop point. Two sessions have run on one
car, [2026-10-22](2026-10-22-vehicle.md) and
[2026-10-25](2026-10-25-vehicle.md). Where an expectation below
depends on the vehicle, it's that car's, and on that car a difference
from the 2026-10-25 sheet is the finding.

## Purpose

A read-only session on a vehicle's bus. Goals, in order:

1. An ECU answers the requests and the `obd2` values decode to
   plausible readings, engine off and idling
2. Link health behaves as designed on a real bus: ignition off, on,
   and through a restart
3. The probe, identification and `uds-generic` on this ECU: what it
   supports, what it says it is, which DIDs it answers and how it
   refuses the rest
4. A recording of every connect: real-bus replay fixtures
   ([ADR-0017](../adr/0017-session-recording.md),
   [ADR-0023](../adr/0023-replay-transport.md)) and the request
   timings ([ADR-0015](../adr/0015-request-engine.md))

Record everything, including every connect's session log. A session
that stops at Phase 1 with evidence is worth more than one that runs
to the end undocumented.

## Safety and scope

- **Read-only.** QtTune sends only read requests: Service 01 ("show
  current data"), Service 09 (vehicle information: VIN, calibration
  IDs) and Service 0x22 (ReadDataByIdentifier), the same read-only
  services a generic scanner uses. It has no code path that writes,
  clears codes, changes a diagnostic session or flashes. Read
  [DISCLAIMER.md](../../DISCLAIMER.md) before starting.
- **Parked:** transmission in Park (or neutral), parking brake set.
  The engine idles for most of the session: outdoors or with
  ventilation.
- **The car's battery: keep the ignition on with the engine off
  short.** Then every module is awake and nothing charges. On
  2026-10-25 the engine-off phases took 16 minutes, and the adapter's
  battery reading fell from 12.2 V to 11.8 V. This sheet keeps that
  state to Phase 1, about two minutes; everything else runs with the
  engine idling or the ignition off.
  - Before Phase 1: headlights, blower, seat heaters and the radio
    off, doors shut so the interior lights go out.
  - Never leave the ignition on between connects: start the engine or
    key off. Changing URIs at idle costs nothing.
  - If Phase 0's "Session:" line reads under 12.0 V, skip Phase 1's
    engine-off readings and start the engine first. If Battery Voltage
    on Live falls under 12.0 V with the engine off, start the engine
    then.
- **Laptop power:** on its own battery, not an inverter on the
  vehicle, and charged to full before leaving the desk. On
  2026-10-25 the laptop's battery ran out at the car, mid-connect.
- **Abort at once if:** a new warning lamp lights, a gauge or the
  cluster misbehaves, relays click repeatedly, the adapter gets hot,
  or the app crashes. **Abort order:** Disconnect in the app (or close
  it), unplug the adapter, then key off. Write down what you saw before
  anything else.

## Pre-flight (at the desk)

Run every item on the laptop that goes to the car.

- [ ] The VX driver is installed on that laptop: the picker lists only
      adapters whose drivers are installed, and shows the VX as "x86"
- [ ] Fresh build of both trees, **Win32 first**
      ([development.md, "Build configurations"](../development.md#build-configurations)),
      and the app build prints "Deploying Win32 J2534 surrogate next to
      qttune.exe"
- [ ] `scripts/verify.sh` prints four `ok` lines, the cross-arch one
      reading `0 skipped`. On Windows it demands that discovery finds a
      device, which the VX's driver provides
      ([development.md](../development.md#running-tests))
- [ ] The same build passes the quiet-bus BENCH run with the VX on this
      laptop ([bench-runsheet.md](bench-runsheet.md)): it loads through
      the surrogate, opens, the filter is accepted, requests fail on the
      no-ACK bus, and teardown is clean. There's no ECU simulator, so
      this is the part of the risk the desk can retire
- [ ] The laptop's settings for the day: on battery, sleep set to
      never, closing the lid set to do nothing, USB selective suspend
      off (Power Options, advanced settings), Windows Update paused
- [ ] Rehearse once with the x86 fake in place of the VX, recording on,
      so the evidence pipeline has run before it matters. The URI is
      `j2534://` plus the full path to
      `build-win32\tests\Release\fake_j2534.dll`, then each phase's
      query:
      - Phase 0: launch with `ECUIO_FAKE_WRITE_STATUS=7` in the
        environment, then the bare URI: "Requests failing";
      - Phases 1–2: relaunch without it, then the bare URI: every
        `obd2` signal shows the fake's canned value, the ECU line reads
        `ECU 0x7E8: supports 10 PIDs; 10 requests, none dropped`, and
        the identity line `ECU 0x7E8 identity: VIN FAKEJ2534VIN00001;
        CALID FAKECAL-01; …; 2 refused: …`;
      - Phase 3: `?profile=raw&filter=0x7E8` goes quiet ("Vehicle not
        responding"); `?poll=0x0C` shows RPM alone;
        `?profile=uds-generic&did=1E18-1E20,F190:text` gives
        `ECU 0x7E8: 2 requests, none dropped; scan: 9 DIDs, 1 found,
        8 refused` (the polled F190 and the found 1E1C), a text row for
        F190 (the fake's VIN), a hex row `DID 1E1C` reading `12 34`,
        and a "—" row for each of the eight refused DIDs.

      Then do the teardown and the post-session scrub on what it
      produced. Phase 4's recovery can't be rehearsed this way: the
      surrogate reads the fake's settings once, at spawn. The knob
      recipes are in [testing.md](../testing.md)'s "Manual smoke —
      link-health UI" table
- [ ] Launch the app and tick **Record sessions** in Settings (it
      can't change during a session). Each connect writes a folder
      under `Documents\QtTune\recordings` (`frames.csv`, `signals.csv`,
      `events.log`), and the activity log names it. Recording also turns
      on the surrogate's payload logging: the session log at
      `%TEMP%\ecuio-surrogate.log` reads "frame logging on" and has an
      `OP_WRITE frame` / `OP_READ frame` line per request and reply
      ([j2534.md](../j2534.md#surrogate)). Each connect keeps the
      previous log beside it as
      `ecuio-surrogate.<YYYYMMDD-HHMMSS-mmm>.log`, the newest 50
- [ ] Know the four link states (see
      [j2534.md](../j2534.md#session-lifecycle)): no banner, ok; amber
      "Vehicle not responding", quiet; amber "Requests failing — check
      the vehicle connection and ignition", writes fail with nothing on
      the bus acknowledging; red "Adapter fault", reads fail. The
      activity log records each transition
- [ ] Every connect goes through Settings: the VX row fills the bare
      URI, the Profile row adds the phase's profile and the DIDs field
      its `did=`; `poll=` is typed, and a URI typed or pasted whole
      works the same
- [ ] A reference for the values: the cluster's gauges, and a second
      scan tool if you have one. Use it between connects, with QtTune
      disconnected, **never on the bus at the same time**: its replies
      also arrive on 0x7E8, so QtTune would record them as its own, and
      both adapters may answer a multi-frame reply with flow control
- [ ] Camera or notebook ready

Abort condition: the suite isn't green, or the surrogate isn't
deployed. Stop and fix it at the desk; the vehicle adds nothing.

## Phase 0 — ignition off

**Action:** adapter plugged into the vehicle, ignition **off**. Connect
the bare URI for about 30 s:

    j2534://OBDX Pro - VX

**Expected** (as on [2026-10-25](2026-10-25-vehicle.md)):
- [ ] Connect succeeds, the filter is accepted (session log
      `OP_START_FILTER … status 0`)
- [ ] Copy the "Session:" line into the sheet: the battery reading with
      the ignition off (12.2 V on 2026-10-25) and the adapter's
      versions. It decides Phase 1 (see Safety)
- [ ] Writes succeed (`OP_WRITE frame: status 0`) but nothing answers:
      the bus still acknowledges frames with the ignition off. The
      link goes quiet after about 1.2 s; the ECU line reads "no answer
      to the supported-PID query; 10 requests, none dropped" and the
      identity line "13 unanswered". On a bus that's fully asleep,
      writes fail instead (three succeed, the fourth blocks about
      0.5 s and fails with `status 7`) and the link latches "Requests
      failing", as on the no-vehicle bench. Record which
- [ ] Disconnect

Either state is correct with no ECU awake. If an ECU does answer with
the ignition off, record that instead.

## Phase 1 — ignition on, engine off: one connect, about two minutes

**Action:** lights and accessories off (see Safety), ignition **on**,
engine off. Connect the bare URI and leave it connected into Phase 2:

    j2534://OBDX Pro - VX

**Expected:**
- [ ] The probe first: the session log's first writes are the bitmap
      requests (`01 00`, `01 20`, … while each bitmap's last bit is
      set), then the rotation. The ECU line follows within about a
      second: `ECU 0x7E8: supports N PIDs; 10 requests, …`, naming each
      PID dropped (on the 2026-10-25 car, six bitmaps, 53 PIDs, `1
      dropped: 01 5C (not in its supported PIDs)`). Copy the bitmap
      replies from the `OP_READ frame` lines into the sheet
- [ ] Within a few seconds, the identity line: `ECU 0x7E8 identity:
      VIN …; CALID …; …`, and which `22 F1 xx` items were refused
      (`7F 22 31`). It's personal data, scrubbed before the sheet is
      committed. Note the answered DIDs for Phase 3's text DID
- [ ] Every signal the probe kept populates within about a second; a
      dropped one stays "—". Plausible engine-off values, the
      2026-10-25 car's in brackets:

| Signal | Expect (engine off) |
|---|---|
| RPM, Vehicle Speed, Engine Load | 0 |
| Throttle Position | steady (22 %) |
| Coolant Temp, Intake Air Temp | near ambient on a cold engine (62–63 °C and 58–62 °C: that engine was warm) |
| Fuel Level | roughly matches the gauge |
| Battery Voltage (PID 0x42) | 11.8–12.6 V (11.9 V), within 0.1 V of the "Session:" line |
| MAF | ~0 g/s (0.3) |
| Engine Oil Temp | "—" where the probe dropped 0x5C |

- [ ] A Live screenshot (Live shows no identification)
- [ ] Then start the engine **on this connect**, at most about two
      minutes after ignition-on. The connect records the start: the
      voltage dips (9.6 V on 2026-10-25), RPM rises within a second and
      peaks before it settles. If the connection drops during cranking,
      reconnect the bare URI once the engine idles

**Evidence:** the session log's `OP_WRITE frame` and `OP_READ frame`
lines (requests, replies, the bitmaps), the recording, the screenshot.
The bitmaps in this log replace the three `?poll=0x00`/`0x20`/`0x40`
connects earlier sessions made; run those (Phase 3) only if the ECU
line disagrees with the earlier sheet.

## Phase 2 — engine idling (the decode check)

Still on Phase 1's connect. With the engine off most values are 0 or
ambient, which a wrong scale or byte order can still produce; idle is
where RPM's two-byte value and the charging voltage move.

**Expected:**
- [ ] RPM settles at about 600–900 (750–850 on the 2026-10-25 car,
      after a start peak near 1,260), matching the tachometer within
      its needle; Battery Voltage 13.4–14.5 V; Coolant Temp rising over
      minutes with the cluster's gauge; Fuel Level as before
- [ ] Values stay bright while they keep arriving; nothing dims, and no
      banner
- [ ] About a minute of idle, then Disconnect

**Evidence:** a photo of the cluster and the Dashboard or Live
together, and the recording: each signal's refresh interval from
`signals.csv`'s `host_us` (about 0.4 s on the 2026-10-25 car),
`frames.csv`'s `timestamp_us` against `host_us` (BENCH prep #5), and
the session log's stamps for requests a second and each reply's time
after its request.

## Phase 3 — the other URIs, engine idling

One connect each, Disconnect between them; the engine keeps idling.

1. **Listen only**, about 15 s:

       j2534://OBDX Pro - VX?profile=raw&filter=0x7E8

   - [ ] No writes in the session log; usually no frames (ECUs don't
         send on 0x7E8 unasked), and quiet after about 1.2 s. Any
         unsolicited frame is news: record its ID and bytes

2. **One PID**, about 15 s:

       j2534://OBDX Pro - VX?poll=0x0C

   - [ ] Log page: `0x7E8` frames, DLC 4, `41 0C` plus two data bytes
         (the adapter hands over only the data: `len 8` in the session
         log, with the 4-byte CAN ID); RPM alone on Live; about 26
         requests a second. DLC 8 means the adapter passes padding
         through, a payload not starting `41` a PCI byte left in-band
         (see "If it fails")

3. **The DID scan**, until the ECU line settles (14.3 s on the
   2026-10-25 car): the VX row, the `uds-generic (DIDs)` button, then
   `F180-F1FF` in the DIDs field:

       j2534://OBDX Pro - VX?profile=uds-generic&did=F180-F1FF

   128 DIDs in ISO 14229's identification range, asked once each after
   identification; a found DID joins the polling.
   - [ ] No bitmap requests, no banner. ECU lines as the counts move,
         the last with no "unanswered": `ECU 0x7E8: k requests, none
         dropped; scan: 128 DIDs, k found, r refused` (k + r = 128; `2
         found, 126 refused` on the 2026-10-25 car). Note the time from
         `Connected:` to that line
   - [ ] How the ECU refuses: `7F 22 <NRC>` in the `OP_READ frame`
         lines. `7F 22 31` is expected; record any other code verbatim
         (`78` is waited out, `11` drops the channel's polled Service
         0x22 requests, `33` refuses one DID: findings, not faults)
   - [ ] Live has a row per DID, found ones refreshing as hex. They're
         personal data (VIN, serials): don't photograph Live here

4. **One DID as text**, about 15 s: one Phase 1's identity line showed
   answered whose bytes read as ASCII and isn't the VIN or a serial
   (on the 2026-10-25 car only F188 qualified):

       j2534://OBDX Pro - VX?profile=uds-generic&did=F188:text

   - [ ] One Live row showing the text, refreshing (a text row's plot
         box is grayed out); no banner. A DID the ECU refuses drops at
         its first reply, and with nothing left to ask the link goes
         quiet a second later, by design

5. **Only if Phase 1's ECU line disagreed with the earlier sheet**, the
   bitmaps one at a time:

       j2534://OBDX Pro - VX?profile=raw&filter=0x7E8&poll=0x00

   and `poll=0x20`, `0x40`, … while each reply's last bit is set. Each
   reply is `41 <PID>` plus four bytes, one bit per PID, most
   significant first (SAE J1979 Service 01).

**Evidence:** each connect's recording and session log. Everything
from the scan and the text DID carries the VIN and the ECU's serial,
so it's scrubbed before any of it is committed.

## Phase 4 — restart (link health through an ignition cycle)

**Action:** connect the bare URI with the engine idling. Key off (the
engine stops), wait 10 s, then start the engine again; don't pause at
ignition-on.

**Expected:**
- [ ] Key off: values dim after 3 s and the link goes quiet (as in
      Phase 0), or "Requests failing" on a bus that sleeps
- [ ] Restart: values resume and the activity log shows the link
      recovering (11 s after it went quiet on 2026-10-25). While
      requests fail, the worker waits between probes, doubling from
      100 ms to a 5 s cap, so the next can be up to 5 s away. Record
      the time from the restart to the first value; longer than about
      10 s is a finding
- [ ] Optional, the probe on a cold connect: Disconnect, key off,
      connect the bare URI, then start the engine at once. The ECU
      line reads "no answer to the supported-PID query" and "none
      dropped" first; once the link is ok the walk resumes and a second
      line names what the ECU supports. Silence must drop nothing
- [ ] Disconnect, then the teardown

## If it fails, suspect

| Symptom | Suspect |
|---|---|
| Connect fails at once, before any write | The Settings error line names the cause: "surrogate process failed" (no surrogate next to `qttune.exe`, or a stale `build-win32`: rebuild both trees), "failed to load transport library" (the DLL not loading), or "the adapter's driver refused: PassThruOpen returned status 34" (the adapter isn't attached; the VT and VX both answer 34). Check the URI names the VX |
| Writes fail with the ignition on (`OP_WRITE: status` nonzero) | BENCH prep #2, request framing (6-byte logical message with `ISO15765_FRAME_PAD`); then baud (500 k assumed) or a 29-bit vehicle (prep #4). The bare URI is 11-bit only. 29-bit IDs given with `?filter=`/`?txid=` connect with `CAN_29BIT_ID` automatically, but no 29-bit OBD IDs are prepared: record and stop rather than improvise |
| Writes succeed but no frames arrive | The ECU doesn't answer physical 0x7E0: retry Phase 3's one PID with `?poll=0x0C&txid=0x7DF` (functional request, replies still on 0x7E8); then the flow-control filter (prep #1) |
| `0x7E8` frames on the Log page, but Live shows "—" everywhere | Payload shape: stamping already proves the channel matched, so suspect a PCI byte left in-band shifting `[41][PID]`. The `OP_READ frame` lines show the bytes as the vendor returned them |
| Values implausible (−40 °C, huge RPM) | Scaling or byte order: temperatures are unsigned 8-bit with a −40 offset; 2-byte PIDs are big-endian |
| Link flips between ok and quiet while frames arrive | Unmatched frames interleaving with replies; note the frame IDs |
| Values refresh much slower than once a rotation | Replies arriving after the next request: compare each `OP_READ frame` stamp with the request before it. Measure; don't fix in the session |
| The scan finds nothing: every DID `7F 22 31` | The ECU keeps its identification DIDs for another diagnostic session, or uses another range. Compare with the identity line's `22 F1 xx` items: if those answered, the scan should find them too. With nothing found there's nothing left to ask, so the link turns quiet a second after the last refusal, by design. QtTune never changes the session |
| Every Service 0x22 reply is `7F 22 11` | The ECU doesn't serve Service 0x22 here: every identification DID and scanned DID is asked once and refused (`scan: 128 DIDs, 0 found, 128 refused`), then quiet, as in the row above. Identification by Service 09 still works |
| The identity line never arrives | An item left unanswered holds the line until the link recovers and the re-ask settles. Find the `22 F1 xx` or `09 xx` request with no reply in the session log; the ECU line doesn't wait for it |
| "Disconnecting…" lasts seconds | A wedged adapter. The teardown runs off the GUI thread and gives up after about 6 s. Note how long (the `Disconnecting:` and `Disconnected:` lines bracket it) and whether the session log says `child unresponsive`. A frozen window is a bug: capture the session log |
| App crashes | Abort. Copy the session logs (`%TEMP%\ecuio-surrogate*.log`) before relaunching |

## Evidence against the [BENCH prep list](bench-prep.md)

| Prep item | What settles it |
|---|---|
| #2 Request framing | Replies arrive (Phase 1) |
| #3 No checksum byte | Replies decode without a trailing byte |
| #4 29-bit addressing | 11-bit IDs expected; any 29-bit traffic is news |
| #5 Timestamp units | Phase 2's `frames.csv`: `timestamp_us` deltas against `host_us` deltas. The VX's clock doesn't reset at connect and wraps 71.6 min after it starts; session time runs through the wrap |
| #6 ReadMsgs statuses | Session log: does any `OP_READ frame: status 9` line appear (`ERR_TIMEOUT` with data)? So far replies came with status 0 and empty reads with 16 |
| #7 Struct layout | A correct decode in Phases 1–2 implies the layout is right |
| #8 Vendor timeouts | Read timing visible in the session log stamps |
| #10 Session info | Phase 0's battery reading against the car's, and against PID 0x42 in Phase 1 |

## Teardown

1. Disconnect in the app and wait for `Disconnected:` in the activity
   log; the session log shows `OP_DISCONNECT: vendor status 0` exactly
   once
2. Unplug the adapter, then key off
3. Copy this session's recording folders (the activity log named
   each) and its session logs: `%TEMP%\ecuio-surrogate.log` and the
   earlier ones kept beside it, this session's by their stamps. Copy
   them before `scripts/verify.sh` or the test suite runs on that
   laptop: a run spawns dozens of surrogates, and the newest-50 cap
   would delete this session's logs

## Post-session actions

1. Copy this sheet, filled in, to `docs/bench-sessions/<date>-vehicle.md`,
   with the session logs and screenshots in a folder beside it, as
   [2026-10-25-vehicle](2026-10-25-vehicle.md) has them; the
   recordings stay on the laptop
   ([CONTRIBUTING.md, "Handling vehicle data"](../../CONTRIBUTING.md#handling-vehicle-data)).
   Before committing,
   scrub the sheet, the logs and anything else committed: no VIN,
   calibration IDs or ECU serial (the identity line, `49 02`, `49 04`
   and `62 F1 xx` replies, Phase 3's scan and text DID), no names or
   initials, and no local paths (`%TEMP%` and `C:\Users\…` carry the
   account name). A recording that becomes a replay fixture keeps only
   its `frames.csv`, with every row whose data starts `49 ` or `62 F1`
   removed
2. Walk the [BENCH prep list](bench-prep.md): mark each
   item CONFIRMED / REFUTED (with evidence) / STILL UNKNOWN
3. Update STATUS.md, the testing.md Vehicle tier and gaps, and any open
   item the session settles
4. Dated appendices where the session measured something:
   [ADR-0015](../adr/0015-request-engine.md) (request-to-reply times,
   any "response pending" `7F 01 78`, the bitmaps),
   [ADR-0020](../adr/0020-uds-generic.md) (the scan's duration and
   pace, the refusal codes, the longest reply) and
   [ADR-0021](../adr/0021-vehicle-identification.md) (which items this
   ECU answered, refused or left unanswered, by name, never their
   values)
5. Engineering-log entry if a lesson emerged
