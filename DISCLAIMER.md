# DISCLAIMER — READ BEFORE USING

**This software may damage your vehicle's control modules.**

QtTune is a cross-platform ECU diagnostic and tuning application in
early development. When vehicle communication and flashing features
are added, errors in this software, its configuration, or your use of
it may result in:

- Corrupted firmware or calibration ("bricked" modules)
- Engine or transmission damage
- Disabled or degraded safety systems (ABS, TCS, stability control)

**Back up your ECU contents before any session where writes are
possible. Never flash a vehicle with an unstable power supply.**

## Current State

This project is in early development. Through a J2534 Pass-Thru
adapter (Windows only), it can send standard, read-only OBD-II Mode-01
requests ("show current data") and display the replies. That path has
been tested on the bench, against a simulated adapter and a real
adapter with no vehicle attached, and in one read-only session on one
vehicle with one adapter. It has **not** been validated beyond that.
QtTune has **no** capability to write, clear codes or flash any ECU.
Do not connect it to a vehicle expecting reliable results.

## Intended Purpose

- **Diagnostics:** Read and log vehicle data
- **Calibration:** View and edit maps for supported vehicles
- **Research:** Understand diagnostic protocol behavior

## Not Intended For

- Defeating emissions controls or safety interlocks
- Circumventing manufacturer security in violation of any law,
  license agreement, or regulation
- Road use of modified calibrations where not street-legal

## Regulatory Notice

Modifying engine control software may violate:

- US: 40 CFR Part 86 (Clean Air Act prohibitions on emissions
  defeat devices)
- EU: type-approval requirements
- Other jurisdictions: local emissions and vehicle-modification laws

Vehicle modifications may void your powertrain warranty.

## Warranty & Liability

THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.
The authors accept no responsibility for vehicle damage, warranty
denials, regulatory violations, or any consequential losses arising
from use of this software. See LICENSE for full terms.

---

By downloading or using this software, you acknowledge that you have
read and understood these warnings.
