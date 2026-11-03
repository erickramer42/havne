# ADR-0049: Recordings in core: a shared writer and an indexed reader

- **Status:** Accepted.
- **Date:** 2026-11-02.
- **Builds on:** [ADR-0017](0017-session-recording.md) (the recording
  folder and its files, whose "recorder in core" alternative this
  takes up); [ADR-0023](0023-replay-transport.md) (`replay://`, the
  reader of `frames.csv`); [ADR-0024](0024-decode-on-the-worker.md)
  (values decoded on the worker); [ADR-0027](0027-vehicle-profiles.md)
  (the folder's label); [ADR-0031](0031-command-line-client.md)
  (havne-cli); [ADR-0032](0032-socketcan-listening.md) (listening to a
  whole bus, CAN FD included); [ADR-0042](0042-reply-timing.md)
  (`latency.csv`).
- **Amends, once built:** ADR-0017 decision items 1 (where the
  recorder lives), 3 (who names the folder), 4 (`events.log`'s header
  and summary) and 7 (how failures are reported), and its consequence
  "one format has two implementations"; ADR-0023's rule that a bad row
  is refused (section 7); ADR-0024 section 1's rule that a session
  decodes only while a values callback is registered (section 1);
  ADR-0027 section 2's label rule, which moves into core; ADR-0039
  section 6's rule that absent signals are judged only while a values
  callback is registered, which follows the decode (section 1);
  ADR-0016 section 8's validator codes, which gain one for a name or
  unit a spreadsheet would run as a formula (section 8); and ADR-0017's
  promise that the folder opens in a spreadsheet, which the export
  takes over (section 8). Each with a dated note.

## Context

A recording is written by the app's `SessionRecorder` on the GUI
thread (ADR-0017 item 1). Three things follow from where it lives:

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
  times, at this many columns" without holding the recording in memory,
  and `havne-cli info` needs what's wrong with a recording's frames
  (gaps, overflows, broken ISO-TP sequences) without replaying it.

ADR-0017 kept the recorder out of core because file I/O would sit on
the worker, whose callbacks must not block, and because values and link
transitions would need ABI. Values are decoded on the worker
(ADR-0024), a writer thread of its own keeps I/O off the worker, and
core writes link transitions itself (section 2).

## Decision

In brief: a recording is a handle of its own in core
(`ecuio_recorder_t`), opened by the front end and attached to a
session before it starts. The session's worker copies each frame and
its decoded values into a ring without locking, and a writer thread
drains the ring into the same folder and files ADR-0017 defines,
dropping and counting records rather than ever blocking the worker. The
recording outlives its session, so it brackets the teardown, and core
writes its own events in a fixed form. A reader opens a recording
offline with memory bounded by its length, answers min/max-per-column
queries for a chart, and finds alignment points; a frames check streams
`frames.csv` for what's wrong with it. The app and `havne-cli` use the
same functions, so they write and read the same recordings.

### 1. A recorder handle, a writer thread, fed by the worker

A recording is a handle of its own, because it outlives its session:
the app's teardown logs its last lines (reply timing's totals,
`Disconnected:`) after `ecuio_session_destroy`, and ADR-0017 item 2
has the recording bracket the whole teardown.

```c
ecuio_status_t ecuio_recorder_open(const ecuio_record_options_t* options,
                                   ecuio_recorder_t** out);
ecuio_status_t ecuio_session_record(ecuio_session_t* session,
                                    ecuio_recorder_t* recorder);
ecuio_status_t ecuio_recorder_close(ecuio_recorder_t* recorder,
                                    const char* utf8_why);
```

- **Open** creates the folder (section 2) and starts the writer
  thread. Nothing is written until a session attaches. The caller
  opens it once `ecuio_session_create` has succeeded, so a connect
  that fails at create leaves no folder. One that fails at
  `ecuio_session_start` leaves a folder with the header and the
  caller's notes and no frames, which is what says why it failed.
- **Attach** comes after `ecuio_session_create` and before
  `ecuio_session_start`, so the recording holds the session's every
  frame. It copies each def's name, unit and kind into the recorder's
  own table, so the writer never reads the session's set, which
  `ecuio_session_destroy` frees while the writer may still be draining.
  It writes `events.log`'s header from the session (section 2) and sets
  the recording's zero: `host_us` counts from this call on
  core's clock (`ecuio_clock_us`, which counts sleep, ADR-0017's
  appendix of 2026-10-29), and the UTC start is read beside it, the
  two calls back to back. A started session, a recorder that already
  has a session, or a second recorder on one session is
  `ECUIO_ERR_INVALID_ARGUMENT`. One recorder holds one session, as
  ADR-0017 has one folder per connect, and the user can't stop a
  recording mid-session (ADR-0017 item 2's switch stays locked while a
  session is up).
