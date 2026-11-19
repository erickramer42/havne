# VEHICLE session: request pacing on v0.8.0's core, Service 01 and 0x22, with the bus recorded

**Date:** 2026-11-18, 19:07-19:30
**Plan:** the width scan's three runs again on today's build
([2026-11-11-width-scan-car](2026-11-11-width-scan-car.md) ran core
0.7.1, before v0.8.0 paced J2534 by its replies: J2534's floor 5 ms,
ELM327's 20 ms), then the floor, the signal count, and the 0x22 pacing:
one DID, two modules, and `scan=first` blocks through both adapters.
A measurement: nothing passes or fails as such; the flags were any
timeout at `gap=0`, a refusal not shaped `7F 22 31`, and D1 below W1.
**Adapters:** on an OBD Y-splitter, one session at a time:
- the OBDX Pro VX on the laptop's USB, J2534 through the x86 surrogate
  (DLL 1.8.9586.12845, API 04.04, FW:1.0.1.0 HW:1.0.0.0);
- the OBDLink MX+ (ELM327 v1.4b, STN2255 v5.12.2) over Bluetooth as
  `COM5`;
- the Innomaker USB2CAN (`gs_usb`) passed into WSL with usbipd as
  `can0`, 500 kbit/s, listen-only, recorded with `candump -t a -L can0`
  from 19:04:58 to 19:30:52 (310,361 frames). ERROR-ACTIVE before and
  after, every error and drop counter 0, nothing sent.
**Vehicle:** the test car, parked, the engine idling (13.5-13.7 V at
each connect); the engine ECU on `0x7E0`/`0x7E8`, a second ECU on
`0x7E1`/`0x7E9`. No broadcast traffic on the OBD port (3 s of `candump`
with no tester: 0 frames).
**Havne build:** the laptop on Windows: `dev` at f513915 (core 0.8.0 by
its `start` line). Each run through the `havne-cli` deployed beside
`havne.exe` (`build/app/Release`), `ECUIO_SUR_LOG_FRAMES` off:
`havne-cli log <uri> <folder> --seconds 60 [--signal ...]` for D1-D8,
`havne-cli stream <uri> --values --seconds <s> --record <folder>` for
D9-D17.
**Evidence:** `havne-cli`'s output and recordings, the surrogate's
session logs, the MX+'s wire logs and the capture stay on the desktop;
the figures below are read from them. They hold the identification
replies. In [2026-11-18-pacing-car/](2026-11-18-pacing-car/): the VX's
refused opens (`vx-open-refused.log`) and D14's lost reply on the bus
(`D14-lost-reply-can0.log`, the identification payloads replaced).
The engine module's DID is written `<DID>`, as in
[2026-11-11-ext-session-car](2026-11-11-ext-session-car.md).

Reads only: Service 01 and 0x22 in the default session, and the
connect's identification and diagnostics reads.

## Service 01: the width scan again, the floor and the signal count

`havne-cli log`, 60 s each. Values a second and sweeps a second are as
on 2026-11-11: values over the span, and values over signals over the
span. The median gap is the median of each signal's median gap. The car
leaves 48 signals of the `obd2` rotation (47 on 2026-11-11).

| Run | Address | `--signal` | Values | Values a second | Sweeps a second | Median gap | 0x7E8 replies a second | Figure to beat |
|---|---|---|---|---|---|---|---|---|
| D1 | `j2534://OBDX Pro - VX?profile=obd2` | none | 23,827 | 397 | 8.28 | 255 ms | 58.7 | W1: 14,380, 240, 5.11, 395 ms |
| D2 | the same with `&pack=0` | none | 9,629 | 161 | 3.35 | 634 ms | 140.8 | W2: 2,425, 41, 0.86, 2,384 ms |
| D3 | `elm327://COM5?profile=obd2` | none | 16,332 | 272 | 5.68 | 364 ms | 40.9 | W3: 15,636, 261, 5.55, 359 ms |
| D4 | D1 with `&gap=0` | none | 21,845 | 364 | 7.58 | 281 ms | 52.7 | D1 |
| D5 | D1 with `&gap=20` | none | 19,492 | 325 | 6.77 | 310 ms | 48.2 | D1 |
| D6 | D2 | RPM | 8,478 | 141 | 141 | 5.8 ms | 140.3 | 38 requests a second (2026-11-09); 180-200 predicted |
| D7 | D2 | RPM and 7 more | 8,248 | 137.5 | 17.2 | 56 ms | 138.4 | the same |
| D8 | D3 with `&gap=0` | none | 16,779 | 280 | 5.83 | 352 ms | 41.9 | D3 |

D7's signals: RPM, Engine Load, Coolant Temp, Vehicle Speed, Intake Air
Temp, Throttle Position, MAF and Timing Advance.

From `candump`, a reply's last frame to the next request, and the ECU's
own times (P50 / P99, ms):

| Run | Reply's end to next request | Request to First Frame | Flow Control to first CF | CF to CF |
|---|---|---|---|---|
| D1 | 1.40 / 3.86 | 3.80 / 12.98 | 4.60 / 12.39 | 5.29 / 12.49 |
| D2 | 1.59 / 8.93 | | | |
| D3 | 7.65 / 38.63 | 3.71 / 12.14 | 4.85 / 11.86 | 5.00 / 11.80 |
| D4 | 1.30 / 2.03 | 5.05 / 13.14 | 4.73 / 13.53 | 5.33 / 12.15 |
| D5 | 5.36 / 15.81 | 4.34 / 11.07 | 4.84 / 10.34 | 5.22 / 10.96 |
| D6 | 1.71 / 9.28 | | | |
| D8 | 6.71 / 47.55 | 3.82 / 12.81 | 4.88 / 12.36 | 4.95 / 11.02 |

- **D1 beat W1 by about 65%:** 397 values a second against 240, 8.28
  sweeps against 5.11. Through the VX the next request follows a
  reply's last frame in 1.4 ms (P50).
- **Unpacked, the VX is about 4x W2:** 141 replies a second for one PID
  (D6) and 137.5 for eight (D7), against 2026-11-09's 38, so the signal
  count doesn't set the rate. Below the draft's 180-200: each exchange
  takes about 7 ms, the ECU's reply most of it.
- **`gap=0` is no faster on the VX (D4):** 52.7 replies a second against
  D1's 58.7, though the turn-round fell to 1.30 ms; the ECU's First
  Frame came later (P50 5.05 against 3.80 ms). The ECU sets the pace at
  `gap=0`. `gap=20` (D5) cost about 18% of D1's replies.
- **The MX+ is where it was:** D3 272 values a second against W3's 261,
  and `gap=0` (D8) the same as D3 (41.9 against 40.9 replies a second):
  the adapter sets its pace.
- No timeouts at `gap=0` (D4, D8) and no refusal in any run but the
  connect's seven `7F 22 31` (D1's capture: its `22 F1xx`
  identification reads).

