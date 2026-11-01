# ADR-0041: Fault injection on a replay: a recording played with frames dropped, lost to an overflow, corrupted, cut short or held back, the same each time

- **Status:** Proposed.
- **Date:** 2026-10-31.
- **Builds on:** [ADR-0023](0023-replay-transport.md) (`replay://` plays a
  recording byte for byte, at its own pace or faster);
  [ADR-0017](0017-session-recording.md) (`frames.csv`);
  [ADR-0024](0024-decode-on-the-worker.md) (decoding on the worker);
  [ADR-0039](0039-full-service01-coverage.md) section 6 (a field the
  replies come without is absent).

## Context

Havne's error handling is tested against the fakes, which inject faults
by knob (a J2534 read that overflows, an ELM327 that answers `BUFFER
FULL`, an ECU that stays silent), and against unit tests of each rule.
Neither is real traffic: the fakes answer from canned bytes, at the
fakes' pace. The recordings of the test car are real traffic, and
`replay://` plays them byte for byte with their own timing, but a
replay is the one session that never goes wrong.

Real sessions do go wrong, in a few ways that reach core:

- **A frame lost with nothing said:** a frame a busy adapter drops, or
  one that never made it into a reply.
- **A frame lost to an overflow, said:** an adapter's buffer filled
  (ELM327 `BUFFER FULL`, J2534 `ERR_BUFFER_OVERFLOW`), and the next
  frame carries `ECUIO_FRAME_FLAG_OVERFLOW`.
- **Bytes wrong:** a bit flipped somewhere between the ECU and core.
  CAN's CRC makes this rare on the bus, but a serial or Bluetooth link,
  a buggy adapter or a misparsed line can do it.
- **A reply cut short:** fewer bytes than a def reads, which the decoder
  refuses past the payload and ADR-0039 section 6 counts toward absent.
- **A stall:** nothing for a while, then the stream resumes: the link
  dropping and coming back, the ignition off and on, a Bluetooth gap.

What has to hold under them is spread across the stack: the decoder
reads nothing past a payload, the absent-signal rule doesn't call a
signal absent for one bad frame, the bridge counts overflows and dims
stale values, the recorder writes what was delivered, the CLI writes
valid JSON lines, and nothing crashes or leaks under the sanitizers.

## Decision

### 1. Faults are a replay's query

```
replay://<path>?faults=<fault>[,<fault>...][&seed=<n>]
```

| Fault | What it does to a frame, with that chance |
|---|---|
| `stall:<p>:<ms>` | before it, the stream stops for `<ms>`, 1 to 10000: it's paced that much later, and it and every later frame are stamped that much later |
| `drop:<p>` | the frame isn't delivered, and nothing says so |
| `overflow:<p>` | the frame isn't delivered, and the next one delivered carries `ECUIO_FRAME_FLAG_OVERFLOW` |
| `corrupt:<p>` | one bit of its data flips (a frame with no data is left alone) |
| `truncate:<p>` | its data is cut to a shorter length, 0 to one less than it had |

- `<p>` is a percentage, more than 0 and at most 100, with up to three
  decimals (`0.5`, `2`, `100`). A kind appears once.
- The faults apply in the table's order: a stall comes before the
  frame either way, and a frame dropped or lost to an overflow isn't
  also corrupted or cut short.
- `?seed=` (1 to 2^32 - 1, default 1) seeds the draws, so the same URI
  gives the same faults on every run and every platform: a pure
  generator (xorshift, `core/src/replay_faults.h`), one draw per listed
  fault per frame in the table's order, then the corruption's bit or the
  truncation's length. A failure seen once can be replayed exactly.
- A loop (`?loop=1`) draws on: each pass gets different faults.
- Nothing else in the replay changes: the rows played, their order,
  their bytes when no fault touched them, the pacing at `?speed=`.

### 2. The session says what it injected

- `ECUIO_SESSION_INFO_REPLAY_FAULTS`, a session-info flag, set when the
  URI has faults, so the activity log's "Session:" line and a recording
  of the replayed session say they came from a faulted replay.
- `ecuio_session_get_replay_faults(session, out)` (`core.h`) copies the
  counts so far: frames read, then each kind's injections. A session
  that isn't a faulted replay reports zeros. Readable after close, as
  the other reports are.
- `havne-cli stream` writes them in its `end` line (`faults`), so a
  script compares what was injected with what it saw.

### 3. Tests: what has to hold

On the vehicle fixture (`tests/fixtures/replay/`) and the mock's
recordings:

- **The faults themselves** (`CoreTest.ReplayFaults*`): the query's
  refusals; the same seed delivering the same frames twice, another
  seed others; each kind's rate within a binomial bound over the
  fixture; a drop removing exactly the frames counted; an overflow
  flagging the next frame delivered; a corruption differing from the
  clean replay in one bit; a truncation shorter; a stall shifting the
  stamps that follow by its length and pacing them so.
