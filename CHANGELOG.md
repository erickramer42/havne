# Changelog

What changed in each release, for people using Havne. The format
follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and
from v0.4.0 versions follow [Semantic Versioning](https://semver.org/)
(before 1.0, a minor version may break the C ABI;
[ADR-0048](docs/adr/0048-semantic-versioning.md)). A release's full notes,
the text of its GitHub release, are in [docs/releases/](docs/releases/).
How each change was built, with its commits and the lessons along the
way, is in [docs/engineering-log.md](docs/engineering-log.md).

## [Unreleased]

### Added

- `havne-cli sim <uri> --elm327 tcp:<port>` serves the simulated car as
  an ELM327 adapter on a TCP port, on Windows, macOS and Linux: Havne's
  own `elm327://127.0.0.1:<port>`, python-OBD and ELM327 apps that take
  a WiFi adapter's address read it with no car. It listens on 127.0.0.1
  unless you name another address, takes one host at a time, and
  answers from the same simulated ECU as `sim --on`: the mock's drive
  cycle or canned values, a recording, a second module, a signal file's
  or a plugin's DIDs, and the faults
  ([ADR-0055](docs/adr/0055-elm327-emulator.md)).
- Core's simulated ECU answers with no bus: `ecuio_sim_create_unbound`
  and `ecuio_sim_ask` (`ecuio/sim.h`) take one request and return each
  ECU's reply, its fault as the time it starts and a flag, on the
  caller's clock.
- `havne-cli clear <uri> --yes` clears each ECU's trouble codes, freeze
  frame and readiness from a script, through the same gate as the app:
  each ECU's report is written first as `diagnostics` lines, then each
  ECU is cleared at that report by its own ID, only with the engine
  stopped unless `--engine-running` gives your word (and only standing
  still unless `--moving` does too), then read again. A `clear` line
  says what each ECU did; the exit is 0 only when every ECU cleared
  ([ADR-0038](docs/adr/0038-clearing-trouble-codes.md)'s appendix of
  2026-11-06).
- The viewer and `havne-cli view --frames` can read a recording's
  values from its frames through a profile: a raw recording (a
  SocketCAN listener's among them) has values to plot, and a signal
  file written today decodes an older recording. The viewer offers it
  beside the values as recorded, and opens a folder that holds frames
  alone through its frames
  ([ADR-0049](docs/adr/0049-recordings-in-core.md)'s appendix of
  2026-11-06).
- JSON Schemas for signal files, plugin manifests and plugin value
  fixtures in `schema/`: an editor mapped to them completes and checks
  a file as you write it, and another repository can lint its files in
  CI. A file can't name its schema with `"$schema"`, which the loader
  refuses, so map it in the editor's settings by file name or folder.
- `havne-cli export <recording> <out> --format pcapng` writes a
  recording's frames for Wireshark, scrubbed: identification replies
  (Service 09, the F180-F1FF DIDs) and text signals' messages are left
  out and counted, then every message and the file itself are searched
  for the car's identity values and anything shaped like a VIN, and a
  match writes nothing. A message an adapter delivered whole goes out
  as the frames its ECU sent, each marked as made up, so Wireshark's
  ISO-TP dissector reassembles it. `--from`/`--to` take a stretch;
  `--raw` writes it as recorded, and the file says so
  ([ADR-0054](docs/adr/0054-exports-and-imports.md)'s appendix of
  2026-11-06).
- `havne-cli import candump <log> <folder> [--interface <name>]` reads
  a `candump -L` log into a recording's folder: `view --frames`, `info`
  and `replay://` read it like any other. One bus to a recording: a log
  of several takes the one you name.
- `havne-cli import csv <file> <folder> --map <map.json>` reads another
  logger's CSV of timed signals into a recording's folder that `view`
  reads like any other, by a column map: the time column (seconds,
  milliseconds, microseconds, ISO 8601 times or times of day with a
  date) and each column's signal name, unit and kind, with the
  logger's preamble, `;` and decimal commas, quoted cells and a
  logger's texts for no reading (`---`) taken. A column the map leaves
  out is ignored and named; a typo in the map is refused by its name
  ([ADR-0054](docs/adr/0054-exports-and-imports.md)'s appendix "the
  CSV import as built").
- The Recordings page lists a folder of values alone (a CSV import
  moved into the recordings folder) and opens it in the viewer, which
  says the recording has no frames to replay and hides Replay and the
  decode choice.
- A CAN FD frame's error state indicator (ESI) is kept: SocketCAN
  sessions flag it, recordings and replays carry it, and `havne-cli
  play` puts it back on the bus.
- Settings imports an OBDb signal set for a vehicle (Import an OBDb
  set, in the Vehicle section, on every platform, Android's document
  picker too): the model year comes from a VIN (the one you look up, or
  the connected car's) or is typed, and the report shows what
  converted, each field that differs and each signal left out before
  Keep saves the file as the vehicle's. Importing the set again, for
  another year say, replaces the vehicle's earlier import, and says so
  first.
- The Log page's frame table (developer mode) filters its frames by ID
  or range (`7E8, 7E0-7EF`), data bytes anywhere or from the start
  (`^62 F1 ??`), direction and flag, and says how many of how many it
  shows. By ID lists one row per ID with its frame count, rate and
  newest payload; a tap shows that ID's frames. A Flags column names
  each frame's flags, and the Time header turns the order.
- Export CSV writes the frames the Log page holds, those the filter
  shows, oldest first, with identification replies and text signals'
  messages left out and a check that fails the export rather than write
  a VIN or another identity value. Copy still copies the selected rows
  as they are ([ADR-0054](docs/adr/0054-exports-and-imports.md)'s
  appendix "frames in memory").
- Core builds as a shared library beside its static one
  (`libecuio.so`, `libecuio.dylib`, `ecuio.dll`), for a program that
  loads core by path: it exports the C API's functions and nothing
  else, which a check holds it to on every desktop platform, and
  `ecuio_version` gives core's version as three numbers
  ([ADR-0053](docs/adr/0053-python-bindings.md)). It isn't in the
  downloads yet.
- Core's pull queue (`ecuio/queue.h`): a program that can't take a
  callback on core's worker thread (Python's ctypes, Rust, Go, C#)
  reads a session's frames or decoded values in batches from a thread
  of its own, waiting up to a timeout, the oldest dropped and counted
  when it falls behind ([ADR-0053](docs/adr/0053-python-bindings.md)).
- `havne-cli export <recording> <out> --format csv` writes a
  recording's values, one row per sample (`time_s,signal,unit,value`),
  after a comment line of counts, for a spreadsheet, pandas
  (`skiprows=1`) or R. Scrubbed unless `--raw`: text signals and
  signals read from identification replies are left out, and the file
  is checked for the car's identity before it's written. A signal name,
  unit or raw text value that a spreadsheet would run as a formula
  fails the export instead of being changed
  ([ADR-0054](docs/adr/0054-exports-and-imports.md)'s appendix "the
  CSV export as built").
- `havne-cli export <recording> <out> --format folder` writes a copy
  of a recording to send, one Havne opens, views, checks and replays as
  the original. Scrubbed unless `--raw`: identification replies and
  text signals are left out of its frames and values, the session's
  address and the vehicle's label out of its log, the app's activity
  notes left out, and every file is checked for the car's identity
  before the copy is put in place. Markers keep their labels as typed.
  Name the copy by its time (`20261106-142233`): the original's folder
  name can hold the vehicle's label
  ([ADR-0054](docs/adr/0054-exports-and-imports.md)'s appendix "the
  folder export as built").
- The `havne` Python package (`python/havne/`): sessions on every
  transport with their frames and decoded values read as iterators,
  recordings, the simulated ECU, a script's own signals as a signal file
  and a plugin's variants, over core's shared library through ctypes,
  with no compiler at install time
  ([ADR-0053](docs/adr/0053-python-bindings.md)). It runs from the source
  tree against a built shared core (`HAVNE_LIBRARY`), or installs from
  a release's wheel (below).
- Core's pull queue tells a replay's end: once a replay that doesn't loop
  has played its last frame, a read returns `ECUIO_ERR_STREAM_ENDED`, so
  a loop over a replay's values finishes.
- A vehicle's baseline recording: on the Recordings page, "Use as
  <vehicle>'s baseline" keeps an open recording as the baseline of the
  vehicle picked in Settings, a Baseline badge marks its row, and
  "Compare with the baseline" overlays it on another recording, the two
  lined up on their engine starts (RPM reaching 500 after being below
  100). A baseline whose folder was moved or deleted says so, with a way
  to clear it. The compare's alignments gain "Align engine starts", and
  `havne-cli view <run> <baseline> --align-on engine-start` lines up the
  same two recordings
  ([ADR-0049](docs/adr/0049-recordings-in-core.md)'s appendix "a
  vehicle's baseline").
- A Python wheel with each release, for Windows x64, Linux x86_64 and
  64-bit ARM, and macOS on Apple silicon (13 or later):
  `pip install havne-<version>-py3-none-<platform>.whl` installs the
  `havne` package with core's library inside, and on Windows the J2534
  surrogates, so no build, `HAVNE_LIBRARY` or Visual C++
  redistributable is needed. Python 3.11 or later
  ([ADR-0053](docs/adr/0053-python-bindings.md)).
- `python/examples/read_pids.py <uri>` prints the engine speed, vehicle
  speed and coolant temperature from any session: `mock://`, SocketCAN,
  an ELM327 adapter or the ELM327 emulator.
- Three more Python examples in `python/examples/`, each runnable with
  no car: `did_scan.py <uri>` lists which identifiers (DIDs) an ECU
  answers, through core's own read-only scan on any session, printing
  each answer as text or hex; `isotp_fuzz.py --interface vcan0` sends
  malformed ISO-TP exchanges to the simulated ECU on a Linux virtual
  CAN bus, from a seed a failing case replays by, and refuses any
  interface that isn't a virtual bus; `ecu_states.ipynb` predicts fuel
  cut half a second ahead from a recording's engine speed, speed and
  throttle with scikit-learn, against a "nothing changes" baseline, on
  a synthetic recording of the mock's drive cycle that comes with it
  ([ADR-0053](docs/adr/0053-python-bindings.md)).
- Python: `havne.import_csv` and `havne.import_candump` make a
  recording's folder from a CSV or a candump log, each returning the
  import's counts.
- `havne-cli health` writes a car's health as one file, JSON or text to
  share with a shop or a buyer: the vehicle from its VIN (model year,
  make, check digit; never the VIN itself), the emissions inspection's
  verdict, each ECU's readiness, trouble codes with their titles,
  freeze frame and the values since the codes were cleared, the
  identification by name only, then the profile's key values. It reads
  a live session (reads only, parked or driving) or a recording, and
  checks the file for the car's identity before writing it. In the app,
  Save report on Diagnostics and in the log viewer writes the same file,
  text or JSON ([ADR-0054](docs/adr/0054-exports-and-imports.md)'s
  appendix "the health report as built").

## [0.5.1] - 2026-11-06

### Added

- A replay decodes with the signal file its recording holds, so a recording replays as it was recorded after the vehicle's file has changed or been removed, or on another machine: the app's Replay loads the copy under a name of its own when it differs from the file loaded now, and `havne-cli stream replay://` uses it with no `--signals`; a copy that no longer matches its hash isn't used, and the replay says why ([docs/signal-files.md](docs/signal-files.md)).
- A signal its module refuses shows as refused, not blank ([ADR-0051](docs/adr/0051-signal-files.md) section 3): Live's row reads "Refused: requestOutOfRange (7F 22 31)", the negative reply's code by name, and `havne-cli stream --values` writes a `refused` line naming each such signal with its request, the reply and the code's name. A module that leaves PIDs out of its bitmaps (most cars, with a signal file that extends `obd2`) still has its refusals named, and past the eight a module's report lists, Live's status line says how many more it refused ("ECU 0x7E8 refused 3 more requests") and the `refused` line's `more` counts them. A refused signal gives its place among the chart's default signals to the next.
- Settings' Vehicle section saves each of a vehicle's signal files out of the app with **Save a copy** ([docs/signal-files.md](docs/signal-files.md)): the desktop's save dialog or Android's document picker, the file named for its profile, written byte for byte as the app keeps it, so a file made in the form can be shared or given to `havne-cli --signals`.

### Changed

- In the form for a vehicle's own signals, a Request ID left empty keeps the one the module has while another of its signals reads it, and a typed one that differs moves the module's other signals too, the note naming them; before, the last signal saved set the module's request ID without a word.
- Each box in the form for a vehicle's own signals has its name above it (Byte, Bits, Start bit, and the rest), so a box still says what it holds once something is typed; the names were only placeholders before.
- On a phone, a signal its module refused reads "Refused (7F 22 31)" when the full words don't fit beside its name, and the name keeps its room; before, both were cut short.
- When the connected car's VIN picks another saved vehicle than the one picked, the Vehicle section says so ("The connected car's VIN picked Daily driver (Weekend car was picked)") until you pick one yourself; before, only the activity log said it.
- The viewer's **Replay view** is **Replay in Settings**: it fills Settings' field with the part in view for Connect, as the list's Replay does, and it's off while a session runs.
- Picking another vehicle, or New, puts **What to read** back to Default when it named the last vehicle's signal file or plugin, and so does the end of a session in which the car's VIN picked another vehicle; a vehicle the VIN adds keeps no signal file or plugin in its connection. So a connect never reads with another vehicle's signals and keeps them as this one's.
- For programs that use the core library: a channel's support report (`ecuio_channel_support_t`) lists in `dropped_list` the requests a negative reply refused before those its supported-PID bitmaps left out, each in set order; before, all were in set order, so on a module whose bitmaps leave out many PIDs the list filled before it reached a refusal.

### Fixed

- A plugin whose data files hold a number in its shortest 17-digit form, as Python and most tools write one (`1.0212934580499022e-07`), loads; it was refused, its variant "reading back as another set". Every number a signal file, a plugin, an OBDb import or a recording holds reads as the double nearest it, where a 17-digit one could land a unit in its last place off.
- On Windows, `havne-cli` takes a path with a character outside the system's code page, so a replay, a recording, `view`, `info`, a signal file, an OBDb import and a plugin under a folder named with an accent or in Greek work; such a path failed, or a character came through as `?`, which a URI reads as its query.
- The charts draw their traces on the GPU: on the test phone, a tap on the Recordings page's chart, given the page with every signal plotted, holds the app for about 50 ms instead of 150, and a drag moves the chart as it goes (about 36 frames in a 600 ms swipe) where it stood still until the drag ended.
- On Windows, a session that asks waits out the engine's 20 ms floor between two requests to a module on time, instead of at Windows' 15.6 ms timer step: against an adapter whose empty reads return at once, each module was asked about every 31 ms, now about every 21 ms, however many modules. A session through the OBDX Pro VX keeps its pace (about 27 ms), which its own held reads set.
- Importing a newer version of a plugin that couldn't be kept (its copy not written) left neither version loaded; the version loaded now stays loaded and kept until the new one has loaded in its place.

## [0.5.0] - 2026-11-06

### Added

- Linux downloads for 64-bit ARM, such as a Raspberry Pi 3 or later on the 64-bit Raspberry Pi OS 13 or Ubuntu 24.04 and newer: the app as an AppImage, and `havne-cli` and the core as a deb and a tarball, beside the x86_64 ones, with their checksums, build provenance and SBOM entries ([docs/linux.md](docs/linux.md)).
- `havne-cli sim "mock://sim?modules=2"` adds a second module on 0x7E1/0x7E9 with Service 22 DIDs of its own (D100, D101, D102, made up for the simulated car), so one session can ask two ECUs; the fault `pending:<n>` answers every Nth request `7F <service> 78` first and the reply 300 ms later, and the `sim_stats` and `end` lines count it as `pending` (`ecuio_sim_stats_t.pending`).
- The simulated ECU answers the Service 22 DIDs of a registered profile (`mock://sim?profile=<name>`): each signal is a field of its DID's reply, written from the value `ecuio_sim_set_value` gives it by inverting its scale and offset, on the engine ECU or on a module of the profile's own channels, so a set of DIDs is checked against a session with no car.
- Core decodes enumerations and computes signals from others ([ADR-0051](docs/adr/0051-signal-files.md)), the groundwork for signal files: an enum's value is its raw number with its label (`ECUIO_VALUE_ENUM`, `ecuio_enum_label`), which `havne-cli`'s values lines carry as `value` and `label`, and Live and the Recordings viewer show in place of the number; a computed signal (`ECUIO_DEF_FLAG_COMPUTED`) is a bounded program over up to four other signals, each no older than its own age bound or `ECUIO_VALUE_STALE_US` (3 s), and a division by zero gives no value.
- Core reads signal files, a user's own signals as JSON ([docs/signal-files.md](docs/signal-files.md), [ADR-0051](docs/adr/0051-signal-files.md)): Service 01 PIDs and Service 22 DIDs on up to four modules, as numbers, text, enumerations and signals computed from others, from the file's bytes (`ecuio_signal_file_load`), registered as a profile `user.<name>`. Everything a file gets wrong is refused when it's loaded, its line and column named: an unknown key, two widths of ID, a module with no request ID where the standard gives none, more requests than a session asks.
- Settings' Vehicle section imports your own signal files ([docs/signal-files.md](docs/signal-files.md)): **Import a signal file** adds one to the picked vehicle, keeping a copy named by its SHA-256, and each file is then a profile of its own under **What to read** (`?profile=user.<name>`). A file the loader refuses shows its line, column and why, never its path; one whose profile another file makes already is refused; **Duplicate** copies a vehicle's files, and **Remove**, **Forget** and **Forget all** delete the copies no vehicle keeps.
- On an adapter, a signal file that extends `obd2` takes obd2's place under **What to read**: its button reads **obd2 + <name>**, and importing one while the field reads Default chooses it, the note under the files saying so ([docs/signal-files.md](docs/signal-files.md)).
- `havne-cli signals check <file>` says what a signal file holds, in the file's own terms, and each unit that never converts, or why the file is refused, with the loader's line and column; `havne-cli stream <uri> --signals <file>` and `sim <uri> --signals <file>` load one and read through its profile, added to a URI that names none. The `signals` line carries each enumeration's labels and marks each computed signal.
- Settings' Vehicle section writes a vehicle's own signals ([docs/signal-files.md](docs/signal-files.md)): a form for one signal at a time (the module, the PID or DID, the field, the scaling, an enumeration's values, the group) that **Save** adds to the vehicle's own file and profile (`user.<vehicle>-own`), **Edit** and **Remove** for each one saved, and **Read once**, which asks the adapter for that signal once while no session is connected and shows the reply's bytes in hex beside the value it decodes to, or the ECU's refusal by name. Core's loader checks the signal as it checks a file, in the same words.
- Core reads plugins' data tier ([ADR-0052](docs/adr/0052-plugin-framework.md)): a package of signal files with one manifest naming the plugin, the cars it's for and its ECUs, as a folder or packed into one file (`ecuio_plugin_load`). Each variant, an ordered list of the package's files merged by signal key, registers as the profile `plugin.<id>.<variant>`; a car's VIN proposes the variants its manufacturer and model year fall in, and the user picks. A package is refused whole, the file, line and column named. The repository's example plugin, `plugins/example-sim/`, is for the simulated car. `ecuio_signal_file_extends` says which set a signal file extends. A variant is one signal file too (`ecuio_plugin_variant_file`): its files' signals merged and written as one, which a recording copies as `signals.json` with a `plugin` line naming the plugin, its version and the variant.
- `havne-cli plugins list`, `plugins check` and `plugins match <pkg> --vin <VIN>` say what a plugin holds, check it as a session would read it, and list the variants a VIN proposes; `stream --plugin` and `sim --plugin` read the variant the URI's `profile=` names, and `stream --plugin --record` keeps that variant's file in the recording. On Linux, `plugins check --on <vcan>` runs a plugin's value fixtures through the simulated ECU and decodes each back, within half its field's step, and CI checks every plugin in the repository that way; core reads a plugin's fixture files for it (`ecuio_plugin_values`) ([ADR-0052](docs/adr/0052-plugin-framework.md)).
- `havne-cli signals import obdb <file> --out <file>` converts an OBDb signal set on disk to a signal file ([docs/signal-files.md](docs/signal-files.md), `ecuio_obdb_import`): Service 01 and 22 on 11-bit IDs, numbers, enumerations and ratios, the commands a model year takes with `--year`, each field that differs from OBDb's reading and each signal left out reported with why, the file's `source` naming OBDb's contributors and CC BY-SA 4.0. Havne fetches nothing and ships no OBDb data.
- A recording of a session a signal file decodes holds a copy of the file (`signals.json`) with its SHA-256 on `events.log`'s header, from the app (the vehicle's file) and `havne-cli stream --record --signals`; a replay with that copy decodes the same values, `ecuio_recording_get_signals` says whether the copy is still the bytes named, and `havne-cli view` and `info` show it on the `opened` line as `signal_file`.
- The simulated car (`mock://`) reads its intake manifold and barometric pressure, and Live shows its boost, computed in core as MAP minus baro (a vacuum, never above zero, on its naturally aspirated engine). Its frames are 16 bytes of CAN FD.
- Settings' Vehicle section imports plugins ([plugins/README.md](plugins/README.md), [ADR-0052](docs/adr/0052-plugin-framework.md)): **Import a plugin** takes a packed plugin (`<id>.havne-plugin.json`) and keeps a copy, every variant becomes a profile (`?profile=plugin.<id>.<variant>`), and with a vehicle picked, the variants a plugin proposes for the car's VIN (the connected car's, or one typed in the VIN lookup) are buttons, to pick once no session is connected: the one picked is kept with the vehicle and offered under **What to read**, and pressed again it's undone. A plugin's **Remove** takes its variants off every vehicle. An import of an id already there replaces it; neither that nor **Remove** goes ahead while a session decodes with it.
- Recordings are format 3: an enum signal's raw number goes in `signals.csv` and its labels in `events.log`'s header, which the reader gives back (`ecuio_recording_get_labels`, `ecuio_recording_enum_label`) and `havne-cli view` and `info` show on the signal's line as `"labels"`; recordings in formats 1 and 2 still open as before.

### Changed

- **C ABI break:** the signal set's layout is 4 (`ECUIO_SIGNAL_SET_LAYOUT`): the set gains its enum and computed tables, and a def's reserved room gives `table_first` and `table_count`; a set built against an earlier layout is refused until it's rebuilt. A set may derive up to 256 requests (`ECUIO_PROFILE_MAX_REQUESTS`, 128 before), and `uds-generic` polls up to 256 DIDs.

## [0.4.1] - 2026-11-05

### Added

- In developer mode, a corner badge counts the frames the window draws each second.
- A pinch on Live's chart, or the mouse wheel over it, steps its window: fingers spreading show less time, closing show more.
- Live's chart and the Recordings viewer's take the page at a press of **Chart**, a double-click or `F`, and give it back the same way; on a phone on its side they start that way. The viewer then shows the values at the cursor once you tap one, beside its chart, or below it when the page is taller than wide (a phone upright); until then the chart has the page, and a button folds those values away and opens them again, kept between runs. A handle below Live's list, and one below the viewer's chart, sets how much of the page each takes, kept between runs.
- The Log page's frame table copies the rows you pick: a tap selects a row (a shift-click a run of them), and **Copy** or `Ctrl+C` (`Cmd+C` on a Mac) puts them on the clipboard as tab-separated lines with a header, ready for notes or a spreadsheet. The selection stays on its frames while new ones arrive.
- With `ECUIO_SUR_LOG_FRAMES=1`, the J2534 session log gives each driver read that brought messages its own line: the timeout asked, the messages asked for and returned, the status, and how long the driver held it ([docs/j2534.md](docs/j2534.md)).

### Changed

- On a phone, upright or on its side, and in a window as small, pages are denser: smaller margins, spacing, buttons, toolbar and sidebar, and shorter rows on Live, so more of each page fits at 100 %.
- On a phone upright, the Dashboard's tachometer and speedometer sit side by side, so the whole Dashboard fits the screen; they stack only on a narrower page.
- Paused, the Log page's frame table says where new frames still go: the recording keeps every one when Record is on; otherwise the table leaves them out. Live and the Dashboard carry on either way.
- An ELM327 session that only listens (`?profile=raw`) needs no flow control, so it turns the adapter's off (`AT CFC0`). Through an OBDLink MX+ it still hears only replies that fit in one frame: the adapter's monitor misses the rest of a longer one.

### Fixed

- Live's chart no longer shimmers as it scrolls, nor the Recordings viewer's as you pan it: a noisy trace's peaks stay where they are instead of shifting at every update.
- On a phone upright, Live's **120 s** button no longer wraps onto two lines.
- Through a J2534 adapter, replies reach the session as they arrive: a reply that took longer than 10 ms no longer comes about 50 ms late on the OBDX Pro VX, and the others no longer wait out the rest of a 10 ms read, so requests go out sooner.
- Through a J2534 adapter, a read that fails right after frames were lost reports the failure at once, not one read later, and messages a driver returns beyond those asked for count as lost, so the next frame carries the overflow flag.
- The Recordings viewer's chart zooms a notch's worth per notch of a high-resolution mouse wheel or a touchpad, which send a notch in parts, instead of a notch per part, and a sideways scroll no longer zooms it out.
- On a phone, a double-tap on Live's or the viewer's chart no longer misses when the two taps land a little apart, nor when the viewer's chart has the page and takes a moment to draw.
- A tap on the Recordings viewer's chart places the cursor on the nearest sample of the first signal plotted, and a dot on each trace marks the sample its value comes from, so the value read always matches the line. Between two samples it used to read the earlier sample's value where the line had moved on.

## [0.4.0] - 2026-11-04

### Added

- The Recordings page: the recordings sessions saved, newest first, each opening in a viewer. It shows the recording's signals and a chart of the part in view: drag it sideways, zoom with the wheel or a pinch, and tap for the values at that moment; on a phone the page scrolls, upright or on its side. Its markers and what a check of its frames found are on the chart's time axis, and each finding listed shows its place. Replay still puts a recording's `replay://` URI in Settings' field.
- A stretch of a recording replayed: `replay://<recording>?from=<s>&to=<s>` plays the part between those seconds, read off the viewer, `havne-cli view` or `havne-cli info`, as a session of its own, and `havne-cli sim` answers from that part alone. The viewer's **Replay view** puts the part in view in Settings' field.
- `havne-cli sim --desk-bus` answers on a real CAN controller too, on a desk bus with no car, so a tester reads the simulated ECU over real wire; the controller must be up, not listen-only and error-active, with or without one-shot mode. `ecuio_sim_create_with` and `ECUIO_SIM_ALLOW_DESK_BUS` in `ecuio/sim.h`. On any bus the simulated ECU goes silent for good once another node sends on its reply ID, 0x7E8 (`ECUIO_SIM_YIELDED`), and `havne-cli sim` ends with `yielded`.
- `havne-cli devices` and Settings show each CAN interface's controller state, the modes it's in and, from Linux 6.0, the modes its driver offers, in `ip link`'s words; `ecuio_can_interface_t` carries them, named by `ecuio_can_state_name` and `ecuio_can_mode_name`. Settings' CAN rows say which mode a session needs, listen-only to listen and one-shot to send requests, and never suggest one the driver doesn't offer.
- Two recordings overlaid on the Recordings page: another recording's signals drawn dashed beside the first's, on the same scales, lined up on their starts, their wall-clock starts, a marker, or a signal rising or falling through a level typed in the units on screen.
- Finding a signal among many: Live's list and the Recordings page's signals filter by words in the name or unit, by **Plotted only**, and by the part of the car each signal reads (Engine, Air, Fuel, Oxygen sensors, Emissions, Vehicle, Electrical, Diagnostics). The filter sits beside Live's title and above the viewer's signals, and on both a line counts the signals shown and plotted.
- Each signal says which part of the car it reads: a `group` in `ecuio_signal_def_t` with `ecuio_signal_group_name`, the last field of a recording's `signal` lines, and `group` in `havne-cli`'s `signals` and `signal` lines.
- **Mark** in the header, while a session records, puts a marker in the recording ("Mark 1", "Mark 2", and so on), as a line typed into `havne-cli stream --record` does.
- `havne-cli stream --record <folder>` records the session into a folder of its own under `<folder>`, named for when it started, with the files the app writes (`frames.csv`, `signals.csv` and `events.log`; `markers.csv` and `latency.csv` once they have a row). `--label <name>` adds a name to the folder's. Each line typed while it runs is a marker. The recording is written on a thread of its own, so a slow disk never holds the session up; a write that fails says why in a `recording` line, and the stream exits 1.
- `ecuio/recording.h` in core's C API: a recorder opened on a folder, attached to a session before it starts, with notes, markers and its state.
- `havne-cli view <recording>...` reads recordings offline: what each holds, then each signal's lowest, highest and count over time in columns (`--columns`, `--from`, `--to`), and with `--at` the values at a moment. `--align-on` puts several on one axis, at a signal rising or falling through a level, a marker, or their wall-clock starts.
- `havne-cli info <recording>` checks a recording's frames offline: what it holds, then each stretch with no frame at all (over `--gap` seconds, 1), each ID whose frames slowed past `--rate-drop` times their usual interval (10) and by a tenth of a second at least (a reply's frames as they crossed the bus counted as one message), each ID first seen over `--settle` seconds in (2), each overflow, broken ISO-TP sequence, link change and sleep, then a summary. A folder holding only `frames.csv` is still checked. Its `signal` lines, and `view`'s, carry each signal's rate.
- `ecuio_recording_check` in `ecuio/recording.h`: one pass over a recording's frames, each finding passed to a callback as it's settled, with a summary of the frames, the IDs and each kind's count.
- A reader in `ecuio/recording.h`: `ecuio_recording_open` reads a recording's folder once, with a progress callback that can cancel it (`ECUIO_ERR_CANCELED`), then answers a time range's lowest and highest per column, the value in force at a time, and the first time a signal crosses a level or a marker was set. An hour of recording opens in a fraction of a second.

### Changed

- A connect or an open refused for what it was given (a J2534 driver, a port, a CAN interface, a recording) reads "couldn't open what was named" before its reason, where it read "couldn't load what the URI names": `havne-cli view` and `info` take no URI.
- A SocketCAN connect through a controller whose driver offers no one-shot mode (or, to listen, no listen-only mode) is refused saying so, instead of suggesting an `ip link` command that can't work; a refusal names a mode's command only where the driver offers it, and the one for a listen-only controller says `listen-only off`, which `ip link` needs.
- The recent recordings moved from Settings to the Recordings page, whose tab reads Recs on a phone.
- The app's recordings are written by core, on a thread of their own, as `havne-cli`'s are: a recording holds every frame the session read, also the ones the Log page skips under load, and its last note says how many it skipped. A write that fails, a full disk among them, stops the recording with `Recording stopped: <reason>` while the session runs on.
- A recording's `events.log` has a new layout: a format line, the app's details under `caller` lines, a kind word on every line (`note`, `link`, `asleep`, `drop`, `end`), and a summary of anything the recording dropped.
- A frame carrying a whole ISO-TP message, as a requesting session's replies and an ELM327 adapter's monitor deliver them, is flagged `0x100` (`ECUIO_FRAME_FLAG_REASSEMBLED`), and the Log page's and `havne-cli`'s `frame` lines show it.
- A signal whose name or unit starts with `=`, `+`, `-`, `@`, a tab or a carriage return is refused when its profile is registered (validator code 18): a spreadsheet opening the recording would run it as a formula.
- A J2534 adapter that refuses to open without saying why gets an error that asks whether the adapter is attached and not open in another program, the two usual causes, instead of naming the status alone.
- A replay of a recording whose last row was cut short, as a crash or a power loss leaves it, plays the rest instead of refusing the recording, and says so: the Session line adds ", its last row cut short and left out", and `havne-cli`'s `session` line counts it in `rows_torn`. The simulated ECU loading such a recording leaves the row out too.

### Fixed

- A SocketCAN session that sends requests no longer drops a long reply, a VIN among them, when the computer is busy: the kernel's times of the session's own frames, and of frames read slightly out of order, no longer put a stall into the reply's timing.
- A SocketCAN session that sends requests counts the bus rate as proven only once its first request is answered, or 25 ms pass after the controller echoed it with no error frame. An adapter can echo a frame no node acknowledged and report the missing acknowledgement after, so a wrong rate could pass for the right one.

## [0.3.6] - 2026-11-02

### Added

- A simulated ECU on Linux's virtual CAN bus: `havne-cli sim mock://sim --on vcan0` answers OBD-II requests as the simulated car's engine ECU, so `havne-cli stream "socketcan://vcan0?profile=obd2"` or the app reads its values, identification and trouble codes with no hardware. `?values=canned`, `?vin=` and `?faults=` change what it answers. `havne-cli sim replay://<recording> --on vcan0` answers as the car in a recording did: its own supported PIDs, its values as they changed, and the identification and trouble codes it gave when the recording holds them.
- `ecuio/sim.h` in core's C API: `ecuio_sim_create`, `ecuio_sim_get_stats` and `ecuio_sim_destroy`.
- Inspection readiness: the Diagnostics page says whether the car would pass the federal OBD inspection rule (40 CFR 85.2222) today, from every ECU's monitors and check-engine light and the model year in its VIN, and names the monitors still incomplete. `havne-cli stream` writes the same as an `inspection` line. A state's program may be stricter.
- `ecuio_diag_inspect` and `ecuio_inspection_t` in core's C API.
- Trouble codes with their names: the Diagnostics page shows SAE J2012's title beside each generic powertrain and network code ("P0301  Cylinder 1 Misfire Detected") and the freeze frame's, and **Look up a code** names a typed one, offline. `havne-cli`'s `diagnostics` line carries the same as `titles`, and `havne-cli dtc <code>` looks one up. The titles come from Wal33D's dtc-database under its MIT license, whose notice ships with every download.
- `ecuio_dtc_parse`, `ecuio_dtc_title`, `ecuio_dtc_title_source` and `ecuio_dtc_title_notice` in core's C API.
- The VIN decoded on the device, with nothing sent anywhere: the model year, the make and whether the check digit matches, by 49 CFR 565 and the manufacturer codes in NHTSA's vPIC database. The activity log adds a `VIN:` line for the connected car, and Settings' Vehicle section has **Look up a VIN** for a typed one, never saved. `havne-cli vin <VIN>` does the same, and `havne-cli stream` writes an `identity` line per ECU: each identification item and the VIN's decode.
- `ecuio/vin.h` in core's C API: `ecuio_vin_decode`, `ecuio_vin_make` and `ecuio_vin_source`.
- The Diagnostics page shows each readiness monitor for this drive cycle beside its state since the last clear: done, not yet, or disabled for the rest of the drive. `havne-cli`'s `diagnostics` line carries the same, with PID 41's own outcome.
- A channel's diagnostics report in core's C API holds this drive cycle's monitors (Service 01 PID 41: `ECUIO_MONITOR_CYCLE_ENABLED`, `ECUIO_MONITOR_CYCLE_COMPLETE`, `ECUIO_DIAG_CYCLE`, the `cycle` outcome), and a new outcome, `ECUIO_DIAG_UNSUPPORTED`, for an item whose PID the ECU says it lacks, which is then not asked.
- A faster DID scan: `?scan=first` under `profile=uds-generic` asks a `did=` range back to back before polling what it found, and over an ELM327 each scanned DID is asked so the adapter returns as soon as a one-line answer is in. `havne-cli stream` writes a `scan` line with the scan's progress, and `--until-scanned` ends the stream once every range is asked.
- The phone relays a paired Bluetooth adapter to a computer on its USB cable, so `havne-cli` on the computer reaches an adapter only the phone can: in developer mode, select the adapter in Settings and press **Relay**, then on the computer `adb forward tcp:35000 tcp:35000` and `elm327://127.0.0.1:35000`. The relay listens on the phone alone, never on a network, takes one computer at a time and keeps running with the screen off.
- `ecuio_session_clear_diagnostics_with` in core's C API: a clear with the caller's word on a running engine and on a moving vehicle. A channel's diagnostics report records the engine and vehicle speeds a clear read (`clear_kmh`, `ECUIO_DIAG_CLEAR_RPM`, `ECUIO_DIAG_CLEAR_KMH`) and two outcomes, moving and no vehicle speed.

### Changed

- Clearing trouble codes no longer needs the engine off. The confirmation says a clear works best with the engine off and the ignition on; the clear then goes out with the engine running, and if an ECU refuses it running, its card and the activity log say so. If the vehicle is moving, the Diagnostics page asks once more, naming the speed, before clearing.
- When no ECU has codes stored, the clear's confirmation says so: a clear then only sets the readiness monitors back to incomplete and resets any values the manufacturer chose.

### Fixed

- Through a J2534 adapter on Windows, the driver process's log of a session that ran while another held the usual log's name was never deleted, so the temporary folder grew on a computer that ran two sessions at once; those logs are kept to the newest 50, as the others are.
- Through a J2534 adapter on Windows, values refresh about a third more often: a reply was seen up to 15.6 ms after it arrived, Windows' default timer step, and is now seen within the 10 ms the read waits. On the test car through the VX, 28 % more replies arrived in the same time.
- After an ignition cycle with the app connected, values refreshed at about a third of their rate for the rest of the session: the requests that went unanswered while the engine was off ended asking several PIDs per request. Asking several resumes once the car answers again.
- Over an ELM327 adapter, the first reading of a PID whose reply spans two CAN frames (such as PID 68, intake air temperature) was lost and counted as a timeout: the request asked the adapter for one frame. It now asks for the whole reply from the start.
- On a phone, Live and the Log's frame table ran off the right edge of the screen; they now fit its width, and the frame table wraps a long payload under its row.
- On a phone, the confirmation for clearing codes could run past the bottom of the screen with no way to reach Clear or Cancel; it now scrolls with the page.
- On a phone, tapping Disconnect while an adapter's replies were still arriving over Bluetooth could crash the app (once in eight disconnects on a car). The closed link's socket now stays until Android has stopped reading it (20 disconnects on a car, none crashed).
- A request sent to every ECU at once (`?txid=0x7DF`) now gets a reply longer than one CAN frame, such as the VIN, through a J2534 adapter or SocketCAN: the reply's flow control went to the functional ID, where an ECU doesn't listen for it, and goes to that ECU's own request ID.
- `havne-cli` ends a connect that hasn't finished within 60 s, with an `error` line and exit code 3, instead of waiting on it; an antivirus holding a newly built driver process at its first run held one.
- A DID scan through a J2534 adapter could stop one DID short and never finish, so `havne-cli stream --until-scanned` ran until `--seconds`: a long reply that came after its request's wait was dropped. A scanned DID's late reply now counts it found.

## [0.3.5] - 2026-11-01

### Added
- Each CI run builds an unsigned iPhone app, `havne-ios-unsigned.ipa`,
  that a tester installs with a sideloading tool and their own Apple ID
  ([docs/ios.md](docs/ios.md)). On iOS the app asks before reaching a
  WiFi adapter on the local network, and its recordings show in the
  Files app.
- **Trouble codes, readiness and the freeze frame**
  ([ADR-0037](docs/adr/0037-reading-trouble-codes.md)): a session that
  asks the car (`obd2` or `uds-generic`, through a J2534 or ELM327
  adapter) reads each ECU's check-engine light, its stored, pending and
  permanent trouble codes, its readiness monitors and the freeze frame
  stored with a code, once at connect, taking turns with the live
  values. `?diagnose=0` leaves them out.
- **A Diagnostics page** (after Live in the sidebar, "Codes" among a
  narrow window's tabs) shows each ECU's light, its codes by list,
  which monitors are complete and the freeze frame's values in the
  chosen units; **Read again** asks once more. The last read stays on
  the page after a disconnect.
- The activity log gets one line per ECU per read
  (`ECU 0x7E8 diagnostics: MIL on; stored P0301 …`), so a recording's
  `events.log` keeps the codes as they were.
- The simulated car (`mock://`) has an engine ECU with trouble codes, a
  readiness report and a freeze frame, so the page and the log line
  show with no hardware (`?diagnose=0` leaves them out).
- `havne-cli stream` writes a `diagnostics` line per ECU when its read
  completes: the light, each list's codes, the monitors and the freeze
  frame's values, with a refusal's reply bytes.
- **Clearing trouble codes**
  ([ADR-0038](docs/adr/0038-clearing-trouble-codes.md)): **Clear
  codes...** on the Diagnostics page asks first, saying what a clear
  erases (the stored and pending codes, the freeze frame, the
  check-engine light, the readiness monitors' results) and what it
  keeps (permanent codes). The report is saved to
  `diagnostics-<date-time>.jsonl` in the recordings folder before
  anything is sent, and nothing is sent if it can't be. Each ECU is
  asked its engine speed first and cleared only with the engine
  stopped, then read again; the page and the activity log say what
  became of each (`ECU 0x7E8 clear: cleared`, `not sent: engine
  running (812 rpm)`, ...). Not yet run on a car.
- **Every standard Service 01 reading**
  ([ADR-0039](docs/adr/0039-full-service01-coverage.md)): the `obd2`
  profile reads every linear field of SAE J1979's Service 01 table,
  130 signals in all (both banks' fuel trims, every oxygen sensor, the
  wide-range sensors' lambda and current, fuel pressure, catalyst
  temperatures, torque, run times and distances among them), and asks
  each car only for the ones it has. The fast-changing readings (rpm,
  speed, load, MAF, throttle, fuel pressure, lambda) are asked twice
  for each slow one (temperatures, pressures, counters).
- Live lists only the signals the car's ECU supports and sends, with a
  line saying how many it left out; the Dashboard and recordings keep
  every signal. A field the ECU's replies come without (bank 3 of a
  two-bank engine's oxygen-sensor trims) is left out too.
- Live plots six signals when a session starts, not every one: the
  first six the car has of a ranked list (rpm, coolant, intake air and
  oil temperatures, MAF, speed, load, timing advance, the fuel trims,
  then MAP, voltage, the pedal and commanded lambda), so a car without
  one plots the next. A tick or untick is kept for the picked vehicle;
  the mock and `uds-generic` still plot every signal. Each signal def
  carries its place as `plot_rank` (core's C API).
- Live's chart shows the last 10, 30, 60 or 120 s, picked above it and
  kept between runs: every trace covers that span, however fast its
  signal refreshes.
- With the probe on, a Service 01 request carries up to six PIDs of
  one rate class, each one the car has already answered alone, so a
  pass asks the same signals in fewer requests; `?pack=0` asks one PID
  per request. A car that answers such requests with silence, a refusal
  or one PID three times in a row is asked one PID per request for the
  session, and its support report says so, as does the activity log's
  ECU line. On the test car through the MX+, each signal refreshed four
  to five times as often.
- Core decodes a Service 01 reply that carries several PIDs (as
  J1979 allows, and as other tools' recordings may hold) record by
  record, using each PID's length as the car sends it;
  `ecuio_session_get_unsplit_replies` counts the ones that wouldn't
  split.
- `ecuio_session_get_absent_signals` (core's C API) lists the signals
  a session's replies come without, and `havne-cli stream --values`
  writes them as an `absent` line.
- Core's C API makes three more judgments for a front end:
  `ecuio_session_get_unsupported_signals` lists the signals whose PID
  the ECU doesn't support, `ecuio_identity_vin` picks the VIN from an
  identification report, and each channel's support report counts its
  supported PIDs (`pids_supported`) and the highest PID its bitmaps
  answered for (`pids_through`).
- Imperial units show Pa as inH2O, L/h as gal/h and `N·m` as `lb·ft`.
- **Requests on SocketCAN**
  ([ADR-0040](docs/adr/0040-socketcan-requests-isotp.md)): on Linux,
  `obd2` and `uds-generic` run on a native CAN interface
  (`socketcan://can0?profile=obd2&baud=500000`), with ISO-TP done in
  core and the same query keys as `j2534://` and `elm327://`. Havne
  sends only through a controller brought up to stop at once on a
  fault (normal mode at the rate `?baud=` states, `restart-ms 0`,
  `one-shot on`, no errors counted), and after a CAN error on its first
  request it sends nothing more; each refusal gives the `ip link`
  commands that fix it. ISO-TP's timeouts run on the kernel's receive
  times, so a reply the bus carried on time still counts when the
  computer is slow to read it. Run on Linux's virtual CAN bus; not yet
  through a real controller or on a car.
- An ELM327's replies reassemble through the same ISO-TP code.
- **Fault injection on a replay**
  ([ADR-0041](docs/adr/0041-replay-fault-injection.md)):
  `replay://<recording>?faults=drop:5,overflow:1,corrupt:1,truncate:2,stall:1:500`
  plays a recording with frames dropped, lost to an overflow (the next
  frame flagged), corrupted (one bit flipped), cut short or held back,
  each at its chance per frame, the same each run from `?seed=`. The
  activity log says a replay has faults and what they did when it
  finishes, `havne-cli stream` writes the counts in its `end` line, and
  `ecuio_session_get_replay_faults` reads them.
- **Reply timing per request**
  ([ADR-0042](docs/adr/0042-reply-timing.md)): a requesting session
  measures each PID's (or DID's) answers, refusals and timeouts, its
  reply latency and its refresh interval at P50, P99 and P99.9, the
  interval on the adapter's, the kernel's or the host's clock, as the
  output says. A recording of the session holds them in `latency.csv`,
  added to every 10 s, and `havne-cli stream` writes them as `timing`
  lines; the activity log sums them up at the disconnect, with ISO-TP's
  counts on SocketCAN and ELM327. `ecuio_session_get_timing` reads them.

### Changed
- The simulated car starts each loop with the key on and the engine
  stopped for 15 s, then a start, so a clear can be shown with no
  hardware; `mock://`'s first frames read 0 rpm.
- The Log page's **Activity | Frames** switch (developer mode) sits
  beside the title and stays in place when the view changes, so it no
  longer moves out from under the pointer as the buttons beside it
  change.
- A release's page opens with a table linking each platform's
  download, and one build provenance file,
  `havne-X.Y.Z.intoto.jsonl`, covers every download instead of one
  beside each, so the list of files is shorter.

### Fixed
- A replay's end is judged by the rows read and the frames lost to
  faults, so the activity log's "Replay finished" line and `havne-cli
  stream`'s end come for a replay whose faults dropped frames.
- Live's time axis no longer draws its captions over one another on a
  narrow window, such as a phone's: the step between ticks grows until
  each caption fits.

## [0.3.4] - 2026-10-30

### Added
- **`havne-cli`**, a command-line client
  ([ADR-0031](docs/adr/0031-command-line-client.md)):
  `havne-cli stream <uri>` opens any session the app can (`mock://`,
  `replay://`, `elm327://`, `j2534://` on Windows) and writes its
  frames to standard output as JSON lines, one object per line, for
  `jq`, scripts and logging without a desktop; `--values` adds the
  values the profile decodes. It stops at Ctrl+C, after `--count`
  frames or `--seconds`, at a replay's last frame, or when a replay
  stops early because its recording changed, and always ends with an
  `end` line. `havne-cli devices` lists the serial ports and
  J2534 adapters a URI can name. On Windows it ships beside
  `havne.exe` in the zip.
- **SocketCAN, listening, on Linux**
  ([ADR-0032](docs/adr/0032-socketcan-listening.md)):
  `socketcan://can0` reads a native CAN interface (a candleLight or
  CANable adapter, PEAK, Kvaser, a Raspberry Pi's CAN hat) and sends
  nothing: every frame on the bus, or the IDs `?filter=` names, CAN FD
  and error frames included (an error frame is reported, never decoded
  as values), timed by the kernel. Bring the interface
  up first, at the bus's bit rate and in listen-only mode
  (`sudo ip link set can0 up type can bitrate 500000 listen-only on`);
  Havne refuses one that could transmit and says how to fix it.
  Settings and `havne-cli devices` list the CAN interfaces. Requesting
  profiles (`obd2`, `uds-generic`) don't run on SocketCAN yet.
- **A simulated car on a virtual CAN bus:** `havne-cli play mock://demo
  --to vcan0` puts the simulated car's frames on Linux's `vcan0`, where
  `socketcan://vcan0?profile=mock` (or any SocketCAN tool) can listen,
  with no hardware. It writes only to virtual interfaces, never to a
  real bus.
- **A sleep says so:** when the computer or phone sleeps during a
  session, the activity log (and a recording's `events.log`) says
  "Asleep for N s: nothing was read meanwhile", and `havne-cli` writes
  a `slept` line.
- **Debug symbols with each release**
  ([ADR-0034](docs/adr/0034-debug-symbols-and-probes.md)):
  `havne-X.Y.Z-windows-x64-symbols.zip` holds the PDBs of `havne.exe`,
  `havne-cli.exe` and both J2534 surrogates, checksummed and attested
  like the zip, so a crash dump or a profile reads against the source.
  `docs/development.md` ("Debug symbols and tracing") says how to keep a
  dump and open it.
- **USDT probes on Linux:** core marks a session's start and close and
  each frame (`ecuio:session_start`, `session_close`, `frame`), so
  `bpftrace` and `perf` can count and time the frame path of a running
  Havne or `havne-cli` with no debug build. Unused, a probe costs one
  `nop`.
- **Linux downloads** ([ADR-0035](docs/adr/0035-linux-downloads-sbom-reproducible.md)):
  each release carries the app as an AppImage, and core alone as
  `havne-core_X.Y.Z_amd64.deb` and a tarball: `havne-cli`, the static
  library and its C headers, with a CMake package (`find_package(ecuio)`)
  and a pkg-config file (`ecuio`) for your own programs. Built on
  Ubuntu 24.04, they run on distributions with glibc 2.39 or later.
  `havne-cli`'s debug info ships beside them, as `havne-core-dbgsym`
  and a `-debug` tarball, found by `gdb` and `perf` by build ID.
- **An SBOM with each release:** `havne-X.Y.Z.spdx.json` (SPDX 2.3)
  lists what each download contains, Qt and the other libraries it
  bundles at their versions, and is attested like the downloads.
- **Reproducible Linux downloads:** the deb and tarball build to the
  same bytes from the same source, wherever they're built, which CI
  checks on every change; the AppImage does too, built on the same
  Ubuntu packages.

### Removed
- The build option `QTTUNE_BUILD_CORE_ONLY`, the name before the
  rename: use `HAVNE_BUILD_CORE_ONLY`.

### Fixed
- **A sleep on Linux or Android is a gap,** on Live, in a recording's
  times and in `havne-cli`'s stream, instead of the values on either
  side being joined as if nothing had been missed; values read before
  the sleep dim as stale at the wake
  ([ADR-0033](docs/adr/0033-session-time-counts-sleep.md)). Windows,
  macOS and iOS already counted a sleep.
- **A replay of a recording cut short after it was opened** says
  "Replay stopped" with the frames it sent, as one whose rows turned
  bad does, instead of going quiet as if finished. Rows added to a
  recording while it plays (one still being written) wait for the next
  replay, so its frame count and length stay as shown at the start.

## [0.3.3] - 2026-10-29

### Added
- An app icon on Windows, Android and iOS: the Home page's tachometer
  on the app's purple. On Windows it's `havne.exe`'s icon, in Explorer,
  the taskbar and the title bar; on Android it's an adaptive icon,
  shaped to fit whichever icon style the launcher uses.
- **ELM327 adapters** ([ADR-0026](docs/adr/0026-elm327-transport.md)):
  `elm327://<port>` reads a car through an ELM327-compatible adapter,
  such as an OBDLink or an OBDX Pro in its ELM mode, on a serial port:
  USB, or Bluetooth paired as a port (`COM5` on Windows, a `/dev/` path
  on macOS and Linux). It takes the `obd2` profile by default and the
  same query keys as `j2534://`, plus `?serial=` for a wired adapter's
  rate. A dropped Bluetooth link recovers on its own. Settings lists
  the machine's serial ports beside the J2534 interfaces, without
  opening any, and the activity log's "Session:" line names the
  adapter's firmware and the battery voltage. Each connect writes a
  wire log to the temporary folder (`ecuio-elm327.<date-time>.log`):
  every command and what the adapter answered, and, while recording,
  every request and reply with its timing. It has read a car through
  an OBDLink MX+ over Bluetooth and an OBDX Pro VX over USB.
- `elm327://` reaches a WiFi adapter by its address and port, for
  example `elm327://192.168.4.1:23` (an OBDX Pro) or
  `elm327://192.168.0.10:35000` (most clones), once the computer has
  joined the adapter's network. The address is typed: Havne doesn't
  search the network. A connection that drops is tried again on its
  own, as a serial port is.
- An `elm327://` session that asks nothing (`?profile=raw` without
  `?poll=`) listens: the adapter monitors the bus for the engine ECU's
  messages (or `?filter=`'s ID), silently, so another tool's
  conversation can be logged and recorded.
- When an adapter's buffer overflows and frames are lost (an ELM327's
  `BUFFER FULL`, a J2534 driver's `ERR_BUFFER_OVERFLOW`), the next frame
  says so in the recording's flags, the activity log says so once, and
  the Log page's frame view counts them.
- Settings remembers the last URI that connected, with its profile and
  DIDs, and the Record sessions switch between runs. A URI that failed
  to connect isn't kept.
- Settings' adapter list shows each device's firmware versions as last
  seen at a connect ("last seen: ..."), without opening the adapter. A
  serial port shows whatever adapter answered on it last.
- Vehicles ([ADR-0027](docs/adr/0027-vehicle-profiles.md)). Settings'
  Vehicle row keeps named vehicles: pick one before connecting and it
  fills in that car's adapter, profile and DIDs, restores the signals
  plotted on Live, and names the session's recording folder after it.
  The VIN recognizes a saved car on the next connect, and an unknown
  one is saved as "Vehicle N". Only a salted hash of the VIN is stored.
  New, Duplicate, Rename, Forget the VIN, Forget and Forget all are
  buttons there, so nothing needs a file edited.
- Developer mode, a switch in Settings that's remembered between runs.
  It adds the `mock://demo` preset and the Log page's raw frame table,
  with its delivery counters; with it off, the app offers only real
  adapters.
- On Android (12 or later), Settings lists the phone's paired
  Bluetooth adapters, and a **Scan for BLE** button finds BLE ones.
  Selecting one puts `elm327://bt:<address>` or `elm327://ble:<address>`
  in the field, and the session runs over the phone's Bluetooth as it
  would over a serial port, reconnecting on its own when the link
  drops. Pairing stays in Android's Bluetooth settings, and Havne asks
  for the Nearby devices permission when the adapter list first opens.
  It has read a car through an OBDLink MX+.
- On Android, an adapter session keeps running in the background and
  with the screen off, recording included. While it's connected a
  notification says so ("Connected to ...", "· recording"), with a
  small gauge in the status bar and no sound; a tap opens the app. Havne asks once, at the first adapter connect, to
  show notifications, and the phone stays awake (not its screen) for
  as long as the session lasts. A dropped classic Bluetooth link
  reconnects with one connection attempt at a time. It has held an
  OBDLink MX+ on a car through the Home screen and the screen off. A
  `mock://` or `replay://` session may pause while the app is in the
  background.
- **An Android download** beside the Windows one: each release carries
  `havne-<version>-android-arm64-v8a.apk`, for Android 12 or later,
  signed with Havne's release key
  ([ADR-0030](docs/adr/0030-signed-android-releases.md)), so each
  release installs over the one before and keeps the app's data. The
  notes give the key's certificate fingerprint, and the APK has its
  own build provenance, as the zip does.
- Settings lists the recent recordings, newest first. Selecting one
  puts its `replay://` address in the field, with the profile and DIDs
  it was recorded with, so a recording replays without typing its path
  (on a phone, the only way).

### Changed
- **QtTune is now Havne**: the program is `havne.exe`, the release
  zip `havne-<version>-windows-x64.zip`, and the repository
  `erickramer42/havne` (the old address redirects). The first
  launch copies QtTune's preferences, vehicles included, and
  Settings' recent recordings list the sessions in
  `Documents\QtTune\recordings` beside the new ones in
  `Documents\Havne\recordings`; neither is moved or deleted. On
  Android Havne is a new app (`io.github.erickramer42.havne`) that
  starts empty. The build option `QTTUNE_BUILD_CORE_ONLY` is
  `HAVNE_BUILD_CORE_ONLY`; the old name still works, with a warning,
  until v0.3.4.
- Settings opens on the connect bar and five folded sections, each
  heading saying what it holds now: Adapter (the devices found, then
  the WiFi addresses and other choices the list doesn't show), Vehicle,
  What to read, Recording (with the recent recordings to replay) and
  Display. Connect... on Home, Live or the header opens Settings with
  Adapter unfolded. A button too long for a phone's width wraps its
  text.
- Selecting a device, a recording or a preset in Settings scrolls back
  to the URI field and Connect when they're out of view, as on a phone,
  and the chosen row is outlined.
- With **Record sessions** on, a replay isn't recorded: it would copy
  the recording it plays. Settings' Record line and the activity log
  say so; the switch keeps its setting for the next live session.
- Live's chart draws a crowded trace's back-and-forth stretches as a
  faint band, the range it swings over edged by thin lines, so a signal
  that swings every moment (lambda through the rev limiter, on a
  phone's narrow chart) reads as one shape rather than a hatch; its
  ramps, steps and small wiggles stay a line. Ten signals get a color
  of their own before any repeats dashed.
- Settings' presets include the two usual WiFi adapter addresses,
  `elm327://192.168.4.1:23` (an OBDX Pro) and
  `elm327://192.168.0.10:35000` (most others), and the Log page has a
  Copy button for the whole activity log.
- The window's title bar is dark on Windows, to match the app; on
  Windows 11 it takes the header's color, so the two read as one bar.
- The Log page shows the activity log (every connect, link change and
  ECU line, time-stamped), which sat at the bottom of Settings. The raw
  frame table is behind its Activity | Frames switch in developer mode.
- Home's logo is a small tachometer whose needle idles and blips into
  the red.

### Fixed
- An ELM327 adapter whose reset answered twice, the second time late
  (an OBDLink MX+ over a new Bluetooth link), no longer fails the
  connect: the setup command that drew the late answer is sent again.
- A supported-PID query answered with another PID's reply is asked
  once more, instead of the session polling without knowing which
  PIDs the car supports.
- Choosing a preset or a device of another kind in Settings no longer
  carries options it can't use into the URI, which failed the next
  connect (a replay's `profile=mock` or `speed=` on an adapter): keys
  that kind doesn't take are dropped, and the profile row shows Default
  in place of the mock's. One of the same kind keeps them all.
- On a phone, the app fills the screen after the phone turns, where
  bands showed at the right and the bottom.
- On a phone, connecting a long URI no longer pushes Settings past the
  screen's right edge: the "Connected:" line wraps.
- A WiFi adapter that can't be reached because this computer left its
  network says so ("the connection was cut off on this computer's
  side"), instead of a Windows sockets error number.
- Settings and Home fit a narrow window: Settings' rows of buttons wrap
  instead of pushing Connect off the right edge, and Home scrolls when
  the window is shorter than it. On a short window the Dashboard's
  dials fit the page.
- On a phone, the URI field no longer capitalizes or predicts what's
  typed into it, and Settings no longer offers an Open folder button
  that no app can open.
- A J2534 driver reporting a buffer overflow on a read no longer reads
  as "Adapter fault" with the read's messages dropped: the messages
  arrive, and the session carries on.
- Over an ELM327, a Service 01 PID whose reply spans several CAN frames
  (engine run time, 0x7F, for one) is read: it was asked with a
  response count that ended the reply after its first frame, so every
  request timed out. Only a profile you register can ask one; the
  built-in `obd2` doesn't.
- On Android, nothing of Havne's goes into the phone's backup or a
  transfer to a new phone: its preferences and recordings can hold the
  vehicle's VIN.
- The phone's session notification, and the header's connection label,
  show a Bluetooth adapter's address only by its last two parts
  (`elm327://bt:…:44:55`), so a screenshot or the lock screen doesn't
  carry the whole address.

## [0.3.2] - 2026-10-26

Replay, decoding in core, and every driver in a process of its own
([full notes](docs/releases/v0.3.2.md)).

### Added
- **Replay** ([ADR-0023](docs/adr/0023-replay-transport.md)):
  `replay://<recording>` plays a recording's `frames.csv` back on every
  platform at its own pace, at `?speed=` 0.1 to 100 or `max`, and with
  `?loop=1` round again (a recording whose frames all share one time
  plays once). Its default profile is `raw`; a J2534
  recording decodes under `?profile=obd2`. The activity log says what's
  playing and when it ends, and the session stays open at the end. If
  the recording changes or disappears while it plays, playback stops
  and every page says "Replay stopped"; playing a recording never locks
  the file.
- Connect-failure text names a vendor's status in hex as well as
  decimal ("status 34 (0x22, vendor-specific)").
- The Windows executables carry a version resource, so Explorer's
  Details tab names QtTune and its version.
- The Windows download's signed build provenance is a release file
  too (`<zip>.intoto.jsonl`), so the zip can be checked with
  `gh attestation verify <zip> --bundle <zip>.intoto.jsonl -R erickramer42/qttune`.

### Changed
- Every J2534 driver runs in a helper process of its own architecture
  ([ADR-0025](docs/adr/0025-one-j2534-backend.md)): 64-bit drivers in
  the new `ecuio-j2534-surrogate-x64.exe`, which ships beside
  `qttune.exe`, as 32-bit ones already did in
  `ecuio-j2534-surrogate.exe`. A driver that crashes ends its helper,
  not QtTune, and the session reports an adapter fault; every
  session's driver calls are in the surrogate's session log. A file
  that isn't a Windows DLL is refused with a plainer message.
- Decoding runs in core on the transport's thread, not in the UI
  ([ADR-0024](docs/adr/0024-decode-on-the-worker.md)): a frame dropped
  under load still delivers its values to the pages and the recording,
  and the UI's work per frame no longer grows with the profile.
- A raw session on an adapter listens to the engine ECU without
  `?filter=`, and `?profile=raw&poll=` asks it; Settings adds no
  `filter=` of its own.
- At disconnect the activity log, and a recording's `events.log`, give
  the session's delivery: frames received and dropped, and how long
  frames took to reach the app and values to reach its pages (median,
  99th percentile and maximum).
- Live's strip chart places each value at the time its frame was
  received (a replay's own time), and keeps the highest and lowest
  value of every 100 ms window, so a short peak stays on the chart.

### Fixed
- A link failure's activity-log line always counts its own episode:
  one could read "adapter fault (0 episode(s))" and repeat with 1.
- The Dashboard's held peaks and lows catch a value between two
  refreshes, as the strip chart does: the engine start replayed fast
  had shown a peak of 1,195 rpm against the recording's 1,261.
- Choosing raw for a replay in Settings no longer gives a URI the
  replay refuses.
- A replay that can't play its recording no longer says it failed to
  load a transport library.
- The dropped-frame count includes frames a fast replay drops while
  the connect completes.
- A URI with spaces around it, as pasted, connects.
- The loader refuses a relative DLL path, from the registry or a
  direct caller, before anything loads it.
- The strip chart stays sharp under zoom, and its labels fit when many
  traces meet a short plot.
- Live's signal list gives up rows on a short page, so the chart stays
  on screen.
- The Log page holds long replies without keeping their memory after
  short frames replace them.
- The first frame of a session is never lost to the connect.

## [0.3.1] - 2026-10-25

The first release with a Windows download
([full notes](docs/releases/v0.3.1.md)).

### Added
- A request engine that paces requests by the ECU's replies
  ([ADR-0015](docs/adr/0015-request-engine.md)), and a PID-support
  probe that stops asking for what the ECU says it lacks.
- Vehicle identification at connect: VIN, calibration IDs and the
  ECU's identification DIDs
  ([ADR-0021](docs/adr/0021-vehicle-identification.md)).
- The `uds-generic` profile: poll the DIDs a URI lists, or scan a range
  for the ones that answer ([ADR-0020](docs/adr/0020-uds-generic.md)).
- Frames carry a whole ISO 15765 message, up to 4,096 bytes.
- A failed connect says why, with the driver's own error text
  ([ADR-0018](docs/adr/0018-connect-failure-reporting.md)); a
  successful one names the adapter, its battery reading and versions
  ([ADR-0019](docs/adr/0019-session-info.md)).
- Metric or imperial display, chosen in Settings and remembered
  ([ADR-0022](docs/adr/0022-display-units.md)).
- Settings composes the URI from a preset, a profile row and a DID
  field.
- The simulated car: a scripted loop of pulls, a cruise and a quarter
  mile, warming over its first loops.

## [0.3.0] - 2026-10-22

### Added
- Signal decoding in core: linear scaling of unsigned, signed and
  float fields in either byte order, gated on message ID and mux key
  ([ADR-0004](docs/adr/0004-signal-decoding-model.md),
  [ADR-0016](docs/adr/0016-signal-definition-layout.md)).
- Profiles chosen by the URI: `mock`, `raw` (log only) and `obd2`
  (Service 01 PIDs on the engine ECU's channel)
  ([ADR-0013](docs/adr/0013-protocol-profiles.md)).
- LiveView with a strip chart, updated at 10 Hz whatever the bus rate
  ([ADR-0005](docs/adr/0005-coalesced-signal-updates.md)), and a
  Dashboard of gauges and shift lights.
- The J2534 transport on Windows: discovery of installed adapters,
  32-bit drivers through a surrogate process, and link health that
  tells a quiet bus, failing requests and a dead adapter apart
  ([ADR-0008](docs/adr/0008-x86-surrogate-host.md),
  [ADR-0012](docs/adr/0012-j2534-transport-adoption.md),
  [ADR-0014](docs/adr/0014-single-threaded-surrogate.md)).
- Session recording: every frame, decoded value and activity line per
  connect ([ADR-0017](docs/adr/0017-session-recording.md)).
- UI zoom, kept between runs.

### Changed
- The C ABI's names carry the `ecuio_` prefix; the frame struct and the
  close and destroy calls were redesigned
  ([ADR-0006](docs/adr/0006-frame-abi-v0.3.md)).

## [0.2.0] - 2026-10-05

### Added
- A session-scoped frame callback API with snapshot dispatch
  ([ADR-0002](docs/adr/0002-callback-pair-identity-snapshot-dispatch.md)).
- The mock transport, streaming synthetic CAN frames.
- A live frame log, fed through the bridge with bounded backpressure
  ([ADR-0003](docs/adr/0003-backpressure-bounds-in-flight-frames.md)).
- CI builds the app on Qt 6.12 LTS.

## [0.1.0] - 2026-09-30

### Added
- The skeleton: a Qt-free C++17 core behind a C ABI
  ([ADR-0001](docs/adr/0001-qt-free-core-c-abi.md)) and a Qt 6 / QML
  shell with a responsive layout.
- CI on Windows, macOS and Linux, running the core's tests.

[Unreleased]: https://github.com/erickramer42/havne/compare/v0.5.1...HEAD
[0.5.1]: https://github.com/erickramer42/havne/compare/v0.5.0...v0.5.1
[0.5.0]: https://github.com/erickramer42/havne/compare/v0.4.1...v0.5.0
[0.4.1]: https://github.com/erickramer42/havne/compare/v0.4.0...v0.4.1
[0.4.0]: https://github.com/erickramer42/havne/compare/v0.3.6...v0.4.0
[0.3.6]: https://github.com/erickramer42/havne/compare/v0.3.5...v0.3.6
[0.3.5]: https://github.com/erickramer42/havne/compare/v0.3.4...v0.3.5
[0.3.4]: https://github.com/erickramer42/havne/compare/v0.3.3...v0.3.4
[0.3.3]: https://github.com/erickramer42/havne/compare/v0.3.2...v0.3.3
[0.3.2]: https://github.com/erickramer42/havne/compare/v0.3.1...v0.3.2
[0.3.1]: https://github.com/erickramer42/havne/compare/v0.3.0...v0.3.1
[0.3.0]: https://github.com/erickramer42/havne/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/erickramer42/havne/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/erickramer42/havne/releases/tag/v0.1.0
