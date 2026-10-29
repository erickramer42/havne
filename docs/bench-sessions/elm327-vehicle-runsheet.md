# ELM327 VEHICLE run sheet — the MX+ on the car, read-only

**Date:** <date>  **Adapter:** OBDLink MX+ (MX201, Bluetooth)
**Vehicle:** the test car, the car of the J2534 sessions (never its make, model or year)
**Havne build:** <commit hash>

Fill in a copy as `docs/bench-sessions/<date>-elm327-vehicle.md`. The
MX+ first powers up here, so this sheet also runs its bench items
([ADR-0026](../adr/0026-elm327-transport.md)'s appendix of
2026-10-27). The expectations are the car's from
[2026-10-25](2026-10-25-vehicle.md), read through the VX: a difference
is the finding.

## Purpose

1. The MX+ pairs, takes the setup (BENCH prep #12) and gives its
   readings (#13)
2. The `obd2` values decode as they did through the VX, engine off and
   idling (#14)
3. How many requests a second it sustains over Bluetooth, against the
   VX's 26
4. A Bluetooth drop recovers without a reconnect (#16)

The evidence is each connect's wire log
(`%TEMP%\ecuio-elm327.<YYYYMMDD-HHMMSS-mmm>.log`: every command and
line, and with **Record sessions** on every request and reply with its
prompt's time), the activity log (each connect's `events.log`), the
recordings and photos. The wire log holds the identification replies
in hex (`49 02` the VIN, `49 04` the CALID, `62 F1 xx` the ECU's
serials): scrub before committing.

## Safety and pre-flight

- [ ] [vehicle-runsheet.md](vehicle-runsheet.md)'s "Safety and scope"
      holds in full: read-only, parked, ignition on with the engine off
      kept short, the laptop on its own charged battery, the abort order
- [ ] The laptop's settings for the day, as there (sleep never, lid
      does nothing, USB selective suspend off, updates paused)
- [ ] [elm327-bench-runsheet.md](elm327-bench-runsheet.md) passed on
      this laptop and build
- [ ] The phone's Bluetooth off, or the OBDLink app closed: the MX+
      takes one connection at a time
- [ ] Havne launched, **Record sessions** ticked

## Phase 0 — the reference, ignition off

**Action:** the VX in the OBD port, ignition off. Connect
`j2534://OBDX Pro - VX` for about 15 s, then Disconnect and unplug it.

- [ ] Copy the battery from its "Session:" line (12.2 V on
      2026-10-25): the MX+'s reading is held against it (#13)

## Phase 1 — pair and connect the MX+, ignition off

**Action:**
1. Plug in the MX+. Note its four lights (Power, OBD, Host, Link)
2. Pair it: Windows' Bluetooth settings, Add device, then the MX+'s
   button. Note the lights again
3. Find its outgoing port: More Bluetooth settings, the COM Ports tab,
   the "Outgoing" row. Settings' **Refresh** lists it among the
   `\Device\BthModem…` rows
4. Connect `elm327://COMn` (that row) for about 30 s

**Expected:**
- [ ] Connect succeeds within a few seconds; the Host and Link lights
      change. Copy the "Session:" line: `ELM327 adapter (ELM327 v1.4b,
      STN2255 v…)` (or STN2256), `ISO 15765 at 500000 baud; profile
      obd2, channel 0x7E8/0x7E0 (rx/tx); battery …`. The battery within
      0.2 V of Phase 0's
- [ ] No "set not to wait past 7F xx 78" note. If it's there, record
      it: another app changed the adapter's parameter 2A, and Havne
      never writes them
- [ ] Nothing answers with the ignition off: quiet after a second or
      two ("Link quiet"; Live: "Vehicle not responding"), as through the
      VX; or "Requests failing" on a bus that sleeps. Record which, and
      from the wire log the time from a request to its `NO DATA` prompt
- [ ] Disconnect

A refused connect names the command and its answer: copy it verbatim
and stop (#12).

## Phase 2 — ignition on, engine off, then the start

**Action:** accessories off, ignition on. Connect `elm327://COMn` and
keep this connect through the start.

- [ ] The ECU line within a few seconds: `ECU 0x7E8: supports 53 PIDs;
      …, 1 dropped: 01 5C (not in its supported PIDs)` on this car
- [ ] In the wire log, a reply line: its ID as `7E8` or `07E8` (#14),
      and any `7F xx 78` line (#17)
- [ ] The identity line (personal data: scrub it), with the same items
      answered and refused as on 2026-10-25
- [ ] Engine-off values as [vehicle-runsheet.md](vehicle-runsheet.md)'s
      Phase 1 table; Battery Voltage (PID 0x42) within 0.2 V of the
      "Session:" line
- [ ] Start the engine within about two minutes of ignition on. RPM
      rises within a second. If the link drops in the crank, it should
      recover by itself: record how long

## Phase 3 — idling

- [ ] On the same connect, about a minute: RPM against the tachometer,
      Battery Voltage 13.4–14.5 V, Coolant Temp rising with the gauge.
      A photo of the cluster with the Dashboard. Disconnect
- [ ] The rate: `elm327://COMn?poll=0x0C` for about 15 s, RPM alone on
      Live. Disconnect. From the recording's `frames.csv`, the replies a
      second (26 through the VX)

## Phase 4 — a Bluetooth drop

**Action:** connect `elm327://COMn` at idle and wait for values. Turn
the laptop's Bluetooth off for about 10 s, then on.

- [ ] Off: values dim after 3 s, and red "Adapter fault" within about
      10 s (a write waits up to 2 s, then 5 s with no answer)
- [ ] On: values resume with no reconnect (the port reopens and the
      setup runs again). Record the time from Bluetooth on to the first
      value, and the lights. Over a minute is a finding: Disconnect and
      connect again
- [ ] Disconnect

## Phase 5 — optional

- [ ] The DID scan: `elm327://COMn?profile=uds-generic&did=F180-F1FF`
      until its ECU line settles (`scan: 128 DIDs, 2 found, 126
      refused` and 14.3 s through the VX). Personal data: no photos
- [ ] The VX in ELM mode: the MX+ out, the VX in by USB, its port from
      the bench sheet, `elm327://COMm` at idle for about 30 s. The same
      values as the MX+'s (#14 for the VX)

## If it fails, suspect

| Symptom | Suspect |
|---|---|
| No `COMn` after pairing, or "no such port" | The wrong row: the Outgoing port is the one to open |
| "COMn: the device behind the port didn't answer" | The MX+ not powered (the Power light), or paired to the phone |
| "didn't answer ATZ within 5 s" | The Bluetooth link not up: unpair, pair again |
| Values in Live but none the VX had, or implausible ones | Copy the activity log whole; compare with the 2026-10-25 sheet |
| Quiet with the engine running | The receive filter or the header: copy the activity log; try `?poll=0x0C&txid=0x7DF` |
| The app crashes | Abort as in the vehicle sheet, and save the activity log |

## Teardown and post-session

1. Disconnect, unplug the MX+, key off
2. As [vehicle-runsheet.md](vehicle-runsheet.md)'s post-session
   actions: copy this sheet, filled in, with its photos and wire logs,
   scrubbed (no VIN, CALID or serials: the identity line, Phase 5's
   scan, and the identification replies in the wire logs); the
   recordings stay on the laptop
3. Mark BENCH prep #12–#17 for the MX+ in [bench-prep.md](bench-prep.md),
   and record the rate and the drop's recovery time in ADR-0026 as a
   dated appendix
4. Update STATUS.md and testing.md's Vehicle tier and gaps
