# ADR-0042: Reply timing per request: latency on the host's clock, the refresh interval on the frames' own, published while the session runs

- **Status:** Accepted. Built on 2026-10-31 and verified through each
  transport against the fakes (J2534 through the surrogate, ELM327,
  SocketCAN on `vcan0`), and on the test car through the VX and the
  MX+ ([2026-11-01-vehicle](../bench-sessions/2026-11-01-vehicle.md),
  [2026-11-01-android](../bench-sessions/2026-11-01-android.md)), and
  beside a listener on the bus (the appendix of 2026-11-03).
- **Date:** 2026-10-31.
- **Builds on:** [ADR-0015](0015-request-engine.md) (the request engine:
  one request outstanding per channel, its wait);
  [ADR-0033](0033-session-time-counts-sleep.md) (the host clock);
  [ADR-0039](0039-full-service01-coverage.md) (rate classes, packing);
  [ADR-0040](0040-socketcan-requests-isotp.md) section 5 (ISO-TP's
  counts reach users with this report);
  [ADR-0017](0017-session-recording.md) (a recording's folder).

## Context

Havne measures how fast it moves frames (`bench_bridge`, CI's load
test, the activity log's "Delivery:" line), but not what a user of a
requesting session sees: how long the ECU takes to answer each request,
and how often each value refreshes. The test car's packing run gave two
numbers by hand (fast signals every 0.107 s, slow every 0.665 s), read
off `signals.csv`. A benchmark per request, measured by core and kept
with the recording, makes those numbers every session's, on every
transport, and lets a car run compare adapters, packing and rate
classes from its own files.

Two times matter, and they come from different clocks:

- **Reply latency:** the request's send to its reply. Only the worker
  sees both ends, so it is the host's clock (ADR-0033), and it includes
  everything between the worker and the ECU: the adapter, its link
  (USB, Bluetooth, the serial line), the bus.
- **Refresh interval:** one request's answer to its next. The frames
  carry stamps of their own: a J2534 adapter's clock (`j2534_clock.h`),
  the kernel's receive time on SocketCAN (`socketcan_clock_step`), the
  host's when an ELM327's line is read. An adapter's or the kernel's
  stamp leaves the host's scheduling out of the interval.

Neither stamp is pure for a whole session. The J2534 clock takes the
host's delta when the vendor's clock steps; SocketCAN's kernel time is
the wall clock (`SO_TIMESTAMPNS`, `CLOCK_REALTIME`), so its transport
takes the host's delta when it steps backward or runs off. An interval
across such a frame is part host time.

## Decision

### 1. What is measured, and where

Core's request engine keeps the figures, for each request in the
rotation (a PID, or a DID under `uds-generic`; one-shots, the walk and
the scan are not rows):

- **Outcomes:** replies answered (positive), refused (negative), and
  timeouts (the wait ended with nothing). A PID asked in a pack shares
  the pack's outcome: each PID whose record the reply carries is
  answered, a refusal refuses them all, a timeout times them all out,
  and a PID the reply leaves out gets nothing that pass. `packed`
  counts the answers and refusals that came to a pack.
- **Latency:** from the request's write to the reply that completes it,
  on the host's clock, for every answer and refusal; a pending reply
  (`7F xx 78`) extends it to the reply that follows. A pack's latency
  is each of its PIDs' that came back in its reply.
- **Refresh interval:** from one answer to the next of the same
  request, on the frames' own stamps. The transport hands the engine
  each reply's stamp and the count of the host fallbacks its clock has
  taken so far; an interval with a fallback between its two answers is
  left out and counted (`interval_mixed`). The session says whose clock
  the intervals are on: `adapter` (J2534), `kernel` (SocketCAN, the
  kernel's wall-clock deltas, so a small clock slew passes through),
  `host` (ELM327).

Each figure is a histogram in fixed memory, log-linear as the bridge's
delivery figures (`app/latencystats.h`): eight buckets to a power of
two, exact below 16 microseconds, so a quantile reads at most 12.5 % high and
never low; up to about 16.8 s, longer ones in the last bucket with
the exact maximum kept. Recording a sample is a few instructions on
the worker, with no lock and no allocation. The engine's own tests
pass no timing and pay nothing.

### 2. The report: rows published every second

- `ecuio_session_get_timing(session, &timing, rows, capacity, &count)`
  (`profile.h`) copies the session's report and up to `capacity` rows
  (`count` all of them, as `ecuio_serial_ports` counts). Readable at
  any time and after close.
- **A row** (`ecuio_request_timing_t`, 80 bytes): the channel and the
  request's bytes; answered, refused, timeouts, packed; latency P50,
  P99, P99.9 and the maximum; the interval's count, mixed count, P50,
  P99, P99.9 and maximum; flags.
- **Too few samples:** a P99 needs 100 samples and a P99.9 1,000; below
  that its flag is clear and the writers leave it empty. A slow PID at
  0.665 s takes eleven minutes to a P99.9.
- **The session's part** (`ecuio_session_timing_t`): the session time
  of the snapshot, the interval clock, and ISO-TP's counts where core
  reassembles (a requesting SocketCAN or ELM327 session: messages begun
  and finished, dropped, frames no message could use; ADR-0040 section
  5). A session with no rotation (mock, replay, a listening session)
  reports no rows and no counts.
- The worker publishes the rows at most once a second and once as it
  stops, so the figures computed into the rows cost the worker one pass
  over the table a second.

### 3. `latency.csv`, the recording's, and the CLI's `timing` lines

Both writers write the same snapshot, row for row, in the same columns
under the same names (`ECUIO_TIMING_COLUMNS`, `profile.h`, the one
list): the recorder's header is that list, and each of the CLI's
`timing` lines has those keys in that order.

- **A snapshot** every 10 s of session time and one at the end. It
  holds the rows whose counts changed since the last snapshot, each
  with `t_s`, the snapshot's session time. The figures are cumulative
  from the connect. **The last row for each request is its total**; a
  PID that was dropped or went quiet stops repeating, and its last row
  stands. Read in order, the snapshots show a session's warm-up apart
  from its steady state.
- **The recorder** appends each snapshot to `latency.csv` in the
  recording's folder (ADR-0017), from the bridge's tick on the GUI
  thread with the recording's other files. The figures are the
  worker's, cumulative, so a stall of the GUI thread delays a snapshot
  but loses nothing from it; an app killed in the background (Android)
  keeps everything up to its last snapshot.
- **`havne-cli stream`** writes each snapshot as `timing` lines (schema
  1 grows by a line type, ADR-0031), and its `end` line carries the
  ISO-TP counts when the session has them.
- **The activity log** gets one line at the disconnect: the rows, the
  replies, the timeouts, the interval's clock, and ISO-TP's counts
  where they apply.

### 4. The benchmark

What v0.3.5 publishes in [docs/testing.md](../testing.md#performance),
all with no hardware:

- The delivery figures as today (`bench_bridge --load`: frames a
  second, throughput, frames dropped), measured again.
- Reply timing against the fakes' ECU through each transport
  (`bench_timing`: the fake J2534 DLL in VX mode, the fake ELM327,
  SocketCAN on `vcan0`): what the stack itself adds, per transport, at
  P50, P99 and P99.9, with the fake's own reply delay stated beside it.

A car's figures come from its runs' `latency.csv` (v0.3.6's runs).

## Alternatives considered

- **Every request raw** (a row per request sent: its send, its reply or
  timeout). Exact percentiles offline, nothing lost to buckets, but it
  needs an event path per request from core to every front end, and a
  30-minute session is about 70,000 rows. The histograms answer the
  percentiles to 12.5 %, which is finer than a car's spread.
- **Latency on the adapter's clock** (J2534's TX indication stamp to
  the reply's, SocketCAN's echo to the reply). It would leave the host
  and the adapter's link out, but nothing in v0.3.5 can check what it
  means: what a vendor's TX indication marks (queued, started, ACKed)
  is the vendor's, and on a gs_usb controller both stamps are when a
  USB message reached the host, carrying USB's millisecond polling; on
  `vcan0` there is no bus at all. It is a v0.3.6 candidate, named for
  what it is (adapter or kernel latency, not the bus's), checked against
  an independent listener on the desk bus before it's published.
- **The refresh interval on the host's clock everywhere.** One place in
  core and no transport changes, but the host's scheduling would sit in
  every interval, which an adapter's or the kernel's stamp leaves out.
- **Rewriting `latency.csv` whole at each snapshot.** A crash between
  the truncation and the write loses the file; appending loses at most
  the last snapshot, and keeps the session's run over time.
- **A recorder thread for the append.** It would keep the file I/O off
  the GUI thread, but the recording's other files are written there
  too, and moving recording to a thread of its own is a decision for
  all three files, not one.

## Consequences

- **Good:**
  - Every requesting session measures its own reply timing, kept with
    its recording, so a car run compares adapters, packing and rate
    classes from its files.
  - The two writers can't drift apart: one column list, one rule for
    reading the snapshots.
  - ISO-TP's counts reach users without a getter of their own.
- **Bad:**
  - A getter, two structs and a column list in the C ABI.
  - About 200 KB of histograms per requesting session.
  - Latency includes the host and the adapter's link, so it is the
    stack's view, not the ECU's response time alone.

## Appendix: Windows' timer step (2026-11-01)

The first benchmark read a 4 ms reply as 16.4 ms on Windows through
both fakes, against 4.1 ms on Linux. Raising the timer to 1 ms in one
process at a time (`timeBeginPeriod`, which since Windows 10 2004
affects only the processes that call it) separated the causes:

- **J2534:** the VX holds a read for its timeout unless the batch
  fills, and the worker asks for one 10 ms slice; inside the
  surrogate, that wait rounded up to the default 15.6 ms step. A real
  vendor DLL waits in the same process, so this is the stack's, not
  the fake's. With 1 ms in the surrogate, 11.3 ms, and RPM every
  164 ms instead of 262.
- **ELM327:** the fake waits out its 4 ms in the test's own process,
  and that wait rounded up. With the fake spinning to its reply time
  instead, at the default step, ELM327 read 4.0 to 4.1 ms and RPM
  refreshed every 148 ms, Linux's figures: the worker adds nothing,
  and a real serial port's read returns when its bytes arrive.

**Decision:** the surrogate holds a 1 ms timer for its whole life
(`surrogate_main.cpp`; one surrogate per session, so only while a
J2534 session runs), and `bench_timing` holds one so the fakes'
delays are their length. Windows 11 stops honoring the request only
for a window-owning process that can't be seen; the surrogate owns no
window. The cost is the one Microsoft names: more timer interrupts and
less CPU power saving, while a session runs. The app's own process
keeps the default: its waits are deadlines (the mock), event-driven
(the ports, the surrogate's pipe) or backoffs.

**Left:** asking the VX for as many messages as replies are
outstanding, rather than 16, would end a read when its reply lands
instead of at the slice; that changes how the adapter is read, so it
waits for the VX on the bench. What a real VX's replies read comes
from the car run's `latency.csv`.

*Amended 2026-11-01 (the car):* on the test car through the VX
([2026-11-01-vehicle](../bench-sessions/2026-11-01-vehicle.md)),
`?pack=0` for 45 s four times, alternating a surrogate built without
the `timeBeginPeriod` call and the build's own: 1,185 and 1,251
replies without the 1 ms timer, 1,519 and 1,601 with it (28 % more);
the median request's P50 18.4 ms against 15.4, the slowest's 30.7 to
32.8 ms against 20.5, RPM every 721 ms against 590. The bench's
11.3 ms doesn't carry: the car's ECU answers slower than the fake's
4 ms, and its time is in each figure.

*Amended 2026-11-06 (the requesting workers' waits):* the app's own
process still keeps the default timer, but a requesting worker's waits
to the engine's next event no longer round up to its step. The J2534
worker's wait after an empty read, and the ELM327 worker's wait while
no command is out, run on a waitable timer of its own made with
`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` (`core/src/precise_wait.h`,
Windows 10 1803 and later; a plain sleep where it's refused), not
`timeBeginPeriod`: nothing changes for the rest of the process, and
Windows 11 honors no 1 ms request from a window-owning process that
can't be seen, the app minimized. Against the fake DLL answering at
once, through `havne-cli`, each module was asked every 31.7 ms, one
module or two, and is asked every 21 ms with it (47 to 48 a second
against the floor's 50); the fake ELM327's two modules, 195 requests
in 2 s for both. The fake as the VX keeps 26.7 ms with or without it,
the car's figure: a reply lands about 4 ms after its write, and the
next read, asked for under 10 ms as the floor nears, is held 10 ms by
the VX inside the surrogate, where the 1 ms timer already is. That
pace is the VX's read hold, the bench item under "Left" above.

## Appendix: the bus beside the host's clock (2026-11-03)

On the test car, a listen-only SocketCAN controller beside the VX timed
the VX's requests on the wire, on the kernel's receive stamps
([2026-11-03-socketcan-car](../bench-sessions/2026-11-03-socketcan-car.md)):
the request to its reply's first frame, a median of 3.46 ms, and to its
last, 13.9 ms, nearly every request a six-PID pack whose reply is
segmented. The VX's Flow Control followed each First Frame by 0.17 ms
(block size 0, STmin 0), and the ECU paced its Consecutive Frames about
5 ms apart. The VX's session put each packed PID's latency, on the
host's clock, at 65.5 to 73.7 ms (2026-11-01's run: 73.7 packed, 14.3
for a single PID). A segmented reply therefore reaches the worker about
52 ms after its last frame crossed the bus, through the adapter, its
USB link and its driver; the worker's read slice is 10 ms.

The host's figure stays what section 1 says it is: what a session gets.
The listener's is the bus's, and the gap is the adapter's: a latency on
the adapter's clock (the alternative above) would still leave out what
it measures here, the delivery after the last frame. Neither changes
what's built.

**Note, 2026-11-04: the 52 ms was the surrogate's.** The VX's frame log
on the test car ([2026-11-04-two-ecu-car](../bench-sessions/2026-11-04-two-ecu-car.md))
puts the cause in the surrogate, not the adapter. A reply that came
after the first read's 10 ms reached the worker about 50 ms after it
arrived, with no empty read between. The child, finding only the TX
indication in its first call, called `ReadMsgs` again with what was
left of the timeout, which was 0, and the VX holds a read asked to wait
0 ms for about 50 ms (50 to 65 ms in that log, median 53). A reply
inside the first call came back when that call's slice ended. The
child's calls now wait at least 1 ms, and the first asks for one
message, so a read ends at its reply and takes the rest of a batch in
one more 1 ms call ([j2534.md](../j2534.md#requests)). The fake as the
VX holds a 0 ms read for 50 ms, and `J2534HealthTest.VxAReply*` time
both cases. The adapter's own delivery after the last frame is what
remains of the gap; the car measures it again.

**Note, 2026-11-05: no call for the rest of a batch.** The re-run on the
test car ([2026-11-05-two-ecu-car-rerun](../bench-sessions/2026-11-05-two-ecu-car-rerun.md))
had no reply 50 ms late, but the VX held the 1 ms call for the rest of
a batch a median 9.8 ms, and only 12 of 3,337 such calls brought a
message: packed requests read a median 24.6 ms against the bus's
15. The child now answers a read with its first message, and the
fake as the VX holds a call asked to wait under 10 ms for 10 ms. Timed
on the car the same day (that sheet's A2): packed 16.4 ms, unpacked
4.6, each call held a median under 1 ms.

## Appendix: `t_s` in a recording (2026-11-07)

Section 3's `t_s` is the snapshot's session time in the CLI's `timing`
lines, as written. In a recording's `latency.csv` it is the recording's
time of the call that published the snapshot, to the ms, since a
recorder can attach to a started session ([ADR-0056](0056-analysis-log.md)
section 3): the session's time would put a mid-session log's reply
timing ahead of its frames. The snapshot's cadence and the columns are
unchanged; [ADR-0017](0017-session-recording.md)'s appendix of
2026-11-07 has what an older recording holds.
