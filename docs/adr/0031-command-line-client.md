# ADR-0031: The command-line client, `havne-cli` streams a session as JSON lines over the C ABI

- **Status:** Accepted.
- **Date:** 2026-10-29. Implemented with this record.
- **Builds on:** [ADR-0001](0001-qt-free-core-c-abi.md) (a Qt-free core
  behind a C ABI, front ends as skins); [ADR-0007](0007-uri-selected-transports.md)
  (one URI selects the transport); [ADR-0018](0018-connect-failure-reporting.md)
  (a refused connect says why); [ADR-0019](0019-session-info.md)
  (what a session runs on); [ADR-0024](0024-decode-on-the-worker.md)
  (decoded values from the worker).

## Context

The C ABI has one caller, the Qt app. Its promises (opaque handles,
POD structs, callbacks on the worker, append-only codes) are checked
from inside: `test_c_abi.c` compiles the headers as C, the unit tests
link core directly and reach `internal.h`, and the ABI check
(`scripts/abi-check.sh`) compares the library's symbols and types with
the last release. Nothing drives core as an outside program would,
through the public headers alone, and nothing pins what such a program
sees: the frames, the values, the session's description, the codes and
the words of a refusal.

A second caller is also wanted for its own sake:

- **Scripts and pipelines.** A recording or a live session as text a
  shell, `jq` or a Python script reads, with no GUI and no Qt.
- **Linux without a desktop.** A Raspberry Pi with a CAN hat, a
  headless logger, CI. The SocketCAN transport (v0.3.4's listening
  prototype) is easiest to run and to show from a terminal.
- **The bindings that follow.** v0.4.0's Python bindings call the same
  ABI in process; the CLI settles first what a stream of a session
  looks like outside the app, and its tests become theirs to match.

v0.3.4 is done when the CLI starts a session, streams frames and exits
cleanly.

## Decision

### 1. A program over the public headers only

`havne-cli` (`cli/`, C++17) links `ecuio-core` and includes only the
public headers (`ecuio/core.h`, `profile.h`, `signals.h`, `serial.h`,
`j2534.h`), never `internal.h` or anything in `core/src`. It has no Qt
and no other dependency, so it builds wherever core does, in the
core-only tree too (`HAVNE_BUILD_CORE_ONLY`): CI's Qt-free jobs build
and test it on Linux, Windows and macOS. It isn't built for Android or
iOS.

It opens one session and owns it, so the session API stays
single-caller (Known Issue #6 is unchanged).

### 2. Commands

```
havne-cli stream <uri> [--values] [--count N] [--seconds S]
havne-cli devices
havne-cli --version | --help
```

- **`stream`** creates and starts the session the URI names, exactly
  as the app's Settings would (`mock://`, `replay://`, `elm327://`,
  `j2534://` on Windows), and writes it to standard output: frames,
  and with `--values` the values core decodes through the session's
  profile. It ends at Ctrl+C (SIGINT, SIGTERM; on Windows the console's
  Ctrl+C, Ctrl+Break and close), after `N` frames, after `S` seconds,
  or when a replay without `loop=1` has delivered every frame its
  recording holds (`replay_frames` in the session info), whichever
  comes first.
- **`devices`** lists what a URI can name: the serial ports
  (`ecuio_serial_ports`) and, on Windows, the J2534 adapters
  (`ecuio_j2534_discover_devices`). It opens nothing. Its output is
  schema 1 too (§3): `start`, then a `port` or `j2534` line per device;
  no `end` line, since a list has no stream to end, and exit 0 says it
  is whole.

The CLI adds no request of its own. A session asks what its profile
and URI make it ask, as in the app, and core's request path is
read-only by construction (ADR-0015).

### 3. The output: JSON lines, schema 1

Standard output holds only JSON lines, for both commands: one object
per line, UTF-8, `\n` line ends on every platform. Every object has
`"type"`. Messages for a person go to standard error, the usage text
after a usage error included, so a usage error writes nothing to
standard output. The exceptions are `--help` and `--version`: asked
for, they print plain text there.

