# BENCH run sheet — real adapter, no vehicle

**Date:** <date>  **Adapter:** OBDX Pro VX (USB)
**Machine:** <the dev machine, or the laptop that goes to the car>
**QtTune build:** <commit hash>  **Trees:** build, build-win32 both fresh
**Vehicle connected:** No. This is the BENCH tier; a vehicle session
has its own sheet, [vehicle-runsheet.md](vehicle-runsheet.md).

Fill in a copy as `docs/bench-sessions/<date>.md`. The sessions so far
are [2026-10-16](2026-10-16.md), [2026-10-20](2026-10-20.md),
[2026-10-22](2026-10-22.md) and [2026-10-25](2026-10-25-bench.md) (a
laptop); the last two ran this procedure, and their results are the
expectations below, except for phase 3's two `uds-generic` URIs,
which haven't run at the bench: on the no-ACK bus they should fail
their writes the same way.

## Purpose

A real vendor DLL and adapter with nothing else on the bus. Goals, in
order:

1. The driver loads and routes through the surrogate (x86)
2. It connects an ISO 15765 channel and accepts the filter shape
3. Every URI shape a vehicle session uses connects on this build and
   machine, and the evidence pipeline (recording, payload logging,
   kept session logs) runs once
4. The adapter's behavior with nothing to acknowledge its frames is
   as recorded; a difference is a finding

There's no ECU simulator, so this is all the desk can check before a
vehicle. A session that stops at phase 1 with its evidence is worth
more than one that runs to the end undocumented.

## Pre-flight (before touching the adapter)

