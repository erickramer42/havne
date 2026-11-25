# Architecture

A short map of how Havne fits together and why. Each section points to
the document that holds the detail.

Havne is an ECU reader and logger. A Qt-free C++17 **core** sits
behind a C ABI. It owns sessions, transports and signal decoding. A
Qt 6 / QML **front end** turns the core's output into pages. Between
them, a single **bridge** object moves data from the core's worker
threads onto the GUI thread.

```
 QML pages: Home · Dashboard · Live · Diagnostics · Log · Recordings · Settings
   │  Theme (colors, link-state labels) · SignalHub (signals by ref)
   ▼  properties and list models, GUI thread only
 SessionBridge (app/)          the app's caller of the core
   │  worker → GUI marshaling, 100 ms coalescing, link-health polling
   │                   havne-cli (cli/): a session as JSON lines on
   │                   standard output, the public headers alone (ADR-0031)
   │                   python/havne (the havne package): the shared core
   │                   ecuio through ctypes, a wheel per release (ADR-0053)
   ▼  C ABI: core.h · signals.h · profile.h · j2534.h · serial.h · stream.h
   │         socketcan.h · vin.h · sim.h · recording.h · plugin.h · obdb.h · queue.h
 ecuio-core (core/)           no Qt, ever
   │  sessions + callbacks · profiles + decode · transports · pull queues
   │  recordings, exports, imports, health reports · the simulated ECU
   ├── mock://    worker thread, scripted drive cycle
   ├── replay://  worker thread, a recording's frames.csv at its own pace
   ├── elm327://  worker thread → a serial port (USB, Bluetooth SPP), a
   │              WiFi adapter's TCP socket, or a link the app opens
   │              (Bluetooth on a phone): an ELM327-compatible adapter's
   │              AT commands, one at a time
   ├── socketcan:// worker thread → a CAN_RAW socket on a Linux CAN
   │              interface: every frame, the kernel's times; requests
   │              through ISO-TP in core
   └── j2534://   worker thread → loader, which picks a surrogate by the
                  DLL's architecture: client ══ named pipe ══
          ├── ecuio-j2534-surrogate.exe      (x86 process, x86 vendor DLLs)
          └── ecuio-j2534-surrogate-x64.exe  (x64 process, x64 vendor DLLs)
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
   compile of every public header pins it. Core also builds as a
   shared library (`ecuio`, ADR-0053) that exports those functions
   and nothing else, for programs that load it by path, and a caller
   that can't take a callback on the worker reads a session through a
   pull queue (`queue.h`) on a thread of its own.

## A frame's journey

| Step | Where | Thread |
|---|---|---|
| 1. The transport produces a frame (the mock's 50 ms tick, a recording's row when its time comes, a J2534 read, an ELM327 reply's lines reassembled, or a SocketCAN read) | `mock_transport.cpp`, `replay_transport.cpp`, `j2534_worker.cpp`, `elm327_worker.cpp`, `socketcan_transport.cpp` | transport worker |
| 2. The session hands it to every registered callback, from a snapshot of the callback list; then it decodes the frame against the profile's set and hands one batch of values, stamped with the frame's time, to each values callback ([ADR-0024](adr/0024-decode-on-the-worker.md)); a recording session also puts the frame and its values into its recorder's ring, which the recorder's thread writes to the folder ([ADR-0049](adr/0049-recordings-in-core.md)); each pull queue a caller opened gets a copy of the frame, or of its values, which the caller reads on its own thread ([ADR-0053](adr/0053-python-bindings.md)); a session with a set of conditions attached judges each batch of values against it, on the frame's own time, and an armed recorder opens its window from the events ([ADR-0063](adr/0063-conditions.md)) | `ecuio_session_dispatch_callbacks` | transport worker |
| 3. The bridge copies the frame (4120 bytes; its payload is inline) and posts it, unless 1000 frames are already in flight; the batch's values go straight into the bridge's staging, under a lock, so a frame dropped at the cap keeps its values | `SessionBridge::onFrame`, `onValues` | transport worker |
| 4. The frame log gets a row (a ring of the newest 10k plain rows, the hex formatted only when shown) | `handleFrameInternal` | GUI |
| 5. Every 100 ms the staging is swapped out: each signal's window (newest, lowest and highest, at their frames' times) goes into the signal model; link health and the recording's state are polled | coalesce timer | GUI |
| 6. Pages bind to the models: Activity to frames, Live and Dashboard to signals | QML | GUI |

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
  ([ADR-0003](adr/0003-backpressure-bounds-in-flight-frames.md)). A
  dropped frame misses the frame log, and its values, staged from the
  worker, don't. The recording misses neither: core writes it from the
  worker, before the bridge's callbacks.
- **A plugin's module on the worker:** on a channel a conversation
  plugin's module is bound to, a requesting worker hands each frame to
  the module after the engine takes it, before step 2, and steps the
  module each pass ([ADR-0066](adr/0066-conversation-tier-and-write-gates.md)'s
  appendix of 2026-11-22). A security access's seed and key reach the
  module whole and the recording cut (`core/src/access_cut.h`).
- **Decode on the worker:** core runs the match and decode, so every
  front end gets values without a loop of its own, and the GUI thread's
  cost per frame doesn't grow with the profile
  ([ADR-0024](adr/0024-decode-on-the-worker.md)).
- **Coalescing:** values reach QML at 10 Hz whatever the bus rate,
  because a bus can outrun the screen
  ([ADR-0005](adr/0005-coalesced-signal-updates.md)).
- **One clock:** every transport stamps its frames, the request engine
  schedules, the recorder writes `host_us` and the model ages values on
  `ecuio_clock_us`, which keeps counting while the machine sleeps
  (`CLOCK_BOOTTIME` on Linux and Android, where `steady_clock` stops).
  A sleep is then a gap on Live and in a recording rather than the
  values on either side joined, and the bridge logs "Asleep for N s"
  ([ADR-0033](adr/0033-session-time-counts-sleep.md)). Waits stay on
  `steady_clock`, the only clock the standard's waits take.

## Sessions, profiles and decode

A URI picks the transport, and an optional `?profile=` picks how frames
decode.

| URI | Transport | Default profile |
|---|---|---|
| `mock://[label][?profile=…]` | Mock, on every platform | `mock` |
| `replay://<recording folder or frames.csv>[?query]` | Replay, on every platform ([ADR-0023](adr/0023-replay-transport.md)) | `raw` |
| `j2534://[vendor or DLL path][?query]` | J2534, Windows only | `obd2` |
| `elm327://<port>[?query]` | An ELM327-compatible adapter on a serial port (`COM5`, `/dev/ttyUSB0`) or at a WiFi adapter's address (`192.168.4.1:23`), on every platform ([ADR-0026](adr/0026-elm327-transport.md)) | `obd2` |
| `socketcan://<ifname>[?query]` | SocketCAN, Linux only ([ADR-0032](adr/0032-socketcan-listening.md), [ADR-0040](adr/0040-socketcan-requests-isotp.md)) | `raw` |
| any URI with `?profile=raw` | as above | `raw` (log only, no decode) |
| any URI with `?profile=uds-generic&did=…` | as above | `uds-generic`: the DIDs the URI names, polled or scanned ([ADR-0020](adr/0020-uds-generic.md)) |
| any URI with `?profile=user.<name>` | as above | a signal file's set, the user's own PIDs and DIDs ([ADR-0051](adr/0051-signal-files.md)) |
| any URI with `?profile=plugin.<id>.<variant>` | as above | a plugin variant's set, its files merged ([ADR-0052](adr/0052-plugin-framework.md)) |