## 0x22: one DID, two modules, and DID blocks

`havne-cli stream`. "Answered" is the run's last `timing` line; the bus
counts are `candump`'s.

| Run | Address | S | Answered | A second | Latency P50 / P99 / max | Interval P50 | Figure to beat |
|---|---|---|---|---|---|---|---|
| D9 | `j2534://OBDX Pro - VX?profile=uds-generic&did=7E9:DA03` | 60 | 8,822, none refused or timed out | 147 | 4.1 / 11.3 / 28.8 ms | 5.6 ms | 2026-11-05 A3: 36.8 a second; 180-200 predicted |
| D10 | `...&did=7E8:<DID>` (segmented) | 60 | 4,804, none refused or timed out | 80 | 10.2 / 20.5 / 43.4 ms | 11.3 ms | 2026-11-11 monitor R1: 2,158 in 60 s (36 a second) |
| D11 | `...&did=7E8:<DID>,7E9:DA03` | 60 | 0x7E8 5,023 (2 timeouts); 0x7E9 596 (485 timeouts) | 84 + 10 | | 0x7E8 11.3 ms | R1: `<DID>` 2,158 of 2,158, DA03 919 of 1,210 (76%) |

`scan=first`, 90 s each: the scan's end from the output's last
`complete` scan line, the poll after it from the delivered replies.

| Run | Address | Scan done | Found | Poll, replies a second | Each DID's median interval | Figure to beat |
|---|---|---|---|---|---|---|
| D12 | `j2534://...&did=7E8:0000-03FF&scan=first` | 7.8 s | 43 of 1024 | 140.5 | 0.304 s | S3 (core 0.7.1): scan 26 s, poll 38 a second; predicted scan about 6 s, poll 180-200 |
| D13 | `...&did=7E8:D800-DBFF&scan=first` | 7.9 s | 46 of 1024 | 62.1 | 0.730 s | S2: scan 21 s, poll 27 a second |
| D15 | D12 with `&gap=0` | 6.8 s | 43 of 1024 | 160.1 | 0.265 s | D12 |
| D14 | `...&did=7E8:0200-03FF,7E9:1E00-1FFF&scan=first` | 0x7E9 6.2 s; 0x7E8 never (below) | 42 of 512, 28 of 512 | 89.1 + 101.1 | 0.271-0.464 s | S3 + S4: 38 + 30 a second |
| D16 | `elm327://COM5?profile=uds-generic&did=7E8:0000-03FF&scan=first` | 23.9 s | 43 of 1024 | 45.5 | 0.940 s | none on record; D12 |
| D17 | `elm327://COM5?...&did=7E8:0200-03FF,7E9:1E00-1FFF&scan=first` | 24.1 s, 25.1 s | 43 of 512, 28 of 512 | 27.0 + 17.4 | 1.587 s | D16 |

