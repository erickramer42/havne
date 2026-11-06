# VEHICLE session: a vehicle's own signals on the test car

**Date:** 2026-11-06, 11:33-12:25
**Plan:** v0.5.0's own signals read from a real ECU: a signal written in
Settings' form and checked with **Read once**, a signal file with two
DIDs read as hex, a DID the module refuses, an enumeration and a
computed signal, in the laptop's app and `havne-cli` through the VX and
in the phone's app through the MX+; then a recording replayed through
its own copy of the file, and the file saved out of the phone with
**Save a copy**. Then three runs of one `havne-cli` session through the
VX (A1-A3) to narrow down the second module's unanswered requests.
**Adapters:**
- the OBDX Pro VX on the laptop's USB, J2534 through the x86 surrogate
  (DLL 1.8.9586.12845, API 04.04, FW:1.0.1.0 HW:1.0.0.0);
- the OBDLink MX+ (STN2255 v5.12.2) over Bluetooth on the test phone
  (Android 16), `elm327://bt:<BT address>`.

The adapters shared the test car's OBD port through an OBD Y-splitter.
**Vehicle:** the test car, engine idling throughout; CAN OBD-II, 11-bit
IDs at 500 kbit/s, the engine ECU on `0x7E0`/`0x7E8` and a second ECU on
`0x7E1`/`0x7E9` ([2026-11-04-two-ecu-car](2026-11-04-two-ecu-car.md)).
**Havne build:**
- the laptop: `dev` at fb3510a (core 0.5.0), `build-win32` then `build`,
  `havne-cli` warmed up on the fake first; the antivirus off for the run;
- the test phone: a local debug build of `dev` at 18fa9ab with the
  chart's GPU traces (a448a92), whose signal files, refused rows and
  **Save a copy** are fb3510a's.

**Evidence:** stays on the desktop; the figures below are read from it:
the recordings, `havne-cli`'s output, the surrogate's session logs with
the frame log on, the phone's recordings and screenshots. The
identification replies and the MX+'s address are in the raw files and
nothing of them is here. Every request was a read; nothing was cleared.

**The file:** `car-run.json`, written for the run (profile `user.car-run`,
extending `obd2`, two modules):

| Signal | Read | Kind |
|---|---|---|
| PCM DA00 | 22 DA00 on 0x7E8, the whole reply | hex |
| TCM DA03 | 22 DA03 on 0x7E9, the whole reply | hex |
| TCM DC00 | 22 DC00 on 0x7E9, refused in 2026-11-04's scan | number |
| Standstill | 01 0D, `0` labeled "Stopped" | enum |
| Boost | `obd2.map - obd2.baro`, baro's age bound 30 s | computed, kPa |

What DA00 and DA03 hold isn't known, so they're read as bytes, not
given a scale.

