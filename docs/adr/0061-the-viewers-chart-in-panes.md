# ADR-0061: The viewer's chart in panes, with two cursors and the span's statistics, and Live's pause

- **Status:** Accepted; built in v0.7.3. The frame figures on the laptop and the test phone are
  owed (see the first appendix).
- **Date:** 2026-11-09.
- **Builds on:** [ADR-0049](0049-recordings-in-core.md) (the reader, its
  per-second summaries and raw-row cache; the viewer and its frame test);
  [ADR-0056](0056-analysis-log.md) (the analysis log; section 6 names the
  statistics as the analyst's first reach); [ADR-0031](0031-command-line-client.md)
  (`havne-cli view`'s lines); [ADR-0048](0048-semantic-versioning.md)
  (a PATCH may carry follow-up work on its MINOR's theme and never breaks
  the C ABI, `havne-cli`'s schema or a plugin format); [ADR-0022](0022-display-units.md)
  (display units); [ADR-0053](0053-python-bindings.md) (the bindings
  wrap each reader call).
- **Amends:** ADR-0049 section 5's memory bound (32 bytes per
  number signal per second becomes 40: the second's sum joins its lowest,
  highest, last and count), with a dated note there (made); ADR-0049's viewer
  appendix gains a note that the chart is in panes with two cursors.

## Context

The viewer draws every plotted trace on one plot, each on its own value
scale, with the value labels at the right end as the only numbers
([ADR-0049](0049-recordings-in-core.md), its viewer appendix). That reads
for a glance and fails for analysis: two traces in one unit (throttle and
load, coolant and intake air) sit on unrelated scales, a trace has no axis
to read a level off, and a cursor gives one moment's values and nothing
about a stretch. The analyst's own tools lay traces out in stacked panes
on one time axis, each pane with an axis in its unit, and read a span
between two cursors: its length, and each signal's change, lowest, highest
and mean. ADR-0056 section 6 named this as what an analyst reaches for
first in the log and left it to an ADR of its own.

Two facts shape the design. The reader answers a column's lowest, highest
and count from per-second summaries, zoomed in from raw rows, with memory
bounded by the recording's seconds; a span's statistics should cost the
same. And a recording's front ends are the app on four platforms and
`havne-cli`, so what the viewer computes the CLI must print.

## Decision

### 1. Panes, by unit

Every plotted trace is drawn in the pane of its unit. Units that are one
quantity share a pane (`°C` with `°F`, `kPa` with `psi`, `km/h` with
`mph`); every other unit is a pane of its own. Panes stack under one time
axis in the order their first trace has in the signal list. Each pane has
a y-axis at its left in its unit: 2 to 4 round ticks, the unit at the
top, drawn by the chart's Canvas as the time captions are. A pane's range
is the lowest and highest of its traces over the part in view, padded 5 %,
so traces of one unit share one scale and each pane is filled.

At most 4 panes (3 on a phone upright at the chart's default height; 4
when the chart has the page), and never under 72 px each: past either
limit the lowest-ranked units fold into one mixed pane, drawn as the whole
chart was before panes (each trace on its own scale, no y-axis) and named "mixed"
at its axis. The value labels at the right end stay in every pane: they
name each trace's value at the cursor, which an axis doesn't.

A pane's range is the user's once changed: `Ctrl`+wheel over a pane
scales it about the pointer and a drag on its y-axis pans it on the
desktop; a vertical pinch inside a pane scales it on the phone; a
double-tap on the axis (a 48 dp strip on the phone) returns it to
automatic. The pane heights are equal shares until a seam between two
panes is dragged; the shares are kept per pane count between runs, a
double-tap on a seam equalizes them. A manual range lasts while the
recording is open.

Markers, the frames check's findings and the stretches (ADR-0056's
appendix) span every pane. A compared recording's dashed traces go to
their namesakes' panes. Live's chart keeps one pane: panes are the
viewer's, and `ChartLayout.layout`'s `panes` option is one Live doesn't
pass.

### 2. Two cursors

A tap places cursor A at the nearest sample of the first plotted signal,
as today. A long press on the phone, or `Shift`+click on the desktop,
places cursor B; a `Shift`+drag on the desktop and a long-press-drag on the
phone draw the span as a box and set both at its ends. Each cursor has a
handle in the axis strip at the plot's foot (48 dp wide on the phone,
where the strip grows from 16 to 24 px while a cursor exists; 24 px on the
desktop) showing its time; dragging a handle moves its cursor, snapped to a
sample at release; dragging B onto A, or Clear in the span line, removes
B. A tap while B exists moves A and keeps B. The span is between the two
whichever was placed first; the earlier reads as A.