- **The worker** stamps each frame `host_us` as dispatch takes it,
  before any callback (ADR-0017 stamps it at the bridge's callback,
  a few microseconds later), and puts one record into a ring: the
  frame's times, ID, flags, length and its used bytes, never the whole
  4 KB frame, followed by each value `decode_values` produces
  (ADR-0024): the def's index in the recorder's table and the number,
  or the text's length and bytes. A frame and its values go in as one
  record or not at all, so `frames.csv` and `signals.csv` never
  disagree about a frame. The ring has one producer because a
  session's frames are dispatched from one thread, its transport's
  worker. Every transport does that, and it becomes part of the
  transport contract: a transport that dispatched from two threads
  would need a ring per thread. Like the rest of the worker's path, the
  ring write allocates nothing (`CoreTest.WorkersAllocateNothingPerFrame`).
  The worker that publishes link health
  (`ecuio_session_get_error_state`) also queues a link record at each
  transition, its state and episode count, so `events.log`'s `link`
  lines (section 2) have the transition's exact time and sit in order
  with the frames. It queues reply timing the same way: a snapshot
  every 10 s of session time, and the final one as its last record
  before it ends (section 1's writer). The writer never holds the
  session: everything it writes reaches it through the ring or was
  copied at attach, so the session can be destroyed whenever the
  caller likes.
  Dispatch decodes while a session records, not only while a values
  callback is registered (the amendment of ADR-0024 section 1), so a
  `havne-cli` session that prints raw frames still records its values.
  ADR-0039 section 6's absent-signal judgment runs inside the decode,
  so it follows it: a recording session judges absent signals too,
  which costs nothing a caller doesn't ask for.
  Nothing is formatted, nothing locks and nothing signals the writer:
  the worker only writes the ring and advances its index.
- **The ring** is single-producer (the worker), single-consumer (the
  writer), and sized in bytes: `options` sets it, 4 MiB by default. By
  estimate, a classic 8-byte frame's record is some 40 bytes with its
  values, and a 64-byte CAN FD frame's some 100; step 1 measures them
  and records them in an appendix. On those figures the default holds
  over 20 s of a
  saturated classic 500 kbit/s bus (about 4,000 frames a second), and
  about 7 s of a saturated FD bus at a 5 Mbit/s data phase (about 5,500
  frames a second, listened to raw, so no values). At those rates a
  drop means the storage stalled for that long; a whole-bus FD listener
  that wants a longer margin raises the size through `options`.
- **The writer thread** wakes every 10 ms while the ring has records
  and every 100 ms once a wake finds it empty, so an idle recording
  costs a phone's background session (ADR-0029) ten wakeups a second,
  not a hundred. It drains the ring, formats the CSV rows exactly as
  ADR-0017 has them, and flushes every 100 ms, so
  if the process crashes, the recording loses at most the last tenth of
  a second. A flush hands the rows to the OS, which may still hold them
  when the power goes, and a power cut is how a recording often ends:
  a computer powered by the car at key-off, a phone whose battery
  dies. So the writer also syncs its files to the storage every 2 s
  (`FlushFileBuffers`, `fsync`), on its own thread, and a power cut
  loses at most the last 2 s. It opens its files for shared reading,
  so the reader can open a recording still being written (section 5).
  From the worker's timing records it appends `latency.csv`'s changed
  rows (ADR-0017's appendix of 2026-10-31), so `latency.csv` moves into
  core too. The worker's final snapshot is its last act before
  `ecuio_session_close` joins it; with no bus left to read, it may wait
  up to 1 s for room in the ring, and is dropped and counted only if
  none comes.
- **A full ring drops records, never blocks.** The worker counts what it
  couldn't queue and carries the count in the next record that fits.
  The writer writes the count into `events.log` at that record's time
  ("the recording dropped 312 frames: the disk fell behind") and adds it
  to the summary. A link transition that doesn't fit is carried the same
  way, its latest state and episode count, so a drop loses frames but
  never the link record. A replay at `speed=max` can outrun any disk,
  and its drops are counted the same way.
- **Detach.** `ecuio_session_destroy` detaches the session: the writer
  drains what the worker queued, and the recorder takes no more frames.
  The session reaches the recorder only through one atomic pointer,
  which only the worker uses, and every use is pinned: the worker
  raises an in-use count, loads the pointer, and lowers the count when
  its record is written (at once, if the pointer is NULL). Detaching
  clears the pointer, then waits until the count is zero, so the
  worker is never left holding the recorder. That makes detaching safe
  against the session's close and destroy running on another thread,
  which is the case close-while-attached (below) exists for: the
  caller's thread closes the recorder while its teardown thread may sit
  in `ecuio_session_close`, the worker still writing its last records.
  The recorder is freed only after that wait; whatever the worker
  queues after the pointer is cleared goes nowhere, so `latency.csv`
  may end with its last 10 s row.
- **Close** writes the summary (the caller's `why`, a line written as a
  note is, then frames, values, records and notes dropped) and the
  files' last rows, closes them, stops the writer and frees the handle;
  a call on a closed recorder is the caller's error, as on a destroyed
  session. Close frees the handle whatever it returns: `ECUIO_OK` when
  every file ended whole, `ECUIO_ERR_RECORDING_FAILED` when a write had
  failed or the last one did, so the files end where they stopped.
  Closing a recorder whose session is still attached detaches it first,
  and the session runs on unrecorded. That is the caller's way out when
  its teardown can't finish (the app quitting while a surrogate's close
  hangs, `havne-cli` on Ctrl-C), so the files still end whole and with
  their summary. Nothing queued is lost at a close.

### 2. One folder, named by core

`options` names a root folder and an optional label; core creates
`<root>/<yyyyMMdd-HHmmss>[-n][ <label>]/`. It creates the folder and
takes the next `-n` when the name is already there, never checking
first and creating after, so two writers starting in the same second
on one root (the app and `havne-cli`, two CLI runs) always get folders
of their own. The label rule of ADR-0027
section 2 moves into core: the caller passes the name as the user
typed it, and core reduces it to letters, digits, space, `-`, `_` and
`.`, at most 40 characters. A label is untrusted (the CLI takes it from
`--label`), so nothing in it reaches the path but those characters, and
an empty result is no label. `--record <root>` and `--label <name>`
take a value each, as `--count N` and `play`'s `--to <vcan>` do
(ADR-0031), and `--label` without `--record` is a usage error (exit
2). Which sessions get a label stays the caller's (the app labels only
an adapter's sessions, ADR-0027 section 1).

The app and `havne-cli stream --record <root>` then write the same
folder with the same files and header rows: `frames.csv`,
`signals.csv`, `events.log`, `latency.csv` when the session times
requests, and `markers.csv` (section 4) when anything is marked.

`events.log`'s header is core's, and a reader parses it (section 5), so
its first line names its format: `ecuio session recording, format 2
(ADR-0049)`. The marker is core's name and a format number, not the
product's, so a rename never changes what a recording is; ADR-0017's
format is 1. A fixed block of `key  value` lines follows: the start in
local time and in UTC to the millisecond, core's version, the URI, the
profile, and one `signal` line per def with its name, kind (number or
text), unit, decimal places and plot rank, so the viewer shows a value
as Live does and plots by default what Live plots. Then come the lines
the caller adds in `options` (the app's surrogate frame-logging state,
the picked vehicle), each under the one key `caller` and with its
newlines written as spaces, as a note's are, so no caller line can pass
for one of core's keys. A reader ignores keys it doesn't know, so the
header can grow. It knows format 2's first line and format 1's by its
ending, `session recording (ADR-0017)`, whatever product name comes
before it; a recording under format 1 is an older recording (below and
section 5). A header whose first line it doesn't know is read for
nothing but its activity lines.

A `frame flags` header line says whether `frames.csv`'s flags are
complete, `ECUIO_FRAME_FLAG_REASSEMBLED` (0x00000100, appended past
`OVERFLOW`) among them: a transport sets
it on a payload reassembled from ISO-TP (every requesting session, an
ELM327's `AT MA` monitor, whose lines core reassembles as it does a
reply's, and J2534's ISO 15765 channels), and leaves it off a frame as
it crossed the bus (a raw SocketCAN socket). It is per frame,
so a session that both requests and listens is described exactly, and
a replay passes it through. No transport mixes the two yet (a
requesting SocketCAN session's kernel filter passes only its reply
IDs, ADR-0040). A live session writes `frame flags  complete`. A
replay writes its source's line, and `frame flags  from an older
recording` when the source has none, since an older recording's frames
never carry the bit; a recording of that replay passes the line on
again. Where the line doesn't say `complete`, the flag's absence says
nothing.

After the header, every line is `HH:mm:ss.zzz  +S.sss  <kind>  <text>`:
the local time and seconds since the start (ADR-0017's form), then a
kind word, so a reader never has to guess from the text what a line
is. The kinds are `note` for the caller's lines and `link`, `asleep`,
`drop` and `end` for core's. A carriage return or newline inside a
note is written as a space, so one note is always one line, and no
note can pass for one of core's lines. Two kinds of line:

- **Core's own,** in a fixed form a reader parses: `link` lines for the
  session's link-health transitions, from the worker's link records
  (section 1), each with its state and episode count; an `asleep` line
  for each sleep `ecuio_clock_slept_us` shows, which the writer reads
  at each wake, so the line is stamped when the machine woke and its
  length of N s covers the N s before its time; the recording's drops;
  and the summary. The callers
  stop writing their own versions of these.
- **The caller's notes,** free text:

```c
ecuio_status_t ecuio_recorder_note(ecuio_recorder_t* recorder,
                                   const char* utf8_line);
```

safe from any thread until close, stamped when called: every thread
that notes or marks has finished its last call before the caller
closes the recorder, as with the getters. Notes and markers go
through a bounded queue of their own under a mutex (`options` sizes
it, 256 lines by default), not the worker's ring, so the ring keeps
one producer; a full queue drops the note and counts it. The app
passes its activity log's other lines, and at the end the frames the
Log page skipped under backpressure ("the Log page skipped 40 frames;
the recording has them"); `havne-cli` passes its end line.

Each line carries its own time, and the file is in the order the writer
took the lines, not sorted: a note and one of core's lines can appear
out of time order by up to a wake.

### 3. Failures and statuses

- **Can't open:** `ecuio_recorder_open` returns a new status,
  `ECUIO_ERR_RECORDING_FAILED` (18, appended), with a detail that names
  the cause and not the path, since the path holds the account name
  ("can't create the recording's folder: Access is denied"). The
  session is untouched: the caller decides whether to go on
  unrecorded, as the app does under ADR-0017 item 7.
- **A write fails** (a full disk, a removed card): the writer stops
  writing, keeps draining the ring so the worker never fills it, and
  publishes the state. The app shows "Recording stopped: the disk is
  full", the CLI writes a `recording` line. The file the write failed
  in ends where it stopped, possibly mid-row (section 7). A file
  system's size limit is a write failure too: FAT32, common on SD
  cards and USB sticks, stops a file at 4 GiB, which whole-bus
  listening (by estimate, about 650 MB an hour of a saturated classic
  bus) reaches in about six hours, and
  the reason says "the file reached the file system's size limit".
  Splitting a long recording across files waits for the container
  (section 8).
- **Notes and markers** are refused with `ECUIO_ERR_INVALID_ARGUMENT`
  until a session first attaches, since the recording has no zero to
  stamp them against. From the attach until close they return
  `ECUIO_OK` once queued, while the session is attached and after it
  has detached alike. `ECUIO_ERR_RECORDING_FAILED` after a write
  failed, and `ECUIO_ERR_NULL_ARGUMENT` for a NULL recorder or line. A
  line longer than 1 KB is cut there, at a character boundary.

```c
typedef struct ecuio_record_options {
    uint32_t           ring_bytes;     /* 0 for the default, 4 MiB */
    uint32_t           note_lines;     /* 0 for the default, 256 */
    uint32_t           header_line_count;
    uint32_t           flags;          /* zero */
    uint64_t           reserved[4];    /* zero; ABI growth space */
    const char*        root_utf8;      /* the folder recordings go in */
    const char*        label_utf8;     /* NULL or "" for none (section 2) */
    const char* const* header_lines;   /* the caller's header lines, UTF-8 */
} ecuio_record_options_t;

typedef struct ecuio_recorder_state {
    uint32_t state;              /* ECUIO_RECORDING_ACTIVE, _DETACHED, _FAILED */
    uint32_t flags;
    uint64_t frame_rows;         /* rows written to frames.csv */
    uint64_t value_rows;         /* rows written to signals.csv */
    uint64_t records_dropped;    /* the ring was full */
    uint64_t notes_dropped;      /* the note queue was full */
    char     reason[128];        /* the OS's reason for _FAILED, no path */
    uint64_t reserved[4];        /* zero; ABI growth space */
} ecuio_recorder_state_t;

ecuio_status_t ecuio_recorder_get_state(const ecuio_recorder_t* recorder,
                                        ecuio_recorder_state_t* out);
ecuio_status_t ecuio_recorder_get_folder(const ecuio_recorder_t* recorder,
                                         char* utf8, uint32_t capacity,
                                         uint32_t* out_len);
```

The options put their pointers last, as `ecuio_value_t` and
`ecuio_value_batch_t` do (`profile.h`), so every other field's offset
is the same on every pointer width and pinned by static asserts; the
size is pinned where pointers are 8 bytes.

The folder is UTF-8 and absolute, for the caller to show relative as
ADR-0017 item 3 does. It is copied terminated, cut to `capacity`, and
`*out_len` is its full length, so a caller spots a cut by comparing.
Both getters are safe from any thread until close.

### 4. Markers

```c
ecuio_status_t ecuio_recorder_mark(ecuio_recorder_t* recorder,
                                   const char* utf8_label);
```

writes `host_us,label` to `markers.csv`, created by the first mark: the
Mark button in the app, a line typed into `havne-cli stream --record`.
The label is CSV-quoted as `signals.csv`'s text values are, and the
statuses are the notes' (section 3). The viewer aligns runs on them
(section 5).

### 5. The reader

```c
typedef int (*ecuio_open_progress_t)(uint64_t bytes_read,
                                     uint64_t bytes_total,
                                     void* user_data);

ecuio_status_t ecuio_recording_open(const char* folder_utf8,
                                    ecuio_open_progress_t progress,
                                    void* user_data,
                                    ecuio_recording_t** out);
ecuio_status_t ecuio_recording_get_info(const ecuio_recording_t* r,
                                        ecuio_recording_info_t* out);
ecuio_status_t ecuio_recording_get_signal(const ecuio_recording_t* r,
                                          uint32_t index,
                                          ecuio_recording_signal_t* out);
ecuio_status_t ecuio_recording_get_marker(const ecuio_recording_t* r,
                                          uint32_t index,
                                          ecuio_recording_marker_t* out);
ecuio_status_t ecuio_recording_query(const ecuio_recording_t* r,
                                     const uint32_t* signals,
                                     uint32_t signal_count,
                                     uint64_t t0_us, uint64_t t1_us,
                                     ecuio_recording_column_t* columns,
                                     uint32_t column_count);
ecuio_status_t ecuio_recording_text_at(const ecuio_recording_t* r,
                                       uint32_t signal, uint64_t t_us,
                                       char* utf8, uint32_t capacity,
                                       uint64_t* at_us);
ecuio_status_t ecuio_recording_find(const ecuio_recording_t* r,
                                    const ecuio_recording_find_t* what,
                                    uint64_t from_us, uint64_t* at_us);
void ecuio_recording_close(ecuio_recording_t* r);
```

The structs are POD with `flags` and `reserved`, their sizes pinned by
static asserts as the rest of the ABI's are:

- `ecuio_recording_info_t`: the duration, the signal and marker
  counts, the UTC start in microseconds (0 when the header has none),
  the rows dropped as torn (section 7), and the records and notes the
  writer reported dropped. A recording with no summary (the process
  crashed, the power went) has `ECUIO_RECORDING_INFO_NO_SUMMARY` set,
  and its drops are counted from `events.log`'s drop lines instead.
- `ecuio_recording_signal_t`: the name (`char[32]`) and unit
  (`char[16]`) as `ecuio_signal_def_t` holds them, its decimal places
  and plot rank as the def has them, a text flag, the sample count, the
  lowest and highest value, the first and last sample's `host_us`. An
  older recording has neither display field in its header: its decimal
  places are the most any of its values shows (at most 3), and its plot
  rank is 0, so the viewer plots nothing of it by default.
- `ecuio_recording_marker_t`: `host_us` and the label.
- `ecuio_recording_column_t`: a column's lowest, highest and sample
  count; a column with no samples has count 0.
- `ecuio_recording_find_t`: a kind (threshold or marker), the signal,
  the level, the direction and the hysteresis, or the marker's label.

How it works:

- **One pass at open** reads `events.log`'s header (the UTC start, each
  signal's kind) and `signals.csv` (the decoded values: the viewer's
  source in v0.4.0; `frames.csv` through a profile's decode comes
  later). It builds, per signal, its name, unit, count and range, and a
  summary per second of `host_us` in which it has samples: for a
  number, the lowest, highest and last value and the count; for a
  text signal, the count and the byte offset of the second's last row.
  Beside it, a sparse index: each second's byte offset in the file.
- **Open is the slow call.** An hour's pass can take seconds on a
  phone, so the viewer opens off the GUI thread. `progress`, when not
  NULL, is called on the opening thread every few megabytes; a nonzero
  return stops the open, which returns `ECUIO_ERR_CANCELED` (19,
  appended). As `ecuio_session_create` does, open sets `*out` to NULL
  before anything else, so it is NULL on every failure, a cancel
  included, and a failed open leaves no file open and nothing
  allocated.
- **Statuses:** every reader call returns `ECUIO_ERR_NULL_ARGUMENT`
  for a NULL handle or out pointer and `ECUIO_ERR_INVALID_ARGUMENT` for
  a signal or marker index past the count, `t0_us >= t1_us`, a
  `column_count` or `signal_count` of 0, or a threshold `find` on a
  text signal. Finding nothing is not an error: `ecuio_recording_find`
  when nothing crosses the level or no marker has the label, and
  `ecuio_recording_text_at` before the signal's first sample, return
  `ECUIO_OK` with `*at_us` set to `ECUIO_RECORDING_NONE` (UINT64_MAX)
  and, for `text_at`, an empty string.
- **Threads:** once open, every query is safe from several threads at
  once on one handle. What open built is read-only; each read of the
  file takes its own file position; and the raw rows' cache (below),
  the one thing queries change, sits behind a mutex held only while a
  query looks a second up in it or fills one. Close is the one call
  that must not overlap the others.
- **A recording still being written** opens up to its last complete
  row (section 7 drops the torn one); what the writer adds after the
  open isn't seen until it's opened again.
- **A raw recording has no values.** A session under the `raw`
  profile writes an empty `signals.csv`, so v0.4.0's viewer shows such
  a recording the check's findings only (section 6). Decoding
  `frames.csv` through a profile at open, which makes a whole-bus
  recording viewable, belongs with listening to the whole bus
  (v0.8.0).
- **Older recordings** have no signal kinds in their header: a signal
  is text there if any of its values doesn't parse as a number. They
  have no UTC start either, so they align on markers and thresholds
  only.
- **The file's order:** rows are in `host_us` order, as both writers
  write them. A row earlier than the one before it is refused with its
  line number, since the offset index rests on that order.
- **The time axis is `host_us`,** the one clock every transport's
  recording shares (`signals.csv` has no other). A recording made of a
  replay at `speed=max` has its times compressed to how fast the replay
  ran, and the viewer shows them as they are.
- **A range query** fills `column_count` columns between `t0_us` and
  `t1_us` for each of `signal_count` signals, `columns` holding
  `signal_count` x `column_count` entries, signal by signal. Each has
  its lowest and highest. Where a column spans a second or more it
  combines the summaries; zoomed closer, it reads the raw rows of just
  those seconds through the offset index, once for every signal in the
  query, so an overlay of several signals reads the file once. The
  reader keeps the raw seconds it parsed last, up to 8 MiB, so a pan
  reads only the seconds it newly reaches. Either way the answer is the
  same min/max-per-column shape, about twice the chart's width in
  points, which Live's chart already draws. A query on a text signal
  returns counts only; `ecuio_recording_text_at` gives
  the text in force at a time (the last at or before it) and its time,
  for the cursor's readout (a VIN, a calibration ID).
- **The memory bound,** stated and tested: 32 bytes per number signal
  and 16 per text signal for each second it has samples, plus 16 bytes
  per second for the index and the raw rows' cache (8 MiB at most),
  whatever the file's size. As an example, 50 number signals each with
  a sample every second of an hour come to about 5.8 MB beside the
  cache.
- **Alignment:** `ecuio_recording_find` returns the first time after
  `from_us` a signal crosses a level in a direction, with hysteresis
  (rpm above 3000 after being below 2900), or a marker's time; the
  viewer and `havne-cli view --align-on` share it. Two recordings also
  line up on their UTC starts (section 2): to about a millisecond when
  one machine's clock stamped both, and to the machines' clock
  agreement (tens of milliseconds under NTP) when two did.
- **Untrusted input:** a recording comes from anywhere. Every row is
  bounds-checked, a bad row is refused with its line number (ADR-0023's
  rule), never its path, and every parser of a recording gets a fuzz
  target: `signals.csv`'s, `events.log`'s header and lines,
  `markers.csv`'s, and the check (section 6), whose ISO-TP state and
  interval estimates run on whatever `frames.csv` holds. A signal name
  longer than 31 bytes or a unit longer than 15 is a bad row: core and
  the app write a def's terminated buffers, so only a foreign file has
  one. Lines end in LF or CRLF (ADR-0017), and `signals.csv`'s
  byte-order mark is skipped. A quoted field may hold a comma or a quote
  but never a newline (ADR-0017's appendix of 2026-10-23: a TEXT value
  shows such bytes as `.`), so a row is a line.

### 6. The frames check

`havne-cli info` and the viewer's findings come from one pass over
`frames.csv` in core, since only core parses frames:

```c
ecuio_status_t ecuio_recording_check(const char* folder_utf8,
                                     const ecuio_check_options_t* options,
                                     ecuio_open_progress_t progress,
                                     ecuio_check_finding_callback_t finding,
                                     void* user_data,
                                     ecuio_check_summary_t* out);
```

- **A stream, no index.** It reads `frames.csv` once, through
  `frames_csv.h`, and `events.log`'s `link` and `asleep` lines. In an
  older recording, which has no kind words, it reads the app's sleep
  lines by their wording (`Asleep for N s: nothing was read
  meanwhile`, ADR-0017's appendix of 2026-10-29), so a sleep in a
  vehicle recording made before v0.4.0 isn't reported as a gap; those
  recordings are what the defaults are set against. Its
  memory follows the IDs seen, not the file: per ID, a fixed handful of
  numbers (its count, last time, interval estimate and ISO-TP state).
  `info` reads once and exits, so nothing is kept.

```c
typedef struct ecuio_check_options {
    uint64_t gap_us;          /* 0 for the default, 1 s */
    uint32_t rate_drop_x100;  /* 0 for the default, 400 (4x the median) */
    uint32_t flags;           /* zero */
    uint64_t settle_us;       /* 0 for the default, 2 s */
    uint64_t reserved[4];     /* zero; ABI growth space */
} ecuio_check_options_t;

typedef struct ecuio_check_finding {
    uint32_t kind;            /* ECUIO_CHECK_GAP, _RATE_DROP, _LATE_ID,
                                 _OVERFLOW, _ISOTP; append-only */
    uint32_t arbitration_id;  /* the ID's; unused with
                                 ECUIO_CHECK_FINDING_WHOLE_BUS */
    uint64_t at_us;           /* host_us where it starts */
    uint64_t length_us;       /* how long it lasted; 0 for an instant */
    uint64_t count;           /* frames it covers (overflows in a row) */
    uint32_t flags;           /* ECUIO_CHECK_FINDING_WHOLE_BUS: a gap,
                                 which is every ID's; 0x000 is an ID */
    uint32_t reserved0;
    uint64_t reserved[2];     /* zero; ABI growth space */
} ecuio_check_finding_t;

typedef struct ecuio_check_summary {
    uint64_t duration_us;
    uint64_t frames;
    uint32_t ids;
    uint32_t flags;           /* ECUIO_CHECK_SKIPPED_ISOTP: no wire frames,
                                 or the frame flags aren't complete */
    uint64_t counts[8];       /* findings per kind, indexed by kind; a
                                 kind past the eighth is counted only
                                 through the callback, until a summary
                                 of its own grows a field */
    uint64_t torn_rows;       /* section 7 */
    uint64_t reserved[4];     /* zero; ABI growth space */
} ecuio_check_summary_t;

/* Called on the checking thread, once per finding, in time order;
 * a nonzero return stops the check (ECUIO_ERR_CANCELED) */
typedef int (*ecuio_check_finding_callback_t)(
    const ecuio_check_finding_t* finding, void* user_data);
```

`options` may be NULL for every default, and `finding` may be NULL
for the summary alone. `progress` is the open's (section 5): called on
the checking thread every few megabytes, and a nonzero return stops the
check with `ECUIO_ERR_CANCELED`, so a long check of a recording with
nothing wrong can show its progress and be canceled too. The summary
is filled on success and on a stop.

- **Findings,** each passed to the callback in time order:
  - a gap: no frame at all for longer than `options` says (1 s by
    default), outside the stretch an `asleep` line covers (the N s
    before its time, section 2), flagged as the whole bus's;
  - a rate drop: an ID's interval past a multiple of its median (4x by
    default). The median is a streaming estimate (the P-square
    estimator, five numbers per ID), not one kept from every interval,
    so memory stays per ID; an ID is judged only after its first 16
    intervals;
  - a late ID: one first seen more than a settle time after the first
    frame (2 s by default), a node that woke late or a stray;
  - an overflow: a frame flagged `ECUIO_FRAME_FLAG_OVERFLOW`;
  - a broken ISO-TP sequence: a consecutive frame out of order, or a
    first frame never completed. Only frames without
    `ECUIO_FRAME_FLAG_REASSEMBLED` in a recording whose header says
    `frame flags  complete` (section 2) are checked for these; a
    reassembled payload has no sequence left to break, and where the
    line says anything else, or is missing, the two can't be told
    apart. A payload's length can't settle it, since a reassembled
    reply can be 8 bytes or fewer. The summary says when the check
    skipped this kind, and why.
- **The summary:** the duration, the frames, the IDs, and each kind's
  count. Each signal's count and rate, which `info` also prints, come
  from the reader (section 5).
- The defaults are starting points, set against the replay fixture and
  the vehicle recordings when it's built, and recorded in an appendix.
  The viewer shows the findings on its time axis; `havne-cli info`
  prints them as JSON lines.

### 7. A torn last row is dropped, not refused

A crash, a pulled cable or a failed write can leave the last line
without its newline. The reader, the check (and `replay://`, through
the same `frames_csv.h`) drop a final line that has no newline and
count it; any other bad row is still refused with its line number. A
recording that stopped hard stays readable.

