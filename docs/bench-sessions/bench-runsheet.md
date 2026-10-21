# BENCH run sheet — first real adapter session

**Date:** <date>  **Operator:** <initials>  **Adapter:** OBDX Pro VX (USB)
**QtTune build:** <commit hash>  **Trees:** build, build-win32 both fresh
**Vehicle connected:** No (phases 1–3) — this sheet's no-vehicle portion is
the BENCH tier; the vehicle session is a separate sheet

## Purpose

First session against a real vendor DLL. Goals, in order:

1. The VX driver loads and routes through the surrogate (x86)
2. It connects an ISO 15765 channel and accepts our filter shape
3. Observable real-device behavior (VBATT, timeout semantics, timestamps)
4. Valid or invalidate every assumption in the BENCH prep list

Record EVERYTHING. A session that fails at phase 2 with captured
evidence is worth more than one that passes undocumented.

## Preliminary results (informal, 2026-10-16)

Ahead of the formal session: the VX completed an unsheeted connect /
disconnect cycle through the surrogate (USB powered, no vehicle) —
phase 1 evidence. Vendor behavior recorded: VX exports all 14
functions, undecorated, spec `Ioctl` spelling; opens with USB power.
The VT (driver installed, adapter absent) refuses `PassThruOpen`
with vendor status 34 in seconds. The formal session below still
owns phases 2–3 (filter shape, VBATT, quiet-bus ReadMsgs).

## Pre-flight (before touching the adapter)

- [ ] Fresh build of both trees, **Win32 first** (the app build copies
      whatever surrogate `build-win32` holds at that moment):
      cmake --build build-win32 --config Release --target qttune-j2534-surrogate
      cmake --build build --config Release
- [ ] The build output shows "Deploying Win32 J2534 surrogate next to
      qttune.exe". If it doesn't, re-run `cmake -B build` and rebuild:
      without the copy, an x86 adapter fails to connect with "failed
      to load transport library"
- [ ] Full suite green: every case passes except the 1 env-gated skip
      (current count in STATUS.md)
- [ ] `qttune.exe` runs and the Settings picker lists the VX with
      architecture label "x86" (discovery smoke — already verified once;
      re-confirm on today's build)
- [ ] Camera or notebook ready for evidence capture
- [ ] Adapter firmware/driver version noted (from Device Manager or the
      vendor app, if installed)

Abort condition: picker doesn't list the VX → discovery regression,
stop and file it; nothing below is meaningful.

## Phase 1 — load and open, no vehicle

**Action:** connect the VX by USB. Enter the URI manually:

    j2534://OBDX Pro VX

(do NOT append ?filter= or ?poll= yet — isolate load from connect
from filter, in that order)

**Expected:**
- [ ] Connection succeeds, no error dialog
- [ ] A surrogate process (qttune-j2534-surrogate.exe, x86) appears in
      Task Manager and dies when you disconnect
- [ ] Disconnect cleanly from the Settings page; process exits with it

**Evidence to record:** success/fail, surrogate process lifetime
observation, any error text verbatim.

**If it fails, suspect (in order):**

| Symptom | Suspect |
|---|---|
| QT_ERR_LOAD_FAILED, fast | PE sniff or export resolution; exit codes 4/5 from j2534.md §Surrogate |
| Handshake timeout (8 s) then fail | Pipe/spawn path — check for AV interference or blocked ports |
| Crash of the app process | Capture crash address; do not guess, bisect |

## Phase 2 — connect and quiet-bus observations, no vehicle

**Action:** same URI, now with the filter parameters but no poll:

    j2534://OBDX Pro VX?filter=0x7E8

**Expected:**
- [ ] Connect succeeds
- [ ] The FLOW_CONTROL filter registration is ACCEPTED — this is
      BENCH prep assumption #1 (8-byte mask/pattern/FC shape) getting
      its first real-world test
- [ ] No frames arrive (nothing on the bus)
- [ ] The read loop spins quietly without errors or callback storms

**Evidence:** how long it ran (aim for ≥2 min), CPU load of the app
(worker spin rate), any error surfaced.

**If it fails, suspect:**

| Symptom | Suspect |
|---|---|
| Filter registration rejected | Prep #1 — the fc_len==mask_len workaround shape. Capture the exact vendor status code |
| Connect itself fails | Prep #8/#2 — baud or protocolID conformance |
| App crashes on quiet bus | ReadMsgs status path (Issue #11). Statuses are split since 2026-10-19 (`ERR_TIMEOUT` → vehicle timeout, `ERR_BUFFER_EMPTY` → no frame, anything else → hard failure); record which status the vendor returns for an empty read, from `%TEMP%\qttune-surrogate.log` or the debugger |

## Phase 3 — VBATT (first real data)

**Action:** same session; if the Settings UI has no VBATT surface,
note "surface absent" and defer to a follow-up. If you add a quick
manual probe via the loader test binary, do it in a scratch branch,
not this session.

**Expected:** READ_VBATT returns something in the 0–15000 mV range
(USB-powered, no OBD connector — possibly 0 or ~5000 depending on
the device's supply arrangement; an unexpected-but-plausible value is
NOT a failure, it's a data point).

**Evidence:** value, and whether the IOCtl succeeded at all (a
strict vendor refusing READ_VBATT unplugged would be notable).

## Phase 4 — STOP POINT (no vehicle)

Everything above is the BENCH tier as scoped. Do NOT plug into a
vehicle on this sheet — that session (parked, ignition ON, engine OFF,
read-only URIs in this order: `?profile=raw&filter=0x7E8` to listen
with no requests, then `?profile=raw&filter=0x7E8&poll=0x0C` for one
PID (or `?poll=0x0C` alone, which keeps the `obd2` decode for that
PID), then a bare `j2534://` for the `obd2` scheduler with decode) is a
separate sheet with its own abort conditions and a second operator
or at minimum a posted procedure:
[vehicle-runsheet.md](vehicle-runsheet.md).

## Post-session actions

1. Copy this sheet, filled in, to docs/bench-sessions/<date>.md
2. Walk the BENCH prep list in STATUS.md and mark each assumption:
   CONFIRMED / REFUTED (with evidence) / STILL UNKNOWN
3. Refuted items become new known issues or immediate fixes, per
   severity — the fc_len rule relaxation (prep #1) is the most likely
   candidate
4. Update docs/testing.md "Gaps": "No real vendor DLL has been
   loaded" flips the moment phase 1 passes
5. Engineering-log entry only if a lesson emerged (the bar: would
   past-you have avoided a real cost by reading it?)