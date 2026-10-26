# ADR-0025: One J2534 backend — every vendor DLL runs in a surrogate process

- **Status:** Accepted.
- **Date:** 2026-10-26. All three steps (§6) built with this record.
  Step 2 also took the health and connect suites' `/InProc` half and
  their parameter: with routing sending x64 DLLs to the surrogate, no
  session can reach in-proc. Step 3 also took `ecuio_j2534_backend_name`
  (the tests that read it read `ecuio_j2534_backend`), and the bridge
  names a session reporting `ECUIO_BACKEND_J2534_INPROC` as an unknown
  backend.
- **Builds on:** [ADR-0008](0008-x86-surrogate-host.md) (the surrogate
  host, its job object and pipe); [ADR-0009](0009-pe-architecture-routing.md)
  (routing by PE machine type, which this record replaces);
  [ADR-0011](0011-vendor-call-relay-wire-forms.md) (the relayed calls'
  wire forms); [ADR-0014](0014-single-threaded-surrogate.md) (the
  single-threaded child and its batched reads);
  [ADR-0018](0018-connect-failure-reporting.md) (the codes and detail a
  refusal carries); [ADR-0019](0019-session-info.md) (the session's
  `backend` field).

## Context

The loader has two backends (ADR-0009). An x86 vendor DLL loads in
`ecuio-j2534-surrogate.exe`, an x86 child process; an x64 DLL, or a
file whose machine type the sniff can't read, loads in the app's own
process. Every vendor-call helper in `j2534_loader.cpp` branches on
`is_surrogate`, and each branch is a separate implementation of the
same J2534 semantics:

- **Two implementations of one contract.** Loading and export
  resolution, the read loop (indications skipped, the count trusted
  only under `STATUS_NOERROR` and `ERR_TIMEOUT`), writes, the ioctl
  relay and the filters exist once in the loader and once in the child
  (`surrogate/surrogate_main.cpp`). Shared helpers cover part of it
  (`ecuio_trusted_count`, `ecuio_pt_fill_msg`, the SCONFIG flatten
  pair, `ecuio_pt_is_indication`); the rest is parallel code, and the
  backends already read differently (in-proc one message per call, the
  surrogate a batch of up to 16).
- **Divergence has been a recurring bug class.** Load flags, the
  ioctl's export spelling, the battery reading's ID, TX indications and
  the ioctl's output cut each differed between the backends at some
  point, and each fix had to land twice. Parity is held by running
  suites on both (`J2534HealthTest`, `J2534ConnectTest`, parameterized
  `/InProc` and `/Surrogate`); `J2534StreamTest`, the largest J2534
  suite, runs in-proc only.
