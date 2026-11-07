# ADR-0056: The analysis log: picked signals logged at the bus's own pace between an explicit Start and Stop, exported as one wide row per sweep

- **Status:** Accepted. Not built.
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

### 5. The export: the wide CSV, sized at export

The wide layout becomes the CSV export's default, in core
(`ecuio_export`), the app and `havne-cli`:

```
Time (ms),Engine speed (rpm),Vehicle speed (km/h),Ignition timing (deg)
0,812,0,10.5
48,815,0,10.0
```

- `Time (ms)` first, then each signal as `Name (unit)` in the picked
  order; every row filled; fixed decimals per signal; comma, CRLF, no
  quoting needed (a name or unit holding a comma or a quote is quoted
  per RFC 4180, a formula-shaped one still fails the export as today).
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
  high load or full throttle, and to steady cruise, found from the
  signals.
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

## Open questions

1. Units: the export in the units the app shows (its imperial choice)
   or core's metric with `havne-cli --units`?
2. Decimals: from each signal's scale, or a signal file's own
   `decimals` key (a schema change)?
3. The row rule when one picked signal stops answering: hold its last
   value and keep writing rows (with a flag column?), or stall rows
   until it returns?
4. The navigation aids' thresholds (what counts as high load, as
   steady cruise), and whether they need throttle or fall back to load.
