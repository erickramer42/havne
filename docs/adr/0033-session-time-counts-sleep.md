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
  section 5 (the kernel's receive time, anchored to the session).

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

Since then, ADR-0029 section 2's wake lock keeps the CPU awake through an
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
| Linux, Android | `CLOCK_BOOTTIME` | `CLOCK_BOOTTIME` - `CLOCK_MONOTONIC` |
| Windows | `QueryPerformanceCounter` | `QueryInterruptTimePrecise` - `QueryUnbiasedInterruptTimePrecise` |
| macOS, iOS | `clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW)` | that - `CLOCK_UPTIME_RAW` |

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
(ADR-0032 section 5). The kernel stamps on `CLOCK_REALTIME`, which counts a
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
  asleep). Section 6's line reports the machine's sleep, not the app's freeze.

## Appendix: 2026-11-21, a Windows laptop's sleep inside a reply's wait

[2026-11-18-session-close-car](../bench-sessions/2026-11-18-session-close-car.md)'s
B10 is section 3 on a real machine: a laptop on its power plan's idle
timer slept with an ELM327 read half answered.

- **The wait had expired at the wake.** The read was typed at
  19:47:19.582 and its second reply line logged 29 ms later; the next
  line is the worker's "no line and no prompt for 5 s" at 20:06:19.515.
  Session time ran 1139.902 s between the two and `havne-cli`'s `slept`
  line gave 1139.290 s asleep, so the host was awake for 0.61 s of it:
  no stall came before the sleep, and none could have been timed out
  during it, since nothing runs on a suspended host.
- **Session time counted the sleep** (+2.8 s to +1142.7 s), section 1 on
  Windows, and the `slept` line said its length, section 6.
- **After it the worker closed the port and published a hard failure,**
  as for any reply's wait run out; its next step is the reopen and the
  setup on the backoff, so a session goes on after the wake. That run
  ended there because `--seconds` counts the same clock.
- **The wire log says the sleep.** A reply's wait that ends with the
  slept count risen by a second or more since its command was typed
  writes "the machine slept N s with <command> out: its reply is lost"
  in place of the 5 s line, and a read that fails on the wake (an
  adapter that re-enumerates, a link that drops) says "the port failed
  on the wake" the same way; the outcome is the same on every platform.
- **Nothing on a desktop keeps the machine awake through a session.**
  Android's service holds a wake lock (ADR-0029 section 2); a laptop's
  idle timer runs while nobody touches it, which is every logged drive.
  A request to stay awake while a session is up is a later release's.
  (Note of 2026-11-22: the next appendix holds the host awake while a
  session reads.)

## Appendix: 2026-11-22, the host kept awake while a session reads

A laptop's idle timer runs while nobody touches it, which is every
logged drive, and a sleep stops a session's reads until the wake (the
appendix of 2026-11-21). A desktop and `havne-cli` hold the host awake
through core; the phone's app holds its own.

```c
#define ECUIO_AWAKE_SYSTEM   0x00000001u
#define ECUIO_AWAKE_WHY_MAX  128
ecuio_status_t ecuio_session_keep_awake(ecuio_session_t* session,
                                        uint32_t what, const char* why);
ecuio_status_t ecuio_session_get_awake(const ecuio_session_t* session,
                                       uint32_t* out_what);
```

`core.h` holds the contract. In short: held from the call until the
close or a call with 0, which never waits; only a live transport holds;
a refusal is `ECUIO_ERR_NOT_IMPLEMENTED` with the detail, and the
session runs on. The getter reads an atomic, since the session info is
written once at the attach and read with no lock. A take runs outside
the session's lock and publishes after it, so a 0 or the close never
waits on a take that waits on Linux's buses: one that comes during the
take wins, and the take's hold is released at once.

### 1. On each platform

