# Havne: Project Status

**Version:** v0.8.0 (2026-11-15). **Next:** v0.9.0, the whole car through plugins. [![CI](https://github.com/erickramer42/havne/actions/workflows/ci.yml/badge.svg)](https://github.com/erickramer42/havne/actions/workflows/ci.yml)

Havne reads a car's ECUs through an OBD-II adapter and shows, records and exports what they say. It runs on Windows, Linux and Android, with `havne-cli` and a Python package over the same core. It reads only: nothing it sends changes a module's memory or settings, apart from clearing trouble codes when asked.

This file is the present state: what works, how far it's verified, what's planned and what's known to be wrong. How it fits together: [docs/architecture.md](docs/architecture.md). Why: [docs/adr/](docs/adr/README.md). What each release changed: [CHANGELOG.md](CHANGELOG.md). Safety and legal caveats: [DISCLAIMER.md](DISCLAIMER.md).

## At a glance

| Area | Where it stands | More |
|---|---|---|
| Adapters | ELM327 and STN over USB, WiFi and Bluetooth (Android); J2534 on Windows, 32- and 64-bit drivers each in its own process; SocketCAN on Linux (listening; requests on a virtual bus); the demo car and replays everywhere | [elm327.md](docs/elm327.md), [j2534.md](docs/j2534.md), [linux.md](docs/linux.md) |
| What it reads | Every linear OBD-II PID, packed several to a request; trouble codes, readiness, the freeze frame and clearing; the VIN and module identification; manufacturer DIDs through signal files and plugins, up to 4 modules and 1024 requests a session | [signal-files.md](docs/signal-files.md), [ADR-0052](docs/adr/0052-plugin-framework.md) |
| Your own signals | Signal files with computed signals (functions, comparisons, chains); one plugin store for every import, OBDb sets included, picked per vehicle | [ADR-0062](docs/adr/0062-computed-grammar.md), [ADR-0064](docs/adr/0064-one-store.md) |
| Live and the Dashboard | Live's table and chart with pause; a Dashboard built from eight kinds of gauge, saved per phone and desktop, exported and loaded; Dark, Light and two Night looks | [architecture.md](docs/architecture.md#the-ui-layer) |
| Conditions and alerts | Condition files judged on the frames' own time; a banner, colored values and on Android a tone; armed recording that writes a window when a condition fires | [condition-files.md](docs/condition-files.md), [ADR-0063](docs/adr/0063-conditions.md) |
| Recordings | Every frame and value, with the link's health and reply timing; a viewer with chart panes, cursors, statistics, pulls and cruises, and comparison; CSV, pcapng and folder exports, scrubbed for sharing; CSV and candump imports; a health report | [ADR-0049](docs/adr/0049-recordings-in-core.md), [ADR-0054](docs/adr/0054-exports-and-imports.md) |
| `havne-cli` and Python | Every session, recording, export, condition and plugin command the app has, as JSON lines; Python's `havne` package over the same C ABI, as wheels for Windows, Linux and macOS | [ADR-0031](docs/adr/0031-command-line-client.md), [ADR-0053](docs/adr/0053-python-bindings.md) |
| Platforms | Windows (zip), Linux (AppImage, `.deb` and tarball for x86_64 and 64-bit ARM), Android 12+ (APK); macOS builds in CI; iOS runs on the Simulator only | [linux.md](docs/linux.md), [docs/development.md](docs/development.md) |

**Verified:** 2711 tests on Windows and the cross-arch subset (111) at v0.8.0, CI on Windows, macOS and Linux with sanitizers and fuzzing ([testing.md](docs/testing.md)). On hardware: three adapters (the VX over J2534 and as an ELM327, the MX+ over Bluetooth, a USB2CAN on SocketCAN) and one car; the phone app on one phone. Not yet: 29-bit addressing, a real 64-bit J2534 driver, other vendors and cars ([bench-prep.md](docs/bench-sessions/bench-prep.md)).

**Next, v0.9.0:** a profile sized for a whole car (up to 32 modules a session) and a vehicle's plugins read together; sessions a plugin declares; the viewer's panes chosen by hand, the Dashboard's presets and computed signals you write; then the conversation tier: security access, and the safety gates a later write path needs. [Roadmap](#roadmap).

---

## BENCH prep

The assumptions in the J2534 and ELM327 code that only real hardware
can confirm, each with its evidence, are in
[docs/bench-sessions/bench-prep.md](docs/bench-sessions/bench-prep.md);
every session walks it. Every item is settled for the adapters on
hand but these: 29-bit addressing (#4, the car is 11-bit), what sets
the VX clock's zero (#5), the VX's serial port on Windows without
OBDX's INF (#11), response pending (#17), the VX's WiFi while
its USB port is held (#20, in part), a BLE adapter (#25), a real CAN controller that sends with
one-shot mode (#32), a sleep on a Linux
machine (#27), and a busy bus through the surrogate (#34). Any other adapter starts the list again.

## Known issues and technical debt

These numbers are stable IDs, and code and docs refer to them. When
an issue is resolved it is removed, and its number is not reused.

- **#2. The iOS app is unvalidated, and no tests run on a phone.** CI
   cross-builds an unsigned iOS arm64 device build and, on request,
   launches the app on the Simulator and screenshots it
   (`ios-simulator-smoke`); it has never run on a device. Each run's
   `havne-ios-arm64-unsigned` artifact can be sideloaded with a
   tester's own Apple ID ([docs/ios.md](docs/ios.md)). The Android app has run on one phone (Android 16) and one
   car, with the MX+ over classic Bluetooth, in the background and with
   the screen off ([2026-10-28-android-11](docs/bench-sessions/2026-10-28-android-11.md),
   [2026-10-29-android](docs/bench-sessions/2026-10-29-android.md));
   no BLE adapter has. Neither platform runs the suite: cross builds
   set `BUILD_TESTING=OFF`, so the phone is checked by its run sheet
   ([android-phone-runsheet.md](docs/bench-sessions/android-phone-runsheet.md)).
   The app needs Android 12 or later; CI uploads the Android build's
   debug APK, and each release carries one signed with the release key
   ([ADR-0030](docs/adr/0030-signed-android-releases.md)).
- **#6. The session API is single-caller by contract.** Each session has
   one caller: the bridge in the app, `havne-cli` in its own process
   ([ADR-0031](docs/adr/0031-command-line-client.md)). Two front ends
   sharing one session would need more.
- **#13. The picker treats one registry entry as one device.** Multiple
    adapters behind one vendor DLL can't be told apart. They would be
    selected through `PassThruOpen`'s `pName` argument, which the
    loader passes as `nullptr`, meaning the default device.
- **#20. Pointer-bearing ioctls (`FAST_INIT`, `FIVE_BAUD_INIT`) are out of
    contract** and refused at both ends. That is fine for ISO 15765,
    since those are ISO 9141/KWP init calls. If a BENCH device needs
    them, give them a dedicated wire form rather than relaxing the
    whitelist.
- **#29. The iOS simulator smoke depends on Intel macOS runners.** Qt
    6.12's iOS simulator libraries are x86_64 only, so
    `ios-simulator-smoke` runs on `macos-15-intel`, where the simulator
    runs natively. GitHub has announced the end of Intel macOS runners
    for Fall 2027. The options after that are an arm64 runner with
    Rosetta plus the universal iOS platform (a large download on every
    run), or Qt's arm64 simulator libraries if Qt ships them. Apple is
    also winding down Rosetta, so the first option has its own end
    date.
- **#41. Through the OBDLink MX+, a listening session hears Single
    Frame replies only.** The MX+'s monitor misses every frame that
    follows another within about 1 ms: another tester's Flow Control
    and the ECU's Consecutive Frames after a First Frame. A multi-frame
    reply never completes, so it is dropped and nothing is delivered for
    it. No setup tried changes that (`AT CAF0` or `CAF1`, `AT CFC0`, with
    or without `AT CRA`, `AT MA` or `STMA`, `STCSEGR1`, `STCMM2`), and the frames
    are on the bus: a SocketCAN listener on the same splitter hears
    them (BENCH prep #19,
    [2026-11-04-two-ecu-car](docs/bench-sessions/2026-11-04-two-ecu-car.md),
    [2026-11-05-two-ecu-car-rerun](docs/bench-sessions/2026-11-05-two-ecu-car-rerun.md),
    [2026-11-11-monitor-car](docs/bench-sessions/2026-11-11-monitor-car.md)).
    A listening session needs no flow control, so its setup still
    turns it off (`AT CFC0`). Untried: `STCMM1`, which acknowledges on
    the bus, and `STM`'s filters. To listen to
    multi-frame replies, use a SocketCAN controller in listen-only mode,
    or the VX in ELM mode, which hears them (BENCH prep #19,
    [2026-11-08-linux-car](docs/bench-sessions/2026-11-08-linux-car.md)).
- **#42. Through the OBDX Pro VX, a second module's reply can be lost
    while the first module's segmented reply comes in.** On the test
    car, a session asking the engine (0x7E8) and a second module
    (0x7E9) lost 22% of the second module's single-frame replies with
    the engine's `obd2` requests packed, and 1.1% with `?pack=0`; nearly
    every lost one was due within 30 ms of a 0x7E8 reply of several
    frames. Each write was accepted and no reply came late; with one DID
    asked of each module, and through the MX+, none was lost
    ([2026-11-06-own-signals-car](docs/bench-sessions/2026-11-06-own-signals-car.md),
    BENCH prep #35). The lost replies are on the bus: a SocketCAN
    listener on the splitter recorded every one, and with one DID on
    each module, the engine's reply segmented, the VX lost about a fifth
    ([2026-11-11-monitor-car](docs/bench-sessions/2026-11-11-monitor-car.md)).
    The request goes out again on its next turn, so a
    signal still updates, less often. For a session that asks two
    modules through the VX, `?pack=0` keeps the losses near 1%.
- **#43. On a phone with gesture navigation, leaving the drive view
    takes three back swipes.** The view hides Android's bars, and
    Android spends the first swipe from the edge showing them; the
    second shows Havne's controls, as Back does in the view
    ([ADR-0059](docs/adr/0059-drive-view.md)), and the third leaves.
    With three-button navigation it takes two presses. A tap on the
    gauges, then **Exit drive**, leaves at once in either mode. Checked
    on the test phone (Android 16) with the mock.
- **#44. The extended session's close is fixed against the fakes; the
    car re-run is owed (BENCH prep #36).** Through an ELM327 adapter the
    close finishes a reply the stop cut off, then sends `10 01`, again
    if the adapter answers `STOPPED`; through J2534 it reads the
    module's answer before the filters stop, so the module isn't left
    repeating it ([ADR-0060](docs/adr/0060-extended-session-reads.md)'s
    appendix "the close waits for the module"). Through SocketCAN the
    close waits for a read still out before `10 01`, with J2534's bound
    (the appendix of 2026-11-12), against the simulated bus only: a
    SocketCAN session that asks needs an adapter whose driver offers
    one-shot mode (BENCH prep #32). The car runs of
    2026-11-11 found the earlier closes short
    ([2026-11-11-ext-session-car](docs/bench-sessions/2026-11-11-ext-session-car.md),
    [2026-11-11-quiet-session-car](docs/bench-sessions/2026-11-11-quiet-session-car.md)).
    The number closes with a parked car run, a listener on the bus,
    that reads one `50 01`.

## Roadmap

| Version | Scope | Status |
|---|---|---|
| v0.7.0 | The dashboard and the UI: gauges built by hand from a library, an analysis log and a wide CSV, import in the app, themes for the dark, a phone-first Settings | Released |
| v0.7.1 | A more modern look, phone first: Night as its own control, the S2 look and shared controls, the phone's five tabs, joined panel and drive view, the viewer's Export, the log's minimum time between sweeps and reads in the extended session | Released |
| v0.7.2 | Finishing the Dashboard and signals: the desktop's gauges in the new design, undo in edit mode, a Light look, built-in layouts by what people watch, a signal file's dial range and warning levels, Python's exports and health report | Released |
| v0.7.3 | The analysis viewer: the log's navigation aids, chart panes with two cursors, a recording's statistics, Live's pause, and the phone pass's fixes | Released |
| v0.7.4 | Follow-ups on v0.7.3's theme: a Dashboard layout exported and loaded, more gauge colors, every plugin variant offered for a vehicle, the viewer's difference trace, the demo car under obd2, and Android's own document picker | Released |
| v0.8.0 | Conditions: a condition file judged in a session, alerts in the app (banner, tone, colors, the Alerts page), armed recording, one plugin store for every import, a fuller grammar for computed signals, and a request budget of 1024 | Released |
| v0.9.0-v0.11.0 | v0.9.0, the whole car through plugins: what v0.8.0 left out, a profile sized for every module, a vehicle's plugins read together in one session (a signal set of your own, an OBDb import and `obd2`), sessions a plugin declares, and a plugin's own conversation with a module, security access to unlock reads among it. v0.10.0, discovery: listening to the whole bus, and the rest of a plugin's conversation. v0.11.0, the scan tool, the last feature release | Planned |
| 1.0.0 | The finished reader: the features above frozen and hardened, full featured on Windows, Linux and Android | Planned |

Versions follow semantic versioning
([ADR-0048](docs/adr/0048-semantic-versioning.md)): each feature
release raises the minor number; a patch carries fixes and follow-up
work on its minor's theme, and never breaks the C ABI, `havne-cli`'s
schema or a plugin format (ADR-0048's appendix "what a PATCH may
carry"). Earlier releases are in [CHANGELOG.md](CHANGELOG.md). The
planned releases are in order and hold about three features each; a
release's detail comes in here when its work starts.

### v0.8.0: conditions

For people who want a log or a warning to start from the car's own state. A feature release: the C ABI grows, append-only (`ecuio/conditions.h`, the plugin wrap calls, `ECUIO_RECORD_ARMED`).
- **Built:**
  - a condition file (`docs/condition-files.md`, [ADR-0063](docs/adr/0063-conditions.md)): signal and link terms judged on the frames' own time, read and checked by `havne-cli conditions check`, `havne.ConditionSet` and the JSON Schema; a recording keeps its conditions and the viewer draws them;
  - alerts in the app: the Alerts page, the banner on every page and in the drive view, colored gauge and Live values, and on Android a tone;
  - an armed recorder that writes a window for each `record` condition (`havne-cli log --armed`, Python's `Recorder(armed=True)`, the app's Armed button);
  - one plugin store: every import is a plugin, with one list and one Import on Settings, Signals, the vehicles' earlier signal files moved at the first start ([ADR-0064](docs/adr/0064-one-store.md)), and `havne-cli plugins wrap`, `put` and `drop`;
  - signal files in format 2: functions, comparisons, `if`, `all`, `any` and chains of computed signals up to 8 deep ([ADR-0062](docs/adr/0062-computed-grammar.md));
  - a request budget of 1024 and a default pick of ranked signals, so an OBDb import of a whole car's set keeps its signals ([ADR-0051](docs/adr/0051-signal-files.md), [ADR-0056](docs/adr/0056-analysis-log.md)).
- **Verified:** the Windows suite and the cross-arch subset on `dev`'s tip; the app on the test phone with the mock (the store's migration, the one list, the looks), not on a car. Conditions have run against the simulated car and recordings only.
- **Left for v0.9.0:** the live checks as terms and `ecuio_recording_evaluate` (ADR-0063), a recording whose every condition event was lost saying so, a dial's caption over a numeral, the long-files limits, the signal store's removal from the app (ADR-0064 step 6), and a choice of where a pull ends (at the throttle's lift, the limiter included, or at top speed).

### Waiting on a tester: an iPhone download

No version until a tester has run CI's unsigned `.ipa` on an iPhone
and the run is filed as a session sheet. Then the release workflow
builds and publishes `havne-X.Y.Z-ios-arm64-unsigned.ipa` with the
other downloads (provenance, the SBOM and the checksums), the release
checks read its version and that it carries no signature, and the
notes' Try it points at [docs/ios.md](docs/ios.md).

### 1.0.0: the finished reader

After v0.11.0 the formats freeze (the C ABI, `havne-cli`'s schema, the
plugin formats, the recording format) and the releases are fixes
alone, as 0.11.x: a second car and testers other than the developer,
every adapter class on a car, the release on a PC without Visual
Studio, Linux on real hardware, accessibility, and
hour-long runs on every transport. 1.0.0 follows once those runs are
filed and the 0.11.x releases have gone a stated stretch with no
high-severity issue: full featured on Windows, Linux and Android, as
much macOS and iOS as is manageable, with a signed Windows executable
and an installer that checks for updates. Writing to a car (unlocking
a module for writes, flashing) is 2.0.

## Backlog

Work not placed in a release, one line each. An item moves into the
roadmap when it's placed. Defects and debt live under
[Known issues](#known-issues-and-technical-debt). **S** is a day or
less, **M** a few days, **L** a week or more or an ADR first.

### User interface

- **Per-signal unit overrides (S):** one quantity in the other system
  (deg F with km/h); whole-UI units are built
  ([ADR-0022](docs/adr/0022-display-units.md)).
- **In-vehicle displays, Android Auto and CarPlay (L, research
  first):** a native front end over the C ABI, only if a live-gauge
  app qualifies.

### Vehicle coverage

- **A scanned DID answered "response pending" on a real adapter (S):**
  tested against the fake only; needs an ECU that sends it (BENCH prep
  #17).

### Performance

- **`FrameListModel` inserts a row per frame (S):** one
  `beginInsertRows` for each frame of a delivery; a batch insert would
  signal the view once. Measure first
  ([docs/testing.md](docs/performance.md)).
- **Full signal sets at full rate (L):** left is the bridge's 4 KB copy
  per frame ([docs/testing.md](docs/performance.md)).

### Toolchain

- **A Release iOS build may link Qt's `qmltooling` plugins (S):** check
  whether the `.ipa` carries them and, if so, exclude them with
  `qt_import_plugins(havne EXCLUDE_BY_TYPE qmltooling)`; needs a CI run
  and an Apple kit.
- **The next type2-runtime pin (S):** the steps are in
  [docs/development.md](docs/development.md) ("A new runtime pin"); the
  newer runtime would take the release that unpacks into a 0700 folder.

- **C++20 baseline (M, an ADR first):** `std::span` and `std::format`
  against older toolchains dropping out; low priority.