- **The stack under them**
  (`BridgeTest.AFaultedReplayFinishesAndRecordsWhatItDelivered`, the
  test car's engine start with every kind): the replay finishes and
  the activity log says what its faults did; the recording holds the
  frames delivered, row for row; the bridge's overflow count is the
  frames flagged in it. `havne-cli stream` ends and counts them the
  same way (`CliGoldenTest.ReplayWithFaultsEndsAndCountsThem`).
- **The rest is each rule's own test**, which a faulted replay would
  only run again: a frame cut short counts toward absent and one whole
  reply keeps a signal present (`CoreTest.AFieldPastEveryReplyIsAbsent`,
  the bridge's reading of it in
  `BridgeTest.LiveHidesAFieldTheRepliesComeWithout`); values age while
  nothing arrives (`BridgeTest.AgeMsRoleEmittedAndRefreshesWhileStalled`),
  and a stall holds the frames back (`CoreTest.ReplayFaultsStallThePace`).
  The faults run under the sanitizer job with the rest of `test_core`.
- **Fuzzing:** `fuzz_replay_file` also takes a `?faults=` query drawn
  from the input.

### 4. Faults at the ISO-TP level: the recording as the ECU

A replay plays whole messages, so a broken Consecutive Frame has
nothing to happen to there. The same recording can answer a live
session instead: `tests/vcan_recording_ecu.h` (Linux, test-side) is an
ECU on `vcan0` that answers each Service 01 request of a requesting
`socketcan://` session with the recording's reply for that PID at its
own time since it started, so the recording plays at its pace, and a
request of several PIDs with J1979's packed answer. The bitmaps are the
car's, so the probe walks the car's chain; a PID the bitmaps list that
the recording holds no reply for is refused (`7F 01 12`), as a PID the
car doesn't answer. A pack's answer is longer than a Single Frame, so
the session's packing (ADR-0039 Appendix A) runs through core's ISO-TP
receiver (ADR-0040) on the test car's own bytes.

The ECU frames its replies itself, on a raw socket, so it can break
them:

| Fault | What the reply gets |
|---|---|
| silent | nothing |
| late | the whole reply 130 ms after the request, past the engine's 100 ms wait |
| bad sequence | a segmented reply's last Consecutive Frame carries the wrong sequence number |
| stalled | a segmented reply's last Consecutive Frame comes 160 ms after the frame before, past N_Cr (150 ms) |

- Each fault hits every Nth reply, single-frame and segmented replies
  counted apart, so a run is the same each time. Counting them together
  falls into step with the rotation's laps: on the test car's PIDs a
  lap is fifteen replies, and every fifth reply silent hit each lap's
  one pack. Periods like 5 and 7 never break three packs in a row; a
  session whose packs fail that often goes back to one PID per request,
  as it should.
- The bitmaps are never broken: the probe's walk ends at its first
  silence, which is its own rule.

What has to hold, under every fault (`SocketCanFaultTest`): the session
delivers each reply the bus carried whole, in order, and nothing else
(a late one is delivered, a broken one never is); the walk reads the
car's bitmaps; the rotation keeps each PID the recording holds and no
other, and each is answered; packing holds; core's Flow Control is
ISO 15765-4's; the link stays healthy.

## Alternatives considered

- **Faults in the fakes only.** They exist (the knobs), and they stay:
  they reach the request path, which a replay doesn't. But they inject
  into canned traffic; this is the same faults in the test car's own.
- **Faults applied to the file** (a script that writes a damaged
  `frames.csv`). Works for drops and corruption, but a stall would have
  to rewrite every later stamp, an overflow flag in the file isn't an
  injected loss, and each fault set is a file to keep. A query keeps the
  recording as it was and makes the faults a parameter.
- **A random seed by default.** Faults would differ run to run, which
  finds more over many runs but makes a failure hard to repeat. Seed 1
  by default, and a test that wants spread loops over seeds.
- **Faults at the ISO-TP level in the replay.** A replay plays whole
  messages, so a bad Consecutive Frame has nothing to happen to; the
  recording answering a live session as the ECU, frame by frame, does
  (section 4).
- **The kernel's ISO-TP as the ECU** (`vcan_bus::Ecu`, which
  `SocketCanRequestTest` uses). It frames correctly and can't be made
  to break a frame; the raw socket can, and the kernel stays the
  independent check of the frames done right (`IsoTpConformanceTest`).
- **Faults drawn from a seed, as in the replay.** Random draws break
  three packs in a row now and then, and the session then rightly
  unpacks, so a test of what holds under the faults would fail at
  random. Fixed periods break as often and never that way.

## Consequences

- **Good:**
  - The error handling meets the test car's traffic, not only canned
    bytes, on every platform, with no hardware.
  - A fault seen once repeats exactly from its URI.
- **Bad:**
  - Another query key on `replay://`, and a getter and a flag in the
    C ABI for what's otherwise a testing tool.
  - Rates are per frame, not per message or per second; a burst of
    losses is a high rate, not a pattern of its own.
