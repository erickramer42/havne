# ADR-0063: Conditions: one model in core, stored as data and judged on the worker, for triggered logging, alerts and markers

- **Status:** Accepted. Built: section 12's steps 2 and 3, the
  condition loader and the evaluator (the two appendices of
  2026-11-12), and step 5's focus half (the appendix "the focus keeping
  a condition's signals"). Step 4, the rest of step 5 and the steps
  after it are not built. "Decided" at the end
  records each choice.
- **Date:** 2026-11-10.
- **Builds on:** [ADR-0015](0015-request-engine.md) (requests as data,
  read-only by construction); [ADR-0024](0024-decode-on-the-worker.md)
  (decoding on the worker, the callback contract);
  [ADR-0049](0049-recordings-in-core.md) (the recorder, markers, the
  reader, the frames check); [ADR-0051](0051-signal-files.md) (signal
  files, computed signals, the stale age, the strict JSON reader);
  [ADR-0062](0062-computed-grammar.md) (the computed grammar of
  signal-file format 2: functions, conditionals, computed signals that
  read each other), built first;
  [ADR-0056](0056-analysis-log.md) (a recorder attached to a running
  session, the focus); [ADR-0057](0057-dashboards.md) (a gauge's
  references and zones); [ADR-0031](0031-command-line-client.md) and
  [ADR-0053](0053-python-bindings.md) (`havne-cli` and the Python
  bindings).
- **Amends, each by a dated note when built:** ADR-0049 (an armed
  recorder, `cond` lines, `conditions.json`), ADR-0056 section 2 (the
  focus keeps a condition's inputs), ADR-0031 section 3 (schema 1
  grows), ADR-0053 section 4 (a third queue kind), ADR-0057 (an alert
  made from a gauge).
- **Read against:** [DISCLAIMER.md](../../DISCLAIMER.md).

## Context

Three planned features ask the same question of a session, "does this
hold":

- **Triggered logging:** record while something holds (full throttle, a
  temperature past a level), or keep a window around a link fault. The
  analysis log (ADR-0056) starts and stops by hand, and a recorder
  attached to a running session holds nothing from before its attach.
- **Alerts:** tell the driver when a signal passes a level. A gauge's
  zones (ADR-0057) color the value it draws, in QML, from values the
  bridge coalesces to 10 Hz (ADR-0005), and `havne-cli` has no gauges.
- **Markers:** a recording's markers come from the Mark button or a
  typed line (ADR-0049 section 4). Nothing marks where a log got
  interesting without a hand on the screen.

Core has most of the parts: computed signals with an age bound per input
(ADR-0051 section 5) and the functions, conditionals and chains of
format 2 (ADR-0062), one stale age (`ECUIO_VALUE_STALE_US`), a signal
file's dial range and warning levels (ADR-0051's appendix "a signal's
gauge"), link health, and the frames check's rules (gaps, rate drops, a
late ID, overflows, broken ISO-TP), which run over a finished recording
only (ADR-0049 section 6).

Forces:

- The UI receives final values and `havne-cli` is a front end of equal
  standing, so the judging belongs in core, once.
- A worker allocates nothing per frame and a callback on it never
  blocks (ADR-0024 sections 5 and 6).
- Signals arrive at their own pace, a fraction of a second to several
  seconds apart, so a condition has to say what an old input means.
- A value sitting at a level crosses it many times; a condition must
  not flap with it.
- A recording is evidence: it says which conditions were attached and
  what fired, and a replay of it fires the same.
- Data, not code: iOS loads no code at run time, and no plugin code
  runs on the worker (ADR-0015 section 2).
- Havne reads. A condition never sends anything.

## Decision

