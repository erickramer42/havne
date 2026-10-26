# Architecture

A short map of how QtTune fits together and why. Each section points to
the document that holds the detail.

QtTune is an ECU reader and logger. A Qt-free C++17 **core** sits
behind a C ABI. It owns sessions, transports and signal decoding. A
Qt 6 / QML **front end** turns the core's output into pages. Between
them, a single **bridge** object moves data from the core's worker
threads onto the GUI thread.

```
 QML pages: Home · Dashboard · Live · Log · Settings
   │  Theme (colors, link-state labels) · SignalHub (signals by name)
   ▼  properties and list models, GUI thread only
 QtTuneBridge (app/)          the only caller of the core
   │  worker → GUI marshaling, 100 ms coalescing, link-health polling
   ▼  C ABI: core.h · signals.h · profile.h · j2534.h
 ecuio-core (core/)           no Qt, ever
   │  sessions + callbacks · profiles + decode · transports
   ├── mock://    worker thread, scripted drive cycle
   ├── replay://  worker thread, a recording's frames.csv at its own pace
   └── j2534://   worker thread → loader, which picks a backend:
          ├── in-proc:   vendor DLL loaded into the app (x64 DLLs)
          └── surrogate: client ══ named pipe ══ ecuio-j2534-surrogate.exe
                         (x86 process hosting x86 vendor DLLs)
```

## Three rules, and why

1. **The core never links Qt.** Other front ends (mobile, scripting,
   another UI toolkit) must be able to drive the same core, and the
   core must build on a machine with no Qt. CI enforces this twice: a
   Qt-free job builds the core, and every configure fails if
   `ecuio-core` links a `Qt*::` target
   ([ADR-0001](adr/0001-qt-free-core-c-abi.md)).
2. **The UI receives final display values.** Decoding, scaling and
   units live in the core, so there is one decode path and QML can't
   drift from it. Transports encode requests; they never decode.
3. **The C ABI is the boundary.** It uses opaque handles, flat
   `extern "C"` functions and POD structs with `flags`/`reserved`
   room, and status codes, enum values and wire opcodes are
   append-only. That keeps foreign-language callers working across
   versions. Every public name carries the core's own prefix,
   `ecuio_` or `ECUIO_` (ECU I/O). It names the domain rather than
   the product, so a product rename leaves the ABI alone. A strict-C
   compile of every public header pins it.

## A frame's journey

