# The J2534 surrogate

The surrogate is the out-of-process host that loads a vendor J2534 DLL of
the other architecture and relays the session's calls over a pipe
([ADR-0025](adr/0025-one-j2534-backend.md)). This is its operational
detail; [j2534.md](j2534.md) covers the rest of the J2534 stack.

[ADR-0008](adr/0008-x86-surrogate-host.md) covers the original design
and [ADR-0014](adr/0014-single-threaded-surrogate.md) its current,
single-threaded shape.

**Spawn.** The client opens the session log, finds the exe, and
creates the pipe and the job object. It then launches
the surrogate (`<exe> <dll> <pipe>`) suspended, inside a
`KILL_ON_JOB_CLOSE` job object. **The child connects the pipe
FIRST**, then loads the DLL, resolves its exports, and performs
`PassThruOpen` only when `OP_OPEN` arrives: inside the request loop.
The child's startup (resumed to its pipe connected) times out after
8 s, and so does `OP_OPEN`'s reply (`ECUIO_SUR_OPEN_MS` overrides the
second; test knob).

**Build fingerprint.** The client and the child compile the shared
headers in separate trees, so each carries `ECUIO_SUR_FINGERPRINT`,
32 bits of a SHA-256 over `j2534_ptypes.h` and `surrogate_protocol.h`
that the configure step generates (`generated/ecuio/sur_fingerprint.h`).
`OP_OPEN` carries the client's. A child with a different one answers
`STATUS_FAILED` without calling the vendor, and every child echoes its
own in the reply, which the client checks as well, so a child built
before the check is caught too. Either way the open fails with
`ECUIO_ERR_SURROGATE_FAILED`, and the session log names both values.

