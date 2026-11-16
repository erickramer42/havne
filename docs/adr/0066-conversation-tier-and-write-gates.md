# ADR-0066: The conversation tier and the write gates

- **Status:** Accepted 2026-11-16. This record is v0.9.0's write phase
  ("phase 4" in the planning below), built in steps, each proven on the
  simulated ECU in CI before any car. Nothing here is built.
- **Date:** 2026-11-16.
- **Builds on:** [ADR-0001](0001-qt-free-core-c-abi.md) (the Qt-free
  core, the C ABI as the boundary);
  [ADR-0013](0013-protocol-profiles.md) (profiles chosen by the URI,
  the URI as complete provenance, "what you connect to is a decision");
  [ADR-0015](0015-request-engine.md) (the request engine, its
  compile-time service allow-list);
  [ADR-0025](0025-one-j2534-backend.md) (one J2534 backend, the vendor
  DLL in a per-architecture surrogate, phase 4's Windows write
  transport);
  [ADR-0038](0038-clearing-trouble-codes.md) (the gate pattern for a
  change to the car: the report saved first, a confirmation that names
  what changes, a path that always works);
  [ADR-0041](0041-replay-fault-injection.md) (a recording as an ECU,
  faults at the ISO-TP level);
  [ADR-0046](0046-simulated-ecu.md) (the simulated ECU, its fault
  model); [ADR-0049](0049-recordings-in-core.md) (recordings, the
  folder core names); [ADR-0051](0051-signal-files.md) (signal files,
  the data tier's format); [ADR-0052](0052-plugin-framework.md) (the
  plugin framework, section 6's conversation tier and section 7's
  writes tier, bounded there and designed here);
  [ADR-0060](0060-extended-session-reads.md) (session control, the
  built-in session step); [ADR-0064](0064-one-store.md) (one store, the
  `lineage` carried into every import); [ADR-0065](0065-car-sized-profile.md) (the
  car-sized profile and sessions as data, the base the programming
  session builds on).
- **Amends, each by a dated note when built:** ADR-0052 section 6 (the
  runtime chosen, the guest ABI built, and the trust model settled: no
  signature is required to run a plugin); ADR-0052 sections 1 and 7, the
  write-signature rule reversed (they required a tier-3 plugin to be
  signed; section 2.4 drops it, for the reason 2.4 gives); ADR-0052
  section 7 (the writes tier takes its first steps in v0.9.0, behind a
  manifest and the gates, rather than waiting whole for 2.0, with whether
  a public build carries them left open); ADR-0048 item 4 (2.0 stays the
  public write product; the write mechanics start in v0.9.0 behind the
  gates, proven on the simulated ECU, which the major-number rule did not
  foresee); ADR-0046 (the simulated ECU gains security access, a readable
  memory region, a programming session and writable flash, and flash
  faults); ADR-0060 (the programming session as a sessions-as-data step
  on the module a write plugin drives); DISCLAIMER.md and STATUS.md (gate
  3's per-feature record, the write path's "Current state", and the
  roadmap line that writes are 2.0), changed in the same docs commit.
- **Leads to:** the rest of the conversation tier in v0.10.0 (gateway
  routing, dynamically defined DIDs, periodic reads, designed against
  the discovery capture); the mobile, WiFi and Linux write paths as later
  transport back ends behind this engine (section 7.2), with DoIP/ENET
  (ISO 13400) for newer vehicles, each placed in STATUS.md's roadmap as
  the owner schedules it; the public write product past 2.0 (map
  definitions, editors, diff and undo, tune management), which this
  record does not build.

## Context

[ADR-0052](0052-plugin-framework.md) set three tiers of plugin and
left two of them bounded but unbuilt. Its section 6 designed the
conversation tier (tier 2): code in a WebAssembly sandbox that opens a
module's doors (extended session, tester present, security access) and
reads, under a compile-time service ceiling that never allows a write.
It fixed the boundary and left the runtime, the guest ABI and the trust
model to the build. Its section 7 bounded the writes tier (tier 3): a
declarative operation plan core runs, with the plugin computing only a
value or a key, gated like a clear, with a backup first and a journal.
It placed the whole writes tier at 2.0.

v0.9.0 takes the first steps of both. The order is read first, then
write, in one phase, with the safety gates built before the write:

- Tier 2's first slice: the sandbox runtime, the guest ABI, security
  access (27) as the first conversation.
- Gate 1, the original copy (and the standalone dump): read the
  calibration as it stands into an immutable, versioned,
  checksum-addressed file, read back and compared. It is the recommended
  restore, offered before a flash, and a first-class operation on its
  own, not forced before a write (section 4.5).
- Gate 2, the write flow's safety: the envelope every write runs
  inside (voltage with a floor, a do-not-power-off state, the
  keep-alive core owns, a recovery story, an abort that works up to the
  point of no return, a dry run first).
- Gate 3, the legal and safety record: DISCLAIMER and the docs naming
  emissions law, ABS, TCS and stability-control exposure, and warranty,
  per feature.
- The flash mechanics: the programming session, the transfer and
  routine services, run from a plan behind the manifest and the gates,
  on J2534 only.

The forces any answer must keep, from the ADRs above:

- **Core never links Qt, and the C ABI is the boundary** (ADR-0001). A
  plugin is something core loads; the runtime and the operation machinery
  are core's, Qt-free, on every platform.
