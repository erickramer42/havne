# Condition files: writing your own conditions

A condition is a named test of a session: "the coolant is above 110",
"full throttle past 2500 RPM", "the link dropped". A condition file is a
JSON file of them that the app, `havne-cli` and the Python package read
alike, on every platform, with no code in it. The design and the reasons
behind each rule are [ADR-0063](adr/0063-conditions.md); this page is
the reference for writing a file by hand.

> **Status:** core's loader (`ecuio_condition_set_load`,
> `conditions.h`), `havne-cli conditions check`, the JSON Schema and the
> Python package's `ConditionSet` read and check a file. Nothing judges
> a condition during a session yet: the alert, the recording and the
> marker a file asks for are read and reported, not acted on.

## A first file

```json
{
  "havne_conditions": 1,
  "name": "Daily driver",
  "conditions": [
    {
      "key": "coolant_high",
      "name": "Coolant high",
      "when": { "signal": ["obd2.coolant"], "unit": "°C",
                "above": 110, "clear": 105 },
      "for": 2,
      "alert": { "level": "danger", "banner": true },
      "mark": true
    }
  ]
}
```

It reads: when `obd2`'s coolant temperature has been above 110 deg C for
two seconds, the condition turns ON, as a danger alert with a banner and
a marker in the recording. It turns OFF once the temperature is back at
105 or below.

Check it with no car:

```
havne-cli conditions check daily.json --profile obd2
```

## The file

| Key | | |
|---|---|---|
| `havne_conditions` | required, first | The format's version: `1`. |
| `name` | optional | The file's name, 1 to 63 bytes. |
| `checks` | optional | Thresholds for the file's check terms, below. |
| `conditions` | required | A list of 0 to 64 conditions. |

`notes` (a string) is allowed in every object, where a comment would
go, and a key starting `x-` is a tool's own: the loader skips both. Any
other key the loader doesn't know refuses the file, a misspelled one
included.

## A condition

| Key | | |
|---|---|---|
| `key` | required | The stable name: 1 to 31 of `a`-`z`, `0`-`9` and `_`, once in the file. `havne-cli`'s lines and a recording's lines name a condition by it. |
| `name` | required | The name shown, and its marker's label: 1 to 40 bytes, once in the file, not starting with `=`, `+`, `-`, `@`, a tab or a CR. |
| `enabled` | optional | `false` keeps the condition in the file and judges nothing. |
| `when` | required | One term, or `all` or `any` over 1 to 4 terms. |
| `for` | optional | The terms stay true this many seconds before the condition turns ON: 0 to 600, 0 when left out. |
| `clear_for` | optional | They stay false this many seconds before it turns OFF: 0 to 600. |
| `alert`, `record`, `mark` | optional | Its uses, below. A condition with none is still judged and reported. |

```json
"when": { "all": [
  { "signal": ["obd2.throttle"], "unit": "%", "above": 80, "clear": 70 },
  { "signal": ["id:1001", "id:7003"], "unit": "RPM", "above": 2500 }
] }
```

`all` is true when every term is, `any` when one is. Terms join one
level deep: a term holds no `all` or `any` of its own. A level that needs
arithmetic (the larger of two signals, a margin that depends on engine
speed) is a computed signal in a signal file
([signal-files.md](signal-files.md)), which a term then reads by its
key.

## Terms

A term tests one input, and is exactly one of three kinds.

### A signal

| Key | | |
|---|---|---|
| `signal` | required | 1 to 8 references, each signal once. The first one the session's signals provide is read. |
| `above`, `below`, `equals`, `differs` | one of them | The comparison and its level, a number in the signal's own unit. |
| `unit` | with `above` and `below` | The unit the level is in, 0 to 15 bytes. |
| `clear` | optional, with `above` and `below` | Hysteresis: once true, the term turns false only at or past this level. |
| `max_age` | optional | A sample older than this many seconds can't be judged: 0.1 to 60, 3 when left out. |

**A reference** names a signal in one of three forms:

- `id:<n>`: a built-in set's signal by its ID (`id:7003` is `obd2`'s
  engine speed, `id:1001` the demo's). A signal file that extends `obd2`
  keeps `obd2`'s IDs. An ID from 20000 up names nothing: from there IDs
  are numbered by order (`uds-generic`'s follow the URI's DIDs, a signal
  file's own follow its list), so they aren't stable names. A file's own
  signal goes by `key:`.
- `key:<key>`: a signal file's own signal, by its `key`
  (`key:trans_temp`).
- `obd2.<name>`: one of `obd2`'s signals by the name a computed
  expression reads it by (`obd2.coolant`, `obd2.pid_0b`). It is read as
  that signal's `id:`.

A list such as `["id:1001", "id:7003"]` lets one condition serve the
demo and a car. Any other form is refused.

**The unit is held to the signal.** A term is judged only on a signal
whose unit is exactly the term's, byte for byte, so a level written in
kPa is never compared with a value in psi. `above` and `below` need the
`unit` key; write `""` for a number that has no unit. `equals` and
`differs` may leave it out, which reads as `""`: `equals` on a signal in
RPM names `"RPM"` too.

**`clear`** lies on the off side of its level: at or below it for
`above`, at or above it for `below`. Without it the level alone decides,
and a value sitting at the level turns the term on and off with every
sample.

**What a term can read:** a number, a computed signal, or an
enumeration (with `equals` and `differs` only, on its raw number). Text
and hex signals have nothing to compare.

### The link

```json
{ "link": "not_ok" }
```

True while the session's link is in the state named: `not_ok` (any
state but OK), `timeout`, `request_failed` or `hard_failure`.