| Step | Where | Thread |
|---|---|---|
| 1. The transport produces a frame (the mock's 50 ms tick, a recording's row when its time comes, or a J2534 read) | `mock_transport.cpp`, `replay_transport.cpp`, `j2534_transport.cpp` | transport worker |
| 2. The session hands it to every registered callback, from a snapshot of the callback list; then, for a values callback, decodes it against the profile's set and hands over one batch of values stamped with the frame's time ([ADR-0024](adr/0024-decode-on-the-worker.md); the bridge doesn't register one yet) | `ecuio_session_dispatch_callbacks` | transport worker |
| 3. The bridge copies the frame (4120 bytes; its payload is inline) and posts it, unless 1000 frames are already in flight | `QtTuneBridge::onFrame` | transport worker |
| 4. The frame log gets a row (a ring of the newest 10k plain rows, the hex formatted only when shown); the profile's signal defs decode it into a staging buffer, numbers and text apart | `handleFrameInternal` | GUI |
| 5. Every 100 ms the staging buffer flushes into the signal model, and link health is polled | coalesce timer | GUI |
| 6. Pages bind to the models: Log to frames, Live and Dashboard to signals | QML | GUI |

**Why each step is shaped this way:**
- **Snapshot dispatch:** a callback can register or unregister
  callbacks without deadlocking. A callback must never close or
  destroy its session, because both join the worker it's running on
  ([ADR-0002](adr/0002-callback-pair-identity-snapshot-dispatch.md)).
  The other side of the snapshot: a dispatch already under way can
  call a callback once more after its unregister returns, so its
  `user_data` stays valid until close returns (`core.h`).
- **Backpressure:** it bounds frames *in flight*, never a lifetime
  count, so a fast bus drops frames rather than freezing the stream
  ([ADR-0003](adr/0003-backpressure-bounds-in-flight-frames.md)).
- **Coalescing:** values reach QML at 10 Hz whatever the bus rate,
  because a bus can outrun the screen
  ([ADR-0005](adr/0005-coalesced-signal-updates.md)).

## Sessions, profiles and decode

A URI picks the transport, and an optional `?profile=` picks how frames
decode.

| URI | Transport | Default profile |
|---|---|---|
| `mock://[label][?profile=…]` | Mock, on every platform | `mock` |
| `replay://<recording folder or frames.csv>[?query]` | Replay, on every platform ([ADR-0023](adr/0023-replay-transport.md)) | `raw` |
| `j2534://[vendor or DLL path][?query]` | J2534, Windows only | `obd2` |
| any URI with `?profile=raw` | as above | `raw` (log only, no decode) |
| any URI with `?profile=uds-generic&did=…` | as above | `uds-generic`: the DIDs the URI names, polled or scanned ([ADR-0020](adr/0020-uds-generic.md)) |

Every scheme shares one strict query grammar (`core/src/uri_query.h`).
`ecuio_session_create` splits the query once and refuses any key the
scheme's transport doesn't take, before anything attaches; `mock://`
takes only `profile` and `did`, both core's, and `replay://` adds
`speed` (0.1 to 100, or `max`) and `loop`.

A **replay** plays a recording's `frames.csv` ([ADR-0017](adr/0017-session-recording.md))
back through the frame API, each frame when the recording's own
session time over the speed has passed, and stamped with that time.
It holds only what the bridge received, never the requests, so it's
passive: no request engine and no link health. Its default profile is
`raw`, which can't decode a recording wrongly; a recorded J2534
session decodes under `?profile=obd2` because its defs match replies,
not requests. The bridge logs "Replay finished" once at the end of the
file, and the session stays open. A file that changed after create
stops playback early with `ECUIO_ERR_RECORDING_CHANGED`, which the
link state shows as `stopped`.

A **profile** is a named signal set ([ADR-0013](adr/0013-protocol-profiles.md)).
- **Signal defs:** each def says where its value sits in a frame
  (start byte, bit offset and length, value kind, byte order, scale,
  offset) and which frames it belongs to (message ID plus an optional
  mux key of up to 3 bytes). The decoder checks the def's kind and
  byte order before its bit parameters, and computes in `double`
  ([ADR-0016](adr/0016-signal-definition-layout.md)).
- **The `obd2` set** also declares its CAN channel (requests to
  `0x7E0`, replies on `0x7E8`). Its defs carry Service 01 mux keys,
  so the J2534 transport derives both its receive filter and its
  requests from the profile; a bare `j2534://` needs no parameters.
- **The `uds-generic` set** is built per session from the URI's
  `?did=` entries (`core/src/uds_generic.h`) and owned by the session:
  one `HEX`, `TEXT` or big-endian numeric def per DID, keyed
  `62 <DID>`, and one channel per ECU named. The registry holds only an
  empty set for the name. Single DIDs are polled; a range is scanned
  once by the request engine beside the rotation, and the DIDs the ECU
  answers join it. The mock answers them, so the profile runs with no
  hardware.
- **Unknown or malformed profile names** fail at session create,
  before any DLL loads.
- **Registration validates.** `ecuio_profile_register` runs the set
  validator, which reads the set's layout version first, accepts
  exactly the defs the decoder can decode, requires reserved bytes to
  be zero, and rejects duplicate def IDs. A refusal can say which
  check failed and on which def or channel (`ecuio_validation_t`).
  The built-ins (`mock`, `raw`, `obd2`, `uds-generic`)
  can't be replaced or unregistered, and the registry keeps the
  caller's pointer, so a registered set must outlive its sessions
  (`profile.h`).

**Lifecycle:**
- `create` resolves the URI and profile and attaches the transport,
  which for J2534 loads and opens the DLL.
- `start` registers the receive filters and starts the worker.
- `close` stops the worker and tears the transport down, and is safe
  to call twice. `destroy` frees the session.

Load failures surface at create, so nothing half-starts.

**Two encode/decode pairs must change together**, because a mistake in
either half still passes structural validation:
- the mock encoder (`mock_drive_cycle.h`) and the mock defs;
- the `obd2` defs and the fake vendor DLL's canned replies.

Tests round-trip real frames through both pairs.

## J2534

The full reference is [j2534.md](j2534.md).

- **Backend routing:** the loader reads the DLL's PE header. An x86 DLL
  can't load into the 64-bit app, so it runs in the **surrogate**, a
  separate 32-bit process the app talks to over a named pipe. An x64
  or unrecognized DLL loads in-proc, so an honest load error
  survives. The transport above doesn't know which backend it's on
  ([ADR-0008](adr/0008-x86-surrogate-host.md),
  [ADR-0009](adr/0009-pe-architecture-routing.md)).
- **The surrogate is single-threaded:** one request in, at most one
  vendor call, one reply out. Vendor calls are serialized by
  construction and no locks exist. A vendor status is data, never a
  reason to exit ([ADR-0014](adr/0014-single-threaded-surrogate.md)).
- **Startup order is diagnostic:** the child connects the pipe
  *before* loading the DLL. So a handshake timeout means process
  startup stalled, an exit code names a failed load, and an
  unanswered open means a vendor stuck in its own code.
- **One session log** (`%TEMP%\ecuio-surrogate.log`) holds both the
  client's and the child's lines, stamped on one clock. A GUI launch
  loses nothing, and each spawn keeps the log it replaces.
- **The two build trees must match:** a fingerprint of the shared
  headers, generated at configure time, is checked at `OP_OPEN`, so a
  stale `build-win32` fails at connect instead of misreading
  constants.
- **The pipe is private:** an unguessable name, a first-instance-only
  create, no remote clients, and a DACL for the current user alone.
  The child inherits the session log and no other handle.
- **Paths are UTF-8** across the ABI and UTF-16 at every Windows call,
  so a non-ASCII install path loads.
- **One worker loop** serves the profile's requests, a single
  `?poll=` PID (which outranks them), or passive listening. The
  request engine ([ADR-0015](adr/0015-request-engine.md)) derives the
  requests from the defs (`41 <PID>` asks for `01 <PID>`, `62 <DID>`
  for `22 <DID>`), sends only reads, keeps one request out per
  channel and sends the next when the reply is in, after a 100 ms wait
  at most and no sooner than 20 ms after the last. Before a channel's
  rotation, its probe reads the ECU's supported-PID bitmaps, and a
  request leaves only when the ECU says it isn't supported, never on
  silence. Beside the rotation, taking turns with it, each channel asks
  its ECU once what it is, the VIN and its identifiers
  ([ADR-0021](adr/0021-vehicle-identification.md)), for the activity
  log and the recording.
- **Teardown order is a hard contract:** join the worker → stop the
  filters → disconnect → close → unload. The worker calls vendor code
  through pointers that dangle once the DLL unmaps.
- **Never trust a vendor's message count under an error status.** Real
  adapters leave it untouched, so trusting it would replay stale
  frames as live data (`ecuio_trusted_count` in `j2534_ptypes.h`).

## Link health

The J2534 worker judges every read and write and publishes one state.
The bridge polls it on its 100 ms tick and maps it for the UI
(`Theme.linkColor`, `Theme.linkLabel`).

| Core code | `linkState` | Means |
|---|---|---|
| `ECUIO_OK` | `ok` | Replies are arriving |
| `ECUIO_ERR_VEHICLE_TIMEOUT` | `quiet` | The adapter answers, but nothing arrives (debounced over ~1 s): no ECU awake on a bus that still acknowledges, as with the first vehicle's ignition off |
| `ECUIO_ERR_REQUEST_FAILED` | `requests` | Writes fail while reads stay clean: nothing on the bus ACKs (the adapter on no live bus, or a bus fully asleep) |
| `ECUIO_ERR_TRANSPORT_HARD_FAILURE` | `fault` | Reads fail: the adapter isn't answering |
| `ECUIO_ERR_RECORDING_CHANGED` | `stopped` | A replay's file changed after it was opened, so playback stopped early ([ADR-0023](adr/0023-replay-transport.md)) |
| no session | `disconnected` | |

- **Why writes are judged apart from reads:** a quiet bus fails every
  write while the adapter is fine. Calling that an adapter fault would
  send the user after the wrong problem.
- **Backoff:** while a failure lasts, probing slows from the request
  pace to one try per 5 s. Each write is also the recovery probe, so
  writing never stops.
- **When reads recover but writes don't,** the state steps down from
  `fault` to `requests`: the adapter answers again, so what fails is
  the requests.

## The UI layer

- **Bridge threading:** `QtTuneBridge` touches only atomics on the
  worker thread; everything else runs on the GUI thread. It links
  only QtCore, so `test_bridge` can exercise it headless.
- **The QML module is a library:** the `QtTune` module (the pages,
  `Theme`, the bridge and its `Bridge` registration) builds as the
  static library `qttune_ui`. The app and `test_qml` both link it and
  its plugin, so the QML tests load the module the app runs.
- **Bridge in QML:** pages reach it as the `Bridge` singleton of the
  `QtTune` module (`Bridge.connected`, `Bridge.signalModel`). The
  registration lives in `app/bridgesingleton.h`, so the bridge itself
  stays QtCore-only, and qmllint checks every binding against it.
- **Bridge outputs:** it exposes a frame model (a ring of the newest
  10k frames, newest or oldest first, with a pause for the Log page), a
  signal model (value, `hasValue`,
  `ageMs`, `decimals`, a 120-sample history per signal), the unit
  system, device discovery, and the link state.
- **Recording is the bridge's too**
  ([ADR-0017](adr/0017-session-recording.md)): with it on, the
  bridge writes every frame it accepts, each decoded value and the
  activity lines to one folder per connect. It sits where those three
  already meet, on the GUI thread, so core is unchanged.
- **Display strings come from core:** each def's name, and the unit
  and decimal places `ecuio_display_convert` gives for the chosen unit
  system, metric or imperial
  ([ADR-0022](adr/0022-display-units.md)), on Live and the Dashboard
  alike, so neither page formats a unit, converts a value or picks a
  precision of its own. The bridge converts on the way out; its
  history and recordings keep the decoded values.
- **Pages:** **Home** (status), **Dashboard** (gauges and shift
  lights), **Live** (value table and strip chart), **Log** (raw
  frames), **Settings** (device picker, URI, activity log). Settings is
  the one place a session is connected; the header and Home open it.
  It composes the URI from a preset or a device, a profile row
  (default, `raw`, `uds-generic`) and a DID field, and leaves the
  field editable: a typed URI sets the row and the field from its
  `profile=` and `did=`, and the next click recomposes it with every
  other key kept. Core judges the result at connect and Settings shows
  its words.
  Navigation is a sidebar at 720 px and wider, a tab bar below that.
- **`Theme`** holds the palette, the chart's series colors, the
  monospace family and page margin, and the link-state color and
  label functions, so every page shows a state the same way.
  `PageHeader` gives every page but Home the same title row.
- **Live** keys each chart trace's color to its list row, so the
  row's swatch is the legend and a trace keeps its color when
  another signal is unsubscribed. The ticks choose what's plotted and
  nothing else. Each trace scales on its own, so its value is printed
  at its right-hand end; its range is at least a few displayed digits,
  so a one-count step stays small, and a gap longer than the stale
  threshold breaks the trace instead of drawing a ramp. The layout is
  arithmetic in the `ChartLayout` singleton, which the tests run with
  synthetic series; the Canvas only strokes what it returns. A Canvas
  paints an image at its own size, so this one is sized up by the
  zoom and scaled back down, which keeps it as sharp as the rest.
- **`SignalHub`** gives the Dashboard signals by *name*, because gauges
  want "RPM" whichever profile provides it. The mock and `obd2` sets
  share names; a profile that names a signal differently adds the
  name to the gauge's `Tap`.
- **Freshness:** a value never received shows "—", never a fabricated
  0. A value older than 3 s dims; the model refreshes ages every tick,
  so a signal that stops arriving dims even when nothing else changes.

## How it's verified

| Tier | What it proves |
|---|---|
| Unit | Core logic, decode, the C ABI |
| Bridge | The bridge and its models, headless, against the real mock worker |
| QML | The app's own QML module against a real bridge, offscreen: properties, not pixels |
| HARNESS | The whole J2534 stack against an in-tree fake vendor DLL, in-proc and through the surrogate |
| Cross-arch | x64 tests driving the x86 surrogate and fake. Local only; CI never runs it |
| Sanitizers | Core under ASan/UBSan in CI |
| Fuzzing | The signal decoder and validators under libFuzzer, 60 s each in CI ([fuzz/README.md](../fuzz/README.md)) |
| Simulator smoke | The iOS app launches and draws |
| Manual smoke | The real app, page by page |
| BENCH / Vehicle | A real adapter with no vehicle; then a real car, read-only |

The fake emits only what a test injected or what it echoes, so J2534
tests assert exact counts. Tests against the timed mock assert ranges.
Detail and gaps: [testing.md](testing.md).

## Where to find what

| Question | Document |
|---|---|
| What works, what's broken, what's next | [STATUS.md](../STATUS.md) |
| Why was it built this way | [adr/](adr/README.md) |
| How the J2534 stack works, op by op | [j2534.md](j2534.md) |
| How to build, run tests, read CI, troubleshoot | [development.md](development.md) |
| What is verified, by which test, and the known gaps | [testing.md](testing.md) |
| What changed in each release | [CHANGELOG.md](../CHANGELOG.md) |
| How it got here, and the lessons along the way | [engineering-log.md](engineering-log.md) |
| Real-hardware sessions and run sheets | [bench-sessions/](bench-sessions/bench-runsheet.md) |
| What only real hardware can confirm, and what it has | [bench-sessions/bench-prep.md](bench-sessions/bench-prep.md) |
| Safety and legal caveats | [DISCLAIMER.md](../DISCLAIMER.md) |
