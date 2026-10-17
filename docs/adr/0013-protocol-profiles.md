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
