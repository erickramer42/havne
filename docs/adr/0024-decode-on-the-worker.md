# ADR-0024: Decoding on the worker — a values callback delivers each frame's decoded signals

- **Status:** Accepted.
- **Date:** 2026-10-26. Step 1 (core) built with this record; steps 2
  and 3 follow (§10).
- **Builds on:** [ADR-0002](0002-callback-pair-identity-snapshot-dispatch.md)
  (callback identity and snapshot dispatch, which the new callback
  shares); [ADR-0003](0003-backpressure-bounds-in-flight-frames.md) (the
  in-flight cap that drops frames); [ADR-0004](0004-signal-decoding-model.md)
  and [ADR-0016](0016-signal-definition-layout.md) (the decode);
  [ADR-0005](0005-coalesced-signal-updates.md) (the 10 Hz staging and the
  strip chart's history); [ADR-0013](0013-protocol-profiles.md) (the
  profile picks the set); [ADR-0017](0017-session-recording.md)
  (`signals.csv`); [ADR-0023](0023-replay-transport.md) (replay as the
  load test, and its planning note).

## Context

Core has the decode: `ecuio_def_applies_to_frame`, `ecuio_decode_signal`
and `ecuio_decode_text`. The loop that runs it is the bridge's. For every
frame, `QtTuneBridge::handleFrameInternal` walks the session's set on
the GUI thread, decodes each def the frame matches, writes the sample
to `signals.csv` and stages the latest value per signal. A 100 ms tick
drains the staging into `SignalListModel::updateValues`, which stamps
the whole batch with the tick's time (ADR-0005). Three things follow,
in the order a user meets them:

- **Known Issue #10: peaks drop out.** One value per signal per window
  survives, stamped with the window's time. The committed engine-start
  recording (`tests/fixtures/replay/2026-10-25-engine-start`) peaks at
  1,261 rpm; replayed under `obd2` at 10× through the bridge, the
  highest RPM the model ever held was 1,232. At `speed=max` the whole
  297 s collapses into a tick or two, and the model's highest was 758.
- **Every front end copies the loop.** The C ABI exists so a UI is a
  skin (ADR-0001). Each one would rewrite the match, the decode calls,
  the text buffer and the staging.
- **Values die with dropped frames.** A frame dropped at the in-flight
  cap (ADR-0003) is never decoded, so its values are lost along with
  its Log row.

It isn't a CPU problem. `bench_bridge` (docs/testing.md, Performance)
measured, on 2026-10-26 (Release, dev machine, median of three runs of
20,000 frames, µs), with the bridge's rows found by id and its staging
by def index:

| Defs | GUI thread a frame (drain) | of it, match and decode | worker's side a frame (`onFrame`) | one tick's `updateValues` |
|---|---|---|---|---|
| raw | 0.35 | — | 1.97 | — |
| 9 | 0.44 | 0.09 | 1.91 | 0.2 |
| 100 | 0.57 | 0.23 | 1.59 | 0.9 |
| 250 | 0.77 | 0.42 | 1.78 | 2.0 |
| 500 | 1.11 | 0.77 | 1.80 | 3.2 |
| 1000 | 1.71 | 1.37 | 1.24 | 7.6 |

- The match and decode cost about 1.4 ns a def a frame: at a full
  500 kbit/s classic CAN bus (about 4,500 frames a second), 1000 defs
  take under 1 % of a thread. Moving the work doesn't make it free; it
  moves it to the worker, where 1.4 µs a frame sits well inside the
  J2534 worker's 10 ms read slice.
- The worker's side of a frame, the 4 KB copy and the queued call, costs
  more than the decode at any profile size. It is unchanged by this
  record (see Consequences).
- The tick's update is linear in the profile. Until the same day each
  value searched the model's rows and the bridge's staging, and the
  tick grew with the square of the profile (819 µs at 1000 defs, the
  same session); that was fixed on its own, ahead of this record, so the
  numbers above measure the decode and not the search.
- The fixture at 10× delivered all 7,074 frames with none dropped, and
  at `max` none dropped either: on this machine the backlog never
  reached the cap, so the drop path is still unexercised by a replay.

