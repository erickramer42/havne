# ADR-0029: Android background sessions — the GUI thread keeps running, a foreground service keeps the process, one Bluetooth connect at a time

- **Status:** Proposed.
- **Date:** 2026-10-28.
- **Closes:** Known Issue #37 (the app in the background), once the
  phone runs in §5 pass.
- **Builds on:** [ADR-0028](0028-app-fed-streams.md) (the app's
  Bluetooth link, whose calls run on the GUI thread);
  [ADR-0017](0017-session-recording.md) (the recorder, on the GUI
  thread); [ADR-0003](0003-backpressure-bounds-in-flight-frames.md)
  (the in-flight cap).

## Context

A logger in a car runs with the screen off. On Android today, leaving
the app stops three things that live on the GUI thread:

- **The bridge's 100 ms windows**: Live's chart has a gap, and frames
  queue for the GUI thread until the in-flight cap drops them (`mock://`
  reaches it in 50 s, the MX+ at ~36 frames a second in under 30 s), so
  a recording loses them
  ([2026-10-28-android-3](../bench-sessions/2026-10-28-android-3.md)).
- **The Bluetooth link**: core's writes wait in the queue, so the
  session faults on core's 5 s silence. On return the queued calls run
  at once and the recovery took 38 s
  ([2026-10-28-android-8](../bench-sessions/2026-10-28-android-8.md)).
- **The activity log and link state**, which the bridge's tick drives.

Core's own threads keep running (the ELM327 worker timed the 5 s
silence and its reopens on schedule in the background), and so does a
WiFi adapter's socket, which core owns.

**Why the GUI thread stops** (Qt 6.12's sources):

- Qt's Java loader sets `QT_BLOCK_EVENT_LOOPS_WHEN_SUSPENDED=1` unless
  the manifest's `android.app.background_running` meta-data is `true`
  (`QtLoader.java`).
- With it set, every `QAndroidEventDispatcher` is stopped when the app
  becomes `Qt::ApplicationSuspended` (`androidjnimain.cpp`,
  `updateApplicationState`), and its loop waits on a semaphore until
  the app returns (`qandroideventdispatcher.cpp`).
- Only the GUI thread gets that dispatcher (the platform plugin's
  `createEventDispatcher`); a `QThread` gets `QEventDispatcherUNIX`,
  which is never stopped (`qthread_unix.cpp`).
- Qt documents the meta-data, with a warning: an app that draws after
  it is suspended may crash.

**Android's side:** a process with no visible activity and no
foreground service may be frozen or stopped (the cached-apps freezer,
Android 14 and later, and the maker's own battery rules), core's
threads included. A foreground service, with its notification, is how
an app tells Android a long task is under way. The `connectedDevice`
type fits: it needs `FOREGROUND_SERVICE` and
`FOREGROUND_SERVICE_CONNECTED_DEVICE` in the manifest, and one of
`BLUETOOTH_CONNECT` granted (the Nearby devices permission the app
already asks for) or `CHANGE_NETWORK_STATE` declared (for a WiFi
adapter). A foreground service can't be started from the background,
so it starts at the connect, which is always made in the foreground.

**QtBluetooth's side** (`qbluetoothsocket_android.cpp`, 6.12): a
classic socket connects on a worker thread through Java's blocking
`BluetoothSocket.connect()`. `abort()` queues the Java `close()` to
that same worker, so it runs only once the connect returns: an abort
can't cut a connect short. The link's `close` aborts and deletes the
socket, and core's next open starts a second connect beside the first;
the run saw one block 25 s and a second start beside it.

## Decision

1. **The GUI thread keeps running in the background.** The app ships
   its own `app/android/AndroidManifest.xml` (Qt 6.12's template, its
   `%%INSERT_…%%` placeholders kept) with
   `android.app.background_running` set to `true`. The bridge's
   windows, the recorder, the activity log, link state and the
   Bluetooth link run on as they do in the foreground. Qt Quick stops
   rendering while no window is exposed; nothing in the app draws by
   hand.
2. **A foreground service while an adapter session is up.** A small
   Java service in the APK
   (`app/android/src/…/SessionService.java`, type `connectedDevice`)
   holds a low-priority notification: "QtTune — connected to <the
   URI's label>", and "recording" when it is; a tap opens the app. An
   app-side class starts it through JNI when an `elm327://` session
   connects and stops it at the disconnect (and when the app ends).
   `mock://` and `replay://` sessions run without it. The manifest
   declares `FOREGROUND_SERVICE`, `FOREGROUND_SERVICE_CONNECTED_DEVICE`,
   `CHANGE_NETWORK_STATE` and `POST_NOTIFICATIONS`. The notification
   permission is asked once, at the first adapter connect; refused,
   the service still runs and Android lists it only in its task
   manager.
3. **One Bluetooth connect at a time.** A classic link's socket is
   never aborted while it connects. A `close` while it connects marks
   it abandoned; an `open` while an abandoned connect is still running
   waits for it and adopts it if it succeeds (it reaches the same
   adapter's SPP service), starting a new connect only once it has
   failed. An abandoned connect that succeeds with no open waiting is
   closed. The rules live in a small Qt-free class, UNIT-tested in
   `test_bridge` beside `blepicker`, which `BtLink` drives. BLE keeps
   its own path: `QLowEnergyController`'s connect can be cancelled.
4. **The recorder, the windows and the in-flight cap are unchanged.**
   With the GUI thread running, frames no longer wait for it, so the
   cap stops being the background's limit.
5. **Verification** (the phone run sheet, a new phase; CI only builds
   the APK and checks its manifest):
   - At the desk, `mock://` recording, then Home for 2 minutes and the
     screen off for 5: the recording holds every frame, Live's chart
     has no gap, nothing logs a fault, and the app returns without a
     crash. The notification while connected, none after.
   - On the car, the MX+, the same with the engine idling: the link
     stays up and the recording runs on. Bluetooth off and on while
     the screen is off recovers as in the foreground.
   - CI's manifest check adds the meta-data, the service and its
     permissions.

## Alternatives considered

- **Only the Bluetooth link on a `QThread` of its own.** Its loop would
  never be stopped, so the link would survive; but the chart, the
  recorder and the activity log would still stop, and frames past the
  cap would still be lost from the recording. Kept as the fallback if
  §1's background drawing proves unsafe on a phone.
- **The recorder in core** (ADR-0017's alternative): it would record
  through any GUI pause, but needs new ABI for values and events, and
  leaves the chart and the link as they are.
- **A foreground service alone:** it keeps the process alive but
  doesn't restart Qt's stopped loop; the run's fault would be the same.
- **Qt's `QtService`** (a second Qt `main` in a service): a second
  process and IPC for what one notification and the meta-data do.
- **Our own JNI Bluetooth socket**, closed from another thread to cut
  a connect short: more code than adopting the connect, and QtBluetooth
  stays for BLE either way. Worth it only if adoption leaves gaps.

## Consequences

- A session runs with the screen off, at the battery cost of the
  adapter's polling and the GUI thread's 10 Hz work, which is what a
  logger is for. The notification says so while it lasts.
- The GUI thread evaluates QML bindings in the background with nothing
  shown. If that costs enough to matter, the bridge can skip the
  model's updates while suspended and keep recording.
- Qt's warning stands until the phone runs pass: a crash on return
  sends §1 back to the alternative above.
- The manifest is the app's own, so a Qt upgrade means diffing it
  against the new template. The rename (Havne) moves the service's
  package with the rest.
- iOS is unchanged: it has no equivalent of a foreground service for a
  Bluetooth serial link, and the app isn't on a device yet.
