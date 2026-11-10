# ADR-0054: Exports and imports: scrubbed by construction, PCAP out, candump and CSV in

- **Status:** Accepted. Built (the appendices of 2026-11-06 and
  2026-11-07): the pcapng export and the candump import, frames in
  memory, the CSV export, the folder export, the health report and the
  CSV import.
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

(Note of 2026-11-07: candump ends every line, so a last line with no
line end was cut short, as a copy taken while candump writes is, and is
left out, frame or not, with the line `import  the log's last line has
no line end: cut short, left out` in `events.log`: ADR-0049 section 7's
torn row, as replay and the reader take it. The second pass reads the
lines the first counted and no others: a line added, or the cut line
ended, between the passes is "the log changed while it was read", and
nothing is written. `ImportTest.ALastLineWithNoLineEndIsLeftOut` and
`.ALogThatChangesBetweenItsPassesIsRefused`, the second through
`ecuio_import_test_between_passes` (internal.h, test-only), which runs
a test's hook between the passes. Mutation-checked 2026-11-07 (6).)

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

**Note of 2026-11-07: the scrub's rules widened, the partial's name.**
Each change closes a way an identity could reach a scrubbed file, and
each fails closed:

- A torn segment is unattributable on any `18DA` ID, both directions,
  not only `18DAF1xx` (`scrub::segment_reply_id`; the health report's
  ECUs stay `scrub::reply_id`'s).
- A frame on a reply ID that no adapter reassembled and that is longer
  than classic CAN's 8 bytes (a CAN FD frame, whose ISO-TP escapes the
  reassembly doesn't read) is left out as unattributable; a raw export
  keeps it.
- An identification reply on an ID the reassembly doesn't take (a
  manufacturer module's, a listening capture's) is left out: a Single
  or First Frame whose payload `scrub::identification` takes, and every
  Consecutive Frame on its ID until that ID's next Single or First
  Frame. Its bytes, gathered up to the message's length, join the
  check's identity values, a reply short of its length too.
- A malformed frame inside a message tears the whole message, so its
  later segments are torn too, never pending into one no frame list
  holds.
- A text signal's value written as hex pairs (`ECUIO_VALUE_HEX`'s form)
  is searched for as its bytes as well as its text.