### A check

```json
{ "check": "rate_drop" }
```

True while a finding of that kind is under way on the live frames: one
of the frames check's rules, `gap`, `rate_drop`, `late_id`, `overflow`
or `isotp`. The file's `checks` sets their thresholds once for every
check term:

| Key | | Default |
|---|---|---|
| `gap` | The whole bus quiet for longer than this many seconds: above 0, up to 3600. | 1 |
| `rate_drop` | An ID's interval past this multiple of its usual one: above 1, up to 1000, read to hundredths. | 10 |
| `settle` | An ID first heard more than this many seconds after the first frame: above 0, up to 3600. | 2 |

## Uses

- **`alert`**: `{ "level": "warning" }` or `"danger"`, required, with
  `sound` and `banner` (`true` or `false`, `false` when left out).
- **`record`**: `{ "before": 5, "after": 5 }`: seconds kept from before
  a firing (0 to 60) and written after the condition turns OFF (0 to
  600). Both are 0 when left out, so `{}` records while ON.
- **`mark`**: `true` writes a marker with the condition's name at each
  firing.

## Bounds

| | |
|---|---|
| File size | 256 KiB |
| Conditions | 64 |
| Terms in a condition | 4 |
| References in a term | 8 |
| `key` | 31 bytes |
| A condition's `name` | 40 bytes |
| `for`, `clear_for`, `after` | 0 to 600 s |
| `before` | 0 to 60 s |
| `max_age` | 0.1 to 60 s |

Times are JSON numbers in seconds and are kept to the microsecond.

## What is refused at load

Everything above is checked when the file is read, and a refusal names
the line, the column and the reason, never the file's path:

```
line 10, column 17: unknown key 'abov' in condition 'pull''s when
```

Besides JSON's own rules (a duplicate key, a trailing comma, a comment
and a byte order mark are refused), the loader refuses: a file that
doesn't start with `havne_conditions`, or names another format; a key it
doesn't know; a value of the wrong type or out of its bounds; a `key` or
a `name` given twice; a term that is no kind or two; a signal term with
no comparison or two; `above` or `below` with no `unit`; a `clear` with
`equals` or `differs`, or on the wrong side of its level; a reference of
another form, an `id:` past 65535, an `obd2.<name>` that names none of
`obd2`'s signals, or one signal twice in a list; an `alert` with no
`level`.

## What only a set of signals settles

Some things depend on the signals a session has, so they don't refuse
the file. They make a condition **unavailable** there: it is kept,
never judged, and reported with its reason.

| Reason | |
|---|---|
| `disabled` | `"enabled": false`. |
| `no_signal` | None of a term's references names a signal of the set. |
| `unit` | The signal's unit isn't the term's. |
| `not_a_number` | The signal is text or hex. |
| `enum_order` | `above` or `below` on an enumeration. |

A term takes the first of its references the set provides and looks no
further, so a list is an order of preference, not a search for one that
fits. A condition with one unavailable term is unavailable as a whole,
under `any` too.

## Checking one with no car

```
havne-cli conditions check <file> [--signals <file> | --profile <name>]
```

It writes JSON lines: `start`, a `condition_set` line (the file's name,
its count, and the check thresholds in force), then a `condition_def`
line for each condition as read. A refused file is an `error` line with
the loader's words and exit code 3.

With `--profile` (a registered profile: `obd2`, `mock`) or `--signals`
(a signal file), each condition is also resolved against that set: a
signal term gains `resolved`, the reference it took and the signal's ID,
name and unit there, and the line gains `available`, `why` and
`why_term`.

```json
{"type":"condition_def","key":"wrong_unit","name":"A level in another unit","enabled":true,"join":"all","terms":[{"kind":"signal","signal":["key:trans_temp"],"unit":"°F","test":"above","level":230,"clear":null,"max_age_us":null,"resolved":{"reference":"key:trans_temp","id":30003,"name":"Trans Temp","unit":"°C"}}],"for_us":0,"clear_for_us":0,"alert":null,"record":null,"mark":false,"available":false,"why":"unit","why_term":0}
```

From Python:

```python
import havne

conditions = havne.ConditionSet(path="daily.json")
for condition, binding in zip(conditions.conditions, conditions.resolve("obd2")):
    print(condition.key, binding.available, binding.why)
```

`resolve` takes a profile's name, a `havne.SignalFile`, or nothing.

## The schema

`schema/condition-file.schema.json` is a JSON Schema for the format, for
an editor's completion and checks as you type. The loader is the truth:
a few rules (a name given twice, a reference that names none of `obd2`'s
signals, a `clear` on the wrong side) are the loader's alone, and the
schema's description lists them. The loader refuses a `$schema` key, as
it refuses every key it doesn't know, so map the schema to your files in
the editor's settings.