| `type` | When | Fields |
|---|---|---|
| `start` | first | `schema` (1), `core` (`ecuio_version_string`) |
| `session` | after start | from `ecuio_session_get_info`: `backend` (`"mock"`, `"replay"`, `"elm327"`, `"j2534"`), `profile`, `protocol_id`, `baud`, `channels` (`[{"rx":"0x7E8","tx":"0x7E0"}]`), and those the flags say are present: `vbatt_mv`, `firmware`, `dll_version`, `api_version`, `pending_off`; for a replay, `replay` (`frames`, `span_ms`, `speed_milli` with 0 for `max`, `loop`) |
| `signals` | with `--values`, after `session` | the session's set: `[{"id":1,"name":"RPM","unit":"rpm","decimals":0}]` |
| `frame` | each frame | `t_us`, `id` (`"0x7E8"`), `ch`, `flags`, `data` (hex pairs, `"41 0C 1A F8"`, as `frames.csv` writes them) |
| `values` | with `--values`, each batch, after its frame | `t_us`, `id`, `values`: `[{"id":1,"value":2512.5}]`, or `"text"` for the text kinds |
| `health` | when the link state or its episode count changes | `code`, `message` (`ecuio_status_string`), `episodes` |
| `dropped` | directly before the first line that got into the ring after lines were lost (§4) | `frames`, `values`: the lines lost since the last report |
| `end` | last | `reason` (`"interrupted"`, `"count"`, `"seconds"`, `"replay-end"`), `frames` (written or dropped; with `--count`, at most `N`), `dropped_frames`, `dropped_values`: the stream's totals |
| `error` | instead of `session`, when the connect fails | `code`, `message`, `detail` (`ecuio_last_error_detail`) |
| `port` | `devices`: each serial port | `name`, `device` (Windows' driver device, else `""`), `uri` (`"elm327://COM5"`) |
| `j2534` | `devices`, Windows: each installed adapter | `name`, `dll`, `arch` (`"x86"`, `"x64"`, `"unknown"`), `uri` (`"j2534://<name>"`) |

- **Numbers** read back as the same double: 15 significant digits
  when they do, which is most values and the short form of a reading
  with a few decimals (`2512.5`), else 16 or 17. A NaN or an infinity
  is `null`, which JSON has in their place. Values are core's, in its
  units (metric); conversion stays a front end's choice (ADR-0022)
  and isn't in the skeleton.
- **Identifiers and data** are text, in the forms `frames.csv` and
  the activity log use, so a line and a recording's row can be
  compared by eye.
- **Growth:** later versions add types and fields; a reader ignores
  what it doesn't know. A field's meaning never changes and none is
  removed within a schema. A change that breaks a reader raises
  `schema`.

### 4. Threads: the worker formats, the main thread writes

The callbacks run on the worker, where blocking delays the session
(`core.h`). Each callback formats its line into a buffer on its stack
and copies it into one byte ring the CLI allocates before the session
starts; the main thread drains the ring to standard output and flushes
once per drain, and polls the error state every 100 ms for `health`.
Nothing on the worker allocates, writes to a file or waits on output.
Frame and values lines share the ring, so they come out in dispatch
order: a frame, then its values (ADR-0024).

**The ring** is 1 MiB of text, and a line at most 32 KiB (the line
buffer). A frame line with a classic CAN payload is 85 to 95 bytes,
and an `obd2` values line about 90 (measured over the replay fixture:
83 and 88 on average; a mock batch of nine values, about 310), so the
ring holds some 6,000 frames with `--values` (11,000 without): about
1.5 s of a fully loaded
500 kbit/s bus (some 4,000 frames a second) with the output stalled,
and minutes of a request-paced session (26 to 36 replies a second on
the car). Sized for a terminal that stalls briefly; a reader that
can't keep up for long loses lines, by design.

When the ring is full a live session's line is dropped and counted; a
slow terminal costs lines, never the session's timing. The worker
writes the report: the next push that finds room for both its line and
a `dropped` line puts the `dropped` line in first, under the same lock,
so the report sits directly before that line and is never itself
dropped. A push without room for both is dropped too and adds to the
count. Lines lost after the last line that got in (at the end, or when
the output closed) have no `dropped` line; the `end` line's totals
count every drop, and are the ones to trust.

A replay waits for room instead: a recording has no bus to fall
behind, and its output is then complete at any speed, `speed=max`
included, which the tests and batch processing need. The main thread
keeps draining while the session closes, so a waiting push always
completes; the waits end early only when the output has failed and
nothing drains the ring any more.

A line longer than the line buffer (a text value near
`ECUIO_DECODE_TEXT_MAX`) carries its text cut short and `"cut":true`.

### 5. Ending cleanly, and exit codes

The signal handlers only set a flag. The main thread sees it within
100 ms and closes the session on a second thread, draining the ring
while close joins the worker (a replay's last push may be waiting for
room), then writes what is left and the `end` line, destroys the
session and exits. A closed standard output (`havne-cli stream … |
head`) ends the stream the same way, with no message and no `end`
line, and the ring stops its waits, since nothing drains it any more.
How a closed pipe shows differs by platform:

- **POSIX:** SIGPIPE is ignored, so the write fails with `EPIPE`.
- **Windows:** `WriteFile` to a pipe whose reader has gone fails with
  `ERROR_NO_DATA`, which the CRT reports as `EINVAL`, not `EPIPE`, so
  the CLI reads `GetLastError()` after the failed write and takes
  `ERROR_NO_DATA` and `ERROR_BROKEN_PIPE` as closed (measured
  2026-10-29: Git Bash's `| head -1` and PowerShell's
  `| Select-Object -First 2`, both `ERROR_NO_DATA`, both exit 0 with
  nothing on standard error). Any other failure is an error (exit 1).

A pipe still open whose reader stops reading isn't closed: the writes
block, the ring fills and a live session drops lines, as above.

| Exit | Meaning |
|---|---|
| 0 | the stream ended: any `end` reason, or its output closed |
| 1 | the CLI itself failed: out of memory for the ring, or a write failing for another reason than a closed pipe (a full disk) |
| 2 | usage: an unknown command or option, a missing URI |
| 3 | the connect failed: an `error` line says why |

### 6. Tests: the ABI as an outside program sees it

- **Unit** (`test_cli`, Qt-free): the line formatting (string escaping,
  control characters, UTF-8 passed through, invalid UTF-8 replaced,
  hex, numbers that read back exactly, NaN as `null`, the cut line) and
  the ring (order, the drop count, a replay's wait ended by stop).
- **Golden runs** through `tests/golden.py`, the CLI as a child process
  and its whole output compared with a committed file, timing keys
  ignored:
  - the replay fixture (`tests/fixtures/replay/2026-10-25-engine-start`)
    through `obd2` at `speed=max` with `--values`: the frames and
    values the real car's session decodes to, from outside the process.
    The golden file holds the first few hundred frames (`--count`), and
    a second run to the recording's end compares its last lines only
    (a `--last N` option for `golden.py`): the `end` line's
    `replay-end` and the frame count. The whole run as a golden file
    would be about 1 MB. That second run also proves the replay's
    wait: `HAVNE_CLI_RING_BYTES` (read by the CLI for tests only)
    shrinks the ring to a few lines and `golden.py --slow` reads like a
    slow terminal, so the ring fills at once and a replay that dropped
    instead would end with thousands of `dropped_frames`;
  - `mock://` with `--values --count`: the drive cycle's first frames
    and their values (`t_us` ignored);
  - the refusals: a malformed URI (3, `BAD_URI` and its detail) and
    an unknown profile (3, `UNKNOWN_PROFILE`).
- **Usage** (`CliUsageTest`, `cmake -P`, no Python needed): no
  arguments, no URI, an unknown command or option, a zero count, two
  URIs, each exit 2 with nothing on standard output.
- **A stop from outside** (`CliSignalTest`): on POSIX, SIGINT, then
  SIGTERM, to a running `mock://` stream (ctest under a shell); on
  Windows, Ctrl+Break to a `mock://` and a replay stream, each started
  in a process group of its own so the event reaches it alone (Ctrl+C
  can't be sent to a group; Ctrl+Break goes through the same handler),
  skipped where the test has no console to send it through. Each must
  exit 0 with an `end` line whose reason is `interrupted`. Ctrl+C at a
  console and closing the console window are left to the same handler,
  untested.

A change to what core shows a caller (a frame field, a decode, a
code, a detail's words) then fails a golden run, and goes in only with
the golden file's diff in the same commit: the durability check the
roadmap asks of the CLI.

### 7. Shipping

- **Windows:** the zip carries `havne-cli.exe` beside `havne.exe`, so
  it finds the two surrogates there (the runtime location rule), and
  the release's checks cover it (`stage.ps1`: its imports, its version
  resource, and a short `mock://` stream from the staged folder).
- **Linux:** the core-only deb that v0.3.4's packaging item adds,
  which Ubuntu and Kali (Debian-based) install; and a tarball of the
  binary alone (`havne-cli-<version>-linux-x86_64.tar.gz`, libstdc++
  linked statically, built on the oldest Ubuntu CI runs, as the
  AppImage is), which runs on any distribution with that glibc or
  newer, Arch among them. Both go in `SHA256SUMS` and the provenance.
  An AUR package that repackages the tarball, an apt repository and
  the CLI inside the AppImage are left for later.
- **macOS:** with a macOS download, when there is one.

## Alternatives considered

- **The CLI in C.** It would prove the headers serve a C caller end to
  end, not only compile as C. But C11 threads are missing on macOS and
  partial on MSVC, so the ring and the signal handling would need
  platform code that C++17's library gives for free, and `test_c_abi.c`
  already holds the headers to C. The CLI uses only `extern "C"`
  entry points either way.
- **Frames as the shared ring of `ecuio_frame_t`, formatted on the
  main thread.** A frame is 4,120 bytes, so a ring of a useful depth
  is megabytes, and values would need a copy format of their own
  (their text is valid only during the call). Formatting on the worker
  keeps one ring of text, sized by the output's rate, with both line
  types in order.
- **Values only, or frames only.** Frames are the CLI's done-when, and
  values are what a script usually wants; `--values` adds the second
  without a second mode.
- **A binary output or a schema language.** JSON lines are what
  `jq`, Python and every log tool read with nothing installed, and the
  golden comparator already reads them. A binary stream belongs to the
  log-viewer work, if a recording format ever needs one.
- **Blocking a live session's worker when the output is slow.** Exact
  output, but a terminal held open with Ctrl+S would stall a session's
  requests and trip its link health. Dropping with a count keeps the
  session honest; replay is the one source where waiting harms nothing.

## Consequences

- **Good:**
  - A second caller of the ABI, outside the process boundary in
    spirit: public headers only, a ring between the worker and output,
    its contract pinned by golden files.
  - The real car's recorded session, decoded end to end through the
    CLI, becomes a regression check that runs on every CI platform.
  - SocketCAN's prototype and the simulated ECU have a front end on a
    headless Linux machine from the start.
- **Bad:**
  - Schema 1 is a public format from its first release: its fields are
    kept or the schema is raised.
  - A change to the fixture, the `obd2` set or the mock's drive cycle
    rewrites a golden file, and its diff has to be read, not just
    accepted.
  - Formatting on the worker costs it time per frame: the replay
    fixture's 7,074 frames with `--values` at `speed=max` take about
    30 ms in all on the development machine, writing included, so a
    few microseconds a frame against a bus's milliseconds.

## Appendix: 2026-10-29, SocketCAN

[ADR-0032](0032-socketcan-listening.md) grows schema 1 the way §3
allows, adding without changing anything a reader already reads:

- **`session`:** `backend` takes `"socketcan"`, and `listen_only`
  (`true`) appears when the interface is a CAN controller in
  listen-only mode.
- **`devices`:** a `can` line per CAN interface (Linux): `name`, `kind`
  (`"can"` or `"vcan"`), `up`, `listen_only`, `fd`, `bitrate` (0 for
  `vcan`) and `uri` (`"socketcan://can0"`).
- **A third command, `play <uri> --to <vcan>`** (Linux), writes a
  session's frames to a virtual CAN interface. Its output: `start`, the
  source's `session`, a `play` line (`to`, `fd`), and an `end` line
  whose `frames` is joined by `sent` and `skipped`; no `frame` lines.
  It ends as `stream` does, and its exit codes are §5's. Elsewhere it
  is a usage error that says it needs Linux.

## Appendix: 2026-10-29, a sleep

[ADR-0033](0033-session-time-counts-sleep.md) grows schema 1 by one
type. A `slept` line (`us`) comes when the machine's slept count
(`ecuio_clock_slept_us`) rose a second or more within one 100 ms poll:
the stream's frames have a gap that long. `--seconds` counts on the
same clock as session time, so a sleep counts toward it.

## Appendix: 2026-10-30, a replay that stops early

A replay whose recording changes under it stops short of its last frame
and says so through the error state (`ECUIO_ERR_RECORDING_CHANGED`,
[ADR-0023](0023-replay-transport.md)'s appendix of 2026-10-26). `stream`
and `play` counted to `replay_frames` and waited for the rest, which
never came: a script piping a replay sat there for good. Both now end
on that code, with a fifth `end` reason, `"recording-changed"`, after
`stream`'s `health` line (code 16), and exit 0 as for any `end` reason
(§5): the stream ended, and its health line says why it ended short.
A new value of `reason` grows schema 1 the way §3 allows; a reader that
knows only the four first ones still reads the line.
`CliReplayTest.ACutRecordingEndsTheStream` (`tests/cli_recording_changed.py`)
cuts a recording half a second into its play.

## Appendix: 2026-10-30, trouble codes

[ADR-0037](0037-reading-trouble-codes.md) grows schema 1 by one type.
`stream` writes a `diagnostics` line for an ECU each time its read
completes (`ECUIO_DIAG_COMPLETE`, at a generation not yet written):
right after `session` for a read that finished before the stream began
(the mock's, at create), then from the main thread's drain as reads
complete, a Read again's included. The main thread polls the report,
so the worker's callbacks (§4) are unchanged. Its fields:

- `rx` and `generation`;
- `readiness`: `outcome`, and when answered `mil`, `dtc_count`,
  `compression` and `monitors`, each a `name` and `complete`;
- `stored`, `pending` and `permanent`: `outcome`, and when answered
  `reported` (the count the ECU gave) and `codes`, each as it's
  written (`"P0301"`);
- `freeze`: `outcome`, and when answered `code` (`null` when none is
  stored) and `values`, each an `id`, the `name` and `unit` of the
  `obd2` def that decoded it, and its `value` in that unit.

An `outcome` is `"answered"`, `"refused"` (with `reply`, the negative
reply in `frames.csv`'s form, `"7F 0A 11"`), `"unanswered"` or
`"unasked"`. The names come from core (`ecuio_dtc_format`,
`ecuio_monitor_name`, the `obd2` set), through the public headers as
§1 requires. `cli/diagnostics_line.h` formats it; `CliDiagnosticsTest`
covers each outcome, and the mock's line is in `CliGoldenTest`'s
`mock.jsonl`.

## Appendix: 2026-10-31, absent signals

[ADR-0039](0039-full-service01-coverage.md) §6 grows schema 1 by one
type. With `--values`, `stream` writes an `absent` line (`ids`, the
signal IDs the `signals` line names) whenever core's list of signals
the session's frames come without changes
(`ecuio_session_get_absent_signals`): a def whose key matched three
frames that ended before its field, while none carried it, as the test
car's one-byte PID 55 replies leave bank 3's field. The main thread
polls it in its drain, as it does the diagnostics, so the worker's
callbacks (§4) are unchanged; a list that empties again writes
`"ids": []`. `CliReplayTest.AnAbsentFieldIsNamed`
(`tests/cli_absent.py`) replays such replies.
