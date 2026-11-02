# ADR-0049: Recordings in core: a writer thread every front end shares, and a reader with an index whose memory follows the recording's length, not its size

- **Status:** Proposed.
- **Date:** 2026-11-02.
- **Builds on:** [ADR-0017](0017-session-recording.md) (the recording
  folder and its files, whose "recorder in core" alternative this
  takes up); [ADR-0023](0023-replay-transport.md) (`replay://`, the
  reader of `frames.csv`); [ADR-0024](0024-decode-on-the-worker.md)
  (values decoded on the worker); [ADR-0031](0031-command-line-client.md)
  (havne-cli); [ADR-0042](0042-reply-timing.md) (`latency.csv`).
- **Amends, once built:** ADR-0017 sections 1, 6 and 7, with a dated
  note.

## Context

A recording is written by the app's `SessionRecorder` on the GUI
thread (ADR-0017 section 1). Three things follow from where it lives:

- **It records what the bridge accepts, not what the bus carried.**
  Frames the bridge drops under backpressure
  ([ADR-0003](0003-backpressure-bounds-in-flight-frames.md)) are only
  counted in the summary. A recording is the evidence a session sheet
  rests on, and the one place it can lose frames is the one place
  nothing needs it to.
- **`havne-cli` can't record.** The CLI is a first-class front end and
  the one that runs long sessions (a DID sweep, the phone's relay, a
  script on a laptop at the car), yet its only output is JSON lines on
  stdout, in another shape from the folder the app writes.
- **Reading one back is replay alone.** `replay://` plays `frames.csv`
  at its own pace or faster ([ADR-0023](0023-replay-transport.md)),
  which is all a viewer can't use: seeking, zooming, an overview of an
  hour, two runs side by side. v0.4.0's log viewer needs a reader that
  opens a recording offline and answers "this signal between these two
  times, at this many columns" without holding the recording in memory.

ADR-0017 kept the recorder out of core because file I/O would sit on
the worker, whose callbacks must not block, and because values and link
transitions would need ABI. Both have since changed shape: values are
decoded on the worker (ADR-0024), and a writer thread of its own keeps
I/O off the worker.

## Decision

### 1. A writer thread in core, fed by the worker

A recording belongs to a session. The caller asks for one after create
and before start, so it brackets the whole session:

```c
ecuio_status_t ecuio_session_record(ecuio_session_t* session,
                                    const ecuio_record_options_t* options);
```

- **The worker** stamps each frame `host_us` (ADR-0017's column, core's
  clock) as dispatch takes it, before any callback, and puts a compact
  row into a bounded ring: the frame's times, ID, flags, length and its
  used bytes, never the whole 4 KB frame. Each value
  `decode_values` produces (ADR-0024) goes in after its frame: the def's
  index and the number, or the text. Dispatch decodes while a session
  records, not only while a values callback is registered, so a
  `havne-cli` session that prints raw frames still records its values.
  Nothing is formatted and nothing waits on the worker.
