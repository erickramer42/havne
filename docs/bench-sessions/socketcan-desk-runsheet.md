# SocketCAN DESK run sheet: a real CAN controller on a two-node bus

A real CAN controller under Linux's SocketCAN, on a desk bus with the
OBDX Pro VX as the other node: no car. It settles BENCH prep #26
(listening) and #32 (sending), with #32 whether ADR-0040 section 2
keeps its one-shot rule, and #33 (the simulated ECU read over real
wire). Results go into a dated session sheet,
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

## After the run

- `sudo ip link set can0 down`; `usbipd detach --busid <id>` when done.
- `vehicle.sh pull socketcan-desk`, then the session sheet with the
  candump lines (no identification is on this bus), Havne's output and
  the counters; BENCH prep #26 and #32 from it.