Every scheme shares one strict query grammar (`core/src/uri_query.h`).
`ecuio_session_create` splits the query once and refuses any key the
scheme's transport doesn't take, before anything attaches; `mock://`
takes `profile` and `did`, both core's, and `diagnose` for its ECU's
trouble codes, and `replay://` adds `speed` (0.1 to 100, or `max`),
`loop`, a stretch (`from`, `to`) and fault injection (`faults`, `seed`,
[ADR-0041](adr/0041-replay-fault-injection.md)). `j2534://`,
`elm327://` and `socketcan://` share the request keys (`filter`,
`txid`, `poll`, `probe`, `identify`, `diagnose`, `pack`, `packframes`,
`scan`, `gap`; the values are `core/src/request_params.h`) and derive
their channels and requests the same way (`core/src/request_setup.h`), so a run sheet's query means the same on
each; `baud` is the CAN rate on all three, `j2534://` adds `flags`, and
`elm327://` adds `serial`, the serial port's rate for a wired adapter.

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
link state shows as `stopped`. A last row with no newline, a write cut
short, is left out and counted in the session info.

A **profile** is a named signal set ([ADR-0013](adr/0013-protocol-profiles.md)).
- **Signal defs:** each def says where its value sits in a frame
  (start byte, bit offset and length, value kind, byte order, scale,
  offset) and which frames it belongs to (message ID plus an optional
  mux key of up to 3 bytes). The decoder checks the def's kind and
  byte order before its bit parameters, and computes in `double`
  ([ADR-0016](adr/0016-signal-definition-layout.md)).