In brief: a condition is a named test with a state core keeps. It is
written in a condition file (JSON, core's strict reader), bound to a
session's signals by the Dashboard's references, and judged on the
worker inside the frame decoder, on the frames' own time, so a live
session, a replay and a read of the recording give the same events. Its
uses are a closed list: an alert, a recording and a marker. Core records
and marks; a front end shows the alert.

### 1. The model

A **condition** has a key, a name, one to four **terms** joined by
`all` or `any` (one level, no nesting), its timing (section 2) and its
**uses**. Core keeps its state: OFF or ON, and whether its inputs are
fresh enough to judge.

A term reads one input:

| Term | Reads | True when |
|---|---|---|
| Signal | One number, enumeration or computed signal, by reference | The value is `above` or `below` a level, or `equals` or `differs` from a number |
| Link | The session's link health (`ecuio_session_get_error_state`) | The state is the one named: `not_ok`, `timeout`, `request_failed` or `hard_failure` |
| Check | One of the frames check's rules, run on the live frames (section 4) | A finding of that kind is under way: `gap`, `rate_drop`, `late_id`, `overflow` or `isotp` |

- **A signal term's reference** is a Dashboard cell's (ADR-0057
  section 2): an ordered list, at most 8, of `id:<n>` and `key:<key>`,
  taking the first the session's set provides. A file written by hand
  may write `obd2.<name>` (ADR-0051's names: `obd2.coolant`), read as
  that def's `id:`. So one condition serves the mock, `obd2` and a
  vehicle's signal file, as one gauge does. The first reference the set
  provides is the one read, whatever it turns out to be: the list is an
  order of preference, never a search for a def that fits. `id:<n>`
  names the def with that ID, a base def a signal file replaced by name
  included. An ID from 20000 up names nothing: from there IDs follow an
  order (`uds-generic`'s the URI's DIDs, a signal file's own its list),
  so none is a stable name. A reference of any other form refuses the
  file.
- **Levels are in the signal's own unit,** the decoded one, as a
  `gauge` key's and a cell's options are. The term names that unit
  (`unit`), and a reference resolving to a def of another unit makes
  the term unavailable: 150 is never compared with a value in psi.
  The units are compared byte for byte. A term that names none (only
  `equals` and `differs` may) has the unit `""`, which matches a def
  with no unit and no other, and `"unit": ""` is how `above` and
  `below` name a number that has none.
- **An enumeration** takes `equals` and `differs` on its raw number.
  Text and hex signals have nothing to compare.
- **A computed signal** is a def like any other, so an alert on boost
  is a term on `key:boost`. Arithmetic belongs to computed signals,
  whose grammar has `min`, `max`, `abs` and `if` (ADR-0062); a term
  only compares, and holds no expression (section 11).
- **An unavailable condition** (a term whose signal the set lacks, a
  unit that differs, a link term on a session with no link health, an
  `isotp` term on frames already reassembled) is never judged. It
  stays in the set and is reported with its reason, as a gauge without
  its signal is kept and says why (ADR-0057 section 3).

The uses are a closed list in core, and none sends a request:

- **`alert`:** a level (`warning` or `danger`) and whether it sounds
  and shows a banner. Core carries these to the front end and does
  nothing with them (section 9).
- **`record`:** an armed recorder writes while the condition is ON,
  from `before` seconds ahead of it to `after` seconds past it
  (section 7).
- **`mark`:** a row in `markers.csv` each time it fires (section 8).

A condition with no use is still judged and reported: a script reading
`havne-cli`'s lines or a Python program is a use of its own.

### 2. Time: hysteresis, holds and old inputs

Every rule runs on the frames' session time (`timestamp_us`), as a
computed signal's ages do.

- **A sample holds until the next.** Between two samples a term keeps
  the truth of the earlier one, as a wide export's cell holds its value
  (ADR-0056 section 4): nothing is interpolated.
- **Hysteresis.** `above` with `clear` turns true above the level and
  false at or below `clear`; `below` mirrors it. `clear` lies on the
  off side of the level, and without it the level alone decides.
- **Holds.** `for` (0 to 600 s, default 0): the joined terms stay true
  that long before the condition turns ON. `clear_for`: they stay false
  that long before it turns OFF.
- **Old inputs.** A term whose sample is older than its age bound can't
  be judged. The bound is `ECUIO_VALUE_STALE_US` unless the term's
  `max_age` (0.1 to 60 s, ADR-0051's bounds) sets it, so an alert never
  claims more than a dimmed gauge shows, and a slow signal sets its
  own. The join is three-valued: `all` is false when any term is false
  and unjudged when none is false and one can't be judged; `any` is
  true when any term is true and unjudged when none is true and one
  can't be judged.
- **A condition that can't be judged keeps its state and gains
  `STALE`.** ON and stale reads "high when last read, nothing fresh
  since". A running `for` starts over. The next judgment clears the
  flag. A stale ON never turns OFF by age alone: a fresh sample or the
  session's end does that.
- **Firing** is OFF to ON. A marker, a sound and the fired count belong
  to a firing only, so a condition coming back from stale to the state
  it had fires nothing. `record` reads stale as OFF: its `after` starts
  running, so a signal that stops answering never holds a recording
  open.
- **A deadline is settled by the next frame, at the deadline's own
  time.** A hold's end and an input's age bound are deadlines: a
  sample's time plus a duration. Before judging a frame's values the
  evaluator settles every deadline earlier than that frame's time and
  stamps each change with the deadline's time, not the frame's. So the
  events are a function of the frames alone: the same frames give the
  same events at the same times, live, replayed at any speed, or read
  back from the recording. The cost: a wholly silent bus settles
  nothing until it speaks. Link health reports that silence within
  about a second on its own path, and a link term reads it.

### 3. Where a condition is judged

On the worker, in the frame decoder (`core/src/frame_decoder.{h,cpp}`),
after the computed stage. The session's worker and the reader's frames
path already share that decoder (ADR-0049's appendix "the reader decodes
frames.csv"), so one evaluator serves a live session, a replay and a
recording read back.

- **Prepared once.** Attaching a set builds a plan: for each def, the
  terms that read it; one state per condition; a fixed buffer for one
  frame's events. A decode allocates nothing, and
  `CoreTest.WorkersAllocateNothingPerFrame` gains a case with the
  largest set attached.
- **The cost per frame:** one compare against the earliest deadline;
  for each value the frame decodes, one or two compares for each term
  reading that def; a term that changes joins its condition's terms
  again, four at most. The bounds are 64 conditions and 4 terms each,
  so 256 terms at most when one frame feeds every one of them; a
  packed `obd2` reply carries `ECUIO_OBD2_MAX_RECORDS` PIDs at most.
- **The memory per session,** by estimate: under 32 KB for the largest
  set. The live checks' table (section 4) adds about 100 KB, only
  while a check term is attached. The build measures both.
- **Decoding runs while conditions are attached,** as it does while a
  session records (ADR-0049's amendment of ADR-0024 section 1).
- **Delivery.** A frame's events go to a third callback list under the
  frame callback's whole contract (ADR-0024 section 5), after that
  frame's values callbacks, and into the recorder's record for the
  frame. A link transition is judged where `link_set_state` publishes
  it, on the same worker, and its events are delivered from there on
  the session's clock.
- **Measured at the build.** `bench_ab`'s load A/B with the largest set
  attached goes in an appendix. A cost past 5 % of the saturated mock's
  frames per second is a reason to index the terms harder before
  shipping.

### 4. The live checks

The frames check is a stream already (`core/src/recording_check.h`'s
`checker`): fed each frame in order, it settles a finding when the frame
that ends it arrives. While a check term is attached the decoder feeds
it each live frame at the frame's session time.

- **A fixed table.** Read offline, the check's per-ID state grows with
  the IDs seen. Live it takes a table of 512 IDs sized at the attach,
  so a new ID costs the worker no allocation. An ID past the table
  isn't judged, and the states say so once
  (`ECUIO_CONDITIONS_IDS_FULL`). A requesting session hears a handful
  of IDs; the table is for a whole-bus listener.
- **What ON means for each kind:**
  - `gap`: the whole bus quiet for longer than the gap. It is known at
    the frame that ends it, so the ON at the gap's start and the OFF at
    its end arrive together. "Quiet right at this moment" is the link
    term's.
  - `rate_drop`: from an ID's first slow interval, known when its late
    frame arrives, to its next normal one.
  - `overflow`: across a run of frames flagged
    `ECUIO_FRAME_FLAG_OVERFLOW`.
  - `late_id`, and `isotp` for a consecutive frame out of order: an
    instant, ON and OFF at one time. `isotp` for a first frame never
    completed: from that frame to the one that cuts it short.
- **`isotp` needs frames as they crossed the bus** (a listening
  SocketCAN session). Where a transport reassembles, the term is
  unavailable, by the check's own `SKIPPED_ISOTP` rule.
- **Thresholds** are the check's public defaults
  (`ECUIO_CHECK_DEFAULT_*`), which a condition file may set once for
  the whole file (`checks`).
- **A sleep** is taken out of a gap by the rise of
  `ecuio_clock_slept_us` since the frame before, as the check of a
  recording takes out its `asleep` lines.
- An event from a check term carries the finding's arbitration ID.

### 5. The condition file

Conditions live in a file of their own, JSON (UTF-8), read from bytes by
the reader signal files use (`core/src/json.{h,cpp}`), with the same
strictness: an unknown key, a wrong type or a duplicate key refuses the
file, the error naming the line, the column and the key and never a
path; `notes` and keys starting `x-` are ignored. A fuzz target
(`fuzz_condition_file`) and a JSON Schema
(`schema/condition-file.schema.json`, kept in step by the tests that
keep the signal file's) come with it.

```json
{
  "havne_conditions": 1,
  "name": "Daily driver",
  "checks": { "gap": 1, "rate_drop": 10, "settle": 2 },
  "conditions": [
    {
      "key": "coolant_high",
      "name": "Coolant high",
      "when": { "signal": ["obd2.coolant"], "unit": "°C",
                "above": 110, "clear": 105, "max_age": 10 },
      "for": 2,
      "alert": { "level": "danger", "sound": true, "banner": true },
      "mark": true
    },
    {
      "key": "pull",
      "name": "Full throttle",
      "when": { "all": [
        { "signal": ["obd2.throttle"], "unit": "%", "above": 80, "clear": 70 },
        { "signal": ["id:1001", "id:7003"], "unit": "RPM", "above": 2500 }
      ] },
      "for": 0.5,
      "record": { "before": 5, "after": 5 },
      "mark": true
    },
    {
      "key": "link_fault",
      "name": "Link fault",
      "when": { "link": "not_ok" },
      "record": { "before": 10, "after": 10 }
    },
    {
      "key": "module_slow",
      "name": "A module slowed down",
      "when": { "check": "rate_drop" },
      "alert": { "level": "warning", "banner": true },
      "mark": true
    }
  ]
}
```

- **`havne_conditions`**, first and required, is the format's version;
  a reader refuses one it doesn't know.
- **`when`** is one term, or `all` or `any` over 1 to 4 terms.
- **`key`** is 1 to 31 of `a`-`z`, `0`-`9` and `_`, unique in the file,
  and the stable name: the CLI's lines, the recording's lines and the
  states refer to it. **`name`** is 1 to 40 bytes and passes the
  formula guard (ADR-0049 section 8), since it reaches `markers.csv`.
- **`enabled`** (optional, default true): false keeps a condition in
  the file and judges nothing.
- **Bounds:** 256 KiB a file, 64 conditions, 4 terms each, 8 references
  a term, `for` and `clear_for` 0 to 600 s, `before` 0 to 60 s, `after`
  0 to 600 s, `max_age` 0.1 to 60 s.
- **Refused at load:** every rule above; `clear` on the wrong side of
  its level; `above` or `below` on a term that names no unit;
  `sound` or `banner` without a level; a use that names nothing. What
  only a session can settle (a reference the set lacks, a unit that
  differs) is not refused: it makes a condition unavailable at the
  attach, and `havne-cli conditions check` reports it against a profile
  or a signal file on the desk.

**Why a file of its own.** A condition on a built-in set's signal
(`obd2`'s coolant) needs no signal file. One reader serves the app,
`havne-cli` and Python. A recording copies the file whole (section 8).
A signal file keeps saying what a signal is, and gives an alert its
starting level through its `gauge` key (section 9).

**How a user writes one.**

- **In the app:** from a gauge (its panel's **Alert**, section 9), or
  on a Settings page listing the set, where Add picks a signal with
  Live's filter (ADR-0050), a comparison, a level typed in the unit on
  screen and turned back into the recorded one (as the viewer's
  alignment level is), a hold, and the uses. The app keeps one set for
  every vehicle, as it keeps one set of dashboards, as this file's
  text in its settings; core's loader judges every edit before it is
  stored. Import and Export read and write the same file `havne-cli`
  takes.
- **For `havne-cli` and Python:** the file, written by hand or by the
  app's Export, checked with `havne-cli conditions check`.

Havne ships no condition switched on: a level that suits one engine is
a false alarm on another.

### 6. The C ABI

All of it is additive: a new public header, `ecuio/conditions.h`, in
`ECUIO_PUBLIC_HEADERS` (so exported, generated into
`python/havne/_ecuio.py` and compiled as strict C by `test_c_abi.c`),
one more queue kind, one recorder flag and fields in reserved room.

```c
typedef struct ecuio_condition_set ecuio_condition_set_t;   /* opaque */

/* Reads a condition file's bytes; a refusal's words through
 * ecuio_last_error_detail(). The set keeps the bytes it was read from. */
ecuio_status_t ecuio_condition_set_load(const char* bytes, size_t length,
                                        ecuio_condition_set_t** out);
void ecuio_condition_set_free(ecuio_condition_set_t* set);
uint32_t ecuio_condition_set_count(const ecuio_condition_set_t* set);
const char* ecuio_condition_set_name(const ecuio_condition_set_t* set);
/* The file's "checks" as the frames check's options; 0 is its default */
ecuio_status_t ecuio_condition_set_checks(const ecuio_condition_set_t* set,
                                          ecuio_check_options_t* out);
ecuio_status_t ecuio_condition_set_get(const ecuio_condition_set_t* set,
                                       uint32_t index,
                                       ecuio_condition_info_t* out);
/* One term as the file wrote it: 456 bytes, its references inline, no
 * pointer */
ecuio_status_t ecuio_condition_set_get_term(const ecuio_condition_set_t* set,
                                            uint32_t condition, uint32_t term,
                                            ecuio_condition_term_t* out);

typedef struct ecuio_condition_binding {
    uint32_t why;             /* ECUIO_CONDITION_WHY_*; 0: every term binds */
    uint32_t why_term;        /* the first term that doesn't; UINT32_MAX
                                 when no one term is the reason */
    uint32_t term_count;
    uint32_t reserved0;
    uint32_t reference[4];    /* per term: which reference was taken */
    uint32_t signal_id[4];    /* and the def it names; UINT32_MAX for none */
    uint64_t reserved[2];     /* zero */
} ecuio_condition_binding_t;  /* 64 bytes, no pointer */

/* How one condition's references meet a set of signals, with no
 * session, as ecuio_session_set_conditions binds them. file is the
 * signal file whose set signals is, for key: references, or NULL. */
ecuio_status_t ecuio_condition_set_resolve(const ecuio_condition_set_t* set,
                                           uint32_t index,
                                           const ecuio_signal_set_t* signals,
                                           const ecuio_signal_file_t* file,
                                           ecuio_condition_binding_t* out);

/* Binds the set to the session's signals and judges it from the next
 * frame; NULL takes it away. file is the signal file decoding the
 * session, for key: references, or NULL. The session copies what it
 * needs: the set may be freed once this returns. */
ecuio_status_t ecuio_session_set_conditions(ecuio_session_t* session,
                                            const ecuio_condition_set_t* set,
                                            const ecuio_signal_file_t* file);

typedef struct ecuio_condition_event {
    uint64_t timestamp_us;    /* session time of the change: a deadline's
                                 own time (section 2) */
    uint32_t condition;       /* its index in the set */
    uint32_t state;           /* ECUIO_CONDITION_OFF 0, _ON 1 */
    uint32_t flags;           /* ECUIO_CONDITION_STALE 0x1, _FIRED 0x2 */
    uint32_t term;            /* the term that decided it */
    double   value;           /* that term's sample; 0 for a link or
                                 check term */
    uint32_t arbitration_id;  /* a check term: the finding's ID */
    int32_t  code;            /* a link term: the link's state; a check
                                 term: the finding's kind */
    uint32_t generation;      /* the attach it belongs to */
    uint32_t reserved0;       /* zero */
    uint64_t reserved;        /* zero */
} ecuio_condition_event_t;    /* 56 bytes, no pointer */

typedef void (*ecuio_conditions_callback_t)(
    const ecuio_condition_event_t* events, uint32_t count, void* user_data);
ecuio_status_t ecuio_register_conditions_callback(
    ecuio_session_t* session, ecuio_conditions_callback_t callback,
    void* user_data);
ecuio_status_t ecuio_unregister_conditions_callback(
    ecuio_session_t* session, ecuio_conditions_callback_t callback,
    void* user_data);

typedef struct ecuio_condition_state {
    uint32_t state;           /* ECUIO_CONDITION_OFF, _ON */
    uint32_t flags;           /* _STALE; ECUIO_CONDITION_UNAVAILABLE 0x4 */
    uint64_t since_us;        /* session time of the last change */
    uint64_t fired;           /* firings so far */
    double   value;           /* the deciding term's sample at the
                                 last change */
    uint32_t why;             /* UNAVAILABLE: ECUIO_CONDITION_WHY_* */
    uint32_t generation;      /* the attach these states belong to */
    uint64_t reserved[2];     /* zero */
} ecuio_condition_state_t;    /* 56 bytes */

/* Every condition's state, in the set's order (the serial ports'
 * contract for sizes). Any thread; readable after close. */
ecuio_status_t ecuio_session_get_condition_states(
    ecuio_session_t* session, ecuio_condition_state_t* out,
    uint32_t capacity, uint32_t* out_count);
```

- **`ecuio_condition_info_t`** gives what a front end needs without
  parsing the file: the uses as flags, the alert's level, `for`,
  `clear_for`, `before` and `after`, the term count, `flags` and
  reserved room, and the key and name as borrowed pointers placed last
  (valid until the set is freed), as `ecuio_value_t` places its own.
- **`ecuio_condition_term_t`** gives one term without parsing the
  file either: its kind, its comparison (or the link state or the check
  kind it names), the level, `clear`, the age bound, the unit, and its
  references inline, each with its kind, its def ID and its text as
  written. A fixed struct with no pointer, so both pointer widths and
  the Python declarations read it alike.
- **`ecuio_condition_set_resolve`** is the bind with no session: which
  reference each signal term takes in a set of signals, the def it
  names there, and why the condition can't be judged
  (`ECUIO_CONDITION_WHY_*`: switched off, no signal, another unit, a
  text signal, an enumeration above or below a level). `havne-cli
  conditions check` and an editor read it; the reasons only a session
  knows (no link health, frames already reassembled) join the list with
  their steps.
- **A refusal has no code of its own.** `ecuio_condition_set_load`
  returns `ECUIO_ERR_INVALID_ARGUMENT` with the line, the column and the
  words in `ecuio_last_error_detail()`, as `ecuio_signal_file_load`
  does.
- **Opaque, with its release.** `ecuio_condition_set_load` hands out a
  set and `ecuio_condition_set_free` takes it back, the pair the export
  rule asks of every allocation. Every value list (states, flags, the
  `WHY` codes, the queue kinds) is append-only.
- **Attach before or after start.** A running session takes the new
  plan between two frames, through a pinned pointer as the record link
  swaps its recorder, and every state starts again OFF and stale. While
  a recorder is attached the call is `ECUIO_ERR_IN_USE`, so a recording
  holds one set for its whole length. A file whose set isn't the
  session's is `ECUIO_ERR_INVALID_ARGUMENT`, as the recorder's signal
  file is.
- **The states** are published under a lock taken only when one
  changes, as support and timing are published, so the getter never
  waits on a frame.
- **The pull queue** (ADR-0053 section 4) gains
  `ECUIO_QUEUE_CONDITIONS` (3) and `ecuio_queue_read_conditions`. An
  entry is the event itself: it holds no pointer.
- **The recorder:** `ECUIO_RECORD_ARMED` (0x1) in
  `ecuio_record_options_t.flags`, `ECUIO_RECORDING_ARMED` (4) among the
  states, and the count of windows written in
  `ecuio_recorder_state_t`'s reserved room (section 7).
- **The reader:** `ecuio_recording_get_condition_event` with a count in
  `ecuio_recording_info_t`'s reserved room,
  `ecuio_recording_get_conditions` (the header's line, whether
  `conditions.json` is there, whether its hash matches, as
  `ecuio_recording_get_signals` answers for the signal file), and
  `ecuio_recording_evaluate` (section 8).

Sizes and offsets are pinned by static asserts in the header and in
`test_c_abi.c`, on x86 too.

### 7. Triggered logging: an armed recorder

A recorder opened with `ECUIO_RECORD_ARMED` and attached as any other
(before or after start, ADR-0056 section 3) writes nothing until a
condition with `record` turns ON.

- **The pre-roll is the ring.** The worker puts every record into the
  recorder's ring as for any recording. While armed, the writer holds
  them there and lets go only of those older than the longest `before`
  among the attached conditions, so nothing reaches the disk and no
  folder exists. A ring that fills first lets its oldest go: a shorter
  pre-roll, counted in the state. The default ring holds over 20 s of a
  saturated classic bus (ADR-0049 section 1), and `before` stops at
  60 s.
- **A firing opens a window.** The writer makes a folder (ADR-0049
  section 2's name) and writes from the oldest record within that
  condition's `before`. A frame is judged before the pre-roll lets
  anything go, so a window around a gap, known only at the gap's end,
  still starts `before` ahead of the gap's start.
- **A window is the union** of the `record` conditions that are ON. It
  ends `after` seconds past the last of them turning OFF or stale, and
  a firing inside that time carries it on. A condition that never
  clears records until the session ends, as Record sessions does.
- **One folder per window,** each a whole recording with its zero at
  its first frame, a header as a recording begun mid-session has it
  (ADR-0049's appendix "a recorder attached after start": the focus and
  the link's state there), plus `trigger  <key>,<before> ms` and one
  `condition  <key>,on` line for each condition already ON at its
  start. Every reader, the viewer, the exports and a replay take it as
  any recording. `ecuio_recorder_get_folder` gives the newest window's
  folder, and empty text before the first.
- **Closing** an armed recorder that never fired leaves nothing on
  disk, as a connect that fails at create leaves no folder.
- **One recorder per session stays the rule,** so a session is in one
  of three modes: Record sessions (every frame), a log (Start and
  Stop), or armed (conditions start and stop it).
- **The focus keeps a condition's inputs** (amends ADR-0056 section 2):
  the engine never parks a request that carries an attached, enabled
  condition's input, so an alert on coolant keeps working while a log
  of five fast signals runs. A computed input counts as the signals it
  reads, through its chain to the ones a reply carries (ADR-0062
  section 4). The cost is those requests in every sweep, and the log's
  `focus` line still lists only what was picked.

### 8. What a recording holds, and a replay

Any recording of a session with conditions attached (Record sessions, a
log or an armed window) holds:

- **`conditions.json`:** the set's bytes as loaded, with a header line
  `conditions  conditions.json,<SHA-256>` in `events.log`, as
  `signals.json` has (ADR-0051's appendix "a recording's signal file as
  built"). The format number doesn't move: readers pass a header key
  they don't know.
- **`cond` lines** in `events.log`, a new kind word (readers pass an
  unknown kind too): `cond  <key>,<on|off>,<flags>,<value>`, the flags
  `fired` and `stale` as words. Each is written at the recording's time
  of the frame that settled it, less the distance back to its deadline,
  so the line's time is the event's. These are what fired, kept as
  evidence whether or not anything judges the recording again.
- **`markers.csv` rows** for conditions with `mark`: one at each
  firing, labeled with the condition's name. They are markers like any
  other, so two runs align on "Full throttle" as they do on "Mark 1"
  (ADR-0049 section 5), and the file's two columns don't change.
- **Events are never lost to a full ring.** A frame's events ride in
  its record. If the record doesn't fit, they are carried to the next
  one that does, as a link transition is, up to a fixed count that the
  `drop` line reports past.

**A replay fires the same conditions.** A `replay://` session with the
recording's `signals.json` and `conditions.json` attached decodes the
same values in the same order (ADR-0051's appendix "a replay reads with
its recording's copy") and settles the same deadlines on the same frame
times, so it delivers the events the `cond` lines hold, at any `speed=`.
Both front ends attach the recording's copy on a replay when its hash
matches the header's, unless the user names another set.

**A set run over a recording.** `ecuio_recording_evaluate(folder,
signal_set, conditions, file, progress, callback, user_data)` reads
`frames.csv` through the decoder, as `ecuio_recording_open_frames`
does, and hands back the events any set gives on it, with
`events.log`'s `link` and `asleep` lines feeding the link terms and the
gaps. A new condition can be tried against last month's drives.

**Where the three differ,** each for a stated reason:

- **Link terms** fire live and in `ecuio_recording_evaluate`, never on
  a `replay://` session, which has no link health (ADR-0023).
- **A sleep** is known live and to `ecuio_recording_evaluate`; a
  `replay://` session reports a gap across one.
- **A recording that dropped frames** (`drop` lines) lacks frames the
  live evaluator saw.
- **A window or a log begun mid-session** starts its replay with every
  condition OFF and stale, while live a condition may have stood inside
  its hysteresis band or its hold. The `condition  <key>,on` header
  lines say which were ON.
- **A recording whose frame times step backward** replays across the
  step on its host times (ADR-0023), so an event just after such a step
  can move by the step's size.

### 9. Alerts

Core says which conditions are ON, at which level, and whether to sound
or show a banner. Showing it is each front end's.

**In the app, Theme roles first:**

- **Roles.** `Theme` gains the alert roles (a warning and a danger
  fill, and the text on each), set in every palette and held to the
  contrast tests. The two levels differ by a word and a mark, never by
  hue alone (ADR-0057 section 9's rule; a night palette has one hue).
- **Color.** A gauge or a Live row whose signal an ON alert reads draws
  its value in the level's role, with the mark.
- **A banner.** A band in the page, never a `Popup`, on every tab and
  in the drive view (ADR-0059), naming the condition and its value,
  danger above warning, shown while ON and for at least 3 s. A stale
  alert's band says nothing fresh has been read.
- **A sound,** after the banner ships: one short tone at a firing, the
  two levels distinct, at most one tone per condition in 5 s. On
  Android a firing in the background also posts a notification; the
  foreground service keeps the session judging (ADR-0029).

**An alert starts from a gauge.** A cell's panel gains **Alert**: a
condition made from the cell's references and unit and its zone in
force, `dangerFrom` as `above` at level `danger`, else `warnFrom` at
`warning`, with `clear` 2 % of the cell's range back from the level and
no hold. A cell made for a signal whose file has a `gauge` key starts
from the file's warning levels (ADR-0051's appendix), so its alert does
too, and Settings' Add reads the same levels for a signal on no page.
The alert is a copy: a later edit of the zone doesn't move it, and the
dial's zones stay a drawing of the value (ADR-0057). The defaults are a
starting point, tuned against real logs before the feature ships.

**DISCLAIMER.md** says an alert is an aid and never stands in for the
car's own warnings: an input can go stale, a link can drop, a phone can
be muted.

### 10. Front ends, and what each lacks

- **The app:** the alerts above; the Settings page for the set
  (section 5); **Armed** beside Start log; the activity log's line for
  each firing; in the viewer, a recording's condition events as spans
  on the time axis beside the check's findings, each listed and
  centering the view on itself, and a set run over the open recording.
- **`havne-cli` (schema 1 grows by line types and keys, none
  changed):**
  - `stream <uri> --conditions <file>`: a `conditions` line at the
    start (each key, its uses, available or why not) and a `condition`
    line per event, `{"type":"condition","t_us":12431000,
    "key":"coolant_high","state":"on","fired":true,"stale":false,
    "value":112.0,"term":0}`;
  - `log <uri> ... --conditions <file> [--armed]`: an armed log, with a
    `recording` line as each window opens and closes;
  - `conditions check <file> [--signals <file> | --profile <name>]`:
    the loader's words, then each condition as read and how its
    references resolve, exit 3 for a refused file;
  - `info <folder> --conditions <file>`: a set run over a recording;
    `view` and `info` list a recording's own events, and their `opened`
    line gains `conditions` (null, or `sha256`, `present`, `matches`).
- **Python:** `ConditionSet.load(bytes)`, `session.set_conditions(...)`,
  the events through the pull queue (no callback: ADR-0053 section 4),
  `Recorder(armed=True)`, a recording's `condition_events()` and
  `evaluate(...)`.

**Gaps, on purpose:**

- `havne-cli` and Python show no color or banner and play no sound: the
  `condition` line and the queue's event are their alert, and a script
  does what it likes with one.
- Neither has an editor: the file is written by hand or exported from
  the app, and `conditions check` judges it.
- No front end fires link terms on a `replay://` session (section 8).

### 11. Conditions and the computed grammar

The computed grammar of signal-file format 2 (ADR-0062) is built before
conditions, so a term has functions, conditionals and chains to read
from the first release. The split stays: a computed signal works a
quantity out, and a term compares one signal with a level.

```json
{ "key": "trim_total", "name": "Fuel trim, total", "kind": "computed",
  "expr": "abs(obd2.pid_06 + obd2.pid_07)", "unit": "%", "decimals": 1 },
{ "key": "oil_margin", "name": "Oil pressure margin", "kind": "computed",
  "expr": "oil_pressure - if(obd2.rpm > 3000, 25, 10)", "unit": "psi" }
```

```json
{ "key": "trim_far", "name": "Fuel trim far off",
  "when": { "signal": ["key:trim_total"], "unit": "%",
            "above": 15, "clear": 12 },
  "for": 5, "alert": { "level": "warning", "banner": true } },
{ "key": "oil_low", "name": "Oil pressure low",
  "when": { "signal": ["key:oil_margin"], "unit": "psi", "below": 0 },
  "for": 1, "alert": { "level": "danger", "sound": true, "banner": true } }
```

- **Past a level:** the larger of several signals is `max(...)`, the
  smaller `min(...)`, a distance from zero or between two signals
  `abs(...)`, each a computed signal a term reads by `key:`.
- **A level that depends on another signal** is a conditional: the
  margin above is oil pressure less the level its engine speed asks
  for, and the term is `below` 0.
- **A join or a computed signal.** `max(a, b)` above a level is also
  `any` of two terms, and `min(a, b)` above it `all`. The computed
  signal is the form for a quantity worth seeing, since a gauge, the
  recording and the viewer get it too. The join is the form for a
  built-in set's signals with no signal file, and it differs on old
  inputs: `any` is true on one fresh input past the level (section 2's
  join), while `max` has no value until both are fresh.
- **A term holds no expression.** Arithmetic has one home, with one set
  of names, ages and refusals, and what an alert judged is a signal the
  recording holds. A condition on a built-in set's arithmetic takes a
  signal file that extends the set.
- **A level that would flap belongs in a term.** A comparison inside an
  expression has no hysteresis and no hold (a program keeps no state),
  so `if(coolant > 110, 1, 0)` flips with every sample at the level.
- **Time.** A computed sample's time is the frame that fed it, and a
  term bounds its age as any sample's. A computed signal with an input
  past that input's own bound gives no value (ADR-0062 section 5), so
  its term goes stale as one on a signal that stopped answering.
- **Stages.** The decoder decodes, computes (a chain in its order),
  then judges. A computed signal never reads a condition, so the order
  never loops.

### 12. Steps

Each step leaves `havne-cli` and Python level with core; the app's
pages are step 7.

1. The computed grammar, by ADR-0062's own steps: the machine, the
   compiler and format 2, chains, the front ends. No condition code.
2. The condition loader, `conditions check`, the schema and the fuzz
   target. No session.
3. The evaluator in the decoder (signal terms and section 2's timing),
   the callback, the states, the queue kind; `stream --conditions`;
   Python; the load A/B.
4. The recording: `conditions.json`, `cond` lines, markers, the
   reader's events, a replay with the copy.
5. The armed recorder; `log --armed`; the focus keeping inputs.
6. Link terms and the live checks.
7. The app: the roles, the Settings page, the banner and colors, Alert
   on a gauge, Armed, the viewer's spans; then the sound; then
   Android's notification.
8. `ecuio_recording_evaluate`, `info --conditions` and the viewer's
   run.

## Tests

- **The rules, pure:** hysteresis, holds, the three-valued join, stale
  keeping the state, a deadline stamped with its own time and settled
  by a later frame, firing counted once across a stale stretch.
- **One evaluator, three ways:** a mock session recorded with a set
  attached; its recording replayed at `speed=max`; the same set run
  over it with `ecuio_recording_evaluate`. The three event lists are
  equal by key, state, flags and time, and equal to the `cond` lines.
  This joins CI's recording gate.
- **A term on a computed signal:** a level on a `max`, an `abs` and an
  `if` of format 2; a term on a chain going stale when a leaf passes
  its bound, and firing nothing on the way back; the focus keeping a
  chain's leaves asked.
- **The worker:** `CoreTest.WorkersAllocateNothingPerFrame` with the
  largest set and a check term attached; a set swapped under the mock's
  frames in a loop, for the sanitizers.
- **The armed recorder:** nothing on disk before a firing; a window
  starting `before` ahead of it; two conditions' windows joined; a
  stale input ending a window after `after`; a full ring shortening the
  pre-roll and saying so.
- **The file:** every refusal with its line and column; `SchemaKeysTest`
  and `SchemaFixturesTest` over the new schema; `fuzz_condition_file`.
- **The ABI:** `test_c_abi.c`'s offsets, the exports test, the
  regenerated Python declarations.
- **The front ends:** `havne-cli` goldens for each new line;
  `BridgeTest` for the states and events reaching QML; QML tests for
  the banner, the gauge's color and the roles' contrast in every
  palette.

## Alternatives considered

- **Judged in each front end.** Three evaluators (QML, the CLI, Python)
  that drift apart, a 10 Hz view of the values in the app, nothing a
  replay can be held to, and no pre-roll.
- **A thread of its own, fed through a ring.** A copy per value, one
  more thread per session, events parted from their frame, and a reader
  that needs the same thread to agree, to spare the worker a few
  compares.
- **A worker tick settling deadlines on the host's clock.** A silent
  bus would settle them, but an event's time would depend on when a
  thread woke, and a replay could not repeat it.
- **Conditions as computed signals.** The grammar compares (ADR-0062),
  but a program keeps no state, and hysteresis, holds and stale are
  state; a consumer wants changes, not a column of 0 and 1.
- **An expression inside a term.** A second home for arithmetic, with
  its own names, ages and refusals, judging a quantity no recording
  holds.
- **Conditions on format 1's computed signals, the richer grammar
  later.** Enough for a level on one signal, but a level that depends
  on another signal has no form, and the signal file's format would
  move a release after the condition file arrives.
- **Conditions inside signal files.** A condition on `obd2`'s coolant
  would need a signal file, a user's alerts would sit in a file someone
  else wrote, and an older Havne would refuse the whole file for one
  key.
- **Conditions in the app's settings alone.** `havne-cli` and Python
  would have none, and a recording nothing to copy.
- **A set per vehicle in the vehicle book.** Fits levels that differ by
  car, at a set to manage for each car; one set with references serves
  several cars, as the dashboards do.
- **Code in a sandbox** (the conversation tier's WebAssembly,
  ADR-0052). Far more than a comparison needs, and code on the worker
  is what ADR-0015 section 2 rules out.
- **A set changed before start only.** Simpler, but every edit of an
  alert waits for a reconnect.
- **The front end starting a recorder at an event** (ADR-0056
  section 3 as it stands). No new recorder mode, and no pre-roll: the
  seconds before a fault are the ones wanted.
- **Armed windows beside a running log.** Two recorders on one session,
  for a case the log already covers by recording everything.
- **One folder with holes for an armed session.** Every reader assumes
  continuous time: the check would report each hole as a gap, and the
  export's rows would span them.
- **A focus that parks a condition's inputs.** An alert goes stale
  while a narrow log runs, which is when it is wanted.
- **Stale as a third state, or stale as OFF.** A third state makes
  every consumer handle three; OFF makes a slow signal's alert clear
  and fire again each cycle.
- **An age bound that follows each signal's pace** (three times its
  usual interval). A second notion of stale beside the one the gauges
  dim by.
- **A replay judged by the app's current set.** The recording's own
  copy is what fired; another set is the user's to name.
- **A replay that publishes the recorded link states as its own.** A
  replay has no link (ADR-0023); the `cond` lines and
  `ecuio_recording_evaluate` hold what the link did.
- **Link terms and `overflow` first, the per-ID checks with whole-bus
  listening.** A smaller first release, but the check is a stream
  already, and a requesting session's few IDs are where a rate drop
  shows.
- **An alert bound to the gauge's zone, or to the file's level, by
  reference.** An edit of the zone or the file would move an alert
  silently.
- **Alerts that latch until acknowledged.** A tap while driving; the
  fired count and the activity log keep what came and went.
- **A set run over a recording, in a later release.** A replay repeats
  a recording's own set, but a new level tried against old drives is
  how a level gets tuned.
- **A starter set written by core from a signal file's levels**
  (`havne-cli conditions new --signals <file>`). The app's Add and
  Alert start from those levels; the CLI's user writes the file.

## Consequences

- One evaluator gives triggered logging, alerts and markers to the app
  on every platform, `havne-cli` and Python, and a replay or a read of
  a recording can be held to what fired live.
- The release carries two formats: signal-file format 2 for the
  grammar, built first, and the condition file's format 1.
- A quantity an alert judges is a signal: it is shown, recorded and
  replayed, and a condition on a built-in set's arithmetic needs a
  signal file that extends the set.
- The worker does more per frame while a set is attached: a few
  compares, measured and recorded at the build.
- The C ABI grows by one header, a queue kind, a recorder flag and
  fields in reserved room. Nothing breaks, and the recording's format
  number stays.
- A second user-written file format, with its loader, schema, fuzz
  target and documentation page.
- The frames check's per-ID state gains a fixed-table form for live
  use, and a whole-bus listener past 512 IDs is judged in part, and
  says so.
- A silent bus settles no hold and no stale input until it speaks; the
  link's own state is what reports the silence.
- A session that records can't change its conditions without stopping
  the recording, so with Record sessions on, an edit waits for the
  next connect.
- The app keeps one set for every vehicle: two cars that want
  different levels on one signal need two conditions on different
  references, or one level.
- Sound needs a Qt module or a platform call the app doesn't link, and
  a background alert on Android needs a notification.
- An alert is an aid, and DISCLAIMER.md says so.

## Decided

Each choice as taken on 2026-11-10, with the section that holds it.
The computed grammar's decisions (ADR-0062) were taken the same day,
all as recommended: a term holds no expression (section 11).

1. **Where a condition is judged:** on the worker, in the frame decoder
   (section 3).
2. **Deadlines:** settled by the next frame at the deadline's own time,
   so a replay repeats them (section 2).
3. **Where conditions are stored:** a file of their own, with the
   Dashboard's references (section 5).
4. **The app's scope:** one set for every vehicle, as the dashboards.
5. **The computed grammar:** built first, in the same release
   (ADR-0062): functions, conditionals and computed signals that read
   each other, in signal-file format 2. A term reads the result
   (section 11).
6. **A stale input:** the state is kept and flagged; `record` reads it
   as OFF (section 2).
7. **The default age bound:** `ECUIO_VALUE_STALE_US`, with `max_age`
   per term.
8. **Changing a set mid-session:** allowed between two frames, refused
   while a recorder is attached (section 6).
9. **Triggered logging:** an armed recorder in core, the ring as its
   pre-roll, one folder per window (section 7).
10. **Recording modes:** Record sessions, a log and armed exclude each
    other.
11. **The focus:** it keeps an attached condition's inputs asked.
12. **The set on a replay:** the recording's own copy, unless the user
    names another.
13. **Link terms on a replay:** not fired by `replay://`; the
    recording's `cond` lines and `ecuio_recording_evaluate` have them.
14. **The live checks' scope:** every rule of the frames check behind a
    512-ID table, in this release (section 4).
15. **Alerts' order:** roles, colors and the banner first; the sound
    next, through Qt Multimedia or one small call per platform, sized
    at the build; Android's notification last.
16. **An alert from a gauge:** a copy of the zone's level (section 9).
17. **Defaults:** no condition ships switched on; `clear` 2 % of the
    range and no hold for an alert made from a gauge; the banner's 3 s
    and the sound's 5 s.
18. **A set run over a recording:** in this release, as the last step.
19. **A starter set from a signal file's levels:** left to the app's
    Add and Alert.

## Appendix: the loader as built (2026-11-12)

Section 12's step 2: `ecuio/conditions.h`, `core/src/condition_file.cpp`,
`havne-cli conditions check`, `schema/condition-file.schema.json`,
`fuzz_condition_file` and the Python package's `ConditionSet`. No
session, no evaluator. The format's reference is
[condition-files.md](../condition-files.md).

**The ABI.** The header declares only what this step defines: the
load and its release, the count, the file's name and checks, a
condition, a term and the resolve. The events, the states, the
callbacks, `ecuio_session_set_conditions`, the queue kind and the
recorder's flag join it with their steps, since a declaration is
exported the day it is written. `ecuio_condition_info_t` is 48 bytes
and two pointers (the key and the name, borrowed, last),
`ecuio_condition_reference_t` 48 bytes, `ecuio_condition_term_t` 456
and `ecuio_condition_binding_t` 64, the last three with no pointer;
sizes and offsets are asserted in the header and in `test_c_abi.c` on
both pointer widths. Section 6 gave a condition's term count only: the
term getter, the resolve and the getters for the file's name and
checks were added because `havne-cli` includes public headers alone and
must show what the loader read.

**Settled while building,** where sections 5 and 6 were silent:

- **The file:** `name` is optional (1 to 63 bytes), and `conditions`
  is required and may be empty, so the app's set with nothing in it is a
  file. `checks` takes `gap` and `settle` above 0 and up to 3600 s and
  `rate_drop` above 1 and up to 1000, read to hundredths: the `info`
  command's rules, with upper bounds.
- **A term** is exactly one of `signal`, `link` and `check`, and a
  signal term takes exactly one of `above`, `below`, `equals` and
  `differs`. `unit`, `clear` and `max_age` are a signal term's; `clear`
  goes with `above` and `below` only, and one equal to its level is
  taken (it reads as none).
- **The unit:** the key is required with `above` and `below`, and
  `""` is taken for a number with no unit, so a level on a ratio has a
  form. Without the key the unit reads as `""`. At the bind the term's
  unit equals the def's byte for byte, or the term is unavailable.
- **References** are exactly three forms, each signal once in a list
  (an `obd2.<name>` and its `id:` are one signal), and an `obd2.<name>`
  that names none of `obd2`'s signals is refused at load. ADR-0057
  keeps a reference of an unknown form and matches nothing; this loader
  refuses it, and a new form moves the format's number. At the bind the
  first reference the set provides is taken and nothing further is
  searched, and an ID from 20000 up names nothing.
- **A def a signal file replaced by name** keeps the base def's ID and
  gains a key (ADR-0051's appendix of 2026-11-10). `id:<n>` matches it,
  so a condition on `obd2.map` still binds on a vehicle whose file
  replaces that signal, and `key:` reaches it too.
- **Uses:** an `alert` needs its `level` (section 5's "a use that names
  nothing" is `"alert": {}`); `sound` and `banner` are false when left
  out. A `record` may be `{}`, which records while ON. `mark` is true
  or false, and false is no use.
- **Names** are given once in a file, as keys are: a name labels its
  condition's markers, and two runs align on it.
- **Times** are JSON numbers in seconds, rounded to the nearest
  microsecond.
- **What makes a condition unavailable** against a set, in the order
  judged for the def a term took: text or hex, an enumeration under
  `above` or `below`, then the unit. The first term that doesn't bind
  is the one named, and `"enabled": false` is reported in place of any
  of them.

**`havne-cli conditions check <file> [--signals <file> | --profile
<name>]`** writes a `condition_set` line (the file's name, its count,
the check thresholds in force, the profile resolved against) and a
`condition_def` line for each condition as read; with a set, each
signal term gains `resolved` and the line `available`, `why` and
`why_term`. The types `conditions` and `condition` are left for
section 10's `stream --conditions`. A refused file, a refused signal
file and a profile that isn't registered are each an `error` line and
exit 3.

**The schema** is held to the loader as the signal file's is
(ADR-0051's appendix "the JSON Schema"): each object's keys, the link,
check and level words in the loader's order, the refused fixtures at
their rule and in the loader's words, and edge cases with one verdict
from the schema and from `conditions check`.

**Python:** `havne.ConditionSet` reads a file from its text, bytes or
path into named tuples, and `resolve()` takes a profile's name, a
`SignalFile` or nothing.

**Not in this step:** a check against a plugin's own signals
(`conditions check --plugin`, `ConditionSet.resolve` with a `Plugin`)
comes with step 3's lines.

**Tests:** `ConditionFileTest` (each refusal with its line, column and
words; the bounds at their edges; the resolve against `obd2`, the
mock, no set and a signal file; every fixture loaded, since the
fixtures are the fuzz target's seeds), `CliConditionTest`, goldens and
usage cases for `conditions check`, `SchemaKeysTest`,
`SchemaFixturesTest`, `PythonTest.test_conditions`, `c_abi`, and
`fuzz_condition_file`.

## Appendix: the evaluator as built (2026-11-12)

Section 12's step 3: signal terms judged in a session with section 2's
timing, the callback, the states, the pull queue's third kind,
`havne-cli stream --conditions`, the Python package's side, and the
desk check against a plugin's variant. Link and check terms (step 6)
aren't judged, and nothing is recorded yet (step 4).

**Where it runs.** `core/src/condition_eval.{h,cpp}` holds a set bound
to a signal set and its state, with one entry, `judge(time, values)`.
The session's dispatch calls it right after the frame's decode, so it
sees the values the callbacks get, computed ones included, and step 8's
reader calls the same pair. On the worker, one frame goes: frame
callbacks, frame queues, the decode, the judgment, the recorder, values
callbacks, values queues, conditions callbacks, conditions queues. A
frame's events arrive in one call after its values, ordered by time and
then by the set's order; an event stamped with a deadline's time,
earlier than the frame's, still comes after that frame's values, since
the frame is what settled it. A session decodes every frame while a set
is attached, a values consumer or not, and a frame that decodes nothing
still settles what is due.

**The swap.** The session keeps three slots under the mutex its
dispatch already takes for each frame: the worker's set, a pending one
and a retired one. `ecuio_session_set_conditions` builds the next set
on the caller's thread and puts it in the pending slot; the next
dispatch, inside that lock, moves its own into the retired slot and
takes the pending one. The caller frees a retired set, and a pending
one the worker never took, at its next call. So a frame gains no
synchronization, the worker frees nothing, the caller never frees a set
the worker holds, and any number of calls between two frames leave only
the last to be judged. Inside that same lock the call takes its
generation and writes the new set's states before it fills the pending
slot: the worker can take the set and judge a frame the moment the lock
goes, and it writes only states of its own generation, so states
written after the handover would overwrite what that frame changed. Two
callers at once each get a generation of their own.

**What an event is.** A change of a condition's state or of its STALE
flag; the changes one frame settles at one time are one event. The
first judgment after an attach is an event, the attach is none. One
frame gives one condition three events at most: a hold's end, an age
bound, and the frame's own judgment. The buffer holds three for each
condition that binds; the evaluator checks the bound where it writes
(an assert, and a counted drop in a release build), and the fuzz target
holds the count to zero. That count is the evaluator's own and no call
reads it: the ABI gains nothing for a number that is zero unless the
evaluator is wrong, and a field for it can take reserved room when
something needs to read it.

**Changed from section 6,** each for a reason found while building:

- **A generation** in the event and in the state, in their reserved
  room. A set can be replaced mid-session, and an event of the set
  before can still sit in a queue, in a front end's staging, or in a
  callback under way when the call returns, where its index would be
  read against the new set. The generation is 1 for a session's first
  attach and moves by one with each, a removal too.
- **A state's value** is the settling term's sample at its last event,
  not its newest: the states are published only when one changes, so
  the getter never waits on a frame and a frame never takes the lock
  for a sample alone. A live value is the values path's.
- **Two reasons** join the list for the terms this step doesn't judge:
  `NO_LINK` and `NO_CHECK`. Both stay true after step 6: a `replay://`
  session has no link health, and a transport that reassembles runs no
  `isotp` check.
- **No event at the close.** A closed session delivers nothing, so a
  consumer reads the close as the end of every condition; the states
  keep what they last held.

**Settled while building:**

- **A value that is no number is no sample.** A FLOAT field can carry a
  NaN off the wire. It compares with no level, so the evaluator passes
  it by: the term keeps its truth and its sample's age runs on, as a
  computed signal treats a NaN input as no value. An infinity is a
  number, on one side of every level.
- **Time never goes back.** A session's frame times rise on each
  channel, not across channels, so a frame can be stamped a little
  earlier than the one dispatched before it. The evaluator judges such a
  frame at the time it had already reached. The alternative, taking the
  earlier stamp, would let a sample move a deadline earlier (which the
  deadline index rules out) and an event carry a time before one
  already delivered.
- **A hold that ends exactly at a frame's time** is settled after that
  frame's values, if they didn't break it. A sample exactly at its age
  bound is still fresh, as a computed signal's input is.
- **A term keeps its truth across a stale stretch,** so a sample inside
  its hysteresis band afterward reads as before, and a condition that
  comes back to the state it had fires nothing.
- **A quiet link.** Deadlines are settled by frames (section 2), so a
  STALE that is due shows with the next frame and not before. The
  workers have no wake in common that carries a session time: each
  waits in its transport's own way, and a tick on the host's clock is
  the alternative this record sets aside, since a replay could not
  repeat it. Until a frame comes, a front end has the values' own ages
  (a gauge dims at `ECUIO_VALUE_STALE_US` on the host's clock), and
  step 6's link term reads link health, which reports a silent bus
  within about a second on its own path.
- **A recorder attached refuses a new set** (`ECUIO_ERR_IN_USE`): step
  4 copies the set into the recording and writes its events against it,
  so a recording holds one set for its whole length. Changing alerts
  during a log would need the recording to hold a second set from some
  point on; that is step 7's to decide with its editor, and allowing
  later what is refused here breaks no caller.
- **The time on `stream`'s line** is `t_us`, integer microseconds, as
  every other line's, where section 10's example had seconds.

**The cost.** Each watched signal keeps the levels and clears of its
terms sorted. A term's truth changes only when a sample lands on or
crosses one of them, so a sample visits the points between the sample
before it and itself and no other term; a term not yet judged, or back
from stale, has its signal's terms visited whole once. A sample moves a
deadline later, never earlier, so each condition keeps a time no later
than its earliest deadline and is examined only when a frame passes it,
about once per age bound. Measured on the replay fixture's frames
through the session's dispatch (`bench_bridge --decode`, Windows,
Release; each figure a run's median of nine rounds, the lowest of seven
runs, on a machine other builds were loading, so an upper bound):

| | ns a frame |
|---|---|
| obd2, no set | 53 |
| obd2, a typical set (8 conditions, 12 terms) | 61 |
| obd2, the largest set (64 conditions of 4 terms, 18 terms a value) | 70 |
| obd2, no values consumer, no set (nothing is decoded) | 16 |
| obd2, no values consumer, a typical set (decoded for the conditions) | 59 |

The largest set cost 155 ns before the points were sorted. A session
with no set attached pays one test of a pointer: the conditions'
callbacks are listed only by a frame that changed something. A/B against
the commit before (`scripts/bench_ab.py decode`, seven runs of each,
alternated), with no set attached, the lowest run of each: obd2 alone
51.7 ns before and 53.0 after, eight computed signals 90.8 and 90.5, a
chain of eight 195.3 and 192.9. Under that load one build's runs spread
further apart than any of the three differences.

The saturated run section 3 names goes through the bridge, which can't
attach a set before step 7. Core's own replay worker looping at full
speed, with a values consumer, is the stricter stand-in, since a frame
costs it about 260 ns in all: 3.80 million frames a second with no set,
3.61 million with the typical set (0.95) and 3.32 million with the
largest (0.87), medians of five 1.5 s runs that spread by 15 % or more
on the loaded machine. So section 3's 5 % is met for the typical set
and not shown for the largest on that worker. The bridge's own load run
with no set attached (`scripts/bench_ab.py load`, five runs of each)
gave 0.36 million worker frames a second before and 0.40 million after,
inside the runs' spread (0.27 to 0.44 million): about 2.5 us a frame,
of which the largest set's 17 ns would be under 1 % by arithmetic. The
bridge's run with the largest set attached is step 7's to make, and
both tables are measured again on a quiet machine before the release.
The largest set holds 44,480 bytes of state for a session.

**The front ends.** `havne-cli stream <uri> --conditions <file>` reads
the file before the connect, attaches it before a recording begins,
writes a `conditions` line (the name, the generation, each key with its
uses, `available` and `why`, the session's own reasons among them) and
a `condition` line for each event (`t_us`, `key`, `state`, `fired`,
`stale`, `value`, `term`), from the worker as a values line is, after
the frame that settled it. A live session's stream drops lines when its
reader falls behind; a lost event is a change of state the reader never
sees, so the `end` line counts, in `conditions_dropped` (written only
with `--conditions`), the changes whose line was dropped, with its
frame or alone. `conditions check` takes `--plugin <pkg>
[--variant <id>]`, resolving against a plugin variant's set and keys:
its only variant, else the one named. In Python, `Session` gains
`set_conditions`, `conditions()` (a stream of `ConditionEvent`) and
`condition_states()`, and `ConditionSet.resolve` takes a `Plugin`. The
package names an event by its attach's keys, stored before the call
that attaches them, and keeps the newest 64 attaches' keys.

**Tests:** `ConditionEvalTest` (the rules, fed by hand: hysteresis,
holds, the join, stale, deadlines at their own time, three events in
one frame, the same events however the frames are split),
`ConditionSessionTest` (the attach and its states, the order after the
values callback, attaches back to back, a swap under a running worker,
the recorder's refusal, the queue, no allocation, a computed signal's
term, a replay of a recording giving the live session's events),
`CliConditionTest`, goldens and usage cases, `PythonTest.test_conditions`
(the replay's events equal to `havne-cli`'s lines), `c_abi`, and
`fuzz_condition_file`, which judges each loaded set.

## Appendix: the focus keeping a condition's signals, as built (2026-11-13)

Step 5's focus half (section 7, decision 11). While a focus is on, the
requests carrying an attached condition's signals stay asked; a
computed signal counts as its inputs, through its chain. With no focus
nothing changes. A disabled condition, and one that can't be judged on
the session (its signal missing, its unit another, a link or check
term), keeps nothing asked: the signals kept are the ones the
evaluator judges.

`ecuio_session_set_conditions` works the conditions' signals out once,
as one byte for each def of the session's set, and the worker adds
them to the picks each time it takes the focus. Conditions that change
the signals kept, attached or removed under a focus, start a new sweep,
as a focus call does, and the minimum time between sweeps stands; the
same conditions again, or any attach with no focus, change nothing, so
a caller re-attaching them can't keep a sweep from ending.

Only the caller's picks decide whether a focus leaves something to
request: a focus none of whose picks the session can ask is refused
even when a condition's signal would be asked. The call's answer then
never depends on which conditions are loaded. The focus's own picks,
and a recording's `focus` line, stay the caller's.