- The VIN shape is also any 17 characters of the VIN alphabet inside a
  longer run of letters and digits whose ninth is their check digit
  (`ecuio_vin_decode`'s `ECUIO_VIN_CHECK_MATCHES`), with a letter and a
  digit, not all hex: a VIN that a letter or digit either side hides (a
  DID's last byte before the text). Other text of that shape matches
  about one window in eleven, which is why a run of exactly 17 keeps
  the shape rule alone. Measured 2026-11-07 over the test car's
  recordings on hand, 243 in all (64 from the laptop, 185,956 frame
  rows, and 179 from the phone): every message the frames carry and
  every frames.csv, signals.csv and events.log, as bytes. No window
  outside an identification reply matched; the laptop's identification
  replies, which classification leaves out, held the VIN 1,529 times.
  In each piece the recording's own values are searched before the
  shape, so a VIN the recording holds is reported as its class.
- A recording whose `signals.json` is there but doesn't load (or can't
  be opened) refuses a scrubbed export of every format, saying so and
  why: the set it names may hold text defs no other set has, so falling
  back to the profile could keep a text signal's frames. A raw export
  doesn't read it, and the health report still falls back.
- An export, a health report and an import write under a name of their
  own, `<out>.partial-<pid>-<n>` (`folder_files.h`'s `partial_name`),
  made with an exclusive create and a new name tried while one exists,
  so another export's partial is never in the way and never touched.
  Every way out, running out of memory included, removes it.

**Note of 2026-11-07: ISO-TP read in both layouts, joined on any ID, the
rename that never replaces.** Each change fails closed:

- The other-ID rule above reads each frame in both addressing layouts,
  normal (the PCI at byte 0) and extended (byte 0 a target address, the
  PCI at byte 1), classic or CAN FD (ISO 15765-2:2016's Single Frame
  escape, its length in the next byte, and First Frame escape, 12 bits
  of zero and then 32), through `frame_reassembly.h`'s `tp_read`. A frame
  is an identification reply's when either layout reads it as one. The
  match is on the payload after the PCI, never elsewhere in the frame.
  Only the forms the standards give are read: no extended layout on ISO
  15765-4's IDs (`iso15765_id`), which that standard gives normal
  addressing, so a frame there that isn't on the wire is an adapter's
  payload with no PCI; and a Single Frame, in either layout, in the
  escape's form only in a frame longer than 8 bytes and in the nibble's
  only in one of 8 or less, as ISO 15765-2:2016 gives them.
- A Single or First Frame whose payload `scrub::identification` takes is
  one whatever its length says. A length that can't be right (short of
  what the frame holds, past `ECUIO_FRAME_DATA_MAX`, or an escape that
  reads as neither) is followed all the same: each Consecutive Frame on
  its ID and layout until a frame there that isn't one, or until
  `ECUIO_FRAME_DATA_MAX` bytes, the most an ISO-TP message holds, so one
  false match on a busy ID leaves out at most that many bytes of it. Its
  bytes join the identity values.
- A reply that a frame on its ID ends before it is whole (a new Single or
  First Frame, any frame that isn't a Consecutive Frame) gives its bytes
  so far to the identity values, as one the recording ends does.
- The second check also joins ISO-TP-shaped frames on any ID that the
  reassembly doesn't take (`any_id_messages`), in both layouts, a First
  Frame's payload with each Consecutive Frame after it until its length,
  a frame there that isn't one, or the end, and checks each message with
  a written frame whole: a manufacturer's reply that echoes the VIN,
  segmented on its own ID, fails the export though no frame of it holds
  the VIN whole. Nothing more is left out. The pcapng and the folder
  export do it, and `ecuio_scrub_frames`.
- The rename into place (`folder_files.h`'s `rename_to`) never replaces a
  name taken meanwhile wherever the file system can say so: `MoveFileExW`
  without replacing on Windows, `renameat2` with `RENAME_NOREPLACE` on
  Linux, `renamex_np` with `RENAME_EXCL` on macOS and iOS; where those
  aren't there, a file by `link` and then `unlink`, a folder onto an
  empty folder made first as a reservation (removed again if the rename
  fails). Where the file system can do none of these (some of Android's
  shared storage), the name is checked as the rename starts: a file or
  an empty folder made there in between is replaced, so no-replace
  holds where the file system allows it.

Measured 2026-11-07 over the recordings on hand, 245 in all (23
listening captures with 27,944 frame rows, 75 sessions that request
with 333,587, and 147 older recordings with no profile line with
408,679), through `ecuio_scrub_frames` of the shared core before these
changes and after: no frame more left out and no export refused. The
other-ID rule takes no frame in either layout there; the listening
captures' frames are all on the wire, and the extended layout matters
for a manufacturer's extended-addressing IDs, which none of these
recordings uses.
Read without the limits above on the forms, the rules took 2,003
Single and First Frames that aren't identification,
none of them nor the frames followed after them holding a VIN-shaped
run: 1,872 adapter payloads on `0x7E8` read in the extended layout
(Mode 01's `41 05 49` as a Single Frame of `49`; a reply read as a
First Frame then took the replies after it) and 131 CAN FD frames of
the mock's whose byte 1 is `49`, read as nibble Single Frames. Their
bytes, taken for identity values, refused one export.

What the scrub doesn't find, by design:

- A VIN with no check digit in place (a manufacturer's own, or one from
  a market that doesn't use it) glued to letters or digits either side:
  the shape rule takes a run of exactly 17, and the window rule a run
  whose ninth character is its check digit.
- A VIN a module broadcasts with no ISO-TP form (a counter byte first,
  then the VIN spread over frames): no frame holds it whole, and nothing
  joins the frames.
- A typed marker label of 17 or more characters of the VIN alphabet can
  refuse a folder copy, at the window rule's rate (about one window in
  eleven): the label is searched as the shape like any other text.

## Appendix: frames in memory, the Log page's export (2026-11-06)

The Log page's Export CSV writes the frames its ring holds, which no
recording holds, so it can't go through `ecuio_export`. Core scrubs them
by the same rules instead, through one more append-only call:
`ecuio_scrub_frames(count, get, user_data, set, flags, keep, report)`
(`recording.h`, `core/src/scrub_frames.cpp`).

- **The frames come one at a time.** A frame is 4 KB (its data holds a
  whole ISO-TP message), so an array of the ring's 10,000 would take
  about 41 MB, too much on a phone. Core calls `get` for each index, in
  arrival order, into one frame of its own, twice: the classification
  pass, then the check's. Both passes must see the same frames, so the
  page copies the ring's rows (each about a hundred bytes) before the
  call and builds each frame from that copy. `get` runs on the caller's
  thread before the call returns, so a binding can pass a plain foreign
  function (ADR-0053).
- **The rules are the export's.** The ISO-TP reassembly is the export's
  own (`core/src/frame_reassembly.h`, shared by both), timed on each
  frame's `timestamp_us`. The classes are the same: an identification
  reply's frames, every segment; a torn message's segments on a
  diagnostic reply ID; the messages of a text or hex def of `set`. The
  page passes the text and hex defs of every session's set since the
  bridge started, copied at each connect, so an export made after the
  session ends still leaves their messages out.
- **The check** searches each kept frame, and each whole message a kept
  frame carries, for the identity values the left-out replies carry and
  for VIN-shaped tokens. It doesn't search the text the page writes: the
  CSV holds each frame's bytes as spaced hex, a form the check already
  covers, and a row's time and ID separate one frame's bytes from the
  next. There are no `signals.csv` text values in memory, so a text
  value outside a text def's message is caught only by its VIN shape. A
  match fails the call with `ECUIO_ERR_LOAD_FAILED`, the same as
  `ecuio_export`, the class in `failed_class` and `keep` all zero. The
  page then writes nothing and shows the class's words.
- **Nothing is made up.** A message an adapter delivered whole is one
  row with its `reassembled` flag, so `made_up` stays 0.
- **The file:** the kept frames the page's filter shows, oldest first.
  One comment line names Havne's version, the frames written, and each
  class's count out of the frames held. Then
  `time_s,id,ext,dir,dlc,flags,data`: the frame's session time in
  seconds, the ID in hex, 1 for a 29-bit ID, `rx` or `tx`, the bytes,
  the flags in the table's words, and the bytes as spaced hex. The
  page's words say that Export CSV leaves identification replies out
  and that Copy copies the selected rows as they are.

**The CLI:** frames in memory are the app's alone. `havne-cli stream`
writes each frame as it comes, unscrubbed, and keeps no ring to export.
`havne-cli export --format csv` over a recording is still to come:
`ECUIO_EXPORT_CSV` answers `ECUIO_ERR_NOT_IMPLEMENTED` until it's built.
(Note of 2026-11-06: built; see the appendix "the CSV export as
built".)

**Tests:** `ScrubFramesTest` (test_core: an identification reply whole
and as it crossed the bus, a torn segment on a reply ID, a text def's
messages whole and segmented, a DID echoing the fake VIN whole and as
it crossed the bus before the reply that names it, a VIN shape with no
reply, the refusals and a cancel), `BridgeTest.FrameCsv*` and
`.ExportFrames*` (test_bridge: the file byte for byte, the filter, a
noted set after its file is freed, a failed check leaving the file as
it was), `QmlTest.log`'s Export CSV case, and `test_c_abi`.

## Appendix: the CSV export as built (2026-11-06)

`ECUIO_EXPORT_CSV` writes a recording's values, `havne-cli export
<recording> <out> --format csv [--from S] [--to S] [--raw]` its front
end. This appendix's note amends the first appendix's "until they're
built" for the CSV export; the folder export and the CSV import still
answer `ECUIO_ERR_NOT_IMPLEMENTED`.

**Values, not frames.** The frames already have their export, pcapng,
which the tools that read frames open. Section 1's context names this
one "CSV of signals over a time range", for a spreadsheet, pandas or R,
and one file with two kinds of row would serve neither. The Log page's
CSV is frames because the page holds no values (the appendix "frames in
memory").

**One row a sample, in `signals.csv`'s order:**

```
# Havne 0.6.0, a scrubbed export (ADR-0054): 4 values of 3 signals, 0.001000 s to 0.003000 s; left out: 0 values of identification signals, 0 of text signals
time_s,signal,unit,value
0.001000,RPM,rpm,1726
```

- **The comment line** names Havne's version, scrubbed or raw, the
  values written, the signals they're of, the span, and each class's
  count. A reader skips it: pandas with `skiprows=1`, not `comment="#"`,
  which would cut a value or name holding a `#`.
- **`time_s`** is the row's `host_us` in seconds, to the microsecond:
  the clock the viewer, `havne-cli view` and `info` and the pcapng
  export's range use. `from_us` and `to_us` take it as the pcapng export
  does.
- **The long form.** Each row is a reading the ECU sent, with nothing
  filled in. One column per signal would leave most cells blank under
  `obd2`, where each PID comes in a frame of its own, and pairs of
  columns, ADR-0049 section 8's sheet layout, are ragged in a CSV. pandas
  and R take the long form as it is.
- **RFC 4180 quotes** a name, unit or value holding a comma, a quote or
  a line end, and nothing else.
- **Markers aren't written.** A label is the user's own text, which
  section 1's table doesn't classify, so leaving it out fails closed.
  The viewer's Export can take them up with the check over them.

**The classes, by the def first.** The recording's set is found as the
pcapng export finds it (its `signals.json`, else uds-generic's from the
URI's `did=` list, else its profile by name), and each `signals.csv`
signal is matched to its def by name.

- A text or hex signal's values are left out, counted as text values:
  by its def, by the header's `signal` line declaring it text, or by a
  value that isn't a number (`parse_number` takes it whole, or it isn't
  one). Its name and unit stay out with them, since it has no rows.
- A signal whose def is keyed as an identification reply (49 xx, or 62
  with a DID in F180-F1FF) is left out even when its values are
  numbers, counted as identification: the pcapng export leaves those
  frames out, so the CSV doesn't let their decoded values through. The
  def's mux key goes through the same `scrub::identification` the frames
  do, so the two can't drift.
- Numbers are written as recorded.

**Then checked.** A recording with frames gets the pcapng export's
first pass over `frames.csv` for the identification replies' values;
`signals.csv`'s text values join them as section 1 says. The written
bytes then go through the same file check, and a match fails the export
with its class, leaving no file. A recording with no `frames.csv`
(values alone) is exported from `signals.csv` with the check over the
text values and the VIN shape; one with no `signals.csv` is refused.

**Spreadsheet formulas, with no escape** (ADR-0049 section 8). A name or
unit that starts with `=`, `+`, `-`, `@`, a tab or a carriage return
fails the export, naming the signal, and so does a text value starting
so in a raw export. A number is exempt: one `parse_number` takes whole,
such as `-12.5`, starts with `-` and is read as a number. Core's own
defs can't carry such a name or unit (the validator refuses one), but a
recording someone sends can.

**The report** gains `values` and `signal_count` from its reserved room (not `signals`, a Qt macro that a Qt program including the header would expand)
(`reserved[4]` is `reserved[2]`; the size, 120 bytes, and
`failed_class` at 80 stay; `values` at 88), both static-asserted and in
`test_c_abi`. For CSV, `frames` and `made_up` are 0 and `left_out`
counts values. `havne-cli`'s `exported` line adds `values` and
`signals` for CSV only, so the pcapng line is as it was.

**Tests:** `CsvExportTest` (test_core: the file byte for byte, a
negative number, a quoted name and no `frames.csv`; a range and an
empty one; a text DID, an identification-keyed number DID, a text value
and a header-declared text signal left out, raw keeping them; a unit
echoing a frame's calibration ID and a VIN-shaped name failing it; a
formula's start in a name, a unit and a raw text value failing it, the
scrubbed text value left out instead; no `signals.csv` and a bad row),
`CliTransferTest` (the `exported` line), `CliTransferRunTest.csv`
(`tests/golden/recording/replay-obd2`, core's recording of the replay
fixture, exported and read back by Python's `csv` module row for row,
twice to the same bytes, and a range).

## Appendix: the folder export as built (2026-11-06)

`ECUIO_EXPORT_FOLDER` writes a scrubbed copy of a recording's folder,
one a user can send, and `havne-cli export <recording> <out> --format
folder [--from S] [--to S] [--raw]` is its front end. This appendix's
note amends the first appendix's "until they're built" for the folder
export; the CSV import still answers `ECUIO_ERR_NOT_IMPLEMENTED`.

**The caller names the folder.** Section 1's "the folder is named by
its time stamp alone" is the front ends' to meet: core writes the copy
at `out_path_utf8` and never carries the source folder's name, which
holds the vehicle's label when the user gave one. `havne-cli`'s usage
suggests the stamp form, `20261106-142233`.

**A recording again.** Each file the export knows is rewritten by its
classes, so the copy opens in the viewer, `havne-cli view` and `info`,
replays through `replay://` and exports again as any recording does:

| File | In a scrubbed copy |
|---|---|
| `frames.csv` | the rows the pcapng export's plan keeps, each as recorded: a message an adapter delivered whole stays one row, not re-segmented, and a frame no CAN frame could carry stays too (the pcapng's rule for those is its format's) |
| `signals.csv` | the rows the CSV export's classes keep, as recorded, its header with its byte order mark |
| `events.log` | the header line by line, the export's own line, then the activity lines by kind (below) |
| `markers.csv` | every row, its label as typed |
| `latency.csv` | every row |
| `signals.json` | the whole file |
| anything else | left out, counted in `files_left_out` |

Each file goes through the file check of its own as it's written,
against the identity values the plan collects (the identification
replies' values from `frames.csv`, the text values from `signals.csv`)
and the VIN shape. The copy is made under `<out>.partial`, renamed into
place only when every file passes; a match fails the whole copy, names
the class and the file ("the check found a VIN-shaped token in the
copy's markers.csv: nothing was written") and removes the partial
folder. A folder with neither `frames.csv` nor `signals.csv` is refused,
and so is an `out` that exists.

**Section 1's table, row by row, in `events.log`:**

- **Identification replies** and **text signals**: their frames and
  values are the files' rows above; the header's `signal` lines stay,
  as the CSV export keeps a text signal's name with no rows.
- **The session's URI**: the `uri` line keeps its scheme, then the
  authority as `<path>` (a replay's file, a J2534 DLL), `<label>` (a
  mock's, the user's own text) or `<address>` (a serial port, a host and
  port, a Bluetooth stream, an interface), then only the query pairs
  core reads whose value is plain (letters, digits and `: , . / _ -`):
  `profile`, `did`, `baud`, `serial`, `filter`, `txid`, `poll`,
  `probe`, `identify`, `diagnose`, `pack`, `speed`, `loop`, `from`,
  `to`. `serial` is ELM327's serial-port rate, not a device's serial
  number. The profile and `did=` list stay, so the copy finds its set
  and exports again by the same classes.
- **The vehicle's label**: the folder's name is the caller's; the
  bridge's `caller  vehicle  <label>` line becomes `caller  vehicle
  <vehicle>`. The app's `Vehicle:` lines are notes, left out with them.
- **Activity lines**: `link`, `drop` and `asleep` are core's words and
  stay as they are; `end` keeps its counts, its why (the caller's words)
  as `<why>`; a `note` (the app's activity log, a CLI's end line) and a
  line of a kind the export doesn't know are left out and counted as
  `ACTIVITY`. Header lines core writes stay (`started`, `utc`, `core`,
  `profile`, `frame flags`, `signal`, `label`, `signal file`, `plugin`,
  `columns`); a `caller` line stays only for the front end's name and
  version and the surrogate's frame logging; any other header line is
  left out and counted. A log whose first line isn't format 2 or 3
  (format 1's, before ADR-0049) is a file the export doesn't know, left
  out and counted in `files_left_out`.
- **Numbers** are as recorded.

**Markers are kept, their labels as typed.** The CSV export leaves them
out, since a label is the user's own text; a copy of the recording is
the user's recording, sent by the user, and a marker is often the reason
to send it (where the pull starts). The check covers them, the export's
line says "their labels as typed", and so does `havne-cli`'s usage.

**A spreadsheet formula's start** in a `signals.csv` name, unit or kept
text value fails the copy, raw too, as the CSV export's: `signals.csv`
opens in a spreadsheet as readily as a CSV export does.

**The export's line**, before `columns` in the copy's header:

```
export  a scrubbed copy (ADR-0054), Havne 0.6.0: frames.csv 2 frames (left out: 1 of identification replies, 0 of text signals' messages, 0 segments of messages that couldn't be reassembled); signals.csv 2 values (left out: 0 of identification signals, 1 of text signals); markers.csv 2 markers, their labels as typed; events.log 2 lines left out (the caller's notes, its other lines and lines of a kind the export doesn't know); other files left out: 1
```

The `end` line's counts stay the source's: they describe the recording
as it was recorded, and the export's line says what the copy holds.

**A range** keeps each file's rows by `host_us` (`latency.csv`'s `t_s`,
the session time to the ms) and the activity lines by their seconds
since the start; the header and the `end` line stay whole.

**The report** gains `files` and `files_left_out` (uint32 each, at 104
and 108) from its reserved room (`reserved[2]` is `reserved[1]`; the
size, 120 bytes, stays), static-asserted and in `test_c_abi`. For a
folder, `frames` counts the kept rows, `made_up` is 0, `values` and
`signal_count` are the CSV export's, and `left_out` sums the frames'
and the values' counts per class with the activity lines under
`ACTIVITY`. No status code is added. `havne-cli`'s `exported` line adds
`values`, `signals`, `files` and `files_left_out` for a folder.

**Tests:** `FolderExportTest` (test_core: every file byte for byte, an
ELM327 URI's port and a key that isn't core's out, the vehicle's label
and a caller's line and note out, a VIN reply's frame and value out, the
copy exported again; a range; raw; a mock's label; a VIN typed as a
marker's label and a calibration ID in `signals.json` failing the copy;
an existing folder, no recording, a bad row, a formula's start),
`CliTransferTest` (the `exported` line), `CliTransferRunTest.folder`
(the findings fixture copied raw, its `info` and replay the source's,
and scrubbed, its `info` the source's less the torn ISO-TP message the
scrub leaves out; `tests/golden/recording/replay-obd2`'s copy viewed as
the source; a mock's recording made with a label, its copy's URI
`mock://<label>`, its `info` and replay the source's).

Amended 2026-11-07: `latency.csv`'s `t_s` is the recording's time to the
ms ([ADR-0017](0017-session-recording.md)'s appendix of 2026-11-07), so
the range reads it as it reads `host_us`; a recording from before that
wrote the session time there, and its rows land by that. The export
keeps the header's `focus` and `link` lines ([ADR-0049](0049-recordings-in-core.md)'s
appendix of 2026-11-07).

## Appendix: the health report as built (2026-11-06)

Section 2's health report: a car's health as one scrubbed file, in core
so the app, `havne-cli` and Python share it. It grows in four steps:
the report over a live session (this appendix's first part), then over
a recording, then `havne-cli health`, then the app's Save report on
Diagnostics and the viewer. `folder_utf8` answers
`ECUIO_ERR_NOT_IMPLEMENTED` until the second.

**The call.** `ecuio_health_export(options, report)` (`recording.h`)
writes `out_path_utf8` in `ECUIO_HEALTH_JSON` (one object,
`"havne_health": 1`) or `ECUIO_HEALTH_TEXT` (the same facts for a
person: a forum post, a shop). One model in core is built once and
written by either writer, which reads only the model, so the two can't
hold different facts; `HealthTest` checks that every string the JSON
holds is in the text. The options put their scalars first and their
pointers last, so the offsets are the same on either width.

**Deterministic.** Core reads no clock for the report: its time is the
caller's (`utc_us`, microseconds since 1970 UTC; 0 writes none), and so
is each value's age. The same inputs give the same bytes, so a test
compares them. Numbers are written to their def's decimals without the
locale (a comma under a German one would break both formats).

**What it holds, in order:**

1. **The source:** the session's URI as the folder export scrubs it
   (`scrub::uri`, now in `scrub.cpp` and shared), its backend, bit rate
   and profile. No vehicle label, and no adapter version strings, which
   can carry a serial number.
2. **The vehicle:** the first ECU's VIN that decodes
   (`ecuio_identity_vin`, `ecuio_vin_decode` with the caller's
   `latest_model_year`), as its model year, make and whether the check
   digit matched. Never the VIN, its WMI or its serial part.
3. **The inspection:** `ecuio_diag_inspect` over every ECU for that
   model year (ADR-0045): the verdict, the incomplete monitors by name,
   the allowance and the reports counted. With no model year (no VIN
   read, or a recording scrubbed of it) it's the rule for none, which
   the report states: one incomplete monitor passes either model year's
   rule, two pass only 1996-2000's (`year_needed`), three or more pass
   neither.
4. **Each ECU:** its state (complete, reading, not asked); readiness
   with the MIL, the confirmed-code count and each monitor (with this
   drive cycle's where 01 41 answered); the stored, pending and
   permanent codes with core's titles, a refusal as its reply, "N more"
   past the held list; the freeze frame, its code and its values by the
   obd2 defs; **the since-the-codes-were-cleared values**, Service 01
   PIDs 21 (distance with the MIL on), 30 (warm-ups), 31 (distance), 4D
   (time with the MIL on) and 4E (time), whenever a value of a def keyed
   `41 <PID>` on the ECU's reply ID is given, ranked or not, since a
   shop or a buyer reads those first; and the identification items by
   name and outcome, their text left out and counted.
5. **The key values,** as a rule: the session's set's ranked signals
   (`plot_rank` above 0, the ones a UI plots first, ADR-0039 section 8),
   numbers and enumerations, among the values the caller gives, by rank
   and then the set's order, each in the units asked
   (`ecuio_display_convert`), with its age and marked stale past
   `ECUIO_VALUE_STALE_US`. An enumeration is its label from the set. The
   since-cleared PIDs aren't repeated here; the odometer (A6) is a key
   value only when ranked. A set that ranks nothing says so. Core keeps
   no newest value per signal, so the caller passes the values it holds
   (`ecuio_health_value_t`): the app from Live, the CLI from a values
   callback, Python from a value queue.
6. **Left out:** the identification texts, counted.

**Then checked.** Every answered identification item's text (the VIN, a
calibration ID, a serial, an ECU's name) goes into the scrub check's
values, and the written bytes are searched for them and for the VIN's
shape before the file is renamed into place from `<out>.partial`. A
match fails it with the class ("the check found an identification
reply's value in the report: nothing was written"): a number signal
that reads an ECU's serial, for one.

**The report** (`ecuio_health_report_t`, 120 bytes) counts the ECUs,
codes, key values and since-cleared values written, the texts left out
(by the export's classes, `IDENTIFICATION`), the check's
`failed_class`, and `ECUIO_HEALTH_VIN_DECODED`. No status code is added.

**Tests:** `HealthTest` (test_core: both formats holding the same facts
and neither the VIN, a calibration ID or the port; the key values' rule,
a ranked since-cleared PID kept with its ECU and another ECU's value not
taken; imperial units; an enumeration's label; the inspection with no
model year; an identity text echoed by a value failing the report; the
text of a quiet car byte for byte; numbers and times without the
locale; the export over a `mock://` session, its label out, the file
written once; the argument checks), `test_c_abi`.

**Over a recording (2026-11-07).** `folder_utf8` names a recording's
folder, and the report is built from what the recording holds, through
the same model and writers, so it reads as the session's would. Nothing
in the call changed: a folder takes no values of the caller's
(`value_count` 0, else `ECUIO_ERR_INVALID_ARGUMENT`), since the
recording has its own, and `from_us`/`to_us` are host_us as the
exports' (0 and 0 the whole recording; a `to_us` past the end is the
end; a `from_us` past the end is refused, `ECUIO_ERR_LOAD_FAILED` "the
range starts at A s, after the recording ends at B s", as replay
refuses a stretch with no frames).

- **The ECUs** come from the replies in `frames.csv`
  (`core/src/health_replies.h`, pure): whole messages on the diagnostic
  reply IDs (`scrub::reply_id`), a reassembled row as it is and wire
  frames through `frame_reassembly.h` (the export's rules), each taken
  when it ends in the range. They go through the request engine's
  parsers: 41 01 `ecuio_diag_take_readiness`, 41 41 `ecuio_diag_take_cycle`
  (after readiness, whatever their order), 43/47/4A
  `ecuio_diag_take_codes`, 7F 03/07/0A that list's refusal, 42 00 00 the
  freeze frame's PIDs, 42 02 00 its code, 42 PID 00 its values by the
  obd2 defs (only with a nonzero code), 49 xx (not a PID bitmap) and 62
  F180-F1FF identification items, their text by
  `ecuio_identity_format`. The last reply of each kind wins, so a
  readiness reply after the codes were read is the one shown, and the
  drive cycle's monitors (41 41) are laid on it. An ECU is a reply ID
  with at least one positive reply of these kinds: a listener's raw
  recording hears every module's traffic, and a refusal alone makes no
  ECU. Any other refusal says nothing of which request it answers, and
  7F xx 78 (still working) is no answer; both are skipped. Nothing is
  marked asked or complete, so the inspection never reads READING, and
  a kind with no reply reads "not recorded" (a recording can't say
  whether it was asked); each ECU's state is "recorded".
- **The values** are the recording's set's (`core/src/recording_set.h`,
  the exports' rule, shared with them: its own `signals.json`, else
  uds-generic's set from the URI's `did=` list, else the profile by
  name): each number def with a signal of its name, the value in force at
  the range's end (`ecuio_recording_text_at`, parsed without the locale)
  and aged from that end; one whose last sample is before the range's
  start has none. A recording with `frames.csv` and a set but no
  `signals.csv` has its values decoded from the frames
  (`ecuio_recording_open_frames`). The key values and since-cleared
  values then follow the live rule.
- **The source:** kind `recording`, the URI from `events.log` through
  `scrub::uri` (none: `null`, "no URI"), the backend by the URI's scheme
  (`unknown` without one), the profile from the header, no bit rate
  (the header has none), the recording's start (`started`, from its
  `utc` line), the range (`from_s`, `to_s`) and whether it has
  `frames.csv`. A recording of values alone (no `frames.csv`) has no ECU
  and says so (`ecus_note` "the recording has no frames"); one whose
  frames hold no such reply says no ECU answered in the range. A folder
  with neither file is refused as the folder export refuses it.
- **The check** also takes every identification reply's values in the
  whole recording (`scrub::identification_values`), in the range or not,
  besides the items' texts: a number signal in the range that reads an
  ECU's serial fails the report even when the serial's reply came
  before the range. A scrubbed copy (the folder export) holds no
  identification reply, so its report reads no VIN and lists no item.

`frames.csv` is read a row at a time by `core/src/frame_rows.h`'s
`frame_rows`, the exports' reader, shared with them.

**Tests (step 2):** `HealthTest`: a hand-written recording of replies,
reassembled and on the wire, rebuilt into the ECU's report (both formats
holding the same facts, no VIN, calibration ID or port; another module's
refusal no ECU; 7F 03 78 and a Service 09 bitmap ignored); a range
taking the replies that end in it and no value from before it, and one
starting past the end refused; the
replay fixture's frames decoded for its values, with no ECU; a recording
of values alone; a refusal of a folder with neither file and of a row
out of order; an identity reply before the range still checked; a
folder export's scrubbed copy; a small recording's text byte for byte.
`ExportTest`, `CsvExportTest` and `FolderExportTest` cover the shared
header and rows.

**`havne-cli health` (2026-11-07).** The third step: `havne-cli health
<uri | recording> <out> --format json|text [--units metric|imperial]`,
public headers only (`cli/health_lines.h`, `run_health_session`,
`run_health_recording`). An argument with a scheme's `://` is a live
session, anything else a recording's folder. The CLI gives the time
(`utc_us` from the system clock) and the latest model year (this year
plus one, as the front ends pass it).

- **A recording** takes `--from`/`--to` (view's seconds) and nothing of a
  session's: its own set, as core reads it.
- **A live session** takes `--seconds` (the wait's bound, 30 by default,
  as `clear`'s) and `--signals`/`--plugin` (loaded as `stream` loads
  them, so a user's ranked signals are key values). A values callback
  keeps each def's newest value and when it came (`ecuio_clock_us`), one
  slot per def ID so the worker allocates nothing; each value's age is
  the report's time less that. The wait is for what's asked: a channel
  is settled once its diagnostics are complete or were never asked
  (only obd2 and the mock ask them; a uds-generic or raw session never
  does) and its identification is complete or has no items. Then it
  waits until no shown signal (a ranked one or a since-cleared PID) has
  gained its first value for 2 s: a PID the ECU lacks never comes, so
  the wait ends on quiet, not on all of them. `--seconds` or Ctrl+C
  ends it sooner, and the report holds what's in, a report still being
  read saying so.
- **Reads only.** On a car the session asks only what core's
  diagnostics and identification ask (Services 01, 02, 03, 07, 09 and
  0A, never 04's clear), so it runs parked or driving; the usage says
  so.
- **Its lines:** `start`, a session's `session`, then `health` (format,
  file, source, the report's counts, the identification texts left out,
  `vin_decoded` as a yes or no, and a session's `settled`); never the
  VIN or an identification text. A failed check writes `health_failed`
  with its class before the `error` line. Exits as `export`'s: 0
  written, 2 usage, 3 a connect, a refused folder, an existing file or a
  failed check, 1 the CLI's own output failing.

**Tests (step 3):** `CliHealthTest` (test_cli: each argument form and
refusal, the wait's rule for asked and unasked reports, the shown defs,
the values' quiet, the lines), `CliUsageTest.Health*`, and
`CliHealthRunTest` (`tests/cli_health.py`): the mock's report (its label
out, the file written once), a uds-generic mock with diagnostics off
ending well before its `--seconds`, the simulated car as an ELM327
adapter on loopback (its VIN decoded and in neither the file nor the
lines, its ranked values and five since-cleared PIDs), the same car
answering late through uds-generic (its identification outlasts the
values' quiet, and the report waits for it), a recording of values alone
and a range past its end.

**The app's Save report (2026-11-07).** The fourth step: Diagnostics'
"Save report..." (while connected, whatever the read's state) and the log
viewer's (a recording open), on every platform, through one helper
(`app/healthreport.{h,cpp}`) and no change in core.

- **The file:** a save dialog with two filters, text first (the one a
  shop or a buyer reads) and JSON. The format is the chosen filter's,
  or JSON for a name ending `.json` (`healthFormatFor`): Android's
  content:// URI carries no name, so the filter decides there.
- **Writing:** core writes only to a path and refuses one that exists,
  while the dialog has already asked about replacing a file and Android
  hands back a content:// URI. So core writes into a temporary folder of
  the app's own (its file named `health.txt` or `health.json`, never the
  vehicle's label), and the bytes its check passed are written to the
  chosen file (`app/urlwrite.h`, as Export CSV's and a signal file's
  copy are); the temporary folder goes either way.
- **A live session:** the session's reports as they stand and the
  values Live's rows hold, each as core decoded it (the def's unit, an
  enumeration's raw number; never the display's conversion) and aged
  from the window that brought it; the app's units, this year + 1 as the
  latest model year, the time from the system clock. No wait, unlike
  `havne-cli health`: the page shows the report as it stands, and a
  report still being read says so.
- **A recording:** always the whole of it; the viewer's zoom is a view,
  not a choice of range, and a zoomed-in stretch after the diagnostics
  replies would read "not recorded" for every ECU. A range is
  `havne-cli health`'s alone for now. Core reads all of frames.csv, so
  the report runs on a thread of the viewer's own, one at a time; a
  report under way when the viewer closes or opens another recording
  still writes its file (core's export can't stop midway), and its result
  is dropped.
- **Its words:** a note under the button ("Report saved: 1 ECU, 3
  trouble codes, 14 key values", or "Report not saved: <why>") and one
  log line, "Health report saved: 1 ECU, 3 trouble codes, 14 key values
  (13 identification items left out)" or "Health report not saved:
  <why>", each noun as its count wants it (`healthSummary`,
  `healthSavedLine`); never a path, a folder's name (it can hold the
  vehicle's label), the VIN or a value.

**Tests (step 4):** `BridgeTest.HealthFormatIsTheFiltersOrTheNames`,
`.HealthReportWordsEachCount` (one, none and many),
`.HealthReportOfTheMockIsWrittenWhereChosen` (text, then JSON by the
filter and by the name, over a file that exists, the mock's label never
written, core's temporary folder gone, a folder refused, not connected
refused), `.HeldValuesAreCoresInTheDefsUnit` (an imperial model's held
value in km/h, its age), `.HealthReportCarriesLivesValuesInTheAppsUnits`
(a replay under obd2: the key values in mph or deg F);
`RecordingViewTest.HealthReportIsTheWholeRecordingsOnItsOwnThread` (one
at a time, the whole recording, the folder's name in neither the file
nor the words) and `.AReportEndingAfterACloseIsDropped`; `QmlTest.
diagnostics`' `test_saveReportWritesTheChosenFormat` and `QmlTest.
recordings`' `test_saveReportWritesTheWholeRecording` (zoomed in, the
report still the whole recording's).

**Note of 2026-11-07: what a report shows and refuses.**

- A since-cleared value on a reply ID no listed ECU takes (a recording
  whose range holds none of that ECU's diagnostic or identification
  replies, one made with `diagnose=0`) is shown among the key values,
  after the ranked ones, in PID order; the text gives them the line
  "Since the codes were cleared, on a reply ID no ECU is listed for:".
  A value an ECU takes stays under that ECU alone. The report counts
  such a value in both `values` and `since_cleared`.
- A recording older than the `REASSEMBLED` flag holds whole replies with
  flags 0. A row that isn't on the wire (not 1-8 bytes with a PCI
  nibble on ISO 15765-4's IDs) and isn't an error or truncated is taken
  whole, as the reader decodes it. An ECU is still listed only on a
  reply ID, so another ID's frame that opens like a reply never becomes
  one; the identity values of every such row go to the check.
- A session with a range (`from_us` or `to_us` nonzero) is refused
  `ECUIO_ERR_INVALID_ARGUMENT`: a session's values are its newest, and a
  range is a recording's. `havne-cli health` already refused `--from`
  and `--to` for a live session ("--from and --to take a recording's
  seconds, not a session's").
- A report that isn't written counts nothing written: on any failure
  the report is zero but a failed check's `failed_class`.
- The JSON is valid UTF-8 whatever bytes its sources held: a byte no
  valid sequence holds is written as U+FFFD (`json.h`'s `utf8_length`,
  the rule the reader holds a string to).
- A value's age saturates where milliseconds can't round it; an
  enumeration's value that is negative, a fraction or past UINT32_MAX is
  written as its number, with no label; a recording's number value is
  read with a row's room, so a long one is never cut into another
  number.

**Note of 2026-11-07: the vehicle's baseline named.** A report names the
vehicle's baseline recording (ADR-0049's appendix "a vehicle's
baseline") when its caller gives the folder; nothing in core knows which
recording is a baseline.

- **The option.** The options' first reserved word is an anonymous
  union of `baseline_folder_utf8` and `baseline_room` (`uint64_t`), so
  the pointer sits at 48 on either width and the struct keeps its size;
  `reserved` is three words. NULL or "" names none.
- **What's read of it:** its `events.log` alone. Its first line must be
  a recording's (the reader's test: format 2 or 3 by the whole line,
  format 1 by its ending, a byte order mark skipped); then the header's
  `utc`, `profile` and `uri` lines (`recording_set.h`'s `read_header`),
  and its length, the last timed line's time. A line past the reader's
  room is one line, its rest never a timed line of its own. Nothing else
  of the folder is opened, so a long baseline adds little to a live
  report, which the app writes on the GUI thread.
- **What's written,** after the source: the JSON's `baseline`, null for
  none; `read` true with `started` (UTC; null without a `utc` line),
  `length_s` (null with no timed line), `profile`, `backend` (the URI's
  scheme, `unknown` without one) and `is_source`, whether the report's
  own recording is it (the same start, so a folder export's copy of it
  is it too; a session's report never is); or `read` false with `why`:
  "its folder is gone: it was moved or deleted", "its folder holds no
  events.log" or "its events.log isn't a recording's". The text's line,
  after the source's: "Baseline: the vehicle's baseline recording,
  started <UTC>, <S> s (<backend>), profile <profile>", with ": this
  recording" at its end when it is, or "Baseline: the vehicle's baseline
  recording can't be read: <why>"; none, no line. The report is written
  either way, and `ECUIO_HEALTH_BASELINE_READ` (0x2) in the report's
  flags says whether it was read.
- **Never its folder:** the app names a recording's folder by its stamp
  and the vehicle's label, so neither the folder's name nor its path is
  written. The name isn't added to the check's values: a label can be
  any word (a profile's, a unit's), and the report never reads it.
- **`havne-cli health`** takes `--baseline <recording>` for a session
  and a recording alike: the CLI has no vehicle book, so the caller
  names it, as `view`'s overlay does. An empty one is usage. The
  `health` line gains `baseline`: "none", "read" or "unread".
- **The app's Save report,** on Diagnostics and in the viewer, passes
  the picked vehicle's baseline, the one the Recordings page badges and
  compares with; a folder that's gone is passed too, so the report says
  so. Its result map gains `baselineRead`.
- **Python:** `scripts/ctypes_gen.py` writes an anonymous union as a
  `Union` of its own (`ecuio_health_options_t_u0`) in the struct's
  `_anonymous_`, so `baseline_folder_utf8` reads as the struct's; a
  named union fails the script.

Tests: `HealthTest.AReportNamesTheBaselineByItsStartNeverItsFolder`,
`.TheReportsOwnRecordingAsItsBaselineIsSaidToBeIt`,
`.ABaselineThatCantBeReadSaysWhyAndTheReportIsWritten`,
`.ABaselinesEventsLogIsReadAsTheReadersIs` (test_core);
`CliHealthTest.ABaselineIsNamedForEitherSourceAndTheLineSaysIfRead`,
`CliHealthRunTest.recording`; `BridgeTest.HealthReportNamesTheBaselineItIsGiven`,
`RecordingViewTest.HealthReportNamesTheBaselineItIsGiven`;
`QmlTest.diagnostics`, `QmlTest.recordings` and `QmlTest.phone`'s
`test_theReportsPagesTakeSettingsVehicles`; `test_c_abi`;
`PythonTest.test_declarations` on either width.

## Appendix: the CSV import as built (2026-11-07)

`ECUIO_IMPORT_CSV` makes a recording's folder from another logger's CSV
of timed signals, by a column map. `havne-cli import csv <file>
<folder> --map <map.json>` and Python's `havne.import_csv` are its front
ends. This appendix amends the first appendix's "until they're built"
and the later appendices' "still answers `ECUIO_ERR_NOT_IMPLEMENTED`"
for the CSV import.

**The call.** `ecuio_import(options, report)` with `format`
`ECUIO_IMPORT_CSV` (2), `in_path_utf8` the file, `out_folder_utf8` the
folder to make, and `column_map_utf8` the map's JSON text (core's strict
reader, 1 MiB at most). A CSV with no map, a map with candump, or a CSV
with `interface_utf8` is `ECUIO_ERR_INVALID_ARGUMENT`; a refused map or
file is `ECUIO_ERR_LOAD_FAILED` with the words in the detail ("the
column map: unknown key 'decimal' in signal 1", "line 12: ..."), never a
path. The report's first two reserved words became `values` (48) and
`columns_ignored` (56), so it stays 80 bytes with `reserved[2]` at 64
(static-asserted, `test_c_abi`; `ecuio_import` is new since v0.5.1, so
the ABI check against it sees an added function). A CSV import fills `lines`, `values`,
`columns_ignored`, `utc_start_us` (0 when the file gives no UTC time)
and `duration_us`; `frames`, `error_frames` and `interfaces` stay 0.

**The column map** (`core/src/csv_import.h`'s `parse_map`):

```json
{
  "havne_column_map": 1,
  "header_row": 3,
  "delimiter": ";",
  "decimal": ",",
  "time": {"column": "Time [s]", "unit": "s"},
  "start": "2026-11-05T22:03:22.125+09:00",
  "missing": ["---"],
  "signals": [
    {"column": "Engine Speed [rpm]", "name": "RPM", "unit": "rpm", "decimals": 0},
    {"column": "Gear", "name": "Gear", "kind": "text"}
  ]
}
```

- `havne_column_map` is 1 and required. Any key not named here is
  refused by its name, in the map, in `time` or in a signal: a typo
  must not import silently.
- `header_row` 1-1000 (default 1): the rows above it, a logger's
  preamble, are skipped as lines. `delimiter` `,`, `;` or a tab;
  `decimal` `.` or `,`, a decimal comma needing another delimiter.
- `time`: `column` and `unit`, one of `s`, `ms`, `us` (from any zero:
  the first row is the recording's start), `iso8601` (each cell a date and time with `Z` or an offset,
  `T` or a space between, up to six fraction digits, from 1970 on) or
  `time-of-day` (`hh:mm:ss[.ffffff]`, with `date` YYYY-MM-DD and
  `utc_offset` `Z` or +hh:mm, which come with it and only with it; a
  time more than 12 hours before the row before's is the next day's).
- `start`, for the relative units only: an ISO 8601 time with its
  offset, the first row's UTC time. **It is optional:** without it the
  folder has no UTC time, as below. A made-up start would line a
  recording up wrong against another; none lines it up with nothing.
- `missing`: up to 16 texts of 1-32 bytes read as no sample (`---`,
  `NaN`), empty by default. An empty cell is no sample either; any
  other cell a number column can't read is refused with its line.
- `signals`: 1 to 256, each `column` (the header's text, matched whole:
  **by name only**, never by index, which takes the wrong column when a
  logger adds one), `name` (1-31 bytes), `unit` (up to 15), `kind`
  `number` (the default) or `text`, `decimals` 0-3 (a number's; default
  2, a text 0), `rank` 0-255 (default the map's order, 1 first, capped
  at 255, so the viewer plots the numbers; a text 0) and `group`
  (`ECUIO_SIGNAL_GROUP_*`, default 0). A name or unit starting as a
  spreadsheet formula (`=`, `+`, `-`, `@`, a tab, a carriage return) is
  refused, the validator's rule; two signals with one name or column,
  or the time column a signal's too, are refused. A header column the
  map doesn't read is ignored, counted and named.

**The reader** is RFC 4180 and strict (`reader<Source>`, one byte at a
time from a file or memory): a UTF-8 byte order mark at the file's
start dropped, quoted fields with `""` escapes and delimiters and line
breaks inside, CRLF or LF, blank lines skipped, no last line break
needed. Each refusal names its line: a carriage return without a line
feed, a NUL byte, invalid UTF-8, a quote inside an unquoted field, text
after a closing quote, an unclosed quote, a row over 64 KiB, more than
1024 columns, a row whose fields aren't the header's count, a header
missing a mapped column or naming one twice, a row with no time. Numbers
are read locale-free (spaces around them allowed, inf and NaN refused;
with a decimal comma a point is refused) and times to the microsecond
(within 1e15 us). A row up to 100 ms before the row before takes that
row's time, the candump import's rule; further back is refused. At most
1e8 rows.

**What it makes:** `<folder>.partial`, renamed into place once whole,
removed on any failure, and only when the folder is free. Two passes, as
the candump import's: the first checks and counts, the second writes,
and a second pass whose counts differ is "the file changed while it was
read". `signals.csv` (its byte order mark, `host_us,signal,value,unit`,
microseconds from the first row) and `events.log` in format 3, no
`frames.csv`:

```
ecuio session recording, format 3 (ADR-0049)
started  2026-11-05 13:03:22.250 (UTC)
utc  2026-11-05T13:03:22.250Z
core  0.6.0
import  CSV, its time from the column 'timestamp' as ISO 8601 times
import  columns ignored: 2 ('throttle', 'comment')
profile  raw
signal  Vehicle Speed,number,km/h,1,1,0
signal  Gear,text,,0,0,0
columns  UTC time, seconds since the recording started (host_us / 1e6 in the CSV files), kind, text

13:03:25.750  +3.500  end  import: 0 frames, 16 values, 0 records dropped, 0 notes dropped
```

The `signal` lines are the recorder's own formatting
(`core/src/recording_lines.h`, which `recorder.cpp` writes through too).
The ignored columns name the first 32 and count the rest. The file's
name is never written: it can hold a vehicle's label.

**With no UTC time** (a relative unit and no `start`), `events.log` has
no `started` or `utc` line, its `columns` line says "time since the
start" and its end line's clock runs from 00:00:00. Then:

- the reader's `utc_start_us` is 0, as for a recording made before the
  `utc` line;
- `havne-cli import csv` and `view` write `"utc_start_us":null`, and
  `view --align-on utc` leaves that recording unaligned (`found` false,
  no columns), aligning the others as before;
- the app's viewer gives it a `utcStartMs` of 0, which the Recordings
  page reads as no start: no "started" text, and a comparison on UTC
  starts isn't drawn, saying which of the two has none ("this
  recording has no UTC start");
- `havne-cli info` checks a recording's frames, which an import of a
  CSV has none of: it is `error` "the folder has no frames.csv" and exit
  3, as for any recording of values alone; `replay://` refuses it for
  the same reason.

**The front ends.** `havne-cli import csv <file> <folder> --map
<map.json>` reads the map (1 MiB at most; a map that can't be read is
an `error` line in `read_whole`'s words, exit 3), then writes
`{"type":"imported","format":"csv","folder":...,"lines":N,"values":N,"columns_ignored":N,"utc_start_us":N|null,"duration_us":N}`;
a refusal is the `error` line with core's detail and exit 3, nothing
made. `--map` is required for csv and refused for candump, as
`--interface` is the other way round (exit 2). Python:
`havne.import_csv(path, folder, column_map)`, the map a dict or its JSON
text, and `havne.import_candump(path, folder, interface=None)`, each
returning `havne.Imported` (the folder and core's counts,
`utc_start_us` None when the file gave none) or raising `havne.Error`
with core's words.

**In the app.** The Recordings page lists every folder in its
recordings folder that holds a `signals.csv` or a `frames.csv`
(`SessionBridge::recentRecordings`, through the one rule
`recordingRow` uses for a vehicle's baseline), so an import made there,
CSV or candump, is listed and opens. Each row says whether it has
frames (`frames`), and a recording of values alone, a CSV import's,
shows what it can't do instead of failing at it: its row says "values
alone: the recording has no frames to replay" and has no Replay; the
viewer (`RecordingView.hasFrames`) has no Replay in Settings and no
decode through a profile (`openFrames` refuses at once, "the recording
has no frames", never a pass that fails or waits), and its frames
check reads "Frames check: the recording has no frames", the check not
run. **Left for v0.7.0:** the Import button, which picks a file and a
map and makes the folder in the recordings folder; until then a folder
made elsewhere is moved there to be listed.

**Tests:** `CsvImportTest` (test_core, `tests/test_csv_import.cpp`:
each map key and every map refusal, the map's bounds at their edges;
the reader's quoting, BOM, preamble, blank lines and every refusal at
its line, its bounds at their edges; each time form to its
microseconds, midnight crossed, the 100 ms rule; empty cells and
`missing`; the decimal comma; a folder made and read back through
`ecuio_recording_open`, a start or the file's own times giving the
`utc` line, a refused file making nothing, many ignored columns; the two
made-up loggers' files, `tests/fixtures/import/csv/`: a preamble with
`;` and decimal commas, ISO times with an offset and quoted cells),
`ImportTest`'s argument refusals, `RecordingViewTest.AnImportedCsvOpensAndPlots`
(the Done-when: imported, opened by the viewer, its numbers plotted and
their columns), `CliTransferTest` (the arguments and the `imported`
line), `CliUsageTest.ImportCsv*`, `CliGoldenTest.ImportCsvRefusesAMapsUnknownKey`
(`import-csv-unknown-key.jsonl`), `CliTransferRunTest.csv-import` (both
loggers imported and viewed; a missing map, a refused map and a folder
already there), `PythonTest.test_import`,
`BridgeTest.TheRecordingsListTakesAFolderOfValuesAlone` (the list's
rule is `recordingRow`'s, each row's `frames`),
`RecordingViewTest.AFolderWithNoFramesOpensUnchecked` (the check's and
a decode's words), `QmlTest.recordings`'
`test_aCsvImportIsListedAndOpensWithoutFrames` (an import listed, its
row without Replay, opened from the list with its numbers plotted, no
check, Replay in Settings or decode), and `fuzz_csv_import` (the
first line a map, the rest the CSV; two reads agree; the samples within
their bounds). Mutation-checked 2026-11-07 (14: a quoted delimiter
splitting, a short row taken, the BOM kept, the 100 ms bound off, a time
back kept, an unknown key taken, the second pass skipped, the ignored
list dropped, a `utc` line with no UTC time, a missing text kept,
midnight not crossed, the CLI's null start a number, the map never
handed to core, Python's None start a 0), and the app's 6 (the list
taking frames alone, a row without frames still replaying, a decode
of no frames going ahead, the check run on no frames, the decode
choice and Replay in Settings shown without frames).

## Appendix: a column map guessed from the header (2026-11-07)

Accepted 2026-11-07; step 1 built 2026-11-07, as the note "settled
while building" at the end of this appendix says.

The CSV import (the appendix "the CSV import as built") takes a column
map and refuses a file without one. Writing JSON for an ordinary file
is more than a user should have to do, and the files people import
mostly say what they are in their header row: a time column first, then
`Name (unit)` or `Name [unit]` columns. Havne's own wide CSV
(ADR-0056) is one of them. So core guesses the map from the file, and
every front end shows or prints the guess before it imports. The map
stays: a guess is a map, in the same format, through the same strict
`parse_map`, and a map file still overrides it.

### 1. The call

```c
/* Reads the start of a CSV file and writes a column map (JSON text,
 * havne_column_map 1) to map_out. Imports nothing, writes no file.
 * ECUIO_ERR_BUFFER_TOO_SMALL with *needed set when cap is short. */
ecuio_status_t ecuio_import_csv_guess(const char* path_utf8,
                                      char* map_out, size_t cap,
                                      size_t* needed,
                                      ecuio_csv_guess_t* report);

typedef struct ecuio_csv_guess {
    uint32_t flags;          /* ECUIO_CSV_GUESS_* below */
    uint32_t columns;        /* signal columns in the map */
    uint32_t left_out;       /* header columns left out (notes say why) */
    uint32_t rows_read;      /* rows the guess looked at */
    char     notes_utf8[1024]; /* one line per choice worth saying, column
                                * names as the header has them, never a
                                * cell's value or the file's path */
    uint64_t reserved[4];
} ecuio_csv_guess_t;

#define ECUIO_CSV_GUESS_TIME_UNIT_KNOWN  0x1u /* the map's time unit is set */
#define ECUIO_CSV_GUESS_DECIMAL_COMMA    0x2u
#define ECUIO_CSV_GUESS_PREAMBLE         0x4u /* header_row above 1 */
```

Append-only, as every addition is: a new function, a new struct with
`flags` and `reserved`, no change to `ecuio_import`. The file's
contents never reach a log; a refusal names the line, as the reader's
do.

### 2. What it reads

The first 64 KiB or 200 rows after the header, whichever ends first,
through the import's own RFC 4180 reader (quotes, the BOM, CRLF or LF).
The same file always gives the same map.

- **The delimiter:** `,`, `;` or a tab, whichever splits the candidate
  header and the rows below it into the same number of cells (at least
  two); a tie goes in that order.
- **The header row:** the first row, within the map's 1000, whose cells
  are mostly not numbers while the rows below have its cell count and
  are mostly numbers. The rows above are a preamble: `header_row` says
  so and the flag is set.
- **The decimal comma:** only with a `;` or tab delimiter, when number
  cells hold a comma and none a point.
- **The time column:** a header named like time (`time`, `timestamp`,
  `t`, `elapsed`, `time stamp`, any case, before its unit), else the
  first column when its cells only rise. Its unit:
  - from the name's unit: `(s)`, `[s]`, `sec`; `(ms)`, `[ms]`; `(us)`,
    `[us]` and the micro sign's spelling;
  - from the cells: ISO 8601 times give `iso8601`, which needs no unit;
  - otherwise **unknown**: the map has no `unit`, the flag is clear, and
    the import refuses it until one is given (section 3). A number
    column of times without a unit could be seconds, ms or us, and a
    wrong pick scales the whole log by 1000 without a word, so it is
    never guessed. A time of day (`hh:mm:ss`) needs a date and a UTC
    offset the file doesn't hold: unknown too.
  - `start` is never guessed: a made-up start lines a recording up
    wrong (the appendix's rule).
- **Each other column** becomes a signal, in the header's order:
  - `name`: the header text before a trailing `(unit)` or `[unit]`,
    trimmed; cut to 31 bytes on a character boundary, and a name that
    then repeats another gets ` 2`, ` 3`; each cut or rename is a note.
  - `unit`: the bracketed text, up to 15 bytes; a longer one is
    dropped with a note.
  - `kind`: `number` when every cell in the sample that isn't empty or
    a placeholder reads as a number, else `text`.
  - `decimals`: the most fraction digits seen, at most 3.
  - `rank` the map's order, `group` 0, as the map's defaults.
- **Placeholders** (`missing`): any of `---`, `--`, `NaN`, `nan`, `N/A`,
  `n/a` seen in a column that is otherwise numbers.
- **Left out, each with a note:** a header cell that is empty (a
  trailing delimiter gives one), a column empty in every sampled row,
  and a column the map would refuse (a name starting as a spreadsheet
  formula). The preview can put each back.

The guess is checked by `parse_map` before it is returned: a guess core
would refuse is never handed out.

### 3. The front ends

- **The app:** the Recordings page's Import panel shows the guess
  before anything is imported: the time column and its unit (a choice
  of s, ms and us when unknown, Import waiting on it), each column with
  a checkbox, its name, unit and kind, and the first rows. An edit
  changes the map the import gets. **Save as map** writes it to the
  settings folder's `maps/`, offered next time; **Column map...** still
  picks a file, which skips the guess.
- **`havne-cli import csv <file> <folder>`:** without `--map` it
  guesses; `--time-unit s|ms|us` gives the unit when the header
  doesn't, and with none known it refuses, naming the option.
  `--print-map` prints the guessed map and imports nothing, so it can
  be saved and edited. `--map` behaves as today.
- **Python:** `havne.guess_csv_map(path)` returns the map as a dict
  (with the report's notes); `havne.import_csv(path, folder,
  column_map=None, time_unit=None)` guesses when no map is given.

### 4. Tests

- One case per rule above, each refusal and note in its words, in
  `CsvImportTest`'s style; a guess that `parse_map` would refuse never
  returned.
- Round trips: synthetic files shaped like common loggers (a preamble,
  `;` with decimal commas, ISO times, a trailing delimiter, an always
  empty column), each guessed, imported and viewed; Havne's wide CSV
  once ADR-0056's export exists.
- `fuzz_csv_import` grows to run the guess, and the import with each
  guess it makes.
- The app's preview at 384 px, the CLI's `--print-map` and refusal,
  Python's two calls.

### 5. Steps

1. Core's call and its tests, the CLI and Python (one branch).
2. The preview on the Import panel (after step 1).

### Settled while building (2026-11-07)

Step 1 settled these, and the call as built is `recording.h`'s, not
section 1's sketch:

- **The text and its room:** the call writes one JSON text,
  `{"map": {...}, "columns": [{"column", "used", "why"}], "notes":
  [...]}`, so the notes are in the text and the struct has no notes
  buffer. `map_out` and `cap` follow `ecuio_serial_ports`' contract (cap
  0 with a NULL `map_out` asks the size, `*needed` always set, no
  status of its own): on `ECUIO_OK` `*needed` is the text's length and
  its NUL, and the text is written only when `cap` holds it; on any
  failure, the arguments' refusals too, `*needed` is 0, the report zero
  and a `map_out` with room `""`. `ecuio_csv_guess_t` keeps `flags` and
  the counts and is 48 bytes on both widths.
- **The flags:** `TIME_UNIT_KNOWN` (the map's time has a unit),
  `DECIMAL_COMMA`, `PREAMBLE` (lines above the header left out) and
  `TIME_NUMBERS`: the time column holds plain numbers, the one time a
  unit given can fix. A front end names `--time-unit` (`time_unit`) only
  then; for any other time it names a map with the zone (or the date and
  offset).
- **The map's slice:** `map_offset` and `map_length` in the report give
  the map's byte range in the text this call wrote. That slice alone is
  complete JSON, with two-space indents, which `ecuio_import` takes as
  it is: the CLI prints and passes it with no JSON reader of its own.
  `CsvGuessTest.TheMadeUpLoggersAreGuessedAndImported` pins each made-up
  logger's slice to its `guessed-map.json`, and
  `CliTransferRunTest.csv-guess` pins `--print-map` to the same bytes.
- **The time unit:** an argument, `time_unit`: NULL, `"s"`, `"ms"` or
  `"us"`, anything else `ECUIO_ERR_INVALID_ARGUMENT`. A time of plain
  numbers takes it when given, else the unit its header names. ISO 8601
  times with their offset stay `iso8601`, with a note that the unit
  given isn't used. A map with no unit is checked with one in its place
  and handed out without it.
- **Times without a zone:** ISO 8601 cells with no UTC offset, and
  times of day, leave the unit unknown with a note that a map with the
  zone (or the date and offset) is needed. A UTC offset given to the
  guess waits for a later step.
- **The bounds:** the header search covers the file's first 1000 lines
  within 1 MiB; the sample's 64 KiB and 200 rows count from the header.
  A read past what the guess needs stops at a line feed, and a quoted
  field that read cuts short ends the sample; a read with no line feed
  at all is refused as one ("the file's first 1152 KiB hold no line
  end"). The header's check reads 64 KiB from the candidate: a row that
  window ends inside ends the check as the file's end would, so wide
  rows keep their header, and a candidate needs one whole row below it.
  A preamble line the import refuses (past 64 KiB, a NUL) refuses the
  guess in the import's words, as a map with that `header_row` would.
- **The header and the time:** a header has two or more cells, more
  than half of its non-empty ones not numbers or times, and up to five
  rows below it of its cell count, each with more than half of its
  non-empty cells numbers or times (a row of empty cells skipped). Each
  row on its own, not the rows together, so a preamble line with the
  data's field count is never taken for the header. A time column that
  isn't named so must be non-decreasing with at least two distinct
  values. Majority is more than half of the non-empty cells.
- **Units in the header:** `sec`, `secs`, `seconds`, `msec` and `usec`
  inside the brackets too, and both micro signs (U+00B5 and U+03BC)
  read as data.
- **Names and notes:** a cut name leaves room for its ` 2`; past 64
  notes the rest are counted in one `...and N more`; columns past the
  map's 256 signals are left out and counted in one note.
- **The front ends:** `havne-cli import csv <file> <folder>` without
  `--map` writes a `guessed` line (the text's three keys) before the
  import's own, as compact JSON like the CLI's other lines, and refuses
  a map with no unit with an `error` line naming `--time-unit` for a time
  of plain numbers, a map with the zone (or the date and offset) for any
  other. `havne-cli import csv <file> --print-map
  [--time-unit ...]` prints the slice alone and a line end, no JSON
  lines; a map with no unit is printed with a stderr line saying so.
  Python's `havne.guess_csv_map(path, time_unit=None)` returns a
  `GuessedMap` (`map` a dict, `notes`, `flags`, `columns` the text's
  list); `havne.import_csv(path, folder, column_map=None,
  time_unit=None)` takes the slice when no map is given.

### The preview as built (2026-11-07)

Step 2 settled these:

- **The call:** the app's `RecordingImport` runs `ecuio_import_csv_guess`
  on its worker (a file that isn't local is copied to 1280 KiB first,
  past core's read, so core cuts it as it cuts the file) and hands the
  panel the map's slice, each header column's use and why, and the notes.
  The edits are composed in the app from the slice (`composeMap`) and
  imported from their text, with no map file written; core's strict
  `parse_map` still judges them, so a name past 31 bytes or one a
  spreadsheet would run as a formula is refused in core's words and the
  panel keeps the edits.
- **The first rows** are not in core's text, which never carries a cell's
  value: the app splits the file's first 64 KiB itself for display (at
  most five rows below the header row, a cell cut at 32 characters) and
  never decides anything from them.
- **Editing:** a checkbox, name, unit and a number/text toggle a column.
  A column the guess left out can be put back as text named by its
  header (the name cut to 31 bytes on a character); a column with an
  empty header, or a header the file has twice, can't be named in a map
  and stays out. The time column is the map's time and never also a
  signal, whatever the edits say. Decimals and the other keys are the
  guess's. A time of plain numbers takes its unit from a choice of s, ms
  and us; any other time without a unit needs a map with its zone, and
  **Column map...** picks one.
- **Saved maps:** **Save as map** writes `<settings folder>/maps/<name>.json`
  (the name made of ASCII letters, digits, `-` and `_`, never over a name
  taken) once Import would be enabled, so a map with no time unit is
  never saved; a **Saved maps** list offers them, and choosing one, like
  **Column map...**, skips the guess until **Use the guess**.

## Appendix: the wide CSV as built (2026-11-07)

[ADR-0056](0056-analysis-log.md) sections 4 and 5 make the wide layout
the CSV export's default; the long form of the appendix "the CSV export
as built" stays behind `ECUIO_CSV_LONG` (`havne-cli export --layout
long`), byte for byte.

**The options grow within `reserved`** (the C API appendix's
`reserved[4]` is now `layout`, `row_ms`, `units`, `column_count`,
`columns_utf8` and `reserved[1]`): `layout` at 24 plus two pointers (40
on 64-bit, 32 on 32-bit), `columns_utf8` at 40 plus two pointers, the
size 56 plus two pointers, each pinned in `test_c_abi`. A caller that
zeroed `reserved` gets the wide CSV. The report's `reserved[1]` is
`rows` (at 112), the last reserved word it had: the report can't grow
again within its size, so a later figure needs a struct of its own (a
known limit). The fields are the wide CSV's alone: set for another
format or for the long layout, `ECUIO_ERR_INVALID_ARGUMENT`.

**The file:**

```
Time (ms),RPM (rpm),Speed (km/h),Throttle (%)
160,812,0,10.5
260,815,0,10.0
```

- `Time (ms)` first, then each column as `Name (unit)` (the name alone
  with no unit), RFC 4180 quotes only where a cell needs them, CRLF, no
  comment line: the analyst's tools want the header first.
- **The columns:** `columns_utf8`'s names in order (`--signal <name>`
  repeated); with none, the header's `focus` line's names in its order
  (a log begun with a focus exports its picks by itself); with neither,
  every number signal that has a sample in range, the header's signal
  lines first in their order, then any the header lacks as first met.
  A named column the recording lacks is an empty column (a focus pick
  the session never answered is a column of a log; `havne-cli export`
  says so on stderr for each, the exit code unchanged); one that names
  a text or identification signal, or a name twice, is
  `ECUIO_ERR_INVALID_ARGUMENT` with the reason in the detail (the same
  pick in the header's focus line is a bad recording,
  `ECUIO_ERR_LOAD_FAILED`). Text and identification signals are never
  columns and their values are counted left out as the long form counts them.
- **The rows** (section 4's rule): a row once every column in the rule
  has a sample newer than the last row, at the completing sample's
  time in whole ms (rounded) from the recording's zero, each cell the
  column's newest value, held. A column with samples in range starts
  in the rule (its first sample is due with the first sweep's); one
  with none, a named pick the recording never answered, starts outside
  it and holds no row up. A column with no sample for 1 s, or 3x its
  usual interval when that is longer (the median of the gaps between
  its samples, from a log-linear histogram as reply timing keeps),
  leaves the rule when the next sample of any column finds it quiet,
  and its cells stay empty until its next sample, which puts it back.
- **A grid** (`row_ms`, `--rows <ms>`): rows at the multiples of the
  interval from the recording's zero (so two exports of one log align,
  `--from` or not), each the newest values at its time under the same
  silence rule, a step with no new cell since the last written row
  skipped (a stretch of them in one move: nothing is new until the
  next sample, so a gap of hours at `--rows 1` costs no loop, and the
  silence is judged at the step that writes), and a last row at the
  step at or after the last sample so the newest values are in.
- **Cells:** each in its signal's decimal places from the header's
  signal line (an older recording's: the most its values show, at most
  3), converted to `units` (`ECUIO_UNITS_*`, `--units metric|imperial`)
  through `ecuio_display_convert`, which adds a place where the
  conversion needs it (kPa to psi); the head cell carries the unit
  shown. Formatted by core's own digits, a `.` whatever the locale, no
  `-0.0`; a value past the fixed digits' reach (9e15 once scaled) is
  written as `signals.csv` writes it (`%.12g`), never an empty cell.
- **The report:** `rows`, `values` the filled cells, `signal_count` the
  columns; the file goes through the same check as the long form and
  the same formula refusal for a name or unit.

**The estimate** (`ecuio_export_estimate(options, row_ms, count, out)`):
one pass over the recording answers each interval in `row_ms` with its
rows, the file's exact bytes (every cell formatted, nothing written,
the check not run) and the columns, for the app's choices beside each
other and for scripts; the wide CSV's alone, `out_path_utf8` may be
NULL.

**`havne-cli export`:** `--layout wide|long` (wide the default),
`--rows sweep|<ms>` (1 to 86400000), `--units metric|imperial`,
`--signal <name>` repeated; the four go with `--format csv`, the last
three with the wide layout, else usage. The `exported` line adds
`layout`, `rows`, `row_ms` and `missing` for CSV (schema 1 growth):
`missing` counts the named columns the recording has no values for
(the reader's signal list, `ecuio_recording_open`, which lists the
signals with a sample), each also a stderr line
`havne-cli: the recording has no values for '<name>': its column is
empty`.

**Tests:** `CsvExportTest.Wide*` (test_core: a row a sweep over every
number signal, a range and an empty one; a column that stops leaving
the rule at 1 s and rejoining; a slow column's 3x; a 100 ms grid
holding values, skipping an empty step and ending on the step after
the last sample, aligned to the zero under `--from`; decimals and
units from the header, metric and imperial; named columns in order
with an empty one, a text, an identification and a repeated name
refused; the focus line's columns; text and identification signals
never columns; a quoted head and a formula refused),
`ExportTest.TheEstimateMatchesTheFileForEachInterval` and
`.ArgumentsAreCheckedBeforeAnythingIsRead` (the shape fields and the
estimate's refusals; the estimate also over named columns, one quoted
and one the recording lacks, a range and imperial units),
`FolderExportTest.EachFileIsCopiedThroughItsClasses` (a copy exported
wide through its focus line), `test_c_abi` (the offsets),
`CliTransferTest` (the flags, their bad forms, the line, the missing
names), `CliTransferRunTest.csv` (the replay fixture's recording
exported long as before and wide: the header, every cell a number or
empty, the times whole ms ascending, a 1 s grid, named columns in
order, a named column the recording lacks warned and counted). The
sweep row's time to the nearest ms, the silence judged at a grid's
writing step, a gap jumped in one move and a value past the fixed
digits have a case each (`CsvExportTest.WideA*`), and
`RequestTimingTest.ASamplePastThirtyOneBitsLandsInTheLastBucket` pins
the histogram's bound, which the survey's gaps reach.

**The column limit (2026-11-08):** a wide CSV has at most 16384 columns,
the width a spreadsheet opens. The survey counts the columns as it adds
them, named or found (the header's number signals and every new name in
signals.csv), and the first one past the limit ends the export with
`ECUIO_ERR_LOAD_FAILED` and "the wide CSV is limited to 16384 columns:
pick the columns to keep", before any row is costed (each sample touches
every column, so a recording naming tens of thousands of signals would
otherwise run for minutes and write a file nothing opens). The long
layout has no such limit. `ecuio_export_estimate` takes at most 64 row
intervals in a call, each one keeping a rows engine, and refuses more
with `ECUIO_ERR_INVALID_ARGUMENT` before it allocates.

## Note of 2026-11-08: the viewer's Export and the range

Section 2 has the viewer's Export take its range from the chart's view.
It covers the whole recording in every format instead:
[ADR-0056](0056-analysis-log.md) section 5 sets the rule for a log
(its size comes from the row interval, and the steady state is as much
the analyst's as any pull), and one rule for all the Export's formats
keeps a zoom from silently cutting a file, as Save report's zoom doesn't.
`--from` and `--to` stay `havne-cli`'s alone, for scripts. Markers stay
out of the CSV: the section's "the viewer's Export can take them up"
isn't taken. ADR-0056's appendix "the viewer's Export as built" has the
rest.

## Appendix: an export canceled (2026-11-09)

A long export, a wide CSV of a long log or a folder copy through a
provider, can run tens of seconds, and the app can quit meanwhile, or
Android tear it down. So the export and its sizing take a cancel, as the
reader's open does (ADR-0049 section 5):

- **The calls are added, nothing changed.** `ecuio_export_with(options,
  cancel, user_data, report)` and `ecuio_export_estimate_with(options,
  row_ms, count, cancel, user_data, out)`, with
  `typedef int (*ecuio_cancel_t)(void* user_data)`. The type is any long
  call's stop, not the export's, so a later long call (an import, the
  viewer's report) takes the same one and Python has one `CFUNCTYPE`.
  `ecuio_export` and `ecuio_export_estimate` are the same calls with a
  NULL cancel, so their bytes don't change. `ECUIO_ERR_CANCELED` (19)
  is the reader's.
- **Where it's asked.** As each pass over the recording begins, as each
  file of a folder copy begins, and as rows are read within a pass. The
  header names those points and not how often, so the interval (1024
  rows) can change without a contract change.
- **A stop is a failure like any other.** The call returns
  `ECUIO_ERR_CANCELED` with "the export was canceled" in the detail.
  Nothing is at `out_path_utf8`, and the file or folder written under a
  name of its own beside it is removed by the path every failure takes.
  An estimate's `out` stays zeroed. A process killed mid-export still
  leaves what it had begun: core's `<name>.partial-<pid>-<n>` beside a
  local target, and the app's temporary folder. For the second, the
  app's sweep of stale export folders at startup
  (`RecordingExport::removeStaleCopies`, called from `main.cpp`) is the
  backstop.
- **Threads.** The call is one caller's. The cancel runs on that
  caller's thread and never after the call returns, so what it reads to
  decide, a flag another thread sets, is the caller's to make safe.
- **Cancel only.** Nothing asks for progress. A progress callback would
  be an addition of its own.
- **The app.** `RecordingExport`'s destructor sets a flag that both
  workers' cancels read, then joins them, so quitting the app, an
  Android teardown included, waits for the next ask, not the export's
  end. The Export panel lives as long as the Recordings page, a fixed
  page of Main's, so leaving the page doesn't end an export: it goes on
  while another page shows. The copy to a provider checks the same flag
  between its 1 MiB chunks and removes what it made, as a failed copy
  does. Nothing else is user-visible: a Cancel button in the Export
  panel is a separate item.
- **havne-cli.** `export` and `export --estimate` install the Ctrl+C and
  SIGTERM handlers and pass the interrupt through the cancel. An
  interrupted run writes `clear`'s interrupted end line,
  `{"type":"end","reason":"interrupted"}`, and exits 1, since nothing was
  written. `HAVNE_CLI_EXPORT_HOLD_MS` (tests only) holds the run's first
  ask, naming it on stderr (the export's or the column check's), so a
  test's interrupt lands where it says. The named columns' check of a
  wide CSV (`--signal`) opens the recording before the export, through
  the same cancel, so an interrupt during it leaves nothing written; its
  stderr lines come after the export.
- **Python** has no export wrapper. `_ecuio.py` binds both calls.

*Note of 2026-11-09:* the import and the health report take the same
cancel. `ecuio_import_with` is asked as each pass begins and as bytes are
read (every 64 KiB); `ecuio_health_export_with` is asked, for a
recording's folder, as the frames pass begins and every 1024 of its
rows, and through the reader's open as it reads. A session's report
never asks. Each stop is `ECUIO_ERR_CANCELED` ("the import was
canceled", "the report was canceled") with nothing left made.
`RecordingImport` and `RecordingView` set a stop in their destructors
before they join; a close or another open still lets a report run out,
so the file the user picked is written. `havne-cli import` stops on
Ctrl+C with the interrupted end line and exit 1. Python's `import_csv`
and `import_candump` take `cancel=`, and a Ctrl+C on the main thread
stops core at its next ask and is raised again as `KeyboardInterrupt`.
`ecuio_import_csv_guess` takes no cancel: it reads at most 1.25 MiB.