- **The `obd2` set** also declares its CAN channel (requests to
  `0x7E0`, replies on `0x7E8`). Its defs carry Service 01 mux keys,
  so the J2534 and ELM327 transports derive both their receive filter
  and their requests from the profile; a bare `j2534://` needs no
  parameters.
- **The `uds-generic` set** is built per session from the URI's
  `?did=` entries (`core/src/uds_generic.h`) and owned by the session:
  one `HEX`, `TEXT` or big-endian numeric def per DID, keyed
  `62 <DID>`, and one channel per ECU named. The registry holds only an
  empty set for the name. Single DIDs are polled; a range is scanned
  once by the request engine beside the rotation, and the DIDs the ECU
  answers join it. The mock answers them, so the profile runs with no
  hardware.
- **Signal files** ([ADR-0051](adr/0051-signal-files.md),
  [signal-files.md](signal-files.md)) are a user's own signals as
  JSON: core reads one from bytes through a strict parser
  (`core/src/json.h`), refuses everything it gets wrong with a line
  and column, and registers it as the profile `user.<name>`, its
  modules as channels and its PIDs and DIDs as requests. A file may
  extend `obd2`. Core never opens a path: the app and `havne-cli`
  read the file, so a phone's picked file goes through the same load.
  `havne-cli signals import obdb` converts an OBDb signal set into one
  (`obdb.h`), and so does Settings (`app/obdbimport.h`). A recording holds the file it decoded with as
  `signals.json`, its hash in `events.log`'s header, and a replay of
  that recording decodes through that copy when it still matches,
  under the recorded name or one of its own, so a file edited or
  removed since doesn't change what the replay shows;
  `cli/recording_signals.h` reads the header's lines for the app and
  `havne-cli` alike, through the public headers only.
- **Refused signals:** a request a negative reply refuses leaves the
  rotation and goes into its channel's support report
  (`ecuio_session_get_channel_support`), the refusals listed ahead of
  the requests a bitmap left out. `cli/refused_signals.h` maps each
  refusal to the defs that read its reply (on the channel's reply ID,
  by the reply's service and key) and counts those past the report's
  list; the app's Live row reads "Refused" with the code's name, and
  `havne-cli`'s `refused` line names the same. A replay has no support
  report, so it shows no refusal.
- **Plugins' data tier** ([ADR-0052](adr/0052-plugin-framework.md),
  `plugin.h`): a package of signal files with one manifest, as a
  folder or packed into one file. Each variant, an ordered list of the
  package's files merged by signal key, registers as
  `plugin.<id>.<variant>`, and a VIN's manufacturer code and model year
  propose the variants that fit. A plugin can't be freed while a
  session decodes with one of its sets (`ECUIO_ERR_IN_USE`).
