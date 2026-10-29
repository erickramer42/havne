# ADR-0017: Session recording — one folder per connect, frames.csv as the replay format

- **Status:** Accepted.
- **Date:** 2026-10-22. Implemented with this record.
- **Related:** [ADR-0003](0003-backpressure-bounds-in-flight-frames.md)
  (what the bridge drops), [ADR-0013](0013-protocol-profiles.md)
  (signal sets), and the record-replay transport,
  [ADR-0023](0023-replay-transport.md).

## Context

The first vehicle session is the first talking bus, and it is costly
to repeat: a car, its ignition states, a second person for the
engine-on phase. Before this record its evidence was the surrogate
session log, screenshots and notes. The session log, with
`ECUIO_SUR_LOG_FRAMES=1`, holds every request and reply with its
status, but only on the surrogate path, stamped to the millisecond,
and with no decoded values. The Log page holds at most 10,000 frames,
keeps frames out while paused, and holds no values.

So nothing machine-readable recorded what QtTune decoded, to compare
against a reference reading or to measure each signal's refresh. BENCH
prep item 5 (timestamp units) needs the vendor's timestamps against a
host clock over many frames. And Phase 4's replay transport needs
fixtures from a real bus, in a raw format STATUS asked to be defined
once.

## Decision

1. **The recorder lives in the app layer.** The bridge owns a
   `SessionRecorder` (`app/sessionrecorder.{h,cpp}`, Qt Core only, so
   `test_bridge` covers it). It sees what already meets on the GUI
   thread: every frame the bridge accepts, each decoded value, and the
   activity lines, link transitions included. Core and the ABI are
   unchanged.
2. **Opt-in, and always a whole session.** A Settings switch, off by
   default and not saved between runs (there are no persisted
   preferences yet). It can't change while a session is up, connecting
   or disconnecting. A recording opens as the connect is applied,
   before any frame is accepted, and closes when the disconnect's
   teardown completes, after `Disconnected:`, so it brackets the
   teardown too.
3. **Where.** `Documents/QtTune/recordings/<yyyyMMdd-HHmmss>/`, one
   folder per connect, with `-2`, `-3` and so on if the name is taken.
   Settings shows the folder relative to Documents, so no account name
   reaches the screen or a screenshot, and offers Open folder.
