# Havne

_From Norwegian *havn*, a harbor or port: Havne reaches a car through its ports. The verb *havne* is to end up somewhere, as in *havne i grøfta*, in the ditch; staying out of it is still your job._

[![CI](https://github.com/erickramer42/havne/actions/workflows/ci.yml/badge.svg)](https://github.com/erickramer42/havne/actions/workflows/ci.yml) [![Coverage](https://codecov.io/gh/erickramer42/havne/graph/badge.svg)](https://codecov.io/gh/erickramer42/havne) [![Release](https://img.shields.io/github/v/release/erickramer42/havne)](https://github.com/erickramer42/havne/releases/latest)
[![OpenSSF Scorecard](https://api.scorecard.dev/projects/github.com/erickramer42/havne/badge)](https://scorecard.dev/viewer/?uri=github.com/erickramer42/havne) [![OpenSSF Best Practices](https://www.bestpractices.dev/projects/15816/badge)](https://www.bestpractices.dev/projects/15816)

Havne reads a car's computer and shows what it says: live
gauges, a strip chart, a log of every frame. A Qt-free C++17 core does
the vehicle I/O and decoding behind a C ABI; a Qt 6 / QML front end
draws the results on Windows, Linux, macOS and Android. It talks to
J2534 Pass-Thru adapters on Windows and to ELM327-compatible adapters
everywhere (USB, Bluetooth, WiFi), and a simulated vehicle runs every
page with no hardware.

> [!WARNING]
> **Experimental, read-only.** It has read one car through two adapters, from a laptop and from an Android phone; other vehicles and adapters are untested. Read [DISCLAIMER.md](DISCLAIMER.md) before connecting it to a vehicle.

![The Havne Dashboard through a quarter mile on the warmed-up simulated vehicle: launch control holds the engine near 4,000 rpm at the line, the tires spin off it, three upshifts at 6,800 rpm refill the shift lights, and speed climbs to about 107 mph at the line while coolant rises from 194 to 216 °F, where the fan cuts in, and the oil from 225 to 239 °F, before the driver lifts](docs/images/dashboard.gif)

*The Dashboard on the simulated vehicle (`mock://demo`), in real time:
a quarter mile off launch control on a warm loop.*

## Engineering highlights

- **Vendor drivers run in a process of their own.** J2534 drivers are
  third-party DLLs, often 32-bit. The loader reads each DLL's PE header
  and runs it in a single-threaded surrogate process of the same
  architecture behind a private named pipe, so a crashing driver ends
  its surrogate, not the app, and every vendor call lands in one log
  ([ADR-0025](docs/adr/0025-one-j2534-backend.md),
  [ADR-0014](docs/adr/0014-single-threaded-surrogate.md)).
- **A bounded data path, measured.** Callbacks dispatch from a
  snapshot, the transport's worker decodes and allocates nothing per
  frame (a test counts its allocations), backpressure bounds the frames
  in flight, and values reach QML at 10 Hz whatever the bus rate. A
  recorded engine start replayed through the app sustains 375,000 to
  565,000 frames a second through decode, more than 80 times a full
  classic CAN bus, and CI re-runs that load test on every push
  ([ADR-0024](docs/adr/0024-decode-on-the-worker.md),
  [docs/testing.md](docs/testing.md#load-test-the-replay-fixture-through-the-bridge)).
- **Requests follow the replies.** One request out per ECU, the next
  when the answer is in; a supported-PID probe drops only what the ECU
  says it lacks, never on silence; nothing but read services can be
  sent ([ADR-0015](docs/adr/0015-request-engine.md)). On the car,
  about 26 requests a second through J2534 and over 30 through an
  ELM327 on Bluetooth.
- **Link health that names the right problem.** Reads and writes are
  judged apart, so a quiet bus (ignition off) isn't an adapter fault
  and failing requests aren't a dead adapter; recovery backs off from
  100 ms to 5 s. A value never received shows "—", never 0.
- **Protocol parsers built to be fuzzed.** The ELM327 wire layer is
  pure (no I/O, no clock, no allocation), so every rule is a unit test
  over canned bytes, and libFuzzer runs it, the signal decoder, the
  validators and the replay reader in CI under ASan and UBSan
  ([fuzz/README.md](fuzz/README.md)).
- **A stable C ABI.** Opaque handles, flat functions and POD structs
  with reserved room, append-only enums; a strict-C compile of every
  public header pins the layout, and a CI job builds and tests the core
  on a machine with no Qt
  ([ADR-0001](docs/adr/0001-qt-free-core-c-abi.md)).
- **Verified in tiers, and honest about it.** Unit, bridge and Qt Quick
  tests; a harness tier that drives the whole J2534 stack against an
  in-tree fake vendor DLL (through the 32-bit surrogate too) and the
  ELM327 transport against a fake adapter; clang-tidy and CodeQL; a
  signed build provenance on every download. What only hardware can
  confirm is listed, and every real-adapter session is written up
  ([docs/testing.md](docs/testing.md),
  [bench-prep.md](docs/bench-sessions/bench-prep.md)).
- **Decisions on record.** Every design decision is an
  [architecture decision record](docs/adr/README.md), with the
  alternatives it turned down and dated appendices where hardware
  changed the plan; lessons go in an
  [engineering log](docs/engineering-log.md).

## What it does

- **Dashboard, Live and Log pages.** Gauges with shift lights, peak
  hold and stale dimming; decoded values and a strip chart that keeps
  each 100 ms window's peak; an activity log of every connect, link
  change, adapter and ECU, with the raw frames in developer mode.
- **Three ways to reach a car.** J2534 Pass-Thru on Windows
  ([docs/j2534.md](docs/j2534.md)); ELM327-compatible adapters on a
  serial port, Bluetooth or WiFi on every desktop
  ([docs/elm327.md](docs/elm327.md)); and an Android phone's Bluetooth,
  with sessions that keep running in the background
  ([ADR-0028](docs/adr/0028-app-fed-streams.md),
  [ADR-0029](docs/adr/0029-android-background-sessions.md)).
- **Profiles chosen by the URI.** `obd2` polls standard PIDs,
  `uds-generic` polls or scans the UDS identifiers the URI names, and
  `raw` logs frames; the car's VIN and calibration IDs are read at
  connect ([ADR-0013](docs/adr/0013-protocol-profiles.md),
  [ADR-0021](docs/adr/0021-vehicle-identification.md)).
- **Recording and replay.** Each connect can record every frame, value
  and event; `replay://` plays a recording back at its own pace or
  faster, through the same decoding
  ([ADR-0017](docs/adr/0017-session-recording.md),
  [ADR-0023](docs/adr/0023-replay-transport.md)).
- **Vehicles and preferences.** Name a car once and it remembers its
  adapter, what Live plots and its recordings' folder; it's recognized
  next time by a salted hash of its VIN. Metric or imperial, zoom and
  the last connection are kept between runs.
- **A simulated vehicle.** A modeled car (gearing, airflow, tires with
  weight transfer, coolant and oil heat) drives a cold start, two pulls
  onto the rev limiter, a cruise and a quarter mile, with readings that
  stay consistent with each other the way a real ECU's log does.

| Live view | Activity log | Link health |
|---|---|---|
| [![Live view: RPM, speed, MAF and lambda plotted through a 3rd-gear pull onto the limiter](docs/images/live.png)](docs/images/live.png) | [![The Log page's activity log: a mock session's connect, its Session line, the Delivery line at the disconnect (none dropped, the GUI thread's wait in microseconds) and a second connect](docs/images/log.png)](docs/images/log.png) | [![Live view over J2534 with every request failing: an amber "Requests failing" banner and pill, and each signal waiting for data](docs/images/link-health.png)](docs/images/link-health.png) |
| A 3rd-gear pull onto the limiter: rpm, speed and MAF rise together, then rpm saws while speed holds and lambda swings lean on each fuel cut | Each session as it happened: the connect, the "Session:" line, and how its frames reached the screen | Every request refused by the fake J2534 driver: the link reads "requests failing", not idle, and the page says what to check |

## Architecture

```
 QML pages: Home · Dashboard · Live · Log · Settings
   ▼  properties and list models, GUI thread only
 SessionBridge (app/)          the only caller of the core: worker → GUI
   │                           marshaling, 100 ms coalescing, link health
   ▼  C ABI: core.h · signals.h · profile.h · j2534.h · serial.h · stream.h
 ecuio-core (core/)            no Qt, ever: sessions, profiles, decoding
   ├── mock://    a scripted drive on a vehicle model
   ├── replay://  a recording's frames.csv at its own pace
   ├── elm327://  a serial port, a WiFi socket, or a link the app opens
   └── j2534://   a surrogate process per driver, over a named pipe
          ├── ecuio-j2534-surrogate.exe      (32-bit drivers)
          └── ecuio-j2534-surrogate-x64.exe  (64-bit drivers)
```

Three rules shape it: **the core never links Qt**, so other front ends
can drive the same core; **the UI receives final display values**, so
decoding, scaling and units have one home; and **the C ABI is the
boundary**. [docs/architecture.md](docs/architecture.md) walks through
it, a frame's path from transport to screen included.

**Where to look in the code:**

| File | What it shows |
|---|---|
| [`core/src/request_engine.h`](core/src/request_engine.h) | The request engine: pacing, the probe, the DID scan and identification, with no I/O and no clock, so every rule is a unit test |
| [`core/src/elm327_protocol.h`](core/src/elm327_protocol.h) | A pure protocol layer: line reading, frame parsing, ISO-TP reassembly; fuzzed |
| [`core/src/j2534_surrogate_client.cpp`](core/src/j2534_surrogate_client.cpp), [`surrogate/surrogate_main.cpp`](surrogate/surrogate_main.cpp) | Both ends of the out-of-process driver host: the pipe's protections, late replies, a child that dies |
| [`core/src/link_health.h`](core/src/link_health.h) | The link's state machine, shared by both adapter transports |
| [`app/sessionbridge.cpp`](app/sessionbridge.cpp) | The worker-to-GUI boundary: backpressure, coalescing, recording |
| [`core/src/mock_drive_cycle.h`](core/src/mock_drive_cycle.h) | The simulated vehicle and its physics |

## Try it

On Windows 10 or 11 (x64), download the zip from the
[latest release](https://github.com/erickramer42/havne/releases/latest),
unzip it and run `havne.exe`; Qt and the Visual C++ runtime are inside.
It isn't code-signed, so SmartScreen may ask first (**More info**, then
**Run anyway**). On Android 12 or later, download and open the release's
APK on the phone; it's signed with Havne's release key, and each
release installs over the last. In Settings, tick **Developer mode**
under Display, then press the `mock://demo` preset and **Connect**.

To build from source you need CMake 3.21+, Qt 6.12 and a C++17
compiler:

```sh
cmake -B build -DCMAKE_PREFIX_PATH=<qt-install>/<kit>
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

`-DHAVNE_BUILD_CORE_ONLY=ON` builds the core and its tests without Qt.
[docs/development.md](docs/development.md) covers the 32-bit
surrogate's second tree, Linux, macOS, Android and iOS, and
troubleshooting.

## Status

v0.3.3 (ELM327 adapters, preferences and vehicles, Android with
Bluetooth, the rename from QtTune) is the latest release. Next:

| Version | Plan |
|---|---|
| v0.3.4 | A command-line client over the C ABI; Linux as a supported desktop; a SocketCAN transport that listens |
| v0.3.5 | Full OBD-II diagnostics, reading and clearing trouble codes; SocketCAN requests through ISO-TP |
| Later | Python bindings, capture formats and discovery; a plugin framework for make-specific data; older buses; writing to the ECU at 1.0 and beyond |

[STATUS.md](STATUS.md) holds what works, what's verified, the known
issues and the full roadmap; [CHANGELOG.md](CHANGELOG.md) what each
release changed.

## Documentation

| Document | What's in it |
|---|---|
| [docs/architecture.md](docs/architecture.md) | How the parts fit together and why, and a map of the other docs |
| [docs/adr/](docs/adr/README.md) | The architecture decision records |
| [docs/testing.md](docs/testing.md) | Test tiers, what each test pins, and what isn't verified |
| [docs/j2534.md](docs/j2534.md), [docs/elm327.md](docs/elm327.md) | How each adapter transport works |
| [docs/development.md](docs/development.md) | Builds, tests, CI, releases, troubleshooting |
| [docs/bench-sessions/](docs/bench-sessions/bench-prep.md) | Real-hardware sessions, their run sheets, and what only hardware can confirm |
| [docs/engineering-log.md](docs/engineering-log.md) | Milestones and lessons learned |
| [CONTRIBUTING.md](CONTRIBUTING.md), [SECURITY.md](SECURITY.md) | Where to start, the rules a change follows, reporting a vulnerability |

## License

MIT; see [LICENSE](LICENSE). Havne uses [Qt](https://www.qt.io) under
LGPL-3.0; Qt isn't bundled in this repository. Havne is an independent
project, not affiliated with or endorsed by The Qt Company.