- **Plugins' conversation tier** ([ADR-0066](adr/0066-conversation-tier-and-write-gates.md);
  built, inert in v0.9.0): a format 2 manifest may carry a WebAssembly
  module. Core checks its shape before a runtime loads it
  (`wasm_check.h`), runs it in a sandbox (`wasm_sandbox.h`: an
  instruction budget for each call, a memory of a fixed size counted by
  core's allocator, no import) and lets what it causes reach the car
  only through tier 2's ceiling (`tier2_ceiling.h`) and the engine's
  talk door (`request_talk.h`). A security-access attempts' store
  (`security_store.h`) keeps the failed attempts and their delay across
  a restart. A module binds only behind the consent gate (`consent_gate.h`): a consent a person gave to the plugin's fingerprint and a front end applied, a `vcan` link or the test door; otherwise such a plugin is data only.
- **Enumerations and computed signals:** an enum def is a raw number
  with a label from the set's table (`ecuio_enum_label`); a computed
  def reads no frame and runs a bounded program over up to four other
  signals, each no older than its age bound, after its frame's own
  values in the same batch. The mock's boost is one (MAP minus baro).
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
  to call twice.
- `destroy` frees the session.

Load failures surface at create, so nothing half-starts.

**Two encode/decode pairs must change together**, because a mistake in
either half still passes structural validation:
- the mock encoder (`mock_drive_cycle.h`) and the mock defs;
- the `obd2` defs and the fakes' canned replies (`tests/fake_ecu.h`,
  the one ECU the fake J2534 DLL and the fake ELM327 both answer from).

Tests round-trip real frames through both pairs.

## J2534

The full reference is [j2534.md](j2534.md); the surrogate has its own,
[surrogate.md](surrogate.md).

- **Routing:** the loader reads the DLL's PE header and loads it in a
  **surrogate**, a separate process of the DLL's architecture that the
  app talks to over a named pipe: the 32-bit one for an x86 DLL (which
  can't load into the 64-bit app), the 64-bit one for every other. A
  vendor DLL that crashes ends its surrogate, not the app, and every
  session's vendor calls are in the surrogate's session log. A file
  that isn't a DLL is refused before any spawn. The transport above
  doesn't know which surrogate it's on
  ([ADR-0008](adr/0008-x86-surrogate-host.md),
  [ADR-0025](adr/0025-one-j2534-backend.md)).
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
  for `22 <DID>`), keeps one request out per
  channel and sends the next when the reply is in, after a 100 ms wait
  at most and no sooner than 20 ms after the last. Before a channel's
  rotation, its probe reads the ECU's supported-PID bitmaps, and a
  request leaves only when the ECU says it isn't supported, never on
  silence. Beside the rotation, taking turns with it, each channel asks
  its ECU once what it is, the VIN and its identifiers
  ([ADR-0021](adr/0021-vehicle-identification.md)), then its trouble
  codes, readiness and freeze frame
  ([ADR-0037](adr/0037-reading-trouble-codes.md)), for the activity
  log, the recording and the Diagnostics page; and, when the user
  confirms a clear there, its engine and vehicle speeds and then `04`,
  at 0 km/h unless the user confirms once more for a moving vehicle
  ([ADR-0038](adr/0038-clearing-trouble-codes.md) and its appendix of
  2026-11-01). `havne-cli clear <uri> --yes` runs the same clear from a
  script, each ECU at the report it wrote first.
- **A focus narrows a running session** to picked signals
  (`ecuio_session_set_focus`, [ADR-0056](adr/0056-analysis-log.md)
  section 2): the engine's rotation on each channel asks only the picks'
  requests, while the probe, the DID scan, identification and diagnostics
  go on as before. The focus takes effect on the worker between two
  requests and lasts until it changes or ends. It can carry a minimum time
  between sweeps (`ecuio_session_set_focus_with`), for a slow adapter or a
  busy ECU: each channel starts a sweep of the picks at most that often,
  and only the rotation waits. A session that asks nothing (the mock, a
  replay, a listener) takes a focus and changes nothing.
