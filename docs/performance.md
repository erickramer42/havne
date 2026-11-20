# Performance

Measurements of the stack's cost, each dated with how it was taken (machine, build, runs).
Each section names its bench or test and how to run it.

### Cost per frame, by profile size

Result: the worker's lookup and decode cost about 0.13 microseconds a frame at any profile size, and the GUI thread's tick grows linearly with it (about 19 microseconds at 1000 defs).

`tests/bench_bridge.cpp` and the headers beside it,
`tests/bench_bridge_*.h` (target `bench_bridge`, built with the tests,
never run by ctest) measure each thread's work per frame through the
bridge, by the size of the session's profile:
- **On the worker** ([ADR-0024](adr/0024-decode-on-the-worker.md)):
  `onFrame` (the frame's 4 KB copy and the queued call), core's lookup
  of the defs the frame can match (by its CAN ID and key,
  `core/src/def_index.h`) and their decode into a batch, and the
  bridge's staging of that batch.
- **On the GUI thread:** the drain (the queued call, the frame log and
  the counters), and one 100 ms tick's update of a model of N rows.

Recording is off and no view is attached. The defs sit four to a CAN
ID, 16 bits each, as a passive-bus profile packs them, so each frame
decodes the four values on its ID. A raw session, which decodes
nothing, is the baseline.

```sh
build/tests/Release/bench_bridge.exe [frames per run, default 20000]
```

On the dev machine, 2026-10-31, with no heavy process running
(Release; three runs of 20,000 frames each, the range across them):

| Defs | Worker: lookup and decode, microseconds a frame | GUI thread: one tick's update, microseconds |
|---|---|---|
| 9 | 0.08 | 0.3 |
| 100 | 0.13 to 0.15 | 1.9 to 2.2 |
| 250 | 0.13 to 0.14 | 4.8 to 5.8 |
| 500 | 0.13 to 0.16 | 9.5 to 10.9 |
| 1000 | 0.13 | 18.6 to 19.6 |

- **The lookup and decode cost the same at any profile size:** a frame
  is checked only against the defs on its CAN ID with its key, so the
  cost follows the values a frame carries (four here), not the
  profile. Checking every def instead cost about 1.7 ns a def a frame
  (1.76 microseconds at 1000 defs, 2026-10-27).
- **The 4 KB copy is the worker's largest cost:** `onFrame` (the copy
  and the queued call) takes 0.9 to 1.9 microseconds at any profile
  size, several times the lookup and decode.
- **The GUI thread's cost per frame doesn't grow with the profile:**
  its drain stays under 0.4 us from a raw session to 1000 defs, because
  the decode runs on the worker. The bridge's staging of a batch is
  near the bench's resolution at every size.
