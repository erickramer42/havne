# Android phone run sheet — the app on a phone, the VX's WiFi, then Bluetooth

**Date:** <date>  **Phone:** Android <version> (12 or later)
**Adapter:** OBDX Pro VX over WiFi, no vehicle (Phases 1–6); the
OBDLink MX+ on the test car (Phase 7)
**Havne build:** <commit hash> (CI run <run id>)

Fill in a copy as `docs/bench-sessions/<date>-android[-n].md`. Phases
1–5 are step 1 of [ADR-0028](../adr/0028-app-fed-streams.md), the
Android baseline (Known Issue #2): the app, and its first route to a
car, `elm327://` over WiFi, which needs no Bluetooth code. Phase 6 is
step 3, the phone's Bluetooth without an adapter. Phase 7 is step 4,
the MX+ through the phone on the car: run it on its own day, with
Phase 1's install and nothing else. Phase 8 is background sessions
(ADR-0029): the app keeps reading behind another app. The VX's WiFi on a
computer is [elm327-bench-runsheet.md](elm327-bench-runsheet.md)'s
pre-flight and BENCH prep #20.

## Purpose

Goals, in order:

1. The CI APK installs and the app runs at phone size (BENCH prep #21)
2. `mock://` streams; a recording is written, and found again
   (#21, #23)
3. `elm327://192.168.4.1:23` reaches the VX from the phone (#22)
4. Bluetooth: the permission, the paired list, a BLE scan, and a
   connect refused as it should be (#24, ADR-0028 step 3). An adapter
   over Bluetooth is the car's run (step 4): the MX+ takes its power
   from the car only

The evidence is the activity log (the Log page; copy it whole after
each phase) and screenshots. The ELM327 wire log stays in the app's
private storage: Phase 5 copies it off with `adb`, if a computer with
`adb` is at hand.

## Pre-flight

- [ ] The CI run of the build is green on `android-build`. Download
      its `havne-android-arm64-v8a` artifact (the run's page, or
      `gh run download <run id> -n havne-android-arm64-v8a`) and unzip
      the APK
- [ ] A previous Havne on the phone uninstalled: each CI run signs
      with a new debug key, and Android refuses the update
- [ ] A QtTune on the phone (package `org.qtproject.example.qttune`,
      the name before the rename) is another app: its recordings
      copied off first if they're wanted (Phase 5's `adb run-as`
      with that package, `files/Documents/QtTune/recordings`), then
      it's uninstalled by hand. Havne starts with its own empty
      preferences and recordings
- [ ] The VX power-cycled since its last USB session or unclean WiFi
      leave, then powered from a USB charger, nothing holding its USB
      port (BENCH prep #20: one link at a time)

## Phase 1 — install and launch

**Action:** copy the APK to the phone and open it. Allow installs from
the app that opens it when asked; Play Protect may warn about an
unknown developer ("Install anyway").

- [ ] It installs. Copy any warning's text
- [ ] It launches: the Home page, the tab bar at the bottom (the
      sidebar shows only from 720 px wide). Copy the time from the tap
      to the first page drawn, roughly
- [ ] Each page opens from the tab bar: Home, Dashboard, Live, Log,
      Settings. Screenshot each. Note text that is cut off or controls
      too small to press
- [ ] Turn the phone to landscape and back: the page lays out again,
      filling the screen. Note any band of another color (white or
      light gray) around the app, and where; the navigation bar's
      buttons should be light on dark

## Phase 2 — mock

**Action:** Settings → **Display** → **Developer mode** on, then
**Adapter** → **Mock**, then
**Connect**.

- [ ] Turn the phone to landscape and back once more, then on the
      Log page copy the "Display:" lines (developer mode logs the
      screen, the window and the safe area after each change)
- [ ] Connected within a second; the Log page's "Session:" line reads
      "mock transport"
- [ ] Live's values move and the chart draws; the Dashboard's gauges
      move. Screenshot both
- [ ] Press Home (the app goes to the background) for about 30 s, then
      return: still connected, values moving. Note what the activity
      log says about the gap
- [ ] Turn the screen off for about 30 s and back on: the same check
- [ ] Disconnect: "Disconnected:" at once

## Phase 3 — a recording, then its replay

**Action:** Settings → **Recording** → **Record sessions** on, connect `mock://demo`
for about 20 s, disconnect.

- [ ] The activity log's "Recording:" line. Copy it: it says where
      Android put the recordings (BENCH prep #23). An error instead
      (the folder couldn't be made): copy it, and skip the replay
- [ ] Settings → **Recording**, its recent recordings: the newest row has this
      recording's time and "mock://demo · replays as mock". Select it:
      the field gets its `replay://` address. Connect, and confirm the
      values play back
- [ ] **Record sessions** off again

## Phase 4 — the VX's WiFi

**Action:** turn the phone's mobile data off. Join the VX's network
("OBDX Pro VX <MAC>"); when Android says it has no internet, choose
to stay connected. In Havne, Settings: `elm327://192.168.4.1:23?profile=raw&poll=0x0C`,
**Connect**.

- [ ] Connects within a few seconds. Copy the "Session:" line: on
      USB power alone, "ELM327 adapter (ELM327 V2.1); …; battery
      0.0 V", as on the computer
- [ ] Amber "Link quiet" within about 3 s (no bus: every request
      `NO DATA`, BENCH prep #15), no fault, for about 30 s
- [ ] Disconnect in Havne **before** leaving the VX's network (an
      unclean leave hangs the VX's network side until a power cycle,
      BENCH prep #20)
- [ ] Mobile data back on, still on the VX's network: connect the same
      URI once more. Record whether it connects, or fails "no answer
      within 2 s": Android may send the socket over mobile data, which
      can't reach 192.168.4.1. Disconnect

## Phase 5 — the files (with `adb`; optional)

**Action:** the phone in developer mode with USB debugging on, on a
computer with `adb`. The package is `io.github.erickramer42.havne`
(`adb shell pm list packages havne` prints it after `package:`,
which isn't part of the name).

- [ ] `adb shell run-as io.github.erickramer42.havne ls -R cache files`:
      copy the listing.
      The wire logs are `ecuio-elm327.*.log` in the temporary folder;
      note which directory holds them
- [ ] Copy Phase 4's wire log off:
      `adb exec-out run-as io.github.erickramer42.havne cat cache/<log's name> > <log's name>`,
      once for each `ecuio-elm327.*.log` the listing shows
- [ ] If Phase 3 couldn't find its recording: `find` it here, and
      record the full path

## Phase 6 — Bluetooth, no adapter

**Action:** Bluetooth on. Settings: open **Adapter**.

- [ ] Android asks for the Nearby devices permission once, when
      Adapter opens (not at launch). Allow it
- [ ] The device list shows the phone's paired Bluetooth devices,
      each "Bluetooth, paired · <address>" (headphones and the like
      too: the list can't tell an adapter without connecting). Copy
      the count line
- [ ] **Scan for BLE**: the button reads "Scanning…" for about 8 s;
      BLE devices nearby join the list as "Bluetooth LE · <address>",
      or the line under it says the scan found none
- [ ] Select a paired device that isn't an adapter (headphones, a
      watch): the page scrolls back to the URI field, which now holds
      `elm327://bt:<address>`, and the row is outlined. Then
      **Connect**. Record how long until the error line appears and
      its words: the link refused, "the link didn't open within 10 s",
      or, from a device that offers a serial service (earbuds often
      do), "didn't answer ATZ within 5 s". The app stays responsive
      throughout
- [ ] The same with a BLE row from the scan
- [ ] Turn Bluetooth off, then **Refresh**: the line under the count
      reads "Bluetooth is off: turn it on, then Refresh". Bluetooth
      back on
- [ ] Optional: Android's settings, Apps, Havne, Permissions, Nearby
      devices: Don't allow. Back in Havne, **Refresh**: the line says
      to allow it. Allow it again

## Phase 7 — the MX+ on the car (ADR-0028 step 4)

The MX+ over the phone's classic Bluetooth, held against the same
adapter through the laptop
([2026-10-27-elm327-vehicle](2026-10-27-elm327-vehicle.md)): a
difference is the finding. The phone's own wire log and recordings are
the evidence, so a computer with `adb` afterwards, or Phase 5's
copies.

**Pre-flight:**
- [ ] [vehicle-runsheet.md](vehicle-runsheet.md)'s "Safety and scope"
      holds in full: read-only, parked, ignition on with the engine off
      kept short, the abort order (Disconnect, then unplug the MX+)
- [ ] The phone charged, its screen kept on for the run (Display,
      Screen timeout, the longest), Do not disturb on
- [ ] The laptop's Bluetooth off, or the MX+ unpaired from it, and the
      OBDLink app closed: the MX+ takes one connection at a time
- [ ] The phone's earlier recordings backed up, if a reinstall comes
      first (uninstalling deletes them)
- [ ] Havne: Settings, Recording, **Record sessions** on

**7a — pair, ignition off.**

**Action:** plug in the MX+. Pair it in Android's Bluetooth settings
(Pair new device; press the MX+'s button when asked). In Havne,
Settings, Adapter, **Refresh**: the MX+ row, "Bluetooth, paired ·
<address>". Select it, then **Connect** (`elm327://bt:<address>`),
for about 30 s.

- [ ] It connects within a few seconds. Copy the "Session:" line:
      `ELM327 adapter (ELM327 v1.4b, STN2255 v5.12.2); ISO 15765 at
      500000 baud; profile obd2, …; battery …` (12.1 V through the
      laptop with the ignition off)
- [ ] Quiet with the ignition off, as through the laptop ("Link
      quiet"). Disconnect

**7b — ignition on, the start, idling.**

**Action:** ignition on, engine off. Connect the MX+ row and keep this
connect through the start and a minute of idling.

- [ ] The ECU line within a few seconds: `ECU 0x7E8: supports 53
      PIDs; …, 1 dropped: 01 5C (not in its supported PIDs)`
- [ ] The identity line (personal data: never photographed, scrubbed
      from the sheet)
- [ ] Start the engine within about two minutes. RPM rises within a
      second on Live and the Dashboard. A drop in the crank recovers by
      itself: record how long
- [ ] Idling: RPM against the tachometer, Battery Voltage 13.4–14.5 V,
      Coolant Temp rising with the gauge. A screenshot of the Dashboard
      (no identity line in view). Disconnect
- [ ] The rate: put `?poll=0x0C` after the address in the field,
      Connect for about 15 s, Disconnect. The replies a second come
      from the recording (~32/s through the laptop)

**7c — a Bluetooth drop, idling.**

**Action:** connect the MX+ row (without `?poll=`) and wait for
values. Turn the phone's Bluetooth off from the quick settings for
about 10 s, then on, and come back to Havne.

- [ ] Off: values dim after 3 s, and the link turns red ("Adapter
      fault") within about 10 s
- [ ] On: values resume with no reconnect. Record the time from
      Bluetooth on to the first value (~6.5 s through the laptop). Over
      a minute is a finding: Disconnect and connect again

**7d: the app in the background (optional; Phase 8 runs it in full).**

**Action:** connected and idling, press Home for about 20 s, then
return to Havne.

- [ ] The link survives (no fault, or one that recovers by itself),
      the session's notification shows while Havne is in the
      background, and Live's chart has no gap. Record what the link did
- [ ] Disconnect, unplug the MX+, key off

## Phase 8: background sessions (ADR-0029)

The app keeps running in the background, with a notification while an
adapter session is up. 8a at
the desk, 8b on the car. The phone's log (`adb logcat`) and the
recordings are the evidence, so a computer with `adb` afterwards.

**8a — at the desk, `mock://`.**

**Action:** Settings, Recording, **Record sessions** on; Display,
**Developer mode** on. Connect `mock://demo`, wait for values.

- [ ] No notification for the mock (only an adapter session gets one)
- [ ] Press Home for about 2 minutes. Return: still connected, no
      fault in the activity log. Live's chart may have a gap: with no
      service, Android may freeze the app or let the CPU sleep, as it
      decides ([2026-10-28-android-9](2026-10-28-android-9.md); mock
      and replay are desk tools)
- [ ] The screen off for about 5 minutes (the power button). On: the
      same, and the app returned without a crash. Note the times the
      screen went off and on
- [ ] The sleep in session time (BENCH prep #27, ADR-0033): with no
      wake lock under mock, the CPU sleeps with the screen off. The
      activity log says "Asleep for N s: nothing was read meanwhile",
      N about the time the CPU slept (at most the screen-off time), and
      Live's chart has a gap that long. No "Asleep" line: note it, and
      whether values kept moving through the screen-off (then the CPU
      didn't sleep, and #27 stays open)
- [ ] Disconnect. Copy the recording's Delivery line (events.log,
      Phase 5's copy or `adb`)
- [ ] Afterwards, with `adb`: in the recording, `signals.csv`'s
      `host_us` jumps by about N s where the "Asleep" line falls, and
      events.log's two columns (the clock, and "+seconds") agree
      within a second across the screen-off

**8b — on the car, the MX+, engine idling.** Phase 7's pre-flight and
safety hold.

**Action:** connect the MX+ row, **Record sessions** on.

- [ ] At the first adapter connect Android asks to allow
      notifications: allow it. A notification "Havne — Connected to
      …" with "· recording", for as long as the session is up, and
      the gauge icon in the status bar, with no sound or vibration
- [ ] The notification and the header name the adapter as
      `elm327://bt:…:` and the address's last two parts only, never
      the whole address
- [ ] Press Home for about 2 minutes, then return. The link stayed up
      the whole time (no "Link: adapter fault" in the activity log),
      Live has no gap, the values kept recording
- [ ] On Live, with Havne in front, the screen off for about 5
      minutes (the power button), then on: the same. This is the case
      that stopped the GUI thread
      ([2026-10-28-android-10](2026-10-28-android-10.md)). Note the
      times the screen went off and on
- [ ] Afterwards, with `adb`: the recording's Delivery line (events.log,
      at the disconnect), "to the GUI thread … max" well under a second
- [ ] Afterwards, with `adb`: in the recording's events.log, the
      time between two lines by the clock (the first column) and by
      the session ("+seconds") agree within a second across the
      screen off: the session clock counts a sleep (ADR-0033). The
      wake lock keeps the CPU awake, so no "Asleep" line is expected;
      one means the CPU slept despite it (note N)
- [ ] With the screen off, Bluetooth off for about 10 s from the quick
      settings, then on. Back in Havne: a fault, then recovered on its
      own, as in 7c
- [ ] Disconnect: the notification goes at once
- [ ] Disconnect, unplug the MX+, key off

## If it fails, suspect

| Symptom | Suspect |
|---|---|
| "App not installed" | A previous Havne signed with another key: uninstall it first |
| It closes at launch | Copy `adb logcat -d` around the launch if `adb` is at hand; otherwise note the Android version and stop |
| A blank or black window | The graphics backend: note the phone's chip if known, and stop |
| "no answer within 2 s" on WiFi with mobile data off | The phone isn't on the VX's network, or the VX's network side hung (power-cycle it) |
| "the network is unreachable" or a permission error on WiFi | `INTERNET` missing from the manifest, which CI checks: copy the error whole |
| No permission prompt, and the list says to allow Nearby devices | The permission was refused earlier: allow it in Android's settings |
| The MX+ "didn't answer ATZ within 5 s" | Its Power light off (it takes power from the car only), or the laptop or the OBDLink app holding it: close them, connect again |
| No paired devices listed with Bluetooth on | Copy the count line and the line under it; `adb logcat -d | grep -i -E "havne|bluetooth"` if `adb` is at hand |

## Post-session actions

1. Copy this sheet, filled in, to `docs/bench-sessions/<date>-android.md`,
   the screenshots and the wire log to a folder beside it. Scrub them
   as [bench-runsheet.md](bench-runsheet.md) says: the VX's MAC (in its
   network's name), no names, no paths with an account name
2. Mark BENCH prep #21–#24 in [bench-prep.md](bench-prep.md). Scrub
   Bluetooth addresses from the sheet and screenshots: `<BT address>`.
   After Phase 7, also the identity line, and the wire log's
   identification replies (`49 02` VIN, `49 04` CALID, `62 F1 xx`
   serials), as [elm327-vehicle-runsheet.md](elm327-vehicle-runsheet.md)
   says
3. Update Known Issue #2 in STATUS.md, and testing.md's gaps