- **The extended session is on request** ([ADR-0060](adr/0060-extended-session-reads.md)):
  with `?session=03` on `uds-generic`, each module's `10 03` goes out
  first, and nothing else to that module until it answers. Tester present
  (`3E 00`) fills a quiet gap to keep the session past S3, a `7E` or `7F`
  to a read enters the session again and asks the request again, and
  three losses within 60 s fall back to the default session with the
  module's reads going on there. The close sends `10 01` to each module
  that entered. The session requests are allowed apart from the service
  table, which stays reads only: `10 01`, `10 03` and `3E 00` are all the
  engine sends beyond it.
- **Teardown order is a hard contract:** join the worker, then stop the
  filters, disconnect, close and unload. The worker calls vendor code
  through pointers that dangle once the DLL unmaps.
- **Never trust a vendor's message count under an error status.** Real
  adapters leave it untouched, so trusting it would replay stale
  frames as live data (`ecuio_trusted_count` in `j2534_ptypes.h`).

## ELM327

`elm327://` drives an ELM327-compatible adapter, an OBDLink's STN chip
or the OBDX in its ELM mode, over a serial port that core opens itself
(`\\.\COMn` on Windows, `termios` elsewhere), a WiFi adapter's TCP
socket, or a link the app opens for it. A paired Bluetooth SPP
adapter is a serial port on every desktop system, so core never touches
a Bluetooth stack ([ADR-0026](adr/0026-elm327-transport.md)).

