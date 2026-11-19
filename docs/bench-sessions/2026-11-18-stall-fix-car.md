# VEHICLE session: the Dashboard's values on a plugin's vehicles, through the MX+

**Date:** 2026-11-18, 18:26-18:50
**Plan:** the Dashboard stall fix on the car. The same morning the test
phone (v0.8.0) showed a plugin's four-channel vehicle with
its 0x7E8 values every 3.7-3.9 s and the engine's support line reading
`supports 19 PIDs up to 0x20, no answer past that`; the two-channel
vehicle on 2026-11-16 read `49 PIDs up to 0x80`, a value about every
2.36 s. This session is the after only, on the laptop's app: the same
core and ELM327 worker as the phone's, the adapter over Bluetooth as on
the phone. The phone's own check rides on v0.9.0's release APK.
**Adapter:** the OBDLink MX+ (ELM327 v1.4b, STN2255 v5.12.2) over
Bluetooth as `COM5` at 38400 bit/s, alone in the OBD port; the test
phone's Bluetooth off.
**Vehicle:** the test car, parked, the selector in P, the engine idling
(13.6-13.7 V at the adapter); CAN OBD-II, 11-bit IDs at 500 kbit/s.
**Havne build:** the laptop on Windows: `dev` at f513915 (core 0.8.0 by
its `start` line), `build-win32` then `build`, Release. The app with
Settings, Recording, Record sessions on. A1a was launched with
`ECUIO_SUR_LOG_FRAMES=1`, A1b, A1c and A2 without it; the recording
turned the frame log on for those three too (their `events.log`:
`surrogate frame logging on, set by this recording`), so all four wire
logs hold every exchange.
**The app's setup:** a plugin's four-channel vehicle
(0x7E8, 0x7E9, 0x769, 0x72E) and two-channel vehicle (0x7E8, 0x7E9),
and one Dashboard layout for both: three pages, 30 gauges, the set the
phone asked that morning (19 `obd2` PIDs, 0x5C among them, plus the
plugin's DIDs).
**Evidence:** in [2026-11-18-stall-fix-car/](2026-11-18-stall-fix-car/):
each recording's notes (`events-A1a.log` to `events-A2.log`, the
identity and VIN lines reduced to placeholders, the plugin's profiles
and the vehicle labels as `<4ch plugin profile>`, `<4ch vehicle>` and
so on), and A1a's wire log from its start, its support walk and one lap
after 10 s (`A1a-walk-and-lap.log`). The plugin's DIDs are written
`<DID>`. The recordings' `frames.csv` and `signals.csv` and the full
wire logs stay on the desktop: they hold the identification replies.

Reads only: Service 01, Service 22 and the connect's identification and
diagnostics reads.

| Run | Vehicle | Session (`events.log`) | Battery | 0x7E8 support line | After 10 s: worst median / worst max | Whole run: worst max | Reply timing |
|---|---|---|---|---|---|---|---|
| A1a | four-channel, launched with the frame log | 18:26:36-18:30:22 | 13.7 V | `supports 53 PIDs`, several PIDs per request | 0.304 / 0.585 s, 27 signals | 2.778 s (PID 0x04, the connect) | 18052 answered, none refused, none timed out; P50 12-25 ms |
| A1b | four-channel | 18:32:22-18:37:46 | 13.7 V | the same | 0.300 / 0.569 s, 27 signals | 2.580 s (0x04) | 26425 answered, none refused, none timed out |
| A1c | four-channel | 18:40:05-18:44:41 | 13.6 V | the same | 0.300 / 0.570 s, 27 signals | 2.723 s (0x04) | 22412 answered, none refused, none timed out |
| A2 | two-channel | 18:46:51-18:50:03 | 13.6 V | the same | 0.257 / 0.346 s, 25 signals | 1.845 s (0x04) | 16995 answered, none refused, none timed out |

The intervals are each signal's, from `signals.csv`: after the first
10 s (the connect, the walk and identification), and over the whole run
for the worst first gap.

## What passed