- **Every platform, iOS included** (ADR-0052's Context): iOS loads no
  native code at run time. A code plugin's logic must run without
  compiling to native on the device.
- **The request engine sends only what a compile-time list allows**
  (ADR-0015, ADR-0052 section 6). A plugin cannot widen that list.
- **The URI is complete provenance** (ADR-0013). A recording says what
  decoded it; a write session says what wrote.
- **Nothing on the network** (ADR-0052 section 10). A plugin is a file
  the user brings; there is no index, store or download.
- **A change to the car is gated** (ADR-0038): the report saved first,
  a confirmation that names what changes, a path that always works.

Everything below is exercised against the simulated ECU, whose
challenge, seed and key, bootloader and flash memory are fictional. No
real seed or key, no real vehicle's identifiers, appear here or in the
repository; committed text and fixtures say `<DID>`.

## Decision

The thesis that holds the phase together: **tier 2 opens doors and
reads; tier 3 runs write and transfer plans; core owns the wire for
both.** Tier 2 carries the conversation, tier 3 carries the transfer.
The sections below build tier 2's first slice, then set where the write
path lives (section 3's decision), then the three gates, then the flash,
in that order, with a sequencing section last.

### 1. The spine: tier 2 never sends a write, an unlock never widens the ceiling

This record's write phase grows tier 2 into a conversation tier able to
carry a flash. Read against ADR-0052 that has one firm reading, and the
whole phase rests on it:

- **Tier 2 never sends a write or a transfer.** ADR-0052 section 6's
  ceiling is a compile-time allow-list: a tier-2 plugin's WASM code may
  cause core to send `10 03`, `3E`, `27`, `22`, `2C`, `2A` and `19`, and
  never `23`, `2E`, `2F`, `31`, `34` to `37`, `3D`, `14`, `04`, `11`,
  `28`, `85` or `10 02`, whatever its manifest says. **The ceiling is
  checked at the sub-function level where a service's sub-functions split
  read from write:** `10 03` is allowed but `10 02` is not, and `2C 01`
  (defineByIdentifier) and `2C 03` (clear) are allowed but `2C 02`
  (defineByMemoryAddress) is not, since `2C 02` followed by `22` would
  read arbitrary memory from tier 2 and skip gate 1. The same
  `static_assert` pins the sub-function split. `2A` is in the ceiling but
  phase 4 enrols no periodic read (those are v0.10.0's), so nothing sends
  it yet; an unsolicited periodic reply would break the engine's one
  outstanding request per channel. A tier-2 plugin opens doors (the
  session, security access) and reads; it cannot reach a transfer service
  or read memory this way.
- **The transfer and programming services are tier 3, run through
  core.** Phase 4's tier-3 service list, pinned by its own
  `static_assert` beside tier 2's: `10 01`/`10 02`/`10 03` (session),
  `27` (unlock), `23` and `35`/`36`/`37` (read and upload), `34` and
  `36`/`37` (download), `2E` (a fingerprint or write-by-id before the
  erase), `31` (routines: erase, check), `3E` and `11 01` (keep-alive and
  reset). The pre-programming step some modules need (`85` to stop code
  setting, `28` to quiet normal messages, often to the functional
  address) is not in phase 4; a module that requires it waits for the
  plugin-supplied sequence (section 3). Core runs the list from a
  declarative plan (ADR-0052 section 7), with plugin code computing only
  a value or a key; a later step lets a plugin supply the sequence as
  sandboxed code, still emitting each request through core under the
  write ceiling. Either way the plugin never touches the wire, and tier 2
  never reaches a transfer service.
- **The original-copy read is a tier-3 read operation.** It uses `23`
  or the `35`/`36`/`37` upload sequence, none of which tier 2 may send.
  It writes nothing to memory, but it is not free of ECU state: it holds
  a core-owned session and its `27` unlock, and a plan that reads only in
  the programming session enters `10 02` (which stops the application on
  many modules), so the dump carries its own session handling (section
  4.5), not tier 2's. It rides the same directional transfer machinery a
  flash uses, so it is core's.
- **An unlock never widens the service ceiling.** Security access
  changes the ECU's state, but core's per-tier service list is
  independent of unlock state. Unlocking a programming level from tier 2
  still cannot send a write: the ceiling is checked at compile time per
  tier, not per unlock.

So the dependency order inside phase 4 is: tier 2 (the sandbox, `27`)
first, because the unlock gates the reads; then the transfer machinery
and gate 1 (the original copy) built on it; then gates 2 and 3; then
the flash, the download direction added to the same machinery, reusing
tier 2's `27` and the gates.

### 2. Tier 2's first slice: the conversation tier built

ADR-0052 section 6 designed the boundary (the sandbox, the service
ceiling, the `ecuio_plugin_conversation_ops` table) and left the
runtime, the guest ABI, the trust model and the first conversation to
the build. This section builds them.

#### 2.1 The runtime: WAMR, interpreter only, metering on

**The runtime is WAMR (the WebAssembly Micro Runtime), in interpreter
mode, with instruction metering on.** ADR-0052 section 6 named the
criteria: size on a phone, license, and maintenance. Metering is the
fourth, and it decides.

| Criterion | wasm3 | WAMR |
|---|---|---|
| Metering | None of its own; core would hand-roll an instruction counter or a wall-clock guard | Built in (`WAMR_BUILD_INSTRUCTION_METERING`): core sets a per-call budget |
| Maintenance | Minimal by the maintainer's own note; last release 2021 | A Bytecode Alliance project, actively maintained |
| License | MIT | Apache-2.0 with the LLVM exception |
| Size | Smaller | A small interpreter, both modes modest beside Qt |
| iOS and Android | Pure interpreter | Pure interpreter in classic or fast mode; AOT and JIT off |

Metering is decisive because ADR-0052 section 6 requires core to meter a
plugin's instructions per call so it cannot stall the worker. WAMR
provides it; wasm3 would need a hand-rolled guard core maintains.

- **The interpreter is why WASM is the sandbox at all.** The iOS rule
  (ADR-0052 section 1: iOS loads no native code at run time) is met
  because the interpreter is native code compiled into the app, and the
  plugin's WASM is bytecode the interpreter reads, never compiled to
  native on the device. **AOT and JIT are off on every platform,** not
  iOS alone, so one mode is tested everywhere.
- **Classic or fast interpreter, to confirm at the build.** Metering may
  be built for the classic interpreter only (to confirm against WAMR at
  the pinned tag); if it is, classic is fixed and the size question is a
  check, not a choice. Where both support metering, the pick is the fast
  interpreter if its size holds under the phone budget, else classic. The
  measurement needs no car and runs on the test phone and in the iOS
  Simulator.
- **WAMR is vendored at a pinned tag** under `third_party/`, a pin core
  controls and audits, its build flags set in Havne's CMake:
  interpreter on, AOT and JIT off, a minimal libc, metering on.
- **A caveat carried from WAMR's tracker:** an open issue reports
  metering state corruption on resumed execution. Havne meters per call
  and never resumes a call mid-instruction, so it is outside this path;
  the build confirms it before relying on the counter.

#### 2.2 The guest ABI over the ops table

ADR-0052 section 6's `ecuio_plugin_conversation_ops` (`begin`,
`next_request`, `accept`, `end`) is the host-side vtable core calls. The
build adds the guest side.

- **The module exports the four entry points,** matching the table, so
  the host code reads the same on both sides. The host calls each under
  a fresh instruction budget.
- **The module imports nothing that reaches outside the sandbox:** no
  clock, no file, no socket, no thread (ADR-0052 section 6). Requests
  and replies pass as offsets and lengths into the guest's own linear
  memory; the host reads and writes that memory, the guest never reads
  the host's. A tiny import surface (a log hook in a developer build, a
  bounded memory-grow) keeps the attack surface small.
- **Every PDU the guest asks to send is checked twice** before it
  reaches the wire: against the manifest's declared `services`, and
  against the compile-time ceiling (section 1). A `static_assert` pins
  the ceiling as the request engine's lists are pinned. The engine's
  request today is 3 bytes; each transport's send path grows to carry a
  whole PDU for the conversation tier (step 1's work on J2534 and
  SocketCAN, bounded at 7 bytes on ELM327, section 4.2).
- **The per-call instruction budget and the linear-memory cap** are
  bounded in core. A compile-time default budget, generous enough for a
  key computation and a small state machine, is overridable down by the
  manifest but never up. The linear memory has a fixed cap (1 MiB) a
  plugin declares within. A call that passes its budget is a plugin
  fault: the conversation ends for that module; core keeps the session it
  owns (the keep-alive is core's, section 2.5) and the built-in read step
  takes the module over, and the Log says so.
- **One sandbox instance per open module** (per `ecu`), so two modules'
  conversations never share memory or budget, matching the engine's
  one-outstanding-per-channel and sized from the open modules
  (ADR-0065 section 5).

#### 2.3 Security access (27) as the first conversation

Core owns the wire; the plugin owns only the key. Shipping the key
computation as a vendor-supplied script the tool runs is established
practice on production ECUs, so the plugin-computes-the-key shape is not
novel.

1. Core sends `27 <odd sub>` (request seed) for the level the manifest
   names. The ECU answers `67 <odd sub> <seed>`.
2. Core hands the seed to the plugin's `accept`. The plugin computes the
   key in the sandbox and returns the key bytes only; core builds the
   `27 <even sub> <key>` PDU with the sub-function that pairs with the
   seed it sent. Core refuses any `27` the guest tries to send from
   `next_request`, so the plugin cannot request a seed of its own, unlock
   a level other than the one asked, or send a `27` outside the one slot
   after a seed.
3. Core sends it. `67 <even sub>` unlocks; `7F 27 35` (invalidKey),
   `7F 27 36` (exceededNumberOfAttempts) and `7F 27 37`
   (requiredTimeDelayNotExpired) do not.

- **Core owns the attempt counter and the delay, never the plugin.** On
  `7F 27 36` or `37`, core refuses to send another `27` until the delay
  passes; the plugin cannot drive core past the lockout by asking again.
  This is the lockout the roadmap means by "honors the lockout delay":
  it lives in core, outside the untrusted code. `7F 27 37` carries no
  delay value to read, so core waits the manifest's declared delay, else
  a conservative built-in minimum, and never retries faster than that.
- **Core never retries a failed unlock on its own.** Some ECUs lock out
  for a long time, or permanently, after only a few wrong keys, so a
  burnt attempt is expensive. After any negative response to `27`
  (invalidKey, exceededNumberOfAttempts, requiredTimeDelayNotExpired),
  core stops and waits for the user to ask for another attempt; it never
  loops on its own. The delay pacing above governs when a
  user-requested retry may go, not whether core retries by itself.
- **The first `27` to a module needs the user's word.** Because any
  third-party plugin computes the key (section 2.4) and a wrong key can
  cost a long or permanent lockout, `27` is a state change with a cost
  the rest of tier 2 does not carry. So the first unlock attempt on a
  module waits for an explicit action: the front end names the plugin and
  the level and that a wrong key counts toward a lockout, and the user
  confirms (the app a dialog, `havne-cli` a flag, as ADR-0038 has it).
  Core cannot see the confirmation (ADR-0038 section 2); what core
  enforces is that it sends no `27` until the front end's call says so,
  and it shows the module's failed-attempt count, which it keeps across
  Havne sessions and app starts because the module's own counter does.
  "Session" here is the diagnostic session: an unlock lost when the
  session changes is asked for again, and needs the user's word again.
- **Which levels tier 2 may unlock** are named in the manifest as
  sub-function pairs; core allows only `27` within the tier-2 ceiling.
  Because the ceiling is independent of unlock state (section 1),
  unlocking a programming level from tier 2 still cannot send a write.
- **The seed-to-key window and the power-on delay.** Where an ECU
  enforces a seed-to-key window, the key must follow the seed inside it
  or the seed is reset; core bounds the plugin's `accept`-to-key time by
  the instruction budget and a wall-clock guard inside that window. On an
  overrun the seed has lapsed, so core sends no key at all (a late key
  would only be rejected and burn an attempt); a fresh seed is requested
  before another try. Some ECUs also require a minimum delay from
  power-on before the first seed request: core waits it out rather than
  racing the first `27`. Neither bound is the server response timer (P2
  or P2*).

#### 2.4 The trust model: the sandbox is the protection, not a signature

A tier-2 or tier-3 plugin runs code, so the question ADR-0052 section 6
left ("signing is decided with the tier") is settled here. The decision:
**a public build runs any third-party plugin, code plugins included,
with no signing requirement and no restriction to the repository's
example.** That is the point of an open, third-party plugin setup. The
protection is containment, not provenance, in four layers:

- **The sandbox:** the WASM interpreter with instruction metering and a
  bounded linear memory, and nothing reachable but the ops table (no
  clock, file, socket or thread). A plugin cannot do anything the host
  does not hand it, and cannot stall the worker.
- **The manifest's declared services under core's ceiling:** a plugin
  declares the services it sends; core checks every PDU against that
  declaration and against the compile-time ceiling, so a tier-2 plugin
  can send no write however it is written.
- **The user sees a plugin's allowed services before a session**
  (section 5, gate 2's preview), from the manifest, so what a plugin may
  send is visible before it runs.
- **The gates for any write** (sections 4 to 6): a transfer runs only as
  a tier-3 operation, behind the original copy, the safety envelope and
  the confirmation.

Where plugins load is unchanged from ADR-0052 section 4: the app loads
an imported plugin from its own data folder, `havne-cli` takes explicit
paths, and there is no search path or environment variable, so nothing
loads that the user did not name. A code plugin imported as a packed
file runs the same way on a phone: the sandbox contains it on every
platform. The one carve-out from ADR-0052 section 6 stands: **native
(non-WASM) modules remain developer-only;** a release build runs WASM
plugins only.

**Ed25519 is not adopted in phase 4.** A signature over the plugin's
canonical bytes, as a trust gate, is not load-bearing with no signing
requirement to run: it would add
a public-key primitive to the Qt-free core (a new dependency and attack
surface) for an informational display alone. Provenance is already
carried without it: ADR-0052's `source` block (the author and license,
copied into every recording) and ADR-0064's `lineage` (the signal file a
plugin descends from). A signing-for-provenance feature, or a trust
store, can return in a later ADR if one is wanted; it is not built here
and is never a precondition to run a plugin. So core gains one new
dependency in phase 4 (WAMR), not two.

#### 2.5 The relation to the built-in session step

ADR-0060 built `?session=03` as a fixed step and said a module that
loads a conversation plugin does not run the built-in step on that
module. The keep-alive and the session's close are core's on every
module, by ADR-0060's rules: a guest has no clock and cannot time `3E`
against S3, so the plugin's `begin` asks for the session it needs and
core enters it, keeps it alive with `3E`, and returns the module to the
default session with `10 01` at the close (both are core's, like the
attempt counter, not the plugin's to send, and ADR-0060 section 2 and
DISCLAIMER already require the return). So 2.5 needs no special case for
a write: every tier-3 operation, read or write, holds one core-owned
session, entered first and unlocked inside it. After a plugin fault core
keeps the session and says so, rather than leaving it to a module with no
driver. Sessions as data is specified here (ADR-0065 section 6 only
reserves the room for it); ADR-0060 section 3's built-in step still
refuses `10 02` for its own use.

#### 2.6 License

Havne is MIT. WAMR is Apache-2.0 with the LLVM exception, permissive and
inbound-compatible with an MIT project as a vendored dependency carrying
its own notice. Leaving out Ed25519 removes the one other inbound
license a signing dependency would have added.

#### 2.7 What the simulated ECU gains

This extends ADR-0046's engine node and is faults-free; the faults come
with the flash (section 7).

- A security-access level: `27 01` returns a fixed, fictional seed;
  `27 02` expects a key from a documented, clearly fictional transform
  (no real algorithm) and answers `67 02` or `7F 27 35`.
- A lockout: after a set number of wrong keys, `7F 27 36`, then
  `7F 27 37` until a fictional delay passes. The counter and delay are
  the simulator's, so a session's lockout handling is tested with no
  car.
- A DID the simulator answers only after the unlock (`7F 22 33`,
  securityAccessDenied, before it), so an example plugin "unlocks the
  simulated ECU's gated DID."

#### 2.8 The package: carrying code and an image

ADR-0052 section 2's package is JSON with each data file inlined, every
file an ADR-0051 signal file of at most 1 MiB, the package at most 16
MiB, and `tiers` only `["data"]`. A code plugin and a write plugin need
more, decided here:

- **The manifest's format number rises** (format 2), and `tiers` gains
  `"conversation"` and `"write"`. A format-1 reader refuses a format-2
  package whole, rather than load part of it.
- **A binary file (a `.wasm` module, a firmware image) is carried** as a
  `files` entry with a `kind` (`wasm`, `image`), its bytes base64-encoded
  and its SHA-256 recorded. The `.wasm` keeps the 1 MiB signal-file
  bound; an image is bounded by the package's 16 MiB.
- **The manifest keys the new tiers need:** the declared `services` (with
  sub-functions), the security levels and their key lengths, the voltage
  floor, the operation plans (the dump, the flash, the restore), and each
  plan's declared ranges, `dfi`, point of no return and recovery.
  `plugins check` lists the declared services (ADR-0052 section 6) and
  the plans.
- **The schema and its version are pinned,** and the manifest's format
  number is the single gate: a reader that does not know format 2 loads
  nothing from the package.

### 3. Where the write path lives

**The decision: Havne owns the wire and every gate; the plugin supplies
the car-specific write sequence (option 4 below).** The declarative UDS
plan is built first; the sandboxed-code sequence (emitted through core,
under a manifest write ceiling the user sees, with the point of no
return marked) comes later, with a second simulated bootloader to prove
it. Whether a public release compiles the write services out (option 3)
stays open; where the write path lives does not.

The choice turned on three things pulling different ways: where the
safety gates are enforced, whether one transfer engine can cover real
vehicles, and the project's legal and distribution exposure. The
subsections below set out that reasoning: the options weighed, and what
option 4 costs. What follows is engineering considerations, not legal
advice.

#### 3.1 How far a fixed transfer sequence reaches

A fixed UDS engine is `10 02`, `34`/`36`/`37`, a `31` erase and check,
`11` reset. On a modern UDS module that is the whole shape. It does not
cover every vehicle a plugin might target, and the older the vehicle the
less it covers:

- **KWP2000 (ISO 14230):** service numbers close to UDS's but a
  different session model and timing rules; a flash built to UDS's
  sequencing and timers does not drive it unchanged.
- **K-line and older diagnostic buses:** a different line protocol under
  the services, not CAN ISO-TP, so the wire core owns is not the same
  wire.
- **Manufacturer bootloaders:** a module may drop into a bootloader with
  its own services, or a raw-CAN protocol with its own request and ack
  framing that is not UDS at all.
- **Different block and checksum schemes:** block length, addressing and
  the end-of-transfer checksum vary; erase may be an explicit routine
  (`31`) on one module and implicit in the download on another.
- **A kernel loader and a two-stage download:** a protected module may
  accept a flash only through a small kernel uploaded to RAM first (an
  encrypted `34` to RAM, started by a routine), the payload then sent
  through that kernel; a single download does not reach it (section 7.4).
- **Calibration and older transports:** some modules are written over a
  calibration protocol (CCP or XCP) rather than UDS, or over an older
  manufacturer transport, each with its own framing and flow.

So a single fixed sequence in core either stays UDS-only (and a non-UDS
or older vehicle needs core changed, not a plugin) or grows into a
multi-protocol engine core maintains and must test without those
vehicles on hand. Only car-confirmed generic facts inform this; no
vehicle's private sequence appears here or in the repository.

#### 3.2 The options

1. **Mechanics and gates in core, vehicle-specifics in the plugin.**
   Core carries the transfer engine, the wire, the envelope
   and every gate; the plugin carries the key, the plan's parameters and
   the image. Safety is strongest: every write PDU is core's, checked
   against the ceiling and metered, and the abort line is core's.
   Architecture is cleanest: one engine, Qt-free, on every platform
   including iOS. The cost is reach: the engine is UDS-shaped, so a
   non-UDS or older vehicle needs core extended rather than a new
   plugin. Legally, the public release ships the write engine itself.
2. **The write path in the plugin.** A plugin carries its own transfer
   code and drives the write sequence itself. This reaches any protocol
   the plugin's author can write, and the plugin, not the project,
   authors a vehicle's write. But if the plugin owns the wire, the
   service ceiling, the metering, the keep-alive, the voltage floor and
   the abort boundary are no longer core's to enforce: the gates become
   advice, not a fence, and the most dangerous service sits behind the
   least enforcement. On iOS the plugin's code still cannot be native,
   so it has to run in the sandbox anyway, which means core still
   mediates the wire, which is option 4, not this. Taken literally (the
   plugin on the transport) this is the least safe option and the
   hardest to test uniformly.
3. **Mechanics and gates in core, compiled out of a public release.**
   Option 1's code behind a build option, so a public release build
   carries no write or transfer services and only a build the user makes
   themselves enables them; the plugin stays as in option 1. This is
   orthogonal to where the code lives: it answers the distribution
   question (what the public release ships), not the reach question
   (older vehicles), and it composes with option 1 or option 4. It is
   the existing open item "whether a public build carries the write
   services at all" ("What stays open").
4. **Core owns the envelope and the wire; the plugin supplies the
   transfer sequence.** Core keeps the transport and the wire (its own
   ISO-TP over raw CAN, or whole PDUs through an adapter that frames it),
   the voltage floor, the keep-alive, the original copy, the
   journal, the logging, the confirmation and the abort boundary. The
   plugin supplies the sequence: declaratively where the plan can
   express it (the UDS case of option 1 is exactly this plan), or as
   sandboxed code that emits each request through core one at a time,
   under a write ceiling the manifest declares and the user sees before
   the session. The plugin never touches the wire; every PDU still
   passes core's check, metering and log. The plan must mark its point
   of no return (the erase or its equivalent) so core can still draw the
   abort line. This reaches non-UDS and older protocols (the sequence is
   the plugin's) while keeping core as the single enforcement point (the
   wire is core's). It is the generalization of option 1: option 1 is
   option 4 restricted to one declarative UDS plan.

#### 3.3 What option 4 costs, and what it needs from the simulator

Moving the sequence to the plugin costs core some of what it can check
statically. With a fixed UDS engine, core knows each service's meaning:
this `31` is the erase, that `34` sets the block length. With the
sequence supplied by the plugin, core no longer knows the semantics; it
knows only what the plan and manifest declare. So:

- The write ceiling is coarser: it must allow the union of transfer and
  write services a bootloader might use, so it guards "a write happens
  only in a tier-3 write session, behind the gates, declared in the
  manifest and shown to the user," not "only these exact services in
  this order."
- Core leans on the plan's declared markers rather than on understanding
  the protocol, but it does not trust the mark blind: for the declarative
  UDS plan core sets a floor of its own, the point of no return being the
  earlier of the declared mark and the first `31`, `34`, `2E` or `3D`
  sent in the programming session (a mark can only move it earlier). The
  simulator adapts to the plan, so it cannot catch a real module's
  mismark; core's floor is what guards the abort line.
- What core enforces regardless: every PDU through core's check, metering
  and log; the verified covering copy first; the voltage floor and the
  do-not-power-off state; the keep-alive; the journal; the request log;
  and the abort line drawn at core's point of no return (above). The
  write confirmation and the services preview are the front end's word,
  not core's (section 2.4, and ADR-0038 section 2: core cannot see a
  confirmation). These are the gates, and they hold in option 4 as in
  option 1.

To test option 4's generality, the simulated ECU must model more than
the one UDS bootloader of section 7: at least a second, non-UDS
bootloader shape (a KWP2000-style session, or a raw-CAN block protocol
with its own framing, a different block and checksum scheme, and erase
implicit rather than an explicit routine), so the sequence-from-plugin
path is proven against more than one protocol in CI. That is fixture
cost beyond this record's single UDS bootloader, and it is the price of
proving reach without a car.

#### 3.4 The decision and what it rests on

**Option 4 is chosen, with the declarative UDS plan built first and the
sandboxed-code sequence later.** The enforcement sits architecturally in
Havne (the wire, the gates, the abort line stay core's), while the
vehicle-specific sequence lives with the plugin that authors it, which
also answers the reach problem older vehicles raise. The declarative UDS
plan is option 4's simplest expression and the only shape the first
simulated bootloader needs; the sandboxed-code sequence path follows when
a non-UDS vehicle is the target and a second simulated bootloader exists
to prove it (section 3.3). Option 2 (the plugin on the wire) was
rejected: it trades away the enforcement that makes any of this safe, and
on iOS it collapses into option 4 anyway. Option 3 (a build-time
compile-out of the write services) is orthogonal and stays open for the
public release (see "What stays open").

Until the sandboxed-code path is built, the body below describes the
declarative UDS mechanics (option 4's first case); the flash section
generalizes to a plugin-supplied sequence without changing the gates, and
the simulator grows the second bootloader shape (section 3.3).

### 4. Gate 1: the ECU dump and the original copy

Gate 1 reads the calibration as it stands into an immutable, versioned,
checksum-addressed file, read back and compared before anything else, so
a later step can show against it. The dump is the recommended restore but
not forced before a write (section 4.5); writing a copy back is a
separate operation and the plan's claim (section 7.3), not something gate
1 guarantees. Gate 1 introduces tier 3's operation shape, in its read
direction.

**The same read is a first-class operation on its own: a dump (read and
save the ECU, no write).** It is the first tier-3 operation to ship
(sequencing step 2, before the flash exists at all), started from the app
or `havne-cli` with no flash planned (section 4.5). Like the flash, it
needs a successful security-access unlock first: core reads no memory
until the plugin's key draws a `67` positive response (a confirmed
`27`/`67` chain, section 2.3). It runs under gate 2's lighter read
envelope (section 5): the voltage shown and the floor honored, but no
do-not-power-off state and no charger demand. When a write follows, the
saved read is also the restore; when none does, it is simply a saved
dump. The `backups/` folder holds the file either way (its format section
4.2, its retention and privacy section 4.4).

#### 4.1 The operation plan (tier 3's shape, introduced here)

ADR-0052 section 7 fixes an operation as a named plan: parameters,
preconditions, the reads that back up what changes, the steps, the
read-back, the undo. Core runs the plan; in phase 4's declarative form
plugin code computes only a value or a key (a plugin-supplied sequence
comes later, section 3). Gate 1 is the first and simplest: its steps are
a read, its undo is empty (a read changes nothing), and its read-back is
a second read whose SHA-256 core compares with the first (a read never
sends a `31`, section 4.2). Introducing the shape here, where the first
operation needs it, keeps it concrete rather than an abstract framework
with no operation to apply it to.

#### 4.2 The read itself

- **Which service reads the memory:** core supports both `23`
  (ReadMemoryByAddress) and the `35`/`36`/`37` upload sequence, the plan
  naming which and the address range or region. The simulated ECU
  answers both, so both are exercised. The upload sequence shares the
  transfer machinery the flash extends, so building it here is not
  wasted.
- **The dump's transport reach, and the ELM327 bound.** A read is not
  Windows-only the way a write is: `23` and the `35`/`36`/`37` upload run
  on J2534, SocketCAN and ELM327, so a dump can run on the phone, Linux
  and macOS. But ELM327 carries a request of at most 7 bytes
  (`core/src/elm327_transport.cpp`), so a `23` with a 4-byte address and
  a 2-byte size (8 bytes), an `35` with its format bytes, or a `27` whose
  key runs over 5 bytes do not fit it; those need SocketCAN or J2534.
  Core refuses up front from the manifest's declared lengths, before any
  seed is asked. The confirmed `27`/`67` unlock is required before any
  read (the intro). A large upload over a slow adapter is slow, not
  blocked. On a transport with no voltage source (SocketCAN), the dump
  runs anyway with a line that the voltage cannot be read; `mock://` asks
  nothing (ADR-0060 section 1), so no dump runs on it.
- **What the file holds, and how it is addressed:** a new file type in a
  `backups/` folder beside recordings (ADR-0049's folder), named by the
  SHA-256 of its raw bytes (checksum-addressed), holding the raw bytes,
  the address map read, the plugin id and version, the transport and
  adapter, a timestamp, and the ECU's identifiers (on the device;
  committed fixtures and this text say `<DID>`). Immutable: written once,
  never edited; a new read is a new file with its own checksum.
- **The read-back compare:** a read never sends a `31` (a routine id is
  the manufacturer's, so core cannot tell a checksum routine from an
  erase; every `31` is write-class, section 7). Core instead reads the
  range a second time and refuses to call the copy good unless the two
  passes' SHA-256 match, so a truncated or corrupted read is caught
  before it is trusted as the restore.

#### 4.3 What the simulated ECU gains

- A fictional memory region of a fixed size, readable by `23` and
  uploadable by `35`/`36`/`37`, but only after the security-access unlock
  (before it, `7F 23 33` / `7F 35 33`, securityAccessDenied), so the
  dump's required unlock is exercised with no car.
- A second region the simulator answers only inside the programming
  session (`10 02`), so the dump's session handling (a plan that must
  enter `10 02` to read) is exercised with no car.

#### 4.4 Keeping the restore: retention, export and privacy

The original copy holds the ECU's identifiers (on the device; `<DID>` in
committed text and fixtures) and is a restore the user may rely on, so
its handling is stricter than a recording's:

