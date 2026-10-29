Havne for Windows (x64)
========================

Havne reads and logs live data from a car's engine computer through a
J2534 Pass-Thru adapter, and shows it as gauges, a strip chart and a raw
CAN frame log. A built-in simulated car lets you try every page with no
hardware at all.

Read DISCLAIMER.md before connecting it to a vehicle.


Running it
----------

1. Unzip the whole folder anywhere you like. Nothing is installed.
2. Run havne.exe.

   The program isn't code-signed, so Windows SmartScreen may say it
   "protected your PC". Choose "More info", then "Run anyway".

3. To see it work with no hardware: in Settings, tick "Developer mode"
   under Display, then press the "mock://demo" preset under Adapter,
   then Connect. The simulated car warms up, then drives a loop
   of pulls, a highway cruise and a quarter mile. The Dashboard, Live
   and Log pages all follow it.

To remove Havne, delete the folder. It keeps its preferences (the
display, the last connection, your vehicles) under
HKEY_CURRENT_USER\Software\Havne, and any sessions you recorded under
Documents\Havne\recordings.

Havne was called QtTune before version 0.3.3. Its first launch copies
QtTune's preferences (HKEY_CURRENT_USER\Software\QtTune) once, and
Settings' recent recordings list the sessions under
Documents\QtTune\recordings beside the new ones. Neither is moved or
deleted: remove them by hand once you no longer need them.


Recording and replaying a session
---------------------------------

- Turn on "Record sessions" in Settings before you connect. Each
  connect then writes a folder under Documents\Havne\recordings: every
  frame, every decoded value and the activity log.
- To play one back, with no adapter or car: in Settings, enter
  replay://<that folder's full path>?profile=obd2 (or ?profile=mock for
  a recording of the simulated car) and Connect. Add &speed=10 to play
  it ten times faster, or &loop=1 to play it again and again.


Using an adapter
----------------

- Install your adapter vendor's J2534 driver first. Havne finds
  installed drivers and lists them in Settings.
- Each vendor driver runs in a helper process of its own: 32-bit
  drivers in ecuio-j2534-surrogate.exe, 64-bit ones in
  ecuio-j2534-surrogate-x64.exe. Both must stay next to havne.exe.
- Havne only reads: it sends standard diagnostic read requests (live
  data and identification) and never changes anything in the vehicle.
  It has been tested read-only on one vehicle with one adapter; other
  vehicles and adapters are untested.


What's in this folder
---------------------

havne.exe                  the application
ecuio-j2534-surrogate.exe   the 32-bit helper for 32-bit J2534 drivers
ecuio-j2534-surrogate-x64.exe
                            the 64-bit helper for 64-bit J2534 drivers
Qt6*.dll, qml\, platforms\  the Qt runtime (see THIRD_PARTY_NOTICES.txt)
vcruntime140*.dll, msvcp140*.dll
                            the Microsoft Visual C++ runtime, so no
                            separate redistributable is needed
LICENSE.txt                 Havne's license (MIT)
THIRD_PARTY_NOTICES.txt     the licenses of what Havne ships with
licenses\                   the full LGPL-3.0 and GPL-3.0 texts


Source, documentation and issues: https://github.com/erickramer42/havne
