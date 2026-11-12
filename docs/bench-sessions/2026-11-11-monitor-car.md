# VEHICLE session: the VX's lost replies and the MX+'s monitor, with the bus recorded

**Date:** 2026-11-11, 13:08-13:25
**Plan:** BENCH prep #35 (are the second module's replies that the VX
loses on the bus?) and #19 (the MX+'s monitor with a setup not tried
before), with a SocketCAN listener on the same splitter as the bus's
record.
**Adapters:**
- the OBDX Pro VX on the laptop's USB, J2534 through the x86 surrogate
  (DLL 1.8.9586.12845, API 04.04, FW:1.0.1.0 HW:1.0.0.0), asking;
- the OBDLink MX+ (ELM327 v1.4b, STN2255 v5.12.2) over Bluetooth as a
  COM port, listening;
- the Innomaker USB2CAN (`gs_usb`) passed into WSL with usbipd as `can0`,
  500 kbit/s, listen-only, recorded with `candump -t a -L can0`. It stayed
  ERROR-ACTIVE with every error counter at 0 through both sets.

All three on the test car's OBD port through an OBD Y-splitter.
**Vehicle:** the test car, parked, the engine idling; CAN OBD-II, 11-bit
IDs at 500 kbit/s, the engine ECU on `0x7E0`/`0x7E8` and a second ECU on
`0x7E1`/`0x7E9`. No broadcast traffic on the OBD port (3 s of `candump`
with no tester: 0 frames).
**Havne build:** the laptop on Windows: `dev` at 9e01d03 (core 0.7.1 by
its `start` line).
**Evidence:** stays on the desktop; the figures below are read from it:
the two `candump` logs, `havne-cli`'s output and recordings, the
surrogate's session logs with the frame log on, the MX+'s wire log and
the monitor script's raw lines. The identification replies are in the
raw files and nothing of them is here. The engine module's DID is
written `<DID>` here, as in
[2026-11-11-ext-session-car](2026-11-11-ext-session-car.md).

Reads only: Service 0x22 from the VX and the Flow Control it sends.
The MX+ and `can0` sent nothing.

In both sets the VX ran
`havne-cli stream "j2534://OBDX Pro - VX?profile=uds-generic&did=7E8:<DID>,7E9:DA03" --values --seconds 60`
with `ECUIO_SUR_LOG_FRAMES=1`: one DID on each module, the engine's
reply segmented (a First Frame, the VX's Flow Control, Consecutive
Frames), the second module's a Single Frame.

| Set | The MX+ | Result |
|---|---|---|
| R1 | Havne's listener, `elm327://COM5?profile=raw`, 70 s | #35: lost replies on the bus; the MX+ delivered nothing (Known Issue #41) |
| R2 | A script's monitor: `AT CAF0`, `AT CFC0`, `STCMM2`, `AT MA`, 70 s | #35 again; #19: no Consecutive Frame and no Flow Control |

## #35: the VX loses replies that are on the bus

| Set | DA03 requests on the bus | DA03 replies on the bus | The VX answered | The VX timed out |
|---|---|---|---|---|
| R1 | 1,210 | 1,210 | 919 | 290 |
| R2 | 1,328 | 1,327 | 1,074 | 253 |

- Every DA03 request had its reply on the bus within the set (one
  request at R2's end without one). The VX delivered about 76% (R1) and
  81% (R2) of them; the rest timed out with nothing late.
- The engine's `<DID>` replies arrived whole through the VX in both sets
  (R1: 2,158 requests, 2,158 First Frames on the bus, 2,158 answered).
- In R2 the MX+'s monitor printed all 1,350 of 0x7E9's Single Frames on
  the same splitter.
- On 2026-11-06 one DID asked of each module lost none; that engine DID's
  reply was a Single Frame. Here the engine's reply is segmented, and one
  DID each loses about a fifth: the losses follow the other module's
  segmented replies, not packing as such.

So the second module answers and the bus carries the reply; the VX
doesn't deliver it while the first module's segmented reply is coming
in. Not the ECU and not the bus.

## #19: the MX+'s monitor in the STN's monitor mode 2

`STCMM2` (receive every frame, error frames included, no
acknowledgement) was accepted with `OK`; `AT CS` read `T:00 R:00` before
and after.

| ID | `candump` | The MX+'s monitor |
|---|---|---|
| 0x7E0 | 2,177 Single Frames, 2,162 Flow Control | 2,177 Single Frames, **0 Flow Control** |
| 0x7E8 | 15 Single Frames, 2,162 First Frames, 2,170 Consecutive Frames | 15, 2,162, **0 Consecutive Frames** |
| 0x7E1 | 1,355 Single Frames, 2 Flow Control | 1,355, 0 Flow Control |
| 0x7E9 | 1,350 Single Frames, 4 First Frames, 4 Consecutive Frames | 1,350, 4, **0 Consecutive Frames** |

The monitor matched the bus exactly on every Single and First Frame and
printed no frame that follows another within about 1 ms. Mode 2 changes
nothing against 2026-11-05's setups. Mode 1 (an acknowledging node) was
not tried: a listener must not acknowledge.

## R1: Havne's listener delivered nothing

Every setup command answered `OK` (`ATCRA7E8` among them) and `AT MA`
started at 13:08:50.5, after the VX's connect. From then on 0x7E8 sent
only First Frames (`candump`: 0 Single Frames, 2,122 First Frames), each
of which the monitor passes without its Consecutive Frames, so every
reply was incomplete and dropped: 0 frames delivered, and the session's
health line "vehicle not responding" from its start. The monitor's
5-second check fired only after the VX had stopped. That is Known Issue
#41 with a request whose every reply is segmented.

## Against the BENCH prep list

| # | Item | Result |
|---|---|---|
| 35 | Two modules' replies through the VX at once | REFUTED, and the lost replies are on the bus: the VX doesn't deliver them |
| 19 | Listening hears another tester's replies | REFUTED again for multi-frame replies on the MX+, under `STCMM2` too |