- **Never pruned, and never deleted without asking.** Unlike recordings
  and the surrogate log, the `backups/` originals are not aged out; core
  removes one only when the user asks, and confirms before it does.
- **Scrubbed before it leaves the device.** An export runs the identity
  scrub Havne applies before evidence is shared: it covers the file's
  metadata, the identifiers Havne recorded beside the bytes. The raw
  calibration bytes themselves may still hold identity or security data
  (a VIN, immobilizer data) that no metadata scrub reaches, so core says
  so at the export, and the raw bytes go only where the user chooses.
- **The restore tension on Android.** Android excludes the app's data
  from its automatic backup, so an original kept only in app data is lost
  if the app or the device is. The user can export the copy to keep the
  restore off the phone, but a restore has to match the same ECU, so the
  identifiers it needs to target that ECU are exactly what the scrub
  would remove. Core states that trade at the export (a working restore
  keeps the identity; a shareable export is scrubbed and is not one) and
  lets the user choose, rather than choosing for them. The export is
  always the user's action, never automatic.

#### 4.5 Running the dump, and the flash's dependence on it

- **Entry points:** the dump is started from the app (a read-and-save
  action on a connected ECU) and from `havne-cli` (a dump subcommand
  naming the plugin and the region), on its own, with no flash planned.
  Both go through the same tier-3 read operation and the same confirmed
  `27`/`67` unlock.
