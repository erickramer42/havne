# Signal files: writing your own signals

A signal file describes signals of your own, the ones no built-in set
has: a manufacturer's DID on the transmission's module, a PID decoded your own
way, a value computed from two others. It is a JSON file that the app
and `havne-cli` read alike, on every platform, with no code in it. Its
design and the reasons behind each rule are
[ADR-0051](adr/0051-signal-files.md); this page is the reference for
writing one by hand.

> **Status:** in v0.5.0: core's loader (`ecuio_signal_file_load`,
> `signals.h`), the app's import, its form for a vehicle's own signals
> and **Read once**, `havne-cli`'s check, stream and OBDb import
> (`ecuio_obdb_import`, `obdb.h`), and a recording holding the file that
> decoded it. A replay that decodes with its recording's copy and
> **Save a copy** come with the next release. What isn't settled is
> marked **Not settled** below.

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

## From a scan to a shared file

A walkthrough in the app, for a DID you don't know yet, with the
`havne-cli` form of each step after it. Every step reads; none writes
to the car.

1. **Find what a module answers.** In Settings, Connection, under
   **Profile**, pick **uds-generic (DIDs)**, and give the DID field a
   range, with the module's reply ID before it for any module but the
   engine's:
   `7E9:D800-D8FF`. Add `&scan=first` to the URI, so the range is asked
   before anything is polled, and connect. Each DID that answers joins
   Live as a row, `0x7E9 DID D8xx`, showing its reply's bytes in hex,
   and the activity log's `ECU 0x7E9:` line counts the scan
   (`scan: 256 DIDs, 3 found, 253 refused`). The same with `havne-cli`:

   ```sh
   havne-cli stream "elm327://COM5?profile=uds-generic&did=7E9:D800-D8FF&scan=first" \
     --values --until-scanned
   ```

2. **Watch which bytes move.** With the car doing what the signal
   should follow (the transmission warming, a gear change), the bytes
   that follow it are the field. With **Record sessions** on (Settings,
   **Recording**), the session can be read again in the Recordings
   viewer.
3. **Write the signal and check it with Read once.** Disconnect, then
   under **Your own signals** fill in its name, the module's reply ID,
   service 22 and the DID, the field (its first byte after the DID
   echo, and its bits), and the scale, offset and unit. **Read once**
   asks the module for it once and shows the reply's bytes beside the
   value the draft decodes to; when that isn't what the car shows,
   change the field or the scaling and read again. **Save** adds it to
   the vehicle's own file, `<vehicle> own`.
4. **Read it in a session.** With the vehicle picked, its file is a
   profile under **Profile** (`user.daily-driver-own` for the
   vehicle "Daily driver"), and Live shows the signal by its name, in
   its unit. A signal the module refuses shows as refused, with the
   code by name ("Refused: requestOutOfRange (7F 22 31)").
5. **Record with it.** A recording holds the file as `signals.json`, so
   a replay decodes the same values, even after the file has changed
   or gone ("Using a file"). A replay asks nothing, so a signal the
   module refused shows no value there, not refused; the recording's
   `events.log` names the refusal in its ECU line.
6. **Share it.** **Save a copy** writes the file where you choose, as
   `daily-driver-own.json`. Another machine's app imports it (**Import
   a signal file**), and `havne-cli` reads it as it is:

   ```sh
   havne-cli signals check daily-driver-own.json
   havne-cli stream elm327://COM5 --signals daily-driver-own.json --values
   ```

## The file

The top level is one JSON object. Its keys:

| Key | Required | What it holds |
|---|---|---|
| `havne_signals` | yes, first | The format's version: `1`, `2` (more for a computed signal, below) or `3` (files read together, below). A reader refuses a version it doesn't know |
| `name` | yes | The file's name, shown where you pick it. A loaded file registers as the profile `user.<slug>` made from it: "Daily driver extras" is `?profile=user.daily-driver-extras` |
| `source` | yes | Where the signals come from (below) |
| `extends` | no | A built-in set the file adds to: `obd2` only, in format 1 |
| `needs` | no | Format 3: the keys the file's expressions read from another file (below) |
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
and a session on it reads only your signals. A signal of yours named as one
of `obd2`'s (`Boost`, say) replaces it: the session shows yours, in its
place, and a gauge or layout that named `obd2`'s signal reads yours. So does a computed signal of `obd2`'s: name yours `Intake Manifold Pressure` or `Barometric Pressure` and Boost is worked out from it, in its unit, so keep it kPa. A signal a computed one reads is a number or an enum, or in a format 2 file a computed signal: the loader refuses a text in its place, a computed one in a format 1 file, and a computed one that `obd2`'s Boost reads with its 30 s bound (baro: a computed input's ages are its own inputs'), each naming the computed signal that reads it. A computed replacement joins the file's chains, so a circle through it, or a chain past 8 deep (`obd2`'s Boost counted), is refused too.

### `ecus`

Each module the file reads from, at most 32, the base set's counted
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
- `name`: optional, what a screen calls the module (`"Transmission"`), 1
  to 63 bytes. Left out, the `id` stands in upper case. Havne 0.9.1 or
  later reads it; an older one refuses the key.

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
| `name` | yes | What Live, the Dashboard and the logs show: 1 to 31 bytes, one signal each in the file (a name twice is refused) |
| `kind` | no | `number` (the default), `text`, `hex`, `enum` or `computed` |
| `ecu` | no | Which module answers: an `id` from `ecus` |
| `read` | all but computed | The request (below) |
| `field` | all but computed | Where the value sits in the reply (below) |
| `scale`, `offset` | no | value = raw x scale + offset; defaults 1 and 0 |
| `unit` | no | 0 to 15 bytes, shown as written (below) |
| `decimals` | no | 0 to 3, how many Live shows |
| `group` | no | The part of the car it reads, which Live and the viewer group by: `engine`, `air`, `fuel`, `oxygen sensors`, `emissions`, `vehicle`, `electrical` or `diagnostics`; left out, Other |
| `plot` | no | 0 to 255: 0 not plotted when a session starts, otherwise its place among the signals that are, 1 first |
| `gauge` | no | Number and computed signals: the range a dial shows and its warning levels (below) |

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

The bound also has to cover the time between two inputs' reads. Asked
once a sweep, two inputs arrive about a sweep apart at worst; with the
whole of a large set asked, a sweep can take longer than 3 s, and a
signal computed from two of its inputs then never has a value ("waiting
for data", in none of the unsupported, absent or unasked lists). A
`max_age` near the sweep's length on the input read least often (Live's
Delivery line and the support report give the sweep, and a `max_age` is
at most 60 s), or asking fewer signals, fixes it.

**Trying an expression.** `havne-cli signals expr --signals my.json
"max(coolant, oil) - 40"` reads the expression as a computed signal of
that file would be read: what it reads, or the character it is refused at
and why. Add `--value coolant=95 --value oil=110@1.5` (a value, and
optionally its age in seconds after an `@`) to work it out from those
values; with no value the line says which signal is missing or too old.
`--key hottest` checks it as an edit of that signal, which catches an
expression that would read itself. With no expression the command lists
the name each signal is read by: a file's own key, or `obd2.rpm` and its
kin in a file that extends `obd2`. A plugin's variant is checked the same
with `--plugin <pkg> --variant <id>`.

**In the app.** Settings, Signals, Add a signal has a computed type that
writes the signal for you ([user-guide.md](user-guide.md)). It writes
`kind`, `expr`, `unit`, `decimals` and `group` and no `read` or `field`,
and raises `havne_signals` to 2 when the expression needs it. A computed
signal whose key starts with a digit loads, but another expression can't
read it by that key (an expression reads a leading digit as a number), so
the form refuses such a name for a new signal.

### Units

A unit is shown exactly as written. These convert when the imperial
display is chosen ([ADR-0022](adr/0022-display-units.md)), and only
when written exactly so:

| Written | Shown imperial |
|---|---|
| `km/h` | `mph` |
| `°C` | `°F` |
| `kPa` | `psi` |
| `bar` | `psi` |
| `g/s` | `lb/min` |
| `km` | `mi` |
| `Pa` | `inH2O` |
| `L/h` | `gal/h` |
| `N·m` | `lb·ft` |

Any other unit (`%`, `V`, `rpm`, `ms`) loads, shows as written and
never converts; `signals check` lists them, so a misspelled `kph`
doesn't pass for `km/h`.

### Gauges

A number or computed signal can say how a dial should show it:

```json
"gauge": { "min": 40, "max": 130, "warn_low": 60, "warn_high": 110 }
```

| Key | Required | What it holds |
|---|---|---|
| `min`, `max` | yes | The dial's range, `min` below `max` |
| `warn_low` | no | The level at or below which it warns, within the range |
| `warn_high` | no | The level at or above which it warns, within the range, above `warn_low` |

The numbers are in the signal's own `unit`, the ones its value has after
`scale` and `offset`. A front end converts them as it converts the value
([Units](#units)).

A range may be wider than the field can read, or narrower. One that
shares nothing with what the field can read is refused, since the needle
could never move: the usual cause is numbers in another unit. A computed
signal has no field, so its range isn't judged against one.

`havne-cli` reports a gauge on the `signal_def` lines of `signals check`
and `plugins check` and on the `signals` line of `stream`, and the Python
package through `SignalFile.gauge(key)` and `Plugin.gauge(key)`. The
app's dials don't read it: each still starts from the range the field
decodes to.

`gauge` needs Havne 0.7.2 or later; an older one refuses a file that uses
it as a key it doesn't know, and loads every other file as before. A
plugin whose data files use it names 0.7.2 or later in `requires.core`,
so an older Havne says which version it needs. A recording made through
such a file keeps the key in its `signals.json`, so an older Havne
refuses that recording's scrubbed export too, its `signals.json` being a
file it can't load. `havne-cli plugins check` says when a plugin's data
files use `gauge` and its `requires.core` names an older version.

## Format 3

`"havne_signals": 3` is for files that are read together with others
([ADR-0065](adr/0065-car-sized-profile.md) section 9). It keeps format
2's expressions and changes this:

- **A name may repeat.** Two signals of one file may share a name (two
  modules' "Battery voltage"); a key still names one signal.
- **No signal of `obd2`'s is replaced.** A signal named as one of
  `obd2`'s is a second signal of that name, read beside it, and
  `obd2.<key>` in an expression always reads `obd2`'s.
- **One address, one signal.** Two signals of the file that read the
  same module, request and field are refused, as two of one key are. A
  signal at one of `obd2`'s addresses is still taken, beside it.
- **`needs`** (below).

Havne 0.9.1 or later reads a format 3 file; an older one refuses it by
its version. Nothing raises a file to format 3 by itself: a file that
uses none of this stays the format it is.

### Reading another file's signals

A computed signal can read a signal another file defines. The file
lists the keys it reads that way in `needs`:

```json
{
  "havne_signals": 3,
  "name": "My car signals",
  "source": { "name": "Written by hand" },
  "needs": ["oil_t"],
  "signals": [
    { "key": "oil_margin", "name": "Oil margin", "kind": "computed",
      "expr": "130 - oil_t", "unit": "°C" }
  ]
}
```

- A need is a key another file provides: given once, not a key this
  file defines, and read by at least one expression. A need nothing
  reads is refused, as a key that is neither defined nor needed is: both
  are what a typing error looks like.
- **Loaded alone, the file still loads.** A computed signal that reads a
  needed key is checked whole (its syntax, its other inputs, its bounds)
  and then left out of the set with a note, and so is each signal
  computed from it. The other signals read as written, and a file may
  load with every signal left out.
- **Read together** with a file that defines the key, the signal
  computes. When no file defines it, the signal is left out with a note
  and nothing is refused.
- A need never names `obd2.<key>`: a file that reads `obd2` extends it.
- A plugin's data file takes `needs` too. A key another file of the
  same variant defines is met inside the variant.

### A set made of several files

Core can read several loaded files as one set (`ecuio_signal_file_merge`):
a plugin of your own over an import over a manufacturer's package, with
`obd2` under them. The files are layers in the order given, and a later
file's signal takes the place of an earlier one's:

1. **The same key:** it takes that signal's place in the list.
2. **The same address** (module, request and field): it takes that
   signal's place and keeps its key beside its own, so a gauge, a
   condition or an expression that named the earlier signal reads the
   later one.
3. **Otherwise** it is added at the end.

A name alone replaces nothing. Modules at the same reply and request IDs
are one module, under the earliest file's `id`; an `id` another module
already has takes `_2`. What the set can't hold or compute is left out
with a note and the rest still reads: a module whose reply ID another
answers under another request ID, a module past 32, a file whose IDs are
of the other width, and a computed signal that reads a key no file gives,
a text, or a signal left out.

The result is written as one format 3 file, which a recording of such a
session keeps as its `signals.json`. Havne writes it; you don't:

```json
{
  "havne_signals": 3,
  "name": "2 parts read together",
  "source": { "name": "A merge of 2 parts" },
  "extends": "obd2",
  "parts": [
    { "profile": "plugin.example-import.base", "name": "Example import", "format": 2,
      "plugin": { "id": "example-import", "version": "1.2.0", "variant": "base" },
      "source": { "name": "A made-up import", "license": "CC-BY-SA-4.0" } },
    { "profile": "user.my-car-signals", "name": "My car signals", "format": 3,
      "source": { "name": "Written by hand" } }
  ],
  "ecus": [ { "id": "tcm", "name": "Transmission", "rx": "7E9", "part": 0 } ],
  "signals": [
    { "key": "oil_temp", "part": 1, "replaced": ["oil_t"], "name": "Oil temperature",
      "ecu": "tcm", "read": { "service": "22", "number": "4A10" },
      "field": { "byte": 0, "bits": 8 }, "offset": -40, "unit": "°C" }
  ]
}
```

- `parts` lists the files in the order they were read, each with the
  profile it registers under alone, its name, its own format, its plugin
  when it is a plugin's variant, and its `source` whole. **Each part
  keeps its source and its license:** the set's own `source` says only
  that it is a merge, so share-alike terms stay with the signals they
  came with and reach no other part.
- Each signal names its `part`, and `replaced` lists the keys of the
  signals it took the place of. An ECU's `part` is the part whose `id`
  stands for the module.
- The set's profile is `merged.` and 16 hex digits worked out from the
  parts' profiles in order (and `obd2` when it is the base), so the same
  files in the same order give the same profile on every device.
- A merged set is not imported and is no part of another: import, or
  read together, the plugins it names.

## Bounds

| What | Bound |
|---|---|
| A file | 1 MiB, UTF-8 |
| Signals | 1024 for each file |
| Modules | 32, the base set's counted |
| An ECU's requests | 1024, the base set's counted (`obd2` alone asks over 90 of the engine ECU) |
| A variant's files together | 16,384 signals; its one written file 4 MiB |
| Files read together | 16 files; 16,384 signals and as many replaced keys; the written set 4 MiB |
| An ECU's `name` | 1 to 63 bytes |
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
  required key, a `havne_signals` this Havne doesn't read, more than
  the bounds above.
- **Its signals:** a duplicate `key`; a `field` the decoder can't read
  (a big-endian field that isn't whole bytes, `bit` on a big-endian
  field, `bits` out of range); a text or hex field with `bits`; an
  `ecu` the file doesn't list; an enumeration's label too long; an
  `expr` that doesn't parse, names an input that doesn't exist, takes
  a computed signal as an input, or passes 4 inputs or 16 operations;
  a `gauge` on a text, hex or enumeration signal, whose `min` isn't below
  its `max`, whose level is outside its range or out of order, or whose
  range the field can't reach.
- **A name, a unit or a label a spreadsheet would run as a formula:**
  one starting with `=`, `+`, `-`, `@`, a tab or a carriage return.
  Recordings are CSV, often opened in a spreadsheet
  ([ADR-0049](adr/0049-recordings-in-core.md) section 8).
- **Its modules:** a `tx` left out where the standard doesn't give one
  (above); a `tx` of another width than its `rx`; more than 32; and
  **11-bit and 29-bit IDs in one file**, the base set's counted. One
  connection carries one width (a J2534 or ELM327 session refuses the
  mix before it opens the adapter), and `obd2`'s modules are 11-bit, so
  a file that extends it takes 11-bit modules only.
- **Its size:** more signals than one ECU's 1024 requests
  hold with the base set's.
- **Format 3's rules:** two signals of the file at one address; a need
  that is no key, is given twice, is a key the file defines, or is read
  by no expression; `needs` in a format 1 or 2 file; and a merged set's
  keys (`parts`, a signal's `part` and `replaced`, an ECU's `part`)
  anywhere but in a merged set.

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
- **The app:** on Settings' Signals page, **Import a signal file**
  adds a file to the picked vehicle. A file the loader refuses shows its
  words, and so does one whose profile another file makes already, in
  any vehicle: one profile names one file. Each file the vehicle has is
  then a profile under Connection's **Profile**, named as the file's
  profile (`?profile=user.daily-driver-extras`), and **Duplicate** copies the
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

The app keeps each file it imports as a plugin of its own, so the vehicle
still has it when the original moves or the phone's picker loses access
to it:

- **The folder:** `plugins/` in the app's data folder (Windows
  `%APPDATA%\Havne\Havne\plugins`, Linux `~/.local/share/Havne/Havne/plugins`,
  macOS `~/Library/Application Support/Havne/Havne/plugins`, and the
  app's own storage on a phone), or `plugins/` under
  `HAVNE_SETTINGS_DIR` when that's set.
- **The name:** `<sha256>.havne-plugin.json`: the plugin Havne wraps the
  file in, with the file as its one variant, named by the SHA-256 of the
  plugin's own bytes. The variant's file is the one a recording names.
- **Which vehicle has which:** the vehicle book in the app's preferences
  names each vehicle's picks; `store.json` beside the plugins holds the
  store's facts about them.
- **When a copy goes:** when you remove the plugin. A copy that is
  missing, has changed, or no longer loads is shown with why, and its
  profile isn't offered.
- **Before v0.8.0** the app kept each file as `<sha256>.json` in
  `signals/`. The first start of v0.8.0 or later moves those files into
  plugins, and the second removes the folder and the old vehicle list,
  except the files that couldn't be moved: those stay until each is saved
  or removed. Files an older version added after the move are removed with
  the rest, so a downgrade past v0.8.0 shows no vehicles or signal files.

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

In the app, **Import an OBDb set** on Settings' Signals page converts
a set for the picked vehicle, with no session connected (on Android
through the document picker). The **OBDb model year** beside it is the
year to convert for: typed, or filled in from a VIN, the one typed under
**Look up a VIN** or the connected car's once its VIN has named the
picked vehicle; left empty, the commands for some years only are left
out. The report shows before anything is kept: what converted, each
field that differs and each signal left out, and the set's own notes.
**Keep** adds the file to the vehicle as **Import a signal file** adds
one, named "<label> OBDb"; **Discard** keeps nothing. The same set
imported again for the vehicle, for another year say, replaces its
earlier import, and the report says so before **Keep**; a file whose
name you chose is never replaced, and the import takes a name of its
own beside it.

What converts:

- **Commands:** Service 01 and 22 on 11-bit IDs. `hdr` is the module's
  request ID and `rax` its reply's; with no `rax`, a request ID from
  7E0 to 7E7 takes the reply ID the standard gives it, 8 above, and
  any other is left out. At most 32 modules, `obd2`'s counted unless
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
