# Havne

_From Norwegian *havn*, a harbor or port: Havne reaches a car through its ports. The verb *havne* is to end up somewhere, as in *havne i grøfta*, in the ditch; staying out of it is still your job._

[![CI](https://github.com/erickramer42/havne/actions/workflows/ci.yml/badge.svg)](https://github.com/erickramer42/havne/actions/workflows/ci.yml) [![Coverage](https://codecov.io/gh/erickramer42/havne/graph/badge.svg)](https://codecov.io/gh/erickramer42/havne) [![Release](https://img.shields.io/github/v/release/erickramer42/havne)](https://github.com/erickramer42/havne/releases/latest)
[![OpenSSF Scorecard](https://api.scorecard.dev/projects/github.com/erickramer42/havne/badge)](https://scorecard.dev/viewer/?uri=github.com/erickramer42/havne) [![OpenSSF Best Practices](https://www.bestpractices.dev/projects/15816/badge)](https://www.bestpractices.dev/projects/15816)

Havne reads a car's computer and shows what it says: live
gauges, a strip chart, a log of every frame. A Qt-free C++17 core does
the vehicle I/O and decoding behind a C ABI; a Qt 6 / QML front end
draws the results on Windows, Linux, macOS and Android. It talks to
J2534 Pass-Thru adapters on Windows, to ELM327-compatible adapters
everywhere (USB, Bluetooth, WiFi) and to native CAN interfaces on Linux
through SocketCAN, listening only. `havne-cli` streams any session as
JSON lines for scripts, and a simulated vehicle runs every page with no
hardware.

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
  validators, the replay reader and SocketCAN's parsers in CI under
  ASan and UBSan ([fuzz/README.md](fuzz/README.md)).
- **A stable C ABI.** Opaque handles, flat functions and POD structs
  with reserved room, append-only enums; a strict-C compile of every
  public header pins the layout, a CI job compares the ABI with the
  last release's, and another builds and tests the core on a machine
  with no Qt ([ADR-0001](docs/adr/0001-qt-free-core-c-abi.md)).
  `havne-cli` is a second front end written against the public headers
  alone ([ADR-0031](docs/adr/0031-command-line-client.md)).
- **Downloads anyone can check.** Every download carries a signed build
  provenance and is listed in an SPDX SBOM; the Linux core packages
  build to the same bytes under varied folders, clocks and locales,
  which CI checks on every change, and two runners built the same
  AppImage from one commit. Each release ships its PDBs and Linux debug
  info, and USDT probes let `bpftrace` and `perf` time the frame path
  of a running build
  ([ADR-0034](docs/adr/0034-debug-symbols-and-probes.md),
  [ADR-0035](docs/adr/0035-linux-downloads-sbom-reproducible.md)).
- **Verified in tiers, and honest about it.** Unit, bridge and Qt Quick
  tests; a harness tier that drives the whole J2534 stack against an
  in-tree fake vendor DLL (through the 32-bit surrogate too), the
  ELM327 transport against a fake adapter, and SocketCAN and a
  simulated ECU on Linux's virtual CAN bus; clang-tidy and CodeQL; a
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
- **Four ways to reach a car.** J2534 Pass-Thru on Windows
  ([docs/j2534.md](docs/j2534.md)); ELM327-compatible adapters on a
  serial port, Bluetooth or WiFi on every desktop
  ([docs/elm327.md](docs/elm327.md)); an Android phone's Bluetooth,
  with sessions that keep running in the background
  ([ADR-0028](docs/adr/0028-app-fed-streams.md),
  [ADR-0029](docs/adr/0029-android-background-sessions.md)); and a
  native CAN interface on Linux through SocketCAN, listen-only, every
  frame on the bus or the IDs a filter names
  ([ADR-0032](docs/adr/0032-socketcan-listening.md),
  [docs/linux.md](docs/linux.md)).
- **A command-line client.** `havne-cli stream <uri>` writes any
  session's frames, decoded values and link health as JSON lines for
  `jq`, scripts and headless logging; `havne-cli devices` lists the
  ports, adapters and CAN interfaces a URI can name; on Linux,
  `havne-cli play` puts the simulated car or a recording on a virtual
  CAN bus ([ADR-0031](docs/adr/0031-command-line-client.md)).
- **Profiles chosen by the URI.** `obd2` polls standard PIDs,
  `uds-generic` polls or scans the UDS identifiers the URI names, and
  `raw` logs frames; the car's VIN and calibration IDs are read at
  connect ([ADR-0013](docs/adr/0013-protocol-profiles.md),
  [ADR-0021](docs/adr/0021-vehicle-identification.md)).
- **Recording and replay.** Each connect can record every frame, value
  and event; `replay://` plays a recording back at its own pace or
  faster, through the same decoding
  ([ADR-0017](docs/adr/0017-session-recording.md),
  [ADR-0023](docs/adr/0023-replay-transport.md)). If the computer or
  phone sleeps mid-session, the sleep is a gap on Live and in the
  recording, and the activity log says how long it was
  ([ADR-0033](docs/adr/0033-session-time-counts-sleep.md)).
- **Vehicles and preferences.** Name a car once and it remembers its
  adapter, what Live plots and its recordings' folder; it's recognized
  next time by a salted hash of its VIN. Metric or imperial, zoom and
  the last connection are kept between runs.
- **A simulated vehicle.** A modeled car (gearing, airflow, tires with
  weight transfer, coolant and oil heat) drives a cold start, two pulls
  onto the rev limiter, a cruise and a quarter mile, with readings that
  stay consistent with each other the way a real ECU's log does.

| Live view | Link health |
|---|---|
| [![Live view: RPM, speed, MAF and lambda plotted through a 3rd-gear pull onto the limiter](docs/images/live.png)](docs/images/live.png) | [![Live view over J2534 with every request failing: an amber "Requests failing" banner and pill, and each signal waiting for data](docs/images/link-health.png)](docs/images/link-health.png) |
| A 3rd-gear pull onto the limiter: rpm, speed and MAF rise together, then rpm saws while speed holds and lambda swings lean on each fuel cut | Every request refused by the fake J2534 driver: the link reads "requests failing", not idle, and the page says what to check |

![The Log page in developer mode on the simulated vehicle: the activity log holds two connects with their Session lines and, between them, the Delivery line of the disconnect (none dropped, the GUI thread's wait in microseconds); the pointer clicks Frames and the raw frame table streams, newest first, each frame's time, length, standard or extended ID and payload; then it clicks back to Activity](docs/images/log.gif)

*The Log page on the simulated vehicle (`mock://demo`), in developer
mode: each session as it happened, then the frames as they arrive.*

## Architecture

```
 QML pages: Home · Dashboard · Live · Log · Settings
   ▼  properties and list models, GUI thread only
 SessionBridge (app/)          the app's caller of the core: worker → GUI
   │                           marshaling, 100 ms coalescing, link health
   │   havne-cli (cli/)        a second front end: a session as JSON lines
   ▼  C ABI: core.h · signals.h · profile.h · j2534.h · serial.h · stream.h · socketcan.h
 ecuio-core (core/)            no Qt, ever: sessions, profiles, decoding
   ├── mock://    a scripted drive on a vehicle model
   ├── replay://  a recording's frames.csv at its own pace
   ├── elm327://  a serial port, a WiFi socket, or a link the app opens
   ├── socketcan:// a Linux CAN interface, listen-only (havne-cli play
   │              drives the simulated car on a virtual one)
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
| [`cli/main.cpp`](cli/main.cpp), [`cli/line_ring.h`](cli/line_ring.h) | A second front end over the public headers alone: lines formatted on the worker with no allocation, a ring that drops for a live bus and waits for a replay |
| [`core/src/mock_drive_cycle.h`](core/src/mock_drive_cycle.h) | The simulated vehicle and its physics |

## Try it

On Windows 10 or 11 (x64), download the zip from the
[latest release](https://github.com/erickramer42/havne/releases/latest),
unzip it and run `havne.exe`; Qt and the Visual C++ runtime are inside.
It isn't code-signed, so SmartScreen may ask first (**More info**, then
**Run anyway**). On Android 12 or later, download and open the release's
APK on the phone; it's signed with Havne's release key, and each
release installs over the last. On Linux (x86_64, glibc 2.39 or
later), download the AppImage, make it executable and run it;
[docs/linux.md](docs/linux.md) covers what it needs, the core packages
for `havne-cli` and your own programs, serial permissions and
SocketCAN. In Settings, tick **Developer mode** under Display, then
press the `mock://demo` preset and **Connect**.

To build from source you need CMake 3.21+, Qt 6.12 and a C++17
compiler:

```sh
cmake -B build -DCMAKE_PREFIX_PATH=<qt-install>/<kit>
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

`-DHAVNE_BUILD_CORE_ONLY=ON` builds the core, `havne-cli` and their
tests without Qt; `havne-cli stream mock://demo --values` then streams
the simulated car as JSON lines.
[docs/development.md](docs/development.md) covers the 32-bit
surrogate's second tree, Linux, macOS, Android and iOS, and
troubleshooting.

## Status

v0.3.4 (`havne-cli`, Linux as a supported desktop with SocketCAN
listening, the core as packages, debug symbols and an SBOM) is the
latest release. Next:

| Version | Plan |
|---|---|
| v0.3.5 | Full OBD-II diagnostics, reading and clearing trouble codes; SocketCAN requests through ISO-TP |
| v0.3.6–v0.3.9 | A log viewer that overlays runs; CSV import and export, scrubbed for sharing; a health report; triggered logging and alerts |
| Later | Python bindings, capture formats and discovery; a plugin framework; older buses |

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
| [docs/linux.md](docs/linux.md) | Havne on Linux: the AppImage, the core packages, adapters and permissions, SocketCAN, tracing |
| [docs/development.md](docs/development.md) | Builds, tests, CI, releases, troubleshooting |
| [docs/bench-sessions/](docs/bench-sessions/bench-prep.md) | Real-hardware sessions, their run sheets, and what only hardware can confirm |
| [docs/engineering-log.md](docs/engineering-log.md) | Milestones and lessons learned |
| [CONTRIBUTING.md](CONTRIBUTING.md), [SECURITY.md](SECURITY.md) | Where to start, the rules a change follows, reporting a vulnerability |

## License

MIT; see [LICENSE](LICENSE). Havne uses [Qt](https://www.qt.io) under
LGPL-3.0; Qt isn't bundled in this repository. Havne is an independent
project, not affiliated with or endorsed by The Qt Company.
