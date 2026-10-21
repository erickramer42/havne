# VEHICLE run sheet — first read-only vehicle session

**Date:** <date>  **Operator:** <initials>  **Second person or posted procedure:** <name / where posted>
**Adapter:** OBDX Pro VX (USB)  **Vehicle:** <year make model>; CAN-era OBD-II, expected 11-bit IDs at 500 kbit/s
**QtTune build:** <commit hash>  **Trees:** build, build-win32 both fresh

Fill in a copy of this sheet as `docs/bench-sessions/<date>.md`. The
no-vehicle BENCH tier is [bench-runsheet.md](bench-runsheet.md); this
sheet picks up at its Phase 4 stop point.

## Purpose

First session against a talking bus. Goals, in order:

1. Requests leave the adapter and an ECU answers them (BENCH prep #2,
   request framing)
2. Replies decode to plausible values through the `obd2` profile
3. Link health behaves as designed on a real bus, ignition off and on
4. Validate or invalidate the talking-bus items in the
   [BENCH prep list](../../STATUS.md#bench-prep) (#2, #3, #4, #5, #6,
   #7, #8)

Record everything, including the session log after every connect. A
session that stops at phase 2 with evidence is worth more than one
that runs to the end undocumented.

## Safety and scope

- **Read-only.** QtTune sends only Mode-01 ("show current data")
  requests, the same read-only service a generic OBD-II scanner uses.
  It has no code path that writes, clears codes or flashes. Read
  [DISCLAIMER.md](../../DISCLAIMER.md) before starting.
- **Parked:** transmission in Park (or neutral), parking brake set.
  Run any engine-on phase outdoors or with ventilation.
- **Laptop power:** on its own battery, not an inverter on the
  vehicle.
- **Abort at once if:** a new warning lamp lights, a gauge or the
  cluster misbehaves, relays click repeatedly, the adapter gets hot,
  or the app crashes. **Abort order:** Disconnect in the app (or close
  it), unplug the adapter, then key off. Write down what you saw before
  anything else.

## Pre-flight (before going to the vehicle)

- [ ] Fresh build of both trees, **Win32 first**, and the app build
      prints "Deploying Win32 J2534 surrogate next to qttune.exe"
      (see [bench-runsheet.md](bench-runsheet.md) pre-flight)
- [ ] Full suite green: every case passes except the env-gated skip
      (current count in [STATUS.md](../../STATUS.md)), plus the
      cross-arch subset
- [ ] Launch from a shell with `qttune.exe 2> sur.log`. The session log
      at `%TEMP%\qttune-surrogate.log` should hold both
      `[surrogate HH:MM:SS.mmm]` and `[sur …]` lines; the redirect is
      a backup. **Copy the session log after every connect**: each
      spawn rewrites it.
- [ ] Know the four link states before you need them (see
      [j2534.md](../j2534.md#session-lifecycle)):
      - no banner: ok;
      - amber "Vehicle not responding": quiet;
      - amber "Requests failing — check the vehicle connection and
        ignition": writes fail while reads are clean, the no-ECU or
        ignition-off case;
      - red "Adapter fault": reads fail.
      The Settings activity log records each transition.
- [ ] Settings picker lists the VX as "x86"
- [ ] Camera or notebook ready

Abort condition: the suite isn't green, or the surrogate isn't
deployed. Stop and fix it at the desk; the vehicle adds nothing.

## Phase 0 — ignition off

**Action:** adapter plugged into the vehicle, ignition **off**. Connect:

    j2534://OBDX Pro - VX

**Expected** (the no-ACK bus seen on the 2026-10-20 bench):
- [ ] Connect succeeds, the filter is accepted (session log
      `OP_START_FILTER … status 0`)
- [ ] The first few writes succeed, then writes fail (`OP_WRITE: status 7`)
      once nothing ACKs them. At the first failed write the link latches
      `QT_ERR_REQUEST_FAILED`, one episode, and stays there: amber
      "Requests failing" banner, and "Link: requests failing
      (1 episode(s))" in the activity log
- [ ] Disconnect cleanly

This state is correct with nothing awake on the bus. Don't debug it.
If an ECU does answer with the ignition off (some stay awake briefly
after key-off), record that instead.

**Evidence:** the session log; the UI state; how many writes succeeded
before the first failure.

## Phase 1 — ignition on, engine off, listen only

**Action:** ignition **on**, engine off. Connect without requests:

    j2534://OBDX Pro - VX?profile=raw&filter=0x7E8

**Expected:**
- [ ] Connect succeeds on a powered bus
- [ ] No writes in the session log (passive mode)
- [ ] Usually no frames: ECUs don't send on 0x7E8 unasked. The link
      goes quiet ("Vehicle not responding") after about 1.2 s, which is
      correct here

**Evidence:** quiet or unsolicited frames, and if frames, their IDs
and payloads verbatim.

## Phase 2 — one PID

**Action:** same ignition state. Connect:

    j2534://OBDX Pro - VX?poll=0x0C

(the `obd2` profile stays in force, so the reply decodes; only PID 0x0C
is requested)

**Expected:**
- [ ] Log page: frames with ID `0x7E8`, DLC 4, payload `410c` + two
      bytes; engine off, RPM reads 0 or close to it
- [ ] LiveView: RPM populated; every other signal shows "—"
- [ ] No banner (link ok)

**Evidence:** a few payloads verbatim; the session log's `OP_WRITE`
and `OP_READ` lines (statuses and frame counts).

## Phase 3 — full scheduler

**Action:** same ignition state. Connect with the bare URI:

    j2534://OBDX Pro - VX

**Expected:**
- [ ] All eight `obd2` signals populate within about one rotation
      (eight PIDs at one request per 100 ms, so ~0.8 s each); a PID the
      ECU doesn't support stays "—"
- [ ] Plausible engine-off values:

| Signal | Expect (engine off) |
|---|---|
| RPM | 0 |
| Vehicle Speed | 0 km/h |
| Engine Load | 0 % or near it |
| Throttle Position | small, steady |
| Coolant Temp, Intake Air Temp | near ambient on a cold engine |
| Fuel Level | roughly matches the gauge |
| Battery Voltage | ~12.0–12.6 V |

- [ ] Values stay bright while they keep arriving; nothing dims

**Evidence:** a screenshot of LiveView; the refresh you observe per
signal (expect ~0.8 s; replies a cycle late are a known item, not a
failure); the Log page's `timestamp_us` deltas against the wall clock
over a minute (BENCH prep #5).

## Phase 4 — engine idling (optional)

Only if phases 0–3 were clean, and with the second person present.

**Action:** start the engine with the app connected on the bare URI.
The connection may drop during cranking; if so, reconnect once the
engine is idling.

**Expected:**
- [ ] RPM ~600–900, Battery Voltage ~13.5–14.5 V, coolant rising over
      minutes

## Phase 5 — ignition cycle (optional)

**Action:** with the bare URI connected and the engine off, turn the
ignition off, wait 10 s, then turn it back on.

**Expected:**
- [ ] Ignition off: the "Requests failing" banner at the first failed
      write
- [ ] Ignition on: the first successful request within ~5 s (the
      backoff caps at 5 s while requests fail), then "Link: recovered"
      in the activity log and values resume

## If it fails, suspect

| Symptom | Suspect |
|---|---|
| Writes fail with the ignition on (`OP_WRITE: status` nonzero) | BENCH prep #2, request framing (6-byte logical message with `ISO15765_FRAME_PAD`); then baud (500 k assumed) or a 29-bit vehicle (prep #4) |
| Writes succeed but no frames arrive | The ECU doesn't answer physical 0x7E0: retry Phase 2 with `?poll=0x0C&txid=0x7DF` (functional request, replies still on 0x7E8); then the flow-control filter (prep #1) |
| `0x7E8` frames on the Log page, but LiveView shows "—" everywhere | Payload shape: stamping already proves the channel matched, so suspect a PCI byte left in-band shifting `[41][PID]`. Record payloads verbatim |
| Values implausible (−40 °C, huge RPM) | Scaling or byte order: temperatures are UINT8 with a −40 offset; 2-byte PIDs are big-endian |
| Link flips between ok and quiet while frames arrive | Unmatched frames interleaving with replies (code review 2026-10-20, W4). Note the frame IDs |
| Slow refresh, replies a cycle late | Known refresh-rate item (20 ms first read vs up to 50 ms ECU response). Measure; don't fix in the session |
| UI freezes on Disconnect | Known: disconnect is synchronous. Wait it out, then note how long |
| App crashes | Abort. Capture the session log and `sur.log` before relaunching |

## Evidence against the BENCH prep list

| Prep item | What settles it |
|---|---|
| #2 Request framing | Phase 2 replies arrive |
| #3 No checksum byte | Replies decode without a trailing byte |
| #4 29-bit addressing | 11-bit IDs expected; any 29-bit traffic is news |
| #5 Timestamp units | Phase 3 `timestamp_us` deltas match the wall clock |
| #6 ReadMsgs statuses | Session log: does any `OP_READ: status 9` line carry `frames` > 0 (`ERR_TIMEOUT` with data)? |
| #7 Struct layout | A correct decode in phases 2–3 implies the layout is right |
| #8 Vendor timeouts | Read timing visible in the session log stamps |

## Teardown

1. Disconnect in the app; the session log shows `OP_DISCONNECT: vendor
   status 0` exactly once
2. Copy the session log and `sur.log`
3. Unplug the adapter, then key off

## Post-session actions

1. Copy this sheet, filled in, to `docs/bench-sessions/<date>.md`, with
   the session logs alongside (the 2026-10-16 session's
   `surrogate-phase*.log` files are the precedent)
2. Walk the [BENCH prep list](../../STATUS.md#bench-prep): mark each
   item CONFIRMED / REFUTED (with evidence) / STILL UNKNOWN
3. Update STATUS.md (phase checklist: "first live, read-only vehicle
   session"), the testing.md tiers and gaps, and the open code-review
   items the session settles (finding 4's surrogate path, W4, the
   refresh rate)
4. Engineering-log entry if a lesson emerged
