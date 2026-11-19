# VEHICLE session: the extended session's close, with the bus recorded

**Date:** 2026-11-18, 19:37-20:12
**Plan:** Known Issue #44's car re-run (BENCH prep #36's close): one
`50 01` and no resends at each close, through J2534 and through the
ELM327, with a read in flight and on a quiet channel, a listen-only
node on the bus and 20 s with no session after each close. On
2026-11-11 the VX's close left a `50 01` that came after its teardown
repeating for about 13.5 s
([2026-11-11-quiet-session-car](2026-11-11-quiet-session-car.md)), and
the MX+'s close typed into a read in flight got `STOPPED`, 3 of 3
([2026-11-11-ext-session-car](2026-11-11-ext-session-car.md)).
**Adapters:** the OBDX Pro VX (J2534 through the x86 surrogate) and the
OBDLink MX+ (STN2255 v5.12.2, Bluetooth as `COM5`) on an OBD
Y-splitter, one session at a time, the other idle; the Innomaker
USB2CAN (`gs_usb`) in WSL as `can0`, 500 kbit/s, listen-only, recorded
with `candump -t a -L can0`. ERROR-ACTIVE at every check, every error
counter 0, nothing sent. With the VX gone and the MX+ idle, no node on
the bus acknowledges, so a late reply would repeat in the capture.
**Vehicle:** the test car, parked, the engine idling; the engine ECU on
`0x7E0`/`0x7E8`.
**Havne build:** the laptop on Windows: `dev` at f513915 (core 0.8.0 by
its `start` line). Each run
`havne-cli stream <uri> --values --seconds 30 --record <folder>`, with
`ECUIO_SUR_LOG_FRAMES=1`.
**Evidence:** in
[2026-11-18-session-close-car/](2026-11-18-session-close-car/): each
close on the bus (`closes-can0.log`), the VX's closes in the surrogate
logs (`closes-surrogate.log`), the MX+'s in its wire logs
(`closes-elm327.log`), and B10's host sleep (`B10-host-sleep.log`). The
DID is written `<DID>`, as in the 2026-11-11 sheets. `havne-cli`'s
output, the recordings and the full logs stay on the desktop: they hold
the identification replies.

Reads only: Service 0x22 and the session's `10 03`, `3E 00` and `10 01`.

| Close | Address | Reads | `10 01` / `50 01` on the bus | `50 01` after `10 01` | 0x7E8 frames after `50 01` |
|---|---|---|---|---|---|
| B1 | `j2534://OBDX Pro - VX?profile=uds-generic&session=03&did=7E8:<DID>` | 2110 | 1 / 1 | 5.9 ms | 0 in 20.9 s |
| B2 | the same | 2622 | 1 / 1 | 3.4 ms | 0 in 62.8 s |
| B3 | the same | 2210 | 1 / 1 | 4.0 ms | 0 in 20.9 s |
| B4 | the same | 2643 | 1 / 1 | 4.9 ms | 0 in 20.9 s |
| B5 | the same | 2615 | 1 / 1 | 3.9 ms | 0 in 20.9 s |
| B6 | `j2534://OBDX Pro - VX?profile=uds-generic&session=03&did=7E8:FD00&keepalive_ms=4500` | FD00 refused (`7F 22 31`), then quiet | 1 / 1 | 4.0 ms | 0 in 20.9 s |
| B7 | the same | the same | 1 / 1 | 1.3 ms | 0 in 34.0 s |
| B8 | `elm327://COM5?profile=uds-generic&session=03&did=7E8:<DID>` | 432 | 1 / 1 | 6.7 ms | 0 in 36.3 s |
| B9 | the same | 427 | 1 / 1 | 3.5 ms | 0 in 33.8 s |
| B10r2 | the same | 422 | 1 / 1 | 2.3 ms | 0 in 32.5 s |
| B11r | `elm327://COM5?profile=uds-generic&session=03&did=7E8:FD00` | FD00 refused, then quiet | 1 / 1 | 6.4 ms | 0 in 42.5 s |

"After `50 01`" runs to the next session's first frame, or to the
capture's end (B2, B11r). B10r2 and B11r are B10's and B11's re-runs
(below).

## What passed

- **Every valid close: exactly one `10 01`, one `50 01`, and no 0x7E8
  frame after it,** 11 of 11, through both adapters, for at least
  20.9 s each.
- **J2534 reads the module's answer before its filter stops (B1-B7).**
  In each surrogate log the `10 01` write is followed by the `50 01`
  read within 5 ms (the log's stamps are whole milliseconds), then
  `OP_STOP_FILTER` 3-5 ms later and `OP_DISCONNECT` 2-3 ms after that.
  In B1-B5 the last read's reply (First Frame, Flow Control,
  Consecutive Frame) was complete on the bus 1.4-1.6 ms before the
  `10 01`, so no read was left out at the close.
- **The ELM327 close finishes the cut read first (B8, B9, B10r2):** each
  wire log reads `the close waits for 22<DID>'s prompt`, the prompt
  65-77 ms after the read was typed, then `1001` answered
  `7E8065001003201F4` and `7E8 is back in the default session
  (50 01)`. No `STOPPED` in any MX+ wire log of the night. On the quiet
  channel (B11r) the close met nothing in flight and got `50 01` the
  same way.
- Every run's session line: `entered`, `entries` 1, `lost` 0 before the
  close.

## Flagged

- **The laptop slept during B10 (19:47:19-20:06:19), and the session
  stalled for 19 minutes.** B10 started 19:47:14; at 19:47:19 the MX+
  had sent a read's two frames and no prompt when the host slept (the
  power plan's sleep after 30 min idle). On the wake the wire log reads
  `no line and no prompt for 5 s (22<DID>)`, COM5 closed and the session
  closed; `havne-cli` printed `{"type":"slept","us":1139290490}` (19
  min), a `transport hard failure` health line, and ended whole. No
  `10 01` went out: the module left the session by itself after S3. The
  5 s watchdog runs on the host, so it fired on the wake, not 5 s into
  the sleep. B10 is no close test; B10r2 re-ran it.
- **The capture was dead after the wake while usbipd read "Attached".**
  candump-B2's last 12 frames, from 19:47:19, were stamped 20:06:23.330
  on the wake (ordinary request and reply pairs, no repeat), and
  nothing came after. B11 (20:06:54) ran its close on that blind capture
  and B10r (20:08:31) on a new one that took 0 frames, so their quiet
  checks are not evidence; their wire logs show one `1001`
  answered `50 01` each (`closes-elm327.log`). After `usbipd detach`
  and `attach` at 20:10:10 the capture took 1,870 frames, and B10r2 and
  B11r ran on it. The run's own quiet check counted 0 frames on the
  blind capture too: a check that wants frames during each run would
  have caught it.
- **The VX refused its open once with status 34 (19:39:35),** 20 s after
  B2's session had closed cleanly; nothing was sent, and B3 ran 65 s
  later ([2026-11-18-pacing-car](2026-11-18-pacing-car.md)'s
  `vx-open-refused.log`).

## Against the BENCH prep list

| # | Item | Result |
|---|---|---|
| 36 | The extended session on a real ECU | **The close CONFIRMED** through both adapters with a listener on the bus: one `10 01` and one `50 01` at each of 7 J2534 and 4 ELM327 closes, the module silent after each for 20.9-62.8 s; the J2534 close reads `50 01` before its filter stops, and the ELM327 close waits for a cut read's prompt before `1001` (no `STOPPED`). Tester present: [2026-11-18-tester-present-car](2026-11-18-tester-present-car.md). **STILL UNKNOWN:** the module's session read back, and a DID that reads only in the extended session |
