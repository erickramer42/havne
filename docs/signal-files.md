# Signal files: writing your own signals

A signal file describes signals of your own, the ones no built-in set
has: a manufacturer's DID on the transmission's module, a PID decoded your own
way, a value computed from two others. It is a JSON file that the app
and `havne-cli` read alike, on every platform, with no code in it. Its
design and the reasons behind each rule are
[ADR-0051](adr/0051-signal-files.md); this page is the reference for
writing one by hand.

> **Status:** the format is decided (ADR-0051), core's loader reads
> it (`ecuio_signal_file_load`, `signals.h`), `havne-cli` checks and
> streams a file ("Using a file") and imports an OBDb set ("Importing an
> OBDb signal set", `ecuio_obdb_import`, `obdb.h`), and a recording
> holds the file that decoded it. The app's import is v0.5.0's work too.
> None of it is in a release yet; what isn't settled is marked **Not
> settled** below.

## A first file

```json
{
  "havne_signals": 1,
  "name": "Daily driver extras",
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
      "read": { "service": "22", "number": "D100" },
      "field": { "byte": 0, "bits": 8 },
      "scale": 1, "offset": -40,
      "unit": "°C", "group": "engine"
    }
  ]
}
```

This reads DID 0xD100 from the module answering on 0x7E9 (asked on
0x7E1, the standard's rx - 8), takes its first data byte, subtracts 40
and shows it in degrees Celsius, beside every signal of the `obd2` set.
DID 0xD100 is made up for this page: a real car's DIDs are its manufacturer's,
found with the DID scan (`?scan=first`, [ADR-0020](adr/0020-uds-generic.md))
and checked with **Read once** before they're trusted.

## The file

The top level is one JSON object. Its keys:

| Key | Required | What it holds |
|---|---|---|
| `havne_signals` | yes, first | The format's version: `1`. A reader refuses a version it doesn't know |
| `name` | yes | The file's name, shown where you pick it. A loaded file registers as the profile `user.<slug>` made from it: "Daily driver extras" is `?profile=user.daily-driver-extras` |
| `source` | yes | Where the signals come from (below) |
| `extends` | no | A built-in set the file adds to: `obd2` only, in format 1 |
| `ecus` | no | The modules the file reads from (below) |
| `signals` | yes | The signals, an array (below) |
| `notes` | no | Free text, anywhere; JSON has no comments |

Any key starting `x-` is ignored, so a tool can keep its own data in a
file. Every other key is checked: one the format doesn't have refuses
the file, so a typo fails at once instead of quietly doing nothing.

### `source`

Where the file's signals came from, kept with every copy of the file
(a recording holds one), so the attribution travels with it:

```json
"source": {
  "name": "OBDb editors and contributors",
  "url": "https://github.com/OBDb/<repository>",
  "commit": "<the commit, if known>",
  "license": "CC-BY-SA-4.0",
  "modified": "Converted to Havne's signal-file format"
}
```

Only `name` is required. A file you wrote yourself needs only that
(`"Written by hand for the daily driver"`). A file converted from
another project's data says so in all five, and its terms decide what
you may do with it. OBDb's signal sets are CC BY-SA 4.0: the importer
fills the block as above, and a file you share keeps it
([ADR-0051](adr/0051-signal-files.md) section 7).

### `extends`

`"extends": "obd2"` loads the `obd2` set first and your signals after
it, so one session shows both. Without it the file is a set of its own,
and a session on it reads only your signals.

### `ecus`

Each module the file reads from, at most four, the base set's counted
when the file extends one:

```json
"ecus": [
  { "id": "pcm", "rx": "7E8" },
  { "id": "tcm", "rx": "7E9" },
  { "id": "bcm", "rx": "7B8", "tx": "7B0" }
]
```

- `id`: the name signals use to say where they're read (`"ecu": "tcm"`).
- `rx`: the module's reply ID, in hex, 11-bit (`7E9`) or 29-bit
  (`18DAF110`).
- `tx`: the module's request ID. Leave it out only where the standard
  gives it: rx - 8 for `7E8` to `7EF`, and for `18DAF1xx` the swapped
  `18DAxxF1`. Every other module needs it.

A signal with no `ecu` is read from the first module listed.

A file with no `ecus` that extends `obd2` reads from `obd2`'s engine
ECU (`7E8`, asked on `7E0`). One that extends nothing has no module to
read from, and a signal that reads a reply is refused.

## Signals

Each entry of `signals` is one signal. A number read from a reply:

```json
{
  "key": "fuel_trim_b2",
  "name": "Fuel Trim B2",
  "ecu": "pcm",
  "read": { "service": "22", "number": "D110" },
  "field": { "byte": 2, "bits": 16, "order": "be", "signed": true },
  "scale": 0.0078125, "offset": 0,
  "unit": "%", "decimals": 1, "group": "fuel", "plot": 3
}
```

| Key | Required | What it holds |
|---|---|---|
| `key` | yes | The signal's stable name: lower-case letters, digits and `_`, unique in the file. Computed signals refer to it, and later a Dashboard's tile will |
| `name` | yes | What Live, the Dashboard and the logs show: 1 to 31 bytes |
| `kind` | no | `number` (the default), `text`, `hex`, `enum` or `computed` |
| `ecu` | no | Which module answers: an `id` from `ecus` |
| `read` | all but computed | The request (below) |
| `field` | all but computed | Where the value sits in the reply (below) |
| `scale`, `offset` | no | value = raw x scale + offset; defaults 1 and 0 |
| `unit` | no | 0 to 15 bytes, shown as written (below) |
| `decimals` | no | 0 to 3, how many Live shows |
| `group` | no | The part of the car it reads, which Live and the viewer group by: `engine`, `air`, `fuel`, `oxygen sensors`, `emissions`, `vehicle`, `electrical` or `diagnostics`; left out, Other |
| `plot` | no | 0 to 255: 0 not plotted when a session starts, otherwise its place among the signals that are, 1 first |

A `key` is 1 to 31 of `a`-`z`, `0`-`9` and `_`, unique in the file,
and an ECU's `id` the same. A group is spelled as the table lists it,
`oxygen sensors` with its space.

### `read`

```json
"read": { "service": "22", "number": "D110" }
```

- `service`: `"01"` (current data, a PID) or `"22"` (a DID).
- `number`: the PID in two hex digits (`"0C"`) or the DID in four
  (`"D110"`).

The request goes to the signal's module. Signals read from the same
reply share one request, however many fields they take from it.

### `field`

Where the value sits in the reply's data:

| Key | Default | What it means |
|---|---|---|
| `byte` | (required) | The first byte, counted from the reply's data after its echo: 0 is the first data byte (J1979's A) |
| `bits` | (required for numbers) | The width, 1 to 32 |
| `bit` | 0 | The first bit within `byte`, 0 to 7, little-endian fields only |
| `order` | `"be"` | `"be"` (most significant byte first, as both services send) or `"le"` |
| `signed` | `false` | Two's complement, sign-extended from `bits` |
| `bytes` | 0 | Text and hex fields: how many bytes, 0 for the rest of the reply |

A big-endian field is whole bytes (`bits` a multiple of 8, `bit` 0); a
bit field narrower than a byte is little-endian (`"order": "le"`). A
one-bit flag in the second data byte's fourth bit:

```json
"field": { "byte": 1, "bit": 3, "bits": 1, "order": "le" }
```

### Text and hex

`"kind": "text"` shows the field's bytes as text (printable ASCII as it
is, anything else as `.`); `"kind": "hex"` as hex pairs (`"41 0C 1F
40"`). Both take `bytes` instead of `bits`, and no scale or unit:

```json
{
  "key": "tcm_part",
  "name": "TCM Part Number",
  "ecu": "tcm",
  "read": { "service": "22", "number": "F187" },
  "field": { "byte": 0 },
  "kind": "text",
  "group": "diagnostics"
}
```

### Enumerations

A raw number shown as a word: `"kind": "enum"` and a `values` table,
each decimal raw value to its label:

```json
{
  "key": "gear_mode",
  "name": "Gear Mode",
  "ecu": "tcm",
  "read": { "service": "22", "number": "D101" },
  "field": { "byte": 0, "bits": 8 },
  "kind": "enum",
  "values": { "0": "Park", "1": "Reverse", "2": "Neutral", "3": "Drive" }
}
```

- At most 256 entries a signal, each label at most 27 bytes.
- A raw value with no label shows as its number (`7`), so nothing
  reads as a claim the file doesn't make.
- A recording keeps the number and the labels, so the viewer can plot
  the number and show the word.

### Computed signals

A value worked out from other signals, in core, so Live, the Dashboard,
the recording and `havne-cli` all get the same final number:

```json
{
  "key": "boost",
  "name": "Boost",
  "kind": "computed",
  "expr": "obd2.map - obd2.baro",
  "max_age": { "obd2.baro": 30 },
  "unit": "kPa", "decimals": 0, "group": "air", "plot": 1
}
```

