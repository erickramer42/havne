# ADR-0056: The analysis log: picked signals logged at the bus's own pace between an explicit Start and Stop, exported as one wide row per sweep

- **Status:** Accepted. Built: the focus (section 2), the recorder
  attached to a running session (section 3), the rows and the wide CSV
  with its estimate (sections 4 and 5), `havne-cli log` and the app's
  Start log and Stop log, the viewer's Export and a focus's minimum time
  between sweeps (the appendices). The navigation aids (section 6) are
  v0.7.3's.
- **Date:** 2026-11-07.
- **Builds on:** [ADR-0015](0015-request-engine.md) (the request
  engine) and [ADR-0039](0039-full-service01-coverage.md) (its two rate classes);
  [ADR-0049](0049-recordings-in-core.md) (recordings in core, markers,
  the viewer); [ADR-0054](0054-exports-and-imports.md) (exports
  scrubbed by construction, the CSV export as built).

## Context

People who study a car's behavior over a drive (timing under load,
knock, fuel trims, boost) work from a log of a few dozen signals they
chose, taken over a planned run and read in a spreadsheet or a
dedicated analysis tool. The logs they pass around share one shape:
one header row, a time column first, one column per signal named with
its unit, one row per pass through the signals with every cell filled,
and a few hundred KB to about 1 MB for a 10-20 minute drive.

Havne records already, but not in that shape or by that workflow:

- **Record sessions is all or nothing.** The recorder (ADR-0049)
  attaches after `ecuio_session_create` and before
  `ecuio_session_start`, so a recording spans the whole session. There
  is no "start logging now" on a running session.
- **The request set is the profile's.** A session polls every request
  its profile and URI give (after the probe drops what the ECU
  refuses), in two rate classes. Ticking a signal on Live picks what is
  plotted, not what is asked, so a log of five signals is paced like a
  log of fifty.
- **The CSV export is long form** (`time_s,signal,unit,value`, one row
  per sample), in `havne-cli` only. The app has no export of a
  recording; its Log page exports the frames in memory.

## Decision

### 1. The workflow

Connect; pick signals (Live's checkboxes, the same list that picks the
plotted traces); **Start log**; drive; **Stop log**. Stop closes the
recording, which then opens in the viewer and exports. Record sessions
stays as it is, the always-on mode beside this deliberate one.

- **Nothing waits for a save.** The recorder writes as it goes, so a
  crash or a dropped link keeps everything up to that moment; Stop
  only closes the folder.
- **No buttons while driving** beyond Start and Stop: nothing to mark,
  nothing to press during a run. What happened is in the signals
  (throttle, RPM, load), and the whole log is the record, steady state
  as much as any pull.
- **A live readout beside Stop log:** the time logged and the size so
  far at the current row rate (`12:30, 0.8 MB`).

### 2. Requests narrowed to the picked signals

A new call narrows a running session's rotation to the picked signals:

```c
/* Asks only the requests that carry these signals, until called again;
 * count 0 asks the whole set again. Append-only ABI. */
ecuio_status_t ecuio_session_set_focus(ecuio_session_t* session,
                                       const uint32_t* signal_ids,
                                       uint32_t count);
```

