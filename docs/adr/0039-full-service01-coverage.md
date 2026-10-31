# ADR-0039: Full Service 01 coverage in one set, paced by rate classes

- **Status:** Accepted. §1–§7 built on 2026-10-31 (the set, the rate
  classes, Live hiding what a car lacks, the fakes, imperial units),
  verified on the fakes; not yet on a car (BENCH prep #30). Appendix A
  not yet built.
- **Date:** 2026-10-30.
- **Builds on:** [ADR-0013](0013-protocol-profiles.md) (profiles chosen
  by the URI), [ADR-0015](0015-request-engine.md) §1 (the rotation),
  §4 (the probe) and its "still open" list, [ADR-0016](0016-signal-definition-layout.md)
  (the def's layout), [ADR-0017](0017-session-recording.md) (recordings
  keep the wire), [ADR-0024](0024-decode-on-the-worker.md) (decoding in
  core, on the worker), [ADR-0037](0037-reading-trouble-codes.md) §4
  (the freeze frame decoded with `obd2`'s defs).
- **Amends:** [ADR-0015](0015-request-engine.md) §1 and its "still
  open" list, with dated notes: the rotation runs two rate classes, and
  a Service 01 request may carry several PIDs (Appendix A), whose reply
  matches by its first record's PID.

## Context

The `obd2` set decodes ten Service 01 PIDs, each a single field.
`tests/fixtures/obd2/service01.csv` holds J1979's whole Service 01
table, one row per field: 174 PIDs, of which 92 are linear (raw ×
scale + offset), 130 fields between them; the rest are bitmaps (the
probe's), bit fields, enumerations, the freeze frame's DTC, and
layouts the sources leave open.

**What the table rests on.** Its rows come from Wikipedia's table,
each linear row checked against CSS Electronics' OBD2 DBC (v4.3). On
2026-10-30 the 91 linear rows of PIDs 04–4E (67 PIDs) were checked
against SAE J1979 (2002) itself, its Appendix B: every scale, offset,
sign and range agrees. J1979 (2002) reserves 4F–FF, so the 39 linear
rows past 4E (25 PIDs) rest on the two secondary sources only. The
check found two things the table doesn't carry:

- PIDs 06–09 (the fuel trims) carry a second byte, banks 3 and 4, on
  an engine that has them; the table gives each one byte.
- PIDs 14–1B, 24–2B and 34–3B name a sensor by position, 1 to 8. Which
  bank and sensor a position is depends on whether the ECU supports
  PID 13 (two banks of four) or 1D (four banks of two).

**What the test car answers.** In the second vehicle session
([session sheet](../bench-sessions/2026-10-25-vehicle.md), its
surrogate logs) one ECU (`0x7E8`) answered six bitmaps, 00 to A0,
marking 53 data PIDs supported: 43 linear (49 fields), 4 bit fields
(01 03 13 41), 2 enumerations (1C 51), 3 complex (6D 9D 9E) and 02.
It lacks 5C, the one `obd2` PID the probe dropped there. Nine of its
linear PIDs lie past 4E (55 56 59 62 63 67 68 8E A6).

**What a PID costs.** The engine sends one request per PID per pass,
two defs on one PID sharing it (`ecuio_engine_add` answers
`duplicate`). On the test car it ran 26–28 requests a second
(ADR-0015's appendix), so one flat rotation refreshes each PID about
every (kept PIDs / 27) seconds: the ten `obd2` signals about every
0.4 s, the car's 43 linear PIDs about every 1.6 s. Engine speed every
1.6 s is too slow for the Dashboard's tachometer or a logged pull, and
far faster than a coolant temperature or a distance counter needs.

The cost is the schedule's, not the set's. A second profile (a fast
`obd2` beside a slow `obd2-full`) would make the user choose between a
fast tachometer and the catalyst's temperature, and every page, test
and document say which set it means. One set with a schedule that
knows which signals move fast avoids both.

## Decision

### 1. One set: `obd2` grows to the whole linear table

The `obd2` set holds a def for every linear field of the table, 130 in
all. There is no second Service 01 profile. The probe drops what a car
lacks (ADR-0015 §4), so a session asks only the PIDs the car has: 43
on the test car.

### 2. Linear fields only

Bit fields, enumerations and the complex PIDs are out of this record:
each needs a decoder kind or a name table the def ABI doesn't have,
and the readiness and monitor PIDs (01, 41) are already read into the
diagnostics report (ADR-0037). The table keeps their rows, so a later
record starts from a checked list.

### 3. Two rate classes, in the engine

Each Service 01 request is **fast** or **slow**:

- **Fast:** what a driver or a logged pull watches change within a
  second: 04 (load), 06 and 08 (short-term trims), 0A, 22, 23 and 59
  (fuel pressure), 0B (MAP), 0C (rpm), 0D (speed), 0E (timing), 10
  (MAF), 11 (throttle), 24–2B and 34–3B (the wide-range sensors'
  lambda), 44 (commanded lambda), 49 (pedal D). On the test car, 14 of
  its 43.
- **Slow:** everything else: temperatures, barometric and evaporative
  pressures, the voltage, long-term trims, the narrow-band sensors,
  commanded throttle, torque, counters and distances.

Temperatures stay slow even where they're watched closely (coolant,
IAT and IAT2 under boost, ambient): they move over seconds, and with
several PIDs per request (Appendix A) the slow class refreshes in
under a second. Fuel pressure is fast because a falling rail pressure
under load is what a pull needs to show.

**The rule is a ratio, not a timer.** On each channel the rotation
sends `ECUIO_ENGINE_FAST_PER_SLOW` (2) fast requests, then one slow
one; within a class, set order, round-robin, as the rotation is today.
A class with nothing left to ask gives its turns to the other, so a
car that keeps only fast PIDs (or only slow ones) runs today's flat
rotation. The pace follows whatever the ECU sustains, as the engine's
does today (a request when the last reply is in, the 20 ms floor), and
stays deterministic for the tests, which pass time in.

On the test car that gives about 18 fast requests a second over 14
PIDs and 9 slow ones over 29: each fast signal about every 0.8 s, each
slow one about every 3.2 s: estimates from the measured pace, until
a car run measures each class.

Fuel pressure is four PIDs, the forms J1979 gives it (gauge, relative
to the manifold, the high-pressure gauge range, absolute). A car
answers the forms its fuel system has; the test car answers 0A, 23 and
59, and 23 and 59 read one value there
([2026-10-31-elm327-vehicle](../bench-sessions/2026-10-31-elm327-vehicle.md)).
So the fast list has a companion, pairs of fast PIDs that can read one
sensor (`ECUIO_OBD2_DUPLICATE_PIDS`: 23 and 59): when a channel's
bitmaps list both, the second goes slow, judged at each walk's end; a
car that lists one keeps it fast, and a session without the probe
keeps both. On the test car that leaves 13 of its 43 PIDs fast.

**The class lives in core, not the def ABI.** The `obd2` set's file
holds the fast list beside its defs, and request derivation
(`ecuio_engine_derive`) marks each request it builds from that set;
`ecuio_request` is internal, so no public struct changes. A set
registered from outside (`ecuio_profile_register`) gets one class, the
flat rotation it has today. If an outside profile ever needs classes,
that's when the class moves into the def's reserved room, with its
validator code.

### 4. IDs and names

- **IDs:** one rule in the public `signal_sets.h` in place of a macro
  per field, `ECUIO_OBD2_PID_SIG_ID(pid, field) = 11000 + pid * 8 +
  field` (field: the row's order within its PID, 0–4), so 11000–13047:
  16-bit, clear of the mock's 1001–10001 and `uds-generic`'s 20000 and
  up. The ten defs `obd2` has today keep their IDs (7001–7010) and
  names, so every existing recording decodes as before, the freeze
  frame's values keep their IDs, and the Dashboard's gauges, which
  name those IDs, are unchanged.
- **Names are unique within the set, and fit the def's 31 bytes.**
  `signals.csv` keys each value by its name (`host_us,signal,value,unit`),
  and so do Live and the recordings' diff. The table repeats nine
  names (24–2B and 34–3B, 32 and 54) and 27 run past 31 bytes, so the
  display names are written for the set, not copied: "O2 S1 lambda
  (V)" and "O2 S1 lambda (mA)", "Catalyst temp B1 S1", "Evap vapor
  pressure" and "Evap vapor pressure (wide)". A test checks both rules
  for every built-in set.
- **Units** are the table's unit column, which already uses the sets'
  strings. The ones `kImperial` lacks get rows where a conversion is
  usual (Pa to inH2O, L/h to gal/h, N·m to lb·ft); λ, V, mA, s, min, %
  and ° stay as they are.

### 5. Defs written by hand, checked against the table

The defs are a hand-written table, as `obd2`'s are today, not
generated from the CSV. The CSV is what the defs are checked against;
generating one from the other would make
`Obd2TableTest.EveryObd2DefMatchesItsRow` compare the CSV with itself.
A new case checks the set has exactly one def for every linear row,
and that every fast PID is one the set defines.

### 6. Live shows what the car has

Live lists every def of the profile, and an unsupported PID shows `—`:
on the test car about 80 rows of it. Live hides a def whose PID the
ECU marks unsupported, and says how many it hid and why in one line
under the list. The bridge reads that from the channel support
report's bitmaps (`pid_bitmap`, with `ranges` saying which were
answered), not its list of dropped requests, which holds the first
`ECUIO_SUPPORT_MAX_DROPPED` (8) only. A PID in a range the ECU never
answered stays listed; a def hidden this way comes back if a rescan
finds its PID. This is part of the change, not a follow-up: `obd2` is
the default profile for `j2534://` and `elm327://`.

A PID the bitmaps list can still come without one of its fields: the
test car answers 55 and 56 with bank 1's byte alone, where J1979 gives
each a bank 3 byte too
([2026-10-31-elm327-vehicle](../bench-sessions/2026-10-31-elm327-vehicle.md)).
Core knows it, because its decode of that field runs past the reply
(`ECUIO_DECODE_ERR_PAST_PAYLOAD`), so core reports it:
`ecuio_session_get_absent_signals` (`profile.h`) lists each def whose
key matched `ECUIO_ABSENT_AFTER_FRAMES` (3) frames that ended before its
field while none carried it, and drops it once one does. The bridge
reads the list on its tick and Live hides those rows beside the
bitmaps' (its note says how many the ECU doesn't send); `havne-cli
stream --values` writes it as an `absent` line. The judgment is core's
rather than the bridge's, so a program on the C ABI gets it too; it
runs on the decode for a values callback, so a session with none
judges nothing.

### 7. The fakes

The fakes' ECU (`tests/fake_ecu.h`) answers every linear PID with a
plausible value and its bitmaps chain to A0, so the whole set runs end
to end against the fake J2534 DLL and ELM327 and the probe drops
nothing there. PID 7F, which tests reach today only with `probe=0`
because the chain stops at 40, becomes reachable through the chain;
the tests that relied on the probe dropping it change with it.

### 8. Out of this record

- **Banks 3 and 4** of PIDs 06–09, and **bank and sensor names** from
  PIDs 13 and 1D.
- **The freeze frame** keeps its ADR-0037 §4 rules: the PIDs its
  bitmap lists that the set defines, at most `ECUIO_DIAG_MAX_FREEZE`
  (16). A larger set lets it decode more of a frame's PIDs, up to that
  cap.
- The bit-field, enumeration and complex PIDs (§2).
- **Signals no Service 01 PID carries:** transmission fluid and
  differential temperatures, oil pressure, knock retard. They are
  manufacturer DIDs (the roadmap's first manufacturer signal set), read
  meanwhile with `uds-generic` where a DID is known. Boost is MAP (0B)
  less barometric pressure (33), a computed signal (the roadmap's
  computed signals).
- The other ways to a faster pass (Appendix B).

## Alternatives considered

- **Two profiles,** `obd2` kept at ten and an `obd2-full` beside it.
  No engine change, but the user picks between a fast tachometer and
  full coverage, and every page and document says which. Rejected
  (Context).
- **Rate classes in the def ABI now.** It lets outside profiles class
  their own defs, but grows the def struct and the validator for a
  need no outside profile has. Deferred (§3).
- **Periods instead of a ratio** (fast every pass, slow every 3 s).
  The share each class gets would then depend on the ECU's pace: a
  slow ECU spends its time on slow PIDs. Rejected.
- **Grow the set without classes.** Every signal at the slow pace on
  a car with many PIDs. Rejected.
- **Generate the defs from the CSV** at build time. It removes the
  test's independence (§5). Rejected.

## Consequences

- `obd2`'s pace changes for every session. On the test car rpm goes
  from about 0.4 s to about 0.8 s, and the temperatures to about 3.2 s,
  until several PIDs per request (Appendix A), built with the
  classes, brings the fast class well under today's figure.
- The public header gains one macro and no new types: no ABI change.
  The new IDs are append-only like every def ID.
- Recordings hold more signals; existing ones replay and decode as
  before, since decoding works frame by frame and the ten keep their
  IDs and names.
- The tests that pin the rotation's order
  (`Obd2SchedulerRotatesPidsInSetOrder` and its neighbors) state the
  class rule; new engine cases pin the ratio, a class giving its turns
  to the other, and set order within a class, each mutation-checked.
- The fakes' ECU answers about 90 PIDs, and
  `Obd2TableTest.TheFakeEcuAnswersEachSupportedPidWithItsLength`
  covers them all.

## Appendix A: several PIDs per request

Decided with §§1–7 and built after them, once a car run has measured
the class rule, so packing is measured against it.

**What J1979 allows** (2002, §6.1.2.3–6.1.2.4, Tables 123 and 124): a
Service 01 request carries one to six PIDs; the ECU answers with a
record (`PID`, its data bytes) for each it supports, in any order;
PIDs it doesn't support are left out, with no negative reply; an ECU
supporting none of them sends nothing. The support bitmaps have their
own request (Table 121), up to six bitmap PIDs in one.

**Requests.** The engine packs up to six kept PIDs of one class into
one request, `01 p1 … p6`, in set order: on the test car, the fast
class in three requests and the slow one in five. Only PIDs the probe
found supported are packed (a PID the bitmaps never covered goes
alone), and a bitmap PID is never packed with a data PID. The walk
itself may ask up to six bitmaps at once (`01 00 20 40 60 80 A0`),
which cuts the probe's round trips at connect. `ecuio_request` grows
to hold seven bytes; it is internal.

**Matching.** A positive reply (`41`) whose first record's PID is one
of the outstanding request's matches it. A PID the request asked that
the reply leaves out has no value that pass: it isn't dropped, since
the bitmaps said it's supported, and the probe stays the only thing
that drops a PID.

**Decoding: the wire stays as it is.** The transports send and
dispatch frames as they do today, and `frames.csv` records the reply
as the ECU sent it (ADR-0017). Core's decode step (ADR-0024) splits a
Service 01 positive reply into its records, using J1979's data byte
count for each PID (a table in core, checked against the CSV's
`bytes` column by `Obd2TableTest`), and decodes each record as if it
were its own reply, so every def's mux key and start byte stay as they
are. A record whose PID has no known length ends the split: the rest
of that reply goes undecoded, and the count is published. Replay
decodes a recorded multi-PID reply the same way, since the split is in
decode, not in a transport.

**Fallback.** An ECU that answers a packed request with nothing, with
a negative reply, or with only its first PID, three times in a row,
puts its channel back to one PID per request for the session, and the
support report says so (a new `ECUIO_SUPPORT_*` flag, append-only).
The fakes' ECU answers packed requests, and a fake knob makes it
answer only the first PID, so the fallback is tested through J2534
and ELM327.

**The adapters.** J2534 adapters do the ISO-TP framing of a longer
reply; ELM327's response-count digit (`elm327_response_count`) counts
frames, so the engine passes the expected reply length (from the
byte-count table) and the transport turns it into frames: one Single
Frame up to 7 bytes, else a First Frame and its Consecutive Frames.

**What it should buy.** On the test car, at roughly the same requests
a second (each reply a few frames longer), the fast class's 14 PIDs in
three requests about every 0.2 s, and the slow class's 29 in five
requests about every 0.8 s. An estimate, to be measured on the car
through both adapters.

## Appendix B: other ways to a faster pass

Considered beside Appendix A, none decided:

- **Where the time per request goes.** The test car answered in 19 ms
  (median) while the engine ran a request every 37 ms or so: the floor
  (20 ms), the surrogate's round trip and the worker's read cadence
  take the rest. v0.3.5's benchmark (latency per PID, P50 to P99.9)
  measures each part first; lowering the floor (ADR-0015's still-open
  value) comes after that, not before.
- **Several frames per Service 02 request** (J1979 lets a freeze-frame
  request carry PID and frame pairs): one request in place of a
  freeze frame's sixteen at connect. Small: it runs once per read.
- **Demand-driven classes** (what's on screen or plotted goes fast).
  It couples the engine to the UI, and a recording wants every signal
  at a known pace whatever the screen shows. Not planned.
- **Rate classes per def in the ABI** (§3), for outside profiles.
