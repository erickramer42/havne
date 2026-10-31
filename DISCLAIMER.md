# DISCLAIMER: READ BEFORE USING

**This software may damage your vehicle's control modules.**

Havne is a cross-platform ECU reader and logger in early development.
It reads, and it can clear trouble codes; writing and flashing are on
the [roadmap](STATUS.md#roadmap). Clearing already changes the car.
Errors in this software, its configuration, or your use of it, in
clearing now and in writing if it is added, may result in:

- Corrupted firmware or calibration ("bricked" modules)
- Engine or transmission damage
- Disabled or degraded safety systems (ABS, TCS, stability control)
- Cleared emissions readiness monitors, so the vehicle fails an
  inspection until it has been driven enough for them to complete
  again (clearing codes does this, every time it succeeds)

**Back up your ECU contents before any session where writes are
possible. Never flash a vehicle with an unstable power supply.**

## Current state

Havne sends diagnostic reads: OBD-II Services 01 ("current data"), 02
("freeze frame data"), 03, 07 and 0A (the stored, pending and
permanent trouble codes) and 09 ("vehicle information"), and UDS
Service 0x22 ("read data by identifier")
([ADR-0015](docs/adr/0015-request-engine.md),
[ADR-0037](docs/adr/0037-reading-trouble-codes.md)).

It sends one request that changes the car: OBD-II Service 04 ("clear
diagnostic information"), only from the Diagnostics page's **Clear
codes**, after a confirmation
([ADR-0038](docs/adr/0038-clearing-trouble-codes.md)). Before it,
Havne saves the report to a file of its own in the recordings folder
and sends nothing if it can't; then it asks each ECU its engine speed
and sends nothing to one whose engine turns. It clears each ECU by its
own address, never every module at once. On an ECU that obeys, a clear
erases the stored and pending codes and the freeze frame, turns the
check-engine light off and sets the readiness monitors back to
incomplete; permanent codes stay. **Clearing has run only against
Havne's fakes and its simulated vehicle, never on a car.**

It refuses every other service, and has **no** capability to write or
flash any ECU. It reaches a vehicle through:

- a **J2534 Pass-Thru** adapter, on Windows only;
- an **ELM327-compatible** adapter over USB, Bluetooth or WiFi, on
  Windows, Linux and macOS;
- an **Android phone** (Android 12 or later) and a Bluetooth adapter.

It has been tested against its simulated vehicle and fake adapters, on
the bench with real adapters and no vehicle, and in read-only sessions
on **one** vehicle with **two** adapters (the OBDX Pro VX, as a J2534
adapter and in its ELM327 mode, and the OBDLink MX+ over Bluetooth),
from a laptop and from an Android phone. It has **not** been validated
beyond that: other vehicles, other adapters, Bluetooth LE adapters,
29-bit CAN addressing, several ECUs answering at once and the iOS app
on a device are all untested. [STATUS.md](STATUS.md) tracks what is
and isn't verified. Do not rely on its readings for safety-related
decisions.

## Using it on a vehicle

- **Don't operate the app while driving.** Set up the session before
  you set off, mount the phone or laptop where it can't fall into the
  footwell or under the pedals, and leave anything you need to
  operate to a passenger. Follow your local law on device use behind
  the wheel.
- **Route the adapter and its cable** clear of the pedals, the
  steering column and the driver's knees.
- **Clear codes only once you've read them and mean to.** What a clear
  erases can't be read back; Havne keeps the report in the recordings
  folder, but the car's readiness monitors start over and take some
  driving to complete before an emissions inspection.
- **A session keeps the adapter awake.** While it's connected, Havne
  asks the engine ECU for data continuously, and on Android it keeps
  running in the background and with the screen off. An adapter left
  plugged in, or a session left running with the engine off, drains
  the vehicle's battery. Disconnect when you're done, and unplug an
  adapter that doesn't sleep on its own.
- **Requests are traffic on the vehicle's bus.** They are the
  requests a scan tool sends, paced by the ECU's replies, but any
  device on the diagnostic port carries some risk. A module that
  behaves unusually while Havne is connected is a reason to
  disconnect.

## Your data

A session can read your vehicle's VIN and its ECUs' identifiers
(calibration IDs, serial numbers). Havne shows them in the activity
log, and, when **Record sessions** is on, writes them into the
recording's files on your device (`Documents/Havne/recordings` on a
desktop). An ELM327 session also writes a wire log to the system's
temporary folder. Havne sends nothing over the network except to the
adapter you connect to, and on Android it keeps its files out of the
phone's backup and out of a transfer to a new phone. Before you share
a recording, a log or a
screenshot, see [CONTRIBUTING.md](CONTRIBUTING.md#handling-vehicle-data)
for what to remove.

## Intended purpose

- **Diagnostics:** read and log vehicle data
- **Calibration (planned, not built):** view and edit maps for
  supported vehicles
- **Research:** understand diagnostic protocol behavior

## Not intended for

- Defeating emissions controls or safety interlocks
- Circumventing manufacturer security in violation of any law,
  license agreement, or regulation
- Road use of modified calibrations where not street-legal

## Regulatory notice

Modifying engine control software may violate:

- US: 40 CFR Part 86 (Clean Air Act prohibitions on emissions
  defeat devices)
- EU: type-approval requirements
- Other jurisdictions: local emissions and vehicle-modification laws

Vehicle modifications may void your powertrain warranty.

## Warranty and liability

THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.
The authors accept no responsibility for vehicle damage, warranty
denials, regulatory violations, or any consequential losses arising
from use of this software. See LICENSE for full terms.

---

By downloading or using this software, you acknowledge that you have
read and understood these warnings.
