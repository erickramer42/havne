# CAR run sheet: both ECUs in one session, the recorders, and a second listener

**Havne build:** <commit hash> (the release's candidate, laptop and phone)
**Date:** <date>
**Adapters:** OBDX Pro VX (USB, the laptop), OBDLink MX+ (Bluetooth, the
test phone), optional Innomaker USB2CAN (`can0` in WSL); an OBD
Y-splitter
**Vehicle:** "the test car" (never its make, model or year); CAN OBD-II,
11-bit IDs at 500 kbit/s; the engine ECU on `0x7E0`/`0x7E8`, a second
ECU on `0x7E1`/`0x7E9` ([2026-11-01-vehicle](2026-11-01-vehicle.md))

Fill in a copy as `docs/bench-sessions/<date>-two-ecu-car.md`. Every
request here is a read; nothing is cleared.

## Purpose

1. **Two ECUs in one session,** through both adapters: each ECU's
   requests wait only for their own replies, as the fakes show
   (docs/testing.md: two channels at once are tested against the fake
   only)
2. **The recorder on a car** ([ADR-0049](../adr/0049-recordings-in-core.md)),
   in the laptop's app and the phone's: each recording opens in the
   viewer and passes `havne-cli info`'s check
3. **The MX+ listening beside another tester** on the Y-splitter
   (`AT MA` while the VX asks; BENCH prep #19)
4. **Two recordings of one bus on the wall clock:** how far apart the
   same reply lands in two adapters' recordings, aligned on their UTC
   starts (ADR-0049 section 5)

## Pre-flight (at the desk)

- [ ] Both apps on the build above; **Record sessions** on in each
- [ ] `vehicle.sh mark two-ecu` on the laptop
- [ ] **Warm up `havne-cli`** after any rebuild, on the laptop: first
      `havne-cli devices`, then a 5 s session,
      `havne-cli stream "j2534://<path>/build-win32/tests/Release/fake_j2534.dll" --seconds 5 < /dev/null`
      (or the VX's URI once it's plugged in). Antivirus can hold the
      first connect of a newly built surrogate (exit 3, the pipe broken
      at once). `havne-cli` starts the Win32 tree's surrogate, a
      different file from the app's copy, so warming up the app doesn't
      cover it
- [ ] The phone paired with the MX+ (its `bt:` address comes from the
      phone; it is `<BT address>` in the sheet)
- [ ] The engine ECU's DIDs to ask: one or two from the 2026-11-01 VX
      scan's D800-DFFF finds that aren't identification (not F18x)
- [ ] Optional Phase 5: the USB2CAN's **terminator out** (the car's bus
      is terminated), `usbipd` ready, `can0` to come up listen-only

## Phase 1: the second ECU's DIDs (engine idling, the VX alone)

The second ECU answers `01 00` and refuses `22 F190`; which DIDs it
answers is unknown. In Git Bash on the laptop, one block at a time (a
session scans at most 1,024 DIDs):

    V="j2534://OBDX Pro - VX"
    Q="profile=uds-generic&scan=first&identify=0&diagnose=0"
    for b in F180-F1FF D800-DBFF DC00-DFFF; do
      havne-cli stream "$V?$Q&did=7E9:$b" --until-scanned --seconds 120 < /dev/null > scan-7E9-$b.jsonl
    done

- [ ] Each block's last ECU line: asked, found, refused; every refusal
      `7F 22 31` or another code (note which)
- [ ] The DIDs found: pick one or two for Phase 2 that aren't
      identification (F18x replies are serials: scrub)
- [ ] If none is found, Phases 2 and 3 can't pair two ECUs under
      `uds-generic`: note it, and run Phase 4 on

## Phase 2: both ECUs through the VX, in the laptop's app (engine idling)

Settings, the URI field:

    j2534://OBDX Pro - VX?profile=uds-generic&did=<engine DID>,7E9:<second ECU's DID>&identify=0&diagnose=0

Connect, two minutes on Live, then Disconnect.

- [ ] Live's rows for both DIDs update with values (raw-byte DIDs show
      as text, with no tick box to plot); the activity log's ECU lines
      name both `0x7E8` and `0x7E9`
- [ ] No health line; neither ECU's rate falls to the 100 ms wait (each
      waits only for its own reply)
- [ ] Recordings: the new folder opens in the viewer, and its info
      lists both DIDs' signals; `havne-cli info "<folder>"` lists no
      findings but what the run explains; `frames.csv` holds rows from
      both `0x7E8` and `0x7E9`

## Phase 3: both ECUs through the MX+, in the phone's app (engine idling)

The same URI with `elm327://bt:<BT address>` for the adapter, from the
phone's Settings. Two minutes, then Disconnect.

- [ ] The same three checks as Phase 2, on the phone (the viewer on the
      phone; `info` after `phone.sh pull`)
- [ ] The rates beside the VX's: one command at a time for the whole
      adapter (ADR-0026), so each ECU's turn waits for the other's

## Phase 4: the MX+ listening while the VX asks (BENCH prep #19)

The VX and the MX+ on the Y-splitter. The laptop's app on
`j2534://OBDX Pro - VX` (`obd2`), the phone's on
`elm327://bt:<BT address>?profile=raw` (no requests: it listens with
`AT MA`), both recording. Start the phone first, then the laptop; a
minute idling, then a few blips to 3,000 r/min; stop the laptop, then
the phone.