| Platform | Held by | Released by | Listed by |
|---|---|---|---|
| Windows | `PowerCreateRequest` with the reason, then `PowerSetRequest(PowerRequestSystemRequired)` | `PowerClearRequest`, `CloseHandle` | `powercfg /requests` (an administrator's prompt) |
| Linux | logind's `Inhibit("idle", <program>, <why>, "block")` on the system bus, and on the session bus the desktop's `org.freedesktop.PowerManagement.Inhibit` and `org.gnome.SessionManager.Inhibit` (flag 4) where they answer, through libsystemd's sd-bus loaded at run time; a write step takes logind's `"sleep:idle"` | closing logind's descriptor; closing the session bus, which ends the desktop's inhibits | `systemd-inhibit --list`; the desktop's power manager |
| macOS | `IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleSystemSleep)` | `IOPMAssertionRelease` | `pmset -g assertions` |
| Android | the app: `SessionService`'s partial wake lock while the switch is on or a relay runs ([ADR-0029](0029-android-background-sessions.md) section 2) | | `dumpsys power` |
| iOS | nothing | | |

- **A sleep the user asks for still sleeps, except during a write
  step.** A read holds off the idle timer alone on every platform. A
  write step's hold on Linux is logind's sleep lock, which holds off
  the suspend key and makes `systemctl suspend` ask for `-i`: without
  it the write's take is refused, in words, even where the desktop's
  inhibit holds, since that holds off only the desktop's idle timer.
  Windows and macOS let no program hold off a sleep the user asks for,
  so a write's hold there is the read's power request or assertion,
  and a sleep the user asks for still sleeps a write step.
- **Windows takes a power request, not `SetThreadExecutionState`:**
  a request is a handle any thread can take and clear, while an
  execution state belongs to the thread that set it, and a request
  carries a reason the user can read.
- **Linux asks logind and the desktop both,** since a desktop's power
  manager may suspend on its own idle timer without asking logind
  (Xfce's does). libsystemd is loaded when first asked, so core has no
  build dependency; a system with neither refuses with a reason. All
  three calls share one second, the buses' authentication included
  (`sd_bus_call` alone would wait for it by a far longer bound of its
  own). The lock is a descriptor and the inhibits a bus connection, so
  both end with the process; closing the connection ends the desktop's
  inhibits, so their cookies are never passed back. GNOME's flag 4 is
  suspend, which its power daemon's idle suspend honors.
- **A lid is out of reach.** logind's `LidSwitchIgnoreInhibited=`
  defaults to `yes`, Apple's power management lets no application
  prevent a lid's sleep, and Windows' lid action is the power plan's.
  A run that closes the lid sets that by hand.

### 2. When it holds

- **A live session, while it is open.** Through link errors and the
  reopen's backoff, since the worker reopens after a wake or an
  adapter's return; with the app minimized or behind another window.
  The close releases after the transport's detach, so the hold covers
  the close's `10 01`.
- **Not `mock://` or `replay://`.** They are desk tools, as on the
  phone, and section 7's sleep check runs under `mock://`.
- **The system, not the display.** The screen sleeps on its timer and
  the session reads behind it.
- **A write holds whatever the setting.** Core keeps a claim of its
  own for a write-class step, at the write's level, and a caller's 0
  releases only the caller's.
- **At a process's end the platform drops it,** a crash included.

### 3. The front ends

- **The app:** Settings > Connection's "Keep the computer awake while
  connected" ("Keep the phone awake while connected" on a phone), on
  by default and kept between runs. On a desktop the connect thread
  holds after the create, so a take never waits on the GUI thread.
  Switched off during a session, it releases at once; switched on, it
  holds from the next connect. A refusal logs one line, "Couldn't keep
  the computer awake: " and the detail. On Android the switch gates
  the service's wake lock, and a relay holds it whatever the switch
  says, since it serves another device's session.
- **`havne-cli`:** `stream` and `log` hold for a live address unless
  `--allow-sleep`. The `session` line adds `awake` (`true` or `false`)
  for a live session only, which schema 1 grows by (ADR-0031 section
  3), and a refusal writes one standard-error line. `slept` lines keep
  section 6's rule.
- **Python:** `Session.keep_awake()` and `Session.awake`, off unless
  asked.

### 4. Tests

- **UNIT:** a seam stands in for the platform's take and release,
  counting them: `CoreTest.AMockOrReplaySessionHoldsNothing`,
  `CoreTest.KeepAwakeRefusesAnUnknownBitAndALongWhy`,
  `.OnlyALiveBackendHoldsAwake`, `.AWriteHoldNeedsLogindsSleepLock`
  (Linux's rule, built everywhere); on the fake ELM327,
  `Elm327AwakeTest.ALiveSessionHoldsUntilItsClose`,
  `.ASecondCallTakesNothingMore`, `.KeepAwakeWithZeroReleasesAtOnce`,
  `.ARefusedHoldSaysWhyAndTheSessionRuns`, `.TwoSessionsHoldApart`,
  `.ZeroLeavesCoresOwnHold`, `.TheCloseReleasesAfterTheDetach`, and
  with a take held in the seam `.AZeroDuringATakeReturnsAtOnceAndWins`,
  `.ACloseDuringATakeReturnsAtOnceAndHoldsNothing`,
  `.CoresFalseDuringItsTakeReturnsAtOnceAndWins`;
  `CliUsageTest.AllowSleepTakesNoValue`,
  `.AllowSleepIsStreamAndLogOnly`.
- **HARNESS:** the platform's own calls:
  `Elm327AwakeTest.LogindListsTheHoldUntilTheClose` (skipped without
  logind, or on polkit's refusal of the lock),
  `.AWedgedBusRefusesInsideTheSecond` (Linux: a bus that never answers),
  `.MacOsListsTheAssertion`, `.WindowsTakesARealRequest`;
  `BridgeTest.AMockConnectHoldsNothing`,
  `.AConnectHoldsTheHostOnlyWhenTicked`, `.UntickingReleasesAtOnce`,
  `.UntickedWhileConnectingReleasesAtTheApply`,
  `.ARefusedHoldLogsOneLine`; `CliLogRunTest.ALiveSessionLineSaysAwake`;
  the switch's words and its return in `QmlTest.settings_devices`;
  `PythonTest.test_session`; `test_c_abi`.
- **BENCH:** no runner's idle timer runs. BENCH prep #39: a laptop with
  its power plan's sleep at 1 min, the app and `havne-cli log` on
  `elm327://` to the ELM327 emulator, 5 min untouched, no `slept`
  line; switched off, or with `--allow-sleep`, the laptop sleeps and
  the `slept` line says how long. The same on a Linux desktop's own
  idle suspend, on a Modern Standby laptop, and on the phone with its
  screen off.

### Consequences

- **Good:** a logged drive is read end to end without a power-plan
  edit, on every desktop, from the app, the CLI and Python alike; the
  platform's own list says what holds the machine and why.
- **Bad:**
  - A laptop left connected runs its battery down; a critical battery
    still sleeps it.
  - A desktop whose power manager neither asks logind nor takes an
    inhibit on the session bus isn't held; the bench run says which
    desktops do.
  - Whether a system-required request holds a Modern Standby laptop
    once its display is off rests on that run.
- Two functions and two macros join the C ABI (append-only), and
  `test_c_abi` covers them.

### Alternatives considered

- **Each front end owns it:** the platform code written for the app
  and again for the CLI, and Python's sessions sleep.
- **A session info flag:** the info is written once at the attach and
  read with no lock.
- **`SetThreadExecutionState`:** the state is the calling thread's.
- **A `systemd-inhibit` child process:** a process to start and reap
  from inside a library, and the desktop's inhibit still missing.
- **Linking libsystemd at build time:** a dependency for the core-only
  tree and the AppImage, for a few calls.
- **logind's sleep lock for a read:** a user's suspend key would do
  nothing while connected.
- **The display too, or only while recording:** more battery and a
  lit screen in a car; a drive watched without Record sleeps.
