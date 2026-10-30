# ELM327 BENCH run sheet: the VX in ELM mode, no vehicle

**Date:** <date>  **Adapter:** OBDX Pro VX (USB), as an ELM327
**Machine:** <the dev machine, or the laptop that goes to the car>
**Havne build:** <commit hash>

Fill in a copy as `docs/bench-sessions/<date>-elm327-bench.md`. The
J2534 bench sheet is [bench-runsheet.md](bench-runsheet.md); the
vehicle follows [elm327-vehicle-runsheet.md](elm327-vehicle-runsheet.md).
The MX+ has no bench session: it takes its power only from the OBD
connector ([ADR-0026](../adr/0026-elm327-transport.md)'s appendix of
2026-10-27).

## Purpose

`elm327://` on a real adapter for the first time, and the Win32 serial
port code's first run (it has no automated test). Goals, in order:

1. The VX gets a serial port over USB (BENCH prep #11)
2. It takes the setup's every command (#12) and gives its readings
   (#13)
3. With no bus at all, its `NO DATA` reads as quiet (#15), and an
   unplugged cable as a fault that recovers (#16)

The evidence is each connect's wire log,
`%TEMP%\ecuio-elm327.<YYYYMMDD-HHMMSS-mmm>.log`: every command, every
line the adapter sent back, and, with **Record sessions** on, every
request and reply with its prompt's time after the request. The
activity log (`events.log` when recording) has the link's states.

## Pre-flight

- [ ] `scripts/verify.sh` prints four `ok` lines on this build
- [ ] The OBDX vendor app closed, and no `j2534://` session open: one
      program at a time can hold the port
- [ ] For WiFi (`elm327://192.168.4.1:23`): nothing on the VX's USB
      port, and the VX power-cycled since the last USB session or
      unclean WiFi leave; disconnect in Havne before leaving its
      network (BENCH prep #20)
- [ ] Launch Havne and tick **Record sessions**, so the wire logs
      hold the requests too

## Phase 1: the port

**Action:** plug the VX in by USB. In Device Manager, open "Ports (COM
& LPT)"; in Settings, press **Refresh**.

- [ ] A new `COMn` in both, the Settings row reading "Serial port, for
      elm327:// · \Device\…". Copy the port and the device name. The
      VX is a USB CDC serial device (VID 0483, PID 5740, STM32's virtual
      COM port): "OBDX Pro Serial (COM4)" on the dev machine, where
      OBDX's `obdxdriver.inf` names it; Windows 10 and 11 bind their own
      `usbser.sys` without it, as "USB Serial Device (COMn)"
- [ ] Device Manager, the port's Properties, Driver tab: copy the
      provider and the driver file (`usbser.sys`). On a machine without
      the OBDX software, note whether the port appears at all: that
      says whether `elm327://` needs no vendor install on Windows
- [ ] No new port: record what Device Manager shows for the VX (an
      unknown device, or one under another class) and stop: nothing
      below can run

## Phase 2: connect with no bus

**Action:** click the port's row (the field reads `elm327://COMn`),
Connect, and leave it about a minute.

**Expected:**
- [ ] The wire log: the answer to every setup command and each
      prompt's time: `AT Z` answered `OK` alone, then `AT I` with
      `ELM327 V2.1`, then `< OK` to each command (or what came instead);
      after each request, `NO DATA` (#15) and the time to its prompt
- [ ] Connect succeeds within a few seconds. Copy the "Session:" line:
      `ELM327 adapter (ELM327 v…); ISO 15765 at 500000 baud; profile
      obd2, channel 0x7E8/0x7E0 (rx/tx); …`. No STN part (the VX isn't
      one, so `STI` is refused), and the battery `0.0 V` or "no battery
      reading" on USB power (the J2534 path read 0.0 V, BENCH prep #10)
- [ ] Amber "Link quiet" in the header (Live's banner: "Vehicle not
      responding") and "Link: vehicle not responding" in the activity
      log within a few seconds: the VX answers `NO DATA` with no bus, as
      with an ECU that doesn't answer (ADR-0026's appendix of
      2026-10-27, "the VX's answers"), and the requests go on
- [ ] Task Manager: `havne.exe` under 1 % CPU
- [ ] Disconnect: `Disconnected:` at once

A refused connect names the command and the answer ("the adapter on
COMn answered ATxx with '…', not OK"): copy it verbatim and stop. That
answer is the finding (#12).

## Phase 3: the cable pulled

**Action:** connect again, wait for "Link quiet", unplug
the USB cable for about 10 s, then plug it back in.

- [ ] Unplugged: red "Adapter fault" within about 10 s
- [ ] Plugged back: the link returns to "Link quiet" within about
      10 s, with no reconnect (the port reopens and the
      setup runs again). Record the times, and copy the wire log's
      `unasked:` lines after the reopen: what the VX prints on power-up.
      If Windows gives the VX another `COMn`, it can't recover: record
      that
- [ ] Disconnect

## Phase 4: the other CAN rate, and the J2534 path after

- [ ] `elm327://COMn?baud=250000`, about 15 s: connects (protocol 8)
      and goes quiet as in Phase 2. Disconnect
- [ ] `j2534://OBDX Pro - VX`, about 15 s: still connects, as on
      [bench-runsheet.md](bench-runsheet.md)'s Phase 1. Disconnect

## If it fails, suspect

| Symptom | Suspect |
|---|---|
| "COMn: the port is in use by another program" | The vendor app, or a `j2534://` session, holds it |
| "the adapter on COMn didn't answer ATZ within 5 s" | The serial rate: retry with `?serial=115200`. Then a port that isn't the VX's |
| "answered ATZ with '…', not an ELM327 version", or ATI | Garbled text: the serial rate, as above. Readable text: the adapter isn't in ELM mode, or another device is on the port |
| Connects, then red "Adapter fault" with the cable in | The port's reads or writes failing: copy the activity log whole |
| The app crashes | Capture the activity log and stop |

## Post-session actions

1. Copy this sheet, filled in, to `docs/bench-sessions/<date>-elm327-bench.md`,
   and the session's wire logs to a folder beside it. Scrub them as
   [bench-runsheet.md](bench-runsheet.md) says: no names, no local paths
2. Mark BENCH prep #11–#16 for the VX in [bench-prep.md](bench-prep.md)
3. Update STATUS.md, and testing.md's gaps (the Win32 port)