## Decision

### 1. Core decodes, in the session's dispatch

`ecuio_session_dispatch_callbacks` (core.cpp), which every transport
calls once per frame, decodes the frame against the session's set
(`session->active_signal_set`: the profile's, or `uds-generic`'s own)
**after** the frame callbacks have run. It delivers the result to the
values callbacks. No transport changes. A session decodes only while
at least one values callback is registered, so a raw session, or a
consumer that wants frames only, pays nothing.

The session's set is fixed for the session's life: `profile.h` states
it is written once at create and read-only afterwards. `uds-generic`
holds every DID it was given in its set from create, polled and scanned
alike (`ecuio_uds_build`); its scan changes what the request engine
asks, never the set. Everything §4 and §6 size at create rests on that
rule. A change that lets a set change after create moves that sizing
with it, outside dispatch, and needs an appendix here.

### 2. One batch per frame, carrying the frame's time

Each frame that matches at least one def produces one batch. The batch
holds every value the frame decoded, the frame's `timestamp_us`
(session time, as the transport stamped it) and its `arbitration_id`.
A frame that matches nothing produces no batch. Core keeps no timer
and no window: coalescing is the consumer's (the bridge's, at 10 Hz),
because how often to repaint is a UI policy.

### 3. The types and calls, in `profile.h`

`profile.h` already holds the session's set (`ecuio_session_signal_set`)
and includes both `core.h` and `signals.h`:

```c
typedef struct ecuio_value {
    uint16_t signal_id;    /* the def's id */
    uint8_t  kind;         /* the def's ECUIO_VALUE_* */
    uint8_t  flags;        /* 0; reserved */
    uint32_t text_len;     /* text kinds: bytes at text, NUL not counted; else 0 */
    double   value;        /* numeric kinds; 0 for text */
    const char* text;      /* text kinds: NUL-terminated, valid during the call; else NULL */
} ecuio_value_t;

typedef struct ecuio_value_batch {
    uint64_t timestamp_us;      /* the frame's session time */
    uint32_t arbitration_id;    /* the frame's */
    uint32_t count;             /* values[0..count), at least 1 */
    uint32_t flags;             /* 0; reserved */
    uint32_t reserved;
    const ecuio_value_t* values;
} ecuio_value_batch_t;

typedef void (*ecuio_values_callback_t)(const ecuio_value_batch_t* batch,
                                        void* user_data);

ecuio_status_t ecuio_register_values_callback(ecuio_session_t*,
                                              ecuio_values_callback_t,
                                              void* user_data);
ecuio_status_t ecuio_unregister_values_callback(ecuio_session_t*,
                                                ecuio_values_callback_t,
                                                void* user_data);
```

- Each struct holds one pointer, and it comes last, so every other
  field's offset is the same on 32- and 64-bit builds. Static asserts in
  `profile.h` and `test_c_abi.c` pin the offsets, and the sizes (24 and
  32 bytes) where pointers are 8 bytes; MSVC's x86 build pads to the
  same sizes, and `c_abi` runs in `build-win32` as well.
- A value is a def's decode that returned `ECUIO_DECODE_OK`. A def whose
  decode fails is left out of the batch, as the bridge leaves it out
  today.
- The functions are additive and every code is new, so it is a
  `feat(core):` change, not a breaking one.

### 4. Text values point into the session's scratch

A `TEXT` or `HEX` def's value is `ecuio_decode_text`'s output, written
into a buffer the session owns. `text` and `text_len` point into that
buffer, which stays valid until the callback returns, like the frame
itself. One callback carries every kind, so no front end keeps a text
loop of its own.

Each text value in a batch has a slot of its own. The scratch holds
`ECUIO_DECODE_TEXT_MAX` bytes per slot. A batch fills slots from the
first, in def order, and the next batch starts from the first again,
so no two values of one batch share bytes and no value outlives its
batch.