`Left` and `Right` step A to the previous or next sample of the snap's
signal; `Home` and `End` go to the recording's ends; `B` makes the arrows
move B until pressed again. (`Shift+Left` and `Shift+Right` are ADR-0056's
stretch jump.)

### 3. The span's statistics, computed in core

```c
typedef struct ecuio_recording_stats {
    uint64_t samples;     /* in [t0_us, t1_us] */
    double first;         /* the value in force at t0_us (the last at or
                             before it); NaN with none */
    double last;          /* the value in force at t1_us */
    double lowest;        /* of the samples in the span; NaN with none */
    double highest;
    uint64_t lowest_us;   /* the first sample at each extreme;
                             ECUIO_RECORDING_NONE with none */
    uint64_t highest_us;
    double mean;          /* the samples' arithmetic mean; NaN with none */
    uint32_t flags;       /* zero */
    uint32_t reserved0;   /* zero */
    uint64_t reserved[3]; /* zero; ABI growth space */
} ecuio_recording_stats_t;

/* Fills one entry per signal in signal_ids over [t0_us, t1_us]. A text
 * signal's entry has samples alone, its numbers NaN and times NONE.
 * ECUIO_ERR_INVALID_ARGUMENT for t0_us >= t1_us, a count of 0 or an index
 * past the signals; ECUIO_ERR_RECORDING_CHANGED as for a query. */
ecuio_status_t ecuio_recording_stats(const ecuio_recording_t* r,
                                     const uint32_t* signal_ids,
                                     uint32_t signal_count,
                                     uint64_t t0_us, uint64_t t1_us,
                                     ecuio_recording_stats_t* out);

/* The first sample strictly after t_us (ECUIO_RECORDING_STEP_FORWARD) or
 * before it (_BACKWARD): *at_us, or ECUIO_RECORDING_NONE at the ends. */
ecuio_status_t ecuio_recording_step(const ecuio_recording_t* r,
                                    uint32_t signal, uint64_t t_us,
                                    uint32_t direction, uint64_t* at_us);
```

- **Change** is `last - first` and **rate** is the change over the span's
  length: two final values' difference and quotient, which the front ends
  form, as they form a time's text. Everything that reads the file is
  core's.
- **The mean is the samples' arithmetic mean,** because that is what a
  spreadsheet's AVERAGE over the wide CSV's column gives for the same span,
  so the viewer and the analyst's sheet agree. An OBD signal's samples come
  at a near-steady interval, so a time-weighted mean would differ little;
  `flags` leaves room for one if a log shows the difference matters.
- **Cost.** The per-second summary gains the second's sum, so a span's
  count, lowest, highest and sum over its whole seconds are read from the
  summaries and only its two partial seconds read raw rows: a span of an
  hour is 3,600 summary reads per signal. The extremes' times read the one
  second each is in (the summary says which) through the raw-row cache.
  `first` and `last` are `ecuio_recording_text_at`'s rule. The memory
  bound of ADR-0049 section 5 becomes 40 bytes per number signal per second
  (a 50-signal hour about 7.2 MB beside the cache); its test moves with it.
- **Units.** Core gives the values as `signals.csv` holds them; the app
  converts through `ecuio_display_convert` as it does the cursor's values,
  and `havne-cli` prints them as recorded, as its `at` line does.

### 4. The readout

- **Rows:** each signal row shows A's value as today; with B placed, the
  phone's row shows "B <value> · Δ <change>" as a second caption line (the
  row stays 48 dp), the desktop's row two more mono columns, B and the change.
- **The span line** under the zoom row (phone) or at its right (desktop):
  "A 12.4 s to B 20.6 s · 8.2 s", with Zoom to span and Clear.
- **The statistics** under the rows while B exists, one row per plotted
  signal: Signal, Change, Low, High, Mean, Rate on the desktop, in display
  units with the signal's decimals, tabular digits; the phone's 2-column
  grid of 48 dp cards with the change on the first line and "low · high ·
  mean" on the second, Rate left out. The chart maximized, the cursor strip
  shows A, B and the change per cell. The lowest and highest get a dot on the trace
  at their times while they're in view, hollow for the low, filled for the
  high.
- With A placed and no B, the span line says once "Hold to place the
  second cursor" (the desktop: "Shift+click"), as the drive view hints.

### 5. Front ends

- **The app:** sections 1, 2 and 4, on `RecordingView`'s worker for the
  statistics and the step, so the GUI thread never reads the file.
- **`havne-cli view <recording>... --span A:B`** (seconds, as `--at`;
  may repeat) adds one `stats` line per shown signal per recording placed
  on the axis: `{type: "stats", recording, signal, from_us, to_us, samples,
  first, last, lowest, lowest_us, highest, highest_us, mean}`, the values
  as `signals.csv` holds them and null where NaN or NONE; a span not inside
  the axis is usage (exit 2). Panes and cursors have no CLI side: `view`
  already prints every signal's columns and the values at a time, which is
  what a pane and a cursor show. Schema 1 grows by a line type and a flag,
  appended (ADR-0031).