### 8. Compatibility

- **The CSV files don't change.** Same names, columns and header rows,
  so every recording made so far opens, and `replay://` plays a
  recording core wrote as it plays one the app wrote. The new
  `ECUIO_FRAME_FLAG_REASSEMBLED` bit is a value in the `flags` column,
  which readers take as hex already.
  `BridgeTest.ReplayOfAMockRecordingDecodesWhatWasRecorded` keeps
  holding the two together, with core on both sides.
- **`events.log` grows** a format line, the UTC start, the `signal`,
  `frame flags` and `caller` lines, a kind word on every line and core's
  `link`, `asleep` and `drop` lines (section 2), and loses the bridge's
  drop count from its summary. Nothing outside the reader and the check
  parses it.
- **The container stays deferred.** One file holding the folder's
  contents (MDF4, a SQLite file, an archive) waits for listening to the
  whole bus (v0.8.0), where frame rates make CSV's size matter.
- **Spreadsheets: the raw files are evidence, the export is for
  people.** A spreadsheet runs a cell that starts with `=`, `+`, `-` or
  `@` (some also a tab or a carriage return) as a formula, quoted or
  not. `frames.csv` and `latency.csv` hold only hex and numbers core
  formats, so they can't carry one; `signals.csv` and `markers.csv` can,
  in a text value an ECU sent, a marker label, or a signal's name or
  unit.
  - **The raw files stay exact.** No value is escaped or changed in
    them, so they remain the evidence a session sheet rests on and what
    `replay://` and the reader read. Opening one directly in a
    spreadsheet is opening a CSV from elsewhere, and the user docs say
    so. A writer-side escape would guard only recordings core wrote,
    while a recording someone sends never passed through core's
    writer; the reader validates every row of either.
  - **Names and units are refused where defs enter.** Signal-set
    validation refuses a name or unit that starts with one of those
    characters, with a validator code of its own (ADR-0016 section 8),
    so a profile, `uds-generic`'s generated defs and v0.5.0's imported
    ones can't carry one. No built-in def does: no name or unit in the
    Service 01 table starts with one, and `uds-generic`'s names start
    `DID` or `0x`. A recording core writes can then hold such text only
    in an ECU's text values and the user's own labels.
  - **The export** (v0.6.0, with sharing) is how a recording opens in a
    spreadsheet: `ecuio_recording_export` on an open reader, so it
    inherits the reader's validation and guards a foreign recording as
    well as one core wrote; `havne-cli export <recording> <out.xlsx>`
    and the viewer's Export. It writes `.xlsx`, whose cells are typed:
    a text value is stored as text, so nothing runs and nothing is
    escaped, and each cell shows what the ECU sent. The rules it's
    built to:
    - one sheet of samples, a time column and a value column per
      signal side by side, the unit in the header beside the name.
      Each pair is dense, nothing is filled in (a filled cell would
      claim a reading the ECU never sent), and each charts as a line;
      one row per distinct time across all signals would leave most
      cells blank under `obd2`, where each PID arrives in its own
      frame, and a spreadsheet draws blanks as gaps;
    - markers on a sheet of their own;
    - a sheet's 1,048,576 rows bound one signal's samples, and a
      signal with more is refused by name (splitting is v0.6.0's
      choice); a text cell is cut at 32,767 characters rather than
      written into a file the spreadsheet offers to repair;
    - entries stored uncompressed (CRC32 only, no zlib), strings
      inline (no shared-string table), and every entry's date the zip
      format's first, 1980-01-01, so the same recording always gives
      the same file.
  - **When sharing lands** (v0.6.0), whatever it shares goes through
    the same guard: its scrub of VINs and serials writes through the
    export's rules, never a raw text value into a file meant for a
    spreadsheet.
  - **Tests, when it's built:** the export unzipped with Python's
    standard library (`zipfile`, which checks every CRC), its XML parts
    and cell values compared with a golden, as `golden.py` needs no
    packages; LibreOffice headless (`soffice --headless --convert-to
    csv`) opens it and its cells match, in a local or optional CI job,
    as the install is large; and an export opened by hand in Excel and
    Google Sheets before v0.6.0 is tagged. That last check is the one
    rule in this record no test proves, and it's named as manual.