- The engine parks every request whose defs are all outside the focus
  (it stays in the table, it isn't refused) and runs the picked ones
  in the fast class, so the rotation is as short as the picks allow.
  Packing (several Service 01 PIDs to a request) keeps working over
  what is left.
- Passive signals (broadcast frames a listener hears) aren't asked;
  the focus only chooses which of them the log keeps.
- The probe and the DID scan are untouched: the focus applies to the
  rotation after them.
- One call on the worker's own queue, so the rotation changes between
  two requests, never inside an exchange.

### 3. The recorder attaches to a running session

`ecuio_session_record` also accepts a session that has started: the
recorder's ring is attached on the worker between two frames, and the
recording starts at that frame, its time 0 there. ADR-0049's rule
(attach before start) gets a dated amendment: before start is still how
Record sessions works; after start is the analysis log. A second
attach while one is live stays refused.

### 4. Rows: one per sweep, derived at export

A row is written once **every picked signal has a sample newer than the
previous row's**. For a polled set that is one row per sweep through
the rotation; for a mix of polled and broadcast signals it follows the
slowest picked signal, which is the rate at which every cell is new.

- Derived from the values at export, so it needs no new record in the
  recording, and it works on every recording already made, replays and
  imports included.
- No ceiling while logging: the bus and the ECU set the pace, and fewer
  signals run faster. An advanced setting, a minimum time between
  sweeps (default off), exists for a slow adapter or a busy ECU, not
  for file size.
- A row's time is the sample that completed it, in integer ms from the
  log's start. Each cell is that signal's newest value at the row's
  time: held, never interpolated or averaged, so a spike is shown as
  read or not at all.
- **A signal that stops answering never holds up the rest.** One with
  no new sample for 1 s, or 3x its own usual interval if that is
  longer, leaves the rule, and rows go on without it; its cells are
  left empty, never 0 and never its last value, since 0 is a reading
  and a held value hides the loss. It rejoins the rule with its next
  sample. Spreadsheets and analysis tools read an empty cell as
  missing.

### 5. The export: the wide CSV, sized at export

The wide layout becomes the CSV export's default, in core
(`ecuio_export`), the app and `havne-cli`:

```
Time (ms),Engine speed (rpm),Vehicle speed (km/h),Ignition timing (deg)
0,812,0,10.5
48,815,0,10.0
```

- `Time (ms)` first, then each signal as `Name (unit)` in the picked
  order; every cell filled unless its signal is silent (section 4); comma, CRLF, no
  quoting needed (a name or unit holding a comma or a quote is quoted
  per RFC 4180, a formula-shaped one still fails the export as today).
- **Units:** the app's export in the units the app shows (its metric
  or imperial choice), `havne-cli`'s by its `--units`, metric by
  default; both through core's `ecuio_display_convert`.
- **Decimals:** each signal's own decimal places, as the app shows it
  (`decimal_places`, 0 to 3, one more where a conversion needs it, kPa
  to psi). Compared with logs other tools write for the same OBD
  signals, these match (engine speed, coolant and intake temperature,
  speed, timing, throttle) or give exactly the PID's resolution (load
  and fuel trims at one decimal: their steps are 0.39 % and 0.78 %).
  The one that rounds a step away is the MAF (0.01 g/s shown to one
  decimal), which goes to two.
- The long form stays, behind `--layout long` (`havne-cli`) and an
  option in the app, for pandas and R. This is a change in the default:
  a CHANGELOG "Changed" line, in a minor release (ADR-0048).
- **The whole log, sized by its time resolution.** The export always
  covers the whole log (steady state is as much the analyst's as any
  pull); its size is set by the row interval: every sweep (the
  default), every 100 ms, every 250 ms, or a custom interval, with the
  file's size estimated beside each choice before anything is written,
  and the last choice remembered. Thinning keeps each row's newest
  values, as above. The other two levers are the driver's: fewer
  signals picked, and a shorter log between Start and Stop.
- Scrubbed as every export is (ADR-0054): identification and text
  signals left out, the file checked for the car's identity.

### 6. The viewer, for the analyst

The log reads as well in Havne's viewer as in the analyst's own tool,
and the viewer aims to be the better of the two: moving around a long
log quickly, never cutting it.

- Zoom and pan over the whole log, the cursor reading every picked
  signal at once (built).
- Navigation aids, not trims: jump to the next or previous stretch of
  high load and of steady cruise, found from the signals, with
  thresholds the user can change:
  - high load: throttle at 80 % or more while engine speed rises, for
    at least 1 s; engine load at 80 % or more where there's no
    throttle signal;
  - steady cruise: speed within 2 km/h and throttle within 3 % for at
    least 10 s.

  The defaults are a starting point, tuned against real logs before
  the feature ships.
- The roadmap's chart panes, two cursors and a span's statistics (the
  time between the cursors, each signal's change, lowest, highest and
  mean), which an analyst reaches for first; their own ADR.

### 7. Front ends

- **The app:** Start log, Stop log and the readout in the header while
  connected; the viewer's Export (pcapng, CSV wide or long, a folder
  copy) with the row interval and its size estimate. This also closes
  the app's missing recording export.
- **`havne-cli`:** `havne-cli log <uri> --signals <names> <folder>`
  (focus, record, stop on Ctrl+C or `--seconds`);
  `havne-cli export ... --format csv [--layout wide|long] [--rows
  sweep|<ms>]` (`--from`/`--to` stay for scripts).