- **One DID through the VX is 2-4x the earlier figures:** 147 a second
  for a single-frame reply (D9), 80 for a segmented one (D10), against
  36.8 and 36. D9 is still below the draft's 180-200.
- **The scans are about 3x faster:** 1024 DIDs in 7.8 s (D12) and 7.9 s
  (D13, the block with 15 multi-frame replies), against 26 s and 21 s;
  the polls after them 140.5 and 62.1 replies a second against 38 and
  27.
- **`gap=0` helps 0x22 a little (D15):** the scan 6.8 s, the poll 160.1
  a second against D12's 140.5, the same 43 DIDs, and no reply lost on
  the bus: 14,358 requests and 14,358 replies over the run, 14,357
  delivered.
- **Two modules through the VX (D14):** about 190 replies a second
  between them, against 2026-11-11's 38 + 30.
- **The MX+'s scan takes 23.9 s (D16)**, its poll 45.5 a second; with
  two modules (D17) the poll is 27.0 + 17.4, about D16's in all, so the
  `AT SH` switch between the two costs little. The MX+ delivered all
  but 2 of each module's replies on the bus in both.
- Every refusal was `7F 22 31`, plus one `7F 09 12` from 0x7E9 per run
  that asked it: its refusal of the connect's VIN request (it has
  none), as on 2026-11-11. No `7F xx 21` or `7F xx 78`.

## Flagged

- **Known Issue #42 at today's pace (D11):** DA03 went out 1,082 times
  and the bus carries 1,082 replies, but the VX delivered 596 (55%) and
  485 timed out, against R1's 76% on 2026-11-11. The engine's `<DID>`
  was asked at 84 a second against R1's 36: likely more of its segmented
  replies overlapping the second module's.
- **A scan DID lost by the VX is never asked again (D14).** At
  19:27:16.402 0x7E8's single-frame reply to one DID of its block was on
  the bus 4.9 ms after the request, while 0x7E9's segmented `22 F188`
  reply was coming in, and the VX didn't deliver it
  (`D14-lost-reply-can0.log`). The DID went out once in the run, and the
  0x7E8 scan sat at 511 of 512 done (42 found, 1 left) for the
  remaining 84 s; D17 found the same block's 43 through the MX+. Over
  the whole run 127 of 0x7E8's 8,179 replies on the bus never reached
  `havne-cli` (0x7E9's: 1 of 8,999); in the poll a lost reply's request
  goes out again on its next turn.
- **The `scan=first` runs wrote no `timing` lines and no `latency.csv`**
  (D12-D17: the folders hold `events.log`, `frames.csv`, `signals.csv`
  and `markers.csv`), so their timeouts and latency aren't in the
  output; the figures above come from the frames and the capture.
- **D14 and D17's first form was refused before opening:** two 1024-DID
  blocks, `?did= scans 2048 DIDs, more than the 1024 a session scans`
  (exit 3, "malformed transport URI"). The run sheet asked for more
  than a session scans; the halves above ran instead.
- **The VX's open was refused three times:** `PassThruOpen` status 7
  (`ERR_FAILED`) at 19:05:49 and 19:06:20, 3.6 s after each call, and
  status 34 once at 19:39:35 before B3
  ([2026-11-18-session-close-car](2026-11-18-session-close-car.md)),
  20 s after the previous session had closed cleanly; nothing was sent in any of them
  (`vx-open-refused.log`). After a power cycle of the VX it opened; a
  second try 65 s later opened after status 34.
- `havne-cli` in `build/cli/Release` has no surrogate beside it, so a
  `j2534://` session from there fails at once (`no surrogate
  (ecuio-j2534-surrogate.exe): none next to the app`); the copy beside
  `havne.exe` ran every J2534 run.
- `stream` reads its standard input as markers: D16's `markers.csv`
  holds the runner's next line. The runner gave the later runs
  `</dev/null`.