- **The app's recorder shrinks** to calling these functions: Settings'
  switch, the folder shown relative to Documents, Open folder and the
  frame-logging environment variable (ADR-0017 item 6) stay in the
  app.
- **Privacy is unchanged:** `events.log` records the URI, and a
  direct-load URI holds a local path, so a recording the CLI writes is
  scrubbed before it's committed, as one the app writes is (ADR-0017).

### 9. Done when

The seams below (a slow writer, a failing write, the sync count, a
writer held back) are test-only functions in `internal.h`, as
`ecuio_elm327_set_log_dir` is, never in `recording.h`: `havne-cli`
and any outside program see none of them.

- **Comparing folders.** A recording's times depend on the run, so two
  folders are compared as `BridgeTest`'s round trip compares a replay
  with its recording (`tests/test_bridge.cpp`): `host_us` left out,
  `timestamp_us` rebased to the first frame, every other column equal.
  That covers `frames.csv`, `signals.csv` and `latency.csv`.
  `events.log` is compared only between two folders core wrote, and
  only on what doesn't depend on the caller or the run: the header
  without its times, core's version and the `caller` lines, and core's
  own lines without their times; the notes differ by front end. The
  test tooling gets that comparison for folders, beside `golden.py`'s
  for JSON lines.
- **The writer:** the replay fixture recorded through core matches the
  CSV files the app's `SessionRecorder` writes (its `events.log` has the
  older form); the same replay through `havne-cli stream --record`
  matches the core-written folder, `events.log` included; a slow-writer
  seam fills the ring and the drop count lands in `events.log` and the
  summary; a write-failure seam publishes `ECUIO_RECORDING_FAILED` and
  the session keeps running; notes after `ecuio_session_destroy` reach
  `events.log`; a writer held back by a seam until after destroy still
  writes every value's name and unit (under ASan); a close while
  attached detaches and ends the files whole, and so does one made while
  another thread sits in `ecuio_session_close` (under TSan); a link
  transition lands in `events.log` between the frames around it, also
  when the ring was full; a sync seam counts one sync every 2 s; a note
  holding a newline or starting with `asleep` is written as one `note`
  line; `CoreTest.WorkersAllocateNothingPerFrame` passes with a recorder
  attached; a recorder closed while the worker writes its last records
  is freed only after them (under ASan and TSan); a session destroyed
  while the writer is still formatting leaves the writer nothing to read
  of it (under ASan); a caller header line starting `signal` is written
  under `caller`; a replay of an older recording writes `frame flags
  from an older recording`; two recorders opened in the same second on
  one root get two folders; an idle recording's writer wakes every
  100 ms; close frees the handle after a failed write and returns
  `ECUIO_ERR_RECORDING_FAILED`; a signal set whose name or unit starts
  with `=`, `+`, `-` or `@` is refused with its validator code, and
  every built-in profile still validates; the statuses of section 3
  each have a case; TSan runs the ring under the sanitizer job.
