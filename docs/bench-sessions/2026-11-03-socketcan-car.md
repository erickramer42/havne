# VEHICLE session: a real CAN controller listening on the test car, beside the VX

**Date:** 2026-11-03, 21:28-21:40
**Controller:** Innomaker USB2CAN (`gs_usb`, firmware V3.4.3) in WSL
(Ubuntu 26.04.1, kernel 6.18) through `usbipd-win`, as `can0`, on the
laptop. Its driver offers no one-shot mode.
**The other node:** the OBDX Pro VX on the laptop's USB, J2534 through
the x86 surrogate (DLL 1.8.9586.12845, API 04.04, FW:1.0.1.0
HW:1.0.0.0), the `obd2` profile.
**The bus:** the test car's OBD port at 500 kbit/s, the USB2CAN's
DB9-to-OBD cable and the VX on an OBD Y-splitter, no terminator added.
The engine idling; battery 13.67 V by the VX.
**Havne build:** `dev` at the review pass's tip (core 0.3.6 by its
`start` line): `havne-cli` built with GCC in WSL for `can0`, and on
Windows for the VX
**Evidence:** both sides' `havne-cli` output, the listener's
recordings, `candump` and `ip -details -statistics link show can0`
before and after each step, and the surrogate's session logs stay on
the desktop; the figures below are read from them. The VX's output and
the frames hold the car's identification: nothing of it is here.

## Listen-only throughout

`can0` was brought up with `listen-only on` and checked before every
step: LISTEN-ONLY, 500 kbit/s, error-active. Across the run its TX
count stayed 0 and every error counter (re-started, bus-errors,
arbit-lost, error-warn, error-pass, bus-off) stayed 0, from an adapter
attached that evening. The sessions dropped nothing (`dropped_frames`
0, no overflow flag); the kernel's count over a step runs past a
session's by the resent frame below in the seconds before and after
each capture (119,962 against 108,721 for `quiet`, 28,637 against
26,685 for `beside`), and with no such frame it matched (0 and 0 in the
quiet after the run).

| Step | What ran | Result |
|---|---|---|
| quiet | `socketcan://can0` (`raw`) for 30 s, no tester session | 108,721 frames, none dropped, no error frame; all one frame (below) |
| refusal | `socketcan://can0?profile=obd2&baud=500000` | refused at connect, exit 3: "can0's driver offers no one-shot mode, so a frame that fails would be sent again by the controller: this profile sends requests, so nothing was sent; through can0, use a profile that only listens, with can0 up listen-only" |
| beside | the listener for 100 s, recording; 5 s in, the VX's `obd2` session for 60 s | both PASS (below) |
| quiet, again | 30 s after the run, the VX idle | 0 frames |

## A reply resent with nobody to acknowledge it

From the moment `can0` came up, before any tester asked, the bus
carried one frame over and over: the engine ECU's Single Frame reply to
Service 01 PID 05 (coolant temperature), on 0x7E8, about 3,600 times a
second, nothing else. It was there through `quiet` and the first
`beside` attempt, and in the second it stopped at 6.2 s, as the VX's
session started: the VX's controller acknowledged it. After the VX's
session closed the bus was silent, and so was the quiet after the run.

A CAN controller sends a frame again until a node acknowledges it, as
the desk bus showed for the USB2CAN (2026-11-02). The ECU had answered
a request from before the run (no Havne session ran between the run's
start and this listener) and was left with no node to acknowledge the
answer: the USB2CAN only listens, and the VX's controller acknowledges
only while a session has it connected. The test car's OBD port carries
no traffic of its own. A listener alone can't end such a frame; any
active node does.

## Beside the VX

The first attempt lost the VX's connect to the antivirus's sandbox on a
fresh build (the surrogate's pipe broke 2 ms after spawn; `havne-cli`
ended by itself after 29 s, exit 3), as on 2026-11-02. The second:

- **The VX:** 944 replies delivered, none dropped, no health line;
  identification answered six items and refused seven; stored, pending
  and permanent codes read; the inspection line ready (one incomplete
  monitor, which the rule allows); 43 requests, 5,299 answered, 5,256 of
  them packed, no timeout.
- **The listener:** 26,685 frames, none dropped, no error frame. It
  heard the VX's 945 requests on 0x7E0, each answered on 0x7E8, and the
  VX's 882 Flow Control frames; the frames check (`havne-cli info`) on
  its recording found no ISO-TP sequence broken, the resent frame's end
  as rate drops, and 0x7E0's start as an ID first seen late.

**A reply's time on the bus and at the worker.** The listener's kernel
stamps time each request to its reply on the wire; the VX's session
times it on the host's clock, from the write to the read that completes
it (ADR-0042):

| | Median |
|---|---|
| Request to the reply's first frame (the bus) | 3.46 ms (P99 10.93, n 945) |
| The VX's Flow Control after a First Frame (BS 0, STmin 0) | 0.17 ms |
| Flow Control to the first Consecutive Frame, then between them (the ECU's pace) | 4.8 ms, then 5.3 ms |
| Request to the reply's last frame (nearly every request a six-PID pack, segmented) | 13.9 ms |
| The VX's latency per PID, host's clock | 65.5 to 73.7 ms |

So a segmented reply reaches Havne's worker about 52 ms after its last
frame crossed the bus. The VX acts fast on the bus; the time is between
its receiving the message and the worker's read of it: the adapter, its
USB link and its driver.

Note (2026-11-04): the surrogate's frame log on the next car run puts
the time in the surrogate's read instead. A vendor read that brought
only the request's TX indication was read again with none of its
timeout left, and the VX held that read about 50 ms; the VX's own
stamps put the reply about 11 ms after its request left
([2026-11-04-two-ecu-car](2026-11-04-two-ecu-car.md)). Havne's read slice is 10 ms, and a single-frame
reply through the VX on this laptop took 14.3 ms in all
([2026-11-01-vehicle](2026-11-01-vehicle.md)). The VX's figure for a
pack there, 73.7 ms, agrees with this run's.

## Findings

1. **A real controller listens on the test car as on the desk:** the
   frames with the kernel's stamps, none dropped, nothing sent, nothing
   disturbed.
2. **Havne refuses to ask through it,** before anything goes out.
3. **The test car's ECU resends an unacknowledged reply forever,**
   until an active node acknowledges it.
4. **The bus's timing and the VX's differ by the adapter's delivery:**
   about 52 ms for a segmented reply.
5. **`stream --record` reads standard input for markers,** and the run
   script fed its WSL commands on standard input: the listener took the
   script's last lines as markers (in its `markers.csv`), and the lines
   after it never ran. Its recordings were complete and were copied by
   hand. A script gives `stream --record` `/dev/null` for its input.

## Against the BENCH prep list

| # | Result |
|---|---|
| 26 | CONFIRMED on the test car for the Innomaker USB2CAN: listen-only beside a tester, the frames with the kernel's stamps and none dropped, no frame, acknowledgement or error frame sent, no error counted. The wrong rate and the unplug were the desk bus's (2026-11-02), not repeated on the car |
