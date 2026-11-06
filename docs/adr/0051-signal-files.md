# ADR-0051: Signal files: a user's own signals as data

- **Status:** Accepted. Built in v0.5.0.
- **Date:** 2026-11-05.
- **Builds on:** [ADR-0013](0013-protocol-profiles.md) (profiles chosen
  by the URI, the URI as provenance);
  [ADR-0015](0015-request-engine.md) (the request engine, a request
  enrolled by its def's mux key);
  [ADR-0016](0016-signal-definition-layout.md) (the def's layout, its
  reserved room, the layout version);
  [ADR-0020](0020-uds-generic.md) (`?did=`, the `7E9:` prefix
  and the rx - 8 rule); [ADR-0024](0024-decode-on-the-worker.md) (decoding
  on the worker); [ADR-0048](0048-semantic-versioning.md) (what a minor
  version may break); [ADR-0049](0049-recordings-in-core.md)
  (recordings); [ADR-0050](0050-finding-a-signal.md) (the group).
- **Amends:** ADR-0016 (the set's layout version 4, the def's first
  flag, its `reserved[1]`); ADR-0049 (recording format 3, for enums and
  the signal file's copy). A note in each.
- **Leads to:** the plugin framework's ADR (ADR-0052), whose data tier
  is this format, and the OBDb import.

## Context

A session's signals come from four places, all compiled in or typed
into a URI: `mock`, `obd2`, `raw`, and `uds-generic`'s `?did=` entries
(hex, text or a big-endian integer, scale 1, no unit). The signals
enthusiasts ask for most, transmission fluid temperature and boost, are
in no standard set: a transmission's temperature is a maker's DID,
often on the transmission's own module, and boost is MAP minus baro,
two `obd2` PIDs.

A user's own signal needs:

- a request on any ECU: Service 01 or 22, the number, the module's
  request and reply IDs;
- where the value sits in the reply, its signedness, scale and offset,
  unit, name, decimals and group;
- three kinds of value: a number, text, and an enumeration (a raw
  number shown as a word: 1 "Open loop, cold", 2 "Closed loop");
- signals computed from others (boost);
- the same file read by the app on every platform (iOS loads no code at
  run time, so the format is data) and by `havne-cli`;
- one parser, which the plugin framework's data tier and the OBDb
  import reuse.

Core already enrolls a def keyed `62 <DID>` or `41 <PID>` at offset 0
on the channel whose rx is its `message_id` (ADR-0015 section 2), a set
declares up to `ECUIO_MAX_CHANNELS` (4) channels, and
`ecuio_profile_register` takes any set that validates. So a DID on a
transmission's module is a def plus a channel, and the request path
doesn't change. What core can't do is read a file, decode an
enumeration, or compute one signal from others.

`ECUIO_PROFILE_MAX_REQUESTS` is 128 (profile.h), and the `obd2` set
asks over 90 PIDs, so `obd2` plus a user's signals in one session runs
out of room. The cap also sizes the engine's request table, the
session's timing rows and the recorder's timing totals.

## Decision

### 1. The format: JSON, read by a strict parser in core

A signal file is JSON (UTF-8), read by a parser written in core, as
the recordings' and the URI's readers are, and fuzzed
(`fuzz_signal_file`). No library is vendored, so no third-party notice
is added. The parser refuses duplicate keys and reads numbers without
the locale, as the recordings' reader does. JSON has no comments: a
`"notes"` string goes anywhere one would.

### 2. The schema (format 1)

```json
{
  "havne_signals": 1,
  "name": "Daily driver extras",
  "notes": "TFT checked against the gauge",
  "source": { "name": "Written by hand for the daily driver" },
  "extends": "obd2",
  "ecus": [
    { "id": "pcm", "rx": "7E8" },
    { "id": "tcm", "rx": "7E9" }
  ],
  "signals": [
    {
      "key": "tft",
      "name": "Trans Fluid Temp",
      "ecu": "tcm",
      "read": { "service": "22", "number": "1E1C" },
      "field": { "byte": 0, "bits": 16, "order": "be", "signed": false },
      "scale": 1, "offset": -40,
      "unit": "°C", "decimals": 0, "group": "engine"
    },
    {
      "key": "fuel_status",
      "name": "Fuel System 1",
      "ecu": "pcm",
      "read": { "service": "01", "number": "03" },
      "field": { "byte": 0, "bits": 8 },
      "kind": "enum",
      "values": { "1": "Open loop, cold", "2": "Closed loop", "4": "Open loop, load" },
      "group": "fuel"
    },
    {
      "key": "cal_id",
      "name": "TCM Calibration",
      "ecu": "tcm",
      "read": { "service": "22", "number": "F188" },
      "field": { "byte": 0 },
      "kind": "text",
      "group": "diagnostics"
    },
    {
      "key": "boost",
      "name": "Boost",
      "kind": "computed",
      "expr": "obd2.map - obd2.baro",
      "max_age": { "obd2.baro": 30 },
      "unit": "kPa", "decimals": 0, "group": "air", "plot": 1
    }
  ]
}
```

- **`havne_signals`**, first and required, is the format's version. A
  reader refuses a version it doesn't know.
- **`source`** (required): where the file's signals come from,
  `{ "name", "url", "commit", "license", "modified" }`, `name`
  required. The OBDb importer fills all five (section 7), and every
  copy of the file keeps the block, so a file that leaves the user's
  machine carries its attribution. A file without one is refused.
- **`extends`** (optional): a built-in set the file adds to, `obd2`
  only in format 1. The loaded set is the base's defs and channels
  followed by the file's, so one session shows both. Without it the
  file is a set of its own.
- **`ecus`**: `rx` is the reply ID in hex, 11-bit or 29-bit. `tx` may
  be left out only where ADR-0020's rule is the standard's: rx - 8 for
  0x7E8 to 0x7EF, and the `18DA` swap for `18DAF1xx`. Elsewhere it is
  required. At most `ECUIO_MAX_CHANNELS`, the base's included. A signal
  with no `ecu` is on the first. Section 3 says what is refused.
- **`read`**: `service` is `"01"` or `"22"`, `number` the PID (2 hex
  digits) or the DID (4). The def's mux key becomes `41 <PID>` or
  `62 <DID>` at offset 0, which enrolls the request as any other def's.
- **`field`**: `byte` counts the reply's data bytes after the echo (0
  is J1979's A) and becomes `start_byte` (+2 for 01, +3 for 22);
  `bits` (1-32), `bit` (0-7, little-endian only), `order` `"le"` or
  `"be"` (default `"be"`, as both services send), `signed`. A text
  field takes `bytes` (0: to the end). The validator's rules apply
  unchanged, so a big-endian field is whole bytes.
- **`kind`**: `"number"` (the default), `"text"`, `"hex"`, `"enum"`,
  `"computed"`.
- **`values`** (enum only): a decimal raw value to a label, at most
  `ECUIO_ENUM_MAX_ENTRIES` (256) a signal, each label at most 27 bytes.
  A raw value with no label shows as its number ("7"), so nothing reads
  as a claim the file doesn't make.
- **`expr`** (computed only): `+ - * /`, parentheses, decimal constants
  and other signals by key (`obd2.<name>` for the base set's, by a
  stable key list `obd2` publishes); at most 4 inputs and 16
  operations, no functions. A computed signal can't feed another in
  format 1, so there's no order or cycle to check.
- **`max_age`** (computed only, optional): seconds per input, 0.1-60,
  past which that input is too old to compute from. An input it doesn't
  name takes the stale age (section 5).
- **Display**: `name` (1-31 bytes), `unit` (0-15), `decimals` (0-3),
  `group` (`ecuio_signal_group_name`'s words in lower case, `engine` to
  `diagnostics`; none is Other), `plot` (the plot rank, 0-255). The
  formula guard (ADR-0049 section 8) applies to the name, the unit and
  every enum label.
- **Strict**: an unknown key, a wrong type or a duplicate `key` refuses
  the file, the error naming the line, the column and the key, never
  the file's path, which can hold the account name. `notes` and keys
  starting `x-` are ignored, so a tool can carry its own data.
- **Bounds**: 1 MiB a file, 1024 signals (`ECUIO_ENGINE_SCAN_MAX`).
- **IDs**: each def's `id` is `ECUIO_USER_SIG_ID_BASE` (30000) plus its
  place in the file. `key` is the stable name: computed signals and,
  later, a Dashboard's tile refer to it, since a reordered file
  renumbers.

Scale and offset in `?did=` (`1E05:u16*0.1-40:degC`) aren't added:
they would be a second syntax for what a signal file says. If they
come later, they parse into the same internal form as a file's
`field`, `scale`, `offset` and `unit`, sharing the validator and the
fuzz corpus.

### 3. Refused at load, not at the car

Everything a file can get wrong is refused when it's loaded, by the
loader and by `havne-cli signals check` alike, so a file fails on the
desk instead of after a connect at the car:

- **One ID width.** J2534's and ELM327's attach already refuse a
  session whose channels mix 11-bit and 29-bit IDs, before any DLL or
  port opens (one connection can't carry both: J2534 would need
  `CAN_ID_BOTH`). A file whose ECUs, with `extends`' base counted, mix
  the two is refused at load with the ECU that breaks it named.
  `obd2`'s ECUs are 11-bit, so a file that extends it takes 11-bit ECUs
  only. If a transport gains both widths on one connection (SocketCAN
  could), the rule moves to that transport's attach and the loader's
  check becomes a warning.
- **The tx rule.** An `rx` outside 0x7E8 to 0x7EF and `18DAF1xx` with
  no `tx` is refused ("ecu 'bcm': rx 7B8 needs a tx"), as is a `tx` of
  another width than its `rx`.
- **Units.** A unit outside ADR-0022's conversion table loads, is shown
  as written and is never converted, and `signals check` says so.
- Every rule above in section 2.

Live and `havne-cli` show a signal its module refuses (an
extended-session DID, say) as refused, not blank.

### 4. How it loads, on every platform

- **Core reads bytes, never a path:**

  ```c
  ecuio_status_t ecuio_signal_file_load(const char* bytes, size_t length,
                                        ecuio_signal_file_t** out);
  const ecuio_signal_set_t* ecuio_signal_file_set(const ecuio_signal_file_t* file);
  void ecuio_signal_file_free(ecuio_signal_file_t* file);
  ```

  A refusal's words come through `ecuio_last_error_detail()` ("line 12,
  column 9: unknown key 'scael'"). The file owns its set, defs,
  channels, enum table and computed programs.
- **The app reads the file** with Qt, so Android's `content://` URIs
  and iOS's security-scoped URLs from the file picker work as they do
  for recordings, then copies it into its own data folder (beside the
  preferences, never the user's Documents), kept with the vehicle in
  the vehicle book. Loading is data only, so iOS has nothing to refuse.
- **`havne-cli`** reads a path itself: `havne-cli stream <uri> --signals
  <file>`, and `havne-cli signals check <file>` (the loader's words, one
  line a problem, the exit code by the result).
- **Selection stays in the URI** (ADR-0013 section 2): a loaded file
  registers as a profile, `user.<slug>` from its `name`, and the URI
  says `?profile=user.daily-driver-extras`. The registry keeps the
  set's pointer, so the app keeps every loaded file for its life, or
  until every session using it is destroyed.
- **Provenance:** the URI names the profile, and a recording copies the
  file into its folder (`signals.json`) with its SHA-256 on an
  `events.log` header line, so a recording says exactly what decoded
  it, and a replay with that file decodes the same.

### 5. Computed signals in core

The UI receives final values, so computed signals run in core. After
`decode_values` for a frame, each computed def whose inputs include a
value the frame just decoded is evaluated from each input's newest
value, when each input has one no older than its age bound, and goes
into the same batch, stamped with the frame's time. A computed value
so refreshes at its fastest input's pace. Division by zero or a
non-finite result gives no value. The expression is compiled once at
load into a bounded stack machine, so the worker allocates nothing
(`CoreTest.WorkersAllocateNothingPerFrame`).

The age bound answers whether inputs are close enough in time to
combine, which depends on how fast each input moves and how often it's
read. The default is the stale age, one public constant in core,
`ECUIO_VALUE_STALE_US` (3 s), which the app's dimming and `havne-cli`
read too, so the three never drift apart. A computed signal's
`max_age` sets it per input. Ages run on the samples' session time.
Boost shows why: MAP is a fast-class PID (about every 0.8 s on the
test car, ADR-0039 section 3), baro (0x33) a slow-class one (about
every 3.2 s unpacked), and a baro reading 30 s old is still right, so
a 3 s bound on baro would blank boost for part of every baro cycle.
Live still dims an input past 3 s like any signal; the computed value
goes quiet only when an input passes its own bound.

### 6. Core's changes, and which break the ABI

ADR-0048: a 0.x minor version may break the C ABI on purpose, each
break listed in `abi/suppressions.abignore` and the CHANGELOG.

| Change | ABI | Notes |
|---|---|---|
| The loader (`ecuio_signal_file_*`) | Additive | New functions and an opaque type |
| `ECUIO_VALUE_ENUM` = 5 | Additive value, behavior change | A batch value of this kind carries both `value` (the raw number) and `text` (the label), which no kind does today, so every consumer that switches on kind (the bridge, the recorder, `havne-cli`) learns it; `havne-cli`'s schema may rise (ADR-0031) |
| The enum table and computed programs | **Break: `ECUIO_SIGNAL_SET_LAYOUT` 3 to 4** | The set gains `enums`/`enum_count` and `computed`/`computed_count`. Its `reserved[4]` (16 bytes) can't hold two pointers and two counts on 64-bit, and a pointer in a `uint32_t` array changes size between 32- and 64-bit. The bump makes every out-of-tree set restate its version, ADR-0016 section 5's purpose |
| A def's enum range or computed program | Within reserved room | `reserved[1]` (offset 100, free per ADR-0016 section 6): the first entry and the count as two `uint16_t`, or the program's index. `reserved[0]` stays earmarked for the unit's quantity |
| `ECUIO_DEF_FLAG_COMPUTED` | Additive | The first defined flag: the validator accepts that bit and still refuses any other. A computed def has `message_id` 0 and applies to no frame |
| `ECUIO_PROFILE_MAX_REQUESTS` 128 to 256 | Constant change | Public in profile.h; sizes the engine's, the session timing's and the recorder's tables, roughly 20 KB more a session for the timing rows. `uds-generic`'s polled limit follows it. `ECUIO_MAX_CHANNELS` stays 4 (a car's PCM, TCM and two more) until a real car needs more |
| `ECUIO_VALUE_STALE_US` (3 s) | Additive | The age at which a value is stale (section 5) |
| Recording format 3 | Format change | An enum signal's raw number in `signals.csv` and its labels in `events.log`, so the viewer plots the number and shows the word; `signals.json` in the folder with its hash. ADR-0049's reader learns format 3 and keeps reading 1 and 2 |

A sub-byte big-endian field (a new `byte_order` value, room for which
ADR-0016 section 2 keeps) is added only if OBDb's bit positions need
it, which the importer's design checks.

### 7. OBDb's data and its license

OBDb (https://github.com/OBDb) licenses its signal sets under CC BY-SA
4.0 (each vehicle repository's `LICENSE`, read 2026-11-04). A reading
of that license against these plans, not legal advice:

- **Importing a user's OBDb files at run time is use, not Sharing**, so
  the license sets no condition on it, and Havne's MIT code
  incorporates none of the data. A file converted from OBDb's is
  Adapted Material, though, and two things Havne does would Share it: a
  recording copies its signal file (section 4), and an export would.
  So the importer fills the converted file's `source` block: "OBDb
  editors and contributors", the repository's URL, the commit if
  known, `CC-BY-SA-4.0`, and that it was converted to Havne's format.
  The block travels with every copy.
- **Havne ships no OBDb data** and fetches none: a user imports their
  own file. Shipping some would make each shipped file CC BY-SA (never
  MIT), with an OBDb entry in both `THIRD_PARTY_NOTICES` files and the
  release notes, and no added terms on those files.
- **Tests use a synthetic file in OBDb's shape**, written for Havne
  (MIT), never a derived fixture, which would make the repository
  mixed-license. A file's keys and structure are a format, not OBDb's
  content.
- Nothing suggests OBDb sponsors or endorses Havne (section 2(a)(6)).
  Nothing is copied from OBDb's repositories that carry no license
  file.

## Alternatives considered

- **TOML:** comments and friendlier hand-editing, but a larger grammar
  to parse and fuzz in core, nested tables of signals that read worse
  than arrays, and no OBDb shape.
- **YAML:** comments, but a large grammar full of traps (`no` reads as
  false, `1E05` as a number) and a real dependency in core.
- **CSV, a row a signal:** spreadsheet-friendly, but enumerations,
  computed signals and ECUs don't fit a row.
- **DBC:** the CAN world's format, but it describes broadcast frames,
  not requests and replies.
- **A vendored JSON library:** less code, but a license to carry, a
  size and a fuzzing history to vet; a strict subset is small to write.
- **An extension pointer in the set's reserved room** instead of the
  layout bump: no break, but a pointer in `uint32_t` room changes size
  between 32- and 64-bit, and the break 0.x allows is the honest one.
- **Enum labels written as text in `signals.csv`:** simpler, but the
  viewer could plot nothing; the number plus the labels keeps both.
- **A linear sum for computed signals** (enough for boost and AFR):
  smaller, but not enough for a power estimate. Functions (min, max,
  abs), conditionals and computed signals feeding each other are left
  for when a signal needs them; they would be a format bump.
- **One fixed age bound for computed inputs:** simpler, but a bound
  short enough for MAP blanks boost between baro reads, and one long
  enough for baro combines a stale MAP.
- **Running a file's set alone, never merged:** simpler, but a user
  wanting `obd2` and their own signals would need two sessions.
  `extends` makes the merge opt-in.
- **"Unknown (7)" for an unlabeled enum value:** reads as a claim that
  7 is unknown to the ECU; the number alone claims nothing.
- **Refusing a mixed width at connect,** as the transports do: it
  already fails before the cable is touched, but after the file was
  loaded, picked and connected.

## Consequences

- A user's own signals, on any of four modules, need no code and load
  the same on every platform and in `havne-cli`.
- One parser serves signal files, the plugin framework's data tier and
  the OBDb import, and a fuzz target guards it.
- The C ABI breaks once, on purpose: the set's layout version 4 makes
  every out-of-tree set restate its version; ENUM's both-fields value
  touches every consumer of a batch.
- Recordings move to format 3; the reader keeps reading the older two.
- A session can hold 256 requests, at about 20 KB more memory each.
- The `source` block makes attribution travel with a file, at the cost
  of one more required key for a hand-written file.
- A computed signal's inputs can be judged by their own pace, at the
  cost of one optional key to understand.

## Appendix: the loader as built (2026-11-05)

The loader (`core/src/signal_file.cpp`, `ecuio_signal_file_load` and
its kin in `signals.h`) settles what section 2 left open:

- **`obd2`'s keys:** every field of every PID it reads, by the PID
  (`pid_0b`, and `pid_14_1` for a PID's second field), and thirteen
  names for the common ones (`map`, `baro`, `rpm` and the rest,
  [docs/signal-files.md](../signal-files.md)). A name is kept once
  published.
- **A file with no `ecus`** reads from its base's first ECU; with no
  base, a signal that reads a reply is refused.
- **Keys and ECU ids** are 1 to 31 of `a`-`z`, `0`-`9` and `_`.
- **Defaults:** a field is 8 bits, big-endian, unsigned; scale 1,
  offset 0. A scale of 0 is refused.
- **An expression** reads at least one signal; a unary minus is `0 - x`,
  an operation. The limit of 16 operations is the program's 33 steps.
- **The tables:** each holds at most `ECUIO_SET_MAX_TABLE` (65,536)
  entries, what a def's `table_first` reaches, and the validator
  refuses more.
- **A refusal's words** are `line L, column C: ` and why, the value at
  fault named and the file's path never.

## Appendix: havne-cli as built (2026-11-05)

`havne-cli` reads a signal file's path as section 4 says, and settles:

- **`signals check <file>`** writes what the loader read before any
  problem: the file's name, profile and modules, then each of its own
  signals in the file's terms (the service and number, the byte after
  the echo, the field, an enumeration's labels, a computed signal's
  inputs and their age bounds), then a line for each unit that never
  converts (section 3). A refused file is one `error` line with the
  loader's words and exit 3, the code of a check that fails.
- **`stream --signals` and `sim --signals`:** a URI that names no
  profile takes the file's, so the file isn't named twice; a URI that
  names another is a usage error, since the session would decode
  through a set the file isn't. `sim` takes one so the simulated ECU
  answers the file's DIDs from `havne-cli` too.
- **The `signals` line** marks each computed signal and carries each
  enumeration's labels, as a recording's signal line does in format 3.

## Appendix: the OBDb import as built (2026-11-05)

Core converts a set (`ecuio_obdb_import`, `obdb.h`,
`core/src/obdb_import.cpp`) to a signal file's text, which the caller
loads as any other; `havne-cli signals import obdb` writes it. The
semantics are OBDb's own decoder's (its `.schemas` repository's
`python/can/`), which the schema leaves open, and settle section 7's
questions:

- **The field:** OBDb's bit index counts from the most significant bit
  of the first byte after the echo, and `blsb` reverses the order of
  the field's whole bytes. Whole bytes are a big-endian field (a
  little-endian one under `blsb`), and a field within one byte a
  little-endian one from its lowest bit. A field across bytes that
  isn't whole bytes has no form here: the import leaves it out and
  notes it, so a set the user imports shows how many need the sub-byte
  big-endian field section 6 keeps room for, before it's added.
- **What a value is:** `raw * mul / div + add`, two's complement over
  the field's bits under `sign`. OBDb holds a value within `min` and
  `max` and blanks it at `nullmin` and `nullmax`; a signal file does
  neither, so each is a note on its signal, not a refusal.
- **The modules:** `hdr` is the request ID and `rax` the reply's. OBDb
  matches a command with no `rax` to any module; the import takes the
  standard's reply for 7E0 to 7E7 and leaves out the rest. 29-bit
  commands are left out: OBDb's decoder forms only the reply ID, not
  the request's.
- **The model year** (`year_takes`, `core/src/obdb_import.h`): a
  filter's `years`, a range with both ends, a range turned round (from
  past to, the years outside it), either end alone; no year takes only
  the commands with no filter. ADR-0052's OBDb variants take the same
  function.
- **The notes** are field by field (`ecuio_obdb_note_t`): each signal
  converted with its key, each field that differs, each signal left out
  with why, and the set's own keys (`freq`, `diagnosticLevel`,
  `signalGroups`) once.

## Appendix: a recording's signal file as built (2026-11-05)

Section 4's provenance, and how it settles:

- **No format 4.** Format 3 had reached no release, and the reader reads
  past a header key it doesn't know, so format 3 gains one optional
  line after `profile`: `signal file  signals.json,<SHA-256>`, the hash
  as 64 lowercase hex digits. A plugin's line (ADR-0052 section 4) goes
  beside it as `plugin  <id>,<version>,<variant>`.
- **The file is given, not found:** `ecuio_recorder_signal_file(recorder,
  file)` before `ecuio_session_record`, with the loaded file. Attach
  refuses (`ECUIO_ERR_INVALID_ARGUMENT`, the session's profile in the
  detail) a file whose set isn't the one decoding the session, so a
  recording never names a file that didn't decode it. The loaded file
  keeps the bytes it was read from, and the writer copies them as they
  are (binary on every platform, so the hash holds), before the header
  line naming them; a write that fails fails the recording as any does.
- **The reader** gives `ecuio_recording_get_signals`: whether the header
  names a file, whether `signals.json` is in the folder, and whether its
  SHA-256, taken at open, is the header's, with its length. A caller
  reads the bytes from the folder itself. `havne-cli view` and `info`
  put it on the `opened` line as `signal_file` (null, or `sha256`,
  `present`, `matches`).
- **The front ends:** `havne-cli stream --record` with `--signals` gives
  its file; the app gives the vehicle's copy in its store, the file
  whose set the session uses, so the hash a recording names is the
  copy's name.
- **A replay** of such a recording, with its `signals.json` loaded,
  decodes the same values in the same order (`RecorderTest`, and
  `CliRecordTest` through `havne-cli`).
- **Not yet:** a plugin variant's set is merged from several files and
  has no one file's bytes, so a recording of one holds no `signals.json`
  until a writer turns a set back into a signal file's JSON; the
  plugin's line names what decoded it meanwhile.

> **Note of 2026-11-05, a plugin variant's copy.** The writer is built:
> a variant's merged signals are written as one signal file at load and
> copied as any file is, with the plugin line after the signal file
> line ([ADR-0052](0052-plugin-framework.md)'s appendix "a variant's
> recording").

## Appendix: what each front end lacks (2026-11-06)

The app and `havne-cli` read a signal file alike (section 4). Two of
the tools around it reach one front end only:

- **Read once is the app's.** Settings' form asks the adapter for one
  draft signal, with no session connected, and shows the reply's bytes
  beside the value they decode to
  ([docs/signal-files.md](../signal-files.md)). `havne-cli` has no
  one-request command: `signals check` reads a file with no car, and
  on a car `stream --signals` runs a session over every signal the
  file holds, its frame lines carrying the replies' bytes.
- **The OBDb import is `havne-cli`'s.** `signals import obdb` writes a
  signal file, which the app then imports as any other; the app has no
  import of its own. It is placed in v0.6.0, whose scope is imports.

## Appendix: a replay reads with its recording's copy (2026-11-06)

Section 4 says a replay with a recording's `signals.json` decodes the
same values. Both front ends load that copy themselves, so a
recording whose file has since been edited or removed, or one from
another machine, replays as it was recorded:

- **When:** the replay URI selects the profile `events.log`'s header
  names, or the copy's own name (below), and `signals.json` is there
  and still the bytes the header's hash names. `havne-cli stream` uses
  it when the URI selects no profile too, and `--signals` or `--plugin`
  replaces it. A URI selecting another profile decodes as before. The
  app's URI with no `profile=` replays raw, as any replay does.
- **Under which name:** a profile name holds one set (`profile.h`: last
  wins), so the copy never takes a name another file holds. In the app,
  the signal store registering the same bytes under the recorded name
  is the vehicle's own file, and the replay uses it; otherwise the copy
  registers as `rec.` and the hash's first 16 digits, for the bridge's
  life as the store keeps its files, and the URI selects that name. The
  activity log says so. `havne-cli` registers the copy under the profile
  the URI selects, the recorded one when it selects none: a CLI
  process registers nothing else under it.
- **The check:** the app hashes the copy (SHA-256) against the header's
  line; `havne-cli` takes the reader's MATCH (`ecuio_recording_get_signals`),
  which costs one pass over `signals.csv` before the replay starts,
  since the public API has no hash of its own. A copy missing, changed
  or refused by the loader leaves the URI as given, with a line saying
  why (the activity log; `havne-cli`'s stderr).
- **A replay recorded** (the app's test seam, or `havne-cli stream
  --record`) holds the copy again as its own `signals.json`.
- **No ABI change:** the header's two lines are read by
  `cli/recording_signals.h`, which both front ends include.