- **A pure protocol layer** (`core/src/elm327_protocol.h`) turns bytes
  into lines and line kinds, parses frame lines by the addressing mode,
  reassembles ISO-TP per CAN ID (two ECUs' replies can interleave), and
  formats the commands. It does no I/O and reads no clock, so every rule
  is a unit test over canned bytes.
- **The setup** sends contract commands, each of which must answer OK
  or the connect is refused naming the command and its answer, then
  readings (the version strings, the voltage) that never fail a
  connect. Headers stay on, so core sees every CAN ID and PCI byte.
- **The worker** drives the same request engine as J2534's, with one
  command outstanding for the whole adapter, since an ELM327 takes the
  next only after its prompt. A Service 01 request to one ECU whose
  reply fits one frame carries the response-count digit, so the adapter
  returns as soon as the reply is in (ADR-0026's appendix of
  2026-10-29).
- **App-fed streams** (`stream.h`, `core/src/elm327_stream.cpp`,
  [ADR-0028](adr/0028-app-fed-streams.md)) are the port for links only
  the app's framework reaches: classic Bluetooth on Android, Bluetooth
  LE. The app registers a stream by name (`bt:<address>`), and
  `elm327://bt:<address>` runs through it. Core calls the app's `open`,
  `write` and `close`, which return at once; the app pushes `opened`,
  the bytes it received and `failed`. Core waits up to 10 s for an open
  on its own thread, buffers what arrives (64 KB; more fails the link),
  and reopens on the worker's backoff, so the app never reconnects by
  itself and never blocks core. On Android the app's side is
  `app/btstream.cpp` (a classic adapter's SPP socket, or a BLE
  adapter's GATT service) and the `Bluetooth` QML singleton, which
  lists the paired adapters and scans for BLE ones; the bridge, which
  stays QtCore-only, asks it to register the URI's stream before each
  connect.
- **Discovery** (`ecuio_serial_ports`, `serial.h`) lists the ports the
  OS knows (the `SERIALCOMM` registry key on Windows, `/dev/cu.*` on
  macOS, `/dev/ttyUSB*`, `ttyACM*` and `rfcomm*` on Linux) and opens
  none: an adapter's commands could disturb whatever else sits on a
  port, so the user picks it in Settings, as with a J2534 interface.
- **The session line** names the adapter's firmware (`AT I`, plus
  `STI` on an STN), the CAN rate, the channels and the battery, and
  warns when an STN is set not to wait past a `7F xx 78`.
- **A wire log** per connect (`core/src/elm327_log.cpp`,
  `ecuio-elm327.<YYYYMMDD-HHMMSS-mmm>.log` in the temporary directory,
  `%TEMP%` on Windows, the newest 50 kept),
  the counterpart of the surrogate's session log: every setup command
  and what came back, the readings, lines the adapter sent unasked, and
  the port's failures and reopens. With `ECUIO_SUR_LOG_FRAMES=1`, which
  a recording sets, every request and reply line too, each prompt with
  its time after the request.
- **The adapter's memory is never written,** and no serial number is
  asked for.
- **Passive listening:** a session with nothing to ask monitors with
  `AT MA` (ADR-0026's appendix "passive listening"), which keeps the
  receive filter, so it hears its channels' IDs and never acknowledges.
  Five seconds with no line stop and restart it, which proves the
  adapter is there; `BUFFER FULL` flags the next frame
  `ECUIO_FRAME_FLAG_OVERFLOW`, as a J2534 driver's
  `ERR_BUFFER_OVERFLOW` does; closing stops it with a space. A session
  with no channel is refused at create.
- **Not yet:** Bluetooth LE on iOS and the desktop (ADR-0028 step 5),
  and the whole bus unfiltered.

The full reference is [elm327.md](elm327.md).

## The simulated ECU

`sim.h` is a car with no car ([ADR-0046](adr/0046-simulated-ecu.md)):
the mock's drive cycle or canned values, or a recording's replies, with
a second module, a signal file's or a plugin's DIDs, and faults on
demand. On Linux `havne-cli sim <uri> --on <vcan>` answers requests on a
virtual bus as the engine ECU. Unbound, it takes one request at a time
and returns each ECU's reply on the caller's clock (`ecuio_sim_ask`),
which is how `havne-cli sim <uri> --elm327 tcp:<port>` serves it as an
ELM327 adapter on a TCP port, on every desktop system, for Havne's own
`elm327://127.0.0.1:<port>`, python-OBD and ELM327 apps
([ADR-0055](adr/0055-elm327-emulator.md)).

## Link health

The J2534, ELM327 and SocketCAN workers judge every read and write
(`core/src/link_health.h`) and publish one state. An ELM327's `NO DATA`
leaves the request engine's wait to time the request out, `CAN ERROR`
is a write that failed, and a port that fails, or no prompt within 5 s,
is a hard failure: the port reopens on the backoff's schedule.
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
- **A set too large to ask whole.** A session on it asks nothing until
  something is picked, and reports no fault for that silence.

## The UI layer

The pages are QML over a thin C++ bridge. None of them parses a frame,
converts a unit or picks a precision: core hands over final display
values. Each part links to the doc that holds its detail.

- **The bridge** (`SessionBridge`, `app/sessionbridge.h`) sits between
  core and QML. Its callbacks run on the worker thread: the frame
  callback touches only atomics and posts a copy of the frame, and the
  values and conditions callbacks stage what they get under a short
  lock, which the 100 ms tick swaps out. Everything else runs on the GUI thread. It
  links QtCore, Gui and Network (no QML), so `test_bridge` exercises it
  headless, with the stores beside it (`app/pluginstore.h`,
  `app/storemigration.h`). Its state, stateless statics and data-only
  test seams are its base classes in `app/bridgestate.h`,
  `app/bridgeformats.h` and `app/bridgeseams.h`. The bounds and the coalescing:
  [ADR-0003](adr/0003-backpressure-bounds-in-flight-frames.md),
  [ADR-0005](adr/0005-coalesced-signal-updates.md); the costs:
  [performance.md](performance.md).
- **Pages get data through one singleton.** The `App` QML module (the
  pages, `Theme`, the bridge's registration) builds as the static
  library `app_ui`, which the app and `test_qml` both link, so the QML
  tests load the module the app runs. Pages reach the bridge as the
  `Bridge` singleton (`Bridge.connected`, `Bridge.signalModel`),
  registered in `app/bridgesingleton.h` so the bridge stays QtCore-only
  and qmllint checks every binding ([development.md](development.md#everyday-tools)).
  It exposes a frame model (the newest 10k frames), a signal model
  (value, `hasValue`, `ageMs`, `decimals`, and a history of each
  window's lowest, highest and newest values for the charts), the unit
  system, device discovery and the link state ([Link health](#link-health)).
  A value never received shows "—", never a made-up 0, and an old one
  dims ([user-guide.md](user-guide.md#read-live-values)).
- **Display strings come from core:** each def's name, and the unit and
  decimal places `ecuio_display_convert` gives for the chosen unit
  system ([ADR-0022](adr/0022-display-units.md)), so no page formats a
  value of its own. History and recordings keep the decoded values.
- **Recording and viewing are core's**
  ([ADR-0017](adr/0017-session-recording.md),
  [ADR-0049](adr/0049-recordings-in-core.md)): one folder per connect
  holds every frame, each decoded value and the activity lines. The
  worker queues them into a ring and the recorder's thread writes the
  files, so a slow disk never holds the worker up and the frames the
  Activity page drops are recorded too. Reading, exports, imports, the
  health report and the analysis log are core's as well, with
  `havne-cli` and the Recordings page as two front ends
  ([ADR-0054](adr/0054-exports-and-imports.md),
  [ADR-0056](adr/0056-analysis-log.md),
  [the command-line client](development.md#the-command-line-client),
  [user-guide.md](user-guide.md#view-and-export-a-recording)).
- **Pages and navigation:** Home, Dashboard, Live, Diagnostics,
  Activity, Recordings and Settings, as a sidebar on a desktop (a
  laptop too) and a tab bar of five on a phone
  ([user-guide.md](user-guide.md), [screenshots.md](screenshots.md)).
  Settings is the one place a session is connected: it composes the
  address and core judges it at connect
  ([user-guide.md](user-guide.md#connect)). Android's Back takes one
  route across the pages ([ADR-0059](adr/0059-drive-view.md)).
- **Desktop and phone share the module;** the phone differs in its
  navigation and the Drive view. The Dashboard's pages are one JSON
  document per form, and a gauge names its signal by a reference that
  outlives the profile (`SignalHub`: `id:<n>` for a built-in def,
  `key:<k>` for a signal file's own)
  ([ADR-0057](adr/0057-dashboards.md)). `Theme` resolves one of four
  palettes from the look and Night ([ADR-0058](adr/0058-night-mode.md)),
  so every page shows a link state the same way.
- **Preferences and vehicles:** Qt's `QSettings` under the product's
  name, or an ini file under `HAVNE_SETTINGS_DIR`
  ([development.md](development.md#everyday-tools)). A named vehicle is
  recognized by a salted hash of its VIN
  ([ADR-0027](adr/0027-vehicle-profiles.md)) and names the plugin
  variant picked for it. Every import (a plugin, a signal file, an OBDb
  set) is kept as a plugin in the app's one store, a copy named by its
  SHA-256 ([plugins/README.md](../plugins/README.md#in-the-app),
  [ADR-0064](adr/0064-one-store.md)).
- **On a phone,** a session keeps running in the background
  ([mobile-build.md](mobile-build.md#the-phone-in-the-background),
  [ADR-0029](adr/0029-android-background-sessions.md)), and the phone's
  Bluetooth reaches core as an app-fed stream ([ELM327](#elm327)).

## How it's verified

| Tier | What it proves |
|---|---|
| Unit | Core logic, decode, the C ABI |
| Bridge | The bridge and its models, headless, against the real mock worker |
| Python | The `havne` package against the shared core under pytest, every example script, and a wheel built from the tree and installed into a fresh venv |
| QML | The app's own QML module against a real bridge, offscreen: properties, not pixels |
| HARNESS | The whole J2534 stack against an in-tree fake vendor DLL, through the x64 surrogate; the ELM327 transport against a fake ELM327 behind its port, and on Linux and macOS the serial port code over a pseudo-terminal |
| Cross-arch | x64 tests driving the x86 surrogate and fake. Local only; CI never runs it |
| Sanitizers | Core under ASan/UBSan in CI |
| Fuzzing | Each parser of outside input (frames and replies, ELM327 text and the emulator's hosts, ISO-TP, SocketCAN's netlink and frames, the replay and recording readers, candump logs and CSV imports, the exports, JSON, signal files, plugins, OBDb sets, VINs, trouble codes, `?did=`), the simulated ECU, the diagnostics rules and the set validator under libFuzzer, 60 s each in CI, the structured formats starting from seeds built from the committed fixtures ([fuzz/README.md](../fuzz/README.md)) |
| Static analysis | clang-tidy's analyzer and bug-finding checks over core, the surrogate, `havne-cli` and the desktop app, any warning failing CI; CodeQL too |
| Load test | A recorded engine start replayed through the bridge on Windows and Linux CI: throughput, drops and latency, failing on a lost frame or peak or a gross regression |
| Simulator smoke | The iOS app launches and draws |
| Manual smoke | The real app, page by page |
| BENCH / Vehicle | A real adapter with no vehicle; then a real car |

The fake emits only what a test injected or what it echoes, so J2534
tests assert exact counts. Tests against the timed mock assert ranges.
Detail and gaps: [testing.md](testing.md), with each test in [testing-inventory.md](testing-inventory.md).

## Where to find what

| Question | Document |
|---|---|
| What works, what's broken, what's next | [STATUS.md](../STATUS.md) |
| How to do each task in the app: connect, read, log, view, clear codes | [user-guide.md](user-guide.md) |
| Every address form and option, with its default | [addresses.md](addresses.md) |
| Why was it built this way | [adr/](adr/README.md) |
| How the J2534 stack works, op by op | [j2534.md](j2534.md) |
| How the J2534 surrogate process works | [surrogate.md](surrogate.md) |
| How the ELM327 transport works, and the adapters it has run on | [elm327.md](elm327.md) |
| How to build, run tests, troubleshoot | [development.md](development.md) |
| How CI runs, and how the C ABI is checked | [ci.md](ci.md) |
| How to build for Android and iOS, and make the app icon | [mobile-build.md](mobile-build.md) |
| How the Linux packages and the SBOM are made, and how a release is cut | [packaging.md](packaging.md) |
| How to run Havne on Linux: the downloads, adapters and permissions, SocketCAN, tracing | [linux.md](linux.md) |
| How to write a file of your own signals: DIDs and PIDs, enumerations, computed signals | [signal-files.md](signal-files.md) |
| How to write conditions: alerts, armed recording, markers | [condition-files.md](condition-files.md) |
| How to install Havne on an iPhone for testing, and what works there | [ios.md](ios.md) |
| What each page looks like, on the desktop and the phone, in Dark and in Night's amber and red | [screenshots.md](screenshots.md) |
| Which areas each verification tier reaches, and the known gaps | [testing.md](testing.md) |
| What is verified, by which test, with the evidence | [testing-inventory.md](testing-inventory.md) |
| What the stack costs: per frame, a recording, a connect, with how each was measured | [performance.md](performance.md) |
| Why the design and checks are enough for what Havne does, and where they stop | [security-assurance.md](security-assurance.md) |
| What changed in each release | [CHANGELOG.md](../CHANGELOG.md) |
| How it got here, and the lessons along the way | [engineering-log.md](engineering-log.md) |
| Real-hardware sessions and run sheets | [bench-sessions/](bench-sessions/bench-runsheet.md) |
| What only real hardware can confirm, and what it has | [bench-sessions/bench-prep.md](bench-sessions/bench-prep.md) |
| Safety and legal caveats | [DISCLAIMER.md](../DISCLAIMER.md) |