- **The worker's cost:** recording costs the worker at most 10% of its
  rate. `bench_bridge`'s load test measures the worker's frames a
  second on its looping full-speed run of the replay fixture, and
  gains the same run with a recorder attached. Back-to-back runs of one
  build vary by tens of percent on a desktop (`scripts/bench_ab.py`),
  so the 10% is checked as `bench_ab.py` checks a change: alternating
  runs, recorded against unrecorded, their medians compared, on the dev
  machine, and the figures go into `docs/testing.md` ("Cost per
  frame"). CI gates a looser ratio, as it gates the bench's other two
  (the recorded run at least 0.8 of the unrecorded one in the same
  bench), so a regression fails it and a noisy runner doesn't. No
  records are dropped on the paced run at 100x, about 3 s. Step 1
  measures on the bench's core-side run
  (`ecuio_session_dispatch_callbacks` on a session of its own, a
  recorder attached), since the bridge keeps `SessionRecorder` until
  step 2; step 2 adds the bridge's looping run and the CI gate on it.
- **The reader:** an hour-long recording (the replay fixture's session
  looped to an hour under `obd2`) opens, and the reader's own count of
  what it holds is checked against the bound. It opens in under 2 s on
  the laptop and under 10 s on the test phone; the times are recorded
  beside the bound, and a miss is what moves the per-second index into a
  file of its own (the alternative below). A progress callback's nonzero
  return cancels the open. A torn last row is dropped and counted in the
  reader, the check and `replay://`; a row out of order and an overlong
  name are refused with their line numbers; an older recording opens
  with its kinds and decimal places inferred, whichever product name
  heads its first line; each signal's decimal places and plot rank match
  its def's; a recording still being written opens to its last whole
  row; one with no summary sets the flag and counts its drop lines. A
  query over several signals equals the same queries one signal at a
  time, a pan across a zoomed window stays within the cache's bound, and
  concurrent queries on one handle pass under TSan.
- **The check:** a recording of reassembled payloads reports the
  ISO-TP kind skipped, and a callback's nonzero return stops it with
  the summary filled; a fixture with one of each finding, built from the
  replay fixture, reports each at its time; a gap is flagged as the
  whole bus's, and an ID 0x000 is reported as itself; the check's
  memory is the same for the fixture looped ten times as once; an older
  recording's `Asleep for` line keeps its sleep from being a gap; a
  progress callback's nonzero return stops a check that has found
  nothing; the check, `markers.csv` and `events.log` have fuzz targets
  beside `signals.csv`'s; `havne-cli info` has a golden.
- **The ABI:** `ecuio/recording.h` compiles as strict C in
  `test_c_abi`, and the ABI check passes.

### 10. Order of work

1. The writer in core, with `ecuio_session_record`, notes, markers, the
   state, the worker's link and timing records and core's own
   `events.log` lines;
   `ECUIO_FRAME_FLAG_REASSEMBLED` set by every transport that
   reassembles, so every recording core writes carries it;
   `havne-cli stream --record` and `--label`; the record sizes the ring
   is sized by, measured and recorded in an appendix; the validator's
   refusal of formula-leading names and units. The app keeps its
   `SessionRecorder` meanwhile, and the round-trip test holds the two
   to one format.
2. The app moves to core's recorder; `SessionRecorder` and the bridge's
   recording code go, and the dated notes in ADR-0016, ADR-0017,
   ADR-0023, ADR-0024, ADR-0027 and ADR-0039 are written, ADR-0017's
   saying the raw files are evidence and the export is the way into a
   spreadsheet.
3. The reader and its fuzz targets, then `havne-cli view`.
4. The check and `havne-cli info`.
5. The viewer, on the reader and the check. Its chart is Live's
   `Canvas` if that pans and pinches at 60 frames a second with four
   signals at the viewer's full width on the test phone and the laptop,
   and a GPU path if not, the numbers recorded either way; the reader's
   min/max columns serve either.
6. In v0.6.0, with sharing: the export (section 8), `havne-cli export`
   and the viewer's Export, and sharing's scrub through the same rules.

A stretch replayed (`replay://<recording>?from=&to=`, also on v0.4.0's
list) is a change to the replay transport with its own appendix in
ADR-0023, not part of this record.

## Alternatives considered

- **Keep the recorder in the app.** No ABI, but it still loses the
  frames the bridge drops, and `havne-cli` would need a second writer
  in another shape, two implementations of one format that the
  round-trip test would have to hold together twice.
- **Core hands the caller a lossless queue, and each front end
  writes.** The worker's side is the same, but every front end then
  formats and writes the files, and the format has as many
  implementations as front ends.
- **The recording lives and dies with the session** (`ecuio_session_record`
  taking options, the files closed at destroy). One handle fewer, but
  the app's teardown lines come after destroy, so the recording would
  stop bracketing the teardown.
- **A close while attached is refused.** A stricter rule, but a caller
  whose teardown hangs could then never end its files, and a killed CLI
  would leave them without a summary.
- **Escape formula-leading text in the raw files** (a leading `'`,
  reversibly, as the common advice has it). The files stay spreadsheet
  safe without an export, but they stop being exact, a user's own
  script sees the apostrophe, and a recording someone sends never
  passed through the escape at all. Quoting the fields doesn't help: a
  spreadsheet runs a quoted formula too. Sanitizing at the decode
  would change what Live and the CLI show.
- **An escaped CSV as the export.** Smaller than `.xlsx`, but the
  apostrophe comes back in the copy; typed cells need no escape.
- **Format rows on the worker.** Simpler (no compact records), but the
  formatting is the costly part, and the worker's budget is the
  bus's.
- **Block the worker when the ring is full.** Nothing is ever dropped,
  but a slow disk then stalls the bus reads and the request rotation,
  which is worse evidence than a counted drop.
- **The writer wakes on a signal from the worker.** Lower latency to
  disk, but a condition variable's notify per frame is a system call on
  the worker's path; a 10 ms poll, backing off to 100 ms when idle,
  costs nothing the 100 ms flush would notice.
- **The writer also writes the per-second index** as a file beside
  `signals.csv`, so open reads a small file instead of the whole
  recording. Faster to open, but a second format to keep in step, and
  older recordings still need the pass. Kept for if the measured open
  time (section 9) is too slow.
- **`havne-cli info` from a replay** at `speed=max` with a frame
  callback. No new ABI, but the CLI would then parse frames (ISO-TP
  sequences), which is core's job, and the viewer would need the same
  analysis a second time.
- **SQLite as the recording.** Indexed queries for free, but a new
  dependency in core, and a recording stops being a folder a
  spreadsheet opens. Kept in view for the container.
- **Index every row.** Exact, but memory grows with the file; a
  per-second summary plus raw reads near the cursor gives the same
  picture at the screen's resolution.

## Consequences

- A recording holds every frame the transport delivered and every
  value core decoded, whatever the front end did with them; the
  summary's "frames dropped by the bridge" goes (the app notes what
  the Log page skipped instead), and a recording that lost records
  says where and why.
