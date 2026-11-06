# Signal files: writing your own signals

A signal file describes signals of your own, the ones no built-in set
has: a maker's DID on the transmission's module, a PID decoded your own
way, a value computed from two others. It is a JSON file that the app
and `havne-cli` read alike, on every platform, with no code in it. Its
design and the reasons behind each rule are
[ADR-0051](adr/0051-signal-files.md); this page is the reference for
writing one by hand.

> **Status:** the format is decided (ADR-0051), core's loader reads
> it (`ecuio_signal_file_load`, `signals.h`) and `havne-cli` checks and
> streams a file ("Using a file"). The app's import is v0.5.0's work too.
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
DID 0xD100 is made up for this page: a real car's DIDs are its maker's,
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
  vehicle.
- **A recording** copies the file into its folder (`signals.json`) with
  its SHA-256, so it says exactly what decoded it, and a replay with
  that file decodes the same.

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

**Not settled:** the form under Settings' Vehicle section that writes a
vehicle's own file, with **Read once** showing a signal's raw reply
bytes beside its decoded value, so the scaling is checked before it's
trusted.

## Checking one with no car

The simulated ECU answers a registered profile's Service 22 DIDs
([ADR-0046](adr/0046-simulated-ecu.md)'s appendix of 2026-11-05): each
signal's field written from a value you give it, its scale and offset
worked backwards, on the module its `ecu` names. On Linux, with the
virtual bus up ([linux.md](linux.md), "SocketCAN"), a file's DIDs can be
read end to end before a car ever sees it.
