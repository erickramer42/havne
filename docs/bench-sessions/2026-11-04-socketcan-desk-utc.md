# DESK session: one reply on two clocks

**Date:** 2026-11-04, 20:32-20:36
**Controller:** Innomaker USB2CAN (`gs_usb`) in WSL (Ubuntu 26.04) through
`usbipd-win`, as `can0`, 500 kbit/s, normal mode (`listen-only off`,
`restart-ms 0`), error-active throughout
**The other node:** the OBDX Pro VX on USB to the Windows host (J2534
through the x86 surrogate; DLL 1.8.9586.12845, API 04.04,
FW:1.0.1.0 HW:1.0.0.0)
**The bus:** the USB2CAN and the VX on an OBD Y-splitter, as on
2026-11-02 and 2026-11-03. No ECU and no car.
**Havne build:** core 0.4.0 by both sides' `start` lines: `havne-cli sim`
in WSL, `havne-cli stream --record` on Windows
**Run sheet:** [socketcan-desk-runsheet.md](socketcan-desk-runsheet.md),
Phase 4
**Evidence:** the VX's recording, `candump -t a -L can0`'s log, both
sides' `havne-cli` output, the clock checks and the surrogate's session
logs stay on the desktop; the figures below are read from them. The
sim's VIN is made up (`ZZZFAKE11M0000001`), and nothing on this bus
identifies a car.

## The run

`havne-cli sim mock://sim --on can0 --desk-bus --seconds 85` and
`candump -t a -L can0` (80 s) in WSL; about 12 s later on Windows,
`havne-cli stream "j2534://OBDX Pro - VX" --record <folder> --seconds 60`
(`obd2`). WSL's clock against Windows' before and after, as the run
sheet's step 3.

| What | Result |
|---|---|
| The VX's session | 2,627 frames, none dropped, no health line; the recording's UTC start 02:34:41.282 |
| The sim | 2,627 requests, 2,627 replies, 11 refused, none silent, late or unflowed |
| `candump` | 13,014 lines; 2,627 complete `0x7E8` replies, 40 single-frame and 2,587 segmented (the six-PID packs) |
| The clock check | Windows' reading fell between WSL's two, 2.2 s apart before the run and 214 ms apart after: PowerShell's start-up takes most of it |

## One reply's time in both

Each `0x7E8` reply in the recording paired with its completion on the
bus (a segmented reply at its last Consecutive Frame), in order by its
bytes: all 2,627 paired. The recording's time is its UTC start plus
the row's `host_us`; the bus's is candump's.

| Replies | Recording minus bus, median | p10 | p90 |
|---|---|---|---|
| Single frame (40) | 10.25 ms | 9.58 ms | 11.18 ms |
| Segmented (2,587) | 10.41 ms | 9.66 ms | 11.16 ms |

One segmented pair reads -11.45 ms: a reply missing from one side,
so its neighbor about 22 ms later took its place. The rest lie between
9.3 and 20.2 ms.

The durations, each on one clock:

| | Median |
|---|---|
| The bus: request to the reply's last frame, segmented | 1.37 ms (p10 1.30, p90 1.46) |
| The bus: request to a single-frame reply | 0.43 ms |
| The host: request to reply (`latency.csv`, every request kind) | in the bucket ending at 12.3 ms |

## Findings

1. **The same reply lands 10.4 ms later in the VX's recording than on
   the bus,** at the median, single-frame or segmented, with 1.6 ms
   between p10 and p90. That is the VX's delivery plus the offset
   between Windows' clock and WSL's, which this run can't separate:
   the clock check bounds the offset only to between 21 ms behind and
   194 ms ahead. The durations fit a delivery of about that size: the
   host waited at most 12.3 ms at the median for a reply that took
   1.4 ms on the bus.
2. **The VX delivers a segmented reply here in at most about 11 ms,**
   the host's 12.3 ms less the bus's 1.4 ms, against about 52 ms after
   the last frame on the test car
   ([2026-11-03-socketcan-car](2026-11-03-socketcan-car.md)).
   The car's ECU paces its Consecutive Frames at about 5 ms, so its
   replies take 13.9 ms on the bus against 1.4 ms here; why the VX
   then holds them longer is open.

   Note (2026-11-04, later): the car run's surrogate frame log answers
   it. A reply that comes after the surrogate's first read has ended
   is read with no timeout left, which the VX holds about 50 ms; here
   every reply came within the first read
   ([2026-11-04-two-ecu-car](2026-11-04-two-ecu-car.md)).
3. **With nothing answering, the VX's session reads the bus as
   quiet:** a first attempt ran while the simulated ECU wasn't
   running, and got no frames in 60 s and a health line, "vehicle not
   responding (timeout)", as a car with the ignition off would give.
4. **`powershell.exe` reads its standard input:** in a script fed to
   WSL's bash on standard input, the clock check's PowerShell read the
   rest of the script, which then never ran. The run sheet's command
   gives it `/dev/null`, and a script is run by its path.