The slot count is fixed at create, from the set (§1): the most text
defs any one frame can match. A def matches a frame by its
`message_id`, then by its mux key, if it has one, read at its own
`mux_offset` and `mux_length` (`ecuio_def_applies_to_frame`). So, for
each `message_id`:

- every text def with no mux key matches every frame on that ID;
- of the keyed text defs with the same `mux_offset` and `mux_length`,
  one frame matches only those sharing one key, so the most that share
  any one key count;
- keys of different shapes can all match one frame, so each shape's
  count adds.

The ID's bound is the unkeyed count plus, over each key shape, the
largest group sharing one key. The slot count is the largest bound over
the set's IDs. `uds-generic` comes to one: every def keys on
`62 <DID>` at offset 0, one def per DID per ECU. A custom profile can
put several text defs on one frame, and gets a slot for each. A set
with no text defs has no scratch.

### 5. The callback's contract is the frame callback's

The rules `core.h` states for the frame callback hold for the values
callback, word for word:

- It runs on the transport's worker and must not block.
- It must never call `ecuio_session_close` or `ecuio_session_destroy`,
  because both join the worker.
- It may register or unregister reentrantly. Dispatch calls the list
  as it stood when the frame arrived (ADR-0002), and after unregister
  returns a dispatch under way may call it once more, so `user_data`
  stays valid until close returns.
- Identity is the `(callback, user_data)` pair: duplicates are
  deduplicated, and unregister is idempotent.

Two more, which only a second callback can need:

- **Order:** for each frame, every frame callback returns before any
  values callback is called with that frame's batch, on the same
  thread. A consumer that records both sees each frame before its
  values.
- **Lifetime:** the batch, its values and their text are valid only
  during the call; a consumer copies what it keeps.

The two lists live under the session's one callback mutex, and both
use the one snapshot helper: on the stack up to four entries, on the
heap past that, with the frame or batch dropped if that copy fails.

### 6. Nothing allocates on the worker

The batch's value array (one entry per def in the set) and the text
scratch (§4) are allocated by `ecuio_session_create`, once the set is
known, and they never change, because the set never does (§1). The
decode, the batch and the dispatch allocate nothing per frame.
`CoreTest.WorkersAllocateNothingPerFrame` and
`J2534AdoptionTest.TheWorkerAllocatesNothingPerFrame` register a values
callback as well, so a per-frame allocation on the values path fails
them (`tests/new_counter.h` counts `operator new` on the worker).

### 7. The bridge stages from the worker

The bridge registers a values callback beside its frame callback:

- The callback runs on the worker. It checks the same accept gate as
  `onFrame`, so no value from a session being torn down reaches the
  next. It writes into staging the bridge preallocates at connect,
  under a mutex, and the 100 ms tick swaps it out and drains it.
- No queued call per value: a value reaches the model whether or not
  its frame made it past the in-flight cap.
- The staging is bounded. Past its capacity in one window, samples are
  counted and the count goes to the activity log, the way dropped
  frames are counted.
- **The drain hands the model at most one entry per signal per
  tick,** whatever the frame rate: the latest value, and later the
  window's envelope (ADR-0005's appendix). Every sample goes to the
  recorder only. The model's work per tick then grows with the profile
  (rows found by id) and never with the bus rate.
- `handleFrameInternal` keeps the frame log, `frames.csv` and the
  counters, and loses the decode loop and its text buffer.

What the staging keeps per signal per window, and how the chart uses
it (per-sample time on the x-axis, the window's envelope so a peak
survives, age taken from arrival rather than sample time), is the
chart's decision: an appendix to [ADR-0005](0005-coalesced-signal-updates.md),
built after this record's steps.

### 8. The recorder writes from the staged values

`signals.csv` keeps ADR-0017's format (`host_us,signal,value,unit`).
Its rows come from the staged values, every sample, with `host_us`
stamped in the values callback on the worker, as `onFrame` stamps a
frame's arrival today. Values of frames dropped at the cap are
recorded too.

### 9. What stays

- **Units:** the bridge still converts on the way out (ADR-0022); core
  delivers decoded values in the def's unit.
