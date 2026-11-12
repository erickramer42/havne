# VEHICLE session: Service 01 at full width, and DID blocks in both sessions

**Date:** 2026-11-11, 13:44-13:59
**Plan:** two sets after the day's extended-session runs
([2026-11-11-ext-session-car](2026-11-11-ext-session-car.md),
[2026-11-11-quiet-session-car](2026-11-11-quiet-session-car.md)). W: the
analysis log over every Service 01 PID the car supports, the baseline
for request pacing. S: the densest DID blocks of both modules scanned in
the default session and then the extended one, looking for a DID that
reads only in the extended session (BENCH prep #36).
**Adapters:** the OBDX Pro VX (J2534 through the x86 surrogate) and the
OBDLink MX+ (STN2255 v5.12.2, Bluetooth as a COM port), as in those
sessions.
**Vehicle:** the test car, parked, the engine idling, the selector in P;
the engine ECU on `0x7E0`/`0x7E8` and a second ECU on `0x7E1`/`0x7E9`.
**Havne build:** the laptop on Windows: `dev` at 9e01d03 (core 0.7.1 by
its `start` line).
**Evidence:** stays on the desktop: `havne-cli`'s output and
recordings, the surrogate's session logs and the MX+'s wire log. They
hold identification replies; nothing of them is here.

Reads only: Service 01 and 0x22, and the session's `10 03`, `3E 00` and
`10 01`. No health or link line in any run, and no `7F 22 78` (nothing
for BENCH prep #17).

## W: every supported PID

`havne-cli log <uri> <folder> --seconds 60`, no `--signal`, so the
whole `obd2` rotation: the 47 signals the car's bitmaps leave. A sweep
is one new value of every signal; sweeps per second here is values over
signals over the span.

| Run | Address | Values | Values a second | Sweeps a second | Median gap, one signal's values |
|---|---|---|---|---|---|
| W1 | `j2534://OBDX Pro - VX?profile=obd2` (packed) | 14,380 | 240 | 5.11 | 395 ms |
| W2 | the same with `&pack=0` | 2,425 | 41 | 0.86 | 2,384 ms |
| W3 | `elm327://COM5?profile=obd2` | 15,636 | 261 | 5.55 | 359 ms |

- Packing is worth about 6x on the VX at this width; unpacked, the VX
  asks one PID per request (about 41 a second).
- The MX+ edges out the packed VX. Against
  [2026-11-08-linux-car](2026-11-08-linux-car.md)'s 1, 4 and 8 signals,
  the rate at 47 signals is set by requests per second, not by the
  signal count.
- A few signals the profile asks more often came every 70-80 ms; most
  sat at the sweep's gap.

## S: six DID blocks, default then extended

Each block of 1024 DIDs scanned once with `scan=first`, then the answered
ones polled, 50 s a run, through the VX:
`j2534://OBDX Pro - VX?profile=uds-generic[&session=03]&did=<module>:<block>&scan=first`.
The blocks are the three with the most DIDs answered in a full default
sweep of each module on 2026-11-09.

| Module | Blocks | Answered, default | Answered, extended | Only in extended |
|---|---|---|---|---|
| 0x7E8 | three | 59, 46, 43 | 59, 46, 43 | none |
| 0x7E9 | three | 28, 20, 18 | 28, 20, 18 | none |

- Each pair answered the same DIDs in both sessions, and every refusal
  was `7F 22 31` (requestOutOfRange) in both: no `0x22`, `0x33`, `0x7F`
  or `0x78`. The 2026-11-09 sweep's only other refusal, the `0x22` of
  the DID read through
  [2026-11-11-ext-session-car](2026-11-11-ext-session-car.md), didn't
  come back there either.
- **The second module entered the extended session** (`50 03`, its first
  time), parked in P, and held it with `lost` 0 in all three runs.
- With that session's 256-DID range, seven blocks on two modules show no
  DID that reads only in the extended session on this car.
- `7F 09 12` once per 0x7E9 run is its refusal of a Service 09
  identification request, outside the blocks.

## Against the BENCH prep list

| # | Item | Result |
|---|---|---|
| 36 | The extended session on a real ECU | A second module (0x7E9) entered and held it. **STILL UNKNOWN:** a DID that reads only in the extended session: none in the seven blocks tried on two modules |