- **Python:** `session.set_focus([...])` and recording a running
  session; pandas reads the export or `signals.csv`, so no wrapper of
  the export is needed.

## Alternatives considered

- **Reconnect behind the scenes with the picked set.** No new core
  call, but Live goes blank for the reconnect, the probe runs again,
  and an adapter that takes seconds to set up (ELM327 over Bluetooth)
  makes Start feel broken. Kept as the fallback if the focus call
  proves hard on a transport.
- **The recorder marks each sweep's end** (a record per pass). Exact,
  but a new record kind, no help for recordings already made, and
  undefined for broadcast signals. The derived rule gives the same
  rows for a polled set.
- **A rate ceiling while logging, for file size.** Throws away data
  that can't be had again; the size belongs at export, where the full
  log is kept.
- **Trimming the export to marks or to detected pulls.** Marks need a
  button pressed while driving, and both cut away the steady state an
  analyst reads alongside the pulls. The size comes from the row
  interval instead, and the viewer's navigation finds the pulls.
- **Averaging when thinning.** Smooths a knock spike or a misfire into
  nothing; held values are what was read.
- **A spreadsheet workbook (`.xlsx`) instead.** Overlaps the wide CSV,
  which spreadsheets and analysis tools read as is; left in the
  potential roadmap.

## Consequences

- Two append-only ABI additions (the focus call, attach after start)
  and a change of the CSV export's default.
- The engine gets a parked state beside refused; tests pin that a
  parked request comes back on `count 0` and that the probe's results
  survive a focus.
- The app gains its recording export, and with it the scrubbed copy to
  share from the phone.
- What a log of N signals reaches on each adapter is measured, not
  promised: a bench row per transport (J2534, ELM327 over USB, WiFi and
  Bluetooth, SocketCAN) in the run sheets.

## Appendix: the build (2026-11-07)

Steps 1-6 of the build (the focus, the recorder attached to a running
session, the wide CSV and its estimate, `havne-cli log`, the app's Start
log and Stop log, the viewer's Export) are v0.7.0's; the minimum time
between sweeps (section 4) and the navigation aids (section 6) are
v0.7.1's, decided 2026-11-07.

**Section 2, the focus, as built:**

- `ecuio_session_set_focus(session, const uint16_t* signal_ids, count)`
  (`core.h`): the ids are `uint16_t`, as every other id list in the ABI
  (`ecuio_session_get_unsupported_signals`, the absent signals), not the
  `uint32_t` section 2 wrote. A NULL session, or NULL ids with a count,
  `ECUIO_ERR_NULL_ARGUMENT`; an id the session's set lacks
  `ECUIO_ERR_INVALID_ARGUMENT` with "no signal with id N in profile P" in
  the detail and nothing changed; after close `ECUIO_ERR_SESSION_CLOSED`.
  Before or after start alike.
- The session keeps the picks (the ids in the caller's order, one byte
  per def) under a mutex with a generation; each requesting worker
  (J2534, ELM327, SocketCAN) takes a new generation once before its
  loop and then between passes, beside the clear and Read again
  requests (`ecuio_take_focus`), maps each picked def to its request
  (the mux key at offset 0, the channel of its `message_id`) and calls
  the engine: a request none of the picks reads is `parked`, a state
  beside `dropped` (out of the rotation, in the table, back on a count
  of 0); the kept ones run in the fast class and get their class back at
  the end (`slow_default`). The walk, the DID scan, identification,
  diagnostics, a clear and a request already out are untouched; a
  dropped request stays dropped; packing gathers kept requests alone.
- A request added under a focus (a scanned DID found) starts parked,
  and the worker resolves the stored picks again at its next pass: a
  pick that names the DID unparks it, and one that doesn't leaves it
  parked until the focus's end.
- The engine moves a channel's generation only when the focus changed
  its count of requests in the rotation, so a channel the focus leaves
  as it was isn't republished in its support report.
- A requesting session refuses a focus that would leave it nothing to
  ask: none of the picks reads a request the session can make (passive
  signals, Service 09's, a request the walk dropped, a DID the scan
  hasn't found, under `?poll=` any but the polled PID's).
  `ECUIO_ERR_INVALID_ARGUMENT`, "the focus leaves nothing to request" in
  the detail, and the focus it had stands. A focus with one askable
  pick among others is taken. The worker keeps what is askable in step
  with the table (a drop, a DID found), so the call judges by the table
  as it stands.
- `mock://`, `replay://` and a listening session ask nothing and accept
  any focus, changing nothing: their values aren't asked for.
- The call clears the thread's detail on entry, so a refusal without
  words of its own leaves none from an earlier call.
- Python: `Session.set_focus(signals)`, ids or names, before or after
  `start()`; an id outside 0-65535, or one that isn't an `int` (a
  `bool` among them), is a `ValueError` before the call.
