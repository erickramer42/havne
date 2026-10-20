# ADR-0013: Protocol profiles — selection by URI, capability by probe

- **Status:** Accepted.
- **Date:** 2026-10-17
- **Supersedes:** none. Extends [ADR-0012](0012-j2534-transport-adoption.md) (moves the request loop out of the transport) and begins closing Known Issues #15/#21 in STATUS.md.

## Context

J2534 transport adoption (ADR-0012) delivered caller-chosen IDs and a transport-driven Mode-01 poll loop. But the decode layer still binds to the mock signal set only: the bridge hard-wires `qttune_mock_signal_set()` in `connectSession` and `handleFrameInternal` (Known Issue #21), so J2534 frames reach the Log page and decode nowhere. Three forces shape the fix:

- **Multiple application layers exist ahead.** OBD-II Mode-01 is the universal baseline; manufacturer UDS (Mazda, Honda) provides the enhanced data the project ultimately wants; an unknown vehicle must still connect and log usefully. These differ in request shape and byte decoding, not in transport.
- **The manufacturer-plugin milestone (v0.5.0) needs a seam to plug into.** Whatever interprets responses must be implementable by a small, safe plugin that cannot deadlock the GUI thread or violate teardown ordering.
- **Vehicles do not self-describe at the level that matters.** OBD-II describes its supported PIDs (PID 0x00 bitmask chain); UDS DIDs and multi-ECU mappings are not discoverable. Any selection mechanism that pretends otherwise will have an ambiguous-failure state machine on top of the timeout/error indistinguishability (Known Issue #11).

Additionally, the request loop's placement is wrong: it lives in the J2534 transport hardcoding Mode-01 shape, so a second request pattern means a second transport fork.

## Decision

1. **A profile is a core-resident object that owns three concerns:** what to request, how to correlate and interpret responses, and which signal set describes the vehicle. The transport retains only byte movement; the bridge reads the *active* set over the ABI instead of choosing one.

2. **Profile selection is static, declared in the URI.**
   - `mock://` → profile `mock` (the existing mock signal set).
   - `j2534://` → profile `obd2` by default.
   - `?profile=<name>` overrides the default. `?profile=raw` selects the null profile (no requests, no signal set; Log page only) and is today's behavior promoted to a named thing.
   - Selection rules join the existing strict grammar: lowercase, unknown names rejected at `qttune_session_create` before any DLL loads (`QT_ERR_NO_TRANSPORT`), last occurrence wins. The URI must fully determine session behavior — every bench sheet records a URI, and it must be complete provenance.
   - Profiles are string-keyed in a registry, not an ABI enum, so v0.5.0 out-of-tree manufacturer plugins register names without ABI churn.

3. **Probing is a phase inside a profile, never a selection mechanism.** The `obd2` profile gains (in its v2) a post-connect probe: the PID 0x00→0x20→0x40… bitmask chain, driven through the profile's own request/response callbacks. Probe failure degrades to the static signal set; it never fails the session. Selection is never inferred from what the vehicle says, because the things this project ultimately wants to select (Mazda/Honda UDS) cannot be announced. **What you connect to is a decision; what it supports is a discovery.**

4. **Multi-responder is a first-class part of the contract.** A profile declares a list of channels:

   ```c
   /* sketch of the registration-facing shape; final types live in core headers */
   typedef struct {
       unsigned long rx_id;      /* flow-control filter target */
       unsigned long tx_id;      /* request destination */
   } qttune_channel_decl;
   ```

   `?filter=`/`?txid=` remain the user-facing shorthand for the common single-channel case (ADR-0012 unchanged); internally the session registers one filter per declared channel. `obd2` v1 declares exactly one channel (7E0/7E8); UDS profiles will declare more, because enhanced data lives on multiple ECUs (a Mazda oil-temp DID may answer from a gearbox or body module, not 7E8).

5. **Core drives the loop; plugins never see threads, transport, or filters.** The profile is a POD table of callbacks with an opaque context, called only from the session worker:

   ```c
   typedef struct qttune_profile_ops {
       const char* name;                            /* registry key */

       /* worker-thread only; never the GUI thread */
       int  (*next_request)(void* ctx, unsigned char* buf, int cap);
            /* >0 bytes to send now; 0 = idle this cycle; -1 = fatal */
       void (*accept_response)(void* ctx, const qttune_frame_t* frame);
            /* correlation is the profile's own state machine */
       const qttune_signal_set_t* (*signal_set)(void* ctx);
            /* re-query after accept_response — probe results reshape it */
   } qttune_profile_ops;
   ```

   Ownership division: core owns threading, filter registration from the channel list, teardown ordering, and transport routing. A buggy profile can mishandle only its own state. This table is, deliberately, the seed of the v0.5.0 plugin API — decide it consciously now so it does not become an accidental ABI later.

6. **Profiles ship in this order:**
   - `raw` (immediate; escape hatch for unknown vehicles)
   - `obd2` v1 (static starter set: RPM, speed, coolant, intake temp, load, throttle; multi-PID round-robin scheduler replacing the single-PID loop)
   - `obd2` v2 (bitmap probe phase)
   - `uds-generic` (Mode-22 DID reads, user-entered DIDs — the Mazda/Honda research vehicle)
   - `mazda-uds`, `honda-uds` (curated, post-research, v0.5.0 plugin territory)

## Alternatives considered

- **Auto-detect the profile by probing.** Rejected: only OBD-II can be confirmed by detection, which is already the default; UDS and multi-ECU mappings are not discoverable, so detection buys nothing for the actual targets while creating an ambiguous-silent-failure state (silence is uninterpretable, Known Issue #11) and complicating failure design.
- **Selection in UI settings instead of the URI.** Rejected: splits configuration across two places and breaks URI-as-complete-provenance — every bench sheet and test fixture records a URI that must fully determine behavior.
- **Compile-time profile enum.** Rejected: forecloses out-of-tree manufacturer profiles, which is the stated v0.5.0 direction.
- **Request loop stays in the transport, parameterized per protocol.** Rejected: every new protocol profile would fork the transport; the loop is profile data, and moving it is prerequisite for UDS.
- **Unknown vehicle fails the session.** Rejected: the exploratory-tool worst case; `raw` exists precisely so an unprofiled vehicle always connects and logs.

## Consequences

- The bridge's hardcoded `qttune_mock_signal_set()` disappears; LiveView renders whatever the active set contains, and Known Issue #21 closes for every profiled transport.
- ADR-0012's "one PID per session" ends with the `obd2` scheduler; `?poll=` remains as the single-PID convenience and the probe's primitive.
- Probe quality depends on splitting ReadMsgs timeout from hard error (Known Issue #11) — the probe elevates that issue from nice-to-have to load-bearing.
- The channel-decl list is the one surface a plugin can request filters through; it must be bounded and validated at registration (plugins are eventually native code selected by name from a folder — opt-in, like all tuner software, but the blast radius is the filter table, not the process).
- **The pre-2008 fleet needs transport-protocol work below this ADR.**
  Target-era cars split cleanly: 2008+ is CAN (ISO 15765 / UDS) and
  this architecture serves it directly; 1996–2007 is OBD-II over
  legacy buses — ISO 9141-2/KWP2000 (Mazda NB/late NA, early Mazda
  3/6, pre-2008 Ford/GM), J1850 VPW (GM), J1850 PWM (Ford) — none of
  which this transport connects. Legacy-bus support is its own
  workstream: new `PassThruConnect` protocol support, and the
  K-line init ioctls (currently deliberately refused, Known Issue
  #20) graduate from "fine to skip" to load-bearing, needing the
  dedicated wire forms already prescribed. Pre-1996 OBD-I and
  proprietary connectors (early NA Miata) are out of scope for v0.x
  entirely. The profile design must not assume CAN or ISO 15765
  anywhere; `raw` stays valid on every protocol, which is its job.
- **Connect-time protocol auto-detection is a possible future
  transport feature, not a profile-selection mechanism.** Real scan
  tools auto-detect which of the five OBD-II flavors a 1996–2007 car
  speaks (`?proto=auto`-style). That detection is transport-layer
  negotiation and is legitimately detectable — unlike profiles,
  which this ADR keeps statically selected (§3). The two kinds of
  probing must never be conflated: the vehicle answers an
  OBD-II-flavor query; it cannot announce which profile to use.
- Everything here remains read-only telemetry. Profiles that write (session control, security access) are v0.4 scope and subject to the regulatory posture in DISCLAIMER.md.

---

## Appendix — slice 2 implementation notes (2026-10-18)

**Status:** implemented; HARNESS-verified
Extends §6's `obd2` v1 bullet with the concrete mechanisms slice 2
chose, and records what was learned during implementation. The
decision text above is unchanged — this appendix is the
implementation record.

> **Superseded details (corrected 2026-10-19, fixes N1 and N2).** On
> ISO 15765 channels the adapter does the ISO-TP framing, so neither
> direction carries a PCI byte:
> - The decoder sees `[0x41][PID][data]`, and the mux key sits at
>   offset 0 (`mux_offset == 0`).
> - Requests are `[4-byte ID][01][PID]` sent with `ISO15765_FRAME_PAD`.
> - The fake answers `[rx ID][41][PID][data]`.
> - Coolant and intake-air temperature are `UINT8` with a −40 offset
>   (J1979: unsigned A − 40).
>
> The slice-2 text below is kept as the record of what slice 2
> shipped. Where it disagrees with this note, the note and the code are
> current.

### Mux-key grammar

OBD-II responses arrive on arbitration ID `0x7E8` with the ISO 15765
address header stripped by the transport worker (`pump_one` in
`j2534_transport.cpp`). The decoder receives a payload beginning with
the single-frame PCI byte, followed by the Mode-01 echo
`0x41 <PID>`, then the data bytes. The same mux fields drive both
sides: the scheduler derives which PIDs to *request* from the signal
definitions' mux descriptors, and the decoder correlates responses by
matching the mux key.

The derivation contract binding definitions to the scheduler: a
definition drives scheduling iff it carries `mux_offset == 1`,
`mux_length == 2`, and `mux_key == 0x4100u | PID`. The PID to
request is the key's LOW byte — the high byte `0x41` is the Mode-01
response echo. Nothing enforces the shape at registration
(`qttune_profile_register` does not run the validator), so
`qttune_sched_from_set` checks it at enrollment.

Non-mux definitions (raw/mock sets) never enroll; the scheduler comes
up empty and the worker keeps its legacy behavior.

### Scheduler constants and cadence

- Fixed capacity: 16 PIDs (`QTTUNE_SCHED_MAX_PIDS`). No heap
  allocation. Owned exclusively by the transport worker thread —
  single-threaded by construction, so no internal synchronization.
- Rotation: round-robin in first-seen set order. `obd2` v1 defines
  eight PIDs, so the cursor wraps every 8 cycles.
- **Cadence:** one request per 100 ms cycle. Eight PIDs rotate
  through the cycle, so each PID refreshes every 8 x 100 ms = 800 ms,
  about 1.25 Hz.

### Worker loop structure (scheduler mode)

Per 100 ms cycle, measured against an absolute deadline from cycle
START:

1. `qttune_sched_next` yields a PID; build the Mode-01 request
   (12 wire bytes: 4-byte tx ID big-endian, SF PCI `02`, `01`, PID,
   zero padding).
2. Write the request. A failed send retries next cycle — the loop's
   job is resilience, and persistent failure presents as a silent
   bus, which is the BENCH diagnostic story rather than a
   transport-layer error to synthesize here.
3. **Drain:** up to `J2534_DRAIN_BUDGET` (10) reads with a short
   20 ms timeout. The first empty read ends the drain. Leftover
   frames persist into the next cycle's drain — no drops, no
   head-of-line blocking.
4. Sleep the cycle remainder against the absolute deadline, so
   accumulated drain cost cannot drift the request cadence.

### Drain budget rationale (Issue #25)

`DRAIN_BUDGET` bounds the reads per cycle: 10 reads over a 100 ms
cycle caps the worker at roughly 100 msg/s even fully saturated,
raising the old poll-mode receive ceiling (about 10/s, Issue #25) by
an order of magnitude. The 20 ms read timeout is deliberately short:
on a real device `ReadMsgs` blocks up to the timeout when the queue
is empty, so draining with the legacy 100 ms timeout would spend the
whole cycle waiting for silence. The first EMPTY read ends the drain —
the budget exists for bursts, not for silence.

**Whether roughly 100 msg/s clears a busy real bus is a BENCH
measurement, not a settled bound.** A VX measurement slot is owed on
the next bench sheet.

### Generalized coupling rule

Scheduled PIDs require an active receive filter (`?filter=`).
Rationale, the same one the legacy `?poll=` rule rests on: per
J2534-1 v4.04 an ISO 15765 channel receives nothing without a
registered filter, so a scheduler would write requests against
guaranteed silence forever. A guaranteed-silent session is a delayed
failure, not a feature — so it fails fast.

Enforcement point: `j2534_start` checks `sched_mode && rx_id == 0`
and returns `QT_ERR_NO_TRANSPORT` BEFORE connecting the channel. It
lives at start rather than in `parse_query` because the trigger is
the profile, not a URI token. The profile-declares-channel
refinement (§4) will let `obd2` supply the 7E0/7E8 defaults itself;
this check then becomes the backstop for a user override that did
not break it.

Failure mode: a refused start leaves the channel closed, so session
teardown has nothing to disconnect — clean failure, no leaked
resources.

### Default flip consequence

Plain `j2534://` URIs (no query) now select the `obd2` profile and
therefore activate the scheduler. Legacy verbatim receive requires
either `?profile=raw` (log-only mode) or `?filter=0x7E8` (accept the
scheduler with an explicit channel). Existing harness tests were
updated with `?profile=raw` opt-outs to pin their legacy semantics;
the flip itself is pinned by the `DefaultProfileIsObd2` and
`BareJ2534UriRequiresFilterUnderObd2Default`(replaced in slice 3 — see below) tests.

### PID table (obd2 v1, set order)

| Index | PID | Name | Bytes | Encoding |
|---|---|---|---|---|
| 0 | 0x04 | Engine Load | 1 | `100/255` |
| 1 | 0x05 | Coolant Temp | 1 | -40, uint8 (int8 in slice 2; corrected by N2) |
| 2 | 0x0C | RPM | 2 | BE, `0.25` |
| 3 | 0x0D | Vehicle Speed | 1 | 1 |
| 4 | 0x0F | Intake Air Temp | 1 | -40, uint8 (int8 in slice 2; corrected by N2) |
| 5 | 0x11 | Throttle Position | 1 | `100/255` |
| 6 | 0x2F | Fuel Level | 1 | `100/255` |
| 7 | 0x42 | Battery Voltage | 2 | BE, `0.001` |

Scale formulas are the standard SAE J1979 Mode-01 conversions. The
set enumerates SUPPORTED PIDs — what is absent is neither requested
nor decoded. **Multi-frame responses are out of scope for v1**:
definitions assume single-frame response shapes. PIDs 0x0C and 0x42
are 2-byte big-endian; all others are single-byte.

### Decoder extensions (signal set v1)

- **Big-endian support:** `QTTUNE_SIGNAL_FLAG_BIG_ENDIAN` (bit 0)
  activates byte-reversed extraction. Non-byte-aligned BE fields are
  rejected at validation time — a configuration error, not a runtime
  condition.
- **Mux dispatch:** `qttune_decode_signal` and
  `qttune_def_matches_frame` gate muxed definitions on a mux-key match
  before extraction. Mux is keyed on `mux_length`: definitions with
  `mux_length == 0` (the mock set) decode unconditionally, unchanged.
- **Header dependency:** `signals.h` includes `core.h` for the complete
  `qttune_frame_t` that `qttune_def_matches_frame` takes; a forward
  declaration would clash with core.h's typedef. This surfaced as a
  latent defect when `obd2_signal_sets.cpp` became the first TU to
  include `signals.h` with no `core.h` in scope.

### Fake vendor enhancements

The fake DLL recognizes Mode-01 request shapes (`[02][01][PID]`) and
responds with proper OBD-II grammar (`[04][41][PID][data]`) carrying
deterministic canned values (RPM raw 0x1F40 gives 2000 RPM at 0.25
scale — the one pinned numeric value). Legacy non-Mode-01 traffic
keeps the echo path. Note: the fake's uniform PCI byte 0x04 is one
byte fatter than a strict ECU would send for single-byte PIDs
(0x03) — decoder-irrelevant, to be normalized when multi-frame work
forces precision.

### Test coverage added

- **UNIT** (headless, all platforms): `test_obd2` — set structure,
  decoder round-trips against wire bytes, and scheduler
  rotation/wrap/dedupe/capacity logic.
- **HARNESS** (Win32, fake DLL): `test_j2534_stream` — scheduler
  rotation order and wrap read from recorded writes, coupling
  rejection at start (both explicit and via the default flip), and
  drain-backlog delivery beyond the per-cycle budget.

Tier discipline: UNIT proves the logic, HARNESS proves the cadence
and integration, BENCH proves real-device throughput. The drain
budget mandates HARNESS minimum — no unit test can verify that a
20 ms timeout plus a 10-read budget actually clears a busy bus within
the cycle.

## Appendix — slice 3 implementation notes (channel declarations) (2026-10-19)

**Status:** implemented; HARNESS-verified
Implements §4 for the `obd2` v1 set. The decision text above is
unchanged — this appendix is the implementation record.

### Shipped ABI shape

`QttuneChannelDecl` (signals.h) is `{ uint32_t rx_id; uint32_t tx_id; }`
— the §4 sketch used `unsigned long`; the shipped POD uses
fixed-width types per the C-ABI rule (append-only, POD-only growth).
`QttuneSignalSet` gains trailing `channel_count`/`channels` and the
obd2 set bumps its version to 2. Capacity is `QTTUNE_MAX_CHANNELS`
(4). Pre-existing aggregate initializers with three members still
compile (trailing members value-initialize); the obd2 tests state
all members explicitly rather than lean on that.

Validator additions (negative codes, disjoint from per-def returns):
missing array with nonzero count (-10), over-capacity (-11), zero or
out-of-CAN-range ids (-12), duplicate rx (-13).

### Resolution semantics

The channel table is derived at **attach**, before the device opens.
Priority: an explicit `?filter=` wins outright (single-channel
shorthand — pre-N3 semantics preserved for hand-built URIs);
otherwise the profile's declared channels fill the table, with a
lone `?txid=` rewriting every derived tx. A lone `?txid=` with no
profile channels yields NO channel: without an rx there is nothing
to register a filter for, matching the pre-N3 semantics where it
stored a tx nothing used. Set entries with rx==0 are skipped
defensively; the validator is the enforcement point.

### Coupling-rule relocation

Slice 2 enforced scheduled-PIDs-need-a-filter at `j2534_start`, after
`PassThruOpen` — the ORDERING NOTE in its "Generalized coupling
rule" section. Slice 3 resolves channels at attach and demotes that
check to a backstop firing before any hardware is touched, reachable
only by a future set author declaring scheduler-driving defs
without channels. The retired note's prediction held: the obd2 set
supplies 7E0/7E8 itself.

### Per-channel filters and routing

`j2534_start` registers one flow-control filter per channel; a
mid-loop failure unwinds already-registered ones so no partial filter
state survives a failed start. Detach stops every channel's filter
best-effort before disconnect (ADR-0012's vendor-compat priority).
The worker routes scheduler requests by PID → def message_id →
channel index (`rx_for_pid`), keeping `pid_scheduler.h` profile-
agnostic; `pump_one` matches the 4-byte header against every
channel's rx, stripping and stamping on hit. Non-matching frames
are dropped but still consume drain budget (bounds loop time).

The worker's error/backoff state moved from struct members to
worker-local variables as part of the rework.

### Consequence for bare URIs

A bare `j2534://` URI under the obd2 default now derives 7E8/7E0,
registers its filter and runs the scheduler. The two slice-2 tests
that pinned the rejection at start were replaced:
`BareJ2534UriDerivesObd2ChannelAndStarts` (filter shape, tx target,
rotation) and `TxidOverrideRewritesDerivedTransmitTarget` (derived
rx kept, tx rewritten to the broadcast pattern).