- `havne-cli stream --record` writes the folder the app writes, so a
  laptop session, a phone relay session and an app session all replay,
  view, check and attach to session sheets the same way.
- The worker does a little more per frame while recording: a decode
  when no values callback asked for one, and a copy of the used bytes
  and values into the ring. A listening session with a profile decodes
  every frame whether or not anything reads the values; the load test
  gates it (section 9).
- Link transitions and sleeps are written by core in one form, so the
  app's and the CLI's recordings say them the same way and the check
  can read them; a transition is queued by the worker that sees it, so
  its time is exact.
- A power cut loses at most the last 2 s of a recording, at the cost
  of a sync every 2 s on the writer's thread.
- New ABI: `ecuio/recording.h` (the recorder's open, attach, note,
  mark, state, folder and close; the reader's functions; the check;
  their structs with `flags` and `reserved`),
  `ECUIO_ERR_RECORDING_FAILED`, `ECUIO_ERR_CANCELED` and
  `ECUIO_FRAME_FLAG_REASSEMBLED`, and a validator code for a
  formula-leading name or unit. Strict C, `test_c_abi`. v0.6.0 adds
  `ecuio_recording_export` and its options.
- A raw file opened directly in a spreadsheet can still run a formula
  an ECU's text or a sent recording carries, as a CSV from anywhere
  can; the export is the safe way in, and the user docs say so.