- The focus is judged against the session's requests when the call is
  made. A focus taken and later left with nothing to ask isn't refused
  after the fact: the first walk after start, the walk of a focus set
  before start, a re-walk after a recovery, or an NRC can drop every
  pick, and the session then asks nothing until the focus changes or
  ends. The front ends see each drop in the support report, as for any
  dropped request. Between a drop and the worker's next take, the
  askable map the call judges by can still name a PID no longer asked.

**Section 3, the recorder attached to a running session, as built:**

- `ecuio_session_record` takes a started session. The recording starts
  at the call, whose moment is its zero, not at a frame boundary on
  the worker: the worker reads the recorder and its zero together, so
  every frame dispatched after the attach is in, with its `host_us`
  from that zero. A second attach while one is live is refused, as the
  section says; a recorder that has closed frees the session for
  another, which counts from its own attach. The session's link exists
  from create, so a worker started before the attach finds it.
- The header's `focus` line is the focus as it stands at the attach,
  the names in the order they were picked, written when one is set;
  the `link` line is the link's state and episode count at the attach
  (`ecuio_session_get_error_state`'s pair, in the link lines' words),
  written for a started session only. Both go between `frame flags` and
  the `signal` lines; [ADR-0049](0049-recordings-in-core.md)'s appendix
  of 2026-11-07 has the format.
- `latency.csv`'s `t_s` moves to the recording's clock with it (that
  appendix), so a log begun mid-session plots its reply timing against
  its frames.
- Python: `Session.record(root)` after `start()` as before it; one
  recorder at a time.
- The header's `link` line says `healthy (state 0, 0 episodes)` for a
  link that is OK with no episode; `link_text`'s "recovered" stays for
  the event lines, where OK after a vehicle timeout (which counts no
  episode) is a recovery.
- Reply timing's counts in `latency.csv` are the session's, cumulative
  since it started, as in any recording: a log begun mid-session finds
  them already counted up, and the rows still say which requests
  changed since the last snapshot. The attach resets nothing.
- A refused attach (another recorder is live) leaves the recorder as it
  was: everything the attach makes goes into locals and the recorder
  takes them only once the link is taken, under its mutex, so the same
  recorder attaches whole on a later try.
- `ecuio_session_record` runs on the thread that starts and closes the
  session, as `recording.h` says; `started` stays a plain bool.
- `RecorderTest`'s cases are listed in that appendix.

**Sections 4 and 5, the rows and the wide CSV, as built:**

- The rule as section 4 says, with two things it left open: a column
  with samples in range starts in the rule at its first sample, one
  with none (a named pick the session never answered) starts outside
  it and holds no row up, and a column's usual interval is the median
  gap between its samples. A row's time is the completing sample's
  host_us to the nearest ms.
- `ecuio_export_options_t` grows within its reserved words (`layout`,
  `row_ms`, `units`, `column_count`, `columns_utf8`), the report's
  reserved word is `rows`, and the estimate is `ecuio_export_estimate`
  over several intervals in one pass; [ADR-0054](0054-exports-and-imports.md)'s
  appendix "the wide CSV as built" has the file, the columns' order
  and the CLI's flags. Columns are picked by name: the file is keyed
  by name and so are the app's plot choices.
- With no columns named, the recording's `focus` header line (section
  3) picks them, so a log's export needs no list; with no focus line
  either, every number signal in range.
- The grid's last row sits on the step at or after the last sample, so
  the newest values reach the file.
- The MAF's two decimals (section 5) are the mock's and obd2's defs
  together, the sets' shared names and units pinned by
  `SignalsTest.MockAndObd2ShareDisplayNamesAndUnits`.