**The expression** (`expr`):

- `+`, `-`, `*` and `/`, the usual precedence (`*` and `/` before `+`
  and `-`, left to right), and parentheses.
- Decimal constants: `1.8`, `100`, `0.5`.
- Inputs by key: a signal of this file (`tft`), or one of the base
  set's as `obd2.<key>` (`obd2.map`, `obd2.baro`).
- At most 4 inputs and 16 operations. No functions, no conditionals,
  and a computed signal can't be an input of another.

Division by zero, or a result that isn't a finite number, gives no
value for that moment rather than a wrong one.

`obd2`'s keys: every field of every PID it reads by the PID, `pid_0b`
for a PID's first field and `pid_14_1` for its second (`_1` to `_7`),
and these names for the common ones:

| Key | PID | Key | PID |
|---|---|---|---|
| `load` | 04 | `throttle` | 11 |
| `coolant` | 05 | `fuel_level` | 2F |
| `map` | 0B | `baro` | 33 |
| `rpm` | 0C | `module_voltage` | 42 |
| `speed` | 0D | `ambient` | 46 |
| `iat` | 0F | `oil_temp` | 5C |
| `maf` | 10 | | |

An expression reads at least one signal (a constant alone isn't one),
and a unary minus counts as an operation (`-x` is `0 - x`).

**Inputs' ages** (`max_age`): a computed value is worked out when one of
its inputs gets a new value, from each input's newest, and only while
each input is fresh enough. By default an input older than 3 s (the age
at which Live dims a value) stops the computation. `max_age` sets the
bound per input, in seconds, 0.1 to 60:

```json
"max_age": { "obd2.baro": 30 }
```

Boost is why: MAP is read often and moves fast, barometric pressure is
read rarely and barely moves, so a baro reading 30 s old is still right,
and a 3 s bound on it would blank boost between its reads. Live still
dims a stale input as it does any signal; the computed value goes quiet
only when an input passes its own bound.

### Units

A unit is shown exactly as written. These convert when the imperial
display is chosen ([ADR-0022](adr/0022-display-units.md)), and only
when written exactly so:

| Written | Shown imperial |
|---|---|
| `km/h` | `mph` |
| `°C` | `°F` |
| `kPa` | `psi` |
| `g/s` | `lb/min` |
| `km` | `mi` |
| `Pa` | `inH2O` |
| `L/h` | `gal/h` |
| `N·m` | `lb·ft` |

Any other unit (`%`, `V`, `rpm`, `ms`) loads, shows as written and
never converts; `signals check` lists them, so a misspelled `kph`
doesn't pass for `km/h`.

## Bounds

| What | Bound |
|---|---|
| A file | 1 MiB, UTF-8 |
| Signals | 1024 a file |
| Modules | 4, the base set's counted |
| A session's requests | 256, the base set's counted (`obd2` alone asks over 90) |
| `name` | 1 to 31 bytes |
| `unit` | 0 to 15 bytes |
| `decimals` | 0 to 3 |
| `plot` | 0 to 255 |
| `bits` | 1 to 32; `bit` 0 to 7 |
| An enumeration | 256 labels, each 27 bytes at most |
| `expr` | 4 inputs, 16 operations |
| `max_age` | 0.1 to 60 s |

## What is refused at load

Everything a file can get wrong is refused when it's loaded, by the app
and by `havne-cli signals check` alike, so a file fails on the desk
instead of at the car. A refusal names the line, the column and the key,
never the file's path (which can hold your account name):

```
line 12, column 9: unknown key 'scael'
ecu 'bcm': rx 7B8 needs a tx
```

A file is refused for:

- **Its shape:** text that isn't JSON, a duplicate key in an object, a
  value of the wrong type, a key the format doesn't have, a missing
  required key, a `havne_signals` other than 1, more than the bounds
  above.
- **Its signals:** a duplicate `key`; a `field` the decoder can't read
  (a big-endian field that isn't whole bytes, `bit` on a big-endian
  field, `bits` out of range); a text or hex field with `bits`; an
  `ecu` the file doesn't list; an enumeration's label too long; an
  `expr` that doesn't parse, names an input that doesn't exist, takes
  a computed signal as an input, or passes 4 inputs or 16 operations.
- **A name, a unit or a label a spreadsheet would run as a formula:**
  one starting with `=`, `+`, `-`, `@`, a tab or a carriage return.
  Recordings are CSV, often opened in a spreadsheet
  ([ADR-0049](adr/0049-recordings-in-core.md) section 8).
- **Its modules:** a `tx` left out where the standard doesn't give one
  (above); a `tx` of another width than its `rx`; more than four; and
  **11-bit and 29-bit IDs in one file**, the base set's counted. One
  connection carries one width (a J2534 or ELM327 session refuses the
  mix before it opens the adapter), and `obd2`'s modules are 11-bit, so
  a file that extends it takes 11-bit modules only.
- **Its size:** more signals than fit a session's 256 requests with the
  base set's.

Every refusal names the line and column of what it refuses, then why:

```text
line 12, column 9: unknown key 'scael' in signal 'tft'
line 7, column 5: ecu 'bcm': rx 7B8 needs a tx
line 21, column 15: expr 'map - baro' reads 'map', which no signal is (at character 1)
```

A unit outside the table above isn't refused: it loads and shows as
written, and `signals check` says it won't convert.

## Using a file

- **`havne-cli signals check <file>`** reads the file as a session
  would and says what it read, one JSON line each: the file's name,
  profile and modules (`signal_file`), each of its signals in the file's
  own terms (`signal_def`: the service and number, the byte after the
  echo, the field, an enumeration's labels, a computed signal's inputs
  with their age bounds), then each unit that never converts (`unit`).
  A refused file is an `error` line with the loader's words and exit 3.
- **`havne-cli stream <uri> --signals <file>`** loads the file,
  registers its profile and reads through it: a URI that names no
  profile takes the file's (`?profile=user.<name>` added), and one that
  names another is a usage error. For example `havne-cli stream
  elm327://COM5 --signals extras.json --values`; the `signals` line
  carries each enumeration's labels and marks each computed signal.
- **`havne-cli sim <uri> --on <vcan> --signals <file>`** does the same
  for the simulated ECU, which then answers the file's Service 22 DIDs
  ("Checking one with no car").
- **The app:** in Settings' Vehicle section, **Import a signal file**
  adds a file to the picked vehicle. A file the loader refuses shows its
  words, and so does one whose profile another file makes already, in
  any vehicle: one profile names one file. Each file the vehicle has is
  then a profile under **What to read**, named as the file's profile
  (`?profile=user.daily-driver-extras`), and **Duplicate** copies the
  vehicle's files with the rest of it. **Remove** takes a file from the
  vehicle. On an adapter (`j2534://`, `elm327://`), where **Default**
  reads `obd2`, a file that extends `obd2` takes its place: its button
  reads **obd2 + <name>**, and Default's shows only while the field
  reads it. Imported while the field reads Default with no `profile=`
  of its own, the file is read instead, and the line under the files
  says so; a profile you picked stays. Of two such files, the first
  stands in and the second keeps a button of its own.
- **The app's form:** under **Your own signals**, the same section
  writes a vehicle's own file, one signal at a time: its name, the
  module's reply ID (and its request ID where the standard gives none),
  the service and number, the field, then the scale, offset, unit and
  decimals, or the bytes of a text or hex field, or an enumeration's
  values (`0=Park, 3=Drive`), and a group. **Save** adds the signal to
  the file `<vehicle> own` (`user.<vehicle>-own`), or replaces the one of
  the same name; **Edit** fills the form from a saved signal, and
  **Remove** takes it out, the file going with its last. A module has
  one request ID for all its signals: a typed one is the module's, and
  the note under the form names the other signals it moves; left empty,
  the module keeps the one it has, or the standard's when no other
  signal reads it. The form checks nothing itself: the loader reads the
  file it makes and says what's wrong in its own words.
- **Save a copy:** each of the vehicle's files is saved where you
  choose (the desktop's save dialog, Android's document picker), named
  for its profile (`daily-driver-own.json`), so it can be shared or
  given to `havne-cli --signals`. The saved file is the app's copy,
  byte for byte: what was imported or typed into the form, and nothing
  about the vehicle (no VIN). A copy that has changed since it was kept
  isn't saved, and the line under the files says to import it again.
- **Read once:** with no session connected, the form's signal is asked
  once on the adapter the URI field names (or the vehicle's last), in a
  short session of its own: a Service 22 DID through `uds-generic`, a
  Service 01 PID through `raw`'s `?poll=` (`obd2`'s on `socketcan://`,
  where a profile that only listens asks nothing). It shows the request and the
  reply's bytes in hex beside the value the draft decodes to, or the
  ECU's refusal by its ISO 14229 name (`requestOutOfRange (7F 22 31)`),
  so the field and the scaling are checked before the signal is saved.
  The draft goes through the loader first, so a mistake is named before
  anything is sent. A DID is asked only of a module whose request ID
  follows the reply ID's rule (rx - 8, or the swapped 29-bit pair).
- **A recording** of a session the file decodes copies it into its
  folder as `signals.json`, byte for byte, with its SHA-256 on
  `events.log`'s header (`signal file  signals.json,<SHA-256>`), so it
  says exactly what decoded it: the app copies the vehicle's file, and
  `havne-cli stream --record` the one `--signals` names (with
  `--plugin`, the variant's files merged into one). A replay of the
  recording decodes with that copy, the same values, even after the
  vehicle's file has changed or gone, or on another machine: the app's
  Replay does when the copy differs from the file loaded now (under a
  name of its own, `rec.` and the start of its hash, which the activity
  log names), and so does `havne-cli stream` with no `--signals`:

  ```sh
  havne-cli stream "replay://<the recording's folder>" --values
  ```

  A copy that no longer matches the hash on the header isn't used: the
  replay decodes with the profile as loaded now and says why.

  `havne-cli info <folder>` says on its `opened` line whether the copy
  is there and is still the bytes the header names (`signal_file`).

### Where Havne keeps them

The app keeps a copy of each file it imports, so the vehicle still has
it when the original moves or the phone's picker loses access to it:

- **The folder:** `signals/` in the app's data folder (Windows
  `%APPDATA%\Havne\Havne\signals`, Linux `~/.local/share/Havne/Havne/signals`,
  macOS `~/Library/Application Support/Havne/Havne/signals`, and the
  app's own storage on a phone), or `signals/` under
  `HAVNE_SETTINGS_DIR` when that's set.
- **The name:** `<sha256>.json`, the SHA-256 of the file's bytes in
  lower-case hex, the bytes as imported. Two vehicles with the same file
  share one copy, and the hash is the one a recording names.
- **Which vehicle has which:** the vehicle list in the app's preferences
  names each vehicle's files by their hash; the copies are only read.
- **When a copy goes:** when no vehicle has its file any more (Remove,
  Forget, Forget all), its profile first. A copy that is missing, has
  changed, or no longer loads is shown with why, and its profile isn't
  offered.

**Not settled:** whether Read once reads while a session is connected.

## Importing an OBDb signal set

[OBDb](https://github.com/OBDb) keeps signal sets for many vehicles,
one repository each, as JSON (`signalsets/v3/default.json`, and files
for ranges of model years). `havne-cli signals import obdb` converts one
you have on disk into a signal file; Havne fetches nothing and ships no
OBDb data:

```sh
havne-cli signals import obdb default.json --out extras.json --year 2019 \
  --url https://github.com/OBDb/<repository> --commit <the commit>
```

It writes the file whole or not at all, and says field by field what it
did, one JSON line a note (`import_note`): each signal converted, each
field that differs from OBDb's reading, and each signal left out and
why; then a count of each (`imported`) and `signals check`'s lines for
the file written. A set that isn't OBDb's is an `error` line and exit 3.

What converts:

- **Commands:** Service 01 and 22 on 11-bit IDs. `hdr` is the module's
  request ID and `rax` its reply's; with no `rax`, a request ID from
  7E0 to 7E7 takes the reply ID the standard gives it, 8 above, and
  any other is left out. At most four modules, `obd2`'s counted unless
  `--alone` leaves `obd2` out. A command's `filter` takes the model years
  it names: give `--year` for those, or they're left out.
- **Fields:** OBDb counts bits from the top of the first byte after the
  echo. Whole bytes become a big-endian field (little-endian under
  `blsb`), and a field within one byte a little-endian one. A field
  across bytes that isn't whole bytes has no form in a signal file and
  is left out.
- **Values:** `sign` is `signed`, `mul` / `div` the scale, `add` the
  offset, the decimals the scale's step; the unit in the built-in sets'
  spelling (`celsius` is `°C`, `kilometersPerHour` `km/h`), and a word
  that isn't a unit (`onoff`, `yesno`) left off, the value its number. A
  `map` is an enumeration, each label its `description` (or its `value`),
  cut to 27 bytes. The first part of `path` is the group, `description`
  the notes.
- **Synthetics:** a `ratio` of two converted signals is a computed
  signal; any other formula is left out.

What differs, each noted where it applies:

- **`min` and `max`:** OBDb holds a value within them; Havne shows it as
  read.
- **`nullmin` and `nullmax`:** OBDb shows nothing at or past them; Havne
  shows the value.
- **`freq`:** Havne asks each request in turn, not on OBDb's period.
- **`din`:** OBDb enters a diagnostic session first; Havne asks in the
  default one, where the module may refuse.

What's left out: Service 21, K-line commands, 29-bit IDs, extended
addressing, functional requests, hidden signals and fields wider than
32 bits. The converted file's `source` names OBDb's editors and
contributors, the license, the URL and commit you give, and that it was
converted ([ADR-0051](adr/0051-signal-files.md) section 7).

## Checking one with no car

The simulated ECU answers a registered profile's Service 22 DIDs
([ADR-0046](adr/0046-simulated-ecu.md)'s appendix of 2026-11-05): each
signal's field written from a value you give it, its scale and offset
worked backwards, on the module its `ecu` names. On Linux, with the
virtual bus up ([linux.md](linux.md), "SocketCAN"), a file's DIDs can be
read end to end before a car ever sees it.