- [ ] The phone's Log hears the ECU's `0x7E8` replies to the VX, each
      reassembled. `AT MA` keeps the setup's receive filter, so the
      VX's `0x7E0` requests and its Flow Control frames go unheard
      ([elm327.md](../elm327.md#passive-listening))
- [ ] `BUFFER FULL`, if it comes, flags the next frame
      `ECUIO_FRAME_FLAG_OVERFLOW` and the listening goes on
- [ ] The VX's session as it runs alone: no health line, the same
      request rate
- [ ] **The wall clock:** in the laptop's viewer, open the VX's
      recording and compare the phone's (after `phone.sh pull`),
      aligned on their UTC starts; then pair each `0x7E8` reply in
      both `frames.csv` by its order and bytes, and take the
      difference of their UTC times (the recording's UTC start plus the
      row's `host_us`). Its median and spread are the two adapters'
      delivery delays and the two machines' clock agreement together

## Phase 4, again: the monitor with flow control off (BENCH prep #19)

Phase 4's MX+ printed every First Frame and none of their Consecutive
Frames ([2026-11-04-two-ecu-car](2026-11-04-two-ecu-car.md)), so no
multi-frame reply arrived. A session that only listens sets
`AT CFC0` before `AT MA` ([elm327.md](../elm327.md#passive-listening)).
The phone on a build with it; the rest as Phase 4: the VX on
`j2534://OBDX Pro - VX` (`obd2`, packed: nearly every reply spans
frames), the phone on `elm327://bt:<BT address>?profile=raw`, both
recording. The phone first, then the laptop for 90 s.

- [ ] The phone's wire log (`phone.sh pull`) shows `ATCFC0` answered
      `OK` in the setup, before the first `AT MA`
- [ ] The adapter's data lines by type, counted from the wire log
      (counts only; it carries the `<BT address>`): First Frames,
      Consecutive Frames and Single Frames, as the 2026-11-04 sheet's
      table. **Pass:** every First Frame followed by its Consecutive
      Frames, and the phone's delivered frames near the VX's count
- [ ] The phone's Log shows multi-frame `0x7E8` replies, each
      reassembled; no "vehicle not responding" while the VX asks
- [ ] The VX's session as it runs alone: no health line, the same
      request rate
- [ ] **If the Consecutive Frames stay 0:** `AT CAF0` is the fallback
      (ADR-0026's appendix "passive listening", its note of
      2026-11-04). Note the counts; the setup moves to `AT CAF0`, and
      this phase runs again on that build

## Phase 4b, again: the VX's reply latency (the read that ends at its reply)

[2026-11-04-two-ecu-car](2026-11-04-two-ecu-car.md) put the VX's
packed `obd2` latency at 73.7 ms per PID. Its frame log showed that a
reply arriving after the first read's 10 ms waited about 50 more for
the surrogate's 0 ms re-read, which the VX holds
([ADR-0042](../adr/0042-reply-timing.md)'s note of 2026-11-04). The
laptop on a build with the fix, the VX alone on the car, engine
idling, the surrogate's frame log on (`ECUIO_SUR_LOG_FRAMES=1` in
`havne-cli`'s environment; the Record switch doesn't turn it on):

    export ECUIO_SUR_LOG_FRAMES=1
    havne-cli stream "j2534://OBDX Pro - VX" --seconds 90 < /dev/null > vx-latency.jsonl
    havne-cli stream "j2534://OBDX Pro - VX?pack=0" --seconds 60 < /dev/null > vx-latency-nopack.jsonl

- [ ] Each run's final timing lines: per-PID latency P50, packed
      against 73.7 ms and unpacked against 14.3. **Pass:** packed about
      the bus time to the reply's last frame (13.9 ms on 2026-11-03)
      plus 1-2 ms, unpacked about 5-7 ms
- [ ] The frame log (`ecuio-surrogate.*.log`, counts and times only):
      no reply line about 50 ms after its TX indication; write to read
      near the bus time
- [ ] More requests answered a second than the 2026-11-04 run, no
      health line, no timeouts
- [ ] If packed stays near 20-25 ms with no 50 ms class, the VX ends a
      read at its count but not at once: note the median gap from the
      last frame to the read. If the 50 ms class remains, the build
      isn't the fix: check that `havne-cli`'s surrogate (the Win32
      tree's) was rebuilt and warmed up

## Phase 5 (optional): a listener on the laptop's own clock

The MX+ out, the USB2CAN on the Y-splitter, **its terminator out**.

1. `sudo ip link set can0 up type can bitrate 500000 listen-only on`
2. In WSL: `havne-cli stream "socketcan://can0" --record <folder> --seconds 90 < /dev/null`
3. On Windows: `havne-cli stream "j2534://OBDX Pro - VX" --record <folder> --seconds 60 < /dev/null`

- [ ] The same pairing as Phase 4: here one machine's clocks (Windows
      and WSL's) and the delivery through the VX and the surrogate's
      reads, which end at the reply (Phase 4b, again, measures that
      delivery alone)
- [ ] `can0`'s TX count stays 0

## Evidence and teardown

- `vehicle.sh pull two-ecu` on the laptop, `phone.sh pull <name>` for
  the phone, before anything prunes them.
- Recordings stay in `Testing/`; the sheet describes them. Scrub the
  VIN, CALIDs and serials (`62 F1 xx` rows) from anything committed,
  and the `<BT address>` (`phone.sh scrub-logs`, `leakcheck`).
- BENCH prep #19 from Phase 4; testing.md's two-channel gap and the
  recorder rows from Phases 2 and 3.