- `havne-cli export` writes the wide layout by default; the long form
  is `--layout long`. A named column the recording has no values for
  is said on stderr and counted in the `exported` line's `missing`, so
  a typo isn't silent. The app's Export (step 6) takes the same options.
- The export's report has no reserved word left (`rows` took the last):
  a later figure needs a struct of its own.

**Section 7, the app's Start log and Stop log, as built:**

- The bridge's `startLog()` and `stopLog()`, with `logActive`,
  `logElapsedMs`, `logBytes` and `logStopped(folder, byUser)` (the
  folder absolute; `byUser` true for Stop log alone). Start focuses the session on Live's plotted rows
  (`SignalListModel::plottedIds`, the ticked rows Live shows), then
  opens a recorder as Record sessions does (the same root, label and
  `caller` lines, the signal file) and attaches it to the running
  session. The focus goes first, so a focus core refuses opens no
  folder; a recorder that fails to open, or that the session refuses,
  puts the whole set back. A refused recorder's folder, made by that
  open and holding no frame, is removed (in the open Record sessions
  shares, so its refusal leaves none either).
- Start is refused, its reason in the activity log as "Log not
  started: ...", while a log runs, while not connected, while Record
  sessions' recorder is live ("Recording this session already": one
  recorder a session), on a replay unless replays are recorded, with no
  row plotted, and when core refuses the focus (its detail, "the focus
  leaves nothing to request: ..."). A refusal opens no recorder.
- The log takes the bridge's one recorder slot, so `recordingActive`
  holds during it and the notes, a failed write and a disconnect close
  it as they close a Record sessions recording. Every end (Stop log, a
  disconnect, a failed write) goes through the recording's close, which
  then ends the log: the focus back to the whole set while the session
  runs (after the close, so the log never holds the widened rotation),
  the readout's last figures kept, `logStopped` emitted. A Stop log
  during a teardown waits for the teardown's close. A disconnect or a
  failed write is reported as it is for a Record sessions recording
  ("Disconnected: ...", "Recording stopped: ..."), with `byUser` false.
  A teardown closes the recording before it clears `disconnecting`, so
  a connect started from that change finds the slot free.
- Ticks on Live during a log change the plot only; the focus is the
  one taken at Start.
- The readout: the time on the bridge's host clock, which counts a
  sleep, and the folder's file sizes summed once a second on the GUI
  thread. That is one directory listing of a handful of files a second,
  no file opened or read, so it holds the GUI thread for well under a
  millisecond on a phone's storage.
- Main's header: "Start log" while connected with no recorder live (not
  on a replay), enabled with a row plotted; while a log runs, the
  readout (`m:ss`, or `h:mm:ss` past an hour, over MB to a tenth: two
  lines, so a ten-hour log's fits beside the name) and "Stop log", with
  Mark and Disconnect hidden and the name hidden under 420 px, so the
  header fits 384 px. After Stop log the app goes to
  Recordings and opens the folder in the viewer; a log the session's
  end or a failed write closes leaves the page as it is, since the
  driver may be reading Live.
- Tests: `BridgeTest.StartLogIsRefusedWithItsReason` (a replay
  among them), `ALogRecordsTheRunningSessionUntilStopped` (mock; Stop
  log's end is the user's, a disconnect's isn't),
  `AStartLogTheSessionRefusesLeavesNoFolderAndNoFocus` (a recorder of
  the test's own holds the slot), `AFailedWriteEndsALogNotAsTheUsers`,
  `AConnectAtTheTeardownsEndFindsTheRecordingClosed`, and over the fake
  ECU `ALogAsksThePlottedPidsAlone` (the fake's writes are the plotted
  PIDs alone, a tick leaves them so, the first `host_us` is near zero,
  Stop asks the whole set again) and `AFocusCoreRefusesStartsNoLog`;
  `QmlTest.phone`'s `test_theHeadersLogStartsStopsAndOpensItsFolder`
  (the header within 384 px before and during a log, a ten-hour
  readout within 420 px with and without the name, a disconnect during
  a log staying on Live) and `test_theLogsTimeReadsMinutesThenHours`
  (a negative or non-finite time reads 0:00).

**Section 7, `havne-cli log`, as built:**

- `havne-cli log <uri> <folder> [--signal <name>]... [--label <name>]
  [--seconds S] [--signals <file> | --plugin <pkg>]` (`cli/log_lines.h`,
  `run_log` in `cli/main.cpp`): the session as `stream` makes it (the
  same profile sources), the names resolved against its set in the
  order given (a name it lacks, or one twice, is usage: exit 2 with the
  reason on stderr and nothing on stdout, the check made after the
  connect and before any line), `ecuio_session_set_focus` with the ids
  (a focus the session refuses, "the focus leaves nothing to request",
  is an `error` line, exit 3), then a recorder opened under `<folder>`
  and attached before start, so the header's `focus` line is the picks
  and `export` with no `--signal` takes those columns. Lines: `start`,
  `session`, `focus` {signals [{id, name}]}, `recording` started (or
  failed, exit 1), `health` on a change of the error state, `slept` as
  stream's, a `log` line every 10 s of session time {seconds, bytes (the
  recorder's files summed), values (rows in signals.csv)}, `end`
  {reason interrupted|seconds, folder, seconds, bytes, values}, the end
  line also the recording's last note. No frame or values lines: the
  folder is the output. `HAVNE_CLI_LOG_EVERY_MS` (tests only, 100 or
  more) shortens the period. The log runs until Ctrl+C, `--seconds`, a
  failed recording (reason `recording-failed`, exit 1: the log is what
  the command is for) or a replay's last frame (`replay-end`, as
  stream's); a lost or dead link is logged in health lines and waited
  out, since an unattended log must survive a dropout. An output that
  fails under it (a closed pipe) ends it quietly with the recording
  closed whole, `output-failed` the close's why (stream --record the
  same; its close had no why on that path and crashed). Exit codes as
  stream's; the recorder closes after the session's close, attached if
  an interrupted close hangs 2 s, as stream's does.
- Not built: markers typed on standard input (stream --record's); a log
  from the command line is left to run, and the app's Mark button marks
  the app's log.

## Appendix: the log on a car, over ELM327 (2026-11-08)

Amended 2026-11-08: the consequences section promised a bench row per
transport. [2026-11-08-linux-car](../bench-sessions/2026-11-08-linux-car.md)
gives the rows for ELM327 on native Ubuntu 26.04, on the test car at
idle: `havne-cli log` for about 70 s per run, a sweep one new value of
every picked signal, so sweeps per second is the slowest signal's rate.

| Signals | OBDLink MX+ (Bluetooth) | VX, USB | VX, WiFi | VX, USB, `&pack=0` |
|---|---|---|---|---|
| 1 | 27.45 | 47.38 | 45.31 | not run |
| 4 | 24.00 | 9.61 | 8.80 | 11.85 |
| 8 | 15.48 | 6.63 | 4.66, with stalls | 5.93 |

Every reply after the connect's reports carried only the picked PIDs,
`log` lines came every 10 s, SIGINT ended each run with `interrupted`
and the recording whole, and `havne-cli export --format csv --layout
wide` wrote exactly the picked columns. The VX is the faster adapter
with one signal and the slower with several packed: its packed requests
take about 100-113 ms each against about 21 ms for one PID. A pack
carries a response-count digit, and the VX in ELM mode still prints its
prompt about one `AT ST 19` timeout after a multi-frame reply, as the sheet's bus and prompt timings show. `&pack=0` helped at 4
signals and cost at 8. J2534 and SocketCAN sending have no row: they
have not run.

## Appendix: the viewer's Export as built (2026-11-08)

Step 6 ships in v0.7.1, not v0.7.0 as the appendix "the build" has it.

**The whole log, in every format.** The Export writes the whole
recording (`from_us` and `to_us` 0), the chart's zoom being a view, as
Save report's. `--from` and `--to` are `havne-cli`'s alone
([ADR-0054](0054-exports-and-imports.md)'s note of 2026-11-08).

**The panel** (`RecordingExportPanel.qml`), opened by Export... beside
Save report, takes the chart's place while it's open: a phone keeps the
rest of the viewer on its first screen. Inline, since a Popup doesn't
follow the zoom. Save as... and the last result come first, so they stay
on a phone's first screen, then CSV, pcapng or Folder (CSV disabled, with the reason,
for a folder with no `signals.csv`; pcapng hidden with no `frames.csv`);
Wide or Long under CSV; under Wide the row interval and the columns.
Every control is a touch's size on a phone.

**The sizes.** One row a choice: every sweep, every 100 ms, every 250 ms
and a custom interval (1 to 86400000 ms, `--rows`' range), each with its
rows and the file's exact bytes, from one `ecuio_export_estimate` call
over them all on a worker of its own. Asked at the open and 300 ms after
a custom interval or a column change; the Export never waits for one.
Core can't stop a call under way, so one runs at a time, a newer call
waits behind it and replaces any already waiting, and only the newest's
answer is shown. The long form, pcapng and the folder show no figure:
core sizes the wide CSV alone. The last interval is kept between runs;
format, layout, columns and Raw start over at each open.

**The columns:** the log's own (core's rule: the focus line, else every
number signal) or the viewer's ticks, by name in the recording's order.
The ticks aren't offered while the viewer shows values decoded through a
profile, whose names aren't the recording's columns. The panel says then
that the CSV holds the values as recorded, since `signals.csv` is what
core exports and no front end has a profile option.

**Raw** is the named option ADR-0054 section 1 asks for: "Raw, not
scrubbed", shown in Developer mode alone, off at each open, with a line
saying the file can hold the VIN and calibration IDs.

**Where it's written** (`app/recordingexport.{h,cpp}`, `RecordingExport`,
QtCore, on a thread of its own; its destructor waits, core having no
cancel). Core writes paths and refuses one that exists, and Android's
picker hands back a `content://` URI, so a file is written in a
`QTemporaryDir` and copied a chunk at a time to the place picked:
`QSaveFile` for a local file, whole or not at all; the provider's own
stream otherwise. *Amended 2026-11-09:* the destructor, which runs when
the app quits (an Android teardown included), stops both workers
through core's cancel and waits only for the next ask (ADR-0054's
appendix "an export canceled"); at startup the app removes an export's
temporary folder (`havne-export-` and six letters or digits) once it is
a day old. A picker makes its document before the export runs, so
a refused export or a failed copy removes a provider's document; a local
file stays as it was. A folder copy on a local disk is core's own, made
whole under its partial name; through a provider it is made in the
temporary folder, then a new folder is made under the one picked and
filled file by file, and removed if a file fails. The picker asks for a
folder, a copy's name being the app's to choose. Android's picker
offers neither the storage's root nor Download as that folder.

