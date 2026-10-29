# ADR-0005: Coalesce signal updates to 10 Hz; pull API for the strip chart

- **Status:** Accepted
- **Date:** 2026-10-07 (coalescing, `e0c4c31`) and 2026-10-10 (strip
  chart, `a38b896`)

## Context

Notifying the GUI once per frame ties UI work to the bus rate. At the
mock's 20 frames/s the bridge emitted 200 notifications every 10 s,
and J2534 traffic could reach about 500 Hz. The strip chart is a QML
`Canvas`, which paints outside any delegate context, so it cannot
read history through per-row model roles cheaply.

## Decision

- `handleFrameInternal` decodes each frame into `m_stagingBuffer`,
  which keeps the latest value per signal ID.
- A 100 ms `QTimer` drains the staging buffer into
  `SignalListModel::updateValues()`. Each batch produces one
  contiguous `dataChanged` and one `valuesUpdated` (the latter only
  when the batch is non-empty). The timer starts on connect and stops
  on disconnect.
- Each signal keeps a 120-sample history ring (timestamp and value)
  inside the model. Samples are **batch-stamped**: one timestamp per
  window, taken from a model-side `QElapsedTimer` that restarts on
  each connect. Unsubscribing freezes a signal's history, and
  resubscribing shows the accumulated trace again.
- The chart **pulls** its data. `chartData()` (`Q_INVOKABLE`) returns
  a flattened `[ts, val, ts, val, ...]` list for each subscribed
  signal, oldest first. The Canvas repaints only on `valuesUpdated`.
- All series share one time axis, but each series' values are scaled
  independently. RPM and coolant temperature on a single shared
  y-axis would flatten the smaller signals into noise.

## Alternatives considered

- **Per-frame emission.** Rejected because UI cost would scale with
  bus rate.
- **A `HistoryRole` data role.** Rejected because the Canvas would
  need one role lookup per row on every repaint, where one call per
  repaint does the job.

## Consequences

- GUI emissions drop about 20× at mock rate (measured against the
  `guiNotifyCount` baseline) and no longer depend on the frame rate.
- The chart's x-axis resolution is one window, about 100 ms.
  Per-sample timestamps would need decoding to move onto the worker
  (done: the appendix below).
- Every signal is decoded per frame, whether or not anything is
  subscribed to it (Known Issue #4).
- Flattened arrays are easy to index wrongly. On 2026-10-10 the paint
  code read the newest *value* as the newest timestamp, and no C++
  test could see it. The manual smoke tier caught it.

## Appendix — per-sample time and each window's envelope (2026-10-26)

Decoding moved to the transport's worker
([ADR-0024](0024-decode-on-the-worker.md)), and every decoded value
reaches the bridge with its frame's session time. Two decisions above
change with it; the 10 Hz coalescing, the pull API and the shared time
axis stay.

- **Samples carry their own time.** A history sample's time is its
  frame's session time in milliseconds (`timestamp_us / 1000`), not the
  tick that drained it. The chart's x-axis is session time: under a
  replay it is the recording's time, so a replay at 10× shows ten
  seconds of the recording in each second.
- **A window keeps its envelope, not just its newest value.** For each
  signal the bridge's staging keeps the window's newest value and its
  lowest and highest, each with its time. The tick hands the model one
  entry per signal (ADR-0024 §7), and the model's history takes the
  distinct points among the three, in time order: one to three a
  window. A short peak inside a window stays on the chart at its own
  time. The engine-start recording peaks at 1,261 rpm; replayed at 10×
  with the newest value alone, the model's highest was 1,232.
- **The ring holds 360 samples,** three windows' worth for each of the
  120 it held, so the trace still spans about 12 s at 10 Hz.
- **Age is arrival, not sample time.** A row's age, which dims stale
  values, counts from when its last value arrived on the GUI thread, on
  the model's clock. Sample times are session time and can run faster
  than the wall clock.
- **The row shows the newest value,** as before; the envelope is the
  chart's.
- **`updateValues(values, timestampMs)` stays** as the one-point form:
  each value is its window's lowest, highest and newest at that time.
  Tests use it; the bridge hands the model whole windows
  (`updateWindow`).

Alternatives considered:

- **Every sample in a time-based ring.** The most faithful, but the
  ring's size and each repaint grow with the bus rate, which coalescing
  exists to stop.
- **Per-sample time, the newest value only.** The x-axis would be
  right, but a peak between two windows' newest values would still
  vanish.

**Note (2026-10-26):** the envelope is the Dashboard's too. The model
gives each row its last window's highest and lowest beside the newest
value (`windowHigh`, `windowLow`, displayed like the value), and the
gauges' peak and low holds take them, so a gauge holds the peak the
chart shows. Holding the newest value alone, the engine-start
recording replayed at 100× never reached its 1,261 rpm on the tach
(`QmlTest.dashboard`).

## Appendix — the model's timer counts sleep (2026-10-29)

The model's timer, behind a value's age and the fallback stamp, reads
core's clock (`ecuio_clock_us`, [ADR-0033](0033-session-time-counts-sleep.md))
instead of a `QElapsedTimer`, which on Linux and Android stops while
the machine sleeps. A value read before a sleep is as old as it is at
the wake, and its gauge dims then.
