# DESK session: a real CAN controller under SocketCAN, listening and sending

**Date:** 2026-11-02, Phase 1 from 13:16, Phase 2 16:45-16:50
**Controller:** Innomaker USB2CAN (`gs_usb`, firmware V3.4.3) in WSL
(Ubuntu 26.04, kernel 6.18) through `usbipd-win`, as `can0`. Its
driver offers `LOOPBACK`, `LISTEN-ONLY` and `CC-LEN8-DLC`
(`ctrlmode_supported`): no one-shot mode and no bus-error reporting.
**The other node:** the OBDX Pro VX on USB to the Windows host (J2534
through the x86 surrogate; DLL 1.8.9586.12845, API 04.04,
FW:1.0.1.0 HW:1.0.0.0), on its own power (its battery reading 0 V).
**The bus:** USB2CAN, one 120 ohm terminator, a DB9-to-OBD cable, an
OBD Y-splitter, the VX. No ECU and no car.
**Havne build:** `havne-cli` in WSL from `dev` (core 0.3.5 by its
`start` line) for SocketCAN; on Windows, `havne-cli` at core 0.3.6 for
the VX
**Run sheet:** [socketcan-desk-runsheet.md](socketcan-desk-runsheet.md)
**Evidence:** `havne-cli`'s output, `candump -t A -e [-x]` and
`ip -details -statistics link show can0` before and after each run stay
on the desktop; the figures below are read from them. Nothing on this
bus identifies a car.

## Phase 1: listening (BENCH prep #26)

`can0` up at 500 kbit/s with `listen-only on`; Havne's
`socketcan://can0` (the `raw` profile) for 10 s, the VX's
`j2534://OBDX Pro - VX` (`obd2`) beside it.

| Run | Havne's frames | What they were | `can0` after |
|---|---|---|---|
| The bus quiet (the VX not connected) | 0 | | TX 0, error-active |
| 500 kbit/s, the VX asking | 38,497, the kernel's RX count exactly | every one `7E0 02 01 00 00 00 00 00 00`, the VX's first probe request sent again and again (no node acknowledged it), a median 276 us apart; none flagged, none dropped | TX 0, error-active |
| 250 kbit/s, the VX at 500 | 11 error frames, none of data | class 0x0C (controller and protocol): rx and tx error-passive, a form error, the receive counter at its top; delivered with `ECUIO_FRAME_FLAG_ERROR`, ID 0x0C | TX 0, error-passive (11 times) |

- The VX's session reported "request failed (vehicle not accepting
  requests)" in one episode in each: with the USB2CAN only listening,
  nothing acknowledged its requests, at either rate. A listen-only
  controller sent nothing, not even an ACK, at the right rate or the
  wrong one.
- **Unplugged:** with the session running at 500 kbit/s,
  `usbipd detach` at 13:24:49.0 and attach at 13:24:57.2. The session
  reported `transport hard failure` (one episode); `can0` came back
  down, and once it was brought up again the session reported
  `success` with no reconnect of its own, its frames resuming after a
  gap of 126.6 s of session time (157,588 frames in all, none dropped).
- Opening the VX while it was off its USB data link gave the open's
  hint: "PassThruOpen returned status 34 (0x22, vendor-specific): is
  the adapter attached, and not open in another program?".

## Phase 2: sending (BENCH prep #32)

Havne refuses to send through a controller without one-shot mode, so
`can-utils` sent: `cansend can0 7E8#0241000000000000` (a reply to
Service 01 PID 00 listing no PIDs), with
`candump -t A -e -x can0` marking each frame TX or RX. `can0` in normal
mode, `restart-ms 0`.

**Listen-only stays set.** `ip link set can0 up type can bitrate 500000
restart-ms 0` on an interface last brought up with `listen-only on`
keeps listen-only: five `cansend`s returned 0 and the TX count stayed
0. Nothing went out. Normal mode needs `listen-only off` said.

| Run | What happened | `can0` |
|---|---|---|
| 500 kbit/s, the VX's `obd2` session open | five frames 1 s apart, each echoed (`TX` in candump); the VX's session received five frames on 0x7E8, data `41 00` (its ISO 15765 channel took off the length byte), and its health went `success`, then `vehicle not responding (timeout)`, for each | TX 5, error-active, every error counter 0 |
| 500 kbit/s, no VX session (no node to acknowledge) | one frame at 16:47:16.045. Its echo came at .049059 and the TX count rose to 6; the first error frame came at .050576, `no-acknowledgement-on-tx` with the transmit counter at 40; from 16:47:17, one error frame a second, rx and tx error-passive, the transmit counter at 128 | error-passive for 15 s, `bus-off` 0, until `ip link set can0 down` |
| 250 kbit/s, the VX's session at 500 | before the send, one error frame (a bit-stuffing error, the receive counter at 16: the VX's requests at 500 kbit/s); after one `cansend` at 16:48:11.439, its echo at .448, then `no-acknowledgement-on-tx` once a second, the transmit counter at 128; the VX's session reported only `request failed`, no frames | error-passive for 15 s, `bus-off` 0, until `ip link set can0 down` |

**Havne's own check** (`socketcan://can0?profile=obd2&baud=500000`,
`can0` in normal mode at 500 kbit/s): refused at connect, `LOAD_FAILED`
"can0 isn't in one-shot mode, so a frame that fails would be sent again
by the controller: sudo ip link set can0 down, then sudo ip link set
can0 up type can bitrate 500000 restart-ms 0 one-shot on (where its
driver offers it)". Without `&baud=`, refused first with `BAD_URI`:
the rate is stated, never assumed.

## Findings

1. **Without one-shot mode, a frame no node acknowledges is sent
   forever.** The controller went error-passive within about 1.5 ms
   and stayed there, its transmit counter at 128, until the interface
   went down: ISO 11898-1 leaves an error-passive transmitter's counter
   unchanged on an acknowledgement error, so it never reaches bus-off.
   The same at a wrong rate on this bus of two. ADR-0040 section 2's
   one-shot rule stays.
2. **The echo came before any acknowledgement.** With nothing on the
   bus that could acknowledge it, the frame was echoed and counted as
   sent, 1.5 ms before the first `no-acknowledgement-on-tx`. ADR-0040
   section 3 takes the first echo as proof of the rate; on this
   adapter it isn't (Known Issue #40). Havne refuses this adapter for
   sending anyway; an adapter with one-shot mode may echo the same way.
3. **The refusal suggests a mode the controller lacks.** It names
   `one-shot on`, which this driver doesn't offer; the controller's
   supported modes are readable without privileges.
4. **Listen-only must be turned off by name** when an interface goes
   from listening to sending (the run sheet says so).
5. **A listen-only controller disturbs nothing:** no frame, no ACK and
   no error frame from it at either rate, and Havne delivered every
   frame the kernel counted.

## Against the BENCH prep list

| # | Result |
|---|---|
| 26 | CONFIRMED on the desk bus: link kind `can`, its rate and listen-only from rtnetlink; no ACK or error frame from a listen-only controller; frames at the kernel's count and stamps at the right rate, errors only at the wrong one; unplugged, a hard failure and a recovery once `can0` is up again. The car is still to come |
| 32 | REFUTED in part: the echo doesn't wait for an acknowledgement on this adapter (Known Issue #40), and without one-shot mode the resends stop at error-passive, never bus-off, so one-shot mode stays required. CONFIRMED: on the bus of two at the right rate, frames echo and the VX hears them |