- **The tick's update is linear in the profile:** under 0.02 % of the
  GUI thread at 1000 defs and 10 Hz. Each window keeps every signal's
  newest, lowest and highest value
  ([ADR-0005](adr/0005-coalesced-signal-updates.md)'s appendix).
- **The engine-start fixture under `obd2`** (7,074 frames, 297 s
  recorded, peaking at 1,261 rpm): at `speed=10` and at `max`, no frame
  dropped, and the strip chart's highest RPM is 1,261 at both.
- **Not measured:** a QML view attached to the models; text defs.

### Load test: the replay fixture through the bridge

Result: the worker sustains 375,000 to 565,000 frames a second, more than 80 times a fully loaded classic CAN bus (about 4,500 frames a second).

`bench_bridge --load` replays the engine-start fixture (7,074 frames
over 297 s recorded, under `obd2`, RPM peaking at 1,261.25) through a
real `SessionBridge`: once at full speed, once at 100x (paced, about
3 s), and looping at full speed for 3 s. The full bench adds a 10x run
(about 30 s) after its per-profile table. Each run reports the bridge's
own delivery figures, the ones the activity log's "Delivery:" line
gives at every disconnect:
- **Worker frames/s:** frames through the worker's whole path: the
  replay's read, core's decode and the bridge's two callbacks.
- **Frame to GUI thread:** from `onFrame` on the worker to the frame
  log's handler on the GUI thread.
- **Value to model:** from `onValues` on the worker to the 100 ms tick
  that puts the value in the model the pages draw from.

Latencies are read at the top of a histogram bucket, at most 12.5 %
above the true value; the maximum is exact. QML's painting isn't
included: the bench runs headless.

Three runs on the dev machine, 2026-10-27, with no heavy process
running (ranges across the runs; milliseconds as median / p99):

| Run | Frames handled / dropped | Worker frames/s | Frame to GUI thread | Value to model | Chart's peak, rpm |
|---|---|---|---|---|---|
| Full speed, one pass (14-16 ms) | 7,074 / 0 | | 0.09-0.10 / 0.38-1.28 | 98-115 / 100-116 | 1,261.25 |
| 100x, one pass (2.98 s) | 7,074 / 0 | | 0.08-0.10 / 0.38-0.41, one run 28.7 | 49-57 / 110-113, one run 359 | 1,261.25 |
| 10x, one pass (29.75 s) | 7,074 / 0 | | 0.04-0.05 / 0.32-0.35 | 49 / 112-115 | 1,261.25 |
| Full speed, looping 3 s | 1.44-1.72 million / 0-955 | 417,000-474,000 | 0.24-0.26 / 1.15-1.28 | 90-98 / 111-113 | |

- **Two ratios gate CI** whatever the runner's speed, each comparing
  the bench with itself: core's decode at 1,000 defs within 3x its
  cost at 9 (1.26 to 1.94x in four runs on the dev machine,
  2026-11-01), and the looping run under `obd2` at least 0.5 of the
  same run under `raw`, which decodes nothing (0.69 to 0.76). With the
  decode visiting every def instead of the ones the index finds, both
  failed (27x, 0.48), where the 10,000 frames a second floor still
  passed at 229,915.
- **Recorded runs:** the looping run and the 100x pass run again with
  core's recorder attached ([ADR-0049](adr/0049-recordings-in-core.md)).
  The looping pair runs twice, unrecorded and recorded alternated, and
  the faster recorded run gates CI at 0.6 of the faster unrecorded one
  (0.96 on the dev machine, 471,840 frames a second against 493,534, and
  1.01 under WSL, 2026-11-03, no record dropped in either); the 100x one must write all 7,074 frames and
  7,061 values with no record dropped. On a shared runner the share
  measures the runner as much as the recorder: the writer is a third
  busy thread on two cores, and the bridge's worker rate follows the
  GUI thread's through the in-flight gate (holding the worker to a core
  of its own moved the share to 0.94-1.13 on two of the dev machine's
  cores, so the bench doesn't). Over 63 CI runs each, 2026-11-03 to
  2026-11-05, Linux read 0.79 to 0.95 (median 0.89) and Windows 0.68 to
  0.95 (median 0.80), most of the spread runner to runner; a loaded Linux
  runner has read 0.67. So the gate
  is for a gross regression: a 25% loss from a typical share still
  trips it, about 2 us a frame on Windows' typical runner and 1 us on
  its fastest (mutation-checked
  2026-11-05: a busy-wait at `ecuio_recorder_put_frame`'s entry,
  measured at about 3 and 4.5 us a frame on the dev machine, read 0.38
  and 0.28). The recorder's own
  nanoseconds a frame, and ADR-0049's 10%, are `--record`'s, pinned, on
  the dev machine.
- **The looping runs pass the staging's capacity:** at about 400,000
  frames a second the bridge counts most values past it, as
  [ADR-0024](adr/0024-decode-on-the-worker.md) bounds it; the model
  still gets each signal's newest value every tick, and only the
  one-pass runs, at the recording's own rate or a multiple, are held to
  timing every value.
- **Throughput:** across thirteen runs that day (the three above and
  ten more), the worker sustains 375,000 to 565,000 frames a second,
  more than 80 times a fully loaded classic CAN bus (about 4,500
  frames a second).
- **At full speed the in-flight cap drops frames**
  ([ADR-0003](adr/0003-backpressure-bounds-in-flight-frames.md)): a
  one-pass run sends the 7,074 frames in about 15 ms, faster than the
  GUI thread drains them. Five of the ten further runs dropped 1,322
  to 6,074 of them; in the worst the GUI thread waited 22 ms and only
  the cap's 1,000 got through. One looping run in thirteen dropped
  955 of 1.72 million. Every value of a dropped frame still reaches
  the pages ([ADR-0024](adr/0024-decode-on-the-worker.md)): every
  one-pass run timed all 7,061 values and kept the peak.
- **One stall in thirteen runs:** in one 100x pass the GUI thread was
  held up once, up to 316 ms for a frame and 359 ms at p99 for a
  value. Nothing dropped and the peak held. The other twelve stayed
  under 0.6 ms at p99 to the GUI thread, and the stall's cause wasn't
  found. CI's bounds (500 ms and 1,000 ms at p99) have room for a stall
  that size, but not much.
- **Values wait for the tick, by design**
  ([ADR-0005](adr/0005-coalesced-signal-updates.md)): paced, the
  median is about half a tick and p99 a tick plus the timer's jitter.
  At full speed a whole pass arrives within milliseconds of a tick and
  waits out the rest of it.
- **Frames reach the GUI thread** in under a quarter of a millisecond
  at the median, and within 1 to 3 ms at p99 even at full speed.
- **The limit it found:** at full speed the staging holds 16,384
  values a window, 164,000 a second, against 480,000 to 570,000
  arriving, and the Delivery line counts the rest. The pages lose
  nothing: each window keeps every signal's newest, lowest and highest
  value whatever the limit. The recording loses nothing either: core
  writes it from the worker, past the staging. A full classic CAN bus
  at four values a frame gives 18,000 a second.
- **In CI:** `win-build` and `linux-build` run `bench_bridge --load`
  on every run and put its table in the run's summary, so each run
  publishes a second and third machine's figures. The step fails on a
  broken invariant (a one-pass run must account for every frame, time
  every value and keep the 1,261.25 rpm peak) or a bound: at least
  10,000 frames a second through the worker, and at 100x a p99 under
  500 ms to the GUI thread and 1,000 ms to the model. The bounds sit
  far from the dev machine's figures because shared runners vary; they
  catch a regression of an order of magnitude, and smaller drift shows
  in the summaries. macOS doesn't run it: its runner wakes timers
  100-200 ms late.
- **Measured again 2026-10-31, v0.3.5's `dev`** (three runs; the
  machine otherwise near idle, 9 % CPU): every one-pass run handled all
  7,074 frames with none dropped, timed all 7,061 values and kept the
  1,261.25 rpm peak; frames reached the GUI thread in 0.07 to 0.09 ms
  at the median and 0.35 to 0.64 ms at p99. Sustained, looping at full
  speed, the worker ran 191,000 to 193,000 frames a second, against
  358,000 to 374,000 for v0.3.4's build run the same minute on the same
  machine. The difference was the obd2 set's size: it grew to every
  field of J1979's Service 01 table, its defs share the engine ECU's
  reply ID, and the worker checked each frame against every def, so
  each one read its mux key. With decoding turned off, the builds
  before and after that change both ran about 460,000 frames a second.
  With the lookup by ID and key (the cost per frame above), the same
  test runs 344,000 to 450,000 frames a second, against 191,000 to
  282,000 for the build before it, the two alternated run by run.

