# VEHICLE run sheet — read-only vehicle session

**QtTune build:** <commit hash>  **Trees:** build, build-win32 both fresh
**Date:** <date>
**Adapter:** OBDX Pro VX (USB)  **Vehicle:** <year make model>; CAN-era OBD-II, expected 11-bit IDs at 500 kbit/s

Fill in a copy of this sheet as `docs/bench-sessions/<date>.md`. The
no-vehicle BENCH tier is [bench-runsheet.md](bench-runsheet.md); this
sheet picks up at its stop point. Two sessions have run this sheet, on
the same car: [2026-10-22](2026-10-22-vehicle.md) and
[2026-10-25](2026-10-25-vehicle.md), the first with the probe,
identification and `uds-generic`. Where an expectation below depends
on the vehicle, it's that car's.

## Purpose

A read-only session on a vehicle's bus. Goals, in order:

1. Requests leave the adapter and an ECU answers them (BENCH prep #2,
   request framing)
2. Replies decode to plausible values through the `obd2` profile
3. Link health behaves as designed on a real bus, ignition off and on
4. Check the talking-bus items in the
   [BENCH prep list](../../STATUS.md#bench-prep) (#2–#8) on this
   vehicle and adapter
5. Record each phase: the recordings are real-bus replay fixtures
   ([ADR-0017](../adr/0017-session-recording.md)), and the request
   rate and reply timings set the request engine's floor, chosen
   without a vehicle run
   ([ADR-0015](../adr/0015-request-engine.md), appendix of
   2026-10-23)
6. The ECU's identity (Phase 3) and its first DIDs through
   `uds-generic` (Phase 3b): which identification items and DIDs it
   answers in its default session, how it refuses the rest, how long
   a range scan takes, and whether its longer replies arrive whole
   ([ADR-0020](../adr/0020-uds-generic.md),
   [ADR-0021](../adr/0021-vehicle-identification.md)). Nothing in
   those two has run on a car

Record everything, including every connect's session log. A session
that stops at phase 2 with evidence is worth more than one that runs
to the end undocumented.

## Safety and scope

- **Read-only.** QtTune sends only read requests: Service 01 ("show
  current data"), Service 09 (vehicle information: VIN, calibration
  IDs) and Service 0x22 (ReadDataByIdentifier), the same read-only
  services a generic scanner uses. It has no code path that writes,
  clears codes, changes a diagnostic session or flashes. Read
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

Run every item on the laptop that goes to the car, even when it isn't
the machine the code was developed on.

- [ ] The VX driver is installed on that laptop: the picker lists only
      adapters whose drivers are installed
- [ ] Fresh build of both trees, **Win32 first**
      ([development.md, "Build configurations"](../development.md#build-configurations)
      has the commands), and the app build prints "Deploying Win32
      J2534 surrogate next to qttune.exe" (see
      [bench-runsheet.md](bench-runsheet.md) pre-flight)
- [ ] Full suite green: every case passes except the env-gated skip
      (current count in [STATUS.md](../../STATUS.md)), plus the
      cross-arch subset. `scripts/verify.sh` runs both and prints four
      `ok` lines, the last reading `0 skipped`. On Windows it also
      demands that discovery finds a device, which the VX's driver
      provides (`--no-devices` on a laptop without one). Run by hand,
      the cross-arch subset needs `ECUIO_SURROGATE_EXE` and
      `ECUIO_FAKE_DLL`; under any other names its routing case skips
      while everything else passes
      ([development.md](../development.md#running-tests))
- [ ] The same build passes the quiet-bus BENCH run with the VX
      ([bench-runsheet.md](bench-runsheet.md) Phases 1–3): it loads
      through the surrogate, opens, the filter is accepted, requests
      fail on the no-ACK bus, and teardown is clean. There's no ECU
      simulator, so this is the part of the risk the desk can retire.
      The 2026-10-22 run did
      this for `0a8beae` ([2026-10-22](2026-10-22.md)); a later build
      needs its own. On a laptop that hasn't met the VX, connect the
      bare URI once with the VX on USB and see "Requests failing", so
      its driver install, surrogate copy and security software meet
      the adapter at the desk, not at the car
- [ ] The laptop's settings for the day: on battery (see Safety), sleep
      set to never, closing the lid set to do nothing, USB selective
      suspend off (Power Options, advanced settings), and Windows
      Update paused
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
        ignition": writes fail while reads are clean, because nothing
        on the bus acknowledges (on the first vehicle, ignition off
        still acknowledged and read as quiet);
      - red "Adapter fault": reads fail.
      The Settings activity log records each transition; the header
      pill, Home and the Dashboard header show the same state in short
      form.
- [ ] Settings picker lists the VX as "x86". Every connect below goes
      through Settings, the only page that connects. The VX row fills
      the bare URI, the Profile row adds the phase's profile (`raw`
      adds `filter=0x7E8` on an adapter) and the DIDs field its
      `did=`; `poll=` is typed. A URI typed or pasted whole works the
      same, and the row and the field follow it. Then Connect
- [ ] Camera or notebook ready
- [ ] A reference for the values: the cluster's gauges (tachometer,
      coolant, fuel), and a second scan tool if you have one (a phone
      app with a Bluetooth ELM327 adapter will do). Use the
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
        canned value, and the activity log gets the fake's identity
        line (`VIN FAKEJ2534VIN00001; CALID FAKECAL-01; …; 2 refused`);
      - Phase 3b: `?profile=uds-generic&did=1E18-1E20,F190:text`:
        the ECU line reads `2 requests, none dropped; scan: 9 DIDs,
        1 found, 8 refused` (the polled F190 and the found 1E1C),
        Live shows a text row for F190 (the fake's VIN), a hex row
        `DID 1E1C` reading `12 34`, and a "—" row for each of the
        eight refused DIDs.

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

**Expected** (as on the first vehicle,
[2026-10-22](2026-10-22-vehicle.md)):
- [ ] Connect succeeds, the filter is accepted (session log
      `OP_START_FILTER … status 0`)
- [ ] Copy the activity log's "Session:" line into the sheet: the
      battery reading with the ignition off, and the adapter's versions.
      A reading near the battery's (about 12 V to 13 V with the engine
      off) confirms `READ_VBATT` in millivolts (BENCH prep item 10)
- [ ] Writes succeed (`OP_WRITE frame: status 0`) but nothing answers,
      because the bus still acknowledges frames with the ignition off.
      The link goes quiet ("Vehicle not responding") after about 1 s.
      On a bus that's fully asleep, writes fail instead: three succeed,
      the fourth blocks about 0.5 s and fails (`status 7`), and the
      link latches "Requests failing", as on the no-vehicle bench
      ([2026-10-22](2026-10-22.md)). Record which
- [ ] Disconnect cleanly

Either state is correct with no ECU awake. Don't debug it. If an ECU
does answer with the ignition off (some stay awake briefly after
key-off), record that instead.

**Evidence:** the session log; the UI state; whether writes failed,
and after how many.

## Phase 1 — ignition on, engine off, listen only

**Action:** ignition **on**, engine off. Connect without requests:

    j2534://OBDX Pro - VX?profile=raw&filter=0x7E8

**Expected:**
- [ ] Connect succeeds on a powered bus
- [ ] No writes in the session log (passive mode)
- [ ] Usually no frames: ECUs don't send on 0x7E8 unasked. The link
      goes quiet ("Vehicle not responding") after about 1.4 s (on the
      VX each empty read waits out its 100 ms timeout), which is
      correct here

**Evidence:** quiet or unsolicited frames, and if frames, their IDs
and payloads verbatim (the session log's `OP_READ frame` lines).

## Phase 2 — one PID

**Action:** same ignition state. Connect:

    j2534://OBDX Pro - VX?poll=0x0C

(the `obd2` profile stays in force, so the reply decodes; only PID 0x0C
is requested, the next as soon as the reply is in and no sooner than
20 ms after the last, so up to about 50 a second)

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
bytes, statuses, RxStatus). The `rx 0x00000009` line after each write
doesn't show the request reached an ECU: the VX returns one for frames
nothing acknowledged ([2026-10-22](2026-10-22.md)). Only a reply does.
To compare a few payloads on screen,
Pause holds the Log page's rows still; the recording keeps every
frame either way.

## Phase 3 — the full rotation

**Action:** same ignition state. Connect with the bare URI:

    j2534://OBDX Pro - VX

**Expected:**
- [ ] The probe first: the first writes are the bitmap requests
      (`01 00`, `01 20`, `01 40`, and on while each bitmap's last bit
      is set), then the rotation. The activity log gets one line for
      the engine ECU, `ECU 0x7E8: supports N PIDs; 10 requests, …`,
      naming each PID dropped. Against the first vehicle's bitmaps
      that's `1 dropped: 01 5C (not in its supported PIDs)`, and N is
      the data PIDs of every bitmap read, 44 for the three Phase 2
      reads plus those of the next ones
- [ ] Within a few seconds the activity log gets the identity line,
      `ECU 0x7E8 identity: VIN …; CALID …; …`, with the car's VIN and
      calibration IDs and whichever `F1xx` DIDs the ECU answers; note
      which it refused (`7F 22 31`) or left unanswered. The line is
      personal data: it is scrubbed before this sheet is committed
- [ ] Identification's replies are the first segmented ISO 15765
      messages a vehicle has sent this project: the VIN (`49 02 01`
      plus 17 bytes, and `62 F1 90`) and the calibration ID (16
      bytes) are longer than 7 data bytes. The VX reassembles them, so
      the frame's DLC is the whole length (`len` in the session log's
      `OP_READ frame` line, minus the 4-byte CAN ID). Record the
      longest reply's DLC and whether any frame carries the truncated
      flag (it shouldn't: the frame holds 4 KB)
- [ ] Every `obd2` signal populates within about one rotation (a
      request every 20 ms or so while the ECU answers within that, so N
      PIDs take about N × 20 ms; well under a second for the current
      set); a PID the probe dropped is never asked and stays "—"
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
      its colors, with dashed chart traces

**Evidence:** a screenshot of LiveView, and a minute or more of
recording with `ECUIO_SUR_LOG_FRAMES=1`: each signal's refresh interval
comes from its `host_us` in `signals.csv` (expect about one rotation),
and `frames.csv`'s `timestamp_us` against `host_us` gives the vendor
timestamp's unit (BENCH prep #5). This is the request engine's first
run on a vehicle, so the session log's `OP_WRITE frame` and
`OP_READ frame` lines are the measurement: requests a second, each
reply's time after its request, and whether every reply comes back
before the next request (a reply after it means the 100 ms wait ran
out first). The probe's bitmap replies are in the same lines: they
should match Phase 2's, byte for byte.

## Phase 3b — the first DIDs through `uds-generic`

Only if Phase 3 was clean. Same ignition state, engine off. Two
connects: a scan of the standard identification block, then one DID
it found, read as text. Service 0x22 is read-only, and every request
goes to the engine ECU alone (0x7E0, replies on 0x7E8).

**Action, the scan.** Connect (the VX row, the `uds-generic (DIDs)`
button, then `F180-F1FF` in the DIDs field):

    j2534://OBDX Pro - VX?profile=uds-generic&did=F180-F1FF

That's 128 DIDs in ISO 14229's identification range, asked once each,
nothing polled until one answers. With nothing polled, identification
goes first, every item in list order (`09 02`, `09 04`, … then its
`22 F1 xx` items), and only then the scan, from `22 F1 80` up. Once a
DID is found it joins the polling, and from then on each scanned DID
takes turns with one polled request.

**Expected:**
- [ ] Connect succeeds; no bitmap requests (there are no Service 01
      requests to probe for), no banner
- [ ] The activity log gets ECU lines as the counts move (up to one
      per 100 ms), the last with no "unanswered":
      `ECU 0x7E8: k requests, none dropped; scan: 128 DIDs, k found,
      r refused` (k and r add up to 128; the k found DIDs are now the
      polled requests), and the identity line as in Phase 3. Time the
      scan from `Connected:` to that last line. The requests number the
      identification items plus the 128 DIDs, and up to 128 more polled
      requests once an early DID is found, so the run takes at most
      about (items + 256) requests. At about one request every 20–50 ms
      that's under about 15 s, even with the list's room for 16 items
      filled. Longer means the ECU answers slowly or left DIDs
      unanswered (each costs the 100 ms wait); note which
- [ ] Live lists a row for every DID in the range from connect,
      `DID F180` to `DID F1FF`, each "—" until it answers. A found
      DID's row shows its bytes as hex pairs, bright and refreshing
      once it joins the polling; a refused DID's row stays "—". The
      found rows are personal data (VIN, serials): don't photograph
      Live in this phase
- [ ] The Log page shows the replies as they arrive: `62 F1 xx` plus
      the data. Record the longest reply's DLC and whether any frame
      carries the truncated flag, as in Phase 3
- [ ] How the ECU refuses: the session log's `OP_READ frame` lines
      with `7F 22 <NRC>`. Expected for DIDs it doesn't have:
      `7F 22 31` (request out of range), counted as refused. Record
      any other code verbatim. `7F 22 78` (response pending) is
      waited out, up to 5 s per request. `7F 22 11` (service not
      supported) to a polled DID drops every polled Service 0x22
      request on this ECU by design; the scan and identification
      still ask each of theirs once, and each is refused. `7F 22 33`
      (security access denied) refuses that one DID. Either of those
      is a finding, not a fault

**Action, one DID as text.** Disconnect. Pick a DID the identity line
shows answered whose bytes read as ASCII in the Log page and isn't a
serial or the VIN: F191 (the hardware number), F189 (the software
version) or F195 (the supplier software version) where the ECU
answers them. On the 2026-10-25 car only F188, the manufacturer's
software number, qualified; it refused the other three. A refused DID
drops at its first reply and, with nothing left to ask, the link goes
quiet about a second later, by design. Connect with it polled as
text, for example:

    j2534://OBDX Pro - VX?profile=uds-generic&did=F188:text

**Expected:**
- [ ] One Live row, `DID F188`, showing the text, refreshing on every
      reply (a text row's plot box is grayed out); `signals.csv` carries the
      text, quoted
- [ ] No banner, the link ok, the value never dims

**Evidence:** the recording (`frames.csv` for the reply lengths and
timings, `signals.csv` for the hex and text values, `events.log` for
the ECU and identity lines), and the session log's `OP_WRITE frame`
and `OP_READ frame` lines: the request order, each reply's time after
its request, the NRC of every refusal, and the longest reply. All of
it carries the VIN and the ECU's serial, so it's scrubbed before any
of it is committed (see Post-session actions).

## Phase 4 — engine idling (the decode check)

Only if phases 0–3 were clean.
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
- [ ] Ignition off: values dim after 3 s and the link goes quiet, as
      in Phase 0; on a bus that sleeps, "Requests failing" at the first
      failed write instead
- [ ] Ignition on: values resume and the activity log shows the link
      recovering. From quiet, the next request is at most the 100 ms
      wait away.
      While requests fail, the worker waits between probes, doubling
      from 100 ms to a 5 s cap, so after a 10 s outage the next probe
      can be up to 5 s away, and the ECU needs a moment to wake. Record
      the time from ignition-on to the first value; longer than ~10 s
      is a finding
- [ ] Optional, the probe on a cold connect: disconnect, connect with
      the ignition off, then turn it on. The ECU line shows "no answer
      to the supported-PID query" at first; once the link is ok the
      walk resumes and a second line names what the ECU supports, as
      in Phase 3. Silence must drop nothing: before ignition-on, the
      first line says no request was dropped

## If it fails, suspect

| Symptom | Suspect |
|---|---|
| Connect fails at once, before any write | The Settings error line names the cause: "surrogate process failed" (no surrogate next to `qttune.exe`, the pre-flight; or one from a stale `build-win32`: rebuild both trees), "failed to load transport library" (the DLL not loading), or "the adapter's driver refused: PassThruOpen returned status 34" (the adapter isn't attached; the VT and VX both answer 34). Check the URI names the VX |
| Writes fail with the ignition on (`OP_WRITE: status` nonzero) | BENCH prep #2, request framing (6-byte logical message with `ISO15765_FRAME_PAD`); then baud (500 k assumed) or a 29-bit vehicle (prep #4). The bare URI is 11-bit only. 29-bit IDs given with `?filter=`/`?txid=` connect with `CAN_29BIT_ID` automatically, but no 29-bit OBD IDs are prepared: record and stop rather than improvise |
| Writes succeed but no frames arrive | The ECU doesn't answer physical 0x7E0: retry Phase 2 with `?poll=0x0C&txid=0x7DF` (functional request, replies still on 0x7E8); then the flow-control filter (prep #1) |
| `0x7E8` frames on the Log page, but LiveView shows "—" everywhere | Payload shape: stamping already proves the channel matched, so suspect a PCI byte left in-band shifting `[41][PID]`. The `OP_READ frame` lines show the bytes as the vendor returned them |
| Values implausible (−40 °C, huge RPM) | Scaling or byte order: temperatures are unsigned 8-bit with a −40 offset; 2-byte PIDs are big-endian |
| Link flips between ok and quiet while frames arrive | Unmatched frames interleaving with replies: a single unmatched frame reads as quiet with no debounce, by design. Note the frame IDs |
| Slow refresh, replies a cycle late | Known refresh-rate item (20 ms first read vs up to 50 ms ECU response). Measure; don't fix in the session |
| Phase 3b finds nothing: every DID `7F 22 31` | The ECU keeps its identification DIDs for another diagnostic session, or uses another range. The identity line says which of its `22 F1 xx` items answered in Phase 3, and every one is inside the scanned range; if those answered there, the scan should find them too, so compare. With nothing found there is nothing left to ask, so about a second after the last refusal the link turns quiet (amber "Vehicle not responding") by design, though the ECU answered every DID. Record and move on: QtTune never changes the session |
| Phase 3b: every Service 0x22 reply is `7F 22 11` | The ECU says it doesn't serve Service 0x22 here. Each identification item and scanned DID is still asked once and refused, so the ECU line ends `scan: 128 DIDs, 0 found, 128 refused` and the identity line lists every `22 F1 xx` item as refused; a polled DID would leave the rotation, with every other polled Service 0x22 request on that channel. With nothing found and nothing polled, the link turns quiet about a second after the last refusal, by design, as in the row above. Record it; identification by Service 09 still works |
| The identity line never arrives | An item left unanswered holds the line until the link recovers and the re-ask settles. Check the session log for a `22 F1 xx` or `09 xx` request with no reply, note which, and move on: the ECU and support lines don't wait for it |
| "Disconnecting…" lasts seconds | A wedged adapter. The teardown runs off the GUI thread and gives up after about 6 s; Connect stays disabled until it ends. Note how long (the activity log's `Disconnecting:` and `Disconnected:` lines bracket it) and whether the session log says `child unresponsive`. The window itself shouldn't freeze on Disconnect; if it does, that's a bug: capture the session log |
| App crashes | Abort. Capture the session log and `sur.log` before relaunching |

## Evidence against the BENCH prep list

| Prep item | What settles it |
|---|---|
| #2 Request framing | Phase 2 replies arrive |
| #3 No checksum byte | Replies decode without a trailing byte |
| #4 29-bit addressing | 11-bit IDs expected; any 29-bit traffic is news |
| #5 Timestamp units | Microseconds on the VX's TX indications ([2026-10-22](2026-10-22.md)). Phase 3 checks replies: in `frames.csv`, `timestamp_us` deltas against `host_us` deltas. The clock doesn't reset at connect, and wraps 71.6 min after it starts |
| #6 ReadMsgs statuses | Session log: does any `OP_READ frame: status 9` line appear (`ERR_TIMEOUT` with data)? On the bench, short batches came back with status 0 and empty reads with 16 |
| #7 Struct layout | A correct decode in phases 2–3 implies the layout is right |
| #8 Vendor timeouts | Read timing visible in the session log stamps |

Beyond the prep list, this session is the first vehicle run for the
request engine's pacing (Phase 3), the probe (Phase 3),
identification (Phase 3), a segmented reply (Phase 3's identification)
and `uds-generic`'s polling and scan (Phase 3b), each a gap in
[testing.md](../testing.md#gaps-not-verified).

## Teardown

1. Disconnect in the app and wait for `Disconnected:` in the activity
   log (the teardown ends there); the session log shows
   `OP_DISCONNECT: vendor status 0` exactly once
2. Copy this session's recording folders (the Activity log named
   each) and its session logs: `%TEMP%\ecuio-surrogate.log` and the
   earlier ones kept beside it, this session's by their stamps. Copy
   them before the test suite or `scripts/verify.sh` runs on that
   laptop: a run spawns dozens of surrogates, and the newest-50 cap
   would delete this session's logs
3. Unplug the adapter, then key off

## Post-session actions

1. Copy this sheet, filled in, to `docs/bench-sessions/<date>.md`, with
   the session logs and recordings in a folder beside it, as
   [2026-10-22-vehicle](2026-10-22-vehicle.md) has them. Before committing,
   scrub the sheet, the logs, the recordings' `events.log` (it holds
   the URI and the identity line), the `frames.csv` of every connect
   without `?poll=` (identification runs on each, in Phases 3 to 5,
   and its `49 02`, `49 04` and `62 F1 xx` replies carry the VIN,
   calibration IDs and the ECU's serial), Phase 3b's `signals.csv`
   (the DIDs' bytes and text) and any screenshots of the activity log
   or of Live in Phase 3b: no VIN, calibration IDs or ECU serial, no names or
   initials, and no local paths (`%TEMP%` and `C:\Users\…` paths
   carry the account name)
2. Walk the [BENCH prep list](../../STATUS.md#bench-prep): mark each
   item CONFIRMED / REFUTED (with evidence) / STILL UNKNOWN
3. Update STATUS.md, the testing.md Vehicle tier and gaps, and any
   open item the session settles
4. While [ADR-0015](../adr/0015-request-engine.md)'s values are open,
   add the session's measurements to it as a dated appendix:
   request-to-reply times per PID (from the `OP_WRITE frame` /
   `OP_READ frame` stamps), whether any reply was "response pending"
   (`7F 01 78`), and Phase 2's supported-PID bitmaps
5. Add a dated appendix to [ADR-0020](../adr/0020-uds-generic.md)
   with Phase 3b's measurements (the scan's duration and pace, the
   refusal codes seen, the longest reply) and one to
   [ADR-0021](../adr/0021-vehicle-identification.md) saying which
   items this ECU answered, refused or left unanswered in its default
   session (the items by name, never their values)
6. Engineering-log entry if a lesson emerged