- **Only the surrogate has met hardware.** The one adapter on hand, the
  OBDX Pro VX, ships an x86 DLL, so every BENCH and vehicle session
  ran through the surrogate (docs/bench-sessions/). The in-proc
  backend has met only the fake (docs/testing.md, "The in-proc backend
  has no real-hardware coverage").
- **x64 J2534 DLLs are rare.** J2534-1 v04.04 specifies a 32-bit DLL.
  A few vendors also build a 64-bit one (Kvaser documents both; one
  open-source tool reports running an OBDX Pro FT as x64); none is on
  hand or planned.
- **The in-proc backend writes no session log.** The surrogate path
  logs every vendor call and status to `%TEMP%\ecuio-surrogate.log`
  (and each frame with `ECUIO_SUR_LOG_FRAMES=1`); an in-proc session
  leaves no record of its vendor calls.
- **A crashing vendor DLL in-proc takes the app with it,** the open
  recording included. In the surrogate it ends the child, inside a
  `KILL_ON_JOB_CLOSE` job (ADR-0008).
- **The x64 surrogate already exists.** The x64 build tree builds
  `ecuio-j2534-surrogate` for its own architecture, and CI runs it on
  every push: `J2534SurrogateTest`, and the `/Surrogate` halves of the
  health and connect suites with routing forced
  (`ecuio_j2534_force_surrogate_for_testing`). The x86 child is
  checked by the local cross-arch run only.

## Decision

### 1. Every vendor DLL loads in a surrogate of its own architecture

- `ecuio_j2534_open` sniffs the PE machine type (ADR-0009's sniff,
  which tells three outcomes apart, below) and spawns the matching
  surrogate:

  | The file | Surrogate |
  |---|---|
  | A PE for x86 (I386) | `ecuio-j2534-surrogate.exe`, as today |
  | A PE for x64 (AMD64) | `ecuio-j2534-surrogate-x64.exe`, new |
  | A PE for another machine (ARM64, IA64, …) | `ecuio-j2534-surrogate-x64.exe`, which loads it or refuses it as the app's own process would |
  | Unreadable, or not a PE | Refused: `ECUIO_ERR_LOAD_FAILED` before any spawn |

- The x86 name stays, so the release layout, the run sheets and the
  surrogate's session log keep theirs. The x64 tree's surrogate target
  takes the `-x64` name.
- **The sniff tells three outcomes apart** that ADR-0009's returned as
  one (unknown, 0, loaded in-proc): a file it can't open, a file that
  isn't a PE, and a PE for a machine other than x86 and x64. Each keeps
  the outcome it has in-proc:
  - **Unreadable, or not a PE:** in-proc these fail at `LoadLibraryEx`
    with no spawn. The sniff refuses them itself, still with no spawn,
    and says why (ADR-0018's detail): the open's Windows error
    ("couldn't open the DLL (Windows error 2)"), or "the file isn't a
    Windows DLL".
  - **A PE for another machine** goes to the x64 surrogate, a process
    of the app's own architecture, so `LoadLibraryEx` gives the answer
    the app's process gives it: an ARM64 DLL fails there with Windows
    error 193 (`ERROR_BAD_EXE_FORMAT`), reported by the child's load
    detail, while an ARM64X DLL on Windows on ARM, which Microsoft's
    Arm64X documentation says an x64 process can load, loads (not
    tried: no such DLL or machine is on hand). It costs a spawn for a DLL that most likely won't
    load, and keeps the in-proc behavior without the in-proc path.
- `dll_arch` and discovery's architecture keep their values: anything
  but x86 and x64 is `ECUIO_DLL_ARCH_UNKNOWN`.
- **No fallback.** A missing x64 surrogate is `ECUIO_ERR_SURROGATE_FAILED`
  with the existing detail naming the exe, never a quiet in-proc load:
  a fallback is the split this record removes.

### 2. The in-proc vendor calls are deleted

- `j2534_loader.cpp` keeps discovery, the path rule, the sniff and the
  handle; each vendor-call helper forwards to the client
  (`ecuio_j2534_sur_*`). `is_surrogate`, the handle's function pointers
  and module, and `ecuio_j2534_force_surrogate_for_testing` go.
- The child is the one place a vendor function is called. A change to
  J2534 semantics lands once.
- **No in-proc mode for tests.** A test-only in-proc backend would keep
  the second implementation alive and bring the divergence back
  through the suite.

### 3. The session's backend

- `ecuio_session_info_t.backend` is always
  `ECUIO_BACKEND_J2534_SURROGATE` for a J2534 session.
  `ECUIO_BACKEND_J2534_INPROC` stays defined (append-only values),
  reported by no session. `dll_arch` still carries the sniff, so the
  activity log's line reads "Session: x64 DLL, through the surrogate".

### 4. Locating the surrogates

- Both exes sit next to the running exe: the app's build copies the
  x86 one from `build-win32` (as today) and its own tree's x64 one.
- `ECUIO_SURROGATE_EXE` keeps relocating the x86 surrogate, and
  `ECUIO_SURROGATE_X64_EXE` relocates the x64 one. The same rules apply
  to each: an absolute path to an existing file of that exe's name,
  always logged. Two variables, not one whose meaning follows the DLL:
  the cross-arch run sets the x86 one while other cases in the same
  run load the x64 fake.
- `packaging/windows/stage.ps1` fails the release when either exe is
  missing beside `qttune.exe` (it checks the x86 one today) and checks
  both for a C runtime import (each links it statically, `/MT`), so the
  zip always carries both.

### 5. Tests reach the fake only through its control page

- The test hooks a test resolves from the fake in its own process
  (`InjectFrame`, `GetLastConnectParams`, `GetWriteCount`,
  `GetRecordedWrite`, `GetLastFilterStart`, `GetLastFilterStop`,
  `SetFilterGating`, `GetDisconnectCount`, `GetBufferState`,
  `GetCurrentHandle`, `GetLastCallThreadIds`) can't see the child's
  instance. Their facts move onto the control page
  (`tests/fake_control.h`), which already carries knobs, the inbox,
  the write count, the opener's PID and the disconnect and close
  counts: the page grows the recorded writes (the first 16, bytes and
  flags), the last connect's parameters, the last filter started and
  stopped, and filter gating as a knob.
- **The page's layout is pinned and versioned.** Fixed-width fields
  only, and the `static_assert` on `sizeof(Page)` states the exact size
  (as it does today), compiled into the x86 and the x64 fake, so a
  field that pads differently on one architecture fails that tree's
  build. The page also gains a layout version beside `kMagic`, raised
  with any change; the fake ignores a page whose version isn't its own,
  as it ignores a wrong magic, and a test that sees the fake's
  `opener_pid` unset fails at once. That catches a stale fake against
  new tests: `build-win32`'s x86 fake is built apart from the x64 tests
  (the cross-arch run), and the page is a test header that the
  surrogate's fingerprint handshake doesn't cover. The mapping is
  `sizeof(Page)` on both sides, so it has no page-size limit to keep.
- The page reaches an in-proc fake as well, so the tests move first,
  with routing unchanged, and pass on the code they will verify.
- **The bridge's thread checks.** `AsyncConnectOpensOffGuiThread` reads
  which thread ran `PassThruOpen`; under the surrogate that is the
  child's only thread. The property it guards, that the GUI thread
  never waits on a vendor call, stays pinned by its timing (the connect
  returns while a stalled open holds the child) and `connecting`. The
  disconnect test reads the page's `disconnects`.
- `J2534HealthTest` and `J2534ConnectTest` lose their `/InProc` half
  and their parameter; the cross-arch run's filter in
  `scripts/verify.sh` follows the new names. `test_j2534_load`'s
  loader-only cases (exports resolved, the legacy spelling, a missing
  export) run through the child, or are dropped where
  `J2534ConnectTest` already covers them.
- **A crash test.** A fake knob that ends the child's process during a
  read (`ECUIO_FAKE_EXIT_ON_READ`) shows the isolation this record
  claims: the session reports a hard failure, the test process lives,
  and a new session connects.

### 6. Steps

Each is one commit, verified by `scripts/verify.sh`, cross-arch
included.

1. **Tests onto the control page.** The page grows (§5); every J2534
   test and the bridge's fake tests read it; the in-proc-only hooks
   leave `fake_j2534.def`. Routing unchanged; both trees rebuilt (the
   page layout is shared).
2. **x64 DLLs through the x64 surrogate.** The routing table (§1), the
   exe's name and its deploy, `ECUIO_SURROGATE_X64_EXE`, `stage.ps1`,
   the session line, the crash test, and ADR-0009's record: its status
   line says its routing table is superseded by this record, and a
   dated appendix says what replaced it (the table stays, as a record
   isn't rewritten; ADR-0008's status line names ADR-0014 the same
   way). The in-proc code is unreachable from here.
3. **The in-proc backend deleted** (§2), with the `/InProc` halves and
   `ecuio_j2534_force_surrogate_for_testing`. A deletion: no test's
   expectations change.

## Alternatives considered

- **Keep both backends, with parity enforced by tests.** Run every
  J2534 suite on both. Cheapest, but every future J2534 change is
  still written twice, the x64 path stays unproven on hardware, and
  in-proc sessions keep no vendor-call log and no crash isolation.
- **One shared vendor-call layer compiled into both hosts.** Move the
  vendor semantics into one source file that core and the child both
  compile, keeping in-proc. It shrinks the divergence but keeps two
  hosts for the speed of a direct call, which nothing needs (below),
  and leaves the log and the crash isolation as they are.
- **Refuse x64 DLLs instead of shipping an x64 surrogate.** Smaller
  (no second exe in the release), and honest about what has been
  tested. Rejected because the x64 surrogate already builds and runs
  in CI, so supporting an x64 adapter costs one copy and one check,
  against a refusal a user can't work around.

## Consequences

- One implementation of the vendor calls, and the one the VX has run.
  CI's x64 Windows job runs the whole J2534 suite through the x64
  child, the shipping path for x64 DLLs; the x86 child is still
  checked only by the local cross-arch run.
- Every J2534 session writes the surrogate's session log, so an x64
  adapter's vendor calls and statuses are on record like the VX's.
- A crashing x64 vendor DLL ends its child, not the app; the session
  reports a hard failure and the recording closes.
- **A process per session.** A first look, not a benchmark: gtest's
  per-case times for `Backend/J2534ConnectTest.InfoDescribesTheAdapter`
  and `.InfoCarriesTheUrisBaudAndChannel` (each a create and close
  against the x64 fake, Release build, run once in one process, no
  warm-up, 2026-10-26) were 2 ms and 0 ms in-proc, 89 ms and 29 ms
  through the x64 surrogate; the first surrogate case pays the exe's
  cold start. Step 2 measures it properly (connects repeated, cold and
  warm, against the fake) and records it in docs/testing.md
  "Performance". A real adapter's `PassThruOpen` takes longer than
  either. Each read is a pipe round trip, negligible at the
  transport's read cap (ADR-0014).
- **A missing x64 surrogate fails the session,** with
  `ECUIO_ERR_SURROGATE_FAILED` and the detail naming the exe, where an
  x64 DLL loaded with nothing beside the app before. The app's build
  copies it from its own tree, so a build has it; `stage.ps1` keeps a
  release from shipping without it (§4).
- **The release zip carries a second exe,** checked like the first.
- **Pointer-bearing ioctls stay out of reach.** `FAST_INIT` and
  `FIVE_BAUD_INIT` pass pointers to structs (Known Issue #20). In-proc
  they could have been called directly; now they need wire forms in the
  relay (ADR-0011) if K-line support is ever wanted.
- **Debugging a vendor DLL** means attaching to the child, not the app.
- ADR-0009's routing table is replaced by §1, and step 2 marks it so
  (§6); its sniff carries over, and so does its rule that a file that
  can't be a DLL fails without a spawn.