4. **Three files.**
   - `frames.csv`: `host_us,timestamp_us,id,flags,data`. `host_us` is
     microseconds since the recording started, from the steady clock,
     stamped on the worker thread as the frame reaches the bridge's
     callback. `timestamp_us` is the frame's as delivered (on J2534,
     the vendor's raw stamp). `id` and `flags` (`ECUIO_FRAME_FLAG_*`)
     are hex with `0x`; `data` is the payload as spaced uppercase hex.
     ASCII. Every accepted frame is written, whether or not the Log
     page is paused.
   - `signals.csv`: `host_us,signal,value,unit`, one row per decoded
     value, stamped with its frame's `host_us`, the value to 12
     significant digits. UTF-8 with a byte-order mark, so a spreadsheet
     reads "°C" correctly.
   - `events.log`: a header (start time, core version, URI, the signal
     set's names, the frame-logging state), then each activity line
     with its local time and seconds since the start, then a summary
     (frames, values, and frames the bridge dropped).
5. **`frames.csv` is the Phase 4 replay input.** Its header row is the
   format's version: a reader accepts only header rows it knows, and a
   change of columns is a new header row. The replay transport, in
   core, reads it, and its work includes a round-trip test: record a
   mock session, replay it, compare the decoded values.
6. **Frame logging comes with it.** With recording on, the bridge sets
   `ECUIO_SUR_LOG_FRAMES=1` before a connect spawns any surrogate,
   unless the variable was set at launch, and removes its own setting
   at the next connect without recording. The session log then holds
   what the recording can't see: the requests, write statuses and
   `ReadMsgs` statuses.
7. **Failures don't stop the session.** A folder or file that can't be
   created logs `Recording failed: …` and the session runs unrecorded.
   The files are flushed on every 100 ms coalesce tick; a write
   failure stops the recording at that tick with `Recording stopped: …`.

## Alternatives considered

- **Export or copy from the Log page.** The table is bounded, misses paused frames, holds no
  values or events, and has to be exported before the next connect
  clears it. Copying selected rows may still be worth adding; it isn't
  evidence capture.
- **A recorder in core,** writing from the worker thread. It would see
  frames the bridge drops under backpressure and would serve any front
  end. But it puts file I/O on the worker, whose callbacks must not
  block; it needs new ABI; and decoded values and link transitions
  would need more ABI still. Worth revisiting with the replay
  transport, which is core.
- **One file with a kind column.** Awkward in a spreadsheet, and the
  three streams have different shapes.
- **JSON lines or a binary format.** Not spreadsheet-readable. A
  Mode-01 poll delivers about ten frames a second, so CSV's size is
  no concern.

## Consequences

- The vehicle session's capture can become the first real-bus replay
  fixture.
- BENCH prep item 5 and the 32-bit timestamp wrap can be settled
  offline, by fitting `timestamp_us` against `host_us`.
- Each signal's refresh interval, and a comparison against a reference
  reading, come straight from `signals.csv`.
- In-proc sessions, which have no session log, get their received
  frames recorded, but not their requests.
- Not recorded: frames the bridge drops under backpressure (counted in
  the summary), frames before the connect is applied, and requests and
  write statuses (the session log's job).
- `host_us` is when a frame reached the bridge, not when it crossed the
  wire; the vendor's `timestamp_us` is the wire-side time.
- One format has two implementations, a writer in the app and a reader
  in core, held together by the header row and the round-trip test.
- `events.log` records the URI, and a direct-load URI holds a local
  path, so a recording is scrubbed like a session log before it's
  committed.
- The files are written in text mode, so lines end in CRLF on Windows;
  a reader accepts either ending.

## Appendix — J2534 `timestamp_us` is session time (2026-10-22)

A J2534 frame's `timestamp_us` became session time: the host's at the
first frame, then the adapter's own deltas, carried across its 32-bit
wrap ([ADR-0006](0006-frame-abi-v0.3.md)'s appendix of the same date).
So in `frames.csv` it now means what it means for `mock://`, and the
columns are unchanged. J2534 recordings made before then (the bench
and vehicle sessions of 2026-10-22) hold the vendor's raw stamp in
that column.

The vendor's clock can still be fitted against the host's, as the
consequences above describe, from the session log instead: its
`OP_READ frame` lines keep the raw stamp, indications included, next
to their wall-clock times. That is how the vehicle session showed the
VX counts microseconds (STATUS.md, BENCH prep item 5).

## Appendix — text values (2026-10-23)

A text signal ([ADR-0016](0016-signal-definition-layout.md), appendix)
goes into `signals.csv` like a number: one row per decoded value, the
text in the `value` column, CSV-quoted when it holds a comma, a quote
or a newline (a HEX value never does; a TEXT value shows such bytes as
`.`). The columns and the header row are unchanged.

## Appendix — the reader exists (2026-10-25)

`replay://` ([ADR-0023](0023-replay-transport.md)) is the reader point
5 promised. It accepts only the header row above and refuses a bad row
with its line number, and the round trip it describes is a test:
`BridgeTest.ReplayOfAMockRecordingDecodesWhatWasRecorded` records a mock
session, replays it, and compares the two recordings row for row and
value for value. A change of columns in the recorder now needs the
reader changed in the same commit.

The bridge's frame callback opens just before the session starts
rather than when the connect is applied, so frames sent in between
are recorded too; the consequence above, "not recorded: frames before
the connect is applied", now covers only frames before the start,
which a transport doesn't send.

## Appendix — `host_us` counts sleep (2026-10-29)

`host_us` is microseconds since the recording started on core's clock
(`ecuio_clock_us`, [ADR-0033](0033-session-time-counts-sleep.md)),
which counts the time the machine sleeps on every platform. The steady
clock it was taken from doesn't on Linux and Android, so a sleep there
left no gap in `host_us`. The format is unchanged; a sleep is a gap in
`host_us` as in `timestamp_us`, and `events.log` names it ("Asleep for
N s").
