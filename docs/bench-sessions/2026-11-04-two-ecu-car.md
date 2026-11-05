# VEHICLE session: both ECUs in one session, the recorders, and two listeners beside the VX

**Date:** 2026-11-04, 22:00-22:39
**Run sheet:** [two-ecu-car-runsheet.md](two-ecu-car-runsheet.md), its
five phases, and a Phase 4b added at the car
**Adapters:**
- the OBDX Pro VX on the laptop's USB, J2534 through the x86 surrogate
  (DLL 1.8.9586.12845, API 04.04, FW:1.0.1.0 HW:1.0.0.0);
- the OBDLink MX+ (STN2255 v5.12.2) over Bluetooth on the test phone
  (Android 16), `elm327://bt:<BT address>`;
- the Innomaker USB2CAN (`gs_usb`, firmware V3.4.3) in WSL through
  `usbipd-win`, as `can0`, its terminator out.

The adapters shared the test car's OBD port through an OBD Y-splitter.
**Vehicle:** the test car, engine idling; CAN OBD-II, 11-bit IDs at
500 kbit/s, the engine ECU on `0x7E0`/`0x7E8` and a second ECU on
`0x7E1`/`0x7E9`; battery 13.7-13.8 V by both adapters.
**Havne build:**
- the laptop: `dev` at bf76e8e (core 0.4.0), the app, the x86
  surrogate and `havne-cli` rebuilt before the run;
- WSL: `havne-cli` built with GCC from the same commit;
- the test phone: a local build of `dev` (core 0.4.0 by its
  recordings).