- **It survives option 3.** A dump is a read, so a build with the write
  services compiled out (option 3, section 3.2) still carries it: the
  compile-out removes the memory-writing services (`34`, the download
  direction of `36`/`37`, every `31`, `2E`, `3D`) but keeps the session
  services (`10 01`/`10 02`/`10 03`, `11 01` to leave a programming
  session), the upload direction and `23` for a read, so an ECU that
  answers reads only inside `10 02` can still be dumped and left cleanly.
  A read-only build can dump and unlock but not flash.
- **At a flash, core checks for a restore but does not force a dump.** A
  dump can take minutes, so it is the recommended path, not a
  precondition. At a flash core looks in `backups/` for a verified copy
  of this ECU, matched on the hardware identifiers and on the software and
  calibration ids (the CALID and CVN, or the UDS software-number DIDs,
  said as `<DID>`), core adding its own identification read (ADR-0021) so
  the match does not rest on the plugin's DID choice alone, current (a
  copy whose ids differ from the ECU now is stale, another tool may have
  flashed it since) and covering every range the plan erases or downloads
  (H3; a copy of a small range does not cover a flash that erases a large
  one). What a routine erases beyond the declared ranges, core cannot
  see; that is the plugin's claim, shown with the recovery declaration.
  - If such a copy exists, it is the restore.
  - If none does, the front end says so plainly, and the user either
    starts a dump first (offered, one action) or confirms they hold their
    own restore file. Naming that file is optional; if named, core checks
    its size and its coverage of the plan's ranges where it can. The flash
    then goes on, and the journal and request log note "no verified dump,
    the user's own restore" or the named file's SHA-256.

  Havne keeps no stock-image database and fetches nothing from a network
  (ADR-0052 section 10), so unlike a curated cloud service that
  regenerates a stock image from the ECU's ids, holding a restore is the
  user's own responsibility: Havne makes the dump easy and recommends it,
  and records which restore a write ran against.

