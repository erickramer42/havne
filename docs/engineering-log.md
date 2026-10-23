# Engineering log

History and lessons learned, oldest first. This is the record of how
the project got here. The current state is in
[STATUS.md](../STATUS.md), and the decisions themselves are in
[adr/](adr/README.md).

Until 2026-10-15, STATUS.md also carried the long per-milestone
narratives. The last version with that full text is
`git show 6aaf441:STATUS.md`.

## Milestone timeline

Dates are commit dates.

| Date | Milestone | Commits |
|---|---|---|
| 2026-09-29 | v0.1.0 skeleton: C ABI core, Qt 6 / QML shell, responsive layout | `35ef664` |
| 2026-09-30 | Multi-platform CI with the core test suite; Qt 6.12.0 LTS | `3e78b2b`, `ff4cf0b` |
| 2026-10-04 | Session-scoped callback API, mock transport | `6f2657b`, `e08b455` |
| 2026-10-05 | **v0.2.0**: bridge marshaling, sort proxy, live LogPage (tagged) | `7dbdcec` |
| 2026-10-06 | v0.3.0 Phase 1 (signal decode ABI), Phase 1b (mock signal emission) | `bb8eb4b`, `601b0b6` |
| 2026-10-07 | Phase 2: coalesced LiveView; backpressure corrected | `e0c4c31`, `b47687c` |
| 2026-10-10 | Phase 2 closed: per-signal history ring and strip chart | `a38b896` |
| 2026-10-13 | Phase 3 HARNESS tier: transport vtable, registry discovery, fake DLL, loader, session-integrated J2534 transport | `844f44e`, `1752fc9`, `9b33aeb`, `0fa4883`, `1cc3334` |
| 2026-10-13 | URI query parameters; SettingsPage device picker | `728533e`, `167ac88` |
| 2026-10-14 | Frame ABI redesign, close/destroy split, message-ID decode gate (`feat!`) | `d0fb07e` |
| 2026-10-14 | Phase 3 Parts 1–3: PE architecture annotation, x86 surrogate host, cross-arch verification, PE routing | `16e3aed`, `96acce9`, `5681ca7`, `d98792f` |
| 2026-10-14 | J2534-1 v04.04 types verification and fix | `046b320` |
| 2026-10-15 | Phase 3 Part 4: write/ioctl/filter relay through both backends | `6aaf441` |
| 2026-10-15 | Phase 3 Part 5: transport adoption ([ADR-0012](adr/0012-j2534-transport-adoption.md)) | `161af4a` |
| 2026-10-16 | Issue #22: async bridge connect; surrogate pipe-first startup | `eacf1d0` |
| 2026-10-16 | Issue #22 + IoCtl export-spelling fix; first real-vendor session (VX connect cycle) | `eacf1d0` |
| 2026-10-16 | First formal BENCH session: OBDX Pro VX, quiet bus — filter shape accepted, soak clean | `e60325b` |
| 2026-10-17 | Issue #24: surrogate vendor-call serialization, single disconnect; IoCtl dllexport strip; GetDisconnectCount hook | `1a75862` |
| 2026-10-17 | ADR-0013 slice 1: profile registry, raw profile, bridge decoupling (#21 closed) | `57d7fcf` |
| 2026-10-18 | ADR-0013 slice 2: obd2 signal set, multi-PID scheduler, drain budget (formerly #25) | `8305420` |
| 2026-10-19 | ADR-0013 slice 3: profile-declared channels; bare-URI filter derivation | `c5cc6f7` |
| 2026-10-19 | Win32 surrogate auto-deploy from sibling build-win32 | `c5cc6f7` |
| 2026-10-19 | Single-threaded surrogate, batched `OP_READ`, ring retired ([ADR-0014](adr/0014-single-threaded-surrogate.md)); read-status split in both backends (#11 partial); #17 resolved by removal | `0b43261` |
| 2026-10-19 | Surrogate replies to shutdown (#27); missing exports named again; `REG_EXPAND_SZ` expansion; job assignment checked | `4b1635d` |
| 2026-10-19 | Timeout-tagged read batches delivered (#26); surrogate open path hardened; timestamped, change-only child log; ID-0 presence flags | `37d770d` |
| 2026-10-20 | Link health (#11): growing hard-failure backoff, canonical error code, probe writes in both request modes, spec-mode fake; bridge `linkState` and value freshness; LiveView banner, Home and Settings state, "—" for never-arrived signals | `5924214`, `b100029`, `a7975fe` |
| 2026-10-20 | Scripted mock drive cycle: warm-up, 2nd- and 3rd-gear limiter pulls, quarter-mile run (#9 closed) | `c0b102d` |
| 2026-10-20 | CI on all five targets: Linux full build; Android arm64 and iOS device cross-builds; iOS Simulator launch smoke with screenshots, on pull requests and on request (#3 closed; #30 opened and resolved the same day) | `4dd15ca`, `a430af7`, `923f479`, `f282275`, `4be8ba5`, `a0f5c0f` |
| 2026-10-20 | Code review 2026-10-20 fixes: vendor message counts ignored under hard errors (finding 1), surrogate open diagnostics name the stalled stage (finding 3), `?poll=` outranks the profile's rotation (finding 12), the Log page's ID column binds the real `arbId` role (finding 13) | `6420f09`, `5ec331c`, `90020bd`, `1674f5c` |
| 2026-10-20 | Code review 2026-10-20 follow-up: stale dimming driven by the model's `ageMs` (finding 14); one health machine for reads and writes, with `QT_ERR_REQUEST_FAILED` latched by failed writes, hard read failures first, unmatched frames read as quiet, one quiet debounce that clears the backoff, and backoff growth pinned (findings 4–7, 15, 17) | `7e9bd5e`, `946bdf5` |
| 2026-10-20 | One timestamped session log for the surrogate's client and child (#23 closed); a per-spawn log for a concurrent session (finding 9); `OP_OPEN` tells an exiting child from a blocked vendor; merged via PR #11 | `995519b` |
| 2026-10-20 | UI for `QT_ERR_REQUEST_FAILED` (a `requests` link state); link-state color and label centralized in `Theme`; every test that arms a fake knob clears all of them | `da3aa57`, `3c28b63`, `25d0a6f` |
| 2026-10-20 | Dashboard: analog, ring, digital and bar gauges with shift lights and peak hold, fed by name through `SignalHub`; merged via PR #12 | `40e51ce` |
| 2026-10-21 | Present-tense comment sweep; docs/architecture.md | `4fd6633` |
| 2026-10-21 | `obd2` gains MAF (PID 0x10) and a fourth temperature, first as PID 0x5F, then corrected to engine oil temperature (0x5C); the mock set gains MAF and oil, and its frames become 12-byte CAN FD | `bc1ee8c`, `9b71174` |
| 2026-10-21 | Live: the list scrolls, trace colours key to model rows, and each row's swatch is the legend; LogPage role names pinned | `e119656`, `10bfea6` |
| 2026-10-21 | Windows Qt install: aqtinstall pinned to a master commit, 7-Zip extraction, install root created first (#1 closed) | `51f1b0d`, `94e16a1` |
| 2026-10-21 | Oldest-first log follows the tail; drop reports move to the coalesce tick (#5 and #7 closed) | `523cf28` |
| 2026-10-21 | Dashboard header shows link state through `Theme`; tach red zone from 6800 rpm; chart series palette in `Theme` | `6807e93` |
| 2026-10-21 | The stash conflict committed in `6807e93` resolved (every app build had failed); series palette reordered so red comes last | `55373f4`, `105d3cd` |
| 2026-10-21 | The failure backoff clamped at its 5 s cap (#35 closed) | `d1c20c0` |
| 2026-10-21 | Opt-in per-message payload logging in the surrogate session log | `ac6452d` |
| 2026-10-21 | `?poll=0x00` reads the supported-PIDs bitmap; 29-bit channels connect 29-bit (#34 closed); a wedged surrogate child fails fast | `9c3ef16` |
| 2026-10-21 | Sessions start on the GUI thread, so closing mid-connect leaves nothing running (#32 closed); unsubscribed rows keep updating (#31 closed) | `4a86231` |
| 2026-10-21 | UI/UX review pass: Settings is the only connect flow; Theme palette, mono family and page margin; chart value labels, seconds axis and flat-trace lanes; Log `s.mmm` times, Pause and order toggle; display names, units and decimals from core; merged via PR #16 | `8a5e2b5` |
| 2026-10-21 | `scripts/verify.sh`: both trees, the suite and the cross-arch subset in four lines; a `smoke_only` manual CI run | `3a187c4`, `90e57c6` |
| 2026-10-21 | Request engine design ([ADR-0015](adr/0015-request-engine.md), Proposed) | `ea6e4c9` |
| 2026-10-21 | Disconnect tears down off the GUI thread, with "Disconnecting…" on every connection surface (#33 closed) | `ebfa918` |
| 2026-10-21 | Hygiene pass: a private surrogate pipe passing one inherited handle; UTF-8 paths, UTF-16 at every Windows call; a validator that accepts exactly what the decoder decodes; registry validation and unregister; one URI query grammar; warnings on for every target; prefixed status and ID names; the `Bridge` QML singleton | `cd7ef50` |
| 2026-10-22 | The core's C ABI takes the `ecuio_` prefix; the app keeps the product's name ([ADR-0001](adr/0001-qt-free-core-c-abi.md), appendix of 2026-10-22) | `f07c013` |
| 2026-10-22 | Signal definition layout ([ADR-0016](adr/0016-signal-definition-layout.md)): value kinds and explicit byte order, `double` decode with 16-bit lengths, 3-byte mux keys, a set layout version, zero-checked reserved room, and validation that names the failing element; one Mode-01 key test for enrolment and request routing | `0ae8024`, `6337b6f`, `d809cb7`, `5735dd6` |
| 2026-10-22 | A surrogate built from other shared headers is refused at `OP_OPEN`: a configure-time fingerprint of the two headers (#19 closed) | `c029caa` |
| 2026-10-22 | Vehicle-session prep: each spawn keeps the session log it replaces (the newest 50); the Log table sorts by arrival and starts empty on connect; activity-log lines carry times | `6f84571`, `839b176` |
| 2026-10-22 | Session recording ([ADR-0017](adr/0017-session-recording.md)): every frame, decoded value and activity line per connect; `frames.csv` is the replay format | `f590613` |
| 2026-10-22 | Quiet-bus BENCH run of the vehicle session's URIs with recording on: requests failing and quiet on the VX; its timestamps are microseconds and its empty read is `ERR_BUFFER_EMPTY` ([session sheet](bench-sessions/2026-10-22.md)) | build `0a8beae` |
| 2026-10-22 | First read-only vehicle session: the engine ECU answers the `obd2` scheduler, `?poll=` and the PID bitmaps through the VX; values decode and track the cluster, meeting v0.3.0's tag conditions ([session sheet](bench-sessions/2026-10-22-vehicle.md)) | `9a2c2ee` |
| 2026-10-22 | The Dashboard fits a short, wide window (a wide arrangement), Settings scrolls, and the whole UI zooms (Ctrl + / Ctrl − / Ctrl 0), kept between runs | `0a94049` |
| 2026-10-22 | v0.3.0 released: the version string reads 0.3.0 | tag `v0.3.0` |
| 2026-10-22 | Request-engine groundwork: link health runs through both backends, the fake's control page reaching the surrogate child mid-session | `5961027` |
| 2026-10-22 | Request-engine groundwork: the fake models the OBDX Pro VX (`ECUIO_FAKE_VX`): held reads, `ERR_BUFFER_EMPTY`, TX indications, 4 ms replies, the no-ACK bus | `81798f3` |
| 2026-10-22 | Request-engine groundwork: indications no longer end a read on either backend, and `START_OF_MESSAGE` is skipped (Known Issue #28 closed) | `1f2570b` |
| 2026-10-22 | libFuzzer harnesses for the signal decoder and validators (`fuzz/`), built with MSVC locally or Clang in a CI job that runs each for 60 s | `14a43be` |
| 2026-10-22 | A failed connect says why: four status codes and `ecuio_last_error_detail`, with the vendor's `PassThruGetLastError` text on both backends ([ADR-0018](adr/0018-connect-failure-reporting.md)) | `65bae6d` |
| 2026-10-22 | A session says what it runs on: `ecuio_session_get_info` and the activity log's "Session:" line, with the adapter's battery and versions; the ioctl relay takes a capacity on both backends ([ADR-0019](adr/0019-session-info.md)) | `7263224` |
| 2026-10-22 | J2534 frames carry session time: the host's at the first frame, then the adapter's deltas across its 32-bit wrap (#12 closed) | — |

## Lessons learned

**2026-10-04 — callback API round:** dropped never-implemented registration
handle (pair-identity instead), duplicate registration idempotent-success,
tests assert actual behavior, snapshot-dispatch mandated so callback
reentry can't deadlock. MSVC caught an implicit int→enum conversion
(C2664) — three-compiler matrix earns its keep.

**2026-10-05 — bridge + log round:** (1) captureless lambdas unsafe as
pair-identity C callbacks needing self-reference — named free functions
only. (2) Thread-identity tests must exercise real execution paths
(session_start), not manual dispatch. (3) Assertions encode contracts,
not timing coincidences. Q_PROPERTY NOTIFY is a promise; skipping emit
leaves QML serving stale values. Windows DLL deployment for test targets
via `$<TARGET_RUNTIME_DLLS>` copy, not PATH injection (semicolon fragility).

**2026-10-06 — signals round (Phase 1):** (1) error-code precedence is a
contract — type validation before bit-parameter validation. (2)
zero-initialized PODs mask string-termination test bugs — corrupt the
ENTIRE buffer. Also migrated all suites to `gtest_discover_tests()`
(per-case granularity, free empty-suite detection); DISCOVERY runs the
test binary at BUILD time on Windows, so runtime DLL copying must be
ordered before discovery (we hit exit 0xc0000135 until reordered).

**2026-10-07 — emission + coalescing round (Phases 1b/2):** three lessons.
(1) Designated initializers are C++20 — MSVC accepts permissively, GCC on
Ubuntu rejects; C++17 code needs plain constructor helpers. (2)
`strncpy_s`/`_TRUNCATE` are MSVC-only Annex K — absent from glibc —
"fix a warning" edits can compile locally and fail only CI; direction of
travel matters (MSVC-green ≠ portable). Use a small bounded-copy helper.
(3) **Cumulative count is not backpressure**: gating delivery on lifetime
frame count froze the stream permanently at 10k while the model's ring
eviction sat unused. Bound in-flight queue depth, not totals; pair drop
statistics with liveness tests. Also: measuring before optimizing — the
guiNotifyCount instrumentation locked the 20× coalescing-reduction claim
with a baseline number. New-QML-file linker failures (QmlCacheGeneratedCode
unresolved externals) mean stale QML cache — clean reconfigure fixes.

**2026-10-10 — strip chart round (Phase 2 close):** (1) The QML paint
code indexed the LAST ELEMENT of a flattened `[ts, val, ...]` array as
the newest timestamp — it's the newest VALUE. Symptom was deliciously
diagnostic: chart fine for ~1.5 s, then stretched/clipped, "recovering"
exactly during RPM rev blips. **47 green GTests were structurally blind to
it** — the bug lived in QML `onPaint` arithmetic, outside every C++
test's reach — the manual smoke tier caught it, which is why that tier
exists. (2) Declared-but-never-emitted signals are latent debt. (3)
Pull-API beats data-roles for Canvas consumers. (4) Timestamp injection
seams make ring tests exact arithmetic instead of timing tolerance.

**2026-10-13 — J2534 round (Phase 3, HARNESS tier):** seven lessons,
several of them paid for in build-break debugging. (1) **An
anonymous-namespace definition does not complete a global forward
declaration** — C2027 trap; everything the ABI references by name lives
at file scope, ordered before first use. (2) **Spec-shaped signatures
are load-bearing even against your own fake.** (3) **Calling-convention
mismatches are silent on x64** — function-pointer typedefs must match the
artifact's actual declaration. (4) **CMake literal `$(Configuration)` vs
genex `$<CONFIG>`** — doubled Release/Release output dirs; use genex for
per-config paths. (5) **Identical COMDAT folding in dumpbin** is MSVC
merging byte-identical bodies, not corruption. (6) **Baked absolute
paths via `$<TARGET_FILE:...>` beat cwd/search-order schemes for test
DLLs.** (7) **Exact-count assertions against a deterministic fake are
the harness version of range assertions against physics.**

**2026-10-14 — J2534 query-param round (Commit D):** (1) **Write
failure-path assertions against the actual failure path, not the intended
one** — derive each expectation from which layer should reject the input.
(2) **Paired compiler diagnostics fingerprint a lost middle argument**
(code-167 + code-165 on the same token = argument shift). (3) **Existing
green tests can be blind to a regression a new feature introduces in
shared plumbing** — pin the contract you changed, at the point it
changed (`DefaultConnectUsesIso15765At500k`).

**2026-10-14 — device picker + real-vendor discovery round (Commit E):**
(1) **The registry spec describes an ideal world; installers live in the
real one** — dual-root sweep. (2) **Green tests can be vacuously green
against machine-dependent data.** (3) **A picker's first real-world duty
is diagnosing its own environment.** (4) **Unverified war stories rot.**

**2026-10-14 — F-series (Frame ABI + Lifecycle + Gate):** ABI-breaking frame
redesign, close/destroy split, message-ID decode gate, strict-C ABI
verification. Test count rises 64→70. Lessons: (1) ABI breaks require
version-string sync. (2) Poll-until-condition waits eliminate wall-clock
dependence. (3) Shared constants beat comments.

**2026-10-14 — surrogate host round (Phase 3 Part 2):** five lessons,
every one paid for in a hang, a ghost theory, or a red test. (1) **A
synchronous pipe handle silently neuters overlapped I/O:** `ConnectNamedPipe`
with an OVERLAPPED struct on a handle created WITHOUT
`FILE_FLAG_OVERLAPPED` *ignores the struct and blocks forever* — the
carefully-built wait/cancel/fail-fast code downstream of it was dead code,
never reached. Symptom: same-arch tests passed (their children connect
instantly) while the dead-DLL test hung forever. The fix is the flag; the
discipline is knowing that every subsequent ReadFile/WriteFile then needs
its own OVERLAPPED — pseudo-sync wrappers with timeouts came free with
the correct fix and closed the wedged-child risk too. (2) **A wait
predicate that doesn't DO the thing can never fire:** the test waited on
`count >= 3` while the drain that increments count ran only *after* the
wait — every run burned the full timeout and then failed. When the
condition is produced by polling, the predicate must drain inside the
loop. (3) **x86 `__stdcall` name decoration breaks `GetProcAddress` by
spec name:** MSVC exports `_PassThruOpen@8` on x86; x64 has one calling
convention so names go undecorated — the same-arch suite passed for
weeks while the cross-arch run failed on export resolution (child exit
code 5). Fix: a linker `.def` file with bare EXPORTS entries — the exact
mechanism real J2534-1 vendors use to publish undecorated names. The
spec mandates undecorated exports precisely because of this trap.
(4) **Instrument BEFORE theorizing:** the hang took three blind fix
attempts (path dialect, missing x86 CRT — both disproven by one
`ls /c/Windows/SysWOW64` and one bare-exe run) before distinct child
exit codes + client-side stage logging turned the next failure into a
one-command diagnosis. Cheap exit codes on every early-return in a child
process are permanent diagnosability, not scaffolding. (5) **MSYS shells
don't translate environment-variable values** — only command arguments
get path mangling; `/e/...` in an env var reaches CreateProcessA
verbatim and fails. Windows-style paths (forward slashes are fine) in
env overrides. Honorable mention: the two failed theories were each
disproved by a single command because the evidence was checkable — the
methodology is the lesson, not the guesses.

**2026-10-14 — routing round (Phase 3 Part 3):** (1) **Route at the
lowest complete boundary:** putting the in-proc/surrogate switch inside
`qttune_j2534_open` and forwarding one line per vendor-call helper left
`j2534_transport.cpp` byte-identical — the policy layer is
backend-blind, proven by a session-level lifecycle running over the
surrogate with zero transport changes. A routing decision placed higher
forks every future capability; placed at the loader it is inherited for
free by all of them. (2) **A sniffer's failure mode is a routing
decision:** returning "x86" on unreadable PE would have spawned
surrogate processes for garbage paths; returning "unknown → in-proc"
preserves the honest LoadLibraryEx failure. The docstring said one
thing, the code did another — the routing work surfaced the contract
violation that had been hiding in discovery since Part 1. (3) **Test
hooks need to exist at the boundary under test:** cross-arch routing
tests required `qttune_j2534_inject_test_frame` precisely because an
x64 test process cannot LoadLibrary an x86 fake to reach the shared
queue — the inject hook routes like everything else and closes the gap.

**2026-10-15 — ptypes verification round (pre-Part 4):** four lessons.
(1) **No vendor header is the norm, not a red flag** — the SAE
standard is the contract, and the canonical definition is recoverable
from multiple independent public references (FreeSSM header, diamondman
v04.04 header, PEAK manual) that AGREE with each other; verify against
those, not training memory. (2) **A shared-header fake makes spec
divergence structurally invisible:** every consumer compiled from
`j2534_ptypes.h` stays mutually consistent while diverging from every
real device — 77 green tests proved NOTHING here, the reference diff
found three dormant bugs. Of the three, the nastiest was ISO15765=0x05
vs 0x06: connecting with raw CAN's ID half-works, and half-works beats
fails for damage potential. (3) **Constants in a shared header are ABI
across separately built binaries:** fixing CAN_29BIT_ID's value while
one tree was stale produced a cross-process value mismatch — caught
only because one test asserted the extended-frame bit (the version-skew
failure). Since then, response-struct size checks fail loudly on
struct-layout skew, but constant-value skew inside same-sized structs
remains silent (Known Issue #19: rebuild both trees together).
(4) **The spec text reorders priorities:** reading v4.04 revealed ISO
15765 receives NOTHING without a flow-control filter — the planned
"polish" item was actually the prerequisite for the first byte of
live data, which reshaped Part 4's scope before any code was written.

**2026-10-15 — WRITE/IOCTL/filter round (Phase 3 Part 4):** five
lessons. (1) **Expand flat wire forms only where pointers are
meaningful:** `SCONFIG_LIST` carries `ConfigPtr` — the surrogate child
materializes the real struct in its own address space, calls the
vendor, and re-flattens the results; pointer-bearing types never cross
the boundary and the in-proc/surrogate signatures stay identical by
construction. The filter triplet got the same treatment for free
because filters are `PASSTHRU_MSG`s, not a bespoke struct — spec-shaped
data wire-translates naturally; bespoke structs don't. (2) **Whitelist
at BOTH ends or the backends disagree:** the first-cut child code
relayed unknown ioctls with NULL payloads "generously" — a vendor
might support them! — which meant the loader seam refused FAST_INIT
while the surrogate would happily pass it to a device. Asymmetric
backend acceptance is a parity bug by definition; the whitelist is now
identical at seam and child, and the parity test drives BOTH.
(3) **Test observability must be architecture-independent:** a
scripted-response fake requires an ARMING hook, and an x64 test
process cannot reach into the x86 child's fake instance to arm one
(the exact gap `OP_INJECT` exists to bridge). The deterministic echo
(byte 0 + 0x40) makes the full request→response loop observable
through the ring from either arch with zero new protocol surface —
design the observable, not the mechanism. (4) **Writing the tests is
the design review:** composing the caller-facing ioctl test exposed a
real overflow the code review missed — the loader handed a small
caller buffer (4-byte VBATT) straight to a client that copies up to
64 wire bytes. The fix (loader-side staging buffer + cap) was forced
by thinking about what a CALLER would look like, not what the plumbing
permitted. Same session: the surrogate's response struct grew a
payload region, and the pipe length checks — designed as optional
insurance in Part 2 — became the load-bearing defense against
cross-tree version skew, catching a stale win32 child loudly where
the 046b320 round had silently corrupted. (5) **Numbered-opcode
enums with reserved gaps invite missing identifiers:** the Part 3
header defined opcodes 1,3,4,5,... reserving 2 for CONNECT — and Part
4's client compile failed on `QTTUNE_SUR_OP_CONNECT` undefined while
the header had compiled "fine" for weeks. The compiler is the net
here and it worked, but the lesson stands: reserve slots by defining
them (as an unused-but-named constant), not by skipping numbers.

**2026-10-15 — transport adoption round (Phase 3 Part 5):** five
lessons, all paid for in red tests. (1) **Assert the failure reason,
not just the failure:** the coupling-rule test passed vacuously — its
URI was rejected by the strict number parser (bare `0C` isn't hex)
before the coupling rule ever saw it. Green-but-wrong-reason is the
same blindness as vacuous-green. (2) **Appended test code outside the
suite's namespace/ifdef produces identifier soup** — every hook the
new tests resolved needed a typedef at file scope first; the compiler
error that looked like "unidentified parameters" was a scoping bug.
(3) **A rejecting fake must DISCARD, not park:** filter gating that
rejected a non-matching frame while leaving it queued froze the stream
at the pre-mismatch count — head-of-line blocking, same failure class
as the Part 2 wait-predicate bug. Device filter hardware throws frames
away; a fake that models the device must too. (4) **Predict the
symptom before the fix:** when six red tests collapsed to three root
causes, the next failure (0x42 vs 0x02) was pre-announced by reading
which layer the fake's device-model echo had moved through — diagnosed
before running. (5) **Dead locals in tests lie by omission** — an
assigned-never-used `const int n` reads like a dependency of the
assertions below it. Delete them; /W4 on the test targets would have
flagged it. Also: fake conventions (+0x40 on byte 0) must be labeled
as such in tests AND docs, or BENCH inherits them as protocol
expectations.

**2026-10-16 — blocked-open round (Issue #22):** four lessons, two
paid for by a real vendor. (1) **Reorder the pipeline so the protocol
names the blocked stage:** the child called `PassThruOpen` before the
pipe connect, so "alive but blocked in vendor open" (adapter absent —
a real OBDX Pro mode) was invisible to the wire and cost the full 8 s
handshake with no diagnosis. Moving the vendor open into the `OP_OPEN`
handler makes pipe-connected prove load+exports and
OP_OPEN-unanswered mean exactly one thing — instrumentation you don't
have to build is diagnostics you always have, and it cost zero
protocol changes. (2) **Environment variables inherit into the child —
that IS a cross-arch test hook:** arming the fake's open-sleep and
shrinking the handshake window via env works from an x64 test against
an x86 child with no protocol surface, where a hook-based arm would
be unreachable (the OP_INJECT gap, solved by not needing it). (3) **A
synchronous vendor call on the GUI thread is a bug regardless of the
vendor's speed:** the picker row clicked straight into `connectSession`;
the fix wasn't making the vendor faster, it was never blocking the UI
thread on DLL code we don't control. (4) **Selection is intent, not
action:** a row click that only fills the URI field can never
accidentally cost 8 seconds. Also observed, the project's first named real-vendor behaviors: the OBDX Pro VX opens with USB power and no vehicle, and the
OBDX Pro VT REFUSES open (status 34) in seconds when its adapter
is absent — the earlier "blocks indefinitely" belief was an
artifact of the old startup order, where block and refuse were
indistinguishable; refuse-at-least-some-vendors is the truth.

**2026-10-16 — IoCtl spelling round (first real vendor load):** three
lessons, one paid for with a confabulation. (1) **`GetProcAddress`
string literals are ABI too.** The ptypes round verified structs and
constants against public references — but export NAMES are string
literals at call sites, outside any shared header, so the verification
rigor never touched them. Our unverified `PassThruIoCtl` literal
diverged from the spec's `PassThruIoctl` (J2534-1_0500 §7.3.19; every
public reference agrees), and the fake exported what the resolver
asked for — 92 green tests certified our own typo until the first
real vendor (OBDX Pro VX) died on it in 16 ms. (2) **Check the
spec before writing the accommodation:** the first fix design had
primary and fallback BACKWARDS — treating the vendor's spelling as
the tolerated divergence. The owner challenged it, one web search
against the spec text reversed the polarity, and the comments now
record real provenance instead of invented hierarchy. A fallback's
order encodes a claim about what's normal; verify the claim. (3)
**Instrument once, harvest repeatedly:** the pipe-first reorder,
honest read-kinds, exit-code grace, and the TEMP child-stderr log
built for the blocked-open mystery each paid off again unprompted —
the export-casing failure named its own cause (exit 5, one line in
the stage log) and the VT refusal arrived as a labeled status code.
Diagnostics that survive their incident are infrastructure.

**2026-10-16 — formal BENCH round (first real adapter):** three
lessons, two paid for during pre-flight. (1) **Echo the env before diagnosing 
the code: both pre-flight red herrings were wrong env values (x64-tree fake; nonexistent C: path), and each masqueraded convincingly as a routing regression — the test was right both times.** The cross-arch routing test with unset `QTTUNE_FAKE_DLL` doesn't skip in practice on a machine with the x64 tree built — it fails into the baked x64 path looking exactly like a routing regression ("expected
surrogate, got inproc"), and it took a deliberately-fresh shell to see it. Absent config should be a visible skip, never a silent substitution. (2) **A copied log has provenance too.** Diagnosing the apparent `QT_ERR_LOAD_FAILED`, the `%TEMP%` child log — written `CREATE_ALWAYS` by a prior run — showed a full healthy lifecycle
for a spawn that had never happened. The copy-immediately rule was written for adapter evidence; it governs debugging evidence identically. (3) **Instrument the op that answers your question.** The session's central question — does the VX accept the 8-byte flow-control shape? — could only be answered by inference from
session-start success, because the surrogate traces startup and OP_OPEN and nothing else (now Known Issue #23). A quiet test result is still a result, but a logged vendor status is a fact; the gap between the two is the gap between "passed" and "passed, and here is the vendor's word for it."

**2026-10-17 — surrogate serialization round (review finding 2):**
four lessons. (1) **A committed fallback test can certify nothing
while green:** `dllexport` on `PassThruIoctl` leaked the spec
spelling into the legacy fixture through the x64 undecorated export,
so the fallback test resolved primary and never exercised the
fallback — the vacuous-green class F8 was filed against, surviving
inside the F8 fix itself. `dumpbin /exports` on the fixture is one
command and total proof of the export surface; a test's green says
nothing the export table doesn't. (2) **Design the observable before
assuming reachability:** the disconnect-count pin as first imagined
dies on process isolation — an x64 test counting its own module
handle counts a different DLL instance than the child's. The
cross-process pin needs a wire query op (the #23 work); landing
`GetDisconnectCount` now parks the observable where its consumer
will meet it instead of building half of one. (3) **Crash becomes
linger is an upgrade, not a compromise:** holding the vendor lock
across teardown converts `FreeLibrary`-under-a-live-call into a
blocked child reaped by the kill-on-close job — the worst case went
from rare-and-fatal to bounded-and-diagnosable, which is the
correct direction for code you can't control. (4) **Suite green
cannot see child stderr:** the single-disconnect fix's direct
evidence lives only in `%TEMP%\qttune-surrogate.log` — OP_DISCONNECT
present, teardown-disconnect absent after clean closes — a manual
check no automated tier can currently make. The check is cheap;
the discipline is remembering to run it.

**2026-10-17 — ADR-0013 slice 1 (profiles):** three lessons. (1) **Presence
and content are separate axes:** the first design registered "raw" as a NULL
signal set and validated profile names with a NULL check — which cannot
distinguish "registered, empty" from "not registered" and would have rejected
the feature's own headline case (`?profile=raw`). The registry lookup returns
a status with an out-param instead; the pin
(ProfileLookupRawReturnsOkWithNullSet) exists so this collapse can't recur.
(2) **Fail-fast placement is testable, not just reviewable:**
UnknownProfileRejectedBeforeDllLoad discriminates QT_ERR_NO_TRANSPORT from
QT_ERR_LOAD_FAILED by pairing a bogus profile with a nonexistent DLL path —
ordering contracts deserve an observable discriminator, not a comment.
(3) **Null isn't an error anymore:** setSignalSet's NULL early-return was
correct when NULL meant "caller bug"; the moment NULL became the raw
profile's legitimate value it became a stale-data bug — a raw session
following a mock session rendered dead mock rows as if live. When a
sentinel becomes valid state, every guard on it is a bug until audited.

**2026-10-19 — channel-decl + surrogate auto-deploy round (slice 3):**
four lessons, two paid for by corrupted pastes. (1) **A cache
variable outlives the script that set it:** the poisoned
`QTTUNE_SURROGATE_SOURCE` value (`;CACHE;PATH;` fragments) survived
the corrected CMakeLists because cache entries persist across
reconfigures — the fix and the symptom coexisted, and every rebuild
re-failed identically. Reading the error message's variable *value*
(not just its name) named the failure instantly; `cmake -U VAR` is
the surgical purge and should be muscle memory after any
cache-variable debugging. (2) **Multi-line `set(...)` cache
declarations are paste-fragile:** a line-wrapped docstring lost its
quotes and the `CACHE`/`PATH` keywords became list *values*. Cache
declarations now use short single-line strings — the fragile syntax
was removed, not the pastes promised better. (3) **Paste round-trip
is a lossy channel, in both directions:** placeholder text and a
scrambled quote each shipped to disk on the same file across
sessions; the verify-before-writing rule applies to paste-*out* as
well as paste-in — after the second corruption, the fix was
requesting the file back and diffing before proposing a third edit.
(4) **A fail-late coupling check spends hardware first:** slice 2's
scheduler-needs-filter rule fired at `j2534_start`, after
`PassThruOpen` had already touched the device; deriving channels at
attach moved the check ahead of the open. The slice-2 ORDERING NOTE
predicted exactly this — the obligation was repaid by the slice
that retired it.

**2026-10-19 — single-threaded surrogate round (ADR-0014):** four
lessons. (1) **A vendor status is data, not control flow:** the first
cut of the new child exited its loop on any non-success status. The
suite stayed green because the fake answers an empty read with
`STATUS_NOERROR` and `sur_close` returns `QT_OK` whether or not the
child is alive, so a conforming vendor's first quiet-bus
`ERR_TIMEOUT` would have ended every session. Code review caught it,
not the tests. (2) **A rewrite that deletes diagnostics undoes lessons
already paid for:** a misspelled export (`PassThruStopFilter`) failed
eight surrogate tests at exit 5. The child log named the stage
("mandatory export missing") but no longer named the export, because
the same rewrite dropped the per-export list added after the
2026-10-16 dumpbin session (restored the same day). Keep the
diagnostics when restructuring the code around them. (3) **Know the generator before reasoning about
POST_BUILD:** a review claimed the deployed surrogate copy goes stale
when only `surrogate_main.cpp` changes. That holds for Ninja and
Makefiles, but the Visual Studio generator runs POST_BUILD steps on
every build. Building the target twice settled it in a minute.
(4) **A fake that shares the implementer's model certifies it:** the
fake's `ReadMsgs` ignores `NumMsgs` and the timeout and never returns
data with a non-success status, so the batched read's status handling
(#26) is invisible to the suite. The bug was fixed the same day
(`37d770d`) by delivering frames before judging the status. On
2026-10-20 a spec-conformant fake mode (`QTTUNE_FAKE_SPEC_READ_SEMANTICS`)
and `SpecTimeoutReadDeliversPartialBatch` pinned the fix. This is the
same class as the
2026-10-18 framing bug (N1). When code depends on spec semantics, move
the fake toward the spec first.

**2026-10-20 — CI matrix round (five targets):** four lessons. (1)
**Kit identifiers drift between Qt releases:** `gcc_64` stopped
resolving for 6.12.0 because the Linux desktop arch is `linux_gcc_64`
since 6.7, and Android kits moved under the `all_os` host. An arch
string that worked for an older Qt is no evidence for a newer one.
(2) **Build-time test discovery makes cross builds build-only:**
`gtest_discover_tests` runs each test binary during the build (the
2026-10-06 lesson), so with tests on, a target the runner can't
execute fails at build time. The mobile jobs set `BUILD_TESTING=OFF`,
which means a green Android or iOS device job proves the code compiles
and packages, and nothing more. (3) **Check what a prebuilt SDK
actually ships before picking a runner:** Qt's iOS simulator
libraries are x86_64 only. On the arm64 `macos-latest` runner the
simulator would need Rosetta and a separate universal iOS platform
download, so the smoke job runs on `macos-15-intel`. That swaps one
cost for a dependency with an announced end date (#29). (4) **A
liveness check is not a rendering check:** the first simulator run
went green with a blank screenshot. "Still running after 15 s" can't
tell an app that draws from one whose window never drew, and the
artifact's 6.8 KB size said as much before anyone opened it. The job
was made on-request only rather than left as a green that claims
more than it proves. The next run carried a home-screen control shot,
two timed shots and `QSG_INFO`. It drew on Metal by 15 s, so the
runner's GPU was cleared. Two layout bugs showed up that no desktop
smoke had caught: the off-centre Home status card, and the compact
tab bar's "Setup" for Settings. The blank first frame is put down,
by inference rather than proof, to launching the app on a simulator
still settling after a cold boot. The boot now overlaps the build.
Once there was a real picture, the screenshot proved its worth.

**2026-10-20 — code-review fix round (findings 1, 3, 12):** two
lessons. (1) **An error knob that tidies its outputs certifies the
tidy case only:** `QTTUNE_FAKE_READ_STATUS` zeroed the count on every
forced error, so both backends' trust in the vendor's count under a
hard error was invisible. The evidence came from a change-only log
line kept for another reason: the VX's "status 7, written 1" on a
failed write. The fix added the sloppy-vendor knob first
(`QTTUNE_FAKE_UNTOUCHED_COUNT_STATUS`), went red on each backend, then
put one trust rule in the shared header. It is the 2026-10-19 lesson
(4) again, now for error paths: model what a vendor is allowed to do,
not what you'd like it to do. (2) **Put a check where its inputs
exist:** the poll-needs-a-filter rule lived in the URI parser, which
can't see the profile's declared channels. So under `obd2` it refused
`?poll=` without `?filter=`, and with `?filter=` added, the scheduler
(built whatever the URI said) silently overrode `?poll=` anyway.
Deciding the mode and the channel rule at attach, where URI and
profile meet, fixed both, and the check still fires before any DLL
loads.

**2026-10-20 — session-log round (`995519b`):** one lesson. **A
liveness snapshot taken right after a pipe breaks races the process
exit.** The finding-3 fix (`5ec331c`) told a blocked vendor from a
dead child by asking whether the child was still alive once
`OP_OPEN` failed. A child that fails to load the DLL closes the pipe
and exits with code 4 or 5, but the check could run between the two,
so a missing DLL was logged as a blocked vendor. The fix classifies
by why the transaction failed instead: only a timeout means
"blocked", and a broken pipe means "exiting", so the client waits
for the exit code and decodes it. When two events always happen in
order, judge by the first one, not by a snapshot taken between them.

**2026-10-21 — comment-debt sweep:** two lessons. (1) **A comment that
tells history goes stale the moment the history moves on.** About 150
comments carried change narrative ("used to", "formerly #23", "the old
bug"), closed-issue numbers, or code-review labels ("finding 12",
"W6"). The labels pointed into `Testing/`, which is git-ignored, so
for anyone else they were dead references. A few had also become
wrong. One test still described the retired shared-memory ring, the
protocol header still said read frames were delivered "whatever the
status", and a fake comment said a knob had no tests after three used
it. Every one was rewritten as the present-tense reason the code is
the way it is; the narrative stays in commit messages and here. (2)
**Counts in comments drift.** "The 8-PID set" was wrong as soon as
MAF and TFT were added to it. State the rule ("N PIDs at 100 ms
each"), not today's number.

**2026-10-21 — Windows Qt install (Known Issue #1 closed):** two
lessons. (1) **Before calling an archive error a flaky download,
check whether the tool verifies the download.** `win-build`'s one
`Bad7zFile` looked like network noise, and the issue's plan was a
retry-friendly pin. But aqtinstall checks every archive's SHA-256
against Qt's published hash, re-downloading on a mismatch, before it
extracts, so the bytes py7zr rejected were known good and the fault
was py7zr's. That pointed at the real fix: extract with the runner's
7-Zip (`--external 7z`). (2) **A wrapper's dependency pin can be
outvoted by the tool it wraps.** `install-qt-action` pins py7zr 1.1.0
against this very bug, then installs aqtinstall separately so that
aqtinstall "can override py7zr if required". aqtinstall master
requires py7zr 1.1.3 or later, so that pin, and the issue's fallback
of pinning py7zr 1.1.0, could never hold for this job. Unpinned
master also meant each run took whatever aqtinstall and py7zr were
newest; the job now pins aqtinstall to a commit.

**2026-10-21 — oil temperature and a committed merge conflict:** two
lessons. (1) **A fake written from the same memory as the def agrees
with it.** `bc1ee8c` added "transmission fluid temperature" as PID
0x5F. SAE J1979 defines 0x5F as the bit-encoded emission-requirements
byte, and transmission temperature isn't a standard Service 01 PID at
all; automakers expose it through Mode 22. The def, the fake's canned
reply and the decode test all came from the same assumption, so the
suite was green, and a vehicle that supports 0x5F would have shown its
enum byte as a temperature. `9b71174` replaced it with engine oil
temperature (0x5C, the same A − 40 shape). It's the J2534 lesson again,
for PIDs: check every def against a public J1979 table, never memory.
(2) **A red build can look partial.** `6807e93` committed a
`git stash pop` conflict in `LiveViewPage.qml`: three marker blocks,
plus stash-side lines outside them that referred to the other side (a
mistyped property, a second `ScrollBar.vertical`, a variable declared
only inside a marker block). qmlcachegen rejects the file, so all five
app builds failed while the two Qt-free jobs passed. The smoke result
in the commit message couldn't have come from the committed tree. Run
`git diff --cached --check` before committing after a merge or stash
pop, and smoke what is being committed. The conflict's two sides were
two answers to the same Live problem; the resolution keeps the
row-keyed colours with `Theme`'s palette and dashes rows past it.

**2026-10-21 — hygiene pass (review items H1–H8):** four lessons.
(1) **An ASCII path can't catch an ANSI bug.** Every `*A` call works
for an ASCII path, so the encoding bug survived a suite whose paths
were all ASCII. A non-ASCII test has to use characters outside the
machine's code page: 1252 has `ü`, so a `ü` path proves nothing there.
The new tests use Greek and CJK, and the in-proc one was run once with
`LoadLibraryExA` swapped back in to see it fail. (2) **Accepted lint
noise hides the signal.** The bridge as a context property named like
the module made 103 `[unqualified]` warnings "expected", so nobody read
them. Registered as the `Bridge` singleton, qmllint checks every
binding, and 7 warnings are left to read. (3) **Flip a predicate's
polarity under a new name.** `qttune_def_matches_frame` returned 0 on
a match. Inverting it in place would have compiled in every caller and
quietly reversed each one's check; `qttune_def_applies_to_frame` makes
an old caller fail to link instead. (4) **A cast outside an enum's range is
undefined in C++**, and the sanitizer job would flag it: the
validator's type test stays inside the enum's range rather than
probing with −1.

**2026-10-22 — signal definition layout
([ADR-0016](adr/0016-signal-definition-layout.md)):** two lessons.
(1) **One rule, one predicate.** "This def is a Mode-01 PID" was
written twice. The scheduler took a 2-byte key at offset 0 without
checking its `0x41`, and request routing (`rx_for_pid`) took any
muxed def whose key ended in the PID. Nothing failed while every
muxed def was Mode-01, and the first 3-byte `62 <DID>` def, on a
second ECU and listed first, would have pulled a PID's requests to
that ECU. `ecuio_mode01_pid` answers for both now, and the routing
test was run against the old routing first, where it failed with the
requests going to 0x7E1. (2) **A version nobody reads isn't a
version.** The set's `version` was set to 1 or 2 but never checked,
and neither were the def's reserved bytes, so neither could carry a
later change: an old set could hold anything there. `layout_version`
is the set's first member and is checked before anything else, with
3 chosen so neither old value passes, and validation refuses nonzero
reserved bytes, so that room stays usable.

**2026-10-22 — vehicle-session prep
([ADR-0017](adr/0017-session-recording.md)):** two lessons.
(1) **Walk the evidence before the scarce event.** Reading the vehicle
run sheet step by step, asking where each piece of evidence would come
from, found three gaps no test covered. Every connect rewrote the
session log the sheet depended on, across about eight connects. The
Log table sorted on the vendor's timestamp, whose unit nobody knew, so
a new phase's rows could sort below the last phase's. And the sheet
timed disconnects from activity-log lines that carried no time. Each
cost minutes at the desk; at the car each would have cost evidence
that takes a second visit to recover. (2) **Hash what must match;
don't number it.** The two build trees needed to agree on their
shared headers. A hand-kept protocol version works only if every
header edit remembers to bump it. A hash of the headers' bytes changes
by itself, and its one cost, that a comment edit also forces both
trees to be rebuilt, is a rule the build already had.

**2026-10-22 — quiet-bus BENCH run with frame logging
([session sheet](bench-sessions/2026-10-22.md)):** **a count the code
computed isn't what the vendor returned.** On 2026-10-20 the
surrogate's read summary said "status 0, frames 0", and the session
note took it as the VX's empty-read status, a "third flavor" that
STATUS, testing.md and j2534.md then repeated for two days. The count
was taken after TX indications were dropped. With each message logged
before the filter, every one of those reads held an indication, and
the VX's truly empty read is `ERR_BUFFER_EMPTY`. The same change-only
summary hid two successful writes, so "writes fail after roughly
eight cycles" was really three writes, then one blocked for half a
second. Characterize a vendor from its raw output; a summary shows
only what the code already decided to keep.

**2026-10-22 — first vehicle session
([session sheet](bench-sessions/2026-10-22-vehicle.md)):** **a bench
with nothing else on the bus predicts the adapter, not the car.** The
morning's bench run showed what the VX does when nothing acknowledges
its frames, and the run sheet carried that over as the ignition-off
expectation: three writes, then "Requests failing". On the car, the
bus acknowledged every frame with the key off, so writes kept
succeeding and the link went quiet, as designed. The difference
surfaced only because the notes recorded what the screen said beside
the ticked box. An expectation taken from a bench needs the condition it
rests on written beside it ("with nothing on the bus to
acknowledge"), so a different result reads as information, not as a
failure or a pass.

**2026-10-22 — request-engine groundwork:** **a fake that never sends
what the device sends can't test the code that handles it.** Known
Issue #28, an in-proc TX indication ending the drain, was found by
reading the loader and stayed open with no test: the fake queued no
indications, so nothing could fail. Once the fake was modelled on the
VX's logged output, with an indication after every write, a test
written against the old loader failed at once: in-proc, all ten
replies landed after the next request. A second test showed what the
unhandled `START_OF_MESSAGE` indication, listed in the same issue,
actually did: it reached the session as an empty frame on both
backends. A fake earns its place
by reproducing a device's recorded behaviour; one written from the
spec alone tests the code against the author's assumptions.