**Evidence:** the evidence stays on the desktop, and the figures below
are read from it:
- each `havne-cli` run's output;
- the recordings (the laptop's app, `havne-cli`, the phone's and the
  listener's);
- the surrogate's session logs and the phone's ELM327 wire logs;
- `ip -details -statistics link show can0` before and after Phase 5.

The frames and the scans hold identification replies (`62 F1 xx`), and
the wire logs the MX+'s address: nothing of either is here.

| Phase | What ran | Result |
|---|---|---|
| 1 | The second ECU's DIDs, the VX alone | PASS: every block complete, every refusal `7F 22 31` |
| 2 | Both ECUs through the VX: the laptop's app 10 minutes, `havne-cli` 60 s | PASS: each ECU's requests waited only for their own replies; the recording's check clean |
| 3 | Both ECUs through the MX+, the phone's app 2 minutes | PASS: the same, through one adapter; the phone's viewer opened the recording |
| 4 | The MX+ listening (`raw`, `AT MA`) while the VX asks (`obd2`, packed) | The VX PASS; the MX+ delivered only the Single Frame replies (below) |
| 4b | The same with the VX unpacked (`?pack=0`) | 2,822 of the VX's 2,857 replies heard; the 35 missing all multi-frame |
| 5 | The USB2CAN listening (listen-only) while the VX asks | PASS: nothing sent, no error counted; the two recordings paired on the wall clock |

## Phase 1: the second ECU's DIDs

`?profile=uds-generic&scan=first&identify=0&diagnose=0`, one block per
`havne-cli stream --until-scanned`. Every block completed (`left` 0),
and no frame was dropped.

| ECU | Block | Found | Refused |
|---|---|---|---|
| 0x7E9 | F180-F1FF | 2 (F188, F18C: identification, their replies are serials) | 126 |
| 0x7E9 | D800-DBFF | 14 (D982, DA03-DA05, DA8E, DAA2-DAA5, DAA9, DAB2, DAC8-DACA) | 1,010 |
| 0x7E9 | DC00-DFFF | 0 | 1,024 |
| 0x7E8 | D800-D83F | 0 | 64 |

The first connect after the rebuild was lost to the antivirus's sandbox
(the surrogate's pipe broke at once; `havne-cli` exit 3), as on
2026-11-02 and 2026-11-03; the re-run opened normally. The app had been
warmed up, but `havne-cli` spawns the Win32 tree's surrogate, a
different file from the app's copy.

The DIDs for Phases 2 and 3: DA00 on the engine ECU (from the
[2026-11-01-vehicle](2026-11-01-vehicle.md) scan) and DA03 on `0x7E9`.

## Phase 2: both ECUs through the VX

`j2534://OBDX Pro - VX?profile=uds-generic&did=DA00,7E9:DA03&identify=0&diagnose=0`

- **The laptop's app,** 22:03-22:13 (592 s), recording:
  - The session line named both channels, `0x7E8/0x7E0` and
    `0x7E9/0x7E1`; no health line came.
  - 37,318 frames, none dropped; every request answered, none refused
    or timed out; latency P50 16-18 ms.
  - Live listed both DIDs with their values. Both are raw bytes, which
    Live shows as text, so neither row has a tick box: the run sheet's
    "both plot" check can't apply to these DIDs.
  - The recording: 0x7E8 18,894 frames and 0x7E9 18,424 (31.9 and 31.1
    a second). `havne-cli info` found no torn row, gap, overflow, link
    or ISO-TP finding, and two rate drops. The two drops fall in the
    same 0.7 s, 303.7 s in, both ECUs at once.
- **`havne-cli`,** 60 s, recording, end `seconds`, 0 dropped, no health
  line. Per ECU:

  | ECU | Answered | Reply P50 | Interval P50 |
  |---|---|---|---|
  | 0x7E8 DA00 | 1,991 | 18.4 ms | 28.7 ms |
  | 0x7E9 DA03 | 1,947 | 16.4 ms | 28.7 ms |

  Each ECU's interval is less than the two replies' sum (about 35 ms),
  so neither waited for the other's reply.

## Phase 3: both ECUs through the MX+

The same URI on `elm327://bt:<BT address>`, from the test phone,
22:20:37-22:22:35:
- the session line named both channels;
- 4,428 frames, none dropped, each ECU 2,214 (18.8 a second);
- 4,428 answered, none refused or timed out, latency P50 14 ms;
- ISO-TP 4,428 messages, none segmented, none dropped;
- no health line.

The phone's Recordings page opened the recording (1 min 58 s, both
signals); its frames check found nothing in 4,428 frames, and so did
`havne-cli info`.

Each ECU ran at 18.8 a second here, against the VX's 31-33. An ELM327
adapter takes one command at a time (ADR-0026), so each ECU's turn
waits for the other's reply; through J2534 the VX has a request out to
each at once.

## Phase 4: the MX+ listening while the VX asks (BENCH prep #19)

The phone's app on `elm327://bt:<BT address>?profile=raw`, recording,
from 22:25:38. The VX's `obd2` session ran through `havne-cli` for 90 s
from 22:25:53, recording, and the engine was blipped to 3,014 r/min.

- **The VX:** end `seconds`, 1,379 frames, 0 dropped, no health line;
  43 requests, 7,909 answered, 7,866 of them packed, no timeout. That
  is the session as it runs alone.
- **The MX+:** 63 frames delivered, none dropped, no `BUFFER FULL`.
  - Its link went "vehicle not responding" before the VX started,
    since nothing was asking.
  - It went "recovered" at 22:25:53, at the VX's first requests.
  - It went "vehicle not responding" again at 22:25:59, and stayed so
    to the end.

**The MX+'s wire log shows why.** From the VX's first request to the
end, the adapter printed:
- 1,317 First Frames on `0x7E8`;
- 63 Single Frames;
- no Consecutive Frame.

So every multi-frame reply stayed incomplete and was dropped, and with
packing nearly every reply after the probe is multi-frame. The monitor
itself never went quiet: no 5 s gap came between 22:25:48 and 22:27:27,
so no stop and restart. The link-health line was the session seeing no
complete message, not the adapter falling silent.

The setup the monitor ran under is a requesting session's: `AT CAF1`
and `AT CFC1`, then `AT CRA 7E8`. By the FRPM (section 12), an STN with
flow control on handles each First Frame as a reception of its own,
and the manual says nothing of what it then prints while monitoring.

## Phase 4b: the same with the VX unpacked

Added at the car, the USB2CAN off, the MX+ back on the splitter. The
phone on `?profile=raw` from 22:36:52, the VX on
`j2534://OBDX Pro - VX?pack=0` for 90 s from 22:37:09.

- **The VX:** end `seconds`, 2,857 frames, 0 dropped, no health line.
- **The MX+:**
  - "recovered" at 22:37:08 and "vehicle not responding" only at
    22:38:39, the VX's end;
  - 2,822 frames, none dropped;
  - its wire log: 2,822 Single Frames, 36 First Frames, no Consecutive
    Frame.
- **Pairing,** each `0x7E8` reply in both recordings by order and bytes
  (`pair_utc.py`): 2,821 of the phone's 2,822 found their match in the
  VX's. The 35 the VX holds and the phone doesn't are its multi-frame
  replies.

The difference of their UTC times, a median of 745 ms (p10 732, p90
768), is mostly the two machines' clocks: the phone's and the laptop's
agreed only to the second. It says nothing of either adapter's delivery.

## Phase 5: the USB2CAN listening

The MX+ off, the USB2CAN on the splitter, its terminator out.
- `can0` came up LISTEN-ONLY at 500 kbit/s, ERROR-ACTIVE, with RX 0:
  no resent frame on the bus this time (compare
  [2026-11-03-socketcan-car](2026-11-03-socketcan-car.md)).
- The listener in WSL ran `socketcan://can0` for 90 s from 22:30:24,
  recording: 4,486 frames, 0 dropped, exit 0.
- The VX's `obd2` session ran 60 s from 22:30:30: end `seconds`, 923
  frames, no health line.
- After the run, `can0`'s TX count was 0 and every error counter 0,
  still ERROR-ACTIVE.

**One reply on one machine's two clocks:** the VX's recording minus the
listener's, each `0x7E8` reply paired by order and bytes, 923 of 923
paired:

| Replies | n | Median | p10 | p90 | Min | Max |
|---|---|---|---|---|---|---|
| Single Frame | 63 | 10.15 ms | 9.69 | 17.27 | 9.20 | 61.04 |
| Segmented | 860 | 51.20 ms | 40.92 | 52.69 | -5.88 | 61.28 |

The single-frame median matches the desk bus's 10.4 ms
([2026-11-04-socketcan-desk-utc](2026-11-04-socketcan-desk-utc.md)).
The segmented median matches the VX's delivery of segmented replies on
the bus, about 52 ms
([2026-11-03-socketcan-car](2026-11-03-socketcan-car.md)).

Each recording's index matched the other's from the first reply to the
last, so the one negative difference isn't a mis-pair. The listener
stamped that reply 76.6 ms after the one before it, where 70 is usual,
and the next one on time. So the listener's own delivery was late by
about 16 ms, once: its frames reach WSL over USB/IP.

## Findings

1. **Two ECUs in one session work on the car, through both adapters.**
   Through J2534 each ECU's requests wait only for their own replies.
   Through the MX+ they take turns, as one command at a time requires.
2. **The recorder works on the car, in the laptop's app, the phone's
   and `havne-cli`.** Each recording opened and passed `havne-cli
   info`. The phone's viewer opened its recording, and its frames check
   was clean.
3. **The MX+'s monitor, as Havne sets it up, prints no Consecutive
   Frames,** so a listening session hears another tester's Single
   Frame replies only.
4. **A raw-byte DID can't be plotted.** Live and the viewer show it as
   text. The run sheet's plotting checks need a DID with a scaling, or
   rewording.
5. **The antivirus holds the first `havne-cli` connect after a
   rebuild,** whatever the app's warm-up did: a warm-up has to run
   `havne-cli` itself.
6. **The USB2CAN's driver offers no one-shot mode** on the car either.
   `ip -details -json link show can0` lists LOOPBACK, LISTEN-ONLY and
   CC-LEN8-DLC (read only, nothing sent).

## Against the BENCH prep list

| # | Result |
|---|---|
| 19 | CONFIRMED for Single Frame replies on the MX+. `AT MA` with the setup's `AT CRA` passes `0x7E8` and nothing else, and the replies reassemble as a request's do: 2,821 of 2,822 matched the VX's in order and bytes. REFUTED for multi-frame replies under the setup's `AT CAF1` and `AT CFC1`: the adapter printed each First Frame and no Consecutive Frame (1,317 and 0 with the VX packed, 36 and 0 unpacked). Whether the monitor acknowledged nothing wasn't checked; the VX's sessions ran as they run alone |
| 26 | CONFIRMED again: listen-only beside the VX on the test car, nothing sent, no error counted |
