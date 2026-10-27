# DISCLAIMER — READ BEFORE USING

**This software may damage your vehicle's control modules.**

QtTune is a cross-platform ECU diagnostic and tuning application in
early development. It reads only today. If writing and flashing
features are added, errors in this software, its configuration, or
your use of it may result in:

- Corrupted firmware or calibration ("bricked" modules)
- Engine or transmission damage
- Disabled or degraded safety systems (ABS, TCS, stability control)

**Back up your ECU contents before any session where writes are
possible. Never flash a vehicle with an unstable power supply.**

## Current state

This project is in early development. Through a J2534 Pass-Thru
adapter (Windows only), it sends read-only diagnostic requests (OBD-II
Service 01 "current data" and Service 09 "vehicle information", and
UDS Service 0x22 "read data by identifier") and displays the replies.
That path has been tested against a simulated adapter, on the bench
with a real adapter and no vehicle, and in two read-only sessions on
one vehicle with one adapter. It has **not** been validated beyond
that.
QtTune has **no** capability to write, clear codes or flash any ECU.
Do not connect it to a vehicle expecting reliable results.

## Intended purpose

- **Diagnostics:** read and log vehicle data
- **Calibration (planned, not built):** view and edit maps for supported vehicles
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
