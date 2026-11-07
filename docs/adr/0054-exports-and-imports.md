# ADR-0054: Exports and imports: scrubbed by construction, PCAP out, candump and CSV in

- **Status:** Proposed.
- **Date:** 2026-11-06.
- **Builds on:** [ADR-0001](0001-qt-free-core-c-abi.md) (the Qt-free
  core, the C ABI); [ADR-0006](0006-frame-abi-v0.3.md) (the frame and
  its flags); [ADR-0021](0021-vehicle-identification.md)
  (identification: Service 09 and ISO 14229-1's identification DIDs);
  [ADR-0023](0023-replay-transport.md) (replay);
  [ADR-0027](0027-vehicle-profiles.md) (vehicles and their labels);
  [ADR-0031](0031-command-line-client.md) (`havne-cli`);
  [ADR-0049](0049-recordings-in-core.md) (recordings, the reader, the
  spreadsheet export's rules, section 5's frames path, and "when
  sharing lands, the same guard"); [ADR-0051](0051-signal-files.md)
  (signal files, text signals); [ADR-0053](0053-python-bindings.md)
  (Python).
- **Leads to:** v0.6.0's exports and imports in core, `havne-cli
  export` and `import`, the viewer's Export, the health report's
  export.

## Context

A recording is the most useful thing a user can share (a forum post, a
bug report, a Wireshark session) and the most personal. Its files hold
the VIN, the calibration IDs and their verification numbers, the ECU's
serial and part numbers, a Bluetooth address or a local path in the
session's URI, and the vehicle's label in the folder's name and the
activity log. The project's own rule for committed evidence
(`docs/bench-sessions/`) removes all of them, and its tools search for
them afterward. That works for a maintainer who knows what to look for;
a user exporting a file shouldn't need to.

v0.6.0 adds three ways out and two ways in:

- **Out:** a scrubbed copy of a recording folder; CSV of signals over a
  time range; PCAP for Wireshark. ADR-0049's spreadsheet export follows
  the same guard.
- **In:** a candump log (can-utils' format) as a recording; another
  logger's CSV of timed signals, through a column map.

## Decision

### 1. Scrubbed by construction, then checked

An export doesn't copy text and then search it for what to remove.
Each field it writes is classified first, and only a class known to be
safe is written as it is:

| Class | Where it occurs | In a scrubbed export |
|---|---|---|
| Identification replies | frames answering Service 09 (any item), and `62` replies to DIDs `F180`-`F1FF`, ISO 14229-1's identification block (ADR-0021 reads `F187`-`F195` and `F190` there; the export takes the whole block) | the frames are left out, every segment of a segmented reply with them, and counted |
| Text signals | a signal of the text kind (the VIN, a calibration ID, a manufacturer DID read as text) | the signal's name and unit stay; its values are left out and counted. An enumeration's labels come from the signal's definition, not the car, and stay |
| The session's URI | `events.log`'s header, the activity log | its scheme and profile stay; a host, port, Bluetooth address, path or DLL path becomes `<address>` or `<path>` |
| The vehicle's label | the folder's name, `Vehicle:` lines | `<vehicle>`; the folder is named by its time stamp alone |
| Activity lines | `events.log` after the header | a line whose kind the export knows is written with its fields classified; a line of an unknown kind is left out and counted |
| Numbers | `signals.csv`'s number values, frames other than the above | as recorded |

Frames that can't be attributed fail closed: a segment on a
diagnostic reply ID (`0x7E8`-`0x7EF`, `0x18DAF1xx`) whose message
can't be reassembled, because it's torn or its first frame came before
the recording started, is left out. Raw frames are reassembled with
core's ISO-TP, the path ADR-0049 section 5's frames decode uses, so a
listener's recording of a VIN read is found as one message.

**Then checked.** Before an export's file is renamed into place, core
collects the recording's own identity values (each identification
reply's text and bytes, each text signal's values) and searches the
written bytes for every one of them, as text and as hex in each of the
forms the export writes, and for any 17-character VIN-shaped token.
One match fails the export with words naming the class, never the
value, and leaves no file. This catches what classification can't: a
manufacturer's DID that echoes the VIN in a number signal's frames.

**A raw export** (the recording as it is) is a separate, explicit
choice: `--raw` in `havne-cli`, a named option in the app, `raw=True`
in Python. It's never the default, and its file says so in its own
metadata where the format has room for it.

Every export reports what it left out: counts per class, written to the
file where the format allows (a pcapng comment, a CSV comment line, the
folder's `events.log` header) and returned to the caller.

### 2. Where it lives

In core, behind the C ABI (append-only additions), so the app, the
CLI and Python get the same exports from the same code:

- `ecuio_export(recording, format, options, from_us, to_us, out_path,
  &report)`: a format enum (`FOLDER`, `CSV`, `PCAPNG`, and the
  spreadsheet's `XLSX` when it's built), options flags (`RAW`, and per
  format), a time range in session time (0, 0 for all), and a report
  struct with its reserved room.
- `ecuio_import(format, in_path, column_map, out_folder, &report)`:
  `CANDUMP` or `CSV`. The column map is a small JSON document (section
  5), parsed by core's strict JSON parser.

The writers are deterministic: the same recording and options give the
same bytes (ADR-0049's rule for the spreadsheet), so a test compares
files, and so does a user.

`havne-cli export <recording> <out> [--format folder|csv|pcapng]
[--from <s>] [--to <s>] [--raw]` and `havne-cli import candump|csv
<in> <folder> [--map <json>]`; the viewer's Export takes the range from
the chart's view. The health report's export (the roadmap's "one
scrubbed export") is `FOLDER`'s classification applied to the
report's lines.

### 3. PCAP out

- **pcapng,** one section, one interface per bus the recording saw,
  `LINKTYPE_CAN_SOCKETCAN` (227), so Wireshark's SocketCAN dissector and
  its ISO-TP and UDS dissectors read the frames. CAN FD frames carry
  the FD flag. Times are the recording's UTC start plus each frame's
  session time, in microseconds (the interface's `if_tsresol` 6).
- **Re-segmented frames are marked.** A frame core stored reassembled
  (`ECUIO_FRAME_FLAG_REASSEMBLED`: J2534 and ELM327 deliver ISO-TP
  messages whole) is written as the First and Consecutive Frames a
  sender would have sent, with the request's Flow Control between, each
  carrying a packet comment that says it was made up from a reassembled
  message. A frame the recording holds as it arrived (SocketCAN, a
  listener) is written as it is, with no comment.
- **The section's comment** names Havne's version, the export's class
  counts and whether it's scrubbed or raw.

### 4. candump in

- **The format** is can-utils' log, as `candump -L` writes it and
  `canplayer` reads it: one frame a line, a time stamp in seconds, the
  interface, and the frame in `cansend`'s notation (the ID's width by
  its digit count, `#` for classic CAN, `##` for CAN FD with its flags,
  `R` for a remote frame). The grammar is taken from can-utils' own
  parser, cited in the source, and the tests round-trip real `candump
  -L` output. Anything else on a line is refused with its line number.
- **The parser** is in core, strict, bounded (a line's length, a file's
  frame count), with a fuzz target and synthetic fixtures; no recording
  from a real car is a fixture.
- **What it makes** is a recording folder in core's format:
  `frames.csv` with the frames as they arrived (never `REASSEMBLED`),
  an `events.log` header naming the import as its source, the first
  frame's time as the UTC start, and no `signals.csv` values of its own.
  Its values come from ADR-0049 section 5's frames path: open it
  through a profile and it plots, replays and exports like any other.
- **Interfaces** become buses in the recording; a log with several
  keeps them apart.

### 5. CSV in

Another logger's timed signals, signals only (no frames):

- **The column map** names the time column and its unit (seconds,
  milliseconds, or an ISO 8601 time of day with a date), and for each
  imported column a signal name, unit and kind (number or text).
  Columns not in the map are ignored and listed in the report.
- **The parser** is RFC 4180 CSV (quoted fields, embedded commas),
  strict and bounded like candump's, with its own fuzz target.
- **What it makes** is a recording folder with `signals.csv` and the
  header, which the reader opens as it opens any recording.

### 6. Tests

- **Scrubbing:** a synthetic recording carrying a made-up VIN (the
  fixtures' `FAKE` form), calibration IDs, a serial DID, a Bluetooth
  address in its URI, a path and a vehicle label, raw and reassembled,
  exported in each format: none of them in the output by the export's
  own check and by an independent search in the test; each class's
  count as expected. Mutants: a class written as it is, a segment of a
  segmented VIN reply kept, the check skipped; each fails a test.
- **PCAP:** read back by a parser in the test (the standard library's
  `struct`, so CI needs no package) and, where tshark is installed (the
  Linux job, WSL), by `tshark -r` with its CAN and ISO-TP dissectors,
  the re-segmented frames reassembling to the stored message.
- **candump:** a log written by `candump -L` on `vcan0` from the
  simulated car round-trips: imported, opened through `obd2`, the
  values equal to a direct session's. Fuzz targets for both parsers.

## Done when

An export of a recording passes the leak check; a recording opens in
Wireshark with its ISO-TP reassembled; a candump log replays as a
recording; a CSV from another logger plots in the viewer.

## Alternatives considered

- **Search and replace after writing:** what the project's own
  committed-evidence tools do. It misses a value it doesn't know the
  shape of, and a user can't add to its list. Kept as the second check,
  not the method.
- **Scrubbing in each front end:** three copies of the rules (app, CLI,
  Python) that drift. Core holds them once.
- **libpcap's format (not pcapng):** simpler, but no comments, so a
  re-segmented frame couldn't be marked and the file couldn't say it
  was scrubbed.
- **A raw export by default, with scrubbing as an option:** the easy
  path would be the one that leaks.

## Consequences

- A scrubbed export of a recording made by a listener on a busy bus
  can lose more frames than the identification itself: every
  unattributable segment on a diagnostic ID. The report says how many.
- Text signals don't survive a scrubbed export. A user who needs one
  (a gear indicator read as text) exports raw, knowingly, or defines
  it as an enumeration.
- Core's ABI grows by two functions, their enums and their report
  struct; the CLI by two commands.
- The import formats become inputs from strangers, so their parsers
  carry fuzz targets from the start.

## Appendix: the C API as built (2026-11-06)

What v0.6.0 builds of this decision: the pcapng export and the candump
import, in core and `havne-cli`. The folder and CSV exports and the CSV
import keep their place in the API and answer
`ECUIO_ERR_NOT_IMPLEMENTED` until they're built.

**The calls** take option structs in place of section 2's argument
lists, so each can grow through its `reserved` words:
`ecuio_export(options, report)` names the recording by its folder, and
`ecuio_import(options, report)` the folder it makes (`recording.h`).
An unknown format, flag bit or nonzero reserved word is
`ECUIO_ERR_INVALID_ARGUMENT`. The export's classes are named,
`ECUIO_EXPORT_LEFT_OUT_IDENTIFICATION` (0), `_UNATTRIBUTABLE` (1),
`_TEXT_VALUES` (2), `_ACTIVITY` (3) and `_VIN_SHAPED` (4, the check's
alone, so its count stays 0), indexes of the report's `left_out`; a
failed check's class is `failed_class`, the index plus one, 0 for none.
The report is 120 bytes, `failed_class` at 80, the import's report 80,
both static-asserted.

**One recording is one bus.** A candump log with several interfaces
imports the one `interface_utf8` names; with none named it is refused,
the detail listing them. This replaces section 4's "keeps them apart":
a recording holds no bus of its own in its frames, so two buses in one
folder would read as one.

**CAN FD's error state indicator** is a frame flag of its own,
`ECUIO_FRAME_FLAG_FD_ESI` (0x200), appended rather than dropped: the
SocketCAN transport sets it from `CANFD_ESI`, `havne-cli play` writes
it, candump's flags digit carries it (0x2), `frames.csv` and replay
keep the flags as they are, and pcapng writes it in the FD flags byte.

**Classification is by the def, then checked by value**
(`core/src/scrub.{h,cpp}`, shared by every format to come):

- An identification reply's frames are left out, every segment: a
  message (reassembled from the wire, or delivered whole) starting
  `49`, or `62` with a DID in `F180`-`F1FF`.
- A message a text or hex def of the recording's own set applies to (by
  its message ID and mux key: a uds-generic DID, a signal file's read)
  is left out whole, counted as text values. The set is the
  recording's `signals.json`, else uds-generic's built from its URI's
  `did=` list, else its profile by name; an enumeration's frames stay,
  since its labels come from the def. Section 1's table puts text
  signals in a class left out and counted; failing is the check's job,
  for what classification misses.
- A segment on a diagnostic reply ID (`0x7E8`-`0x7EF`, `18DAF1xx`)
  whose message can't be reassembled is left out as unattributable, by
  the reader's own rules (ADR-0049's appendix of 2026-11-06: N_Cr on
  `host_us`, two First Frames on one ID end both).
- **The check runs twice:** over each message the export writes, as it
  was before any segmenting, and over the written bytes as they go out.
  It searches for the recording's identification values (the bytes
  after each reply's header) and `signals.csv`'s text values, each as
  bytes and as hex in upper and lower case, spaced and not, and for any
  VIN-shaped token: 17 of the VIN alphabet with a letter and a digit,
  not all hex, alone between bytes that aren't letters or digits. A
  value under 6 bytes, its padding trimmed, isn't searched for: the
  short identity values (a CVN's 4 bytes) sit in Service 09 frames,
  which classification removes, so the search adds nothing for them
  and only risks chance matches in a large file.

**The pcapng file** (`core/src/pcapng.h`, `core/src/export.cpp`): one
section, one interface (`LINKTYPE_CAN_SOCKETCAN`, `if_tsresol` 6), each
packet as libpcap captures SocketCAN's frames (16 bytes classic, 72 FD
with `CANFD_FDF` set, the ID and its flags big-endian), our own
transmits marked outbound (`epb_flags`). Times are the `utc` line's
start plus each frame's `host_us`, the clock the start is stamped
against. A message delivered whole is written as its Single Frame, or
its First Frame, the Flow Control a tester sends on the physical
request ID (`isotp_flow_control_id`), and its Consecutive Frames, all at
the message's time, each with a comment saying it was made up. The
section's comment names Havne's version, scrubbed or raw, the frames,
the made-up ones and each class's count. A frame no CAN frame can be (a
message cut short or empty, a classic one past 8 bytes) is left out as
unattributable, raw exports too. `to_us` 0 is the end of the
recording; an output file that exists is refused, as the import refuses
a folder that exists.

