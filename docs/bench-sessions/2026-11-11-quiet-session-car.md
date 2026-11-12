# VEHICLE session: tester present in a quiet extended session

**Date:** 2026-11-11, 13:32-13:37
**Plan:** BENCH prep #36's tester present on a real ECU, after
[2026-11-11-ext-session-car](2026-11-11-ext-session-car.md) found that a
polled DID never leaves a channel quiet. Here the channel's only DID is
one the module refuses, so it is dropped and the channel sits in the
extended session with nothing to ask. Then the ELM327 close with nothing
in flight, and the session DID F186.
**Adapters:** the OBDX Pro VX (J2534 through the x86 surrogate) and the
OBDLink MX+ (STN2255 v5.12.2, Bluetooth as a COM port), as in that
session; the Innomaker USB2CAN (`gs_usb`) in WSL as `can0`, 500 kbit/s,
listen-only, recorded with `candump -t a -L can0` around each run.
ERROR-ACTIVE throughout, every error counter 0.
**Vehicle:** the test car, parked, the engine idling; the engine ECU on
`0x7E0`/`0x7E8`. No broadcast traffic on the OBD port.
**Havne build:** the laptop on Windows: `dev` at 9e01d03 (core 0.7.1 by
its `start` line). Each run through `havne-cli stream <uri> --values
--seconds <s> --record <folder>`, the VX's with `ECUIO_SUR_LOG_FRAMES=1`.
**Evidence:** stays on the desktop: the `candump` logs, `havne-cli`'s
output and recordings, the surrogate's session logs and the MX+'s wire
log. They hold identification replies; nothing of them is here.

Reads only: Service 0x22 and the session's `10 03`, `3E 00` and `10 01`.

| Run | Address | Seconds | `3E 00` sent, median gap | `7E 00` | Session | Close |
|---|---|---|---|---|---|---|
| T1 | `j2534://OBDX Pro - VX?profile=uds-generic&session=03&did=7E8:FD00` | 60 | 29, 2.008 s | 29 | entered, lost 0 | `50 01` 3 ms after `10 01` |
| T2 | the same with `&keepalive_ms=4500` | 60 | 13, 4.505 s | 13 | entered, lost 0 | `50 01` 14 ms after `10 01`, after the VX had left the bus (below) |
| T3 | `elm327://COM5?profile=uds-generic&session=03&did=7E8:FD00` | 60 | 29, 2.001 s | 29 | entered, lost 0 | `50 01` |
| T4 | `j2534://OBDX Pro - VX?profile=uds-generic&session=03&did=7E8:F186` | 20 | 9, 2.005 s | 9 | entered, lost 0 | `50 01` 8 ms after `10 01` |

FD00 answered `7F 22 31` in both sessions on 2026-11-11, so as the only
DID it was dropped after its refusal, and after the connect's
identification reads the channel asked nothing more. The counts are
`candump`'s; Havne's logs agree.

## Tester present

- **It goes out in each quiet gap and is answered:** through both
  adapters every `keepalive_ms`, to the millisecond (T1 2.001-2.019 s,
  T3 1.990-2.009 s, T2 4.504-4.511 s), each answered `7E 00`. Nothing
  else was asked.
- **The module never left the session:** no `7F xx 7E` or `7F xx 7F` in
  any run, so the session line stayed `entered` with `lost` 0 through
  each run's 60 s, twelve S3 times over. T2's 4.5 s gaps held too, so the
  module's S3 is at least 4.5 s.
- **Not read back:** a positive answer to `3E 00` comes in either
  session, so these runs show tester present sent, answered and no loss,
  not the module's session read back. F186, the standard "active
  diagnostic session" DID, would show it, but the module refused it in
  the extended session too (T4: `7F 22 31`, 22 ms after `50 03`).

## The ELM327 close with nothing in flight

T3's close typed `1001` with the channel quiet and got `50 01`. Step 4,
B1 and B2 of [2026-11-11-ext-session-car](2026-11-11-ext-session-car.md)
got `STOPPED`, each with a read in flight. So the close fails only when
it lands mid-read.

## The J2534 close and a late `50 01`

The VX's close writes `10 01`, then stops its filter and disconnects
about 10 ms later (ADR-0060 leaves the reply to the disconnect). In T1
and T4 the module answered within 3 and 8 ms, while the VX was still on
the bus. In T2 it answered after 14 ms, when the VX had gone. The
USB2CAN was listen-only and the MX+ idle, so no node acknowledged the
frame, and CAN made the module send it again: `7E8 06 50 01 00 32 01
F4` every 1.21 ms, 4,650 times in T2's `candump` and 4,285 more in T3's,
about 13.5 s in all, until the MX+'s next session took the bus and
acknowledged it. One `10 01` was sent; the rest was the module repeating
its answer. Nothing counted an error.

On a car whose OBD port has no other node, a reply that arrives after
the VX has left the bus repeats until something acknowledges it. The
close should wait for its `50 01` (or P2) before it disconnects. Known
Issue #44; a fix is planned for v0.7.3.

## Against the BENCH prep list

| # | Item | Result |
|---|---|---|
| 36 | The extended session on a real ECU | **CONFIRMED:** `3E 00` every `keepalive_ms` (2 s and 4.5 s) in a quiet channel, each answered, no loss for 60 s, through both adapters. **STILL UNKNOWN:** the module's session read back (F186 refused) and a DID that reads only in the extended session. **The close:** the ELM327's `10 01` fails only mid-read; the J2534 close leaves a late `50 01` unacknowledged |
