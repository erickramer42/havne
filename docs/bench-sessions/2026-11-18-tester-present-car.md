# VEHICLE session: tester present between reads further apart than S3

**Date:** 2026-11-18, 19:30-19:37
**Plan:** BENCH prep #36's tester present with a channel that asks, but
only once a sweep, and a sweep longer than S3 (5 s), so that without
`3E 00` the module would leave the extended session between reads. On
2026-11-11 the polled DID never left `keepalive_ms` quiet
([2026-11-11-ext-session-car](2026-11-11-ext-session-car.md)) and the
quiet runs had nothing to ask
([2026-11-11-quiet-session-car](2026-11-11-quiet-session-car.md)).
**Adapters:** the OBDX Pro VX (J2534 through the x86 surrogate) and the
OBDLink MX+ (STN2255 v5.12.2, Bluetooth as `COM5`) on an OBD
Y-splitter, as in
[2026-11-18-pacing-car](2026-11-18-pacing-car.md), one session at a
time; the Innomaker USB2CAN (`gs_usb`) in WSL as `can0`, 500 kbit/s,
listen-only, recorded with `candump -t a -L can0` from 19:30:56 to
19:37:49 (644 frames). ERROR-ACTIVE throughout, every error counter 0.
**Vehicle:** the test car, parked, the engine idling (13.5-13.6 V at
each connect); the engine ECU on `0x7E0`/`0x7E8`.
**Havne build:** the laptop on Windows: `dev` at f513915 (core 0.8.0 by
its `start` line). Each run
`havne-cli log <uri> <folder> --signal "DID <DID>" --min-sweep <ms> --seconds 90`,
the VX's with `ECUIO_SUR_LOG_FRAMES=1`.
**Evidence:** the capture, in
[2026-11-18-tester-present-car/candump-C.log](2026-11-18-tester-present-car/candump-C.log),
with each identification reply's payload replaced by
`<identification>` (its frame kept); the counts below are its.
`havne-cli`'s output and recordings, the surrogate's session logs and
the MX+'s wire logs stay on the desktop. The DID is written `<DID>`, as
in the 2026-11-11 sheets.

Reads only: Service 0x22 and the session's `10 03`, `3E 00` and `10 01`.

| Run | Address | `--min-sweep` | `10 03` / `50 03` | `3E 00` / `7E 00` | Gap to a `3E 00` | Longest 0x7E0 silence | Reads, apart | Close |
|---|---|---|---|---|---|---|---|---|
| C1 | `j2534://OBDX Pro - VX?profile=uds-generic&session=03&did=7E8:<DID>` | 8000 | 1 / 1 | 34 / 34 | 1.994-2.011 s | 2.011 s | 12, 8.000 s | `50 01` 2.0 ms after `10 01` |
| C2 | C1 with `&keepalive_ms=4500` | 10000 | 1 / 1 | 18 / 18 | 4.491-4.550 s | 4.550 s | 9, 10.000 s | `50 01` 6.5 ms after |
| C3 | `elm327://COM5?profile=uds-generic&session=03&did=7E8:<DID>` | 8000 | 1 / 1 | 33 / 33 | 1.991-2.023 s | 2.023 s | 12, 8.001 s | `50 01` 4.7 ms after |
| C4 | C3 with `&keepalive_ms=4500` | 10000 | 1 / 1 | 17 / 17 | 4.500-4.586 s | 4.586 s | 9, 10.001 s | `50 01` 4.7 ms after |

The gap to a `3E 00` is from the frame before it on 0x7E0 (a `3E 00`,
a read's Flow Control or the connect's last request). The longest
silence counts every 0x7E0 frame from `50 03` to `10 01`.

## What passed

- **Tester present in every gap at `keepalive_ms`, each answered:** at
  2 s between reads 8 s apart (C1, C3), at 4.5 s between reads 10 s
  apart (C2, C4), through both adapters, every `3E 00` answered
  `7E 00`. Between two reads 8 s or 10 s apart the module went up to 8 s
  or 10 s with no read, past its S3.
- **The module held the session:** no `7F xx 7F` or `7F xx 7E` in any
  run; the session line `entered`, `entries` 1, `lost` 0 in each, and
  the read answered positively at every sweep. The only refusals are
  the connect's identification reads' `7F 22 31`.
- **Each close: one `10 01`, one `50 01`.**
- C3's and C4's recordings hold 11 and 8 values against 12 and 9 reads
  on the bus: the first read's reply spans frames and is dropped, and
  the later reads go with no response count (the wire log's "reply
  spans frames" line), as with the connect's segmented reads.

## Flagged

- **C4's keep-alive comes 57-86 ms past `keepalive_ms` after a read.**
  On the MX+ with `keepalive_ms=4500`, the `3E 00` after each read came
  4.542-4.558 s after the read's Flow Control, and the first one 4.586 s
  after the connect's last request: 7 silences on 0x7E0 over 4.550 s
  (4.557-4.586 s), so the run misses "nothing sent for more than
  `keepalive_ms` + 50 ms" by 7-36 ms. A `3E 00` after a `3E 00` came at
  4.500-4.501 s. Through the VX (C2) the same address stayed within
  4.491-4.550 s, and at 2 s (C3) the MX+ stayed within 2.023 s. The
  session held: every gap was under S3's 5 s and every `3E 00` answered.
  C4 ran without the frame log, so its wire log can't show when each
  read's prompt came; in the session-close runs the same read's prompt
  came 65-77 ms after it was typed
  ([2026-11-18-session-close-car](2026-11-18-session-close-car.md)).
- Still not shown: the module's session read back. F186 is refused on
  this car and no DID reads only in the extended session, so BENCH
  prep #36 keeps that half STILL UNKNOWN.

## Against the BENCH prep list

| # | Item | Result |
|---|---|---|
| 36 | The extended session on a real ECU | **CONFIRMED:** `3E 00` every `keepalive_ms` (2 s and 4.5 s) between reads 8 s and 10 s apart, through both adapters, the session held for 90 s with no loss; through the MX+ at 4.5 s, a `3E 00` after a read came up to 86 ms late (flagged above). **STILL UNKNOWN:** the module's session read back, and a DID that reads only in the extended session |