- **The exe** is chosen by the DLL's architecture:
  `ecuio-j2534-surrogate.exe` for x86, `ecuio-j2534-surrogate-x64.exe`
  for anything else, each the one next to the running app or at its
  variable, `ECUIO_SURROGATE_EXE` or `ECUIO_SURROGATE_X64_EXE`. A
  variable only relocates its surrogate: it must be an absolute path
  to an existing file of that exe's name, or the open fails with the
  reason in the session log
  (`J2534SurrogateTest.SurrogateExeOverrideIsRestricted`). A missing
  exe fails the session ("no surrogate (<exe>): none next to the
  app"); nothing falls back to loading the DLL in the app.
  The spawn line names the exe and says when it came from the
  variable. The exe is passed as the application name, so
  `CreateProcess` never searches for it.
- **The pipe** is `\\.\pipe\ecuio-j2534-<pid>-<seq>-<32 hex>`, the hex
  from `BCryptGenRandom`. It is created with
  `FILE_FLAG_FIRST_PIPE_INSTANCE` (an existing pipe of that name fails
  the open rather than being joined), `PIPE_REJECT_REMOTE_CLIENTS`, one
  instance, and a DACL granting the current user alone. The default
  DACL would let any local user open it for reading, enough to take
  the only instance before the child. `ecuio_j2534_sur_create_pipe`
  (`core/src/j2534_surrogate_pipe.cpp`) makes it, and `J2534PipeTest`
  checks each protection on the pipe it makes.
- **Handles:** the child inherits the session log and nothing else.
  A `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` names that one handle; without
  it, inheritance would pass every inheritable handle in the app. The
  client clears the log's inherit flag right after the spawn.
- **Text:** the DLL path reaches the child as UTF-16 (`wmain`), so a
  non-ASCII path loads there too.

**Session log.** Each session has one log file holding both halves:
the child's stdout/stderr and the client's own lines.
- **Where:** `%TEMP%\ecuio-surrogate.log`. Each spawn takes that
  name and keeps the log it replaces as
  `ecuio-surrogate.<YYYYMMDD-HHMMSS-mmm>.log`, named after the old
  log's last write (local time); the newest 50 are kept, and a suite
  run spawns dozens. A session that starts while another holds that
  file gets
  `%TEMP%\ecuio-surrogate-<pid>-<n>.log` instead of running without
  a log. `ecuio_j2534_sur_log_path()` returns the session's path
  (UTF-8), and the client's second line names it.
- **Stamps:** both sides stamp local wall-clock time
  (`HH:MM:SS.mmm`), so the file reads as one timeline. Each side
  writes whole lines in one call, so lines never splice. Child lines
  start `[surrogate `; client lines start `[sur `.
- **The child logs** each startup stage, and the vendor status of
  every `OPEN`, `CONNECT`, `DISCONNECT`, `IOCTL`, `START_FILTER` and
  `STOP_FILTER`. It logs `OP_WRITE` when its status or written count
  changes, and `OP_READ` when its status changes or frames arrive, so
  expect about 10 lines/s while an ECU answers.
- **Payloads, opt-in:** with `ECUIO_SUR_LOG_FRAMES=1` in the app's
  environment (the child reads it once, at spawn), the child
  also logs every message it writes and every one the vendor returns,
  before anything is dropped or stripped. The bytes include the 4-byte
  CAN ID, so a reply with 4 data bytes is `len 8`. Each vendor call
  that brought messages adds an `OP_READ call` line: the call's
  timeout, the count asked and got, the status, and how long the
  vendor held it, which is how an adapter's hold of a read is
  measured. Set it at launch,
  or tick Record sessions in Settings, which sets it for the spawns it
  records unless it was set at launch
  ([ADR-0017](adr/0017-session-recording.md)):

  ```
  OP_WRITE frame: status 0, tx 0x00000040, len 6: 00 00 07 E0 01 0C
  OP_READ frame: status 0, rx 0x00000000, ts 4097, len 8: 00 00 07 E8 41 0C 1F 40
  ```

  Indications are logged too, marked "(indication, not delivered)".
  Bytes past 64 are counted (`+N`), not shown. It adds about two lines
  per request. Every J2534 session, x86 or x64, has the log and
  the capture.
- **The client logs** the spawn first (child pid, surrogate exe, DLL,
  then the log path), written while the child is still suspended.
  After that it logs only trouble:
  - a rejected or missing surrogate exe, a failed pipe or job setup,
    or a failed CreateProcess, naming the exe;
  - a handshake timeout;
  - a blocked `OP_OPEN` (no reply, child alive), and the child's
    decoded exit code;
  - a refused `PassThruOpen` or `PassThruConnect`, with the vendor
    status;
  - transaction failures and desyncs;
  - a skewed reply (`num_frames` or ioctl `data_len` past the wire
    limit);
  - frames dropped with a late `OP_READ` reply (below).
- **stderr:** the client also writes its lines to the app's stderr,
  but a GUI launch needs no redirect to keep them.

**Late replies.** When a transaction times out, the client remembers
its request ID. Before the next request, it reads and discards that
late reply, so the next request isn't paired with it. If the late
reply was an `OP_READ`'s, its frames are dropped with it, and the
client logs `drain_pending: dropped N frame(s)`. Getting there takes a
read the child answers more than 500 ms past its own timeout.

**Unresponsive children.** If the late reply doesn't arrive within that
next request's timeout either, the child is wedged, not slow: the
client logs `child unresponsive` and every later transaction fails at
once. Without that, a teardown would pay a full 3 s per filter, for
the disconnect and for the close. Close then kills the child through
the job object without waiting for a polite exit. A disconnect from a
wedged child costs about 6 s at worst: the timeout in flight plus the
one that marks the child unresponsive.

**Replies out of step.** The child answers one reply per request, in
order, so a reply whose request ID isn't the request's (a request the
client thinks it never sent, after a write that timed out but had gone
through; or a skewed child) means this request's own reply is still in
the pipe, and every later transaction would read the one before its
own. The client logs `DESYNC` and marks the child unresponsive, as for
a reply cut off part way: later calls fail at once without reaching the
child, the session's reads fail hard ("Adapter fault"), and close kills
the child (`J2534SurrogateTest.RequestIdMismatchMakesTheChildUnresponsive`).

This ordering makes the wire itself the diagnostic:

- Handshake times out, child alive: the child never opened the
  pipe; process startup stalled before any vendor code ran.
- Pipe connected: the child started. It hasn't loaded the DLL yet.
- Child exits before answering `OP_OPEN`: its exit code names the
  stage: 4 (the DLL didn't load) or 5 (an export is missing).
- `OP_OPEN` answered: the DLL loaded and all nine exports resolved.
  Then either the fingerprints differ ("built from other shared
  headers", and the vendor was never called), or `PassThruOpen`
  returned: a device ID, or a refusal with the vendor status. After
  either failure the client shuts the child down.
- `OP_OPEN` unanswered, child alive: the vendor is blocked in
  `DllMain` or `PassThruOpen`, typically waiting for absent hardware.
  `OP_OPEN` gets its own window, apart from the child's startup. On
  timeout the client
  terminates the child, because a single-threaded child stuck in
  vendor code can't answer `OP_SHUTDOWN`. Only a reply that *timed
  out* counts as blocked. A broken pipe means the child is exiting
  with code 4 or 5, so the client waits for the exit and decodes it.
  Checking only whether the child is alive would race the moment
  between the pipe closing and the process ending, and log a failed
  load as a blocked vendor.
- A failed handshake cancels the pending `ConnectNamedPipe` before the
  client releases its event and pipe.

**One thread, one request at a time** ([ADR-0014](adr/0014-single-threaded-surrogate.md)).
The child reads a request, makes at most one vendor call, writes the
response and loops, so vendor calls are serialized by construction
and no lock exists. The vendor status in each response is data: a
refused ioctl, a failed filter stop or an empty read is answered and
the loop continues. Only `OP_SHUTDOWN` or a broken pipe ends it,
after which the child disconnects any open channel, closes the
device and unloads the DLL. A successful `OP_DISCONNECT` clears the
channel, so teardown never disconnects twice. Whether a device and a
channel are open is tracked with explicit flags, not by treating ID 0
as "none": J2534 doesn't reserve 0. A vendor call that never
returns leaves the child hung; the client's transaction times out,
and the kill-on-close job reaps the child at close.
`ecuio_j2534_sur_close` says how the child ended: `ECUIO_OK` only when
it was still running, answered `OP_SHUTDOWN` and exited 0;
`ECUIO_ERR_SURROGATE_FAILED` (and a session-log line) when it had
already exited on its own, or was unresponsive or still running and
killed. The transport ignores it at teardown; the tests' closes assert
it, so each one shows its child lived through every refusal before.

**Child exit codes.** These are the first thing to check when a spawn
fails. The client decodes the code into the session log
(`open: child exited with code N (...)`). On exit 5 the child's lines
names every missing export, so the log alone identifies the gap;
`dumpbin /exports` remains the cross-check for x86 name decoration.

| Code | Stage that failed |
|---|---|
| 1 | Argument count |
| 2 | Retired (attached the shared ring before ADR-0014). Not reused. |
| 3 | Retired (mapped the ring view and event before ADR-0014). Not reused. |
| 4 | `LoadLibrary` of the vendor DLL |
| 5 | A mandatory export is missing (on x86, check export name decoration) |
| 6 | Reserved. |
| 7 | Couldn't connect to the app's pipe |

**Environment knobs** (test-oriented; unset in production):

- `ECUIO_SUR_OPEN_MS`: replaces the 8000 ms window for `OP_OPEN`'s
  reply (floor 100 ms), not the child's startup, which keeps its 8 s
  so a loaded machine's slow start isn't taken for a blocked vendor.
  The blocked-open test fails at its window instead of burning 8 s.
- `ECUIO_SUR_TEST_FINGERPRINT`: replaces the child's build
  fingerprint, so it stands in for a child from another build and
  refuses every `OP_OPEN`. `MismatchedFingerprintRefusesOpenBeforeVendor`
  uses it.
- `ECUIO_SUR_TEST_REPLY_TWICE=<opcode>`: the child sends its first
  reply to that opcode twice (never `OP_READ`'s), putting the pipe one
  reply behind. `RequestIdMismatchMakesTheChildUnresponsive` uses it.
- `ECUIO_SUR_TEST_SKEW=<opcode>`: the child's first reply to that
  opcode claims more than the wire carries, as a child built against
  other limits would: an `OP_READ` claiming two frames, one past
  `ECUIO_SUR_MAX_FRAMES_PER_READ`, an
  `OP_IOCTL` a `data_len` past `ECUIO_SUR_IOCTL_DATA_MAX`, any other
  opcode a frame on a reply that takes none (never `OP_OPEN` or
  `OP_SHUTDOWN`). The frames claimed are never sent. The three
  `J2534SurrogateTest` cases named for those replies use it.
- `ECUIO_FAKE_CONTROL`: names the fake's control page
  (`tests/fake_control.h`), a named shared-memory page the test process
  creates. Whichever fake instance serves the session reads it on every
  call, the surrogate child's included. A knob set there overrides the
  environment variable of the same name and takes effect mid-session,
  which the child's spawn-time environment can't do. A test also
  queues received messages on it, with any `RxStatus`, and reads what
  the fake publishes there: the process that opened it, its successful
  writes (counted, the first 16 kept whole with the fake's microsecond
  stamp), the last connect's
  parameters, the last filter started and stopped, its disconnects
  and closes by the IDs it handed out, and its `ReadMsgs` calls (counted,
  the first 16 calls' `Timeout` kept). It is the only way a test reaches
  the fake, inside whichever surrogate serves the session. Its layout
  is pinned by size and versioned (`kLayout`): a fake built from
  another layout ignores the page.
- `ECUIO_FAKE_OPEN_DELAY_MS`: makes the fake's `PassThruOpen` sleep
  before executing (max 60000), simulating a vendor blocked on absent
  hardware. The child inherits the parent's environment, so an x64
  test process arms the x86 child's fake with this knob: the same
  cross-arch consideration that motivates `OP_INJECT`.
- `ECUIO_FAKE_IOCTL_DELAY_MS`: makes the fake's first ioctl after
  load sleep for the given time (max 60000). The
  `TimedOutOpDoesNotDesyncNextRequest` test uses 3500, just past the
  client's 3 s transaction timeout; `UnresponsiveChildFailsFastAfterTwoMissedDeadlines`
  uses 20000, which outlasts both deadlines.
- `ECUIO_FAKE_READ_DELAY_MS`: the same for the fake's first
  `ReadMsgs`: it sleeps first (max 60000), then reads the queue.
  `LateReadReplyIsDroppedAndLogged` uses 1500, past a 50 ms read's
  550 ms transaction timeout.
- `ECUIO_FAKE_DISCONNECT_DELAY_MS`: makes every fake
  `PassThruDisconnect` sleep first (max 60000): a slow vendor teardown.
  Re-read on every call. The bridge's async-disconnect tests set it
  on the control page mid-session.
- `ECUIO_FAKE_EXIT_ON_READ`: `1` makes the fake's next `ReadMsgs`
  end its process (exit 9), a vendor DLL that crashes, but only in a
  surrogate (the exe's name), never a test runner that loads the fake.
  `AVendorDllThatCrashesEndsOnlyItsChild` sets it mid-session.
- `ECUIO_FAKE_ZERO_IDS`: `1` makes `PassThruOpen` return device 0 and
  `PassThruConnect` channel 0, read at open.
  `DeviceAndChannelIdZeroAreRealIds` uses it.
- `ECUIO_FAKE_READ_STATUS`: makes every fake `ReadMsgs` return the
  given vendor status with no messages (for example `0x09` for a
  quiet bus, `0x07` for a dead adapter). Re-read on every call, and
  it always zeroes the count. The link-health tests use it.
- `ECUIO_FAKE_UNTOUCHED_COUNT_STATUS`: makes every fake `ReadMsgs`
  return the given status with the caller's count and message buffer
  left as passed, emulating a vendor that doesn't write its outputs on
  error. `UntouchedCountUnderHardErrorReportsUnhealthy` (both
  backends) and `HardErrorUntouchedCountDeliversNoFrames` (surrogate)
  use it.
- `ECUIO_FAKE_WRITE_STATUS`: makes every fake `WriteMsgs` return
  the given status without recording the message or queuing a
  response, leaving the count as passed: the VX's no-ACK write
  failure. Re-read on every call. `WriteFailureLatchesRequestFailedAndClears`
  and `HardReadFailureOutranksRequestFailed` use it.
- `ECUIO_FAKE_WRITE_ZERO_COUNT`: `1` makes every fake `WriteMsgs`
  return `STATUS_NOERROR` with a count of 0, recording and answering
  nothing: a vendor reporting success for a message it never sent.
  Re-read on every call. `ZeroWrittenCountIsAFailedWrite` uses it.
- `ECUIO_FAKE_SPEC_READ_SEMANTICS`: `1` switches the fake's
  `ReadMsgs` to J2534-1 behavior: up to `NumMsgs` messages, held until
  that many have arrived or the timeout passes; `ERR_TIMEOUT` with a
  short batch when the timeout is nonzero, and `ERR_BUFFER_EMPTY` on
  an empty timeout-0 read. It skips filter gating.
  `SpecTimeoutReadDeliversPartialBatch` uses it.
- `ECUIO_FAKE_VX`: `1` makes the fake behave like the OBDX Pro VX as
  measured ([bench](bench-sessions/2026-10-22.md),
  [vehicle](bench-sessions/2026-10-22-vehicle.md)). Reads batch and
  hold like spec mode, but a short batch comes back `STATUS_NOERROR`
  and any empty read `ERR_BUFFER_EMPTY`. Each accepted write queues a
  TX indication (the 4-byte CAN ID, `RxStatus` `0x09`), and its reply
  arrives 4 ms later, stamped from a 32-bit microsecond clock. A read
  asked to wait 0 ms holds 50 ms, as the VX's did on the car
  ([2026-11-04-two-ecu-car](bench-sessions/2026-11-04-two-ecu-car.md)).
- `ECUIO_FAKE_NO_ACK`: with `ECUIO_FAKE_VX`, a bus with no other
  node: the first three writes after each connect are accepted, with
  an indication and no reply; every later one blocks 500 ms, fails
  `ERR_FAILED` with the count left at 1, and is echoed with `RxStatus`
  `0x201`, as the VX did.
- `ECUIO_FAKE_REPLY_DELAY_MS`: how long after a write its reply
  arrives (default 0, or 4 under `ECUIO_FAKE_VX`). A read sees only
  messages that have arrived.
- `ECUIO_FAKE_PENDING`: `1` answers each request with "response
  pending" (`7F <service> 78`) at once, ahead of its reply.
  `PendingReplyHoldsTheRequestForItsAnswer` uses it.
- `ECUIO_FAKE_UNSUPPORTED_NRC`: the code of the negative reply to a PID
  or DID the fake's ECU doesn't support, in hex or decimal, instead of
  `0x12` and `0x31`; `0` answers nothing. The probe's tests use `0` so
  that only the bitmaps can drop a PID.
- `ECUIO_FAKE_OPEN_STATUS`, `ECUIO_FAKE_CONNECT_STATUS`,
  `ECUIO_FAKE_FILTER_STATUS`, `ECUIO_FAKE_VBATT_STATUS`,
  `ECUIO_FAKE_VERSION_STATUS`: make `PassThruOpen`,
  `PassThruConnect`, `PassThruStartMsgFilter`, `READ_VBATT` or
  `PassThruReadVersion` return the given status, and the fake's
  `PassThruGetLastError` describe the refusal. `J2534ConnectTest` uses
  them, cross-arch too.

**Wire ops** (`surrogate_protocol.h`, append-only):

| # | Op | Notes |
|---|---|---|
| 1 | `OPEN` | `PassThruOpen`, after the build-fingerprint check. The child loads the DLL after the pipe connect, before it serves this first request. |
| 2 | `CONNECT` | `PassThruConnect` |
| 3 | `IOCTL` | Flattened `SCONFIG` pairs; whitelisted |
| 4 | `WRITE` | Single message, 64 bytes max |
| 5 | `READ` | `ReadMsgs` for one message with the caller's timeout (another after a call that brought only indications); never a 0 ms wait. Returns the vendor status, and the messages whole after the response (one `ecuio_pt_rx_t` per `num_frames`) |
| 11 | `GET_LAST_ERROR` | `PassThruGetLastError`: the vendor's text for its last failed call, in the reply's first 80-byte `text` field. `ERR_NOT_SUPPORTED` when the DLL doesn't export it |
| 12 | `READ_VERSION` | `PassThruReadVersion` on the open device: the firmware, DLL and API versions in the reply's three `text` fields. `ERR_NOT_SUPPORTED` when the DLL doesn't export it |
| 6 | `SHUTDOWN` | |
| 7 | `DISCONNECT` | |
| 8 | `INJECT` | Test only; forwarded to the fake's `InjectFrame` |
| 9 | `START_FILTER` | Flat mask, pattern and flow-control arrays, 1-12 bytes |
| 10 | `STOP_FILTER` | |

**Data plane.** Reads travel over the pipe as `OP_READ`. The child
asks the vendor for one message a call with the caller's timeout (another
after a call that brought only indications, with what's left of the
timeout), copies the message whole (`ecuio_pt_rx_t`: `RxStatus`, the
stamp and `Data` with the CAN ID in front, up to `PASSTHRU_MSG`'s 4128
bytes) and writes it after the response, so a reply carries at most one
frame (`ECUIO_SUR_MAX_FRAMES_PER_READ`, 1) and costs the frame it holds.
The child's vendor buffer keeps room for 16 messages, so a vendor that
returns more than it asked writes into room of its own (the rest
counted lost). The client reads a reply exactly, in as many
pieces as the pipe hands over, caches the frame and hands it to the
next `read_msg` call, sending a new `OP_READ` only when the
cache is empty; the transport makes each message a frame (`pump_one`),
cut at `ECUIO_FRAME_DATA_MAX` and marked `ECUIO_FRAME_FLAG_TRUNCATED`
when longer. Its transaction timeout is the read timeout plus 500 ms.
The response struct is 1608 bytes; a tree built from other headers is
refused at `OP_OPEN` (the build fingerprint above), and a reply cut
off part way, carrying frames on another opcode's reply, or answering
another request, marks the child unresponsive.

**One message form each way.** The two backends must hand a vendor
identical messages, so both build them with the same functions in
`j2534_ptypes.h`: `ecuio_pt_fill_msg` for a write and a filter's mask,
pattern and flow-control messages (zeroed, ISO 15765, `ExtraDataIndex`
= `DataSize`), `ecuio_pt_sconfig_unflatten` and
`ecuio_pt_sconfig_flatten` for `GET_CONFIG`/`SET_CONFIG`'s pairs, and
`ecuio_pt_rx_from_msg` for what the vendor returns.

Per J2534-1, a read that asks for N messages with a nonzero timeout
waits for all N and returns `ERR_TIMEOUT` with however many arrived.
The client delivers such a batch and only consults the status when the
batch is empty. So the child's call asks for one message and the read
answers as soon as it arrives. A call with only indications is followed
by another with what's left of the timeout. No call asks for 0 ms:
J2534-1 reads that as "return at once", but the VX holds such a read
for about 50 ms
([2026-11-04-two-ecu-car](bench-sessions/2026-11-04-two-ecu-car.md)).
Nor does a call follow for the rest of a batch: the VX holds a call
asked to wait less than 10 ms for about 10 ms, and nearly always with
nothing in it, so each reply would wait that long
([2026-11-05-two-ecu-car-rerun](bench-sessions/2026-11-05-two-ecu-car-rerun.md));
messages already queued come back in the next reads, each call
returning at once.
A vendor that holds every read for its full timeout whatever it asked
for is why reads hold at most 10 ms while requests are served
([Requests](j2534.md#requests)). The fake's spec mode
(`ECUIO_FAKE_SPEC_READ_SEMANTICS`) holds a read and returns such a
batch, and `SpecTimeoutReadDeliversPartialBatch` proves such a batch
is delivered; a response that carries
frames under `ERR_TIMEOUT` is delivered too
(`J2534HealthTest.FramesUnderErrTimeoutAreDelivered`). The VX holds each read for its
full timeout unless its batch fills, but returns a
short batch with `STATUS_NOERROR`, not `ERR_TIMEOUT`, and an empty
read with `ERR_BUFFER_EMPTY`; both are handled. Its TX indications
come back in the same reads and are skipped before counting
([bench 2026-10-22](bench-sessions/2026-10-22.md), prep item 6).
On a vehicle its reply batches came with `STATUS_NOERROR` too, and no
read returned `ERR_TIMEOUT`
([vehicle 2026-10-22](bench-sessions/2026-10-22-vehicle.md)).

