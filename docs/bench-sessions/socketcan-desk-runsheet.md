# SocketCAN DESK run sheet: a real CAN controller on a two-node bus

A real CAN controller under Linux's SocketCAN, on a desk bus with the
OBDX Pro VX as the other node: no car. It settles BENCH prep #26
(listening) and #32 (sending), with #32 whether ADR-0040 section 2
keeps its one-shot rule, #33 (the simulated ECU read over real
wire), one reply's time on two clocks (ADR-0049 section 5), and #17
(response pending) for the VX in its ELM mode. Results
go into a dated session sheet,
`docs/bench-sessions/<date>-socketcan-desk.md`.

## The bus

    USB2CAN (gs_usb, can0 in WSL) -- 120 ohm terminator -- DB9-to-OBD cable
        -- OBD Y-splitter -- OBDX Pro VX (USB to the Windows host, its own power)

- One terminator: the desk bus is short, and a car's bus is terminated
  already. **Take the terminator out before the cable meets a car.**
- The USB2CAN reaches WSL through `usbipd-win`
  (`usbipd attach --wsl --busid <id>`); `gs_usb` makes it `can0`.
- The VX runs from Windows through Havne's J2534 transport (the x86
  surrogate), the bench's usual setup.
- With no ECU on the bus, nothing answers a request: a frame is
  acknowledged by whichever other node's controller is active.

## Before the run

- [ ] `vehicle.sh mark socketcan-desk` on the Windows host (the VX's
      session logs live in `%TEMP%`; no test runs on this machine until
      `pull`)
- [ ] `ip -details link show can0`: kind `can`, down; note the
      controller's supported modes (`ip -j -details link show can0`,
      `ctrlmode_supported`)
- [ ] `havne-cli devices` in WSL lists `can0`, and a stream on it
      while it's down is refused with the `ip link` fix

## Phase 1: listening (BENCH prep #26)

The USB2CAN never transmits in listen-only mode: no frame, no ACK, no
error frame.