- **The walk reaches the chain's end on every connect.** Each 0x7E8
  support line reads `supports 53 PIDs` with no "up to". A1a's wire log
  holds the six bitmaps (`41 00` to `41 A0`), the last with no further
  bitmap flagged: 58 bits set, 53 PIDs once the five next-bitmap bits
  are taken out. The test car answers six bitmaps, as on
  [2026-10-31-elm327-vehicle](2026-10-31-elm327-vehicle.md).
- **The 0x7E8 values pack:** the line ends "several PIDs per request",
  and from the walk on A1a's wire log asks up to six PIDs a request
  (`01 04 05 0C 0D 0F 11`, `01 42 10 06 07 0B 0E`, `01 33 44 46 49 68`),
  three packed requests a lap.
- **Each 0x7E8 value under 3 s after 10 s, on all four runs:** the worst
  median 0.304 s and the worst max 0.585 s; none of the 27 (25 on A2) at
  or over 3 s. A2's medians are all 0.257 s or less, against 2026-11-16's
  2.36 s. The worst first gaps (PID 0x04, 1.8-2.8 s) are the connect's.
- **The TCM value once a lap:** its median 0.301, 0.299, 0.300 and
  0.255 s, the same as the 0x7E8 values' in each run.
- **No timeouts and no link alarm.** A1a's wire log has 10 `NO DATA`,
  each one of 0x769's diagnostics or identification reads, and no
  `STOPPED` or `BUFFER FULL`; A1b's and A1c's logs the same, A2's (no
  0x769 channel) no `NO DATA` at all.
- **`AT SH` once a lap per channel:** 725 for each of the four headers
  (7E0, 7E1, 761, 726) over the run's 721 laps, so four switches a lap,
  not one for each of the TCM's asks.
- **The other two modules every lap:** 0x769 and 0x72E answered the
  plugin's DIDs at the lap's pace (median about 0.30 s) in A1a-c. That
  morning on the phone both gave only `7F 22 31`. Their diagnostics
  reads stay as known: 0x769 unanswered, 0x72E refused (`7F 03 11`,
  `7F 07 11`, `7F 0A 11`, `7F 01 11`, `7F 02 11`).
- A1a's wire log's 28 `7F 22 31` are all the connect's `22 F1xx`
  identification reads; none is a polled DID's. The 4-5 ISO-TP replies
  dropped in each run are the connect's segmented reads asked again,
  not losses.

## Flagged

- **An early "no answer" line, replaced 0.3 s later (A1a only).** At
  +0.412 s the log read `ECU 0x7E8: no answer to the supported-PID
  query; 97 requests, none dropped`, and at +0.714 s `ECU 0x7E8:
  supports 53 PIDs; ...`. In the wire log the walk's first `01 00` got
  the single frame `41 05 46`, a coolant temperature where the bitmap
  belonged, though nothing before it had asked for PID 0x05; the walk
  asked `01 00` again 0.41 s later and the bitmaps came. A1b, A1c and A2
  printed only the "supports 53 PIDs" lines. A user reads the first line
  as a fault.
- **The vehicle picked at the connect changes by the VIN (A2).** A2 was
  connected with the two-channel vehicle picked, and its log reads
  `Vehicle: <4ch vehicle> (picked; <2ch vehicle> was)` 0.4 s in: A1a had
  marked the four-channel vehicle recognized by this VIN. The session
  kept the two-channel plugin's profile (its channels 0x7E8 and 0x7E9
  only).
- **"Default" on an adapter's address is plain `obd2`, not the
  vehicle's plugin.** Found in the app's Settings without a connect:
  with the four-channel vehicle picked and the address `elm327://COM5`,
  the profile row offered Default, the plugin's profile, `raw` and
  `uds-generic`, with Default chosen, and Default on an adapter is
  `obd2`. Only picking the plugin's profile forms
  `elm327://COM5?profile=<4ch plugin profile>`. The Connection heading
  named the plugin right after it was picked in Signals ("5 found",
  then the plugin's name), and read "5 found · Default" after the
  vehicle was picked again. A user who picks a plugin's vehicle and connects on
  Default reads `obd2` without the plugin's signals.
- A2's first two connects from the driving script went into the Vehicle
  section's label field (the section was open) and never connected; the
  third, after a relaunch, did. A tooling matter, not the app's.

Not run: A3 (the VX in ELM mode).