Run on the test phone (Android 16) over a `mock://demo` recording,
2026-11-08: through the document picker, the folder copy, a wide CSV and
a pcapng each byte for byte what `havne-cli export` writes for the same
recording and options, and a second copy named `<stamp>-2`.

**The names** offered are the recording's stamp alone (`<yyyyMMdd-HHmmss>`
with its `-n`, or "recording"), never the rest of its folder's name,
which can hold the vehicle's label. A folder copy takes the first of
`<stamp>`, `<stamp>-2` and on that is free.

**The result** says what was written and what was left out, by class, in
the panel and as one activity line, never a path.

**`havne-cli export <recording> --format csv --estimate`** (no `<out>`)
writes the same sizes, one `estimate` line {format, layout, columns,
missing, sizes [{row_ms, rows, bytes}]}: at `--rows`' interval, else
the three the panel offers. Exit codes as `export`'s; other formats, the
long form and a file named are usage.

**Tests:** `RecordingExportTest` (test_bridge: each format byte for byte
against core's own call for the same options, a provider's path through
`setThroughProviderForTesting`, the sizes against the files written, the
newest estimate winning), `QmlTest.recordings`' Export cases,
`QmlTest.phone`'s `test_theExportPanelFitsAPhone`, `CliTransferTest`,
`CliUsageTest.ExportEstimate*`, `CliGoldenTest.ExportEstimateSizesTheWideCsv`
and `CliTransferRunTest.csv` (each estimate's bytes the file's).