1. `sudo ip link set can0 up type can bitrate 500000 listen-only on`
2. In WSL: `candump -t A -e can0,0:0,#FFFFFFFF` and
   `havne-cli stream "socketcan://can0" --seconds 30` (the `raw`
   profile: every frame with the kernel's time).
3. On Windows: `havne-cli stream "j2534://OBDX Pro - VX" --seconds 10`
   (`obd2`: Service 01 requests on 0x7E0). With no ECU and the
   USB2CAN only listening, no node acknowledges, so the VX's
   controller resends each frame until it goes error passive.

- [ ] Havne's session hears the VX's 0x7E0 requests with the kernel's
      stamps, and the right payloads
- [ ] `can0`'s TX counters stay 0 and its state stays error active
      (`ip -details -statistics link show can0`)
- [ ] The VX's session reports what a bus with no acknowledging node
      looks like (its health lines and the surrogate's session log)

**Wrong rate:** `sudo ip link set can0 down`, then
`sudo ip link set can0 up type can bitrate 250000 listen-only on`, the
VX again at 500 kbit/s.

- [ ] Havne's session gets no frames and reports errors (the error
      frames' classes), and the VX sees nothing different from the
      right rate (listen-only disturbs nothing)

**Unplugged:** with the session running, `usbipd detach --busid <id>`
on Windows, wait 10 s, attach again.

- [ ] The session fails hard and recovers once `can0` is back and up
      (the user brings it up again)

## Phase 2: sending (BENCH prep #32)

Havne refuses to send through a controller without one-shot mode
(`SOCKETCAN_SEND_NEEDS_ONE_SHOT`), so this phase uses `can-utils`
directly. The USB2CAN transmits, onto this two-node bus only.

1. `sudo ip link set can0 down`, then
   `sudo ip link set can0 up type can bitrate 500000 restart-ms 0 listen-only off`
   (`listen-only off` said: an interface keeps the mode it was last up in)
2. With the VX's channel connected (a Havne J2534 session running, its
   controller active): `cansend can0 7E8#0241000000000000` while
   `candump -t A -e -x can0` watches (`-x` marks TX and RX).

- [ ] The frame is echoed back (`TX` in candump) once the VX
      acknowledges it, and the VX's session hears 0x7E8
- [ ] With the VX's session closed (no active node): whether the frame
      is echoed before the first `no-acknowledgement-on-tx` error frame
      (record both stamps: the rate's proof waits for a reply, or for
      N_As with no error frame, after the echo), and how often it's sent
      again: without one-shot mode, forever at error-passive, never
      bus-off (2026-11-02); with it, once
- [ ] An adapter that offers one-shot mode (`one-shot on`): the same,
      and Havne's own `socketcan://can0?profile=obd2&baud=500000`
      against the VX, whose session hears its requests

**Wrong rate:** `can0` at 250 kbit/s, the VX at 500:

- [ ] The same two questions: the echo against the first error frame,
      and the resends (one in one-shot mode)

**What it decides:** without one-shot mode an unacknowledged frame is
sent again until the interface goes down, so ADR-0040 section 2 keeps
one-shot mode required (its appendix of 2026-11-02). The open question
is section 3's: whether an adapter with one-shot mode echoes only after
an acknowledgement. The proof doesn't rely on it: it waits for the
reply, or for N_As with no error frame, after the echo (ADR-0040's
appendix of 2026-11-02).

## Phase 3: the simulated ECU (BENCH prep #33)

`havne-cli sim --desk-bus` answers as the simulated car's engine ECU
on `can0` ([ADR-0046](../adr/0046-simulated-ecu.md)'s appendix "the
desk bus"), and the VX reads it through J2534, as it reads a car.
The USB2CAN transmits only in reply, onto this two-node bus only.

1. `sudo ip link set can0 down`, then
   `sudo ip link set can0 up type can bitrate 500000 restart-ms 0 listen-only off`
2. In WSL: `candump -t A -e can0` and
   `havne-cli sim mock://sim --on can0 --desk-bus --seconds 120`
3. On Windows: `havne-cli stream "j2534://OBDX Pro - VX" --values --seconds 60`
   (`obd2`), then the app on the same URI: Live, Dashboard and
   Diagnostics.

- [ ] Without `--desk-bus`, `sim` is refused: `can0 is a CAN
      controller, not vcan`; with it and `can0` listen-only, refused
      with `listen-only off`
- [ ] The VX's session walks the sim's bitmaps, reads its VIN
      (`ZZZFAKE11M0000001`), its diagnostics and the inspection line,
      and RPM goes from 0 to idle 15 s after `sim` starts
- [ ] Every reply is acknowledged: `can0` stays error-active, its
      error counters unchanged (they count since the adapter was
      attached: compare before and after), and `sim`'s `unflowed`
      stays 0 (the VX's Flow Control reaches it)
- [ ] With the VX's session closed, `sim` sends nothing (it only
      replies): the TX count holds
- [ ] Another node on 0x7E8: in a second WSL shell,
      `cansend can0 7E8#0641000000000000`; `sim` ends with `yielded`,
      exit 1, and the VX's next requests go unanswered
- [ ] *Optional:* `replay://` with a car recording instead of
      `mock://sim`: the VX walks the car's own bitmaps

## Phase 4: one reply on two clocks (ADR-0049 section 5)

How far apart the same reply lands in the VX's recording, aligned on
its UTC start, and on the bus as WSL's kernel stamps it. One machine's
clock against itself is `SocketCanBusTest.TwoRecordingsOfOneBusLineUpOnTheirUtcStarts`
(under a millisecond); this phase adds the VX's delivery and the
offset between Windows' clock and WSL's. A Havne listener can't share
`can0` with `sim` (a listening session needs the controller
listen-only), so `candump` is the bus's side.

1. `can0` as in Phase 3, `sim` answering:
   `havne-cli sim mock://sim --on can0 --desk-bus --seconds 120`
2. In WSL: `candump -t a -L can0 > desk-bus.log` (each frame's absolute
   time from the kernel)
3. WSL's clock against Windows', before and after the run, in WSL:
   `date +%s.%N; powershell.exe -NoProfile -c "[DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()" < /dev/null; date +%s.%N`
   (the Windows reading falls between the two WSL ones; PowerShell
   reads standard input, so in a script it gets `/dev/null`)
4. On Windows: `havne-cli stream "j2534://OBDX Pro - VX" --record <folder> --seconds 60 < NUL`
   (`obd2`)

- [ ] Pair each `0x7E8` reply in the recording's `frames.csv` with its
      line in `desk-bus.log`, by order and bytes; the difference of
      their times (the recording's UTC start plus the row's `host_us`,
      against candump's) for single-frame and segmented replies apart:
      median and spread
- [ ] The clocks' offset from step 3, and how much of the difference it
      explains; what is left is the delivery through the VX and the
      surrogate's reads (about 10 ms for a reply inside the first read,
      about 52 ms for one after it,
      [2026-11-04-two-ecu-car](2026-11-04-two-ecu-car.md))

## Phase 5: response pending (BENCH prep #17)

`faults=pending:3` makes every third reply of each kind (single-frame
and segmented counted apart, the bitmaps never) go out as
`7F <service> 78` at once and the reply itself 300 ms later, whole
([ADR-0046](../adr/0046-simulated-ecu.md)'s appendix of 2026-11-04).
300 ms is past the request engine's 100 ms wait and the ELM327
setup's `AT ST 19` (100 ms), and well inside P2* (5 s), so only a
tester that keeps waiting after the 78 gets the answer.

**What a tester must do with a 78:** a "response pending" negative
reply extends the request's wait to P2*, 5 s ([ADR-0015](../adr/0015-request-engine.md)
section 1, "Pacing": 5.05 s with the adapter's margin); pending is a
state on the way, not an outcome. On ELM327, the `7F xx 78` line
reaches the engine as on J2534 and extends its wait; whether the
adapter goes on listening for the final reply is the adapter's
([ADR-0026](../adr/0026-elm327-transport.md) section 3 and its open
question "response pending"). An STN does by default (parameter 2A,
bit 2); a plain ELM327's datasheet doesn't say, and neither does the
OBDX's manual. A Service 01 request carries the response-count digit
(`elm327_response_count`), so an adapter that doesn't process the 78
takes its line
as the one reply asked for and returns to the prompt: the final reply
arrives while nobody listens (the wire log's `unasked:` line before the
next command), and the engine waits out its 5.05 s and asks again.

The VX runs one link at a time: its J2534 session, then its ELM mode
on its USB serial port (`OBDX Pro Serial (COM4)` on the dev machine,
[2026-10-27-elm327-bench-2](2026-10-27-elm327-bench-2.md)), each
closed before the next, with the OBDX vendor app closed. The sim's
`canned` values answer every PID its bitmaps list, so with
identification and diagnostics off nothing is refused. Each tester
gets a `sim` run of its own, so the counts are its alone.

1. `can0` as in Phase 3:
   `sudo ip link set can0 down`, then
   `sudo ip link set can0 up type can bitrate 500000 restart-ms 0 listen-only off`
2. In WSL: `candump -t A -e can0 > pending-j2534.log` and
   `havne-cli sim "mock://sim?values=canned&faults=pending:3" --on can0 --desk-bus --seconds 45 < /dev/null`
3. On Windows (cmd), the VX through J2534, the frames logged:
   `set ECUIO_SUR_LOG_FRAMES=1`, then
   `havne-cli stream "j2534://OBDX Pro - VX?identify=0&diagnose=0" --seconds 30 < NUL`
   (`obd2`; its `timing` lines every 10 s and at the end)
4. Once `sim` has ended, step 2 again with `pending-elm.log`, then the
   VX in ELM mode, its wire log holding each exchange:
   `havne-cli stream "elm327://COM4?profile=obd2&identify=0&diagnose=0" --seconds 30 < NUL`

For each tester:

- [ ] `sim`'s `end` line: `pending` rose (about a third of the replies
      that aren't bitmaps), `refused` 0, `unflowed` 0, `yielded` false
- [ ] The stream's last `timing` lines: `timeouts` 0 and `refused` 0 on
      every row, and the rows' highest `latency_max_us` about 300,000
      or more (the reply 300 ms after its 78, plus the adapter's
      delivery)
- [ ] The rotation kept answering: each row's `answered` rises from one
      `timing` line to the next, and the `health` lines report no link
      problem
- [ ] `candump`: each `7F xx 78` from 0x7E8 followed about 300 ms
      later by its reply, and the VX's next request only after that
      reply

The ELM-mode run alone:

- [ ] The wire log (`ecuio-elm327.<stamp>.log` in `%TEMP%`, the
      newest): whether the `7F 01 78` line is printed (ADR-0026's first
      sub-question), and whether the final reply comes in the same read,
      before the prompt (the VX goes on listening), or after it as an
      `unasked:` line (it doesn't)
- [ ] The stream's `session` line: no `pending_off` key (it is there
      only for an STN whose parameter 2A has the processing off; the VX
      isn't an STN)

**What it decides:** BENCH prep #17 for the VX in ELM mode. CONFIRMED
when every pending request completes in the same read: no timeouts,
the slowest replies about 300 ms, no `unasked:` reply. Answered the
other way when the 78's request times out (`timeouts` about the
`pending` count, latencies near 5 s) and its reply comes unasked:
the VX then needs the engine to ask again without the count digit
after a 78, an ADR-0026 amendment. The J2534 run is the control: the
same bus and sim, a transport that hands every frame up, so if it
times out too, the fault is the bus's or the sim's, not the VX's ELM
mode. A plain ELM327 and an STN on this bus stay open.

## After the run

- `sudo ip link set can0 down`; `usbipd detach --busid <id>` when done.
- `vehicle.sh pull socketcan-desk`, then the session sheet with the
  candump lines (no identification is on this bus), Havne's output and
  the counters; BENCH prep #26 and #32 from it, and #17 from Phase 5
  (the ELM-mode wire log's exchanges among the evidence: `vehicle.sh
  pull` takes the `ecuio-elm327.*.log` files too).
