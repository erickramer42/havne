# ADR-0062: Computed signals in format 2: functions, conditionals and chains

- **Status:** Accepted; built in v0.8.0.
- **Date:** 2026-11-10.
- **Builds on:** [ADR-0051](0051-signal-files.md) (signal files, the
  computed stage, the bounded program, the loader's strict rules);
  [ADR-0016](0016-signal-definition-layout.md) (the set's layout and its
  version); [ADR-0024](0024-decode-on-the-worker.md) (decoding on the
  worker, no allocation per frame); [ADR-0048](0048-semantic-versioning.md)
  (what a minor version may change); [ADR-0049](0049-recordings-in-core.md)
  (the reader decodes a recording's frames through the session's
  decoder); [ADR-0052](0052-plugin-framework.md) (a plugin's data files
  are signal files); [ADR-0056](0056-analysis-log.md) (the focus);
  [ADR-0031](0031-command-line-client.md) and
  [ADR-0053](0053-python-bindings.md) (`havne-cli` and Python).
- **Amends, each by a dated note:** ADR-0051 sections 2 and 5 and its
  "linear sum" alternative (format 2, a computed input, a value that
  can't be worked out); ADR-0052 section 3 (a data file's format);
  ADR-0031 section 3 (schema 1 grows by keys); ADR-0053 section 5
  (`SignalFile.format`, `SignalFile.expr`, `Plugin.expr`).
- **Leads to:** [ADR-0063](0063-conditions.md), whose terms read these
  signals, and custom derived channels.

## Context

Format 1's computed signals are bounded arithmetic: `+ - * /` and
parentheses over at most 4 inputs, each a signal a reply carries, run by
a stack machine with no state (ADR-0051 sections 2 and 5). That covers
boost and a ratio. It leaves out:

- **The larger, the smaller, the distance:** the hottest of four exhaust
  temperatures, the lower of two wheel speeds, how far total fuel trim
  sits from zero.
- **A choice:** a quantity guarded against a zero divisor (air per
  revolution at 0 RPM), a level that depends on another signal.
- **A quantity built in steps:** a pressure ratio from boost, a margin
  from a computed load. Format 1 refuses a computed signal as an input.

Three users wait on it: conditions (ADR-0063), whose terms compare a
signal with a level and keep arithmetic out of the condition file;
custom derived channels, a user's own expression typed into the app;
and the signal files users write by hand.

Forces:

- **One evaluator.** The session's worker and the recording's reader
  decode through the same code (ADR-0049), so a value shown live, a
  value read back and a replay agree. A program with state of its own
  would need that state saved wherever the reader keeps a mark.
- **Nothing per frame.** The worker allocates nothing, and a program's
  cost is bounded at load (ADR-0024).
- **A set is public.** `ecuio_signal_set_t` carries its programs as
  `ecuio_computed_t` (208 bytes, fixed counts), a set built outside core
  registers through `ecuio_profile_register`, and the reader and the app
  copy a set's tables by value. The validator is the only gate between
  such a set and the worker.
- **Every existing file loads unchanged,** to the same programs, and an
  older Havne refuses a new file by saying so, not by guessing at a
  typing error.
- **Honest values.** A value that can't be worked out is no value
  (ADR-0051 section 5); an old input is never combined silently.
- **The same bits everywhere.** A recording holds what was computed, and
  a replay on another platform computes it again.

## Decision

In brief: the expression gains function calls, comparisons and one
conditional, checked for two types at load. A step that can't be worked
out gives no value, which passes through every later step except the
branch an `if` doesn't take. A computed signal may read another; the
decoder works a chain out from the signals a reply carries each time,
so nothing is stored and no age compounds. The program keeps its 208
bytes and gains step values. Files that use any of this say
`"havne_signals": 2`.

### 1. The grammar

```json
{
  "havne_signals": 2,
  "name": "Daily driver extras",
  "source": { "name": "Written by hand for the daily driver" },
  "extends": "obd2",
  "signals": [
    { "key": "boost", "name": "Boost", "kind": "computed",
      "expr": "obd2.map - obd2.baro",
      "max_age": { "obd2.baro": 30 }, "unit": "kPa" },
    { "key": "pressure_ratio", "name": "Pressure Ratio", "kind": "computed",
      "expr": "(boost + obd2.baro) / obd2.baro",
      "max_age": { "obd2.baro": 30 }, "decimals": 2 },
    { "key": "trim_total", "name": "Fuel trim, total", "kind": "computed",
      "expr": "abs(obd2.pid_06 + obd2.pid_07)", "unit": "%", "decimals": 1 },
    { "key": "air_per_rev", "name": "Air per revolution", "kind": "computed",
      "expr": "if(obd2.rpm > 0, obd2.maf * 60 / obd2.rpm, 0)",
      "unit": "g/rev", "decimals": 2 }
  ]
}
```

Format 1's expression stays as it is: `+ - * /`, the usual precedence,
parentheses, a unary minus, decimal constants, signals by key. Format 2
adds:

| Form | Takes | Gives |
|---|---|---|
| `min(a, b, ...)`, `max(a, b, ...)` | two or more numbers | the smallest, the largest |
| `abs(a)` | a number | its distance from zero |
| `clamp(a, low, high)` | three numbers | `a` held within `low` and `high` |
| `sqrt(a)` | a number | its square root; no value below zero |
| `round(a)`, `floor(a)`, `ceil(a)` | a number | the nearest whole number (a half goes away from zero), the one below, the one above |
| `a < b`, `a <= b`, `a > b`, `a >= b`, `a == b`, `a != b` | two numbers | a truth |
| `all(p, q, ...)`, `any(p, q, ...)` | two or more truths | a truth |
| `not(p)` | a truth | a truth |
| `if(p, a, b)` | a truth and two numbers | `a` when `p` holds, else `b` |
| a computed signal's key | | its value at this frame (section 4) |

- **Everything new is a call or a comparison.** A call is a name
  followed by `(`, with any spaces between (`max (a, b)` is a call). A
  format 1 file refuses a name followed by `(`, so a signal keyed `min`
  or `if` still reads as a signal where no `(` follows, no word is
  reserved, and no format 1 expression means something else in format 2.
- **A comparison binds looser than `+` and `-`** and takes no neighbor:
  `a < b < c` is refused, as is `a = b`.
- **`==` and `!=` compare exactly,** which suits an enumeration's raw
  number (`gear == 3`) and whole numbers. A level on a measured value is
  `>` or `<`. -0 equals +0.
- **`clamp`** is `min(max(a, low), high)`. Two constants with `low`
  above `high` are refused at load; a bound that reads a signal isn't
  judged.
- **`round`** takes a half away from zero (C's `round`, never
  `nearbyint` under the current rounding mode), so 2.5 is 3 and -2.5 is
  -3, whatever the rounding mode.
- **The functions are the ones IEEE 754 defines to the last bit:** each
  is exact or correctly rounded on every platform, so the same inputs
  give the same bytes in a recording made anywhere. `min` and `max` are
  a comparison in core (`b < a ? b : a`), never the C library's `fmin`
  and `fmax`, whose answer for -0 against +0 is the library's own; of
  -0 and +0 the first part stays. `pow`, `exp`, `log` and the
  trigonometric functions are a library's approximation and differ in
  the last digit between platforms; they are left out (section 9).

### 2. Two types, checked at load

An expression's parts are numbers or truths. A comparison takes numbers
and gives a truth; `all`, `any` and `not` take truths; `if` takes a
truth first; every other operator and function takes numbers. A signal
is a number, an enumeration its raw number. The whole expression is a
number.

The loader checks this as it compiles, so a mistake is refused on the
desk with its place. Every truth reads as "a comparison" in the words,
whether it is a comparison, `all()`, `any()` or `not()`:

```
line 14, column 15: expr 'if(obd2.rpm, 1, 0)' gives if() a number where a comparison should be (at character 4)
line 14, column 15: expr 'obd2.rpm > 3000' is a comparison where a number should be: if(obd2.rpm > 3000, 1, 0) makes one (at character 1)
line 14, column 15: expr '(a > b) + 1' adds a comparison where a number should be (at character 2)
```

The fix in the second line shows the expression when it is 32
characters or fewer, and `...` otherwise. A truth has no meaning as a
number, so `rpm > 3000` never silently becomes 1 in a sum, and a signal
that wants 0 and 1 says so with `if`.

### 3. A value that can't be worked out

One rule covers a zero divisor, an old input and an input never seen:

- **An input** is its newest value when that value is within the
  input's age bound at the frame's time; otherwise it has no value. The
  decoder hands such an input to the program as no value and runs the
  program on every frame that feeds it, so the program's own rule
  decides what a missing input does.
- **An input is a value unless it has none.** An infinite input (a
  `FLOAT` field's bytes can decode as one) is a value: `min(inf, 2)` is
  2, `inf > 0` holds, and format 1's `1 / x` at an infinite x gives 0,
  as it always has.
- **A step with no value in gives no value out,** as does a number step
  whose result isn't finite (a division by zero, an overflow, `sqrt`
  below zero). A comparison with either side missing has no value. A
  program that ends without a finite number gives none.
- **`if` ignores the branch it doesn't take.** `if(p, a, b)` has no
  value when `p` has none, or when the branch `p` picks has none. The
  other branch's value, or lack of one, changes nothing.
- **`all` and `any` join as a condition's terms do:** `all` is false
  when any part is false, and has no value when none is false and one
  has none; `any` is true when any part is true, and has no value when
  none is true and one has none.
- **A signal whose expression ends with no value gives none for that
  frame,** as in format 1: it is absent from the batch, and the stale
  age dims it like any signal that stopped arriving.

So `if(obd2.rpm > 0, obd2.maf * 60 / obd2.rpm, 0)` reads 0 at 0 RPM,
where the division alone has no value, and
`if(gear == 0, 0, obd2.speed / gear_ratio)` keeps working while the
branch it doesn't take reads a signal that has gone quiet.

A format 1 program has no `if`, `all` or `any`, so for it the rule is
ADR-0051's: any input or step without a value leaves the signal without
one, and every format 1 program gives the bits it gave before.

There is no jump in a program. Every step runs once, in order, and the
conditional is a step that picks one of two values already worked out.
A program's cost is its step count, known at load.

### 4. A computed signal reading another

An expression names a computed signal by its key, as it names any
signal of the file, listed before or after it.

- **Worked out from the leaves, each time.** A leaf is a signal a reply
  carries. When a frame arrives, each computed signal it feeds is worked
  out from the leaves' newest values, through every computed signal on
  the way, at that frame's time. A computed signal stores no value of
  its own between frames.
- **What "feeds" means:** a frame feeds a computed signal when it
  decodes a leaf the signal reads, directly or through a chain. Each fed
  signal that has a value joins the frame's batch once, after the
  frame's own values, stamped with the frame's time, in the set's order
  whatever order the chain was worked out in.
- **What one reads of another:** while the leaves under it are within
  their bounds, the value a computed input gives is the one that signal
  last showed, since nothing under it has changed. Once a leaf passes
  its bound, the input has no value, and the reader's program decides
  what that does (section 3).
- **No circle.** A signal that reads itself, directly or through others,
  is refused at load with the circle named:
  `signal 'a' reads 'b', which reads 'a': computed signals can't read in
  a circle`.
- **At most 8 deep** (`ECUIO_COMPUTED_MAX_DEPTH`): a computed signal that
  reads only leaves is 1 deep, and one that reads it is 2. The bound
  keeps every walk of a chain (the validator's, the decoder's plan, the
  focus's walk to the leaves) a fixed number of passes with no
  recursion.
- **How many:** a file's 1024 signals, as before. A chain adds no limit
  of its own. A chain is also how a quantity takes more than 4 inputs:
  an expression still reads at most 4 signals, and a computed input
  counts as one.

**Through a replacement.** A file's signal named as a base def replaces
it and keeps its ID (ADR-0051's appendix "boost in the obd2 set"), so a
chain can pass through a replacement: the file's own signal reading it
by key, a file's `obd2.<key>`, or a base program (obd2's Boost reads MAP
and baro). The loader's chain check finds each input by its ID, a
replacement's included, so a circle or a depth past 8 through one is
refused in the file's words. A computed replacement is read only in
format 2, judged by the reader's file (a base program's by the
replacement's file), and never through an age bound, since a computed
input's ages are its own inputs'. A base program that a replacement puts
past 8 deep is refused at the replacement. A plugin variant's format 1
file may replace a leaf with a computed signal that a format 2 file's
chain reads: the reader's format is the one judged.

The example `examples/boost-ratio.json` is such a chain: its Boost
replaces obd2's at 7011, and its Pressure Ratio reads it.

**What follows a chain to its leaves.** A computed def asks nothing, so
whatever judges a signal by its requests walks down to the leaves:

- **The focus** (ADR-0056 section 2) keeps the leaves under a picked
  computed signal asked, through its whole chain, in at most 8 passes
  with no allocation. The recorder's `focus` line still names what was
  picked.
- **The unsupported and absent lists** (ADR-0039 section 6) list a
  computed signal when an input under it is, through the chain.

### 5. Ages through a chain

- **A leaf's bound is stated where the leaf is read.** `max_age` names
  the leaves an expression reads; one it doesn't name takes the stale
  age (`ECUIO_VALUE_STALE_US`), as in format 1.
- **A computed input takes no `max_age`.** It has no age of its own: its
  leaves carry their own bounds, each stated by the `max_age` of the
  signal that reads the leaf. A `max_age`
  naming one is refused:
  `signal 'pressure_ratio''s max_age names 'boost', a computed signal:
  its own inputs carry their ages`. In a set built outside core, an age
  bound on a computed input is the validator's
  `ECUIO_VALIDATE_ERR_COMPUTED_INPUT`.
- **Nothing compounds.** In the example, `pressure_ratio` reads baro
  twice: through `boost`, under boost's 30 s, and directly, under its
  own 30 s. Each reading is judged against the frame's time. A chain 8
  deep combines no leaf older than the bound written beside it.
- **A computed value's time is the frame that fed it.** That is the
  time a recording, a gauge's dimming and a condition's term see.

The alternative, a computed signal stored with the time it was last
worked out and aged like a leaf, lets a leaf's age add up level by level
(a 3 s bound at each of three levels passes a leaf 9 s old) and puts
state in the decoder that the reader's marks would have to save.

### 6. The program in core

`ecuio_computed_t` keeps its size, its offsets and its counts: 208
bytes, 4 inputs, 16 constants, 33 steps. `ECUIO_SIGNAL_SET_LAYOUT` stays
4.

- **Inputs that never meet within a bound give no value.** A computed
  signal is worked out on a frame that feeds one input, from the
  others' newest values, each within its bound. Two inputs asked once a
  sweep in a sweep longer than twice the bound (about 6 s at the 3 s
  default: a large set asked whole at the cap of 1024 requests) never
  arrive within 3 s of each other, so the signal never has a value and
  reads "waiting for data", in none of the unsupported, absent or
  unasked lists. A `max_age` on the slower input (up to 60 s), a focus
  that asks fewer, or a default pick fixes it.
- **New step values, appended:**

  | Step | Value | Takes | Gives |
  |---|---|---|---|
  | `ECUIO_COMPUTED_MIN`, `_MAX` | 0x24, 0x25 | 2 numbers | a number |
  | `ECUIO_COMPUTED_ABS`, `_SQRT`, `_ROUND`, `_FLOOR`, `_CEIL` | 0x26 to 0x2A | 1 number | a number |
  | `ECUIO_COMPUTED_LESS`, `_LESS_EQUAL`, `_GREATER`, `_GREATER_EQUAL`, `_EQUAL`, `_NOT_EQUAL` | 0x30 to 0x35 | 2 numbers | a truth |
  | `ECUIO_COMPUTED_ALL`, `_ANY` | 0x38, 0x39 | 2 truths | a truth |
  | `ECUIO_COMPUTED_NOT` | 0x3A | 1 truth | a truth |
  | `ECUIO_COMPUTED_IF` | 0x3B | a truth and 2 numbers | a number |

  `min`, `max`, `all` and `any` over more than two parts emit their step
  after each part past the first, and `clamp` is a `MAX` then a `MIN`,
  so a long call is a run of two-value steps.
- **The checker** (`ecuio_computed_check`, which the validator runs on
  every program of every set) tracks each stack slot's type and refuses
  a step given the wrong type or too few values, a truth left at the
  end, and any step value not listed. So a set built outside core is
  held to the rules a file is.
- **The machine** keeps each slot's value and whether it has one; a
  truth is 1 or 0. The stack has a slot for each step (33), and every
  pop is bounds-checked, so a program the checker never saw gives no
  value instead of reaching past the stack. A checked program reaches 17
  values at most, as in format 1: an `if` needs a truth under its two
  numbers, and a truth costs three steps (two pushes and a comparison)
  for one slot.
- **An input's id may be a computed def's.** The validator's rules for
  it: its age bound is 0, and the defs' reads form no circle and no
  chain past `ECUIO_COMPUTED_MAX_DEPTH`. It settles each program's depth
  (1 more than its deepest computed input) in at most 8 passes over the
  programs, with no recursion. A program it can't settle (on a circle,
  reading one, or past the depth) is `ECUIO_VALIDATE_ERR_COMPUTED_CHAIN`
  (-23) at the first such program in the table, so a circle in a program
  no def reads refuses the set too. The depths take one byte for each
  program, allocated only for a set where some computed input is
  computed; when that byte array can't be had, the code is
  `ECUIO_VALIDATE_ERR_OUT_OF_MEMORY` (-24) with the index `UINT32_MAX`,
  which `ecuio_profile_register` and `ecuio_recording_open_frames`
  return as `ECUIO_ERR_OUT_OF_MEMORY` and the loader as out of memory.
  A set with no chain allocates nothing to validate, as before.
- **The decoder's plan,** built once at prepare, when the set has a
  computed def: each computed def's program, and for each of its inputs
  the def it reads and its bound, and, when that def is computed, which
  plan it is. A set where no input is computed (every set built before
  format 2) is unchained and decodes in the single pass it always took.
  A chained set's plans are given their depths in at most 8 passes and
  listed by depth, shallow first, keeping the set's order within a
  depth. Each frame then takes three passes over the plans, allocating
  nothing:
  1. **Fed,** shallow to deep: a plan is fed when a leaf input was
     decoded from this frame or a computed input is fed.
  2. **Needed,** deep to shallow: a fed plan is needed, and so is every
     computed input of a needed plan. A plan no one needs this frame
     costs nothing more.
  3. **Worked out,** shallow to deep, needed plans only: each leaf input
     its newest value within its bound at the frame's time, each
     computed input the value just worked out for it this frame, or no
     value.

  Each fed plan with a value then joins the batch in the set's order.
  The state byte and value for each plan are sized at prepare. A plan
  whose depth never settles (possible only in a set the validator never
  passed) is never worked out or sent, so such a set can't hang the
  decoder.
- **The reader's marks** save the leaves' newest values (ADR-0049's
  frames path). A computed signal has no state, so a read that starts
  at a mark gives a chain the values the session gave, and a mark keeps
  no computed input's value.

What the C ABI and `python/havne/_ecuio.py` see, all of it additive but
one constant:

| Change | ABI | Notes |
|---|---|---|
| The step values above, `ECUIO_COMPUTED_MAX_DEPTH` (8), `ECUIO_VALIDATE_ERR_COMPUTED_CHAIN` (-23), `ECUIO_VALIDATE_ERR_OUT_OF_MEMORY` (-24) | Additive | Step values and validator codes are append-only lists |
| The validator accepts a computed input and the new steps | Behavior | Every set that passed still passes and computes the same values; some sets it refused pass |
| `ECUIO_SIGNAL_FILE_FORMAT` 1 to 2, beside `ECUIO_SIGNAL_FILE_FORMAT_OLDEST` (1) | Constant change | The newest format this core reads; one line in `abi/api_changes.txt` |
| `ecuio_signal_file_format(file)` | Additive | 1 or 2, as the file says; a plugin variant's the newest among its files; 0 for NULL |
| `ecuio_signal_file_expr(file, def)` | Additive | A computed def's expression as written, borrowed until the file is freed; "" for any other def and for NULLs. Finds the def by address, as `ecuio_signal_file_key` does |
| `ecuio_computed_t`, `ecuio_signal_set_t`, `ecuio_signal_def_t` | None | The static asserts in the header and `test_c_abi.c` stand as they are |

The Python package builds no program and never reads one; its generated
declarations gain the constants and the two functions.

### 7. Format 2

- **`"havne_signals": 2`** marks a file that may use section 1's
  additions and a computed input. Nothing else differs from format 1:
  the keys, the kinds, the bounds and every other rule are the same.
- **This core reads formats 1 and 2.** Every format 1 file loads as it
  did, to the same programs byte for byte, and a format 1 expression
  compiles byte for byte the same under format 2.
- **A format 1 file is held to format 1.** A call, a comparison or a
  computed input in one is refused, the words naming the fix:
  `line 21, column 15: expr 'max(a, b)' uses max(), which needs
  "havne_signals": 2 (at character 1)`, and `reads 'e', a computed
  signal, which needs "havne_signals": 2`. So a file marked 1 is one
  every Havne since v0.5.0 reads as this one does (its `gauge` key
  aside, ADR-0051's appendix).
- **One check of the format** serves the signal file and a plugin's data
  file: the first key, then a whole number from 1 to 2. A later format
  is refused as `"havne_signals" is format 3, and this Havne reads
  formats 1 and 2` (`format ?` for a number that isn't whole).
- **An older Havne refuses a format 2 file in its own words:**
  `"havne_signals" is format 2, and this Havne reads format 1`, the line
  its loader has always had. Without the number it would read
  `max(a, b)` as a signal named `max` and say that no signal is.
- **Which to write:** 1 unless the file uses something of format 2, so
  the most versions read it. The OBDb import writes 1.
- **A plugin's data files** (ADR-0052) take either format, and a
  variant's files may mix them. Each signal compiles at its own file's
  format (a replacing signal at its file's), and the variant's format,
  and that of the merged copy a recording keeps, is the newest among its
  files. The copy reads back to the same set, since a format 1
  expression compiles the same under format 2. A format 1 data file
  using a call is refused naming its file, whatever its variant's other
  files say. A plugin that ships a format 2 file names a release that
  reads it in `requires.core`, so an older core gives the version, not
  a refused data file; `plugins check` warns when it doesn't, as it does
  for the `gauge` key.
- **A recording** copies its signal file whole, so a format 2 file's
  recording holds a format 2 `signals.json`. An older Havne reads that
  recording's values and markers as any; its frames path and its
  scrubbed export, which load the copy, refuse with the loader's words.
  The recording's own format number doesn't move.
- **The JSON Schema** takes 1 or 2 in a signal file and a plugin's data
  file, and widens the expression's characters by `, < > = !`. Which
  format a form needs stays the loader's to check, with the rest of the
  grammar.

### 8. Limits and refusals

| Limit | Bound | Refused as |
|---|---|---|
| An expression's text | 256 characters | `is longer than 256 characters` |
| Signals read | 4, leaves and computed together | `reads more than 4 signals` |
| Constants | 16 distinct | `has more than 16 constants` |
| Steps | 33: each value read and each operation is one | `is longer than a program's 33 steps` (format 1 keeps `has more than 16 operations`, the same bound) |
| Nesting | 16 levels of parentheses, calls and unary minus signs | `nests deeper than 16` |
| A chain | 8 deep | `signal 'e8' sits 9 computed signals deep: a chain goes 8 deep at most` |

A refusal of the expression's own grammar is `line L, column C: expr
'...' ` and why, with `(at character N)` for the place inside the
expression. A refusal about signals together (a circle, a depth, a
`max_age`, a replacement) is `line L, column C: ` and why, at the
signal's `expr` or `max_age`. A plugin's data file puts its file's name
first (`b.json, line 1, column 2: ...`). The value at fault is named,
the file's path never. Beyond the limits and sections 2, 4, 5 and 7:

- a call's count of parts (a call that takes a truth counts "parts",
  one of numbers alone "values"): `gives max() 1 value where it takes 2
  or more`, `gives if() 2 parts where it takes 3`;
- a name before `(` that is no function: `calls pow(), which isn't a
  function: min, max, abs, clamp, sqrt, round, floor, ceil, if, all, any
  or not`; in a format 1 file, `calls pow(), which isn't a function, and a format 1 file calls none (functions need "havne_signals": 2)`;
- `clamp` with constant bounds crossed: `gives clamp() a low of 9 above
  its high of 1`;
- `=` alone: `has '=' where '==' should be`; a comparison beside a
  comparison: `has '<' beside a comparison: all() or any() joins two`;
  a `,` outside a call: `has ',' outside a call` (format 1 keeps its
  general words);
- an unclosed call: `has no ')' to close its max(`;
- everything format 1 refuses, in format 1's words.

The loader checks chains before the validator does, so its refusals
name a signal at its `expr`: a circle by the shortest way back,
reported at the first signal of the circle in the file's order (`signal
'e' reads 'e': ...` for one that reads itself); a chain too deep at the
first signal in the file's order that sits past 8, naming its own
depth. A plugin variant's later file that closes a
circle through an earlier file's chain is refused in the later file's
place.

### 9. Front ends

The grammar reaches every front end through the one loader, so a file
that loads in one loads in all, and a value is worked out in core.

- **`havne-cli` (schema 1 grows by keys, none changed):**
  `signals check`'s `signal_file` line gains `format`; a computed
  signal's `signal_def` line gains `expr` before `inputs`, and an input
  that is itself computed is `{"id":N,"max_age_us":null,"computed":true}`,
  since no bound of its own applies. `plugins check` writes the same
  lines, and writes one `requires_core` line for each feature that needs
  a newer core than the manifest names (the gauge 0.7.2, format 2
  0.8.0). A refused file is one `error` line with the loader's words and
  exit 3, as before. `stream`, `log`, `sim` and `view` change nothing: a
  chain's values are values.
- **Python:** `SignalFile.format`, `SignalFile.expr(key)`,
  `Plugin.expr(key, variant=None)` and `Plugin.format(variant=None)`, each expression as written (None for
  a signal that isn't computed, or no such signal). A refusal raises
  with the loader's words, as before.
- **The app:** a format 2 file imports as any, and a refusal shows the
  loader's words. Nothing new is drawn.
- **The example:** `examples/boost-ratio.json`, a format 2 file that
  extends `obd2` with a chain over MAP and baro, checked by the schema,
  by a `signals check` golden and by a `stream` golden over a
  hand-written replay of MAP and baro replies
  (`tests/fixtures/replay/boost-ratio`). The mock answers no PIDs, so
  the mock set gains no format 2 signal and its goldens stay.

**Left out, on purpose:**

- **Functions with a past** (a rate of change, an average over time, a
  running total, the previous value). They need state in the decoder and
  in every mark the reader keeps, and rules for a gap in the inputs.
  They are the next format's, rate of change and smoothing first.
- **`pow`, `exp`, `log`, trigonometry:** not the same bits on every
  platform. A whole power is a product (`v * v * v`).
- **A stand-in for a missing input** (`either(a, b)`): a signal whose
  source changes without a word.
- **A signal that is a truth, or text.** A truth becomes a number with
  `if(p, 1, 0)`.
- **More than 4 signals in one expression.** A chain carries more.

### 10. Steps

1. The machine: the step values, the no-value rule, the typed checker,
   and the decoder handing a stale input to the program as no value.
2. The compiler and format 2: calls, comparisons, types and their
   refusals; the format's one check in the loader and in a plugin's data
   files; the two accessors.
3. Chains: the validator's rule and codes, the loader's refusals, the
   decoder's plan and passes, the reader's marks, the focus's walk to
   the leaves.
4. The front ends: the schema, `havne-cli`'s keys, Python's, the app's
   import, the example.

A chain through a replacement (section 4) followed step 3, once `obd2`
carried its own Boost.

## Tests

- **The machine (`ComputedTest`):** each step's result and operand
  order; `min` and `max` on -0 and +0; `round` in all four rounding
  modes; each comparison below, at and above, with either side missing;
  the no-value rule step by step, with the branch not taken holding a
  zero divisor, an old input, an input never seen and `sqrt` below
  zero; `all` and `any` over every pair of true, false and none; an
  infinite input; the checker taking each listed step with its types,
  refusing each wrong type, a truth at the end and every unlisted value;
  the deepest stack a program reaches. `verify.sh` runs it on the x86
  build too.
- **The same bits:** a table of programs, each labeled with the
  expression it stands for, their inputs and each result's bytes
  (`ComputedTest.TheSameBitsOnEveryPlatform`), run by every compiler CI
  builds with and by the cross-arch build.
- **The compiler (`SignalFileTest`):** each function and comparison;
  precedence; a key named as a function, with and without `(`; every
  refusal of sections 2, 4, 5, 7 and 8 with its place; each format 2
  form refused in a format 1 file; format 1's programs byte for byte
  under both formats, by hashes taken from the format 1 compiler; seeded
  random expression trees compiled and run against a walk of the tree
  written in the test, inputs with and without values.
- **Chains:** the validator on a chain 8 deep listed either way and 9
  refused, a circle, an age bound on a computed input, and running out
  of memory (`SignalTablesTest`, `CoreTest.NoRoomToCheckAChainIsOutOfMemory`);
  a chain joining the batch in the set's order, a chain fed through one
  input working the other out at its frame, no value once a leaf passes
  its bound and none later, and the deepest chain allocating nothing
  (`CoreTest`); a chain through a replacement, its circle and its depth
  (`SignalFileTest`, `PluginTest`).
- **One evaluator:** a recording made under a chain profile read through
  `signals.csv` and through its frames, every summary and column alike
  (`RecordingFramesTest.AChainsRecordingDecodesFromItsFramesAlike`).
- **The focus:** a focus on a computed signal two deep keeps exactly its
  leaves asked (`CoreTest.AFocusOnAComputedSignalKeepsItsLeavesAsked`).
- **Fuzzing:** `fuzz_computed` (arbitrary programs and inputs: the
  checker never faults, a checked program stays on its stack and gives
  no value or a finite one, format 1's steps alone give what a format 1
  machine kept in the target gives); `fuzz_signal_file` (a loaded set
  passes the validator, a format 1 file reads no computed input, a
  decoded set's batch checked against a reference evaluator over core's
  machine); `fuzz_validate_set`'s chain mode (arbitrary input ids: the
  code and index rules, and a passed set decoded by two decoders and a
  reference evaluator, alike). A chain seed in `seeds.py` lets
  `fuzz_signal_file` reach a chain's order.
- **The front ends:** `havne-cli` goldens for `signals check` on a chain,
  the example, a call refused in a format 1 file, and `plugins check` on
  a format 2 plugin under an older `requires.core`; a `stream` golden of
  the example; `SchemaFixturesTest`'s format 2 cases, checked by the
  schema and `havne-cli` alike; `PythonTest.test_signal_file` and
  `test_plugin`; `BridgeTest.AFormat2SignalFileImportsAndIsUsedAsAny`.

## Alternatives considered

- **C's conditional (`p ? a : b`) or Python's (`a if p else b`).** A
  second precedence rule or a second word order for what a call says,
  and spreadsheet users know `if(p, a, b)`.
- **`&&`, `||` and `!`, or the words `and`, `or`, `not` between parts.**
  Symbols a file's author may not know, or words that collide with a
  signal's key; the calls match the condition file's `all` and `any`.
- **Numbers as truths** (C's rule: nonzero is true, a comparison is 1 or
  0). No type check to write, and `(a > b) + 1` and `if(rpm, ...)` load
  and mean something nobody intended.
- **Every input fresh, whichever branch is taken.** One simple rule, but
  a guard against a zero divisor guards nothing when the division it
  avoids still blanks the value.
- **Jumps in the program,** so a branch not taken isn't run. The checker
  must then prove every path's stack and types, and a program's cost
  depends on its inputs.
- **`min` and `max` that skip a part with no value.** The largest of
  four temperatures with one probe dead would read as the largest of
  three, without a word.
- **An infinite input as no value.** One rule for every non-finite
  number, but format 1's `1 / x` at an infinite x would change its
  answer from 0 to none.
- **A computed signal stored and aged like a leaf.** Ages add up level
  by level, and the decoder gains state the reader's marks must save.
- **A `max_age` on a computed input.** Two bounds on one leaf, one of
  them written two signals away from it.
- **No depth limit.** Sound with the same plan, but every walk of a
  chain needs its own guard, and the validator's check stops being a
  fixed number of passes.
- **A wider program** (8 inputs, 64 steps) as a new struct and set
  layout 5. Room for a larger single expression, at an ABI break every
  out-of-tree set restates, new copies in the reader and the app, and a
  second machine kept beside the first for older sets.
- **A program kept inside core,** out of the public set. Free to change,
  but a set built outside core could carry none, and every by-value copy
  of a set would lose its programs.
- **The new grammar in format 1,** with no number changed. Every current
  Havne would refuse such a file as a typing error ("reads 'max', which
  no signal is").
- **A new key for the new grammar** (`formula` beside `expr`). An older
  Havne names the key it doesn't know, as it does for `gauge`, but a
  signal then has two keys for one thing for good.
- **`obd2.boost` as the only way to read obd2's Boost in a chain,** with
  a replacement never read. The example's own Boost would then have no
  chain to join, and a file that replaces MAP would leave obd2's Boost
  reading a def it can't check.
- **`pow`, `exp`, `log` and trigonometry in this format.** Wanted for a power
  estimate, but a recording's values would differ by platform in the
  last digit.
- **Functions with a past in this format.** What a logger's derived channels use
  most after arithmetic, and a second design: state for each signal,
  saved at every mark, with a rule for gaps.

## Consequences

- A signal file says the larger of, the distance between, a guarded
  division and a quantity built in steps, with no code, on every
  platform and in `havne-cli` and Python alike.
- Conditions (ADR-0063) and custom derived channels build on one grammar
  with one set of refusals.
- No struct changes and no layout moves. One public constant changes
  value, the validator accepts sets it refused, and the step-value and
  validator-code lists grow.
- A second format number: this core reads two, the OBDb import writes 1,
  and an older Havne refuses 2 by name.
- A program still reads 4 signals and runs 33 steps. A larger quantity
  is a chain, whose links are signals a user sees in Live and in a
  recording.
- A chain is worked out again from its leaves on every frame that feeds
  it: more steps for each frame than a stored value would cost, in
  exchange for no state and no compounding age. A set with no chain
  decodes as it did (the appendix).
- A comparison in an expression has no hysteresis and no hold. A level
  that must not flap belongs in a condition's term.
- A recording made with a format 2 file is one an older Havne can read
  the values of but not decode again from its frames.
- The functions stop at what is exact everywhere, so a power, a
  logarithm and anything over time wait for a later format.

## Decided

Each choice as taken on 2026-11-10, with the section that holds it.

1. **Where the record goes:** a record of its own, numbered ahead of
   conditions (ADR-0063), which builds on it.
2. **The functions:** `min`, `max`, `abs`, `clamp`, `sqrt`, `round`,
   `floor`, `ceil`, each exact on every platform; `round` takes a half
   away from zero and `clamp`'s crossed constant bounds are refused at
   load (section 1).
3. **The conditional's form:** `if(p, a, b)` (section 1).
4. **Types:** numbers and truths, checked at load (section 2).
5. **Joining comparisons:** `all(...)`, `any(...)`, `not(...)`, the
   condition file's words (section 1).
6. **A missing value:** no value passes through every step, `if` ignores
   the branch it doesn't take, `all` and `any` join as a condition's
   terms do (section 3).
7. **A stand-in for a missing input:** not in this format (section 9).
8. **What a computed signal reads of another:** its value worked out
   from the leaves at this frame; nothing stored, no age added for each
   level (section 4).
9. **`max_age` on a computed input:** refused (section 5).
10. **The depth of a chain:** 8 (section 4).
11. **The program's encoding:** the same 208-byte struct and counts,
    step values appended, no layout change (section 6).
12. **The format:** `"havne_signals": 2` for a file that uses the new
    grammar, and a format 1 file held to format 1 (section 7).
13. **Reading an expression back:** `ecuio_signal_file_format` and
    `ecuio_signal_file_expr`, on `havne-cli`'s lines and in Python
    (sections 6 and 9).
14. **Functions with a past:** the next format, with a design of its
    own; rate of change and smoothing first (section 9).
15. **Custom derived channels:** a design of their own after the
    grammar, with a layer the app owns merged by core over the vehicle's
    file; in v0.8.0 or v0.8.1, since an alert on arithmetic in the app
    needs them.
16. **The mock set:** no format 2 signal; the example in `examples/`
    carries the chain (section 9).
17. **A condition's term holds no expression:** arithmetic stays in
    computed signals (ADR-0063 section 11).

Taken while building:

18. **Spaces before `(`:** allowed; a name is a call when `(` follows it
    after any spaces (section 1).
19. **A truth in the refusals' words:** always "a comparison" (section 2).
20. **An infinite input:** a value; only an input without one is no
    value (section 3).
21. **The validator's memory:** one byte for each program, only for a
    chained set, with its own code when it can't be had (section 6).
22. **A computed replacement in a chain:** read in format 2, judged by
    the reader's file, guarded by the chain check (section 4).
23. **The lists a computed signal is on:** those whose inputs the
    session doesn't get leave its program no value, judged together
    (the appendix of 2026-11-15).

## Appendix: the lists judge what a program lacks (2026-11-15)

Section 4 listed a computed signal unsupported, absent or unasked when
any input under it was, which section 3's `if`, `all` and `any` don't
bear out: `if(obd2.speed > 0, obd2.maf / obd2.speed, 0)` reads 0 at a
standstill on a car without MAF, and was listed, so Live said "Not
asked" over a value that was arriving. A list now holds a computed
signal when its program lacks a value with the inputs the session
doesn't get, all of them at once, judged by a walk of its steps
(`ecuio_computed_lacks`, `core/src/computed.h`):

- a pushed input lacks one when it is gone; a constant never does;
- `if(p, a, b)` lacks one when p does, or when a and b both do;
- `all` and `any` lack one when both parts do;
- any other step lacks one when a part does.

So `any(obd2.maf > 10, obd2.map > 50)` is listed on a car with neither
PID and not on one with either. An input that is computed is judged by
its own walk, through the chain. A format 1 program has no `if`, `all`
or `any`, so it is listed when any input under it is, as before. A
condition on such a signal is NOT_RECEIVED by the same walk over every
input not received, whatever keeps each away (unsupported, absent, its
request dropped). The focus is unchanged: it keeps every leaf under a
picked signal asked, since the branch taken next may need its input.

The walk judges each part apart and never the value it has, so a
listed signal never has a value, but an unlisted one may have none: a
constant predicate (`if(1 > 0, a, b)` with a gone), a constant part of
an `all` (`all(a > 0, 1 > 0)`), a step over constants with no value
(`1 / 0`, which the loader doesn't fold), or a predicate that picks a
gone branch whenever it has a value. Such a signal dims as stale, like
any signal that stopped arriving.

## Appendix: the cost, measured (2026-11-11)

`bench_bridge --decode` dispatches the replay fixture's frames, 40
passes of them, through a session that is never started (no worker, no
bridge), so core's frame decoder is all that runs; median of 9 rounds; `scripts/bench_ab.py decode` alternates 7 runs of a build
before chains (A) and the chains build (B), Windows, Release, unpinned.
The figure is the median over the runs, its range in brackets.

| Set | A | B | B/A | Values for each frame |
|---|---|---|---|---|
| `obd2` alone (no computed signal) | 51.8 ns (51.0-59.4) | 53.1 ns (51.8-64.6) | 1.03 | 1.00 |
| `obd2` and 8 computed signals, format 1, no chain | 92.9 ns (88.9-105.0) | 93.0 ns (91.9-99.0) | 1.00 | 2.55 |
| `obd2` and a chain 8 deep over its PIDs | refused | 201.1 ns (194.5-226.9) | - | 4.99 |

Each build's range holds the other's median in both rows A has: a set
without a chain decodes in the single pass, behind one test of whether
the set is chained. The chain 8 deep costs about 108 ns for each frame
over 8 unchained programs, for about 2.4 more values.

`bench_ab.py load` (the bridge's worker over the fixture under `obd2`,
no computed signal), 9 alternated runs of each on a quiet machine:
488,881 frames/s for A, 497,204 for B (1.02); the p99 from frame to the
GUI at 100x, 0.32 ms both. A set of 7 runs just before it was bimodal
(each build's runs near either 350k or 480k frames/s), so its median
said which mode the runs landed in, not which build was faster.

## Appendix: an expression checked and worked out apart from a file (2026-11-16)

Section 9 left a front end one way to learn whether an expression is
good: write the signal into a file and load it. An editor needs the
answer at each keystroke, for a signal that is in no file yet, and it
needs a value to show beside it. Four calls in `signals.h` give both
against a file that is already loaded. None of them changes the grammar,
the program or the loader's rules: the loader's compiler moved into
`core/src/signal_expr.cpp` behind a table of names, and both the loader
and the check call it.

**The check.** `ecuio_signal_file_check_expr(file, key, expr, length,
out)` compiles the expression as a computed signal of the file: the names
the file's own expressions read, the grammar at its newest format, a
program's bounds, and the chain rules of section 4. `key` names the signal
the expression is for. With one of the file's keys the expression stands
in that signal's place, so a circle through it is found and a reader that
keeps an age bound on it is refused when the signal turns from a number
to a computed one (section 5); with none it is a new signal, which nothing
reads yet. A refusal carries the character, counted from 1, and the
loader's own middle words behind "the expression" ("the expression reads
'bost', which no signal is"): the place is a number of its own, so the
words neither quote the expression nor repeat the place, as the loader's
sentence must for a reader who has only the file's line and column.

**The format is reported, not enforced.** The check compiles at the
newest format and says the oldest `havne_signals` whose loader takes the
expression: 2 when it uses a call, a comparison or `if()`, reads a
computed signal, or is read by one; else 1. Holding the check to the
file's own format was the first design, and it would refuse every
function in a format 1 file in words about a number the user never typed.
The front end decides: an editor writes the file's format up on Save and
says that an older Havne can't read it, and `havne-cli signals expr` says
the two numbers. The number is the one file's: in a plugin variant made of
several files, each file whose signals read the checked one needs it too.

**The name an expression reads a def by.** `ecuio_signal_file_expr_ref`
gives it: the key for a signal of the file's own, and for a base def of a
file that extends obd2 `obd2.` and obd2's key (the short name where it has
one, else `pid_14`, or `pid_14_1` for a PID's second field). Compiling the
name reads that def. obd2's computed Boost has no name, since obd2's keys
name its Service 01 fields; a front end leaves it out of what it offers.
A signal whose key starts with a digit has none either: the loader takes
such a key, and the grammar reads it as a number. One that stands in a
base def's place goes by obd2's name for that def.

**A program's value with no session.** The check hands back the program,
and `ecuio_signal_file_evaluate` works any program out over the file's set
from values the caller gives, each with its age, so the expression being
typed previews with no draft file. `ecuio_signal_file_expr_leaves` lists
the numbers under a program through any chain: what there is to give a
value. The freshness rule is the decoder's own code (`computed.h`'s
`ecuio_computed_bound_us` and `ecuio_computed_fresh`: a value at its bound
counts), and a test holds the two to the same bits over made-up frames.
A program a caller builds is refused where the set's validator would
refuse it as one of the set's own.

**Why there is no value, by rule.** With no value the result names the
leaf of the first read without one, in the order the reads are made,
missing or stale with the bound it is past;
"the steps gave no number" is said only when no leaf lacked a value. A
stale leaf that only an `if`'s untaken branch reads is therefore named
ahead of a division by zero in the taken branch: giving that leaf a value
then shows the division. An exact cause would need a second machine
beside `ecuio_computed_run` or a slower worker, for a case one more test
value settles.

**Not judged.** The signal's name, unit, gauge and `max_age`, and the
file's own bounds, stay the loader's when the file is read. A checked
program has no age bounds (they are the file's `max_age` member); a caller
that wants a preview under a typed bound sets it in the program, where the
validator's bounds apply.

**Room to grow.** An evaluation's input carries `flags` and `reserved0`,
and a nonzero one is refused, so a later core can give them a meaning
without an older one passing it by.

All four calls only read the file, so any thread may call them while it
lives.

## Appendix: the editor and where a computed signal is kept (2026-11-17)

The signal form writes a computed signal into the vehicle's own variant
through `ecuio_plugin_put_variant` ([ADR-0064](0064-one-store.md) Q7.3),
checking it as typed with the calls of the appendix above. A channel over
another plugin's signals waits for a plugin that extends another's variant
(ADR-0064 Q8).

- **Not taken:** a layer for each vehicle merged by core (a second store,
  a profile or URI key, a CLI flag), and a second data file in the picked
  variant kept across re-imports.
- **The first `obd2.` input adds `extends: "obd2"`** to the own variant, so
  a vehicle's signals read with obd2's from the save that needs it; every
  own plugin extending obd2 from its first save was the other pick.
- **The preview is typed values, not a live timer.** The form can fill
  them once from the connected session; nothing re-evaluates by itself.