- **Known Issue #4 in kind:** every def is still matched against every
  frame, now on the worker; the "only while registered" rule is the
  only trim. A per-signal enable mask can come later if a CAN FD rate
  makes the match matter.

### 10. The steps

1. **Core**, one commit: the types, the calls, the decode in dispatch,
   the shared snapshot helper and the create-time buffers, with
   `CoreTest` cases and a `BridgeTest` case that runs both paths over
   the fixture at `speed=max` and asserts every value equal. The app
   doesn't register the new callback yet.
2. **The bridge switches**, the next commit: the values callback, the
   staging and its drain bound, and the recorder; the GUI thread's loop is
   deleted. Steps 1 and 2 both land before the next release tag, so no
   release carries both paths.
3. **The chart's per-sample history**, under ADR-0005's appendix.

## Verification

- **Unit (`test_core`):** the contract's cases mirrored for the values
  list (dedupe, reentry, the list as it stood, order after the frame
  callbacks, no decode while none is registered); every batch equal to
  direct `ecuio_decode_*` calls on the same frame, for the mock set, for
  `obd2` over the fixture and for `uds-generic` text; the allocation
  cases with a values callback registered. Two cases for §4 and §1: a
  set with several text defs that match one frame (unkeyed and keyed of
  two shapes) gets a distinct pointer per value, each with its own
  text, which a single shared slot fails; and a `uds-generic` session
  with a scanned range delivers values for its scanned DIDs, from the
  set built at create. Each case is checked against
  a mutant of the rule it pins.
- **Bridge (`test_bridge`):** step 1's both-paths equality over the
  fixture; after step 2, `ReplayOfAMockRecordingDecodesWhatWasRecorded`
  unchanged, and values that survive frames dropped at the cap.
- **Real-car recordings, offline:** after step 2, the vehicle session's
  recordings (`frames.csv` replayed under `obd2`, diffed against the
  `signals.csv` the current bridge wrote) agree value for value. The
  recordings stay out of the repository (`docs/bench-sessions/`
  scrubbing rules); the comparison is a local, environment-gated test.
- **Load:** `bench_bridge` again after step 2, the table above beside
  the new numbers.
- **BENCH or vehicle:** one live connect of the step-2 build checks
  rate and drops; agreement is the recordings' job.

## Alternatives considered

- **Decode in each transport's worker.** Three copies of the loop, and
  a fourth for the next transport; dispatch is already the one place
  every frame passes.
- **Core coalesces per window.** That puts a timer and a repaint policy
  in core. A consumer that wants every sample (a logger, a test) would
  lose them.
- **Numeric values only, text left to the front end.** It keeps a
  smaller struct, but every front end keeps a text loop, and the bridge
  keeps two paths for good.
- **A separate text callback.** A second list and contract for one
  kind of value.
- **Values on the frame's queued call.** Simpler in the bridge, but a
  frame dropped at the cap still takes its values with it, which is
  one of the three reasons for the change.
- **Both paths in the app, compared live, until the switch.** The two
  run the same core functions on the same bytes, so what can differ is
  plumbing (which frames each saw, order, time, text length). Recorded
  real-car sessions and the fixture show that offline and repeatably,
  and a shipped comparison mode would be a test seam in the product.

## Consequences

- Known Issue #10 can be fixed: every value carries its frame's time
  from the worker. The chart's side is ADR-0005's appendix.
- A front end gets decoded values from core with no loop of its own.
- Values survive frames dropped at the cap, and `signals.csv` can hold
  values whose frames `frames.csv` doesn't. A recording's two files
  then differ in a way they can't today; the events log's drop count
  says when.
- The decode's cost moves to the worker, and the GUI thread keeps only
  the frame log and the staged drain, bounded by the profile, not the
  bus rate (§7).
- The worker's 4 KB copy per frame into the bridge's queued call stays,
  and it is the largest per-frame cost measured. Copying only a frame's
  `data_length` bytes is a separate change, if the frame log needs it.
- The C ABI grows two structs whose sizes follow the pointer width,
  as `ecuio_signal_set_t`'s already does; their asserts pin both.
