QtTune for Windows (x64)
========================

QtTune reads and logs live data from a car's engine computer through a
J2534 Pass-Thru adapter, and shows it as gauges, a strip chart and a raw
CAN frame log. A built-in simulated car lets you try every page with no
hardware at all.

Read DISCLAIMER.md before connecting it to a vehicle.


Running it
----------

1. Unzip the whole folder anywhere you like. Nothing is installed.
2. Run qttune.exe.

   The program isn't code-signed, so Windows SmartScreen may say it
   "protected your PC". Choose "More info", then "Run anyway".

3. To see it work with no hardware: Settings, then the "mock://demo"
   preset, then Connect. The simulated car warms up, then drives a loop
   of pulls, a highway cruise and a quarter mile. The Dashboard, Live
   and Log pages all follow it.

To remove QtTune, delete the folder. It keeps two preferences (zoom
and units) under HKEY_CURRENT_USER\Software\QtTune.


Using an adapter
----------------

- Install your adapter vendor's J2534 driver first. QtTune finds
  installed drivers and lists them in Settings.
- 32-bit vendor drivers run through ecuio-j2534-surrogate.exe, which
  must stay next to qttune.exe.
- QtTune only reads: it sends standard OBD-II requests for live data
  and never changes anything in the vehicle. It has been tested read-only on one vehicle with one
  adapter; other vehicles and adapters are untested.


What's in this folder
---------------------

qttune.exe                  the application
ecuio-j2534-surrogate.exe   the 32-bit helper for 32-bit J2534 drivers
Qt6*.dll, qml\, platforms\  the Qt runtime (see THIRD_PARTY_NOTICES.txt)
vcruntime140*.dll, msvcp140*.dll
                            the Microsoft Visual C++ runtime, so no
                            separate redistributable is needed
LICENSE.txt                 QtTune's licence (MIT)
THIRD_PARTY_NOTICES.txt     the licences of what QtTune ships with
licenses\                   the full LGPL-3.0 and GPL-3.0 texts


Source, documentation and issues: https://github.com/erickramer42/qttune