**The candump import** (`core/src/candump.h`, `core/src/import.cpp`):
can-utils' `(sec.usec) iface frame [ R| T]`, every form `cansend`
writes, CAN XL refused, as is an FD error frame (SocketCAN's error
frames are classic). A blank line is skipped, as `canplayer` skips one;
any other line that doesn't parse refuses the whole log as `line N:
why`. A frame stamped up to 100 ms before the one before it takes that
one's time, the SocketCAN transport's rule; further back is refused.
The folder is written as `<folder>.partial` and renamed once whole.

**havne-cli:** `export <recording> <out> --format pcapng [--from S]
[--to S] [--raw]` writes `exported` (format, file, scrubbed, frames,
made_up, left_out by class), or `export_failed` (the class) then the
error line, exit 3; `import candump <log> <folder> [--interface
<name>]` writes `imported` (the report's counts). Their words and lines
are `cli/transfer_lines.h`.

**Tests:** `CandumpTest`, `ImportTest`, `ScrubTest` and `ExportTest`
(test_core; each export read back by a pcapng parser in the test, its
made-up frames reassembled by core's ISO-TP to the stored message; a DID
echoing the fake VIN in a segmented reply, delivered whole and as it
crossed the bus, fails the export; a uds-generic recording's text and
hex DIDs are left out and counted, and the same value in a number DID's
frames still fails it); `CliTransferTest` (test_cli);
`CliTransferRunTest.roundtrip`, `.refusals` (Python's `struct`),
`.tshark` (where it's installed: the re-segmented frames reassembled by
its ISO-TP dissector to the stored message) and `.candump` (Linux: the
simulated car on vcan0 logged by `candump -L` while a direct session
reads it; the import's values through obd2 are the session's);
`fuzz_candump`. Mutation-checked 2026-11-06 (35: among them a class
written as it is, a segment of a segmented VIN reply kept, and each
check skipped).