| Step | What ran | Result |
|---|---|---|
| 1 | The form and **Read once** (the laptop's app, the VX) | PASS |
| 2 | The file in a recorded session (the laptop's app, the VX), about 4 min | PASS, with the second module's unanswered requests (below) |
| 2b | The vehicle's own file (the form's signal) in a session, 72 s | PASS |
| 3 | The file through `havne-cli`, 30 s, recording | PASS, the same unanswered requests |
| 4 | The file in the phone's app (the MX+), recording; **Save a copy** | PASS |
| 5 | A DID scan, D800-DBFF on 0x7E8 (BENCH prep #17) | No "response pending" reply |
| After | `havne-cli info` on each recording; a replay through its own copy, in the app and `havne-cli` | PASS, a refusal not shown on replay |
| A1-A3 | One `havne-cli` session three ways, 60 s each, the frame log on | The VX loses 0x7E9 replies during 0x7E8's segmented ones |

## 1: the form and Read once

A vehicle with no VIN, one signal in **Your own signals**: 0x7E8,
service 22, DID DA00, byte 0, 8 bits, scale 1. **Read once** showed
"Asked 22 DA 00, got 62 DA 00 00: 0", the reply's bytes beside the
draft's value. **Save** wrote it to the vehicle's own file
(`user.weekend-car-own`), and the file above was imported beside it
("now reading obd2 + Car run").

## 2: the file in the laptop's app

- The session asked both modules, 0x7E8 through 0x7E0 and 0x7E9 through
  0x7E1. Delivery 13,214 frames, none dropped.
- PCM DA00 read `00` (1,003 samples) and TCM DA03 `00` (4,108), both
  steady.
- TCM DC00 read **"Refused: requestOutOfRange (7F 22 31)"**, its row kept
  on Live; the activity log's ECU line named it: "ECU 0x7E9: 2 requests,
  1 dropped: 22 DC 00 (not supported: 7F 22 31)".
- Standstill read **"Stopped"** in all 1,003 values (the speed 0).
- Boost: at idle a median of **-60 kPa** (-56 to -70; MAP 39 kPa, baro
  99 kPa); a blip of the throttle in Park to **-45 kPa** (MAP 54 kPa) at
  3,236 rpm; -85 kPa as the engine came back down.
- Reply timing: 46 requests, 48,246 answered, 1 refused, **1,154 timed
  out**; latency P50 4.6 ms to 20 ms. Every timeout was TCM DA03's
  (1,154 of its 5,262 requests), steady through the session.
- 2b: the form's signal alone, 72 s: 2,518 values, all 0, which is PCM
  DA00's first byte; 1 request, 2,519 answered, none timed out.

## 3: `havne-cli`

`havne-cli stream "j2534://OBDX Pro - VX" --signals car-run.json --values
--seconds 30 --record <dir>`: ended at its `--seconds`, 1,716 frames,
none dropped. One `refused` line: TCM DC00's `22 DC 00`, the reply `7F 22
31`, `requestOutOfRange`. Standstill's label "Stopped" in all 75 values;
Boost 149 values, -61 to -57 kPa. TCM DA03: 590 answered, **117 timed
out**.

## 4: the phone

The file imported from the phone's Downloads through Settings' **Import a
signal file**; a session through the MX+, 83 s, recording. Live showed
the same rows: PCM DA00 `00`, TCM DA03 `00`, TCM DC00 refused, Standstill
"Stopped", Boost -8.4 psi. Delivery 2,381 frames, none dropped; reply
timing 46 requests, 6,705 answered, 1 refused, **none timed out**,
latency P50 14-31 ms. On the phone's width the refused row's name and
words were both cut short ("TC...", "Refused: requestOutOfRang...").

**Save a copy** wrote the file to the phone's Downloads through Android's
document picker; the copy passed `havne-cli signals check`, and its
SHA-256 equals the imported file's.

## 5: the DID scan

`?profile=uds-generic&scan=first&identify=0&diagnose=0&did=D800-DBFF`
through the VX with the frame log on: all 1,024 asked, 46 found, 978
refused. No reply was `7F 22 78`: BENCH prep #17 still needs an ECU that
sends it.

## After the run

- `havne-cli info` on all seven recordings (the laptop's four, the one
  `havne-cli` made, the phone's two): no torn row, no record dropped.
- **A replay through its own copy:** with the file removed from Settings,
  step 2's recording replayed in the app: "Replay: decoding with the
  recording's own signal file, Car run (rec.c32dac389ff4f53e)", and Live
  showed the same values. `havne-cli stream "replay://<folder>?speed=max"
  --values` with no `--signals` gave the same counts as the live session
  for each signal (PCM DA00 1,003, TCM DA03 4,108, Standstill 1,003,
  Boost 2,005). TCM DC00 read "—" in the app's replay, and `havne-cli`'s
  replay wrote no `refused` line: a recording keeps the refusal only in
  the ECU line's words.
- **The vehicle at connect:** a second after step 2 started, the car's
  VIN picked the vehicle saved with it on an earlier run ("Vehicle:
  Vehicle 1 (picked; Weekend car was)"), as
  [ADR-0027](../adr/0027-vehicle-profiles.md) says a VIN does.
  The recording had opened under the vehicle picked before, so its
  folder is named for that one; the later recordings carry the picked
  vehicle's name.

## A1-A3: the second module's unanswered requests

| Run | What was asked | TCM DA03 answered | Timed out |
|---|---|---|---|
| A1 | `?profile=uds-generic&did=DA00,7E9:DA03`: one DID on each module, 2026-11-04's shape | 2,133 | **0** |
| A2 | `--signals car-run.json`: the file's signals and `obd2`'s on 0x7E8, packed | 906 | **298** |
| A3 | the same with `?pack=0` | 2,029 | **23** |

- In A2's frame log, every one of the 1,205 DA03 writes the VX accepted
  (status 0). 906 were answered (the reply 4 ms after the write at the
  median, 10 ms at 90 %, 52 ms at most) and **299 never were**: none came
  late.
- 297 of the 299 unanswered writes had a 0x7E8 segmented reply delivered
  within 30 ms; of the answered ones, 68 % had.
- A3 still had 55 0x7E8 segmented replies (single PIDs whose replies take
  more than one frame), and 22 of its 23 unanswered writes fell within
  30 ms of one of them.
- The MX+ answered every request of the same file in step 4.
- So, through the VX, a 0x7E9 single-frame reply that lands while a
  0x7E8 segmented reply is coming in is lost (STATUS Known Issue #42,
  BENCH prep #35). Packing makes segmented replies many; `?pack=0`
  leaves the few that a single PID's reply needs.

## Observations

- Every v0.5.0 feature the run read worked on a real ECU, in the app and
  in `havne-cli`, and on the phone through the MX+: a signal written in
  the form and **Read once**, a signal file's DIDs on two modules, a
  refused DID shown as refused, an enumeration and a computed signal.
- **Boost from two PIDs** reads as the car does at idle and on a blip.
  01 0B's range ends at 255 kPa, so a gauge reading of about 155 kPa
  over this car's baro is the highest boost it can give; a drive was
  not part of this run.
- **The Mark** was checked against a known press: `markers.csv`'s time fell
  inside the press's window, about a second before it ended.
