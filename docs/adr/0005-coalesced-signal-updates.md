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
- The chart's x-axis resolution is one window, about 100 ms (Known
  Issue #10). Per-sample timestamps would need decoding to move onto
  the worker.
- Every signal is decoded per frame, whether or not anything is
  subscribed to it (Known Issue #4).
- Flattened arrays are easy to index wrongly. On 2026-10-10 the paint
  code read the newest *value* as the newest timestamp, and no C++
  test could see it. The manual smoke tier caught it.