- **The writer thread** drains the ring, formats the CSV rows exactly as
  ADR-0017 has them, and flushes every 100 ms, so a crash loses at most
  the last tenth of a second. Every 10 s of session time it reads the
  session's reply timing and appends `latency.csv`'s changed rows
  (ADR-0017's appendix of 2026-10-31), so that file moves into core too.
- **A full ring drops rows, never blocks.** The worker counts what it
  couldn't queue; the writer writes the count into `events.log` where
  it happened ("the recording dropped 312 frames: the disk fell
  behind") and in the summary. The ring holds several seconds of a
  saturated 500 kbit/s bus (about 4,000 frames a second), so on a live
  session a drop means the storage stalled, and it says so; a replay at
  `speed=max` can outrun any disk, and its drops are counted the same
  way.
- **Close** joins the worker first, then lets the writer drain the
  ring, write the summary (frames, values, rows dropped by the
  recording) and close the files. Nothing queued is lost at a normal
  close.

### 2. One folder, named by core

`options` names a root folder and an optional label; core creates
`<root>/<yyyyMMdd-HHmmss>[-n][ <label>]/` as the app does now (ADR-0027
section 2's label rules stay the caller's: it passes a name a folder
may take). The app and `havne-cli stream --record <root>` then write
the same folder with the same files and header rows:
`frames.csv`, `signals.csv`, `events.log`, `latency.csv` when the
session times requests, and `markers.csv` (section 4) when anything is
marked.

`events.log`'s header is core's: a fixed block of `key  value` lines
(the start in local time and in UTC to the millisecond, core's
version, the URI, the profile and its signals), then any lines the
caller adds in `options` (the app's surrogate frame-logging state, the
picked vehicle). The UTC start is what lines two recordings up on the
wall clock (section 5).

The caller's activity lines go in as notes:

```c
ecuio_status_t ecuio_session_record_note(ecuio_session_t* session,
                                         const char* utf8_line);
```

safe from any thread, stamped when called, queued like a frame. The
app passes its activity log's lines, link transitions included;
`havne-cli` passes its health and end lines.

### 3. Failures are reported, never silent

- **Can't start:** `ecuio_session_record` returns a new status,
  `ECUIO_ERR_RECORDING_FAILED` (18, appended), with a detail that names
  the cause and not the path, since the path holds the account name
  ("can't create the recording's folder: Access is denied"). The
  session itself is unaffected: the caller decides whether to go on
  unrecorded, as the app does now.
- **A write fails** (a full disk, a removed card): the writer stops
  writing, keeps draining the ring so the worker never fills it, and
  publishes the state. `ecuio_session_get_recording` reports it:
  `ECUIO_RECORDING_ACTIVE`, `ECUIO_RECORDING_FAILED` with the OS's
  reason, the rows written, the rows dropped and the folder (UTF-8,
  absolute, for the caller to show relative as ADR-0017 section 3
  does). The app shows "Recording stopped: the disk is full", the CLI
  writes a `recording` line.

### 4. Markers

```c
ecuio_status_t ecuio_session_record_mark(ecuio_session_t* session,
                                         const char* utf8_label);
```

writes `host_us,label` to `markers.csv`, created by the first mark: the
Mark button in the app, a line typed into `havne-cli stream --record`.
The viewer aligns runs on them (section 5).

### 5. The reader

```c
ecuio_status_t ecuio_recording_open(const char* folder_utf8,
                                    ecuio_recording_t** out);
void ecuio_recording_close(ecuio_recording_t* r);
```

- **One pass at open** reads `signals.csv` (the decoded values: the
  viewer's source in v0.4.0; `frames.csv` through a profile's decode
  comes later) and builds, per signal, its name, unit, count and range,
  and a summary per second of `host_us` in which it has samples: the
  lowest, highest and last value and the count (the last text, for a
  text signal). Beside it, a sparse index: each second's byte offset in
  the file.
- **A range query** answers "signal S from t0 to t1 in N columns" with
  each column's lowest and highest. Where a column spans a second or
  more it combines the summaries; zoomed closer, it reads the raw rows
  of just those seconds through the offset index. Either way the answer
  is the same min/max-per-column shape, about twice the chart's width
  in points, which Live's chart already draws.
- **The memory bound,** stated and tested: about 24 bytes per signal
  for each second it has samples, plus 16 bytes per second for the
  index. An hour of the `obd2` set on the test car is at most about
  4.4 MB (about fifty signals, if each had a sample every second),
  whatever the file's size. The done-when test opens an hour-long recording and
  checks the reader's own count of what it holds against the bound.
- **Alignment:** `ecuio_recording_find` returns the first time a signal
  crosses a threshold in a direction, with hysteresis (rpm above 3000
  after being below 2900), or a marker's time; the viewer and
  `havne-cli view --align-on` share it. Two recordings also line up on
  their UTC starts (section 2), to about a millisecond when one
  machine's clock stamped both.
- **Untrusted input:** a recording comes from anywhere. Every row is
  bounds-checked, a bad row is refused with its line number (ADR-0023's
  rule), never its path, and the `signals.csv` parser gets a fuzz
  target.

### 6. A torn last row is dropped, not refused

A crash or a pulled cable can leave the last line without its newline.
The reader (and `replay://`, through the same `frames_csv.h`) drops a
final line that has no newline and counts it; any other bad row is
still refused with its line number. A recording that stopped hard
stays readable.

### 7. Compatibility

- **The files don't change.** Same names, columns and header rows, so
  every recording made so far opens, and `replay://` plays a recording
  core wrote as it plays one the app wrote.
  `BridgeTest.ReplayOfAMockRecordingDecodesWhatWasRecorded` keeps
  holding the two together, now with core on both sides.
- **The container stays deferred.** One file holding the folder's
  contents (MDF4, a SQLite file, an archive) waits for listening to the
  whole bus (v0.8.0), where frame rates make CSV's size matter.
- **The app's recorder shrinks** to calling these functions: Settings'
  switch, the folder shown relative to Documents, Open folder and the
  frame-logging environment variable (ADR-0017 section 6) stay in the
  app.

### 8. The viewer's chart: measured, then chosen

The viewer draws min/max columns like Live, so Live's `Canvas` chart
may be enough; Qt Graphs draws on the GPU and handled 200,000 points
in a trial (2026-11-02), but adds three Qt modules to every platform's
download. The choice is measured, not assumed: the Canvas at viewer
sizes, panning and pinching at 60 frames a second on the test phone
and the laptop. If it holds, it stays; if not, Qt Graphs, and an
appendix records the numbers either way.

## Alternatives considered

- **Keep the recorder in the app.** No ABI, but it still loses the
  frames the bridge drops, and `havne-cli` would need a second writer
  in another shape, two implementations of one format that the
  round-trip test would have to hold together twice.
- **Format rows on the worker.** Simpler (no compact rows), but the
  formatting is the costly part, and the worker's budget is the
  bus's.
- **Block the worker when the ring is full.** Nothing is ever dropped,
  but a slow disk then stalls the bus reads and the request rotation,
  which is worse evidence than a counted drop.
- **SQLite as the recording.** Indexed queries for free, but a new
  dependency in core, and a recording stops being a folder a
  spreadsheet opens. Kept in view for the container.
- **Index every row.** Exact, but memory grows with the file; a
  per-second summary plus raw reads near the cursor gives the same
  picture at the screen's resolution.

## Consequences

- A recording holds every frame the transport delivered and every
  value core decoded, whatever the front end did with them; the
  summary's "frames dropped by the bridge" goes, and a recording that
  lost rows says where and why.
- `havne-cli stream --record` writes the folder the app writes, so a
  laptop session, a phone relay session and an app session all replay,
  view and attach to session sheets the same way.
- The worker does a little more per frame: a copy of the used bytes
  into the ring, only while recording. The load test gates it (the
  replay fixture through a recording session, the writer's rate and
  any rows dropped).
- New ABI: `ecuio/recording.h` (the writer's four functions, the
  reader's, their structs with `flags` and `reserved`),
  `ECUIO_ERR_RECORDING_FAILED`. Strict C, `test_c_abi`.
- `host_us` is still when a frame reached dispatch, not when it crossed
  the wire; `timestamp_us` keeps the transport's time.
- An hour-long recording opens in one pass over `signals.csv`; the time
  that takes is measured in the done-when test and stated with the
  memory bound.
