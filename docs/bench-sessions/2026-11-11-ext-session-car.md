# VEHICLE session: the extended diagnostic session on the test car

**Date:** 2026-11-11, 12:15-12:59
**Plan:** ADR-0060's extended session on a real ECU (BENCH prep #36):
`?session=03` on `uds-generic` through both adapters, one DID that a
2026-11-09 scan saw refused with `7F 22 22` (conditionsNotCorrect), read
in the default session and then in the extended one. Then three short
sets: a 256-DID range in both sessions (C), the ELM327 close twice more
(B), and the DID with the engine off (A).
**Adapters:**
- the OBDX Pro VX on the laptop's USB, J2534 through the x86 surrogate
  (DLL 1.8.9586.12845, API 04.04, FW:1.0.1.0 HW:1.0.0.0);
- the OBDLink MX+ (ELM327 v1.4b, STN2255 v5.12.2) over Bluetooth as a
  COM port at 38400 bit/s.

Both stayed on the test car's OBD port through an OBD Y-splitter for the
whole run; one session at a time.
**Vehicle:** the test car, parked; CAN OBD-II, 11-bit IDs at 500 kbit/s,
the engine ECU on `0x7E0`/`0x7E8`. The engine idling for steps 1-4, C
and B (13.5-13.6 V at the adapters; 12.0 V at step 1's connect, the
car's charge control); off with the ignition on for A (12.4-12.5 V).
**Havne build:** the laptop on Windows: `dev` at 9e01d03 (core 0.7.1 by
its `start` line), `build-win32` then `build`. Every step ran as
`havne-cli stream <uri> --values --seconds <s> --record <folder>`; the
VX steps with `ECUIO_SUR_LOG_FRAMES=1`.
**Evidence:** stays on the desktop; the figures below are read from it:
`havne-cli`'s output, the recordings, the surrogate's session logs with
the frame log on, and the MX+'s wire logs. The identification replies
are in the raw files and nothing of them is here. The DID is written
`<DID>` here; the raw files hold it.

Reads only: Service 0x22, and the session's `10 03`, `10 01` and `3E 00`
on `?session=03`. Nothing was cleared or written.

| Step | Adapter | Address | Engine | Result |
|---|---|---|---|---|
| 1 | VX | `j2534://OBDX Pro - VX?profile=uds-generic&did=7E8:<DID>`, 240 s | idling | PASS: answered in the default session |
| 2 | VX | `...&session=03&did=7E8:<DID>`, 240 s | idling | PASS: entered, read, `10 01` written at the close |
| 3 | MX+ | `elm327://COM5?profile=uds-generic&did=7E8:<DID>`, 240 s | idling | PASS |
| 4 | MX+ | `...&session=03&did=7E8:<DID>`, 240 s | idling | Entered and read; the close's `10 01` never sent (below) |
| C1 | VX | a 256-DID range holding `<DID>`, `scan=first`, 90 s | idling | 3 of 256 answer |
| C2 | VX | the same range with `session=03`, 90 s | idling | entered; the same 3 answer |
| B1, B2 | MX+ | step 4's address, 30 s each | idling | the close's `10 01` never sent, both times |
| A1 | VX | step 1's address, 45 s | off, ignition on | answered |
| A2 | VX | step 2's address, 45 s | off, ignition on | entered; answered |

## Steps 1-4: one DID in both sessions, both adapters

| Step | Answered | Refused | Timeouts | Latency p50 / max | Interval p50 |
|---|---|---|---|---|---|
| 1, VX, default | 8543 | 0 | 0 | 10.2 / 63.1 ms | 30.7 ms |
| 2, VX, extended | 8467 | 0 | 0 | 10.2 / 44.6 ms | 30.7 ms |
| 3, MX+, default | 3615 | 0 | 0 | 16.4 / 63.9 ms | 73.7 ms |
| 4, MX+, extended | 3591 | 0 | 0 | 16.4 / 74.9 ms | 73.7 ms |

- **The DID answered in the default session.** The 2026-11-09 scan's
  `7F 22 22` didn't come back in any step: the refusal was a condition
  of that day, not the session. The reply was the same 10 bytes in every
  step and both sessions.
- **The entry:** `10 03` was answered `50 03 00 32 01 F4` on both
  adapters (P2 50 ms, P2* 5000 ms), the `session` line `entered`,
  `entries` 1, `lost` 0. The reads waited for it: the first `62` reply
  came after `50 03`.
- **No tester present was sent.** A channel sends `3E 00` only after
  `keepalive_ms` (2000 by default, 4500 at most) with nothing sent, and
  the DID went out about every 31 ms (VX) or 74 ms (MX+), so the session
  never went quiet. That is ADR-0060's rule working, and it leaves "past
  S3" unchecked.
- **The VX's close:** step 2 wrote `10 01` (status 0) at 12:24:12.246,
  stopped the filter 10 ms later and disconnected. ADR-0060 leaves the
  reply to the disconnect on J2534, so `50 01` isn't in the log; the
  request did reach the bus.
- **The MX+'s close:** step 4 typed `1001` and the adapter answered
  `STOPPED` 5.2 ms later, the session closing at once. `STOPPED` is the
  adapter's word for a command cut off by another character, so the
  previous read was still in the adapter when `1001` was typed and the
  `10 01` never went out. ADR-0060 says the ELM327 close waits for its
  prompt first; on the car it didn't. The module leaves the extended
  session by itself when S3 lapses, so nothing stays changed, but the
  close doesn't do what the ADR says. A fix is planned for v0.7.3.
- **The MX+'s "3 dropped" ISO-TP replies** are the same three in every
  MX+ session, 240 s or 30 s: the connect's reads whose replies span
  frames, asked again with no response count (the wire log's "asked
  again" lines). None was lost while polling.

## C: does the extended session open more DIDs?

A 256-DID range holding `<DID>`, scanned once and then the answered
ones polled, on the engine idling. C1 in the default session, C2 in the
extended one.

| Set | Answered | Refused `7F 22 31` | Refused `7F 22 22` | Other |
|---|---|---|---|---|
| C1, default | 3 | 253 | 0 | 0 |
| C2, extended | 3 (the same 3) | 253 | 0 | 0 |

Every refusal was requestOutOfRange, in both sessions; 7 more `7F 22 31`
in each set's output are `uds-generic`'s identification reads, not the
range. In this range the extended session opens nothing.

## B: the MX+'s close, twice more

B1 and B2 repeated step 4 for 30 s each. Both entered (`50 03`), read
438 and 436 times with nothing refused, and at the close typed `1001`
and got `STOPPED` 5.1-5.2 ms later. With step 4 that is 3 of 3 MX+
closes. B2's first open, in the same second B1 closed, failed with
Windows error 1167 (the device isn't connected); it opened 30 s later.

## A: the engine off, ignition on

| Set | Session | Answered | Refused | Battery |
|---|---|---|---|---|
| A1 | default | 1554 | 0 | 12.49 V |
| A2 | extended (`10 03` answered `50 03`) | 1590 | 0 | 12.40 V |

The DID answered with the engine off too, in both sessions, and `10 03`
was accepted with the engine off. So neither engine off (ignition on)
nor the session brings back 2026-11-09's refusal. Nothing different
about that day's key position, cranking or adapter is recalled; what the
condition was is unknown.

## The MX+ wouldn't open at first

Before step 3, `COM6` failed twice with Windows error 1231 (the network
location can't be reached), also from a plain serial open and after a
power cycle of the adapter. Removed and paired again in Windows, it came
back as `COM5` and answered `ATI` at once. Nothing in Havne was changed
for it.

## Against the BENCH prep list

| # | Item | Result |
|---|---|---|
| 36 | The extended session on a real ECU | **CONFIRMED:** `10 03` answered `50 03` with P2 50 ms and P2* 5000 ms on both adapters, with the engine idling and off, and reads in the session. **STILL UNKNOWN:** tester present past S3 (the DID poll never leaves `keepalive_ms` quiet), and a DID that reads only in the extended session (none in the range tried; `<DID>` reads in both). **The close:** J2534 writes `10 01` as designed; the ELM327 close never sends it (`STOPPED`, 3 of 3), REFUTED against ADR-0060 |