## Appendix: the minimum time between sweeps as built (2026-11-08)

Section 4's advanced setting belongs to the focus: it is asked for with
the picks and lasts as long as they do.

- `ecuio_session_set_focus_with(session, ids, count, min_sweep_ms,
  flags)` (`core.h`); `ecuio_session_set_focus` is the same call with
  0 and 0. 0 is none; the most is `ECUIO_FOCUS_MIN_SWEEP_MAX_MS`
  (60000). A flag, a time past that, or a time with no signals picked
  is `ECUIO_ERR_INVALID_ARGUMENT` with nothing changed.
- It is on the focus call rather than a URI key, since a key needs a
  reconnect to change, and rather than a call of its own, which would
  be a second state that does nothing outside a focus.
- **Only a focused session has one.** Live, Record sessions and
  `havne-cli stream` keep their pace, and the focus's end takes it
  away. A sweep is defined here because a focus runs every pick in one
  class; across the fast and slow classes of a whole set it isn't, and
  a busy ECU gets no relief on Live.
- **In the engine, per channel, from one sweep's start to the next's**
  (`ecuio_engine_focus`'s `min_sweep_us`, `ecuio_engine_count_sweep`;
  the time is the driver's, as for every engine rule). A sweep ends once
  as many rows have been asked as the rotation holds, and the rows are
  consecutive, so every pick has been asked. A sweep longer than the
  minimum waits for nothing.
- **Packing:** a pack counts each of its rows. Eight picks at six PIDs
  a pack are two requests a sweep, four rows asked twice; the count
  starts over at each sweep, since the wait can only fall between two
  requests.
- **Only the rotation waits.** The walk, identification, diagnostics, a
  clear and the DID scan go out in the gap.
  `ecuio_engine_next_event_us` reports the wait, so the workers sleep
  through it and, still having something to ask, count no quiet reads:
  a long minimum raises no link-health banner. "Not responding" stays
  ten unanswered requests in a row, so with one request a sweep it comes
  after ten times the minimum: 10 s at 1000 ms.
- A later call with the same time keeps a wait under way; a different
  time ends it. A scanned DID joining the focus changes neither the
  sweep under way nor its wait, so DIDs found faster than a sweep runs
  can't keep it from ending.
- `mock://`, `replay://` and a listening session accept it and change
  nothing, as with the focus, so neither the recording's header nor the
  activity log's line names it there. `ECUIO_SESSION_INFO_REQUESTS` (set
  at create) says which sessions ask.