- `host_us` is when a frame reached dispatch, not when it crossed the
  wire; `timestamp_us` keeps the transport's time.
- `ECUIO_FRAME_FLAG_REASSEMBLED` reaches every frame consumer, not
  only the recording: the app's Log page and `havne-cli`'s `frame` line
  show it (0x100) on a live requesting session's replies. The CLI's
  goldens run on replays and the mock, which never set it, so none
  changes.
- The reader's memory follows the recording's length, not its size:
  a long, sparse session costs little, and a multi-hour one with many
  signals costs its seconds times its signals, stated by the bound.

## Appendix: the ring's record sizes (2026-11-02)

A record is its head (16 bytes: its size, its type and the drop count),
then for a frame 32 bytes (`host_us`, `timestamp_us`, the ID, the
flags, the length and the value count), the frame's used bytes, and per
value 8 bytes of head followed by its double (8) or its text's bytes.
Records are packed end to end, unpadded. A link transition is 32 bytes;
a reply-timing snapshot 96 plus 80 per request.

| Record | Bytes |
|---|---|
| A classic 8-byte frame, listened to raw | 56 |
| The same with one number decoded | 72 |
| A Service 01 reply through obd2 (`41 0C 1F 40` and its value) | 68 |
| The engine-start fixture through obd2, on average (7,074 frames, 7,061 values) | 67 |
| The mock's telemetry frame (12 bytes, 9 values) | 204 |
| A 64-byte CAN FD frame, listened to raw | 112 |

Section 1's estimate for classic CAN (some 40 bytes) was low. The
default 4 MiB holds about 19 s of a saturated classic 500 kbit/s bus
listened to raw (about 4,000 frames a second at 56 bytes), and about
15 s with a number decoded from each frame, not over 20 s. The FD
figure stands: about 7 s at about 5,500 frames a second of 112 bytes. A
requesting session's few hundred replies a second take minutes to fill
it. `RecorderTest` sizes a ring in whole records of one shape (65
bytes: a 1-byte frame and its number), so a change to the layout fails
a test.

## Appendix: the worker's cost, measured (2026-11-02)

`bench_bridge --record` runs step 1's measurement on core's own
session: the engine-start fixture replayed under `obd2`, looping at
full speed, to a frame callback that counts and a values callback that
does nothing, as the bridge's are. Recorded and unrecorded runs
alternate, five of 3 s each, with the process held to one logical
processor on each of two physical cores. Without that, where the OS
puts the threads decides the figures: on the dev machine (8 cores, 16
threads) a thread that never rests slowed the worker by 43% when it
shared the worker's core and by 4% on a core of its own. One run
(Release):

| Run | Worker frames a second, median (lowest to highest) |
|---|---|
| Not recorded | 5,082,631 (4,850,466 to 5,113,153) |
| Recorded | 4,179,817 (4,080,536 to 4,281,321) |

Recording costs this worker 31 to 47 ns a frame (0.81 to 0.86 of its
rate in four runs), against a frame that costs it about 0.2 us with
nothing else to do: a replay's row parsed, the decode and two callbacks
that return at once. Under WSL on the same machine it cost 41 and
42 ns, but there the unrecorded worker ran about 8 million frames a
second, so the same cost was 0.75 of its rate: the share follows the
rest of the worker's speed, the nanoseconds don't. About 20 ns of it is the path every frame takes
(the clock read, the pin, the size): with a 4 KB ring, which drops
nearly every record and leaves the writer idle, the recorded runs held
0.89 of the unrecorded rate. The rest is the record's write into a ring
the writer's core reads. At this rate the writer, on a core of its own,
wrote about a million frame rows a second and three records in four
were dropped; both recorded passes, at full speed and at 100x, put all
7,074 frames and 7,061 values in the files with no record dropped.

Section 9's 10% isn't met on this run, and this isn't the run it is
judged on: the bridge's worker spends about 2.5 us on a frame (its 4 KB
copy and queued call, ADR-0024's staging), so the same cost there is
about 2%, and at a saturated classic bus's 4,000 frames a second it is
under 0.02% of a core. Step 2 judges the 10% on the bridge's looping
run, recorded against unrecorded, as section 9 says, with its CI gate.
Here the bench checks a bound against regressions instead: recording
adds at most 75 ns a frame. A busy loop of 200 turns on every recorded
frame fails it (107 and 142 ns in two runs).

## Appendix: the bridge's cost, measured (2026-11-03)

