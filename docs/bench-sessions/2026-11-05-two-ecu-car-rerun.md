# VEHICLE session: the VX's reply latency and the MX+ listening, again

**Date:** 2026-11-05, 10:37-11:04
**Run sheet:** [two-ecu-car-runsheet.md](two-ecu-car-runsheet.md), Phase 4b
again (the VX's latency, packed and unpacked) and Phase 4 again (the MX+
listening while the VX asks, BENCH prep #19), after
[2026-11-04-two-ecu-car](2026-11-04-two-ecu-car.md); then six more listening
runs (B2-B7), asked for at the car.
**Adapters:**
- the OBDX Pro VX on the laptop's USB, J2534 through the x86 surrogate
  (DLL 1.8.9586.12845, API 04.04, FW:1.0.1.0 HW:1.0.0.0);
- the OBDLink MX+ (STN2255 v5.12.2) over Bluetooth on the test phone
  (Android 16), `elm327://bt:<BT address>`.

The adapters shared the test car's OBD port through an OBD Y-splitter.
**Vehicle:** the test car, engine idling; CAN OBD-II, 11-bit IDs at
500 kbit/s, the engine ECU on `0x7E0`/`0x7E8`.
**Havne build:**
- the laptop: `dev` at b98240d (core 0.4.0), the x86 surrogate and
  `havne-cli` rebuilt before the run; the antivirus off for the run, so
  no first connect was held;
- the test phone: a local build of `dev` from earlier the same morning,
  before b98240d, whose core and app are the same as b98240d's.

**Evidence:** stays on the desktop; the figures below are read from it:
`havne-cli`'s output, the surrogate's session logs with the frame log on
(each vendor call's held time), the recordings, the phone's ELM327 wire
log, and B2-B7's monitor logs. The frames hold identification replies and
the wire logs the MX+'s address: nothing of either is here. The phone was
driven over adb, on the user's word.

| Step | What ran | Result |
|---|---|---|
| A | The VX alone (the MX+ idle on the splitter), `obd2` through `havne-cli`: 90 s packed, 60 s with `?pack=0` | The 50 ms class is gone; a call the surrogate makes after each reply holds about 10 ms |
| B | The MX+ listening in the phone's app (`raw`: `AT CFC0`, `AT CRA 7E8`, `AT MA`) while the VX asks, packed, 90 s | FAIL: First Frames and Single Frames, no Consecutive Frame |
| B2-B7 | The MX+'s monitor through the phone's relay ([ADR-0047](../adr/0047-adapter-relay.md)), set up by a script that only listens, each beside 60 s of the VX packed | No Consecutive Frame and no Flow Control under every setup tried; not the splitter |

## A: the VX's reply latency

| | 2026-11-04 | This run, packed (90 s) | This run, `?pack=0` (60 s) |
|---|---|---|---|
| Latency P50 per request, median (range) | 65.5-73.7 ms | 24.6 ms (24.6-30.7) | 14.3 ms (14.3-20.5) |
| Frame log: write to the reply's read, median | | 15.0 ms | 4.0 ms |
| Frame log: replies 35 ms or more after their write | 1,806 of 37,320 | 3 of 3,337 | 1 of 2,289 |
| The first vendor call of a read: held median / 90 % under / slowest | (not logged) | 0.2 / 5.0 / 10.1 ms (9,938 calls) | 0.1 / 5.5 / 10.0 ms (4,508 calls) |
| The call for the rest of a batch (asked to wait 1 ms): held median / 90 % under / slowest | (not logged) | 9.8 / 10.4 / 11.9 ms (3,337 calls, 12 with a message) | 9.7 / 10.3 / 11.5 ms (2,289 calls, 100 with a message) |

- Each run ended at its `--seconds` with no health line and no timeout:
  packed, 43 requests and 19,651 replies (none refused), 3,337 frames;
  unpacked, 43 requests and 2,263 replies, 2,289 frames.
- The 50 ms class of 2026-11-04 is gone: no read asks the VX to wait
  0 ms.
- The VX holds a read asked to wait 1 ms about 10 ms. The call for the
  rest of a batch follows every reply and rarely brings anything, so it
  sits on each reply's time: packed, about 15 ms of bus and delivery
  plus 10, the 24.6; unpacked, about 4 plus 10, the 14.3, the same as on
  2026-11-04.

## B: the MX+ listening while the VX asks (BENCH prep #19)

- **The VX:** `obd2`, packed, 10:43:12-10:44:42: ended at its seconds,
  3,229 frames, no health line, none dropped.
- **The MX+'s setup:** `AT Z` to `AT CRA 7E8` as the setup has it, `AT CAF1`
  and `AT CFC0` among them, every command answered `OK`; then `AT MA`,
  with one quiet restart before the VX asked.
- **Its wire log on `0x7E8`:** 63 Single Frames, 3,167 First Frames and no
  Consecutive Frame (against 63, 1,317 and 0 under `AT CFC1` on
  2026-11-04); no `BUFFER FULL`.
- **Delivered:** 63 frames, every one a Single Frame reply; none dropped.
- **Link health:** "vehicle not responding" before the VX asked,
  "recovered" at its first requests, then "vehicle not responding"
  again 4 s later and to the end, with only the Single Frame replies
  arriving.

`AT CFC0` reached the adapter and changed nothing.

## B2-B7: what the MX+'s monitor misses

A script on the laptop, through the phone's relay, sent each setup
itself, monitored for about 90 s, and counted the data lines; `AT CS`
(the adapter's CAN error counters) before and after B5-B7. Each VX
session beside it ended at its seconds with no health line and none
dropped (2,070 to 2,239 frames).

| Run | Setup | `0x7E0` | `0x7E8` |
|---|---|---|---|
| B2 | `AT CAF0`, `AT CRA 7E8` | (filtered) | 2,177 First Frames, no Consecutive Frame, 63 Single Frames |
| B3 | `AT CAF0`, no receive filter | 2,170 requests, no Flow Control | 2,107 First Frames, no Consecutive Frame, 63 Single Frames |
| B4 | as B3, `STMA` for `AT MA` | 2,222 requests, no Flow Control | 2,159 First Frames, no Consecutive Frame, 63 Single Frames |
| B5 | as B3, `STCSEGR1` | 2,197 lines | 2,206 lines, 2,153 of them a First Frame's data bytes alone; no rest of a message |
| B6 | as B3, `STFAC`, `STFAP 7E8,7FF`, `STFAP 7E0,7FF`, `STM` | nothing printed in 85 s | nothing printed |
| B7 | as B3, the Y-splitter's legs swapped | 2,071 requests, no Flow Control | 2,008 First Frames, no Consecutive Frame, 63 Single Frames |

- With no receive filter the MX+ hears every request and every First
  Frame, but neither the VX's Flow Control, about 1 ms after the First
  Frame, nor the ECU's Consecutive Frames after it. The monitor picks up
  again at the next request.
- `AT CS` read no errors before or after B5-B7, and swapping the
  splitter's legs changed nothing. On 2026-11-04 the USB2CAN on this
  splitter heard the segmented replies in full. The frames are on the
  bus; the MX+'s monitor misses them.
- `STCMM?` answered `?`. Not tried on the car: `STCMM` set to another
  mode (some modes acknowledge on the bus) and `STM`'s filter syntax.

## Findings

1. **The VX's 50 ms class is gone,** and the packed latency per request
   fell from 65.5-73.7 ms to 24.6 ms. **The VX holds a read asked to wait
   1 ms about 10 ms,** and the call for the rest of a batch costs every
   reply that much. Decided after the run: v0.4.1 drops that call.
2. **The MX+'s monitor misses every frame that follows another within
   about 1 ms,** the VX's Flow Control and the ECU's Consecutive Frames
   alike, whatever the formatting (`CAF`), the flow control (`CFC`), the
   receive filter (`CRA`), the monitor command (`AT MA`, `STMA`) or the
   segmentation setting (`STCSEGR`). Not the splitter, not the bus. A
   listening session through it hears Single Frame replies only. `AT
   CFC0` stays a listener's setting, so the listener never answers
   another tester's First Frame with a Flow Control of its own.

## Against the BENCH prep list

| # | Result |
|---|---|
| 19 | CONFIRMED for Single Frame replies on the MX+; REFUTED for multi-frame replies on the MX+ under every setup tried (`AT CAF1` or `CAF0`, `AT CFC0`, with and without `AT CRA`, `AT MA` and `STMA`, `STCSEGR1`), as on [2026-11-04-two-ecu-car](2026-11-04-two-ecu-car.md) under `AT CFC1` |
