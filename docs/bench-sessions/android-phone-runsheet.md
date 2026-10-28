# Android phone run sheet — the app on a phone, then the VX's WiFi

**Date:** <date>  **Phone:** Android <version>
**Adapter:** OBDX Pro VX over WiFi, no vehicle
**QtTune build:** <commit hash> (CI run <run id>)

Fill in a copy as `docs/bench-sessions/<date>-android.md`. This is
step 1 of [ADR-0028](../adr/0028-app-fed-streams.md): the Android app
has never been launched (Known Issue #2), and its first route to a
car, `elm327://` over WiFi, needs no Bluetooth code. The VX's WiFi on
a computer is [elm327-bench-runsheet.md](elm327-bench-runsheet.md)'s
pre-flight and BENCH prep #20.

## Purpose

Goals, in order:

1. The CI APK installs and the app runs at phone size (BENCH prep #21)
2. `mock://` streams; a recording is written, and found again
   (#21, #23)
3. `elm327://192.168.4.1:23` reaches the VX from the phone (#22)

The evidence is the activity log (the Log page; copy it whole after
each phase) and screenshots. The ELM327 wire log stays in the app's
private storage: Phase 5 copies it off with `adb`, if a computer with
`adb` is at hand.

## Pre-flight

- [ ] The CI run of the build is green on `android-build`. Download
      its `qttune-android-arm64-v8a` artifact (the run's page, or
      `gh run download <run id> -n qttune-android-arm64-v8a`) and unzip
      the APK
- [ ] A previous QtTune on the phone uninstalled: each CI run signs
      with a new debug key, and Android refuses the update
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
to stay connected. In QtTune, Settings: `elm327://192.168.4.1:23?profile=raw&poll=0x0C`,
**Connect**.

- [ ] Connects within a few seconds. Copy the "Session:" line: on
      USB power alone, "ELM327 adapter (ELM327 V2.1); …; battery
      0.0 V", as on the computer
- [ ] Amber "Link quiet" within about 3 s (no bus: every request
      `NO DATA`, BENCH prep #15), no fault, for about 30 s
- [ ] Disconnect in QtTune **before** leaving the VX's network (an
      unclean leave hangs the VX's network side until a power cycle,
      BENCH prep #20)
- [ ] Mobile data back on, still on the VX's network: connect the same
      URI once more. Record whether it connects, or fails "no answer
      within 2 s": Android may send the socket over mobile data, which
      can't reach 192.168.4.1. Disconnect

## Phase 5 — the files (with `adb`; optional)

**Action:** the phone in developer mode with USB debugging on, on a
computer with `adb`. The package is `org.qtproject.example.qttune`
(`adb shell pm list packages qttune` prints it after `package:`,
which isn't part of the name).

- [ ] `adb shell run-as org.qtproject.example.qttune ls -R cache files`:
      copy the listing.
      The wire logs are `ecuio-elm327.*.log` in the temporary folder;
      note which directory holds them
- [ ] Copy Phase 4's wire log off:
      `adb exec-out run-as org.qtproject.example.qttune cat cache/<log's name> > <log's name>`,
      once for each `ecuio-elm327.*.log` the listing shows
- [ ] If Phase 3 couldn't find its recording: `find` it here, and
      record the full path

## If it fails, suspect

| Symptom | Suspect |
|---|---|
| "App not installed" | A previous QtTune signed with another key: uninstall it first |
| It closes at launch | Copy `adb logcat -d` around the launch if `adb` is at hand; otherwise note the Android version and stop |
| A blank or black window | The graphics backend: note the phone's chip if known, and stop |
| "no answer within 2 s" on WiFi with mobile data off | The phone isn't on the VX's network, or the VX's network side hung (power-cycle it) |
| "the network is unreachable" or a permission error on WiFi | `INTERNET` missing from the manifest, which CI checks: copy the error whole |

## Post-session actions

1. Copy this sheet, filled in, to `docs/bench-sessions/<date>-android.md`,
   the screenshots and the wire log to a folder beside it. Scrub them
   as [bench-runsheet.md](bench-runsheet.md) says: the VX's MAC (in its
   network's name), no names, no paths with an account name
2. Mark BENCH prep #21–#23 in [bench-prep.md](bench-prep.md)
3. Update Known Issue #2 in STATUS.md, and testing.md's gaps