Step 2 judges section 9's 10% on the bridge's looping run:
`bench_bridge --record` replays the engine-start fixture under `obd2`
through a `SessionBridge` at full speed, five runs of 3 s each way,
recorded and unrecorded alternated, with the process held to one
logical processor on each of three physical cores. One run (Release):

| Run | Worker frames a second, median (lowest to highest) |
|---|---|
| Not recorded | 534,618 (515,656 to 542,733) |
| Recorded | 514,763 (495,563 to 556,927) |

The recorded runs went at 0.96 of the unrecorded rate, so the 10% is
met, and `--record` fails below 0.9. At this rate, over a hundred times
a saturated classic bus's, the writer sharing those three cores fell
behind and 22,125 records were dropped and counted. The core-side runs
of the appendix above follow in the same bench, with their bound of
75 ns a frame.

CI's gate is `--load`'s: its looping run again with a recorder
attached, at least 0.8 of the unrecorded run in the same bench. It read
0.96 on Windows (471,840 frames a second against 493,534) and 1.01
under WSL, with no record dropped in either. The recorded pass at 100x
writes all 7,074 frames and 7,061 values with none dropped, on both.

## Appendix: the reader and `havne-cli view` (2026-11-03)

Step 3 is built: the reader (section 5, `core/src/recording_reader.cpp`
and `core/src/recording_rows.h`), its fuzz target (`fuzz_recording`,
the rows and a whole folder), a torn last row dropped and counted in
the reader and in `replay://` (section 7), and `havne-cli view`, whose
lines are ADR-0031's appendix of 2026-11-03.

**The open, measured.** `bench_recording` (`tests/bench_recording.cpp`,
not in ctest) writes section 9's hour, the replay fixture's session
under `obd2` laid down twelve times 300 s apart, and a dense hour of 50
number signals at 10 Hz each, then opens each five times. It builds
core-only for Android with the NDK, so the test phone (Android 16) runs
it from a shell, its scratch folder first in `/data/local/tmp`, then on
shared storage, where the app keeps its recordings. Medians:

| Hour | Rows | `signals.csv` | Laptop (MSVC) | Laptop (GCC, WSL) | Test phone | Test phone, shared storage |
|---|---|---|---|---|---|---|
| The fixture's, 130 signals | 84,732 | 2.8 MB | 8.9 ms | 5.9 ms | 9.1 ms | 10.0 ms |
| Dense, 50 signals at 10 Hz | 1,800,000 | 50.2 MB | 167 ms | 114 ms | 204 ms | 213 ms |

Each reader's tables came to section 5's bound to the byte (1.04 MB
and 5.82 MB), since every second of both hours has samples. Every open
found the files in the system's cache, the bench having just written
them; a cold open is unmeasured (the phone needs root to drop its
cache). Section 9's 2 s and 10 s are met by two orders of magnitude, so
the per-second index stays in memory, and the alternative of an index
file isn't needed.

**Alignment to the second at wide zoom.** A column of a second or more
combines whole seconds' summaries, so two recordings aligned on events
at different fractions of a second agree there to the second, not
exactly; narrower columns read the raw rows and agree exactly. The
viewer zoomed out draws columns wider than a second, where that
difference is less than a column.

**An axis that starts before a recording.** The reader's times start at
the recording's own zero, so an aligned axis that starts earlier can't
be one query. `havne-cli view` queries the column across the
recording's start on its own: exactly to the split in raw columns, and
to its last whole second in wide ones, the second across the split
going to the next column by the reader's rule. No sample is lost or
counted twice (`CliViewRunTest.ColumnsCountEverySampleOnce`, over
alignments and zooms from one column to 9,999); the viewer has the same
case to meet in step 5.

## Appendix: the frames check and `havne-cli info` (2026-11-03)

Step 4 is built: the check (section 6, `core/src/recording_check.h` and
`core/src/recording_check.cpp`), `fuzz_recording`'s folders checked as
well as opened, and `havne-cli info`, whose lines are ADR-0031's
appendix of 2026-11-03, info. Four things differ from section 6 as
written:

- **Findings come as they settle, not in time order.** Each is passed
  at the frame or `events.log` line that ends it, so a run (a rate
  drop, overflows in a row, a first frame left open) comes after
  anything that began within it; `at_us` still places it. Time order
  would hold every finding until each ID's open run ended, and the
  check's memory would then follow what's wrong with a recording, not
  its IDs. The viewer places findings by `at_us`.
- **Two more kinds.** `ECUIO_CHECK_LINK` (6) is each `link` line in
  `events.log`, its state in `code` (the room section 6 calls
  `reserved0`, an `ecuio_status_t`) and its episodes in `count`;
  `ECUIO_CHECK_ASLEEP` (7) is each sleep, over the stretch its line
  covers. A finding on a 29-bit ID carries
  `ECUIO_CHECK_FINDING_EXTENDED`, and the summary says why ISO-TP
  sequences went unchecked: `ECUIO_CHECK_ISOTP_FLAGS_UNKNOWN` (the
  header has no `frame flags  complete` line) or
  `ECUIO_CHECK_ISOTP_NO_WIRE` (no frame on those IDs came as it crossed
  the bus).
- **ISO-TP only on ISO 15765-4's IDs:** 0x7DF, 0x7E0 to 0x7EF, and the
  29-bit 0x18DAxxxx and 0x18DB33F1. Another ID's first byte isn't a
  protocol control byte, and a broadcast frame starting 0x2N would read
  as a consecutive frame.
- **The defaults are public:** `ECUIO_CHECK_DEFAULT_GAP_US`,
  `ECUIO_CHECK_DEFAULT_RATE_DROP_X100` and
  `ECUIO_CHECK_DEFAULT_SETTLE_US` in `ecuio/recording.h`, so a front end
  can say what it checked against.

**The defaults, measured.** `havne-cli info` ran over the car
recordings on hand: 16 made through J2534 (28 minutes), 50 through an
ELM327 adapter on the test phone (152 minutes), and the replay fixture
(a copy of one of the J2534 ones with its identifying frames taken out,
5 minutes). Every one is in the older format, which says nothing of its
frame flags, so the ISO-TP kind was skipped in all of them; a car
recording of wire frames is still to come. Rate drops by the multiple,
with the recordings that had any:

| Multiple | J2534 | ELM327 | The replay fixture |
|---|---|---|---|
| 4x | 45, in 3 of 16 | 1,360, in 36 of 50 | 36 |
| 6x | 18, in 3 | 259, in 20 | 16 |
| 8x | 7, in 1 | 174, in 10 | 7 |
| 10x | 4, in 1 | 155, in 4 | 4 |
| 20x | none | 4, in 3 | none |

At 4x most of them are the request loop's own rhythm. A reply ID's
interval follows the requests, and a one-shot request, a slow reply or
a reconnect stretches it 100 to 400 ms past a median of 20 to 40 ms.
What's left at 10x are stalls: 151 of the ELM327 drops come from one
recording whose requests stalled for about 0.4 s again and again, the
others are single stalls of 0.35 to 1 s, and the fixture's 4 are about
0.56 s against its 39 ms median. **The rate drop's default is 10x.**

Gaps over 1 s: 3 in the J2534 recordings (1.2 to 31 s) and 20 in the
ELM327 ones (1.8 to 305 s), each a stretch with no frame at all; at
2 s, 2 and 18. **The gap stays 1 s.** No recording has a late ID at any
settle time from 2 s to 30 s, so nothing sets it, and **it stays 2 s**.