- **Python:** `Recording.stats(signals, t0_us, t1_us)` (a named tuple per
  signal) and `Recording.step(signal, t_us, forward=True)`.

### 6. Live's pause

Live's chart follows now over a window of 10, 30, 60 or 120 s, drawn from
the signal model's history (`HistoryCapacity`, 2,400 samples per signal:
a window's lowest, highest and newest, so 120 s at 20 Hz or less). A
spike seen a moment late is gone from the window before it can be read.

- **The pause takes a snapshot.** Pausing copies each plotted signal's
  history, as `chartData()` gives it, into the page (2,400 samples of 16
  bytes per signal: 40 KB for ten), its times counted back from the newest
  sample, and the chart draws from that copy: it holds still for as long as
  the user wants it to. The session, the Dashboard, any recording and
  Live's rows go on, since the rows are what the page is for; only the
  chart stops. A pan moves the view's end within the snapshot (never past
  the pause; the window inside the snapshot, or at its start when the
  snapshot is shorter); a pinch or the wheel steps the window as it does
  live, about the view's end. The axis counts seconds before the pause
  ("−12 s", the pause itself "paused"), and the status line reads
  "Paused · 24 s ago".
- **The gesture.** A drag on the chart pauses it, as pulling a terminal's
  scrollback back does, and pans. While paused, a "Live" button (primary)
  takes the window row's right end; it, `Space` on a desktop (while no
  field has focus: a field types the space) and a drag pushed past the
  snapshot's right edge each follow now again. There is no Pause button:
  the drag is the pause, and `Space` is the keyboard's. On a phone the row
  makes room for Live: the "Window" label gives way and the window buttons
  show their numbers alone, every button a touch's height and Live a
  touch's width.
- **The cursor.** A tap on the paused chart places a cursor: each trace's
  label reads its sample nearest the cursor (none further than the stale
  age, past which a trace is broken anyway), a dot marks that sample, and
  the status line adds the cursor's time before the pause. One cursor: the
  span between two, and its statistics, belong to a recording in the viewer
  (sections 2 and 3).
- **What drops it.** Live, a disconnect, a unit change (the snapshot's
  values are in the units of its time), a new signal set (a profile or a
  signal file) and leaving the page. Nothing is interpolated between the
  snapshot and now: resuming shows the current window.
- **The hint.** The first pause of all says once, under the status line,
  "Start log records the session for the viewer": a paused chart is not a
  recording, and Start log (ADR-0056) is the way to keep one.
- **No core change.** `SignalModel::chartData(windowMs, columns)` already
  gives the samples; the snapshot is its result kept, thinned per pixel
  column at paint time as the live window is.
- **The CLI has no equivalent.** `havne-cli stream` writes lines, and the
  terminal's scrollback is its pause; a recording is `log` or
  `stream --record`.

## Alternatives considered

- **One plot with several y-axes.** Two scales on one plot are what every
  chart guide warns against; the roadmap asked for panes "in place of" it.
- **Panes the user assigns** (a pane number per row, kept per recording).
  It answers "rpm with speed" and is how the analyst's tools work; most of
  its value is in grouping by unit, and it needs a store per recording and
  a control in a 48 dp row that is full. The layout takes an assignment
  map as an input from the start, so it is a UI change if asked for.
- **Two cursors always, at the view's edges,** the statistics the view's
  until the cursors move. Fewer gestures, but a cursor at an edge is at no
  sample (the rows' values and the labels need one), a pan is a core call
  per frame, and a handle at the edge has nowhere to sit.
- **The mean from raw rows alone,** no change to the summary. Simpler, and
  a span of an hour at 10 Hz reads 36,000 rows per signal: tens of
  milliseconds on a worker, and the memory bound stays 32. Rejected for the
  phone, where every statistics change would wait on it.
- **A time-weighted mean.** Right for a held signal sampled unevenly;
  disagrees with the spreadsheet an analyst checks against. A flag for
  later.
- **Standard deviation, percentiles.** Not asked for; a struct with room
  can take them.
- **Writing a span to `markers.csv`.** Useful for other tools; it is
  v0.8.0's marker writing and stays there.

- **Live's pause on the live ring** (the window's end frozen, the ring
  turning). No copy; but after two minutes paused the sample under the
  user's eye has left the ring and the view empties from the left.
- **A pause that records** (a recorder attached at the pause, the viewer
  opened on it). A folder for every look back; ADR-0056's Start log is that
  already.
- **A Pause button in the window row.** A phone's row has no room for it
  beside Live, and a drag already says "look back".

## Consequences

