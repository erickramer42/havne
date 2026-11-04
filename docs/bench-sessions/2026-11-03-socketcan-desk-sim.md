# DESK session: the simulated ECU on a real CAN controller, read by the VX

**Date:** 2026-11-03, 18:56-19:01
**Controller:** Innomaker USB2CAN (`gs_usb`, firmware V3.4.3) in WSL
(Ubuntu 26.04, kernel 6.18) through `usbipd-win`, as `can0`. Its
driver offers `LOOPBACK`, `LISTEN-ONLY` and `CC-LEN8-DLC`: no one-shot
mode.
**The other node:** the OBDX Pro VX on USB to the Windows host (J2534
through the x86 surrogate; DLL 1.8.9586.12845, API 04.04,
FW:1.0.1.0 HW:1.0.0.0), on its own power (its battery reading 0 V).
**The bus:** the USB2CAN and the VX on an OBD Y-splitter, as on
2026-11-02. No ECU and no car.
**Havne build:** `dev` at the simulated ECU's desk bus (core 0.3.6 by
its `start` line): `havne-cli sim` in WSL, `havne-cli stream` on
Windows for the VX
**Run sheet:** [socketcan-desk-runsheet.md](socketcan-desk-runsheet.md),
Phase 3
**Evidence:** both sides' `havne-cli` output, the surrogate's session
logs and `ip -details -statistics link show can0` at each step stay on
the desktop; the figures below are read from them. The sim's VIN is
made up (`ZZZFAKE11M0000001`), and nothing on this bus identifies a
car.

## The refusals

`can0` up at 500 kbit/s with `listen-only on`, then in normal mode.

| Command | Result |
|---|---|
| `havne-cli sim mock://sim --on can0` | `LOAD_FAILED` "can0 is a CAN controller, not vcan: the simulated ECU answers only on a virtual bus, never for a car's own ECU, unless the caller says it's a desk bus with no car" |
| the same with `--desk-bus`, `can0` listen-only | `LOAD_FAILED` "can0 is listen-only, so the simulated ECU couldn't answer: sudo ip link set can0 down, then sudo ip link set can0 up type can bitrate 500000 listen-only off" |
| the same, `can0` in normal mode (`restart-ms 0`, `listen-only off`) | answering: `{"type":"sim","uri":"mock://sim","on":"can0","desk_bus":true}` |

## The VX reading the simulated ECU

`havne-cli sim mock://sim --on can0 --desk-bus --seconds 150`, then on
Windows `havne-cli stream "j2534://OBDX Pro - VX" --values --seconds 60`
(`obd2`) started about 2 s later.

| What | Result |
|---|---|
| Identification | VIN `ZZZFAKE11M0000001` and ECU name `ECM-HavneSim` answered (the VIN a segmented reply, its Flow Control from the VX); CALID and CVN refused `7F 09 12`, as the sim answers |
| Diagnostics | stored P0301, P0420, U0100; pending P0171, C0035, B1001; permanent P0420, each with its title where core has one; the inspection line `mil_on` for model year 2021 |
| Values | RPM 0 through the key on, the engine catching at 12.8 s of the VX's session (about 15 s of the sim's), 1,981 r/min at 60 s; 2,625 frames, none dropped |
| Packing | 466 of 467 replies to `01 4E` packed; reply latency on the adapter's clock 12.3 ms median, 13.3 ms P99, 14.4 ms at most; no timeout, nothing refused but the two identification items |
| The sim's counts | requests and replies equal on every `sim_stats` line (2,625 of each on the last before the yield below), `unflowed` 0, `refused` 11 per VX session |
| `can0` | error-active through the run, every error counter 0; 7,794 frames sent in the VX's minute, every one acknowledged |

A first, shorter run a few minutes earlier gave the same: 315 requests,
315 replies, 11 refused, `unflowed` 0.

## Another node on the reply ID

With the sim still answering, the VX started a 20 s session; 6 s in,
`cansend can0 7E8#0641000000000000` from a second WSL shell (another
socket, so another node to the sim) at 19:00:41.63.

- The sim ended at once: `{"type":"end","reason":"yielded",
  "requests":2887,"replies":2886,...,"yielded":true}`, exit 1, and
  "havne-cli: another node sent on 0x7E8 on can0, so an ECU answers
  there: the simulated ECU went silent" on stderr. The one request it
  took after the frame went unanswered.
- The VX's session reported `vehicle not responding (timeout)` once the
  replies stopped.
- `can0` at the end: error-active, its counters at TX 15,899 and RX
  282,066, every error counter 0; then down.

## Findings

1. **The sim answers a real tester over real wire through a controller
   without one-shot mode.** Every reply was acknowledged, the VX's Flow
   Control reached the sim for every segmented reply, and the session
   read identification, diagnostics, inspection and moving values, and
   packed, as it reads a car.
2. **The desk bus's option reaches the controller check:** without it
   `can0` is refused as a real controller; with it, refused while
   listen-only and taken in normal mode.
3. **The yield works on a real bus:** one frame from another node on
   0x7E8 silenced the sim for good, and `havne-cli sim` said so and
   ended.
4. **No one-shot mode, no harm on this bus:** with the VX acknowledging,
   no frame was resent and no error counted.

## Against the BENCH prep list

| # | Result |
|---|---|
| 33 | CONFIRMED for the Innomaker USB2CAN and the VX: the refusals, a full `obd2` session read over real wire with every reply acknowledged and no segmented reply unflowed, and the yield to another node on 0x7E8 |