### Recording's cost on the worker

Result: recorded, the bridge's worker kept 0.96 of its rate; core's recording costs 31 to 47 ns a frame.

`bench_bridge --record` first runs the load test's looping run through
a `SessionBridge`, recorded and unrecorded alternated, five of 3 s
each, with the process held to one logical processor on each of three
physical cores (Windows and Linux; elsewhere the bench says it isn't,
and where the threads land moves the figures by tens of percent). That
is the run [ADR-0049](adr/0049-recordings-in-core.md)'s 10% is judged
on, and the bench fails below 0.9. On the dev machine, 2026-11-03
(Release; frames a second through the worker, median and range):

| Run | Worker frames/s |
|---|---|
| Not recorded | 534,618 (515,656 to 542,733) |
| Recorded | 514,763 (495,563 to 556,927) |

Recorded, the bridge's worker kept 0.96 of its rate. At over a hundred
times a saturated classic bus's rate, the writer, sharing those three
cores, fell behind and dropped 22,125 records, each counted.

Then it replays the engine-start fixture under `obd2` on core's own
session, looping at full speed, to a frame callback that counts and a
values callback that does nothing, as the bridge's are, alternated the
same way on two physical cores. Then two recorded
passes, at full speed and at 100x, must put every frame and value in
the files with none dropped. Exit status 1 on a failed check; not run
in CI.

On the dev machine, 2026-11-02 (Release; frames a second through the
worker, median and range):

| Run | Worker frames/s |
|---|---|
| Not recorded | 5.08 million (4.85 to 5.11) |
| Recorded | 4.18 million (4.08 to 4.28) |