- Three append-only ABI additions (`ecuio_recording_stats_t`,
  `ecuio_recording_stats`, `ecuio_recording_step`), a line type and a flag
  in `view`, two Python methods; nothing existing changes shape (a PATCH
  under ADR-0048).
- The reader's per-second memory grows by a fourth; the bound is restated
  and tested.
- The chart layout grows a pane model, pure and tested in
  `QmlTest.chartlayout`; the frame test (ADR-0049's viewer appendix) is run
  again with four panes on the dense hour on the laptop and the test phone,
  and the figures recorded in an appendix; a miss cuts the phone's pane
  count before anything else.
- The viewer's gestures grow by a long press, a `Shift`+click or drag, two
  handles, a vertical pinch and a seam drag; every one at `Theme.touchTarget`
  on the phone, none on the bottom inset.

## Done when

- `RecordingReaderTest`: the statistics of each fixture signal over a span
  equal a pass over its raw rows; the summaries' sums give the same mean as
  a raw pass on `bench_recording`'s dense hour; a span inside one second,
  across two, and over the whole recording; a text signal; the extremes'
  times; `first` before the first sample is NaN; the statuses; the bound
  at 40.
- `ecuio_recording_step` at the ends, between samples and at a sample.
- `QmlTest.chartlayout`: the unit groups, the limit, the fold under 72 px,
  the manual range, the seam shares, the axis ticks' rounding.
- `QmlTest.recordings`: two cursors by tap and long press, the span line,
  the rows' B and change, the statistics against `RecordingView.stats()`, the
  keys, Clear, a jump's cursors (ADR-0056's appendix).
- `QmlTest.phone`: the handles, the span line and the statistics grid at
  384 and 320 px; every target 48 dp.
- `CliGoldenTest.ViewSpan*` on the pull fixtures; `cli_view.py` checks a
  `stats` line's mean against the `columns` lines' samples; a `--span`
  outside the axis is usage.
- `PythonTest`: `stats` and `step` against the fixtures.
- `test_c_abi` compiles the header as strict C; `ctypes_gen.py --check`
  passes.
- `QmlTest.liveview`: the snapshot's first and last sample times against
  `chartData()` at the pause, the chart holding while the session goes on,
  a drag pausing and panning and one past the edge resuming, Live and
  `Space` (a field typing it instead), the cursor's readout, the drops (a
  disconnect, a unit change, leaving the page), the hint once;
  `QmlTest.phone`: the window row at 384 and 320 px while paused.

## Appendix: the panes as built (2026-11-10)

- The units of one quantity share a pane by core's imperial table's pairs;
  the layout takes a pane key per row as an input.
- A pane is at least 72 px with 8 px between panes; the phone's default
  chart height is worked out from those (266 dp), so three panes fit it.
- 2 to 4 ticks a pane, the finest of 1, 2, 2.5 and 5 times a power of ten;
  the unit at the axis's top. A range the user set cuts the traces at the
  pane's edges.
- The vertical pinch: the fingers' direction as the pinch starts picks a
  pane's range or the time; the pane's range follows the pinch's scale.
- The frame test plots one signal of each unit first. On the desktop
  (1018 px, 60 Hz) over the dense hour, four of its signals given four
  units: 3 panes at the default height, 59.7 to 60.0 frames/s and 3.8 to
  3.9 ms a chart; 4 panes maximized, 59.6 to 60.1 frames/s and 4.1 to
  4.4 ms a chart; the same four in one plot 3.8 to 3.9 ms. The laptop's and
  the test phone's figures are owed.

## Appendix: the statistics as built (2026-11-11)

- The span is closed, `[t0_us, t1_us]`, and the header says so beside
  `ecuio_recording_query`'s half-open `[t0_us, t1_us)`.
- The struct's comments are one block above it; the fields are as above (96
  bytes, `mean` at 56).
- The mean leaves a NaN out; an inf makes it inf, an inf and a -inf in one
  span NaN.
- `ecuio_recording_step` takes `ECUIO_RECORDING_STEP_FORWARD` 1 and
  `_BACKWARD` 2.

## Appendix: the two cursors as built (2026-11-11)

- The statistics and the step run on a thread of the viewer's own, from the
  first ask to the close, one ask of each kind waiting and a newer one
  replacing it. A thread that can't start answers 0.
- The cursors keep their identities; the readout goes by time, the earlier
  as A. The value labels at the plot's end follow the readout's earlier
  cursor.
- Statistics are asked for every signal, since every row shows its change.
- A handle is 48 x 48 on a phone and 24 x 24 at least on the desktop,
  reaching up over the plot past the axis strip; a phone's strip grows to
  24 px while a cursor exists, and its chart's floor is 274.
- The frame test with a span placed, on the laptop and the test phone, is
  owed.
