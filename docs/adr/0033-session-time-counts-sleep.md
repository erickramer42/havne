# ADR-0033: Session time counts sleep, one host clock for core and the app, and a sleep that says so

- **Status:** Accepted.
- **Date:** 2026-10-29. Implemented with this record.
- **Builds on:** [ADR-0005](0005-coalesced-signal-updates.md) (a
  value's age); [ADR-0006](0006-frame-abi-v0.3.md) (a frame's
  `timestamp_us` is session time); [ADR-0017](0017-session-recording.md)
  (a recording's `host_us`); [ADR-0031](0031-command-line-client.md)
  (the CLI's lines); [ADR-0029](0029-android-background-sessions.md)
  and its appendix "the phone run, and a wake lock" (the CPU's sleep
  joined the values on either side); [ADR-0032](0032-socketcan-listening.md)
  §5 (the kernel's receive time, anchored to the session).

## Context

Session time is microseconds since the session's transport attached
(ADR-0006). Every live transport takes it from
`std::chrono::steady_clock` less an epoch, and the app keeps two more
clocks of its own: the recorder stamps `host_us` from `steady_clock`
(ADR-0017), and the bridge measures a value's age with a
`QElapsedTimer`.

What `steady_clock` does while the machine sleeps depends on the
platform:

| Platform | `steady_clock` is | While asleep |
|---|---|---|
| Windows (MSVC) | `QueryPerformanceCounter` | Counts: "including the time when the machine was in a sleep state such as standby, hibernate, or connected standby" (Microsoft, *Acquiring high-resolution time stamps*) |
| macOS, iOS (libc++) | `CLOCK_MONOTONIC_RAW` | Counts (libc++ chose it because it keeps counting) |
| Linux (libstdc++), Android (libc++) | `CLOCK_MONOTONIC` | Stops |

`QElapsedTimer` is `CLOCK_MONOTONIC` on Linux and Android too.

On Linux and Android, a sleep therefore vanishes from session time. The
phone run behind ADR-0029's appendix
([2026-10-28-android-9](../bench-sessions/2026-10-28-android-9.md))
saw it. The CPU slept about 100 s of a 118 s screen-off, and Live and
the recording joined the values on either side with no gap. Only the
activity log's wall-clock column, read against its session seconds,
showed the hole. Data that weren't read were drawn and recorded as if
they had been.

Since then, ADR-0029 §2's wake lock keeps the CPU awake through an
adapter session. A sleep still happens:
- under `mock://` and `replay://` on a phone, which hold no wake lock
  by design
- on a Linux laptop whose lid closes mid-session (SocketCAN, a USB
  ELM327)
- wherever a wake lock doesn't hold

Live's chart already breaks a trace where samples are more than
`gapMs` apart (`ChartLayout.segments`), so a gap in session time is
drawn as one. The clock is what's missing.

## Decision

### 1. One clock, counting sleep on every platform

Core gets one host clock, public in `core.h`, so the app and the CLI
read the same one:

```c
/* Microseconds on the clock session time runs on. Counts the time the
 * machine sleeps on every platform. The origin is the platform's, so
 * only differences mean anything. */
ECUIO_API uint64_t ecuio_clock_us(void);

/* Microseconds the machine has slept since it started, on the same
 * clock: a rise between two reads is a sleep between them. */
ECUIO_API uint64_t ecuio_clock_slept_us(void);
```

| Platform | `ecuio_clock_us` | `ecuio_clock_slept_us` |
|---|---|---|
| Linux, Android | `CLOCK_BOOTTIME` | `CLOCK_BOOTTIME` − `CLOCK_MONOTONIC` |
| Windows | `QueryPerformanceCounter` | `QueryInterruptTimePrecise` − `QueryUnbiasedInterruptTimePrecise` |
| macOS, iOS | `clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW)` | that − `CLOCK_UPTIME_RAW` |

Each platform's clock is named outright rather than left to the
standard library's choice, so the property rests on documented OS
behavior. `core/src/host_clock.h` wraps it as a C++ chrono clock
(`host_clock::now()`), so the transports' epochs keep their chrono
types and the change is a type at each.

### 2. What runs on it

- **Every live transport's session time:**
  - the mock's frames
  - ELM327's frames and its wire log's elapsed column
  - the J2534 and SocketCAN host times that their clocks anchor to
    (`j2534_clock_step`, `socketcan_clock_step`)
- **The request engine's schedule** (its `now_us`). A reply's stamp and
  its deadline then come from one clock.
- **In the app:**
  - the recorder's `host_us` (`frames.csv`, `signals.csv`, `events.log`):
    `SessionRecorder::Clock` is `ecuio_clock_us` as a chrono clock
  - the bridge's arrival stamps, which are those `host_us`; the delivery
    figures (worker to GUI thread) time the same stamps, so a sleep
    between a frame's arrival and the GUI thread's turn shows as one long
    delivery, as on Windows already
  - the model's value ages, which dim a stale gauge, and its fallback
    stamp (`HostElapsedTimer`)
- **In the CLI:** `--seconds`.

After a sleep:
- **Live** draws a gap.
- **`signals.csv`** skips the slept span.
- **Every value is as old as it is.** It dims at the wake.
- **On Windows, macOS and iOS nothing changes.** Their clock already
  counted sleep.

### 3. What stays on `steady_clock`

- **Waits:** condition variables, `sleep_until`, and the I/O timeouts.
  The standard waits take `steady_clock` alone, and a wait is a
  duration: how long the worker waited for an answer.
- **A replay's pacing.** A recording's stamps are its own, so pacing
  only decides when frames go out.
- **The mock's tick schedule**, which paces its frames; their stamps
  move.

A wait that spans a sleep behaves as each platform's `steady_clock`
does. On Windows and Apple it has expired at the wake; on Linux and
Android it resumes. This ADR changes neither.

### 4. The engine and a wake

On Linux and Android, a request outstanding when the machine sleeps
has passed its deadline at the wake. The engine counts it unanswered,
then asks again, as Windows already does. Link health sees one quiet
window and recovers on the next reply. That's what happened on the
wire: nothing answered while the machine slept.

### 5. The SocketCAN and J2534 clocks across a sleep

`socketcan_clock_step` lets the host's delta stand in when the kernel's
delta would put a frame more than a second past the moment it's read
(ADR-0032 §5). The kernel stamps on `CLOCK_REALTIME`, which counts a
sleep. With a host clock that stopped, the first frame after a wake
fell back to the host's delta and lost the gap. With this clock, the
two deltas agree, and the kernel's time stands. `j2534_clock_step` is
Windows only, where the host clock already counted sleep.

### 6. A sleep says so

While a session runs, the bridge reads `ecuio_clock_slept_us()` on its
100 ms timer. A rise of a second or more logs one line to the activity
log, and through it to `events.log`:

> Asleep for 98.4 s: nothing was read meanwhile

The CLI writes a `slept` line (`us`), a type schema 1 grows by
(ADR-0031's appendix of 2026-10-29); its rule is `cli::SleepWatch`
(`cli/sleep_watch.h`). In both, smaller rises are taken in without a
line, so they never add up to one.

### 7. Tests

- **UNIT:**
  - `CoreTest.ClockNeverGoesBackAndKeepsPace`: neither function
    decreases, and the clock keeps `steady_clock`'s pace across a
    `sleep_for`.
  - `CoreTest.ClockIsThePlatformsSleepCountingClock`: the clock reads
    between two reads of the platform's (`CLOCK_BOOTTIME`,
    `CLOCK_MONOTONIC_RAW`, MSVC's `steady_clock`). That pins the choice
    only where that clock and the one that stops in sleep differ, so on
    a CI runner that hasn't slept since boot a `CLOCK_MONOTONIC` mutant
    survives.
  - `SocketCanTest.ASleepKeepsItsGap`: a kernel delta that matches a
    host delta spanning a sleep stands.
  - `CliSleepTest.ARiseOfASecondInOnePollIsASleep`.
- **HARNESS:**
  - `CoreTest.SessionTimeRunsOnTheClock`: a mock session's stamps are
    `ecuio_clock_us` since its attach.
  - `BridgeTest.ASleepWhileConnectedIsLoggedWithItsLength`: the
    "Asleep" line, once, in `events.log` too, never for a rise under a
    second, one before the connect or one after the disconnect, through
    `addSleptForTesting`, which adds to the count the tick reads.
- **BENCH:** a real sleep is the only real check, and no CI runner can
  sleep. Two runs:
  - the phone: android-9's screen-off repeated under `mock://`, with no
    wake lock
  - a Linux machine suspended mid-session, if one is at hand

  Expected: a gap on Live and in `signals.csv`'s `host_us`, and the
  "Asleep" line. This is BENCH prep #27.

## Alternatives considered

- **Leave `CLOCK_MONOTONIC`.** A sleep keeps joining the values on
  either side, on two platforms out of four.
- **Stamps on the new clock, the engine on the old.** Inside one
  transport, a reply's stamp and its deadline would come from two
  clocks that differ by every sleep so far. Rejected.
- **The wall clock.** NTP and the user step it, so frames could run
  backward. Session time must never decrease.
- **A frame flag for the first frame after a sleep**, like `OVERFLOW`.
  It would spend an ABI bit, and the gap already says it on the chart;
  the log line says how long. Not now.

## Consequences

- **Good:**
  - Session time means the same on every platform: a sleep is a gap,
    on Live, in a recording, and in the CLI's stream.
  - The activity log says when the machine slept and for how long,
    which android-9 had to work out by comparing two columns.
- **Bad:**
  - On Linux and Android, a wake now costs the engine one unanswered
    request and link health one quiet window.
  - After a long sleep, Live's window has scrolled past the old values,
    and the chart is empty until new ones come.
  - A replay that spans a sleep on Windows or Apple still catches up in
    a burst at the wake, as today (its stamps are unaffected).
  - CI can't sleep, so the property itself is verified on hardware
    only.
- Two functions join the C ABI (append-only), and `test_c_abi` covers
  them.

## Appendix: 2026-10-29, the phone's sleeps

[2026-10-29-android-2](../bench-sessions/2026-10-29-android-2.md), the
build of `27592cf` on the test phone under `mock://`:

- **On USB** (charging, `adb` attached) the CPU never slept; the gaps
  on Live were the app frozen, and no "Asleep" line came, rightly.
- **Off USB** the CPU slept thirteen times in 674 s, 583 s in all. Each
  sleep was a gap of its length on Live, in `frames.csv` and in
  `signals.csv`'s `host_us`, each named by an "Asleep for N s" line,
  and session time kept the wall clock's pace (674.47 s against
  674.46 s; about 91 s on a clock that stops in sleep).
- **A freeze with a sleep inside it** is one gap, longer than its
  "Asleep" line, which counts the sleep alone (357.5 s with 342.1 s
  asleep). §6's line reports the machine's sleep, not the app's freeze.
