# ADR-0023: Replay transport — `replay://` plays a recording's `frames.csv` back through the frame API

- **Status:** Accepted.
- **Date:** 2026-10-25. Implemented with this record.
- **Builds on:** [ADR-0007](0007-uri-selected-transports.md) (a
  transport per URI scheme, failures at create);
  [ADR-0013](0013-protocol-profiles.md) (the profile decides the
  decode); [ADR-0017](0017-session-recording.md) (`frames.csv` is the
  replay input, its header row the format's version, and a round-trip
  test is part of this work); [ADR-0018](0018-connect-failure-reporting.md)
  (a refused connect says why).

## Context

The vehicle is the costly tier. The two sessions so far
(docs/bench-sessions/2026-10-22-vehicle.md and 2026-10-25-vehicle.md)
took a car, a driveway and a laptop battery, and left recordings
behind: every frame the bridge received, with the time it arrived and
the time the transport gave it. Nothing can play them back. A change
to a decoder, a profile or a page is checked against the mock's
synthetic drive cycle or the fake DLL's canned replies, never against
the bytes a real ECU sent.

Three needs meet here:

- **Regression evidence from a real bus.** The `obd2` set decoded the
  CX-5's replies on both dates; a test that replays them and checks the
  values keeps that true without the car.
- **A load source.** Decoding on the worker (STATUS.md, backlog
  "Full signal sets at full rate"; Known Issue #4, and per-sample
  time on the chart) needs real
  traffic at more than real time, with frames that carry their own
  times. `bench_bridge` measured the GUI thread's match against every
  def ([testing.md](../testing.md#performance)); the worker's per-frame
  copy and the tick's per-row update are unmeasured, and only a
  sustained stream can show which of them costs most.
- **Demonstration.** Showing what QtTune does on a car, without one.

What a recording holds sets the limits. `frames.csv` has the frames the
bridge accepted: replies and broadcast traffic, never the requests
that drew them (those are only in the surrogate's session log), and
none of the frames the bridge dropped. Its columns are
`host_us,timestamp_us,id,flags,data`. `timestamp_us` is session time
in every recording since ADR-0017's appendix of 2026-10-22; the two
sessions of that date hold the adapter's raw clock there instead, which
counts from the adapter's power-up (2,634 s at the first frame of one).
Both are monotonic in every recording on hand, and both spans match
`host_us` to within milliseconds.

## Decision

### 1. A transport in core, on every platform

`replay://` is a third scheme beside `mock://` and `j2534://`, in core
(`core/src/replay_transport.cpp`), as ADR-0017 placed it. It has no
platform code beyond opening a UTF-8 path (the W call on Windows), so
it builds and runs wherever core does: the Qt-free CI jobs test it,
and it is the second transport the mobile apps can open.

### 2. The URI

```
replay://<path>[?profile=<name>][&did=…][&speed=<x>|max][&loop=1]
```

- **`<path>`** is a recording's folder or its `frames.csv`; a folder
  means the `frames.csv` inside it. It must be absolute, as a
  direct-load J2534 path must (`ecuio_is_absolute_path`, so a drive or
  UNC path on Windows, and `/` elsewhere), or create refuses it with
  `ECUIO_ERR_BAD_URI`.
- **`profile`** and **`did`** are core's, as for every scheme. With no
  `?profile=` a replay decodes with **`raw`**: it decodes nothing and
  can't decode wrongly, and every consumer so far names its profile (a
  test, or whoever made the recording). The recording's `events.log`
  names the profile it ran under, but `events.log` is a human log with
  no format contract, and reading it would give it one. The intended
  evolution is the recorder writing the profile into `frames.csv`'s
  header row, a new format version; the reader enforces the header, so
  that change carries its writer and reader together.
- **`speed`**: a factor from 0.1 to 100, default 1; `max` sends every
  frame as soon as the last is dispatched, with no pacing (tests, load).
- **`loop=1`** plays the file again from the top when it ends.

Unknown keys and malformed values fail at create, before the file is
opened, as `j2534://`'s do (`ecuio_replay_query_keys`, rejection tests
for each malformed form).

### 3. Attach checks the whole file, the worker streams it

Attach opens the file, accepts only the header row ADR-0017 defined
(any other is `ECUIO_ERR_LOAD_FAILED`, "not a QtTune frames.csv, or a
newer format"), then reads every row once: five fields, a decimal
`host_us` and `timestamp_us`, `0x`-hex `id` and `flags`, the data as
spaced hex pairs, at most `ECUIO_FRAME_DATA_MAX` bytes. A bad row
fails create with `LOAD_FAILED` and its line number; so does a file
that can't be opened or holds no frames. Either line ending is
accepted (ADR-0017: the writer's lines end in CRLF on Windows). Attach
keeps the row count and the span for the session info, and nothing
else: the worker reads the file again from the top, a row at a time,
so memory doesn't grow with the recording. A passive bus recorded for
an hour can be millions of rows.

### 4. Time: paced and stamped by the recording's session time

The worker sends row *i* when `(t_i − t_0) / speed` has passed since
the session started, sleeping to a deadline as the mock's worker does,
so Windows' 15.6 ms sleep step doesn't stretch the recording. `t` is
the row's `timestamp_us`; a row whose `timestamp_us` steps backwards
(a raw adapter clock wrapping) uses its `host_us` step instead, as
`j2534_clock.h` stands the host in for a vendor clock that misbehaves.

A frame's own `timestamp_us` is the recording's session time, rebased
so the first row is 0 and pushed on by the file's span on each loop,
so it never decreases. Under `speed` other than 1 it runs at the
recording's pace, not the wall clock's: a frame replayed at 10× still
says when it crossed the car's bus. That is what makes a replay the
load test for per-sample time on the chart. `id`,
`flags` and `data` go out as recorded, the `TRUNCATED`, `EXTENDED` and
`FD` flags included.

### 5. What a replay session is, and isn't

- **Passive.** No requests, so no request engine, probe, scan or
  identification; the channel-support and identity reports are absent
  (`INVALID_ARGUMENT`, as for mock), so the activity log has no ECU
  lines. A recorded J2534 session's replies still decode under `obd2`
  or `uds-generic`, because their defs match replies by ID and mux key,
  not by request.
- **No link health.** The error state stays `ECUIO_OK`, unless the file
  changes under the replay (appendix of 2026-10-26).
- **Session info** gains `ECUIO_BACKEND_REPLAY` (append-only) and three
  fields from its reserved words, `replay_frames` (the file's rows),
  `replay_span_ms` and `replay_speed_milli` (1000 is real time, 0 is
  `max`), with `ECUIO_SESSION_INFO_REPLAY_LOOP` among the flags under
  `loop=1`. The struct keeps its size; a reader built before them sees
  zeros. The bridge's "Session:" line reads "Session: replay of a
  recording, N frames over T s at Nx[, looping]; profile P" (`at full
  speed` for `max`); the path is in the "Connected:" line before it.
- **The bridge takes frames from the moment the session starts.** Its
  callback was gated on the connect being applied, which comes after
  `ecuio_session_start`, so frames sent in between were dropped
  uncounted. A replay sends its first frame the instant it starts, and
  at `max` the whole file can go in that window, so the callback's gate
  opens just before the start; the GUI-thread half still waits for the
  apply, and the frames queued meanwhile reach it after.
- **The end of the file.** Without `loop`, the worker stops sending and
  the session stays open, connected and silent. The bridge logs it
  once, on the coalesce tick that sees the frames it took plus those it
  dropped reach `replay_frames`: "Replay finished: N frames; the
  session stays open". No link-health code: `ECUIO_ERR_VEHICLE_TIMEOUT`
  would show the quiet banner, whose "check the ignition" is wrong for
  a file, and a "replay finished" state is a new code with UI cases in
  every page. Codes are append-only, so one can still be added if the
  log line proves too quiet.
- **The path stays off screen.** `uriLabel` cuts a `replay://` path to
  its last component (the recording's folder name, or `…/frames.csv`
  with it) as it cuts a direct-load DLL path, so a screenshot shows no
  account name.

### 6. Tests and fixtures

- **Core (`test_core`, Qt-free):** the reader against hand-written
  files (the header, each malformed field with its line number, CRLF
  and LF, a 4,096-byte payload, an empty file), pacing bounds at `speed`
  1 and 10, the backward-step fallback, loop stamps that never
  decrease, and every query rejection.
- **The round trip (`test_bridge`):** record a `mock://` session with
  `SessionRecorder`, replay its folder under `?profile=mock&speed=max`,
  and compare every decoded value with the recording's `signals.csv`.
  This is ADR-0017's promised test, and it holds the writer in the app
  and the reader in core to one format.
- **Real-bus fixture:** the second vehicle session's engine start (its
  Phase 4 connect, 297 s, 7,074 frames) committed as
  `tests/fixtures/replay/2026-10-25-engine-start/frames.csv`, scrubbed
  under the bench-sessions rules before it's committed: every
  identification reply (`49 02/04/06/0A`, `62 F1 xx`, six rows) removed,
  nothing else changed. A test replays it under `obd2` at `max`, checks
  the sheet's values (RPM peaking at 1,261, the module voltage's 9.56 V
  dip at the starter, coolant 60 to 86 °C), and fails if the fixture
  holds an identification reply. Only the scrubbed copy is committed;
  the raw recordings stay in `Testing/bench-raw/`.

## Planning note: decoding on the worker

Not decided here. When the decode moves to the worker, it gets its own
ADR: it is an ABI addition with its own text-value question, and it
should be citable apart from this transport. That ADR leads with
per-sample time on the chart, the reason a user sees; then
a decode every front end shares, and values that survive frames
dropped under load. It cites `bench_bridge`'s result that the GUI
thread's matching is no CPU problem at classic-CAN rates, so it isn't
justified on CPU.

Replay supplies its load test: the real-bus fixture at `speed=10` and
`max`, through the bridge, measuring the worker's 4 KB copy per frame,
the tick's per-row update and the GUI drain together, before and after
the change. Those numbers go in that ADR.

## Alternatives considered

- **Replay in the app,** the bridge reading the file and feeding its
  own frame path. Smaller, but the frame API is core's: another front
  end would get nothing, and the reader couldn't be tested in the
  Qt-free CI jobs.
- **A mode of the mock (`mock://?file=`).** It would share a worker
  loop, but the mock's query and its drive cycle mean "synthetic", and
  a recording's session info, path rules and fixtures differ.
- **Load the file into memory at attach.** Simpler pacing, but memory
  grows with the recording. Checking at attach and streaming in the
  worker keeps create-time errors and constant memory, for one extra
  read of the file.
- **Pace by `host_us`.** Always monotonic, but it's when a frame reached
  the bridge, with the queue's jitter in it; `timestamp_us` is the
  wire-side time and the one a frame should carry.
- **Answer the request engine from the recording** (a recorded ECU: a
  request finds its recorded reply). It would exercise the engine, the
  probe and identification on real replies, but the recordings hold no
  requests to pair with, and the fake DLL's VX mode already covers the
  engine's timing. A possible later step, with request capture added
  to the recorder first.

## Consequences

- A real ECU's replies become a regression test, run in every CI job
  that builds core.
- The load test for decoding on the worker exists before that change
  is made.
- A recorded session can be shown and inspected anywhere core runs,
  mobile included, with no adapter.
- What a replay can't show: anything the transport did (requests,
  pacing, link health, the probe's drops), frames the bridge dropped
  while recording, and the adapter's session log.
- C ABI additions: `ECUIO_BACKEND_REPLAY`, a flag and three
  session-info fields taken from its reserved words, and one status
  code, `ECUIO_ERR_RECORDING_CHANGED` (appendix of 2026-10-26); no new
  function.
- A committed fixture is real-car data, so its scrub rule is part of
  this decision, and a fixture added later follows it.
- `frames.csv`'s header row now has a reader that enforces it: a change
  of columns in the recorder needs a reader change in the same commit,
  which the round-trip test catches.

- Settings takes a replay as a typed URI. A "Replay a recording…" row
  with a folder picker would bring Qt Quick Dialogs, a new Qt module
  for the app, for one convenience that no test needs; deferred.

## Appendix — a replay that stops early (2026-10-26)

Attach checks the whole file, but the worker reads it again, and it can
change in between: a recording copied while still being written, a file
regenerated or deleted, a share that drops. The worker then stopped at
the row it couldn't read, or at a file that wouldn't open again, and
nothing said so: the frame count fell short of `replay_frames`, so
"Replay finished" never came, and the session sat connected and silent.

**Decision.** The worker publishes a new status code through the error
state, `ECUIO_ERR_RECORDING_CHANGED` (16, appended), with one episode,
and sends nothing more; the session stays open, as at the end of the
file. A clean end of the file still leaves `ECUIO_OK`. The bridge maps
the code to a link state of its own, `stopped` (amber, "Replay
stopped" on the header pill, Home and the Dashboard; Live's banner
"Replay stopped — the recording changed after it was opened"), and
logs once "Replay stopped after N frames: the recording changed after
it was opened". The link-health codes already there were the wrong
words: `fault` names an adapter, `quiet` says to check the ignition.

The log line gives the frames sent, not the frames.csv line: the line
is in the worker thread's error detail, which the bridge can't read,
and carrying it would need a session-info field the worker writes
after attach, where today the info is written once. For a replay
without `loop` that stopped at a bad row, the line is the count plus 2.

The worker opens the file shared for reading and writing on Windows
(`_wfsopen`, `_SH_DENYNO`), as `fopen` does elsewhere. `_wfopen_s` opened
it unshared, so playing a recording locked it against being copied
over or regenerated, and the case this appendix handles could happen
on Windows only between two passes of a loop.

Tests: `CoreTest.ReplayStopsAtARowThatTurnedBadAfterAttach` (the code
and one episode), `.ReplayOfARecordingThatWentAwaySaysSo`, the end of
the file leaving `ECUIO_OK` in `.ReplaySendsTheRecordedFramesAtTheirRebasedTimes`;
`BridgeTest.ReplayThatStopsEarlySaysSo` (a looping replay whose file is
rewritten: the state, the code, the episode, one line, no "Replay
finished") and `.LinkStateMapsEveryPublishedCode`; `QmlTest.theme` and
`.liveview` for the label, color and banner.

**Note (2026-10-26): a loop needs a span.** A recording whose frames
all share one time (one frame, or several stamped alike) gives a pass
that never waits, so with `loop=1` the worker would send the same
instant again and again, as fast as it can. Create refuses it
(`BAD_URI`, "?loop=1 needs a recording that spans time; this one's
frames all share one time"); attach already knows the span from its
check of the file. Played once, such a recording plays.
`CoreTest.ReplayRefusesToLoopARecordingWithNoSpan`.