- **Recording costs this worker 31 to 47 ns a frame** (0.81 to 0.86 of
  its rate in four runs; 41 and 42 ns under WSL, where the unrecorded
  worker runs about 8 million frames a second, so 0.75 of its rate),
  about 20 ns of it on every frame and the rest on each record
  written. A frame costs this worker only about 0.2 us, so the share
  is large; the bridge's worker spends about 2.5 us on one, and the
  10% is judged on its run, above (the ADR's appendices "the worker's
  cost, measured" and "the bridge's cost, measured"). The check
  here bounds the nanoseconds against regressions: at most 75 a
  frame, which a busy loop on every recorded frame fails (107 and
  142 ns).
- **The writer** wrote about a million frame rows a second on a core of
  its own, and dropped three records in four at this rate.
- **Both recorded passes were whole:** all 7,074 frames and 7,061
  values, none dropped; the full-speed pass's files were complete
  about 0.12 s after it started, the 100x pass's within 70 ms of its
  last frame.

### Opening a recording

Result: an hour opens in 5.9 to 10.0 ms (the fixture's) and 114 to 213 ms (dense), on the laptop and the test phone.

`tests/bench_recording.cpp` (target `bench_recording`, Qt-free, not in
ctest) writes two hours into a scratch folder and opens each five
times, printing the median, fastest, slowest and first open and the
reader's tables against [ADR-0049](adr/0049-recordings-in-core.md)
section 5's bound: section 9's hour (the replay fixture's values laid
down twelve times, `tests/recording_hour.h`, which `RecordingTest`
checks) and a dense one, 50 number signals at 10 Hz. `--open <folder>`
times any recording instead. It exits 1 past `--bound` seconds (2 by
default; 10 for the phone) or the table bound. On the phone it runs
from an NDK build of core alone, which builds only this target
(`BUILD_TESTING` on, so `tests/` is configured; building another test
would run it on the host to list its cases):

```sh
cmake -B build-android-bench -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=<ndk>/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=31 \
  -DHAVNE_BUILD_CORE_ONLY=ON -DBUILD_TESTING=ON
cmake --build build-android-bench --target bench_recording
adb push build-android-bench/tests/bench_recording /data/local/tmp/
adb push tests/fixtures/replay/2026-10-25-engine-start /data/local/tmp/
adb shell /data/local/tmp/bench_recording --fixture /data/local/tmp/2026-10-25-engine-start \
  --scratch /data/local/tmp --bound 10
```

On 2026-11-03 (medians; the files in the system's cache, the bench
having just written them):

| Hour | Rows | `signals.csv` | Laptop (MSVC) | Laptop (GCC, WSL) | Test phone | Test phone, shared storage |
|---|---|---|---|---|---|---|
| The fixture's, 130 signals | 84,732 | 2.8 MB | 8.9 ms | 5.9 ms | 9.1 ms | 10.0 ms |
| Dense, 50 signals at 10 Hz | 1,800,000 | 50.2 MB | 167 ms | 114 ms | 204 ms | 213 ms |

The tables came to the bound exactly, 1.04 MB and 5.82 MB, every second
of both hours having samples. A second run on the phone agreed within
a millisecond. A cold open, which needs root to arrange on the phone,
isn't measured.

On 2026-11-18, the app's own open of a recording of 640,000 frames under
obd2 (the replay fixture looped, 131 signals, 25-28 MB of `frames.csv`),
files in the system's cache, on the laptop:

| Span | Core: open, then the check | The Recordings page: click to the check shown |
|---|---|---|
| 12.8 h | 73 ms, 276 ms | 1.6 s |
| 38 min | 62 ms, 240 ms | 1.8-3.2 s |
| 1.2 s | 58 ms, 223 ms | 1.5 s |

The open and the check run off the GUI thread, which answered within
35 ms throughout; with 12,900 findings from the check, within 190 ms. A
replay of the same files at 100x kept it within 20 ms on the Dashboard.

### Dashboard: a plugin's pages in the Add page list

`DashEdits.loadPluginPages` reads each page of each loaded plugin at
the list's open, one `DashboardBook.importProblem` for each, so a page's
name is known before it is shown. 3 plugins x 16 pages x 24 cells (the
bounds' worst case) opened the list in 92 ms in `test_qml` (2026-11-19,
offscreen, software renderer, Release).

### Drawing a recording

Result: charts drawn a second are 141 and 124 on the laptop and 61-62 on the phone, its ceiling.

The Recordings page's frame test (developer mode, **Frame test** under
the chart) plots four signals and moves the view every frame for six
seconds, panning a tenth of the recording a second for half and
zooming in and out about the middle for half, then puts the view back
and writes, on the page and in the activity log, the frames the window
showed in each half, the charts drawn a second and how long a chart's
paint took. Charts drawn are the measure: the chart draws on the GUI
thread, so each one is in the next frame, while the window shows frames
that hold no new chart too. To run it on `bench_recording --keep`'s two
hours, copy each kept folder into the app's recordings folder with a
`frames.csv` of the header row alone (the list shows only folders that
have one), open it, and remove the copies after. On the phone, the
phone skill's `install local` and `adb push` into
`Android/data/<package>/files/Documents/Havne/recordings`.

On 2026-11-03 ([ADR-0049](adr/0049-recordings-in-core.md)'s appendix
"the viewer"), charts drawn a second:

| Hour | Laptop, 1020 px plot, 144 Hz screen | Test phone, 344 px plot, 120 Hz screen |
|---|---|---|
| The fixture's | 141 | 61-62 |
| Dense, 50 signals at 10 Hz | 124 | 62 |

The phone's frame callback ran about 62 times a second, its ceiling
there. A chart drawn with antialiasing on cost the phone's dense hour
two thirds of its rate (21 a second), so the chart draws without it
while the view moves and once with it at rest.

The traces are a Qt Quick Shape, drawn on the GPU, over the Canvas that
draws the rest. On 2026-11-06, the test phone with a 2 min 45 s
`mock://demo` recording and the chart given the page (every signal
plotted, 3.75 device pixels a logical one), against the same build drawing its traces
on the Canvas: the GUI thread's time on the CPU after each of ten taps
(`atrace`'s scheduler events) was 43-73 ms, against 142-158, and a
600 ms swipe showed 35-38 frames (SurfaceFlinger's frame times), against
12-16, all after the swipe ended. A profile of the Canvas build put most
of a tap's time in rasterizing the antialiased strokes and the band
fills at the device's resolution. The frame test's four signals in the
page's 356 px chart stayed at the ceiling (about 62 charts a second) in
both, a paint taking about 4.8 ms against 4.1.

### Reply timing against the fakes

Result: on Linux the stack adds about 0.1 ms, and on Windows J2534 reads a 4 ms reply at the reply.

`tests/bench_timing.cpp` (target `bench_timing`, built with the tests,
never run by ctest; [ADR-0042](adr/0042-reply-timing.md) section 4)
runs an `obd2` session, the rotation alone, through each transport the
platform has against the fakes' ECU, then prints the session's timing
report: the fake J2534 DLL as the VX (each reply 4 ms after its write),
the fake ELM327 (4 ms after its request), and on Linux SocketCAN on
`vcan0` against the fakes' ECU through the kernel's ISO-TP (at once).
The fakes answer from canned bytes at a fixed delay, so the spread is
the stack's, not an ECU's. Latency is on the host's clock; the
intervals are PID 0C's (RPM), on the clock the table names.

```sh
build/tests/Release/bench_timing.exe [seconds per transport, default 60]
```

On the dev machine, 2026-10-31, 180 s per transport (the machine
otherwise near idle), packing on, 92 requests in the rotation:

| Transport | Fake's delay | Latency P50 (lowest to highest request) / P99 / max, ms | PID 0C's interval P50 / P99 / P99.9, s (clock) | ISO-TP |
|---|---|---|---|---|
| J2534, the fake as the VX (Windows, 2026-11-04, 60 s) | 4 ms | 6.7 to 7.2 / 9.2 / 9.5 | 0.164 / 0.180 / n/a, 368 intervals (adapter) | the adapter's |
| ELM327, the fake (Windows, 2026-11-01, 60 s) | 4 ms | 4.6 / 6.1 / 6.5 | 0.164 / 0.180 / n/a, 377 (host) | 2,873 messages, none dropped |
| ELM327, the fake (Linux, WSL) | 4 ms | 4.1 / 4.2 / 4.2 | 0.148 / 0.164 / 0.246, 1,178 (host) | 8,882 messages, 1 dropped |
| SocketCAN, `vcan0` (Linux, WSL) | none | 0.1 to 0.2 / 0.4 / 4.7 | 0.148 / 0.164 / 0.164, 1,194 (kernel) | 9,000 messages, none dropped |

- **On Linux the stack adds about 0.1 ms:** the ELM327 fake's 4 ms
  reads 4.1 at the median and 4.2 at p99, and SocketCAN through the
  kernel's ISO-TP answers in 0.1 to 0.2 ms.
- **On Windows, J2534 reads a 4 ms reply at the reply:** the VX holds
  a read for its timeout unless the batch fills, so the surrogate's
  call asks for one message and the read answers with it, on the
  surrogate's 1 ms timer. A read held
  to its 10 ms slice reads the same reply at 11.3, and at Windows'
  default 15.6 ms step at 16.4
  ([ADR-0042](adr/0042-reply-timing.md)'s appendix of 2026-11-01 and
  its note of 2026-11-04); a read asked to wait 0 ms, which the VX
  holds about 50 ms, reads a reply past the slice about 50 ms late
  ([2026-11-04-two-ecu-car](bench-sessions/2026-11-04-two-ecu-car.md)).
- **ELM327 on Windows adds nothing either:** the bench holds a 1 ms
  timer for the fake, whose timed 4 ms wait still runs up to a
  millisecond long (4.6 to 5.1); with the fake spinning to its reply
  time instead, at the default step, ELM327 read 4.0 to 4.1 ms and PID
  0C refreshed every 0.148 s, Linux's figures (2026-11-01).
- **The refresh interval follows the latency:** the rotation is the
  same 92 requests, packed, so PID 0C refreshes every 0.148 s where a
  reply takes 4 ms, 0.164 s where it takes 5 to 11, and every 0.23 to
  0.25 s where it took 16 ms (2026-10-31, at Windows' default step). No
  interval crossed a clock's fallback. P99.9 needs 1,000 intervals,
  which 180 s gives only on Linux.
- **The ELM327 rows' one dropped message** (the Linux row, measured
  before the fix) was PID 68's first reply, about 6 s in: its defs fit
  a Single Frame, so it went with the count digit `1`, and the fake cut
  its two-frame reply after the first. The digit follows J1979's length
  too, and the Windows rows' 60 s run (2026-11-01, a sweep through the
  phone's relay running beside it) dropped none and timed none out.

### The pacing model (virtual time)

`tests/bench_pacing.cpp` (target `bench_pacing`, built with the tests,
never run by ctest) drives the request engine on a simulated clock
against the simulated car: each transport's worker is modeled by its
own calls (`tests/pacing_bench.h`), each module's reply timing is drawn
from the test car's (request to first frame P50 3.46 ms, P99 10.93 ms;
[2026-11-03-socketcan-car](bench-sessions/2026-11-03-socketcan-car.md)),
the replies share one 500 kbit/s bus, and the replies are counted by
`log`'s own tally. A run gives the same figures on every machine.
`PacingTest` (`test_pacing`) pins the engine's pacing against it in
CI. The J2534 model (the OBDX Pro VX) and the MX+ model are fitted to
the car's runs of
[2026-11-18-pacing-car](bench-sessions/2026-11-18-pacing-car.md); the VX
in ELM mode and SocketCAN keep their earlier constants. `bench_pacing`
prints the car's runs against the model after its main table.

```sh
build/tests/Release/bench_pacing.exe [seconds, default 70]
```

[ADR-0042](adr/0042-reply-timing.md)'s appendix of 2026-11-18 says what
the model includes and when a pacing change ships on its figures.

#### Fitted to the car (2026-11-18)

The J2534 model's write through the surrogate takes 1.5 ms and its
delivery of a reply's last frame 1 ms; the MX+ model's Bluetooth link
takes 2 ms each way, a character 87 us and a command 2 ms. Replies a
second from the engine ECU, 60 s each:

| Run | Car | Model | Off by |
|---|---|---|---|
| D1, VX, obd2 packed | 58.7 | 56.6 | -3.5% |
| D2, VX, `pack=0` | 140.8 | 144.1 | +2.3% |
| D4, VX, `gap=0` | 52.7 | 56.6 | +7.5% |
| D5, VX, `gap=20` | 48.2 | 49.5 | +2.6% |
| D6, VX, `pack=0`, RPM | 140.3 | 144.0 | +2.7% |
| D7, VX, `pack=0`, eight signals | 138.4 | 144.1 | +4.1% |
| D9, VX, one DID, a Single Frame on 0x7E9 | 147 | 143.6 | -2.3% |
| D10, VX, one DID, segmented, on 0x7E8 | 80 | 81.1 | +1.4% |
| D3, MX+, obd2 | 40.9 | 40.8 | -0.2% |
| D8, MX+, `gap=0` | 41.9 | 40.8 | -2.5% |

`PacingTest` holds D1, D2, D6, D9 and D10 within 10% of the car and D3
within 5% (the car's own D3 and D8 differ by 2.4%). Every pinned run
asked one module, so each is also checked to be the same run with
`?hold=0`.

What the model doesn't cover:

- **The VX at `gap=0` (D4).** On the car the ECU's first frame came
  later at `gap=0` (P50 5.05 against 3.80 ms), so D4 ran slower than
  D1; the model's reply timing doesn't follow the pace, and it runs D4
  as D1.
- **How the MX+'s exchange splits.** The car's MX+ figures fix the
  exchange's total time, not its parts: a longer link with shorter
  characters fits as well.
- **One reply timing for every module.** Each module draws from the
  engine ECU's timing at idle; the car's second module (D9's) answered
  faster.
- **The worker's timing jitter.** The model's worker passes come at
  exact times. At a fixed floor two modules can lock into one phase: at
  `gap=27` (core 0.7.1's pace in
  [2026-11-11-monitor-car](bench-sessions/2026-11-11-monitor-car.md)'s
  R1 and R2, D11's shape) the second module loses 1.4% in the model
  against 24% and 19% on the car, where the loss is near the segmented
  window's share of the period (about 5.5 of 27 ms). At the default
  floor each module asks as its reply lands, no phase forms, and the
  model matches D11.

#### Two modules through the VX (Known Issue #42)

With `lost_under_segments` on (the bench's model of Known Issue #42: a
Single Frame reply that ends while another module's segmented reply is
on the bus, from its First Frame's end to its last Consecutive Frame's
end, is lost), D11's shape (a segmented DID on 0x7E8, a one-frame DID
on 0x7E9, `?hold=0` as the car ran it) gives:

| | Car | Model |
|---|---|---|
| 0x7E8 answered a second | 83.7 | 81.1 |
| 0x7E9 asked a second | 18.0 | 17.0 |
| 0x7E9's share answered | 0.551 | 0.453 |

`PacingTest` pins the two rates within 10% and the share within 0.15.
The share's gap comes from the one reply timing: the car's second
module answered faster than the engine ECU, so fewer of its replies
ended under a segmented one. The model loses more than the car did.

**The hold's prediction (no car run has measured it):** with `?hold=`
on, 0x7E9 loses next to nothing and 0x7E8 keeps about 0.64 of its rate
without the hold, as each segmented reply waits out the other module's.
Since the model loses more than the car, the prediction overstates what
the hold wins for 0x7E9; its cost to 0x7E8 doesn't depend on the loss. A
car run with the hold replaces both figures.

### Connect cost: a surrogate per session

Result: a create and close takes 18 to 28 ms against the x64 fake.

Every J2534 session spawns its surrogate
([ADR-0025](adr/0025-one-j2534-backend.md)). Against the x64 fake, a
create and close (`J2534ConnectTest.InfoCarriesTheUrisBaudAndChannel`,
which spawns, handshakes, opens, connects, reads the battery and the
versions, then closes) takes 18 to 28 ms, warm (30 repeats in one
process) or cold (five fresh processes), Release, 2026-10-26; in the
app's own process, before ADR-0025, it took 0 to 1 ms. A first run
after a build reads up to 89 ms, the exe's file not yet cached. A real
adapter's `PassThruOpen` adds its own time on top. Reads cost a pipe
round trip each, negligible at the transport's read cap
([ADR-0014](adr/0014-single-threaded-surrogate.md)).

### The sandbox's worst case

Result: a call at the default budget takes at most 3.9 ms on x64 and 11.1
ms on the x86 core; one `br_table` walk past the budget takes under a
millisecond.

2026-11-17, Windows 11 desktop, MSVC Release, the default budget of
1,000,000 units ([ADR-0066](adr/0066-conversation-tier-and-write-gates.md)'s
notes, items 18 and 23). The dearest module after the metering of work
beyond dispatch (a plain loop) takes 3.9 ms on x64 and 11.1 ms on the x86
core; without that metering one call ran 101 s. The longest single step is
one `br_table` walking every cached record a module can hold under the 8
MiB making bound: 0.52 ms at 180,000 records on x64, 0.63 ms at 220,000 on
the x86 core. That is how far past its budget a call can run. The phone's
figures are owed when a front end reaches the sandbox.