### 5. Gate 2: the write flow's safety

The envelope every write runs inside. Nothing here is specific to the
flash; it wraps any tier-3 write.

- **Battery voltage throughout, with a floor that stops the start.**
  J2534's `READ_VBATT` during a flash (J2534 is phase 4's write
  transport, section 7.2), ELM327's `ATRV` where a pre-write read uses
  it.
  SocketCAN and the mock have no voltage source and carry no flash in
  phase 4. A configurable floor with a safe engine-off default: core
  refuses to start below it and shows the voltage the whole time. The
  manifest may raise the floor, never lower it.
- **A do-not-power-off state** core raises for the duration and the UI
  shows unmistakably.
- **Preconditions checked:** engine off, key on, a charger advised; the
  plan names them, core checks what it can read.
- **The keep-alive (`3E`) owned by core through the whole write
  operation,** from before its first request until after the `11` reset,
  so the session never lapses. Core owns `3E` on every module (section
  2.5), so a write operation's one core-owned session, the `27` unlock
  included, is never handed to or from the plugin, and the S3 timer is
  never missed.
- **A recovery story stated before the write starts:** how the
  bootloader is re-entered if the transfer fails, shown to the user
  before the first byte. Recovery is the governing safety net where power
  or connection cannot be guaranteed (section 7.2): core refuses a plan
  that declares no recovery path, shows the declaration to the user as
  the plugin's claim (beside the services preview), and the dry run
  exercises it against the simulator. Core checks the declaration for
  presence and shape; it cannot itself prove the bootloader survives.
- **A transfer resumable or recoverable, and a wedged write never
  silent:** a stalled transfer surfaces rather than hanging quietly.
- **An abort that always works up to the point of no return.** Core sets
  the point of no return itself for the declarative UDS plan: the earlier
  of the plan's declared mark and the first `31`, `34`, `2E` or `3D` in
  the programming session (a mark can only move it earlier, section 3.3),
  so a mismarked plan cannot push the boundary past the first write.
  Before it, abort is one action and always available: core stops, sends
  the plan's declared way out (`10 01` or a reset) and journals the stop.
  After it the module is committed and stopping can brick it, so the only
  paths are completing or re-flashing, named as recovery, not abort. For
  the UDS plan the erase precedes the download (section 7), so the
  boundary falls early and the long block transfer runs entirely in the
  recovery-only regime, which is why the recovery story, not the abort,
  covers it. Core refuses to start a plan that declares no point of no
  return.

**What core does when a step fails,** before and after the point of no
return (the rule: before it core stops cleanly; after it core never
stops by itself, it warns, journals and goes on or hands the user the
recovery):

| Failure | Before the point of no return | After it |
|---|---|---|
| Voltage under the floor | stop, way out, journal | warn, hold the power-off lock up, go on |
| `36` refused (`71`, `72`, `73`) | retry the same block a bounded number, then stop | retry the same block a bounded number, then hand the recovery |
| `78` (response pending) repeating | wait, bounded | wait, unbounded (the module is working) |
| the surrogate child gone, or the link lost | stop, way out, journal | journal where it stopped, hand the recovery |

The dry run exercises each row against the simulator (section 7.1).

Plus what makes live testing safer:

- **A dry run against the simulated ECU, required before a real write.**
  Core requires a dry run to have passed for a plan before it will write
  to a car, and checks the record it left (the plugin's id and version
  and the plan's and image's SHA-256) against what is about to run. The
  dry run is a developer's and CI's step too: `havne-cli plugins check`
  runs it for a write plugin. Where the simulated ECU lives is settled in
  section 7.1 (one model reached through `ecuio_sim_ask`: the fake J2534
  DLL on Windows, `vcan` on Linux, in-process where neither bus is
  present), so the dry run runs on the platform that writes. Two things a real plan carries will
  not match the simulator, so its dry-run mode adapts:
  - **Security access:** a real key answers the vehicle's algorithm, not
    the simulator's fictional transform, so a real plan's `27 02` would
    fail. The dry run grants the unlock as a stub (the simulator accepts
    the write session without checking the key). The example plugin,
    whose key matches the documented fictional transform, runs its `27`
    in full, so the unlock path is still exercised in CI.
  - **The memory map, block sizes and routine ids:** a real plan names
    that car's addresses, block length and routine ids, which the
    simulator's fixed region does not hold. The dry-run mode adopts the
    address range, `maxNumberOfBlockLength` and routine ids the plan
    declares, so the real plan's framing and point-of-no-return floor
    drive the exchange.
  What the dry run proves for a real plan: its sequencing and block
  counting, the covering-copy check, the point-of-no-return floor, the
  journal and the request log, end to end. What it does not prove: the
  real key, that the real ECU accepts the image (its own checksum or
  signature check), that the bytes land, and the voltage floor (the fake
  DLL's `READ_VBATT` proves that, not the dry run). Those are a car's, at
  the user's parked run.
- **The plugin's operation shown before a session,** from the manifest:
  each service named in words (not a bare number), and for a write the
  ranges it erases and writes, the image's size and SHA-256, the declared
  point of no return and the recovery declaration, which is what ADR-0052
  section 7 asks the confirmation to name. This is the front end's word,
  not core's (section 2.4).
- **A permanent request log for a write session, beside the original
  copy and the journal.** Writes are J2534 only in phase 4 (section 7.2),
  and the surrogate's `%TEMP%` log records request and reply payloads
  only when `ECUIO_SUR_LOG_FRAMES` is set, and keeps a bounded number of
  sessions. Neither the opt-in nor the pruning is good enough for a
  write's record. So a write session writes its own request log into the
  `backups/` folder beside the original copy and the journal (section 4):
  every request and reply in full, except the `27` seeds and keys and the
  `36` payloads, which are recorded as a length and a SHA-256 (the key
  algorithm is a secret, ADR-0052 section 10, and a `36` payload is the
  dump's identity data again). A recording or wire log of a tier-2
  session that captured `27` bytes is covered by the same scrub (section
  4.4).

Gate 2 applies in full to a flash. For gate 1's read, which draws little
current and changes nothing, the envelope is lighter: the read shows
voltage and honors the floor (a dropout mid-upload still wants catching)
but does not raise the do-not-power-off state or demand the charger. The
full envelope is the write's.

The fake J2534 DLL gains a settable `READ_VBATT`, so the floor, the
dropout and the voltage display are tested with no hardware.

### 6. Gate 3: the legal and safety record

- **The per-feature disclosure rule:** every write operation's
  confirmation names its legal and safety exposure in the operation's
  own words, so the record is a line the user sees at the point of
  action, not one global paragraph.
- **DISCLAIMER's "Current state"** gains the write path's shape once it
  is built: what services, that the transport is J2534 only in phase 4,
  that a dump is taken first, written as the present state.
- **A `docs/write-safety.md`** that collects the gates, the recovery
  story and the regulatory notice in one place the confirmations cite.

Gate 3 is a short section rather than an ADR of its own because this
record folds the whole phase into one ADR; DISCLAIMER's change and the
per-feature rule have their home here. No simulated-ECU change.

### 7. The flash mechanics

Built only after gates 1 to 3. The flash is a tier-3 operation. Phase 4
builds it as a declarative plan core runs, the plugin's code computing
only a value or a key (the seed and key are tier 2's; the flash plan may
compute a checksum or a block parameter); the plugin-supplied sequence
(section 3) is a later step and still emits every request through core.
That tier-3 code runs in the same
metered WAMR sandbox as tier 2's, under the same per-call instruction
budget and linear-memory cap (sections 2.1, 2.2): a checksum or block
value is computed in the sandbox and returned to core, never put on the
wire by the plugin. A calibration image can be several MiB, past the
1 MiB linear cap, so a checksum over the whole image is not held in the
sandbox at once: core computes the standard checksums itself (CRC-16,
CRC-32, a running sum), the plugin naming which; a plan that needs a
nonstandard checksum has core feed the image to the guest in bounded
chunks over repeated calls, each under its own budget.

- **The restore check, not a forced dump.** The rule is 4.5's: core looks
  in `backups/` for a verified, current copy of this ECU (hardware and
  software ids, core's identification read added) covering every range
  this flash erases or downloads. If one exists it is the restore; if none
  does, the user starts a dump (recommended, one action) or confirms their
  own restore file, and the record notes which.
- **The service sequence,** the programming step of ISO 14229's sequence
  (not the whole of it: phase 4 has no pre-programming step, so no `85`
  to stop code setting and no `28` to quiet normal messages; a module
  that needs them waits for the plugin-supplied sequence, section 3).
  Each step is allowed only through the manifest and behind the gates:
  the programming session (`10 02`, entered as a session, specified in
  2.5), security access (`27`, section 2.3), an optional fingerprint
  (`2E`) where the module requires one before the erase, the erase
  routine (`31` routine `FF00`, ISO's erase id; a module's id may
  differ), `34` (request download), `36` (transfer data, block by block
  with a wrapping sequence counter, the block length from `34`'s reply),
  `37` (transfer exit), the check routine (`31` routine `FF01`, ISO's
  check programming dependencies; a module's id may differ), `11 01`
  (reset). The erase precedes the download, so the whole transfer runs
  past the point of no return (the abort boundary, section 5). Core owns
  the block counter, the keep-alive, the P2/P2* timing and the sequence;
  it owns the ISO-TP framing itself only where the link is raw CAN, and
  over J2534's ISO15765 channel or DoIP the adapter frames ISO-TP while
  core drives whole PDUs (section 7.2).
- **The transport:** J2534 only in phase 4 (the OBDX Pro VX), the first
  transport to meet the write capability contract and be car-proven
  (section 7.2). A generic ELM327 is refused (it cannot send a
  multi-frame request and hides errors behind its text protocol); a real
  controller over SocketCAN carries no flash in phase 4 (no voltage
  source, no car-proven path), though a `vcan` write to the simulated ECU
  is allowed for CI (section 7.1). Because J2534 is Windows-only
  (ADR-0025's surrogate), **real writes are Windows-only in phase 4:** the
  phone, Linux and macOS read and unlock (tier 2) but write no car in
  phase 4. The mobile, WiFi and Linux write paths come as later transport
  back ends behind the same engine and gates (section 7.2).
- **Verification:** the `31` check routine; a read-back can be
  byte-compared to the written image only when both are the same
  representation (a plaintext `dfi=00` write), since a read-back is
  plaintext and a transformed or encrypted image is not. Otherwise the
  `31` check is the verification (section 7.4).
- **Where the firmware image comes from:** the plugin's data files for a
  flash, or a `backups/` copy or the user's own file for a restore
  (section 7.3), in the `dfi` the plan declares (section 7.4), bounded as
  a plugin's data is (ADR-0052 section 2). The repository's example
  flashes the simulated ECU's fictional plaintext region with a fictional
  image; a real vehicle's image rides its own private plugin or the
  user's file, never in the repository.
- **How the plan expresses the transfer:** a small declarative shape in
  the manifest, enough for the simulated ECU's one region, extended
  later: the region, the block length source, the `dfi` (`00` in phase
  4), the routine ids and the point of no return. It need not cover every
  vehicle's layout in phase 4.
- **The journal** (ADR-0052 section 7): a step-by-step journal written
  beside the original copy, so a flash that stops leaves a record of
  where it stopped. Its unfinished entry pins the copy taken before that
  write (section 7.3).

The sequence above is ISO 14229's programming step, which the simulated
ECU models. No specific vehicle is confirmed to follow it: a real module
may need a pre-programming step, erase implicitly, use different routine
ids, or diverge from UDS entirely (section 3.1), and that unknown is why
the sequence is proven against the simulator first and why the plugin
supplies the order the car actually needs (section 3). A car confirms the
real shape only when the user runs it.

#### 7.1 What the simulated ECU gains, and where it lives for a write

A fictional bootloader and writable flash memory, with faults and
recovery, all in CI. One ECU model serves every transport: the security
access, memory region, bootloader and faults live in core's simulated
ECU (ADR-0046), transport-independent, reached through `ecuio_sim_ask`
on an unbound sim (`ecuio_sim_create_unbound`, which answers with no bus,
worker or socket and runs on every platform). The transports differ only
in how a request reaches that model:

- **On Windows, the fake J2534 DLL routes to the sim.** It embeds an
  unbound sim and forwards each request to `ecuio_sim_ask`, so the J2534
  write path, whole PDUs through the surrogate, runs in CI on both
  architectures without a car. `tests/fake_ecu.h` keeps its existing
  answers for the current J2534 tests and delegates the new write
  services (security access, memory, bootloader) to the sim, so there is
  one ECU model, not two.
- **On Linux, a `vcan` write to the sim is allowed** (the interface check
  ADR-0046 section 2 already makes), so the same flash runs over
  SocketCAN against the bound sim; a real controller stays refused.
- **Where neither bus is present, `ecuio_sim_ask` is called in-process**
  for the dry run, so every platform can dry-run a plan.

The bootloader and faults the one model answers:

- A programming session (`10 02`) the engine node enters, with state
  distinct from the extended session.
- Writable flash the `34`/`36`/`37` download sequence fills, behind a
  security-access unlock and the programming session, with a
  `maxNumberOfBlockLength` stated in `34`'s reply.
- A dry-run mode that adopts the address range and
  `maxNumberOfBlockLength` a plan declares and stubs the security unlock,
  so a real plugin's plan runs end to end in CI with no car (section 5).
- A `31` erase routine (clears the region) and a `31` check routine
  (validates it), each with a fictional timing.
- Faults, as ADR-0041 and ADR-0046's fault model adds them, per module
  and periodic: power lost mid-transfer (the transfer abandoned), an
  NRC 78 storm (`7F 36 78` repeated), timeouts, a block refused (`71`,
  `72`, `73`), a checksum refused (`7F 31 ...` on the check), and
  `requiredTimeDelay` on security, so each row of the failure table
  (section 5) is exercised. And the recovery: a flash interrupted past
  the erase, then restored from the copy (section 7.3), the bootloader
  re-entered after a failed flash, all with no car.

#### 7.2 The transport capability contract, and the path to mobile and Linux writes

Phase 4 proves writes on the OBDX Pro VX over J2534 on Windows, because
that transport gives the voltage read and the ISO-TP timing a flash needs
and it is the first to be car-proven. That is a starting point, not a
ceiling: a main goal of the project is mobile writes (a phone flashing a
car over a wireless adapter, as established tuning apps do for one make)
and writes on Linux. The write engine is built transport-agnostic to
reach them: core owns the wire, the gates and the recovery; the plugin
supplies the sequence (option 4); the transport is a replaceable back
end.

A transport is **write-qualified** when it provides:

- **Whole-PDU link delivery:** the link carries whole UDS requests and
  replies of any length up to the plan's block size, with standard
  ISO-TP timing, errors reported, and no silent retries or reordering.
  Core's own ISO-TP over raw CAN meets it; so does an adapter that frames
  ISO-TP itself (J2534's ISO15765 channel, DoIP). A generic ELM327 fails
  it: it cannot send a multi-frame request and hides errors behind its
  text protocol. This is why a mobile write path needs a qualified
  adapter, not any dongle; a wireless adapter will most likely frame
  ISO-TP in its own firmware (WiFi and Bluetooth jitter make host-side
  N_Bs/STmin timing hard), which is fine under this condition.
- **A voltage reading, or a procedure fallback:** where the transport or
  vehicle exposes voltage, core enforces the floor (section 5); where it
  cannot, core requires the user to confirm a stable supply (a charger)
  and leans on recovery. The power risk scales with the write's length
  and the adapter's speed, so a slow adapter raises the bar.
- **A declared, recoverable failure path for the ECU.** This is the
  governing rule off the bench: a write over a transport that cannot
  guarantee power or connection (mobile above all) is allowed only when
  the plan declares a recoverable interruption (the bootloader survives,
  a re-flash fixes it). Core checks the declaration for presence and
  shape, shows it to the user as the plugin's claim, and the dry run
  exercises it; it cannot itself prove the bootloader survives. Recovery,
  declared and previewed, not prevention, is the safety net where voltage
  and connection cannot be guaranteed.
- **Car-proven status,** earned per transport over time, the VX over
  J2534 first.

The paths this opens, as later releases (the roadmap places them, as
device testing allows), each a transport back end behind the same engine
and gates:

- **A wireless network adapter over WiFi (or Bluetooth):** the phone or
  desktop talks sockets to a smart adapter whose firmware bridges to the
  car, so no operating-system driver is needed and the same path runs on
  Android, Linux, macOS and Windows. A USB-cable adapter instead needs an
  OS driver per platform, the harder, platform-locked route, so the
  wireless adapter is preferred for mobile. macOS and iOS follow when
  device testing permits (iOS has run only on the Simulator).
- **Linux:** reached by the same wireless adapter, and by the planned
  by-path Linux J2534 driver (`j2534:///path/lib.so`, roadmap), which the
  transport-agnostic engine uses with no change.
- **DoIP/ENET (ISO 13400):** a link layer of its own (roadmap), needed
  for newer vehicles that reprogram over Ethernet rather than CAN. Core
  drives DoIP's whole PDUs over the link as it drives them over CAN; the
  capability contract and the gates are unchanged.

None of these is built in phase 4. The point here is that the write
engine does not bake in J2534 or Windows, so each transport is added
behind the gates once it meets the contract and is proven, without
reopening the write path. Havne meets by rule what a curated single-make
service meets by curation: it ships no stock-image database and no
network (ADR-0052 section 10), so a restore is the user's own to hold
(the dump is recommended, not forced, section 4.5), and recovery is a
declared, previewed and dry-run-exercised path rather than a disclaimer.

#### 7.3 Restore and recovery, as operations of their own

The flash is not the only write operation. Two more share its gates:

- **Restore** writes back an image the user picks, a `backups/` copy
  (named by its SHA-256) or the user's own file, not the plugin's data,
  in the `dfi` the plan declares (section 7.4). A plaintext dump
  (`dfi=00`) round-trips as read; a module that takes its image
  transformed (compressed, encrypted, re-checksummed) needs an image
  already in that form, which Havne does not produce (section 7.4). The
  confirmation says which. Writing a copy back is the plan's claim,
  exercised on the simulator (section 7.1), not a guarantee core makes.
- **Recovery re-flash** completes or re-sends an image after a flash
  stopped past the point of no return. After such a stop the module's
  software ids differ from the copy's or cannot be read, so the "current
  dump" rule (section 4.5) would call the good copy stale; while the
  journal's entry is unfinished core skips that match, matches on the
  hardware ids and the journal, and never files a fresh dump of that
  module as an original. The image it writes is the pinned pre-write
  copy.

Both run under the full write envelope, the covering-copy check and the
write confirmation, exactly as the flash does.

#### 7.4 Three artifacts: the dump, the on-wire image, and a distributable file

A dump is not always a flashable file. Three representations a protected
module's flow keeps apart, and so does this record:

- **The dump** is the plaintext bytes read off the ECU (section 4), a
  record and a tune's input. It is the restore's content, not a
  self-sufficient restore: putting it back needs a plan (the sequence,
  any kernel-loader step, the checksum routine), so a bare dump with no
  matching plan cannot be flashed.
- **The on-wire image** is what a write actually sends, in the format the
  plan's `34` declares through its `dfi` (dataFormatIdentifier: the
  compression and encryption nibbles). For `dfi=00` (plaintext,
  uncompressed) a dump round-trips, so a dump is a restore when a plan
  carries the sequence; for any other `dfi` the image must already be in
  that form, and the module's bootloader undoes it.
- **A distributable file** (a tuner's, often an encrypted container) is a
  different representation again, produced by a process outside Havne.

So **Havne writes the plaintext image a plan or the user supplies, in the
`dfi` the plan declares; it does not decrypt or produce a protected
vendor format.** Transforming a dump into a distributable file, or
decrypting one, is the vendor's or tuner's, not core's: core flashes an
image it is given, it is not a tool for forging or cracking a protected
format. The flash plan declares the `dfi` and core sends the image as is.

Phase 4's simulator and example stay single-download plaintext
(`dfi=00`): one payload to one region, which proves the session, unlock,
download, checksum and reset without a kernel loader. The two-stage
kernel-loader shape (an encrypted kernel uploaded to RAM and started,
then the payload) is a real protected-module pattern (section 3.1) that
the single-download plan does not cover; it rides the plugin-supplied
sequence (section 3), with its own simulated bootloader (section 3.3),
not phase 4.

### 8. The simulated ECU, consolidated

One list, since the simulator work spans the sections and is what lets
every rule run in CI with no car:

1. **Security access (section 2):** a fictional seed on `27 01`, a key
   checked against a fictional transform on `27 02`, a lockout (`36`
   then `37` for a delay), a DID gated behind the unlock.
2. **A readable memory region (section 4):** readable by `23` and by
   the `35`/`36`/`37` upload sequence behind the security-access unlock,
   plus a second region answered only in `10 02`, so the user-facing dump
   and its session handling are exercised in CI.
3. **A programming session and writable flash (section 7):** `10 02`,
   the `34`/`36`/`37` download sequence behind the unlock and the
   session, a `31` erase and a `31` check, `11 01` reset, and a dry-run
   mode that adopts a plan's region, block size and routine ids (section
   5), single-download plaintext (`dfi=00`, section 7.4). It is one
   model, reached through `ecuio_sim_ask`: behind the fake
   J2534 DLL on Windows, on `vcan` on Linux, in-process for the dry run
   (section 7.1).
4. **Flash faults and recovery (section 7):** power lost, NRC 78 storms,
   timeouts, a refused block or checksum, a flash interrupted past the
   erase then restored from the copy, the bootloader re-entered.
5. **The fake J2534 DLL (section 5):** a settable `READ_VBATT` for the
   voltage floor, the dropout and the display.
6. **The example plugins:** a car-sized example set already exists from
   ADR-0065; phase 4 adds an example conversation plugin that unlocks
   the simulated ECU's gated DID, and an example write operation that
   flashes the simulated region, both running in CI.

The abstraction's seam is `ecuio_sim_ask` on an unbound sim: the write
services are built once in core's sim, and both transports route to them,
rather than a second ECU modeled in `tests/fake_ecu.h`. Its cost is the
routing (the fake J2534 DLL embeds the unbound sim and forwards each
request) and keeping `ecuio_sim_ask`'s no-bus clock, which ticks per call
(ADR-0046), in step through a multi-frame write.

### 9. Sequencing

Core first, each a branch with the cross-arch suite, as ADR-0065's steps
run. The order follows the dependency in section 1:

1. Tier 2's first slice: WAMR vendored into `third_party/` (which the
   build creates) and wired into the Win32 tree and the warnings-as-errors
   builds, with its SBOM and `THIRD_PARTY_NOTICES.txt` entries; the guest
   ABI, the metering, the trust model, the manifest format 2 and `plugins
   check`; `27`; the simulated seed, key and lockout; a fuzz target for
   the module boundary (as `fuzz_plugin_manifest` is for the manifest);
   how the example's `.wasm` is produced in CI (a toolchain, or a
   committed binary rebuilt and compared); the example conversation plugin
   unlocking the gated DID in CI.
2. Gate 1: the operation-plan shape, the read-transfer machinery, the
   original-copy file and its read-back, the simulated memory region.
   The standalone dump (read and save, no write) ships at this step.
3. Gates 2 and 3: the safety envelope and the legal record, with the
   fake DLL's `READ_VBATT` for the voltage floor.
4. The flash mechanics and the dry run: the download sequence, the erase
   and check, the reset, the one simulated ECU reached through
   `ecuio_sim_ask` (behind the fake J2534 DLL on Windows, on `vcan` on
   Linux), the simulated bootloader and its faults, the dry-run mode, and
   the example write and restore operations in CI.

The car runs (the programming session entered and left with no write,
then a restore of the verified copy as the first flash, section 7.3,
confirmed by a read) are the user's: parked, through a private plugin,
taken only when the user runs them. This record does not build them.

## What stays open

- **Whether a public build carries the write services at all** (option
  3, section 3.2). Where the write path lives is decided (option 4,
  section 3). Whether a public release build enables the tier-3 write and
  flash operations, or compiles them out until a build the user makes, is
  the roadmap's call once the gates are built and tested. Running a
  tier-2 code plugin in a public build is already settled (yes, section
  2.4). This record leaves only the compile-out open.
- **Classic or fast interpreter** (section 2.1): fixed to classic if
  metering is classic-only at the pinned tag (to confirm at the build),
  else measured on the test phone and in the iOS Simulator under the
  phone size budget, no car needed.
- **The exact instruction budget and the delay floor's value**
  (sections 2.2, 2.3): tuned to the simulated key computation and a
  conservative UDS default; the shape (overridable down, never up) is
  fixed here.

## Alternatives considered

- **Five ADRs, one per gate.** One ADR each for tier 2, the three gates
  and the flash. One ADR instead, since these sections share one thesis
  (section 1) and one simulated-ECU contract (section 8); splitting them
  would repeat the spine five times.
- **A signing requirement for a write plugin** (ADR-0052 sections 1 and
  7 required one). Reversed here: a signature guards provenance, not
  safety, and a plugin's danger is bounded by what core lets it send, not
  by who wrote it. The sandbox, the service ceiling and the gates contain
  any plugin, and requiring a signature would defeat the open,
  third-party setup the owner chose. Provenance rides `source` and
  `lineage` without it.
- **One abstract tier-3 framework ADR before the gates.** An
  operation-plan ADR with no operation in it ages badly and forces the
  reader to hold the shape with nothing to apply it to. The shape is
  introduced in gate 1 (section 4.1), where the first operation needs it.
- **wasm3 for the sandbox.** Smaller, but no built-in metering (core
  would hand-roll the instruction guard ADR-0052 section 6 requires) and
  minimal maintenance. WAMR's interpreter meets the iOS rule just as
  wasm3 does and brings metering.
- **A native-code sandbox (a shared library) for tier 2.** Fast on the
  desktop, impossible on iOS, and a buggy module takes the process down.
  Kept for developer builds only, as ADR-0052 has it.
- **Tier 2 carries the flash** (one loose reading of the goal).
  Rejected: tier 2 carries the conversation, tier 3 the transfer
  (section 1). A tier that could send a transfer would put the most
  dangerous service behind the least enforcement.
- **A generic ELM327 for writes.** Refused: it cannot send a multi-frame
  request and hides errors behind its text protocol (section 7.2). J2534
  (the VX) is phase 4's qualified transport, and a qualified network
  adapter can qualify later; a generic ELM327 does not.
- **Folding gate 1 into the flash.** Gate 1 is a read that runs on the
  car once tier 2 unlocks it, before any write exists; it ships and is
  exercised first. Kept as its own section, built and run ahead of the
  write path.

## Consequences

- A third-party code plugin runs on every platform in a public build,
  contained by the sandbox, the service ceiling and the gates, with no
  signing gate: the open plugin setup is real, not example-only.
- Core gains one new dependency in phase 4 (WAMR), not two; Ed25519 is
  dropped, and provenance rides the existing `source` and `lineage`.
- ADR-0052's write-signature requirement (sections 1 and 7) is reversed
  and ADR-0048 item 4 clarified: the write mechanics start in v0.9.0
  behind the gates, proven on the simulated ECU; 2.0 stays the public
  write product. STATUS.md and DISCLAIMER.md change in the same docs
  commit.
- Tier 2 can never send a write, by a compile-time ceiling that is
  independent of unlock state; the most dangerous services run only
  through core's wire, write ceiling and gates (phase 4 from a
  declarative plan; a plugin-supplied sequence later, still through
  core).
- A flash checks `backups/` for a verified, covering copy (matched on
  hardware and software ids) and uses it as the restore; if none exists
  the user dumps first (recommended, one action) or confirms their own
  restore file, and the record notes which. The dump is a first-class
  standalone operation, started on its own from the app or `havne-cli`
  behind a confirmed `27`/`67` unlock, and the recommended path, not a
  forced one. Writing a copy back is a restore operation of its own
  (section 7.3) and the plan's claim, not a guarantee.
- A dump is the restore's content, not a flashable file by itself: core
  writes a plaintext image over the declared `dfi` and does not decrypt
  or produce a protected vendor format (section 7.4), so a restore needs
  both the bytes and a plan, and a transformed image is the user's to
  supply.
- Every write runs inside the safety envelope; an abort is guaranteed up
  to core's point of no return (the earlier of the plan's mark and the
  first write-class service) and named as recovery after it. What core
  enforces and what is the front end's word are split out (sections 2.4,
  3.3).
- Writes are Windows-only in phase 4 (J2534 only), so the phone, Linux
  and macOS read and unlock but do not write. The tier-2 conversation and
  the original-copy read are the cross-platform part, and that read is
  also a standalone dump (read and save the ECU, no write). The write
  engine is transport-agnostic, so the mobile, WiFi and Linux write paths
  follow as later transport back ends behind the same gates (section
  7.2), DoIP/ENET included for newer vehicles.
- The legal and safety exposure is named per feature, at the point of
  action, with one `docs/write-safety.md` to cite.
- Whether a public build carries the write services is left to the
  roadmap once the gates are built, and is stated so rather than decided
  early.
- Everything runs on the simulated ECU in CI before any car, on the
  platform that writes: one transport-independent model (ADR-0046,
  reached through `ecuio_sim_ask`) answers behind the fake J2534 DLL on
  Windows and on `vcan` on Linux (section 7.1), so the seed and key, the
  lockout, the read, the flash, its faults and the restore are all
  fictional and exercised with no hardware.
- The cost: the simulated ECU grows a security challenge, a programming
  session, writable flash and a fault-and-recovery model, which is a
  large fixture to build and maintain; and the write path is complex
  even behind the gates, so the phase is long and sequenced strictly.