- [ ] Fresh build of both trees, **Win32 first**
      ([development.md, "Build configurations"](../development.md#build-configurations)
      has the commands), and the app build
      prints "Deploying Win32 J2534 surrogate next to qttune.exe". If
      it doesn't, re-run `cmake -B build` and rebuild: without the copy,
      an x86 adapter fails to connect with "surrogate process failed: no
      surrogate (ecuio-j2534-surrogate.exe): none next to the app"
- [ ] `scripts/verify.sh` prints four `ok` lines, the cross-arch one
      reading `0 skipped` ([development.md](../development.md#running-tests))
- [ ] The VX driver is installed, and Settings' picker lists the VX as
      "x86". Close the vendor app and any other J2534 tool: one process
      at a time can hold the device
- [ ] Tick **Record sessions** before the first connect, so each
      connect writes a recording and the surrogate logs every request
      and reply ([ADR-0017](../adr/0017-session-recording.md))
- [ ] Note the driver's version from its installer or the vendor app;
      the adapter's own versions come from the first connect (phase 1)

Abort condition: the picker doesn't list the VX. Stop and file it as a
discovery problem; nothing below is meaningful.

## Phase 1 — load, open, requests on a bus with no other node

**Action:** connect the VX by USB, pick its row, and Connect the bare
URI. Leave it about two minutes:

    j2534://OBDX Pro - VX

**Expected** ([2026-10-22](2026-10-22.md)):
- [ ] Connect succeeds; the session log shows `OP_OPEN: vendor status
      0`, `OP_CONNECT` 0 and `OP_START_FILTER: type 3, status 0`
- [ ] The activity log's "Session:" line follows "Connected:". Copy it
      into the sheet: "x86 DLL, through the surrogate", the battery,
      and the adapter's firmware, DLL and API versions, or "no version
      reading" ([ADR-0019](../adr/0019-session-info.md))
- [ ] Three writes succeed, then the fourth blocks about 0.5 s and
      fails (`OP_WRITE: status 7`), as does every later one: nothing
      acknowledges the frames. The three are the PID-support probe's
      first bitmap (`01 00`), the rotation's first PID (`01 04`) and
      identification's first item (`09 02`), in that order
- [ ] The activity log reads "ECU 0x7E8: no answer to the
      supported-PID query; 10 requests, none dropped" within about
      0.3 s (nothing answers the bitmap, and silence drops nothing),
      then amber "Requests failing" and "Link: requests failing
      (1 episode(s))" within about a second
- [ ] The probe interval grows to about 5.6 s as the backoff reaches
      its 5 s cap
- [ ] Task Manager: `qttune.exe` and the 32-bit
      `ecuio-j2534-surrogate.exe` both under 1 % CPU
- [ ] Disconnect: `Disconnected:` within about 0.5 s, one
      `OP_DISCONNECT: vendor status 0`, and the surrogate process gone

**If it fails, suspect:**

| Symptom | Suspect |
|---|---|
| "PassThruOpen returned status 34 (0x22, vendor-specific)" | The adapter isn't attached (the VX and the VT both answer 34 then) |
| "built from other shared headers" | A stale `build-win32`: rebuild both trees |
| "surrogate process failed: no surrogate" | The surrogate missing next to `qttune.exe` |
| "failed to load transport library", fast | The DLL didn't load in the surrogate, or lacks an export: the exit codes in [j2534.md](../j2534.md#surrogate) |
| A handshake timeout (8 s) | The child never started: security software holding a newly built exe, or a blocked spawn |
| Filter registration rejected | BENCH prep #1, the 4-byte filter shape. Record the vendor status |
| The app crashes | Capture the session log; don't guess, bisect |

## Phase 2 — listen only

**Action:** about 30 s:

    j2534://OBDX Pro - VX?profile=raw&filter=0x7E8

**Expected:**
- [ ] No writes in the session log
- [ ] Every read returns `ERR_BUFFER_EMPTY` (status 16) after waiting
      out its 100 ms timeout; amber "Vehicle not responding" after
      about 1.2 s

## Phase 3 — the vehicle session's other URIs

**Action:** connect each for 15–30 s:

    j2534://OBDX Pro - VX?poll=0x0C
    j2534://OBDX Pro - VX?profile=uds-generic&did=F180-F1FF
    j2534://OBDX Pro - VX?profile=uds-generic&did=F188:text

**Expected:**
- [ ] Each connects and goes to "Requests failing" as in phase 1: three
      writes accepted, then 0.6 s apart growing to the 5.6 s cap. (On a
      bus that answers, the next request goes out as soon as the reply
      is in.) `?poll=` asks only its PID, so no probe or identification
      line; the `uds-generic` connects have no Service 01 requests, so
      no bitmap requests either
- [ ] One recording folder per connect: `events.log` holding the URI,
      "surrogate frame logging on" and the link transition;
      `frames.csv` its header row only (no frame reaches the bridge);
      `signals.csv` its byte-order mark and header
- [ ] Each connect's session log kept as
      `ecuio-surrogate.<YYYYMMDD-HHMMSS-mmm>.log`, stamped with its
      last line

## Phase 4 — VBATT

Each connect reads the battery once (`READ_VBATT`), and the activity
log's "Session:" line shows it.

- [ ] Record the battery part of phase 1's "Session:" line: a
      reading, or "no battery reading". On USB power alone there is no
      vehicle battery to measure, so either is an observation, not a
      failure; the vehicle run sheet compares it with a powered
      connector

## Stop point

Nothing above connects to a vehicle. A vehicle session follows
[vehicle-runsheet.md](vehicle-runsheet.md), with its own safety rules
and abort conditions.

## Post-session actions

1. Copy the session logs and this sheet, filled in, to
   `docs/bench-sessions/<date>.md` and a log file beside it. Scrub
   them first: no names or account names, and no local paths
   (`%TEMP%` and `C:\Users\…` paths carry the account name)
2. Walk the [BENCH prep list](bench-prep.md) and mark what
   the session confirms or contradicts, with evidence
3. A contradicted item becomes a known issue or a fix, by severity
4. Update the testing.md BENCH tier and gaps, and STATUS.md
5. An engineering-log entry only if a lesson emerged (the bar: would
   past-you have avoided a real cost by reading it?)