- **The front ends:** Settings' Advanced page, Off (the default), 100,
  250, 500 or 1000 ms, kept between runs and read at each Start log, so
  no reconnect, and the activity log's line says it;
  `havne-cli log --min-sweep <ms>` (with `--signal`), the `focus` line's
  `min_sweep_ms` when one is set; Python's `set_focus(signals,
  min_sweep_ms=0)`.
- **The recording** has `min sweep  250 ms` after its `focus` line, so
  a log's row rate can be explained later; the folder export keeps it.
  A row of the wide CSV is unchanged: a minimum gives fewer of them, and
  the silence rule's 3x interval follows the sweep's.
- **The frames check** reads that line and adds the minimum to its
  bus-gap limit (the default or the caller's): a sweep of one reply can
  be that quiet, start to start, plus its overshoot. A limit of the
  larger of the two would call every sweep at 1000 ms a gap.
- No real adapter has run it; the checks are the fakes' and the
  simulated ELM327's.

## Appendix: where the later steps landed (2026-11-09)

The appendix "the build" places the minimum time between sweeps
(section 4) and the navigation aids (section 6) in v0.7.1. The minimum
time is v0.7.1's and is built (the appendix "the minimum time between
sweeps as built"). The navigation aids moved to v0.7.3, with the
analysis viewer, since they need logs from real drives to design
against.

## Note: a focus on a computed signal (2026-11-10)

Section 2's focus names signals to ask, and a computed signal asks nothing
itself. A picked computed def now keeps its inputs' requests in rotation:
`ecuio_session_set_focus` adds each input's def to the session's picked
defs (`focus_defs`), which `ecuio_take_focus` and the askable judgment
read, and leaves the caller's ids (`focus_ids`) as given, so the recorder's
`focus` line, the CLI's `focus` line and the app's picks name what was
picked. A focus naming obd2's Boost alone is taken and keeps MAP (01 0B)
and baro (01 33); a focus whose picks, inputs included, have no askable
request is refused as before. `CoreTest.AComputedPickKeepsItsInputsRequested`
and `J2534AdoptionTest.AFocusOnBoostAsksItsInputsAndComputesIt` pin it.
