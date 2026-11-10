# ADR-0057: Configurable dashboards: the app's pages of gauges, one set for every vehicle, each gauge bound to a signal by a reference that outlives the profile, stored per form in the app's settings

- **Status:** Accepted. Built: section 10's steps 1 to 7 (each in an
  appendix); step 8's run has no session sheet yet.
- **Date:** 2026-11-07.
- **Builds on:** [ADR-0001](0001-qt-free-core-c-abi.md) (rule 2: the UI
  gets final display values); [ADR-0022](0022-display-units.md) (the
  unit system, converted in core at the UI's edge);
  [ADR-0027](0027-vehicle-profiles.md) section 5 (preferences as one
  JSON document in QML `Settings`, its rules in a `pragma Singleton`);
  [ADR-0039](0039-full-service01-coverage.md) section 8 (what the
  Dashboard reads); [ADR-0048](0048-semantic-versioning.md) (an
  additive C ABI change is a minor release);
  [ADR-0050](0050-finding-a-signal.md) (the signal filter);
  [ADR-0051](0051-signal-files.md) section 2 (a signal file's `key` as
  the stable name); [ADR-0052](0052-plugin-framework.md) (a plugin
  variant's signals as a signal file).
- **Leads to:** v0.7.0's dashboard steps (section 10); the gauge roles
  the low-glare and night themes set (section 9); a plugin's default
  layout and alerts on the gauges, each a later decision on this
  document's format.

## Context

The Dashboard is one fixed page: a shift-light strip, two dials
(engine speed, vehicle speed), two rings (coolant, oil), two digital
tiles (IAT, MAF) and two bars (throttle, load), in three hand-written
arrangements (`DashboardPage.qml`'s `fit`: stacked, narrow, wide). Each
gauge reads one `Tap`: the first of a list of core's def IDs
(`Bridge.gaugeSignals`, a mock ID and an `obd2` ID per gauge) that the
session's set holds, looked up through `SignalHub`, an ID-keyed mirror
of `Bridge.signalModel`. Scale ends and zones are written in the
decoded units and shown through `Bridge.displayValue` (ADR-0022). A
gauge with no signal says why: "no session", "not in profile", "waiting
for data".

A user's own signals (ADR-0051), a plugin variant's (ADR-0052) and
Service 01's other PIDs (ADR-0039) reach Live but never a gauge. The
roadmap's v0.7.0 asks for a gauge library, any signal on any gauge,
several pages, and the phone's layout apart from the desktop's.

What the code gives this decision:

- **Signals have two kinds of stable name, and some have none.** A
  built-in set's def IDs are public constants (`signal_sets.h`): the
  mock's from 1001, `obd2`'s first ten fixed at 7001-7010 and the rest
  `ECUIO_OBD2_PID_SIG_ID(pid, field)`, append-only. A signal file that
  `extends` `obd2` loads `obd2`'s defs first, with those IDs. A file's
  own defs are numbered by their place (`ECUIO_USER_SIG_ID_BASE` plus
  the index), which a reordered file changes; ADR-0051 section 2 names
  `key` as the name a Dashboard's tile refers to, 1 to 31 of `a-z`,
  `0-9` and `_`. A plugin variant's signals are a signal file
  (`ecuio_plugin_variant_file`), keys included. `uds-generic`'s IDs
  follow the URI's `did=` order and its defs have no key.
- **Names aren't stable.** A set may reword a name, and a gauge's
  label isn't its def's name: the tach is "Engine speed", and the mock
  and `obd2` both name its def "RPM". `QmlTest.dashboard`'s
  `test_aGaugeBindsByRefNotByName` pins that a gauge binds by reference,
  never by name.
- **Core finds a def by its key** (`ecuio_signal_file_find`) **but
  can't name the key of a def**, so the bridge can't tell QML which key
  a row carries. `obd2`'s own key names (`rpm`, `pid_0c`) exist only
  inside the signal-file loader, for computed expressions.
- **A def has no display range.** Its field (kind, bit length, scale,
  offset) bounds what it can decode, which for engine speed is 0 to
  16383.75: a span, not a gauge's range.
- **The model's rows are the set's defs in order** (`setSignalSet`),
  each with `IdRole`, `NameRole`, `UnitRole` (converted), `DecimalsRole`,
  `GroupRole`, `IsTextRole`, `LabelRole`, `HiddenRole`, `RefusedCodeRole`,
  `WindowHighRole`, `WindowLowRole`, `HasValueRole`, `AgeMsRole`.
- **Preferences** are QML `Settings` over QSettings (the registry on
  Windows, an ini under `HAVNE_SETTINGS_DIR` for the tests and the
  run-havne skill), and the rename's carryover copies every key. The
  vehicles are one JSON string there (category `vehicles`), parsed and
  changed by pure functions in `VehicleBook.qml`, tested without a page
  (`tst_vehiclebook.qml`).
- **The gauges draw without Canvas** (AnalogGauge's header: a Canvas
  repaint rasters the whole dial on the CPU at animation rate, which a
  3x phone screen pays for; a Canvas also paints at its own size under
  the zoom). Each is a `GaugeBase` (range, easing, zones, the sweep,
  peak and low hold) inside a `GaugeCard`. DigitalGauge's 8 s trace is
  a `ShapePath` over an array sampled at 10 Hz.
- **A `Popup` doesn't fit the zoom** (it draws in the unscaled
  overlay); every in-page question so far is a panel inside the page's
  `Flickable`.
- **The density rules:** 48 dp targets on a
  phone (`Theme.touchTarget`), 8 dp between them, at most three buttons
  in a phone's row, nothing past the edge at 320 dp.

The decisions of 2026-11-06 hold here as given: the layouts are the
app's, not a vehicle's, one set every vehicle and session uses; several
pages, swiped on the phone and clicked or keyed through on the desktop;
the phone's and the desktop's layouts kept apart; a missing signal
shows "-" and says so; the night and low-glare looks in red and orange,
suggesting a segmented bar and a seven-segment readout for the library;
and no dashboard in `havne-cli`, which isn't a gap.

## Decision

### 1. The library: eight kinds on one contract

Each kind is one QML component on `GaugeBase`, named in a layout by the
first column. Kinds are append-only: a name is never reused for
another drawing.

| Kind | Component | What it draws | Its own options | Default span (w x h) |
|---|---|---|---|---|
| `analog` | AnalogGauge | Dial, needle, zones, tell-tale peak | `majorStep`, `minorStep`, `labelDivisor`, `caption`, `progress` | 2 x 8 |
| `ring` | RingGauge | A thick arc colored by zone, a centered readout | none | 1 x 4 |
| `bar` | BarGauge | A straight LED bar with scale marks, horizontal or vertical | `vertical`, `marks` | 2 x 2 (vertical 1 x 4) |
| `segments` | SegmentGauge (new) | Discrete cells along a rising curve, each cell taller than the one before (a tachometer's curve), lit up to the value | `cells`, `rise` | 2 x 3 |
| `digital` | DigitalGauge | A number, its unit, an 8 s trace and the session's low and high | none | 1 x 4 |
| `seven-segment` | SegmentReadout (new) | Digits of seven segments, the unlit ones faint, the unit beside | `digits` | 1 x 2 |
| `sparkline` | SparkGauge (new) | One trace over a window, the newest value at its end, the window's low and high as a scale | `window` (10 to 120 s) | 2 x 3 |
| `shift-light` | ShiftLightBar | A row of lights filling to the shift point, flashing past it | `count` | full width x 1 |

**The shared contract** is `GaugeBase`'s today: `value`, `windowHigh`,
`windowLow`, `hasValue`, `stale`, `from`, `to`, `label`, `unit`, `note`,
`decimals`, the zones `coldBelow`, `warnFrom`, `dangerFrom`, the hold
(`peakValue`, `lowValue`, `resetPeaks`) and `sweep()`. Every kind takes
the common options `from`, `to`, `coldBelow`, `warnFrom`, `dangerFrom`,
`decimals` and `peak` (the hold on or off); a shift light reads `from`
as its first light and `to` as its shift point. A kind ignores a common
option it has no use for (a seven-segment readout has no zones to
color, so it colors its digits by them instead).

The new kinds keep the gauges' drawing rule: static items (Shapes,
Rectangles, Text) built once for a size, and per frame only opacities
and one transform change. A seven-segment digit is seven Shape paths
whose opacity follows the digit; a segment cell is a Rectangle whose
opacity follows the value, as BarGauge's are; the sparkline is
DigitalGauge's `ShapePath` trace with a longer window (at 10 Hz, 1,200
points at most), never `StripChart`, which is a Canvas. The sparkline
samples only while its page is on screen, so a page left and returned
to starts its trace again; the trace is a glance, Live's chart is the
record.

Why eight and these: the five the roadmap names, the shift light the
page has, and the two kinds the night-theme references suggest. A
kind not on this list (a horizontal needle, a numeric with a bar) waits
for a user to ask: each kind is a component to draw, test at two sizes
and keep on three themes.

### 2. A gauge binds to a signal by a reference

A reference is a short string:

- **`id:<n>`**: a built-in set's def by its ID (`id:7003` is `obd2`'s
  engine speed; `id:1001` the mock's). It matches in every set that
  holds that ID, so a signal file extending `obd2` keeps `obd2`'s
  gauges.
- **`key:<key>`**: a signal file's own def, or a plugin variant's, by
  its `key` (`key:boost`). It matches the session's file whatever the
  file's name, so one layout reads `boost` from whichever vehicle's
  file has that key, and from a recording replayed with its own
  `signals.json`.

A cell holds an ordered list of references, at most 8, and shows the
first the session's set provides, as `Tap` does today: the built-in
layout's engine speed is `["id:1001", "id:7003"]`. A reference of any
other form is kept as it is and matches nothing, so a later form (a
`did:` for `uds-generic`, should one be decided) reads in an older
version as a gauge without its signal, not as damage.

A `uds-generic` row has no reference, so Add doesn't offer it: its ID
is the URI's order and its name is display text. A DID that belongs on
a gauge goes in a signal file (ADR-0051), which is also where it gets
its unit and scale. Names are not references: they differ between sets
for one quantity and a set may reword them. A file's def ID isn't one
either: a reordered file renumbers.

**Core's part:** two functions, appended to `signals.h`, additive in
v0.7.0's feature release (ADR-0048), exported by the shared core and
regenerated into the Python bindings as every public change is:

```c
/* The "key" of a def of the file's own (never a base set's, extends):
 * valid until ecuio_signal_file_free; "" for a base set's def, a def
 * that isn't the set's, or a NULL argument. The inverse of
 * ecuio_signal_file_find. */
const char* ecuio_signal_file_key(const ecuio_signal_file_t* file,
                                  const ecuio_signal_def_t* def);

/* The lowest and highest value a def's field decodes to (scale and
 * offset applied; INT sign-extended; an ENUM as validated its raw
 * range), in the def's unit. ECUIO_DECODE_OK, or
 * ECUIO_DECODE_ERR_UNSUPPORTED for a kind with no such span (FLOAT,
 * the text kinds, COMPUTED), or ECUIO_DECODE_ERR_NULL. *lo and *hi are
 * written only on OK. */
int ecuio_def_range(const ecuio_signal_def_t* def, double* lo, double* hi);
```

The key function takes the def, as `ecuio_enum_label` does, rather
than an index: the caller has the def (a model row is the set's def at
that row), and an index would need the set's and the file's numbering
to be one thing. The range lives in core because it is the field model
(sign extension, the FLOAT and text kinds have no span): the bridge
computing it would be a second decoder's worth of rules in the app,
against rule 2's spirit, and `havne-cli` and the Python bindings get it
for free.

**The bridge's part:** `SignalListModel` gains `RefRole` (`ref`):
`id:<n>` for a def of a built-in set (the mock, `obd2`, and a file's
`extends` base), `key:<key>` for a file's or a plugin variant's own def
(the file found through the stores' `fileFor(set)` and the bridge's
own replay files, the key through the function above), "" for
`uds-generic` and every other row; fixed per set. And `RangeLowRole`
and `RangeHighRole` (`rangeLow`, `rangeHigh`, through
`ecuio_display_convert` like the value, so they read in the unit system;
NaN where core gives no range). `SignalHub` keys its entries by
reference instead of def ID and `lookup()` takes a list of references;
its `Entry` adds `ref`, `hidden`, `refusedCode`, `label`, `isText`,
`rangeLow` and `rangeHigh`. `Bridge.gaugeSignals` becomes a map of
reference lists (`["id:1001", "id:7003"]` per key), what the built-in
layout and the Add flow's counterparts (section 5) read.

For building a page with no session connected, the bridge offers
`Bridge.profileSignals(uri)`: the rows (name, unit, decimals, group,
ref, rangeLow, rangeHigh, isText) of the set the URI's profile names
(`ecuio_profile_lookup`: the built-ins, the vehicles' signal files and
the plugins' variants as registered; none for `uds-generic` or an
unknown profile). The same shape as the model's rows, so Add's list is
one delegate either way.

### 3. What a gauge shows without its signal

A layout is never changed because the session lacks a signal: a cell
naming a signal that no profile, signal file or plugin provides stays
in the layout, is saved with it, and reads again under the next
session whose set has it. The gauge draws its face at rest, its value
"-", and its note says which state it is in, the first that applies:

1. "no session";
2. "not in this profile: <the cell's label>" (no reference matches a
   row);
3. "not supported by this car" (the row is hidden: core found it
   unsupported or absent);
4. "refused (7F 22 31)" (the row's `RefusedCodeRole`);
5. "waiting for data".

A stale value dims (`Theme.staleMs`), as today. In edit mode a cell in
state 2 or 3 carries a mark, so a user tidying a page sees which gauges
this vehicle can't fill.

Text rows are never offered. An enumeration row is offered to
`digital` alone, which shows its label (`LabelRole`) and falls back to
the number; a seven-segment readout has digits only, and the other
kinds would plot a code as a quantity.

### 4. Pages, the two forms and the grid

The dashboards are the app's: one set that every vehicle and session
uses. There are two forms, each with its own pages: **phone** (Android
and iOS, `Theme.mobile`) and **desktop** (everything else). A layout
made on one is never shown on the other: a gauge sized for a 384 px
column reads badly in a 1400 px one. The form is the platform's, not
the window's width, so a desktop window narrowed under 720 px keeps
the desktop pages (at fewer columns), and a tablet gets the phone form.

A form holds 1 to 10 pages; a page has a name and 0 to 24 cells. Each
cell has a span, `w` columns (1 to 12) and `h` rows (1 to 16). The
**row unit is a quarter of the column width**, so a cell with `h` four
times `w` is square and the compact kinds (a shift light at `h` 1, a
bar at 2) keep their proportions at every width; a span wider than the
grid takes the whole row. Cells are stored **in order**, not at
positions: placement puts each cell, in order, at the first free block
of its span scanning rows top to bottom and columns left to right
(`DashboardBook.place`, pure). So a page reflows when the columns
change and a move is a change of order.

- **Phone:** 2 columns upright, 4 on its side. Pages swipe
  (`SwipeView`), a `PageIndicator` above the tab bar.
- **Desktop:** the columns follow the width, about one per 220 px,
  between 2 and 12. Page buttons in the header; Left and Right (with no
  text field focused) go to the previous and next page.
- A page taller than its area scrolls, as the Dashboard does, with the
  scroll bar shown while it overflows.
- **Reset peaks** and the once-a-session sweep act on every page; a
  change of units resets the peaks, as today.

### 5. Edit mode

Edit starts from the header's **Edit** button on both forms, or a long
press on a gauge on the phone; **Done** ends it and saves. Every
control it shows meets the density rules (48 dp targets, 8 dp apart,
at most three buttons on a phone's row), and every panel it opens is
in the page, inside its scroll area, never a `Popup`.

- **Add:** a "+" cell at the page's end opens the kinds (each with a
  small still preview), then the signals: the session's numeric rows
  with references, searched with `SignalFilterBar` (ADR-0050); with no
  session, `Bridge.profileSignals` of the URI Settings holds, so a page
  can be built in the garage.
- **A new gauge's options:** the label is the row's name; `decimals`
  the row's; `from` and `to` from section 6's rule; no zones. The
  references are the picked one plus its counterparts in the other
  built-in set where `Bridge.gaugeSignals` pairs them, so an
  engine-speed gauge picked under the mock works under `obd2` and the
  other way.
- **The cell's panel** (a tap on a cell in edit mode): signal, label,
  range, zones, decimals, peak hold, the kind's own options, the span
  as two steppers (`w` and `h`), and **Move earlier**, **Move later**,
  **To the start**, **To the end**. A move is a change of order on both
  forms. Drag-to-reorder, on both forms, is v0.7.1's, on top of the
  same order: v0.7.0's done-when is a page built from the library that
  survives a restart, and a drag in a flowing grid adds a
  pointer-handler layer and a test surface for what four buttons
  already give.
- **Remove:** the cell's close button, at `Theme.touchTarget`.
- **Pages:** add, rename, move left or right, remove (never the last).
- **Reset to the built-in layout:** under the edit bar's menu, with an
  in-page confirmation; it replaces this form's pages only.

### 6. Ranges and units

Options are written in the unit the signal decodes to (the metric one
core gives), with that unit stored in the cell (`unit`), and shown in
the current unit system through `Bridge.displayValue`, as the
Dashboard's scales are today. When a reference resolves to a row whose
unit differs from the cell's (another vehicle's `boost` in another
unit, judged by `Bridge.displayUnit(cell.unit)` against the row's
unit), the gauge takes the row's range (section 2) for its scale, keeps
the stored options for the next match, and its note adds the unit it
expected.

A new gauge's range (`DashboardBook.defaultRange(unit, rangeLow,
rangeHigh)`, pure) is a table by the decoded unit first, the field's
range second, 0 to 100 last: engine speed 0 to 8000, km/h 0 to 260,
deg C 0 to 150, percent 0 to 100, kPa 0 to 300, V 0 to 16, g/s 0 to
300, L/h 0 to 60. A field's span alone is wider than a useful dial for
most 16-bit PIDs, and the table covers nearly every gauge a user adds;
the user narrows the rest.

### 7. Where it's stored, and how it's read

QML `Settings`, category `dashboards`, as the vehicles are (ADR-0027
section 5): key `phone` and key `desktop`, each one JSON string, and
`phonePage` and `desktopPage`, the page last shown (an integer, so a
swipe doesn't rewrite the layout). A form with no stored text shows the
built-in layout, which isn't stored until the user's first edit, so a
later version's improved built-in layout reaches everyone who never
edited theirs.

```json
{
  "format": 1,
  "pages": [
    {
      "name": "Engine",
      "cells": [
        { "kind": "shift-light", "w": 12, "h": 1, "label": "Shift lights",
          "refs": ["id:1001", "id:7003"], "unit": "RPM",
          "options": { "from": 4500, "to": 6500 } },
        { "kind": "analog", "w": 2, "h": 8, "label": "Engine speed",
          "refs": ["id:1001", "id:7003"], "unit": "RPM",
          "options": { "from": 0, "to": 8000, "dangerFrom": 6800,
                       "majorStep": 1000, "labelDivisor": 1000, "peak": true } },
        { "kind": "digital", "w": 1, "h": 4, "label": "Boost",
          "refs": ["key:boost"], "unit": "kPa",
          "options": { "from": -100, "to": 150 } }
      ]
    }
  ]
}
```

The key is `refs`, never `signals`: a QML property of that name meets
Qt's macro (qml.md's `signalList` rule), and the page binds cells to
properties.

The rules are pure functions in a `pragma Singleton`,
`DashboardBook.qml` (`parse`, `serialize`, `builtIn(form)`, `place`,
`defaultRange`, `addCell`, `moveCell`, `resizeCell`, `removeCell`,
`setOptions`, the page functions, `reset`), each taking a document and
returning a new one, never changing its argument, so
`tst_dashboardbook.qml` runs them without a page, as
`tst_vehiclebook.qml` runs VehicleBook's.

**Reading** keeps what it can, and never loses what it can't read:

- **Within format 1,** a cell of a kind this version doesn't know, and
  any key it doesn't know (in a cell, its options or a page), is kept
  as read and written back. Such a cell draws as an empty card saying
  "a gauge this version of Havne doesn't have" and can be removed. So a
  later version adds kinds and options without a new format, and going
  back a version loses nothing.
- **A known field of the wrong type or out of its bounds** drops that
  cell, or that page when it's the page's own field; the rest reads. A
  dashboard holds no identity, so one bad cell costs one gauge, not
  every page (the vehicles drop whole because a half-read entry could
  name the wrong car).
- **Text that doesn't parse,** a `format` that isn't a whole number,
  or a `format` above the one this version reads: the built-in layout
  shows, the page says once which of these it was ("couldn't be read",
  "made by a newer version of Havne"), and the stored text is left
  alone. The first edit after that copies it to `phoneUnread` or
  `desktopUnread` (one slot, the newest) before saving the new layout.
- **Bounds:** 10 pages, 24 cells a page, 8 references a cell, a label
  of 40 characters, `w` 1 to 12, `h` 1 to 16, and 64 KiB of text a
  form; past a bound the document reads as damaged.
- **A new format** comes only with a change an older reader would
  misread; adding a kind, an option or a reference form isn't one.

A file per form under the settings folder was weighed (Alternatives);
an export and import of a form's pages as a file can follow on top of
this document.

### 8. havne-cli has no dashboard

Gauges are display only, so `havne-cli` has none, and this isn't a gap
in it: its values are the same values (`stream --values`, the
recordings), and the two core functions (section 2) reach it and the
Python bindings like any other C ABI addition.

### 9. Colors and themes

The gauges take every color from `Theme`, and this ADR adds the roles
they need so the low-glare and night themes can set them: `gaugeFace`,
`gaugeValue` (the lit segments, the arc, the readout), `gaugeNeedle`
(AnalogGauge's needle, a literal today), `gaugeTick`, `gaugeWarn` and
`gaugeDanger`. Warning and danger differ by more than hue: past
`dangerFrom` a readout gains a mark beside its value and the zone's
stroke is heavier. The themes themselves (the palettes, the setting,
Home's logo) are the themes item's.

### 10. Steps

Each step is handed over on its own branch with its tests and mutants;
the model named is the one sized for it.

1. **Core:** `ecuio_signal_file_key` and `ecuio_def_range`,
   their tests, `test_c_abi`, the exports check, the regenerated
   `_ecuio.py`.
2. **The bridge and the hub:** `RefRole`, `RangeLowRole`,
   `RangeHighRole`, `profileSignals(uri)`; `SignalHub` by reference
   with its wider `Entry`; `gaugeSignals` as reference lists. The
   Dashboard reads through it and looks the same.
3. **The rules:** `DashboardBook.qml` with the built-in layout
   per form (today's gauges, in today's order, at spans that keep their
   proportions) and every function of section 7; `tst_dashboardbook`.
4. **The renderer**, to this ADR: `DashboardPage.qml` draws a
   form's pages from a document (no editing yet): the grid, the phone's
   swipe, the desktop's buttons and keys, section 3's states and notes,
   the saved last page. `tst_dashboard` keeps its cases over the
   built-in layout.
5. **The new kinds and options:** `segments`, `seven-segment`,
   `sparkline`; `vertical` and `marks` on the bar; `peak` on every kind.
6. **Edit mode** (the rules being step 3's):
   Edit and Done, Add, the cell's panel, remove, the pages' controls,
   Reset, the save to `Settings` and the restart test.
7. **The gauge roles in `Theme`**, with the themes item.
8. **The done-when run** on the test phone and the desktop: a page built
   from the library survives a restart; the run sheet names the build.

## Tests

- **Core:** `SignalFileTest` (`test_signals`): every own def's key, ""
  for an `extends` base's defs, a def of another set and NULL; the same
  through a plugin variant's file (`PluginTest`). `SignalsTest`: the
  range of an unsigned, a signed and a scaled field, an ENUM, and
  UNSUPPORTED for FLOAT, the text kinds and a COMPUTED def; the mock's
  and `obd2`'s defs each give a range that holds every value the fakes
  answer. `test_c_abi.c` compiles both declarations as strict C.
- **Bridge** (`test_bridge`): `RefRole` for the mock, `obd2`, a file
  extending `obd2` (base `id:`, own `key:`), a plugin variant, a replay
  with its own `signals.json`, and "" for `uds-generic`; the range
  roles in both unit systems; `profileSignals` for a built-in, a
  registered file, `uds-generic` (none) and an unknown name.
- **Rules** (`tst_dashboardbook.qml`, QML tier): parse and serialize
  round trips; an unknown kind, an unknown reference form and unknown
  keys kept; a wrong-typed field drops its cell and keeps the rest; a
  newer format and damaged text give the built-in layout and leave the
  text; the first edit copies it to the `Unread` key; each bound;
  placement at 2, 4 and 12 columns, a span wider than the grid, the
  order kept across a reflow; each move and page function;
  `defaultRange` by unit, by field, and the fallback.
- **The page** (`tst_dashboard.qml`): each kind shows "-" and each note
  of section 3 in its state; a layout naming a signal no profile has
  keeps that cell after a profile switch and a save, and the gauge
  reads again when a set that has it returns; an enum label on
  `digital`; stale dims; Left and Right and the page buttons on the
  desktop form, a swipe on the phone form; the last page shown comes
  back; Add, the panel, move, resize, remove and Reset, each saved to
  `Settings` (the test's ini under `HAVNE_SETTINGS_DIR`) and read back
  by a new page (the restart).
- **Sizes:** the phone form at 384 x 832 and 832 x 384: every cell
  inside the width (`pastTheEdge`'s rule), every edit control at least
  `Theme.touchTarget` on the glass; the desktop form at 1280 x 800 and
  500 x 300 (a whole dial still fits a short page).
- **Mutants** for each new test, as every new test has.
- **Manual:** the done-when on the test phone and the desktop, filed
  in the run sheets.

## Alternatives considered

- **Layouts per vehicle** (the roadmap's first wording, and a file per
  vehicle id in the design note): each new vehicle would start over;
  with a missing signal shown as "-", one layout serves every car.
- **A file per form** (`<settings>/dashboards/phone.json`): a layout
  is small and changes in place, as the vehicles do; `Settings` already
  holds those, the rename's carryover copies it, and the tests' ini
  holds it. Signal files and plugins are files because they are
  imported and kept by content; a layout is neither.
- **Binding by name:** names differ between sets for one quantity and
  can be reworded.
- **Binding a file's signal by its def ID:** a reordered file
  renumbers.
- **One key namespace for every set** (`obd2:rpm`, `mock:rpm`,
  `user:boost`, `did:7E9:1E1C`), with core naming every def's key: it
  reads better in the stored text and would reach `uds-generic`, but it
  needs keys for the mock that don't exist, a buffer-filling call for
  `obd2`'s computed names, and a way for the bridge to know which
  namespace a session's set is in. A def ID is already the contract
  the Dashboard relies on and is as stable as a key; the reference
  grammar leaves room for a `did:` form later.
- **The app reading a file's keys from its JSON:** a second reader of
  the format beside core's strict one, and a plugin's variant files
  aren't the app's to parse.
- **The range worked out in the bridge from the def's field:** a
  second copy of the decoder's rules (sign extension, FLOAT, the text
  kinds) in the app; core's one function serves every front end.
- **No default range, or the field's span alone:** the user types a
  range for every gauge, or narrows a 16,383 rpm dial; a table by unit
  covers nearly every gauge added.
- **Cells at fixed grid positions:** they don't reflow between 2 and 4
  columns; an order does, and a move stays one change.
- **Square cells (the row unit a whole column):** a shift light or a
  bar at one row is as tall as it is wide; the quarter-column unit
  keeps the compact kinds' proportions.
- **One document holding both forms:** an edit on one form would
  rewrite the other's text, and nothing moves a layout between them.
- **Dropping the whole document on any damage** (the vehicles' rule):
  one bad cell would cost every page.
- **Drag-to-reorder in v0.7.0:** a pointer-handler layer and a test
  surface over a flowing grid, for what the panel's move buttons give;
  v0.7.1 adds it on the same order model.
- **Canvas for the new kinds, or `StripChart` for the sparkline:** the
  CPU repaint AnalogGauge avoids, and the zoom's scaling trap.
- **Offering `uds-generic` DIDs by their name** ("0x7E9 DID 1E1C"):
  binding by display text, which the rest of this decision refuses; a
  signal file gives the DID a key, a unit and a scale.

## Consequences

- Any signal a session decodes, numeric and with a reference, can go
  on any of eight gauges, on pages the user arranges, kept across
  restarts and shared by every vehicle.
- The Dashboard page becomes a layout's renderer; today's look is the
  built-in layout, so a user who never edits sees the same gauges in
  the same order.
- Core gains two functions (a minor version); the model three roles and
  one invokable.
- A `uds-generic` DID reaches a gauge only through a signal file.
- A def still has no display range of its own: a new gauge starts
  from a table by unit, then its field's span. A range in the def's
  reserved room would be a separate decision.
- A plugin's default layout for its variants is left to a later
  decision, on top of this document's format.
- Alerts on the gauges (the roadmap's optional item) would read the
  zones' thresholds; they aren't part of this decision.
- Drag-to-reorder (v0.7.1), and an export and import of a form's
  pages, build on the order and the document as decided here.

## Appendix: core's two functions as built (2026-11-07)

Section 10's step 1.

- **`ecuio_def_range(def, lo, hi)`** (`signals.h`, beside the decoder
  in `core/src/signals.cpp`) makes the decoder's checks in the
  decoder's order, so a hand-built def gets the decoder's word:
  `ECUIO_DECODE_ERR_NULL` for a NULL argument;
  `ECUIO_DECODE_ERR_UNSUPPORTED` for a kind or byte order with no
  decoder, a text kind or a COMPUTED def (whose kind reads UINT, so
  the flag is checked with the kind); `ECUIO_DECODE_ERR_BAD_PARAMS` for
  bit parameters the decoder refuses (a `bit_length` of 0 or over 32,
  a `bit_offset` over 7, a big-endian field not whole bytes); then
  `ECUIO_DECODE_ERR_UNSUPPORTED` for a FLOAT, where the decoder's FLOAT
  check comes, since a FLOAT's field holds any binary32. Last,
  `ECUIO_DECODE_ERR_BAD_PARAMS` for an end that isn't finite or a span
  of zero width (a scale of 0, inf or NaN, an offset NaN, an end past
  the double range), which no UI can scale a gauge to. `BAD_PARAMS` is
  a fourth code beside section 2's three. The mux key doesn't move the
  span and isn't checked (`mux_length` included). UINT spans 0 to
  2^n - 1 and INT -2^(n-1) to 2^(n-1) - 1, both through scale and
  offset, with `lo` < `hi` for a negative scale. An ENUM scales as
  the decoder scales it, so an ENUM as validated (scale 1, offset 0)
  gives its raw range.
- **`ecuio_signal_file_key(file, def)`** (`core/src/signal_file.cpp`)
  gives the key whose def has that address in the file's set. Another
  set's def with the same ID, and a copy, give "", as a base set's defs
  do (they are in the set with no key). A plugin variant's file is the
  same structure, so it needs nothing more.
- Both are exported by the shared core and bound in
  `python/havne/_ecuio.py`.
- **Tests:** `SignalsTest.ADefsRangeIsItsFieldsSpanScaled`,
  `.ADefsRangeRefusesWhatHasNoSpan`,
  `.EachBuiltInDefsRangeIsTheDecodersExtremes` (each def of the mock
  and `obd2`: its ends are the decoder's values at the field's lowest
  and highest raw) and `.ADefsRangeIsTheDecodersExtremesForEveryFieldShape`
  (UINT and INT, each byte order, 1, 8, 13 and 32 bits, a positive and
  a negative scale, bit 0 and 3, and a muxed def, against the decoder;
  a shape it refuses gets the same code); `Obd2SetTest.EveryValueTheFakesAnswerIsInsideItsDefsRange`
  (`test_obd2`, where the fakes are); the mock's round trip over three
  loops (`CoreTest.MockDriveCycleEveryTickRoundTripsThroughTheMockSet`)
  keeps each signal inside its range; `SignalFileTest.EachOwnDefNamesItsKeyAndNoOtherDefDoes`;
  `PluginTest.AVariantsFileNamesEachOfItsOwnDefsKeys`; `c_abi`.

## Appendix: the bridge and the hub as built (2026-11-07)

Section 10's step 2. No ABI change; the Dashboard looks and reads as
before it.

- **A row's reference** (`SignalListModel`'s `RefRole`, `ref`, fixed
  per set): `id:<n>` for a def of the registered mock or `obd2` set
  (the def `ecuio_profile_lookup` gives, by address) and for a file's
  `extends` base; `key:<k>` for a file's or a plugin variant's own def,
  through `ecuio_signal_file_key`; "" for every other set's def. A set
  registered through the C ABI without a signal file is among those:
  its rows show and get values, but no layout binds them, since its IDs
  are its own numbering (a copy of the mock's carries the mock's) and
  it has no keys. `SessionBridge::fileOfSet(set)` finds a set's file:
  the signal store, the plugin store, then the replay's own files.
- **A row's range** (`RangeLowRole` and `RangeHighRole`, `rangeLow` and
  `rangeHigh`): `ecuio_def_range`, NaN at both ends for any refusal.
  Both ends go through the display conversion, and the range is none
  (NaN at both ends) when either converts past a double (a deg C end
  near `DBL_MAX`, in deg F) or the converted ends meet. The roles are
  notified with `UnitRole`, so a change of units moves them.
- **`IsEnumRole`** (`isEnum`) marks an enumeration row, which section 3
  offers to `digital` alone.
- **`Bridge.profileSignals(uri)`** gives the same facts as the model's
  rows (section 2's fields, `id` and `isEnum`) for the set the URI's
  profile names, with no session, in the current unit system. It gives
  [] for a URI whose scheme or `profile=` core refuses, and for
  `uds-generic`, `raw` and an unknown name. A URI core refuses for
  another reason (an unknown key, too many segments) still gives its
  profile's rows. A replay's own `signals.json` is registered only when
  it connects, so its URI gives [] before then. The profile is
  `SessionBridge::profileOfUri`'s, core's choice at
  `ecuio_session_create`, pinned against the session's info for each
  URI core takes.
- **`Bridge.gaugeSignals`** gives `id:` reference lists. `SignalHub`
  keys its entries by reference (`byRef`; `lookup(refs)`, the first
  that matches), and a "" row has no entry. Its `Entry` adds `ref`,
  `hidden`, `refusedCode`, `label`, `isText`, `isEnum`, `rangeLow` and
  `rangeHigh`.
- **Tests:** `BridgeTest.SignalRoleNamesMatchQmlBindings`,
  `.ABuiltInSetsRowsAreReferencedByTheirIds`, `.ASetWithNoFileHasNoRefs`,
  `.ASignalFilesRowsAreReferencedByTheirKeys`,
  `.APluginVariantsRowsHaveRefs`, `.ARowsRangeIsShownInTheUnitSystem`,
  `.ARangeOfAnEndPastADoubleIsNoRange`,
  `.ProfileSignalsGivesTheProfilesRows`,
  `.ProfileSignalsOfAReplaysOwnFileFollowTheConnect` and
  `.ProfileOfUriIsTheProfileCoreChooses`; `QmlTest.dashboard`'s
  `test_hubFindsSignalsByRef`, `test_hubEntryCarriesTheRowsFacts`,
  `test_aTextEntryHasNoRange`, `test_aSetWithNoFileHasNoRefs` and
  `test_aGaugeBindsByRefNotByName`.

## Appendix: the rules as built (2026-11-07)

Section 10's step 3: `app/qml/DashboardBook.qml`, a `pragma Singleton`
(`QT_QML_SINGLETON_TYPE` in `app/CMakeLists.txt`).

- **Whose field it is decides what a bad one costs.** Section 7's two
  bullets divide this way. The document's own fields and bounds make
  the whole text damaged, and the built-in layout shows: a `format`
  that is missing, not a whole number or below 1 (above 1 is "newer"),
  `pages` not an array, more than 10 pages, more than 24 cells on a
  page, or more than 64 KiB (65,536 bytes of UTF-8). A page or cell
  that is dropped still counts toward those bounds. So does a document
  with no page left once the bad ones are dropped (`pages: []` among
  them), and one that is past 64 KiB once written back (`1e5` is
  written `100000`), since a read document is what the next write
  stores. A page's own fields drop that page: its name (text of 1 to 40
  code points once trimmed, kept as stored) and `cells` (an array). A
  cell's own fields drop that cell; the rest reads.
- **A cell's fields:** `kind` a non-empty string; `w` a whole number 1
  to 12 and `h` 1 to 16; `refs` an array of at most 8 strings (an
  empty one reads); `label` and `unit` strings where present, a label
  of at most 40 code points (a character outside the BMP counts once);
  `options` an object where present, with `from` below `to` when both
  are there. `label`, `unit` and `options` may be left out and read as
  "", "" and {} (`labelOf`, `unitOf`, `optionsOf`). Each option this
  version knows has one rule for every kind:

  | Options | Rule |
  |---|---|
  | `from`, `to`, `coldBelow`, `warnFrom`, `dangerFrom`, `rise` | a finite number |
  | `majorStep`, `minorStep`, `labelDivisor` | a finite number above 0 |
  | `decimals` | a whole number, 0 to 6 |
  | `count` | a whole number, 1 to 32 |
  | `cells` | a whole number, 2 to 64 |
  | `digits` | a whole number, 1 to 8 |
  | `window` | a whole number, 10 to 120 |
  | `peak`, `progress`, `vertical`, `marks` | a boolean (`marks`: the ticks on or off) |
  | `caption` | text of 0 to 40 code points |
  | `description` | text of 0 to 80 code points |
  | `peakLabel` | text of 0 to 8 code points |
  | `accent` | a string |

  The zones (`coldBelow`, `warnFrom`, `dangerFrom`) aren't checked
  against the range: one outside it doesn't draw.
- **Kept as read:** a kind outside section 1's eight (`knownKind`
  false, for section 3's empty card), a reference form, a key this
  version doesn't know in the document, a page, a cell or its options,
  and an `accent` other than `accent`, `info` and `success`
  (`knownAccent` false), which draws as the default accent. An `accent`
  that isn't a string is a wrong type and drops its cell.
- **Writes.** Each one copies the document, applies its change and
  checks the result with `documentProblem`, the reading rules made
  strict (every page and cell must pass, not only the survivors). A
  change that doesn't apply, a result that fails, or an input that
  fails returns a copy equal to the input, which is never changed. The
  result is the change as stored text reads it back: a key holding
  `undefined` is left out (of a change, and of a cell or options
  added), and nothing in it is shared with the write's arguments. A
  value stored text doesn't hold as it is (a function, NaN or an
  infinity, a list holding `undefined`, an object of a class such as a
  `Date`) refuses the write. So `parse(serialize(doc))` gives `doc` back
  after every write.
  `resizeCell` rounds the span and clamps it to 1 to 12 and 1 to 16
  (a span that isn't a finite number is refused). `setCell` changes
  only `label`, `refs` and `unit`, `null` removing `label` or `unit`;
  any other field refuses the write, so a cell of another kind is a new
  cell. `setOptions` merges its changes into the cell's options, `null`
  removing a key. `addPage` and `renamePage` store the name trimmed.
  `removePage` refuses the last page. `canAddPage`, `canRemovePage`,
  `canAddCell`, `cellProblem` and `pageNameProblem` give the controls
  their state and words. `saveTexts(problem, stored, doc)` gives the
  text to save and the one for `<form>Unread`: the stored text after a
  damaged or newer read, else "".
  Note (2026-11-08): `parse` also reports `"dropped"` when it read the
  text but left out a page or cell with a wrong field, with their counts
  in `dropped` (`{ pages, cells }`), and `saveTexts` keeps the stored
  text after that read too. The edit-mode appendix below says why.
- **The built-in layout** (`builtIn(form)`, also `reset(form)`): one
  page, "Engine", with the Dashboard's gauges in its order, each bound
  to the refs the bridge's `gaugeSignals` gives it, in the units core's
  sets decode to:

  | Cell | Kind | Phone w x h | Desktop w x h | Options |
  |---|---|---|---|---|
  | Shift lights | `shift-light` | 12 x 1 | 12 x 1 | 4500 to 6500, `count` 11 (phone) or 15 (desktop) |
  | Engine speed | `analog` | 1 x 4 | 2 x 8 | 0 to 8000, steps 1000 and 250, `labelDivisor` 1000, `dangerFrom` 6800, the caption, `peak`, `progress` |
  | Vehicle speed | `analog` | 1 x 4 | 2 x 8 | 0 to 260, steps 20 and 10, `peak` labeled "TOP" |
  | Coolant | `ring` | 1 x 4 | 1 x 4 | 0 to 130, zones 60, 105, 115 |
  | Oil temp | `ring` | 1 x 4 | 1 x 4 | 0 to 160, zones 70, 125, 140 |
  | IAT | `digital` | 1 x 4 | 1 x 4 | 0 to 120, zones 55 and 70, a description |
  | MAF | `digital` | 1 x 4 | 1 x 4 | 0 to 300, a description, accent `info` |
  | Throttle | `bar` | 1 x 2 | 2 x 2 | 0 to 100, accent `success` |
  | Engine load | `bar` | 1 x 2 | 2 x 2 | 0 to 100 |

  The phone's two columns put the dials side by side under the shift
  lights, the rings and tiles in pairs below them and the bars last.
  The desktop's 12 put the dials, rings, tiles and bars in one band.
- **Placement** (`place(cells, columns)`): in the cells' order, each at
  the first free block of its span, rows top to bottom and columns left
  to right, so a later small cell fills a gap an earlier wide one left.
  A span wider than the grid takes the whole row, and fewer than one
  column, or a count that isn't a finite number (a page not yet laid
  out), reads as one.
- **A new gauge's range** (`defaultRange(unit, lo, hi)`): section 6's
  table by unit (RPM 0 to 8000, km/h 0 to 260, deg C 0 to 150, % 0 to
  100, kPa 0 to 300, V 0 to 16, g/s 0 to 300, L/h 0 to 60), then the
  field's range when both ends are finite and `lo` is below `hi`, then
  0 to 100. Its unit and ends are the decoded unit's, as the stored
  options are.
- **Tests:** `QmlTest.dashboardbook` (`tests/qml/tst_dashboardbook.qml`,
  test_qml): each form's built-in layout against the table and
  `Bridge.gaugeSignals`; what a later version writes kept and written
  back; each wrong field dropping only its cell or page, table-driven,
  and each bound read at its edge and refused past it (pages, cells,
  refs, a label in astral code points, the spans, the bytes in
  multibyte text, the written-back size); the four problems and
  `saveTexts`; placement at 2, 4, 6 and 12 columns, the first free
  block, and no overlap and the order kept at every width; every write,
  every refusal returning an equal copy with the input unchanged, and
  every kind of write reading back equal.

## Appendix: an analog dial's ticks under another unit system (2026-11-07)

Settled before steps 3 and 4, for section 6. `majorStep`, `minorStep`
and `labelDivisor` are spacings in the unit shown, never converted. A
`from` or `to` converted into the other unit system snaps to the
nearest multiple of `majorStep`, where the kind has one: the built-in
speed dial's 260 km/h reads 160 mph with a tick every 20, as the
Dashboard's speed dial reads. A kind with no `majorStep` keeps the
converted end as it is. A value past an end pins the needle, arc or
bar at that end while the readout shows the value, as `GaugeBase`'s
clamp does.

## Appendix: the renderer as built (2026-11-08)

Section 10's step 4: `app/qml/DashboardPage.qml` draws a form's pages
from its document. Editing is step 6's.

- **The document** is read from `Settings`' `dashboards` category:
  `phone` and `desktop` hold each form's text, `phonePage` and
  `desktopPage` the page last shown on each, stored when it changes and
  restored when the page is made, clamped to the document's pages. The
  text goes through `DashboardBook.parse`. None shows the built-in
  layout. A damaged or newer text shows the built-in layout under a
  line saying which, with Dismiss, and the stored text is left as it
  is.
- **The grid:** the phone's 2 columns upright and 4 on its side; the
  desktop's one per 220 px of the page's width less its margins,
  rounded, between 2 and 12, with no cap on a column's width. A row is
  a quarter of a column plus the gap, or less where that would make the
  page's tallest cell taller than the area that shows the page (with a
  floor of 160 px, below which the page scrolls), so a short, wide
  window keeps a dial whole.
- **Moving between pages:** one `SwipeView`, swiped on the phone form
  only, with a `PageIndicator` that shows the page and isn't pressed.
  The desktop's page buttons sit under the header's line, one per page
  with its name ("Page N" for a page with none), the shown one
  highlighted, and none for a document of one page. Left and Right
  step back and forward, stopping at the ends, on the desktop form
  only, with more than one page, while the page is visible and no
  `TextInput` or `TextEdit` has the focus.
- **A cell** is a `GaugeCard` whose `Loader` makes its kind: `analog`,
  `ring`, `bar`, `digital` or `shift-light`. Any other kind (step 5's
  `segments`, `seven-segment` and `sparkline`, or one unknown) is an
  empty card saying this version doesn't have that gauge, and stays in
  the document.
- **A page not shown loads later:** its cells make their gauges
  asynchronously, between frames, and a page moved to finishes at once.
  So a full document (10 pages of 24 cells) draws its first frame about
  as soon as one page of 24 does: offscreen on the software renderer on
  the development desktop, under 1 s, against 11 to 19 s with every
  gauge made before the first frame.
- **The notes** are section 3's, first that applies. A row of another
  unit than the cell's (section 6) adds "(expected <unit>)" to them,
  and gives the gauge the row's unit, its range (0 to 100 when the row
  has none) and no zones. A gauge shows its note only while it has no
  value.
- **The scale:** the stored `from` and `to`, or `defaultRange`'s, in
  the current unit system. An analog dial's converted end snaps to its
  major step (the appendix on ticks); an end the conversion left as
  stored is kept. A `majorStep` that would draw more than 40 ticks over
  the span gives way to a round step (1, 2 or 5 times a power of ten)
  near an eighth of it, and a `minorStep` that would draw more than 200
  to half the major. `AnalogGauge` never counts fewer than no ticks,
  which a reused cell asks for while it rebinds its ends one at a time.
- **An enumeration's label:** `digital` shows the row's label for its
  value (`GaugeBase.valueLabel`), and the number where it has none.
- **The sweep and the peaks** reach every page: the once-a-session
  sweep plays on the analog, ring and shift-light kinds, not on a bar
  or a digital tile; Reset peaks clears every kind; a connect and a
  change of units reset the peaks.
- **Tests:** `QmlTest.dashboard` (`tests/qml/tst_dashboard.qml`):
  `test_theGridFollowsTheForm` (desktop windows of 500 to 3840 px and
  the phone both ways: the columns, each cell where `place` puts it, no
  overlap, a square tach), `test_aWholeDialFitsAShortPage`,
  `test_thePhonesDialsShareARow`, `test_theDesktopsPageButtonsAndKeys`,
  `test_thePhonesPagesSwipe`, `test_theLastPageShownIsKept`,
  `test_aStoredDocumentThatCantBeUsedSaysSo` (damaged and newer),
  `test_aKindThisVersionDoesntDrawIsAnEmptyCard`,
  `test_aCellOverAnotherUnitTakesTheRowsRange`,
  `test_anEnumerationShowsItsLabel`, `test_aValueThatStopsComingDims`,
  `test_theEcusAnswersAreTheNotes` (the fake DLL's ECU: a withheld PID
  and a refused DID), `test_theSweepAndThePeaksReachEveryPage` and
  `test_aFullDocumentsOtherPagesLoadLater`. `QmlTest.phone`'s
  `test_theDashboardsDialsShareARowUpright` (the phone form upright and
  on its side, in `Main`).

## Appendix: edit mode as built (2026-11-08)

Section 10's step 6: `app/qml/DashboardPage.qml`'s edit mode, over step
3's rules. Where it differs from section 5, this is what's built.

- **Saving:** each edit is stored in `Settings` as it's made (the
  page's `Settings` writes within half a second, and when the page
  goes), so **Done** only leaves edit mode and there is no Cancel. A
  closed app or a crash mid-edit loses nothing made before it. An edit
  that changes nothing (a write the rules refused gives an equal copy)
  stores nothing. The first edit or reset after a stored text that
  couldn't be used (damaged or newer), or was read only in part (a
  page or cell left out for a wrong field: `"dropped"`, with a line
  naming how many), keeps that text in `phoneUnread` or
  `desktopUnread` (`DashboardBook.saveTexts`), so an edit never makes
  a drop final. Any edit or reset hides the line about it until the
  stored text next changes.
- **Starting:** the header's **Edit** on both forms; on the phone a
  long press on a cell of the shown page also starts it, with that
  cell's panel open. A long press on the desktop does nothing.
- **The edit bar** holds three buttons on both forms: **Done**,
  **Add gauge** (off while the page is full) and **Pages**. On the
  phone they are compact, so the row fits a phone's width. **Pages**
  opens an in-page row under the bar, never a `Menu` (a `Popup`): the
  page's name, **Add page**, **Move page left**, **Move page right**,
  **Remove page** (never the last) and **Reset**. Remove page and
  Reset each replace the row with their question: "Remove the page
  ... and its gauges?" with **Remove** and **Keep**, and Reset's with
  **Reset** and **Keep**. Reset stores no text for the form, so a
  later version's built-in layout reaches it. A cell's remove button
  asks nothing.
- **One panel** at the top of the page's scroll area, full width on
  both forms, the grid below it: the cell's panel, Add's kinds and the
  signals take the same place. The cell being edited is outlined; a tap
  on another cell of the shown page switches the panel to it. A page
  change, and removing the page shown, close whatever panel is open.
  In edit mode every cell of the shown page shows a remove button at
  `Theme.touchTarget` in its corner, and the page ends with a "+" cell
  while it has room; the button and the outline are made only while
  editing (a `Loader` each), so a page that isn't edited builds
  neither.
- **Typing:** a field commits what was typed when it loses the focus.
  A tap on a cell and a swipe take no focus, so selecting a cell,
  Add, and a page change first clear the focused field's own focus,
  and the text goes to the cell or page it was typed for. Focusing the
  page wouldn't do: it's a focus scope, which gives the focus back to
  the field.
- **Add:** `drawnKinds`, the one list of the kinds the page draws, in
  the order Add offers them, each with a still preview (the real gauge
  at 60 of a 0 to 100 range, with no binding, sweep or peaks), then
  the signals through `SignalFilterBar`'s text field (its **Filter**
  button, whose options open a `Popup`, is hidden here). When the
  profile of the session has nothing to offer, the list says so; with
  no session, it says to connect or choose a profile. A dial, ring, bar or shift
  light takes a number; a digital tile also takes an enumeration;
  text rows and rows without a reference aren't offered. The rows are
  `Bridge.profileSignals` of the session's URI while connected, and of
  Settings' URI otherwise.
- **The decoded unit and range:** `profileSignals`' maps carry
  `decodedUnit`, `decodedLow` and `decodedHigh`, the def's own unit
  and range as core decodes them, the same in every unit system (NaN
  for no range). A new gauge stores those (section 6), not the
  converted `unit`, `rangeLow` and `rangeHigh`, so a gauge added
  under imperial stores km/h and deg C. Its span is a kind's default:
  1 x 4 for a dial, ring or digital tile, 2 x 2 for a bar, 12 x 1 for
  a shift light. Picking another signal for a cell sets its
  references, unit, label and range, and drops what belonged to the
  old signal: its zones, major and minor step, label divisor, caption,
  description, peak label and decimals. Its kind, span, peak hold and
  accent stay.
- **The cell's panel:** the signal, label, from, to, the three zones
  (an emptied field removes the zone), decimals, the span's `w` and
  `h`, peak hold (for the kinds in `peakKinds`, the dial alone, since
  only it draws peaks), the kind's own (an analog dial's major step,
  typed in the stored unit, and caption, a digital tile's description,
  a shift light's count), and
  **Move earlier**, **Move later**, **To the start**, **To the end**.
  The selection follows a moved cell, and a removal before it keeps
  it on its own cell. Decimals shows the row's own while the cell sets
  none.
- **Density on the phone:** every control in the bar, the Pages row,
  both questions, the three panels and the header's **Reset peaks**
  and **Edit** is at least `Theme.touchTarget` tall. The header's
  link state takes a line of its own under the title row, at the
  header's small size with no extra padding, and keeps its words
  beside the dot, so the state is never told by color alone.
- **A narrow header** (under 600 px, either form): Reset peaks and
  Edit are compact, and the row gives way by order, the desktop's
  link state first (it has no width of its own, only what is left)
  and the title last, eliding (`PageHeader.titleShrinks`). So the
  row fits a phone's width whatever the font's widths, with no width
  tuned to one font.
- **A tap on a peak-holding dial** in edit mode also clears its peaks:
  `AnalogGauge`'s own `TapHandler` takes the same tap as the cell's.
- **Tests:** `QmlTest.dashedit` (`tests/qml/tst_dashedit.qml`):
  `test_editAndDone`, `test_addTakesTheDecodedUnitAndRange` (metric and
  imperial), `test_thePickerOffersWhatTheKindCanShow`,
  `test_withNoSessionThePickerReadsTheLastUri`,
  `test_theCellsPanelSetsItsOptions`, `test_resizeMoveAndRemove`,
  `test_aFullPageTakesNoMore`, `test_thePagesControls` (Remove page's
  question), `test_removingThePageShownClosesItsPanel`,
  `test_aPageChangeClosesAnyPanel`,
  `test_typedTextStaysWithItsCellAndPage` (keys typed, then a tap or
  a swipe), `test_resetAsksFirst`, `test_resetClosesThePanel`,
  `test_resetKeepsATextThatCantBeRead`,
  `test_doneClosesTheRowAndTheQuestions`,
  `test_peakHoldOnlyWhereItDraws`,
  `test_theDecimalsStepperStartsAtTheRows`,
  `test_aProfileWithNothingToPickSaysSo`,
  `test_theMajorStepIsNotConverted`,
  `test_aLayoutBuiltInEditModeSurvivesARestart`
  (a layout built through the controls, the page destroyed and a new
  one made: the same document, on the page it was left on),
  `test_theFirstEditKeepsATextThatCantBeRead` (damaged, newer and
  dropped) and `test_aLongPressOnThePhoneStartsEditing`.
  `QmlTest.dashboardbook` checks `"dropped"` and its counts. `QmlTest.phone`: the edit
  controls in `Main` at the phone widths of its edge check (no item
  past the window's edge), `test_thePhonesEditControlsAreTouchTargets`
  and `test_thePhonesLinkStateTakesACompactLine`.
  `BridgeTest.ARowsRangeIsShownInTheUnitSystem` checks the three
  decoded keys under both unit systems.

## Appendix: the edit-mode mark for unfilled cells (2026-11-08)

Section 3 says a cell in state 2 or 3 carries a mark in edit mode. The
page does not draw one: the cell's note ("not supported by this car",
"refused (7F 22 31)") shows whether or not the page is being edited, so a
user tidying a page sees which gauges the vehicle can't fill without it.
The mark stays out of the first release; a cell with no value is shown
by its note alone.

## Appendix: the new kinds and the theme roles as built (2026-11-08)

Steps 5 and 7. Where this contradicts an earlier appendix's account of
a kind, this is the present state; the earlier text is kept as written.

- **`segments`** (`SegmentGauge`, the Cells tile): `cells` (2 to 64,
  default 24) Rectangles along a rising curve, each taller than the one
  before; `rise` (0 to 4, default 1.6) sets the curve's steepness and 0
  gives one height. Cells are lit up to the value, each in its zone's
  color, with the danger cells outlined and a notch over the cell the
  held peak reached.
- **`seven-segment`** (`SegmentReadout`, the Digits tile): `digits` (1
  to 8, default 4). Each digit is seven Rectangles whose opacity
  follows the digit (the decision's "Shape paths" became
  Rectangles), right aligned, with the decimal point and a
  negative's middle bar. A value too long for its digits shows dashes in
  every digit, never a wrong number; no value blanks them. The digits
  take the zones' colors.
- **`sparkline`** (`SparkGauge`): one `ShapePath` trace over `window`
  (10 to 120 s, default 30), the newest value at its end and the
  window's own low and high as its scale, padded by 4 % of full scale.
  It samples only while its page is the one shown and the app is active
  (neither Suspended nor Hidden).
- **The bar** takes `vertical` (segments stacked from the bottom, the
  scale beside) and `marks` (the scale's ticks, on by default).
- **`peak` on every kind that has a scale:** `showPeak` is
  `GaugeBase`'s, off by default. The dial keeps its tell-tale, the bar
  a mark across it, the ring a mark on its zone strip and the cells a
  notch. The digital tile keeps its MIN and MAX footer, and the
  seven-segment readout and the sparkline have no scale to mark.
  `DashboardPage.peakKinds` names the four kinds that draw one, and the
  cell's panel offers Peak hold only there. The once-a-session sweep
  plays on the dial, the ring, the cells and the shift lights.
- **Every new kind keeps the danger mark's slot** (opacity, never
  visibility), so a readout never moves as the mark appears, and is
  tested at two sizes in each theme (`QmlTest.kinds`).
- **The panel and Add:** the cell's panel has rows for each kind's own
  options (Cells and Curve, Digits, Window, Standing up, Scale marks),
  hidden on the kinds without them. A new gauge's span is
  `DashboardPage.newSpans`: Cells 2 x 3, Digits 1 x 2, Sparkline 2 x 3.
  The Add tile of a sparkline draws a made-up trace with its sampling
  off.
- **The theme roles** (section 9): `gaugeFace`, `gaugeValue`,
  `gaugeNeedle`, `gaugeTick`, `gaugeWarn` and `gaugeDanger`, and two
  more: `gaugeReadout` (the readout's own color, which in Dark stays
  near-white where `gaugeValue` is the arcs' lilac) and `gaugeShade`
  (the dial's bezel, face, needle and hub shades). Every gauge takes
  its colors from them, with no literals left. The themes are Dark, Low
  glare (amber and orange on near black) and Night (red on near black),
  chosen under Settings, Display and kept in the `ui` settings; a name
  that isn't a theme reads as Dark. In Low glare and Night the danger
  color is the theme's red or orange, so a danger is told by more than
  hue: a mark beside the readout and a heavier zone stroke, in every
  theme.

## Appendix: section 9 amended (2026-11-08)

Amended 2026-11-08: the themes section's "low-glare and night themes" are
the night styles of [ADR-0058](0058-night-mode.md): Night is a mode (off,
on, or on a schedule) beside the look, and its style is amber or red. The
palettes are `dark`, `nightAmber` and `nightRed`, and in the two night
palettes `gaugeReadout` is the text color and no chart trace is brighter
than the text. The gauge roles of section 9 are unchanged.

## Appendix: six columns, the panel, one gesture, copy and paste (2026-11-08)

Sections 1 and 2 of this appendix are built. Sections 3 to 5 are decided
and not built.

### 1. Six columns upright, twelve on the side

Section 4 gives the phone 2 columns upright and 4 on its side. The
decided layout puts an RPM dial at two thirds of the width with three
readouts stacked beside it, and three readouts across a row; neither is
a half or a whole. The phone form moves to **6 columns upright and 12
on the side**. The row unit stays a quarter of the column width, so a
column is a third as wide as before and a row a third as tall, and the
same shape on screen needs `w` and `h` three times larger. The bound on
`h` rises from 16 to **48**; `w` stays 1 to 12 (12 is the whole row on
the side, and anything 6 or more is the whole row upright, as section 4
already says of a span wider than the grid). The built-in phone layout
(`DashboardBook.builtIn("phone")`) is written with the new spans
directly: the shift lights at 12 x 3, the dial at 4 x 18 with three
digital tiles at 2 x 6 stacked beside it, a digital trace across the
grid at 6 x 8, and three digital tiles at 2 x 7 across. A digital tile
too short for its trace and footer (under 84 px of gauge) draws its
label and value alone. The desktop form is unchanged: its columns
follow the width (2 to 12), its documents do not move, and its height
control stays 1 to 16.

### 2. The migration of a stored layout

v0.7.0 ships format 1 with phone layouts on 2 and 4 columns. A user
who edited theirs has a `dashboards/phone` document whose spans mean
half and whole widths; read on 6 columns unchanged, every gauge would
shrink to a third. So:

- **Format 2.** `format` becomes 2. The only difference from 1 is the
  meaning of a phone document's spans and the `h` bound of 48. The
  desktop document's spans mean the same in 1 and 2.
- **When it runs.** In `DashboardBook.parse(text, form)`, which already
  has the form (`"phone"` or `"desktop"`). A format 1 document is first
  checked against **format 1's bounds** (`w` 1 to 12, `h` 1 to 16), with
  a bad page or cell dropped exactly as v0.7.0 drops it; only then is it
  upgraded. So `cellProblem` takes the bound from the text's format: a
  format 1 `h` of 17 stays invalid on either form, and a format 1 `w` of
  13 is dropped, not clamped. A format 1 phone document is then upgraded
  in memory: every cell's `w` and `h` are multiplied by 3, `w` clamped
  to 12. A format 1 desktop document is upgraded by setting `format`
  alone, before `documentProblem`, which compares it. Nothing is written
  at read time: as with the built-in layout, the stored text changes on
  the user's first edit, when `serialize` writes format 2. Until then a
  user who goes back to v0.7.0 finds their format 1 text as it was.
- **After the first edit,** v0.7.0 reads format 2 as "made by a newer
  version of Havne" and shows the built-in layout, leaving the text
  alone, by section 7's existing rule (a format 2 text past 64 KiB reads
  there as damaged instead, with the same result). Nothing is lost in
  either direction.
- **A span that does not scale cleanly.** With a factor of 3 on
  integers there is none; the one edge is the bound. `h` x 3 is at most
  48, the new bound. `w` x 3 above 12 is clamped to 12, and the clamp
  changes nothing on screen: upright, a format 1 `w` of 2 or more was
  the whole row and becomes 6 or more, still the whole row; on the
  side, a `w` of 4 or more was the whole row and becomes 12, the whole
  row. So the upgrade is exact, and at any gap between cells: a row's
  pitch is (column + gap) / 4 and a cell's height is `h` x pitch - gap,
  one gap whatever its rows, and three times the columns makes column +
  gap exactly a third, so three times the rows at a third of the pitch
  is the same height. The cap on the pitch, (shown height + gap) /
  tallest `h`, scales the same way.
- **`phonePage` and `desktopPage`** (the page last shown) are not part
  of the document and need nothing.
- **The two forms differ.** Each document has its own `format`, so a
  phone document at 1 and a desktop document at 2 read correctly side
  by side, and a device that has only one of the two (a phone never
  used as a desktop) upgrades the one it has. The form is the key the
  text came from, never guessed from the content.
- **The 64 KiB bound.** Scaling can add up to two bytes per cell, so a
  format 1 document just under the limit could pass the read's check
  and fail `documentProblem` as written back. The upgrade must not turn
  a readable document into "damaged": format 1 keeps its 64 KiB
  (`format1MaxBytes`), and format 2's bound, `maxBytes`, is that plus
  two bytes for each of the most cells a document can hold, which the
  first edit's write follows too.
- **Spans written in the code.** Everything that names a phone span
  moves to the new grid with the form known: the built-in layout, the
  bar's vertical switch (2 x 2 and 1 x 4 become 6 x 6 and 3 x 12 on the
  phone), the add placeholder and a new cell's default span. The
  desktop's stay as they are.
- **The size control.** The phone's height control steps by 3 (one old
  row), not by 1 up to 48; `w` steps by 1. A stored odd `h` still reads
  and displays; a step moves to the next multiple of 3.
- **Idempotence.** A format 2 document is never multiplied again; a
  format 1 document read twice (two launches before the first edit)
  gives the same result each time, since the stored text has not
  changed.
- **Unknown cells and keys** (section 7's "within format 1" rule) are
  kept and, if they are cells, scaled like the rest: their `w` and `h`
  are known fields even when their kind is not.

**The tests** are in `tst_dashboardbook.qml`, the last in
`tst_dashedit.qml`:

1. The sample document of section 7 as a format 1 phone text: after
   `parse`, `format` is 2, the shift lights are `w` 12 `h` 3, the dial
   `w` 6 `h` 24, the digital `w` 3 `h` 12; `place` on 6 columns puts
   them in the same relative positions as `place` on 2 columns put the
   original.
2. The same text as a desktop document: `format` 2, spans unchanged.
3. A format 1 phone cell with `w` 5 (whole row on 4 columns) becomes
   `w` 12 and is still the whole row on 12 columns; `h` 16 becomes 48
   and reads.
4. A format 2 phone text through `parse` is unchanged.
5. `serialize(parse(format1))` writes `"format": 2` and the scaled
   spans; `parse` of that gives the same document (round trip).
6. An unknown kind's cell keeps its keys and scales.
7. A format 3 text shows the built-in layout and the "newer version"
   notice, as before.
8. A format 1 cell with `h` 17 is dropped, phone and desktop; with `w`
   13 is dropped, not clamped; the dropped count is reported as in
   v0.7.0. A format 2 cell with `h` 48 reads, with `h` 49 is dropped.
9. A format 1 phone document at 64 KiB, whose upgrade passes that
   limit, reads with no problem and survives its first edit.
10. On the real page at its own gap, a stored format 1 phone layout
    gives every cell the box the 2 and 4 column grid gave it, and the
    store changes only on the first edit.
11. The bar's vertical switch on a format 2 phone cell at 6 x 6 and at
    3 x 12; on a desktop cell at 2 x 2 and 1 x 4.

### 3. The panel chrome

Cells on the phone meet at hairlines on one flat surface: the grid's
spacing is 0 and it draws a 1 dp line (`Theme.hairline`, the outline at
55 % over surface) between neighbors and around the panel, with
radius 6 on the panel; `GaugeCard` paints no gradient, no border and no
radius of its own. Hierarchy comes from the spans. The desktop keeps
its cards in v0.7.1 and takes this design in the release after it.

### 4. One gesture on a gauge

Press and hold on a gauge (the platform's long-press time,
`styleHints.mousePressAndHoldInterval`), in or out of edit mode, except
in the drive view, where the long press does nothing. Then:

- **release** opens the cell's menu (section 5's panel with Copy and
  Paste after added), in the page, in its scroll area;
- **move** drags the cell; the drop is a change of order (section 4),
  and placement reflows the page as the drag passes other cells.

It yields to the page swipe and to scrolling by time: a finger that
moves before the hold completes is a swipe or a scroll and the hold is
canceled; once the hold completes (a short haptic, the cell lifts) the
`SwipeView` and the page's `Flickable` are made non-interactive until
release, and a drag near the top or bottom edge scrolls the page
itself. Nothing in the two gestures' timing collides, since both
alternatives need motion before the hold and the hold needs stillness.
The Edit button's mode remains for Add, Remove, the pages' controls and
Reset to the built-in layout, and in it the same gesture works the same
way. Drag-to-reorder is v0.7.1's, as section 5 already says.

Added 2026-11-09. Qt has no haptic from QML without native code, so
the hold shows only the cell lifting.

### 5. Copy and paste

**Copy** takes the whole cell: kind, refs, unit, label, options and
span, into one in-app slot that lives for the app's run (not the system
clipboard: a dashboard cell is not text a user pastes elsewhere, and
the slot survives a page swipe without a serialization step). **Paste
after**, in a cell's menu, inserts a copy after that cell; **Paste**, in
the edit bar, appends it to the page shown. A paste onto a full page
(24 cells) is refused with the page's existing message. The pasted cell
is independent of its source. `DashboardBook` gains `copyCell(doc,
page, i)` returning the cell and `pasteCell(doc, page, i, cell)`, both
pure, with the bound check; the slot is page state. The slot is per
form, and Paste is disabled when it holds the other form's cell, whose
spans are meant for the other grid.

Added 2026-11-09. As built it is one slot, `{form, cell}`, for the
app's run: a copy on either form replaces it, and Paste is disabled
while it holds the other form's cell.

### 6. A blank cell

Added 2026-11-08. A new kind, `blank`, appended to
`DashboardBook.kinds` and offered in Add: sized and placed like any
gauge, needing no signal (its `refs` are empty), drawing nothing
outside edit mode and an outlined empty cell in it. It holds a spot
open, for instance beside a floating overlay app, so placement doesn't
pack the next gauges into it. Its panel has the size, the order, Copy
and Paste after. Outside edit mode it has nothing to hold, so section
4's gesture starts on it only in edit mode.

## Appendix: the phone's panel, the gauges under the S2 look and the fit, as built (2026-11-09)

The appendix "six columns, the panel, one gesture, copy and paste" says
its sections 3 to 5 are decided and not built. All six of its sections
are built: section 4's hold without a haptic and section 5's one slot,
as their notes say.

**The panel (section 3).** On the phone form the cells meet: the grid's
gap is 0, and the grid draws one `Theme.surface` panel with a
`Theme.hairline` edge and radius `Theme.radiusCard` (6), and a hairline
between neighboring cells. `DashboardBook.seams(placed, boxes, columns)`
gives the lines: between two different cells with at least one drawn,
none past a blank or an empty square, one line along a boundary. A cell
draws no fill, border or radius of its own; a held cell fills with
`surface`, so it covers what it crosses. A hairline is a whole number of
the display's pixels. The desktop keeps a card for each gauge. In the
drive view ([ADR-0059](0059-drive-view.md)) the panel reaches the
screen's edges with the same lines.

**No empty strips.** Outside edit mode and while no cell is held, a
gauge's drawn height runs down over the empty squares under it, as far
as every column it spans stays empty, within the panel's rows, and a
gauge alone in its rows runs right to the panel's edge
(`DashboardBook.stretched(placed, boxes, columns)`). A blank cell
doesn't stretch, and nothing stretches over one. The stored document
keeps its `w` and `h`; edit mode and a held cell show the true sizes.

**The built-in phone page's heights** are multiples of
`phoneSpanScale` (3), the Height control's step: the shift lights 12 x 3,
the dial 4 x 18 with three digital tiles at 2 x 6 beside it, the trace
6 x 9, and three digital tiles at 2 x 6 across, so the page keeps its 36
rows. Saved layouts don't change.

**Digits.** A Digits card stacks when that draws its digits at least a
fifth larger: the digits take the card's width, centered, with the unit
centered under them; a short card keeps the unit beside, as section 1's
table has it. With its `digits` option unset, a card's count comes from
its range (`DashboardBook.digitsFor(from, to, decimals)`): what a value
10 % past either end needs at the card's decimals, a minus counting as a
digit, from 3 to 8, and 4 without a finite range, so a brief overshoot
still draws. The option, 1 to 8, still wins. The appendix "the new kinds
and the theme roles as built" gives the option a default of 4; that
holds only without a finite range. A card's decimals come from the
signal once a session runs, so an unset card's count can change by one
at connect.

**The gauges' drawing.** The appendix "the new kinds and the theme roles
as built" lists `gaugeFace` and `gaugeShade`, and section 9 lists
`gaugeFace`. Neither role is in `Theme`. A dial has no bezel and no
face: its ticks, zones and needle draw on what is behind them, a card on
the desktop and the panel on the phone. Its drawing grows by up to 6 %
into the margin the bezel held, and less on a small dial, so the
progress arc's glow stays inside the item at every size. The needle is
one flat tapered shape in `gaugeNeedle` with no shadow, and the hub is
`elevatedLight` with a `gaugeNeedle` pin. A bar's and the cells' lit
segments are one flat color. The Digital and Sparkline traces are the
line alone, with no fill under it. The danger mark's point is `surface`.

**The card and the type.** A desktop card is flat: `Theme.surface`, a
`Theme.hairline` border (the outline at 55 % over the surface) and
radius `Theme.radiusCard` (6); controls take `Theme.radius` (4). The
type is Barlow, bundled with its license (`THIRD_PARTY_NOTICES`), and
readings at 17 sp and above take Barlow Semi Condensed Bold with
fixed-width digits (`Theme.valueFamily`, `Theme.valueFeatures`). A
bundled face draws the same on every platform, and Barlow gives the app
an automotive feel.
