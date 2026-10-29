# ADR-0032: SocketCAN, listening — `socketcan://` reads a Linux CAN interface, listen-only, and a simulated ECU feeds it

- **Status:** Accepted.
- **Date:** 2026-10-29. Implemented with this record.
- **Builds on:** [ADR-0007](0007-uri-selected-transports.md) (a
  transport per URI scheme); [ADR-0013](0013-protocol-profiles.md)
  (the profile decides the decode); [ADR-0018](0018-connect-failure-reporting.md)
  (a refused connect says why); [ADR-0019](0019-session-info.md)
  (what a session runs on); [ADR-0026](0026-elm327-transport.md)'s
  appendix "passive listening" (a session that asks nothing, and its
  link health); [ADR-0031](0031-command-line-client.md) (the CLI).

## Context

Every transport so far reaches the bus through an adapter that does
the protocol work: a J2534 driver or an ELM327 reassembles ISO-TP,
filters, and stamps or paces what it hands over. Linux has a native
CAN stack, SocketCAN: a CAN interface (`can0`) is a network device,
its frames come from a socket, and the hardware behind it is a plain
CAN controller (candleLight and CANable USB adapters through `gs_usb`,
PEAK, Kvaser, a Raspberry Pi's MCP2515 hat, or an ELM327 through the
kernel's `can327`). Nothing between the bus and core interprets a
frame, so core sees the bus as it is: every ID, every frame, error
frames, at the controller's own timing.

v0.3.4 is the listening half: raw frames from an interface, nothing
sent. v0.3.5 adds requests, which on SocketCAN need ISO-TP, done by
the kernel's `can-isotp` sockets or by core (an ADR of its own then).

Three facts shape a listener:

- **Configuring an interface needs privileges.** The bit rate and the
  controller's modes (listen-only among them) are set through rtnetlink
  with `CAP_NET_ADMIN`: `ip link set can0 type can bitrate 500000
  listen-only on`. Reading them back needs none.
- **A wrong bit rate disturbs the bus, unless the controller only
  listens.** A controller in normal mode acknowledges every frame it
  receives, and at the wrong rate it reads errors and sends error
  frames onto the car's bus. In listen-only mode it never transmits:
  no ACK, no error frame, nothing.
- **There is no bus to test against.** No native CAN adapter is on
  hand and no car is wired for one. Linux's virtual CAN driver (`vcan`)
  is a bus inside the kernel; `vcan0` already runs on CI's Linux
  runner and in WSL (the v0.3.4 tooling, `tests/vcan_bus.h`).

## Decision

### 1. A transport in core, Linux only

`socketcan://<interface>` (`core/src/socketcan_transport.cpp`) builds
and runs on Linux, Android excluded (its kernels ship without CAN). On
every other platform the scheme is `NO_TRANSPORT` with the detail
"socketcan:// needs Linux", as `j2534://` says it needs Windows.

The interface name is what `ip link` shows: 1 to 15 characters of
letters, digits, `_`, `-` and `.` (`IFNAMSIZ`).

### 2. Core reads the interface, never configures it

Attach asks rtnetlink (`RTM_GETLINK`, no privileges) for the interface
and refuses what it can't listen on safely:

| The interface | Attach |
|---|---|
| doesn't exist | `LOAD_FAILED`: "no network interface is named can0" |
| isn't a CAN device (`eth0`) | `LOAD_FAILED`: "eth0 isn't a CAN interface" |
| is down | `LOAD_FAILED`, with the command that brings it up |
| is a CAN controller (link kind `can`) not in listen-only mode | `LOAD_FAILED`: "can0 isn't listen-only", with the commands that make it so (`ip link set can0 down`, then `… up type can bitrate <its rate> listen-only on`) |
| was brought up at another rate than `?baud=` asks | `BAD_URI`: "can0 runs at 250000 bit/s, not 500000" |
| is virtual (`vcan`) | accepted: no controller, nothing reaches a car |

Core never sets a rate or a mode itself: it would need root, and a
reader that quietly reconfigures a car's bus is the wrong default. The
user brings the interface up once, with the rate the car's bus runs at;
`?baud=` (500000 or 250000, as `elm327://` takes it) makes the session
check it. The listen-only rule is strict while SocketCAN only listens:
a session that sends needs the controller in normal mode, so v0.3.5's
requests revisit it in their own ADR (when normal mode is allowed, and
what then guards against a wrong rate on a car).

### 3. The URI

```
socketcan://<interface>[?profile=…][&baud=500000|250000][&filter=<id>[/<mask>],…]
```

- **`profile`:** `raw` by default, as `replay://` has. A profile that
  requests (`obd2`, `uds-generic`) is refused (`NOT_IMPLEMENTED`:
  requests on SocketCAN come with ISO-TP), and so is decoding through
  one: on a raw bus their replies arrive as ISO-TP frames, a PCI byte
  first, which their defs don't expect. A profile of broadcast frames
  (`mock`, and later manufacturer sets of broadcast IDs) decodes as on
  any transport.
- **`filter`:** up to 8 IDs, each with an optional mask, set on the
  socket (`CAN_RAW_FILTER`), so the kernel drops the rest; the whole
  bus without it. An ID above 0x7FF is a 29-bit filter.
- `poll`, `probe`, `identify`, `txid` belong to requests and aren't
  taken. `did` is, only so that `?profile=uds-generic&did=…` is refused
  for what it is, a profile that requests, rather than for its key.

### 4. The socket and the worker

- One `CAN_RAW` socket, bound to the interface, non-blocking and
  close-on-exec, with CAN FD frames on (`CAN_RAW_FD_FRAMES`), error
  frames subscribed (`CAN_RAW_ERR_FILTER`), the kernel's receive time
  (`SO_TIMESTAMPNS`) and its count of frames dropped for a full socket
  buffer (`SO_RXQ_OVFL`).
- The worker waits in `poll` for up to 100 ms, then reads every frame
  waiting (`recvmsg` until `EAGAIN`). Each becomes an `ecuio_frame_t`:
  its ID (`FLAG_EXTENDED` for 29-bit, `FLAG_REMOTE`, `FLAG_FD` and
  `FLAG_BITRATE_SWITCH` from the frame), its 0 to 8 or 64 bytes, on
  channel 0. Nothing is allocated per frame.
- **Lost frames:** the kernel stamps each frame with the socket's drop
  count as it queues it (`SO_RXQ_OVFL`), so the first frame queued
  after a loss carries a higher count, and it reaches the callbacks
  with `ECUIO_FRAME_FLAG_OVERFLOW`, as after an ELM327's `BUFFER FULL`
  or J2534's `BUFFER_OVERFLOW`. The frames already in the buffer when
  it filled came before the loss and aren't flagged.
- **Error frames** reach the callbacks as frames with
  `ECUIO_FRAME_FLAG_ERROR`: the arbitration ID holds the error class
  bits (`CAN_ERR_*`), the 8 data bytes the kernel's detail. A raw
  session logs and records them; no def matches them.

### 5. Time: the kernel's receive time, anchored to the session

`SO_TIMESTAMPNS` stamps each frame when the kernel received it, on the
wall clock (`CLOCK_REALTIME`), which can step. The mapping follows the
J2534 clock's (`j2534_clock.h`): the first frame takes the session's
host time; each later one adds the kernel's delta since the frame
before, so frames read in one batch, or read late after a stall, keep
their real spacing. A frame is received before it's read, so a delta
that runs backward, or that would put the frame more than a second
after the moment it's read, means the wall clock stepped, and the
host's delta stands in. The result never decreases. A pure function
(`socketcan_clock_step`), unit-tested like the J2534 one.

### 6. Link health

Through `link_health.h`, as a passive ELM327 session:

| What the worker sees | State |
|---|---|
| frames arriving (any, the filter's) | `OK` |
| none for about a second (ten empty 100 ms waits) | `VEHICLE_TIMEOUT`: a quiet bus, or the ignition off |
| an error frame for bus-off, or the controller restarting | `TRANSPORT_HARD_FAILURE`, one episode |
| the interface going down (`ENETDOWN`), or gone (`ENODEV`) | `TRANSPORT_HARD_FAILURE`; the socket closes, and the worker reopens it on the backoff (100 ms doubling to 5 s) until it's back |

Error-passive and error-warning reports only reach the log as error
frames; a controller reporting them still receives.

### 7. What the session says

- `ECUIO_BACKEND_SOCKETCAN` (6, appended) in the session info, `baud`
  the interface's rate (0 for `vcan`), and a new flag,
  `ECUIO_SESSION_INFO_LISTEN_ONLY`, set for a controller in listen-only
  mode. The activity log's "Session:" line names them.
- **Discovery,** `ecuio/socketcan.h`: `ecuio_can_interfaces(list,
  capacity, &count)`, as `ecuio_serial_ports` works (the caller's
  array, no free): each interface's name, its kind (`can` or `vcan`),
  whether it's up, its rate and whether it listens only. Settings lists
  them on Linux beside the serial ports, and `havne-cli devices` writes
  a `can` line for each. An empty list everywhere else.

### 8. The simulated ECU: `havne-cli play`

```
havne-cli play <uri> --to <interface>
```

Opens any session the CLI can (`mock://` for the drive cycle, a
`replay://` recording) and writes each of its frames to the interface
as it arrives, at the session's own pace, so the mock's car drives on
the bus. It writes only to a `vcan` interface and refuses any other
(`LOAD_FAILED`, "play writes only to a virtual CAN interface"): a
frame from a recording or a simulation never goes onto a real bus,
whoever runs it.

- The mock's telemetry is 12 bytes, so it needs CAN FD: the interface
  takes FD frames when its MTU is at least 72. Recent kernels create a
  `vcan` with CAN XL's 2060 (WSL's 6.18 does); an older one's 16 is
  raised while the interface is down (`ip link set vcan0 down`, then
  `ip link set vcan0 mtu 72 up`), and `play` says so, skipping frames
  over 8 bytes until it is. A message of 9 to 63 bytes goes out at the
  next CAN FD length (12, 16, 20, 24, 32, 48, 64), zero-padded, as an
  FD controller sends it.
- Its output is JSON lines as `stream`'s (ADR-0031): `start`, the
  source's `session`, a `play` line (`to`, `fd`), and an `end` line with
  `sent` and `skipped` beside `frames`. A frame is skipped when it
  doesn't fit one CAN frame, is an error report, or the write fails;
  the write never blocks the source's worker.
- A recording's frames go out as they were recorded. A J2534 or ELM327
  recording holds whole ISO-TP messages, which `play` sends only when
  they fit one frame (8 bytes, or 64 on FD) and counts the rest as
  skipped; putting them back into ISO-TP framing is v0.3.5's.
- It ends as `stream` does (Ctrl+C, `--count`, `--seconds`, a replay's
  end), with an `end` line on standard output.

Two terminals then run the whole path with no hardware:

```
havne-cli play mock://demo --to vcan0
havne-cli stream "socketcan://vcan0?profile=mock" --values
```

and the app on Linux connects to `socketcan://vcan0?profile=mock` the
same way.

### 9. Tests

- **Unit** (`SocketCanTest`, Qt-free, every platform, over
  `core/src/socketcan_wire.h`, which mirrors the kernel's values and
  checks them against its headers on Linux): the interface name's
  rules, `?filter=`, the URI's refusals, the clock mapping (batches, a
  late read, a backward step, a forward jump, never decreasing), the
  frame conversion (flags, FD, 29-bit, remote, error frames), and the
  netlink attributes parsed from canned messages (a `can` in
  listen-only mode, one not, one reporting no mode, a `vcan`, an
  `eth0`, a down link, a rate against `?baud=`), since no listen-only
  controller is on hand. On Linux also the loopback refused as not CAN
  and a name no interface has.
- **HARNESS on `vcan0`** (`SocketCanBusTest`, in `test_vcan`; required
  on CI's `linux-build`, whose `vcan0` gets MTU 72): discovery lists
  it; frames written to the bus arrive in order with their flags; the
  filter keeps only its IDs and their width; the mock's drive cycle
  played onto the bus decodes through `?profile=mock` to the values
  the mock transport gives for the same frames; a quiet bus turns the
  state to `VEHICLE_TIMEOUT` and the next frame back to `OK`, with no
  episode; a flood past the socket's buffer flags the first frame
  after it `OVERFLOW`; the worker allocates nothing per frame; the
  interface set down and up again is one hard-failure episode that
  recovers (as root: CI runs that case again under `sudo`).
- **`play` with `stream`:** the mock's car played onto `vcan0` and
  heard through `socketcan://vcan0?profile=mock`, by hand; `play`
  refuses an interface that isn't `vcan`.
- **What no test reaches:** a real controller, listen-only or not, and
  a car's bus. That waits for a CAN adapter (candleLight or CANable,
  through `usbipd-win` into WSL, or a Linux machine) as a BENCH prep
  item, and an OBD breakout for the car.

## Alternatives considered

- **Core sets the rate and listen-only mode.** One step fewer for the
  user, but it needs `CAP_NET_ADMIN` (root, or a capability on the
  binary that every front end would then carry), and a reader that
  reconfigures a bus is surprising. The refusal names the commands
  instead.
- **Warn instead of refusing a controller in normal mode.** Normal
  mode at the right rate does no harm on most buses (the controller
  only ACKs), and a warning is friendlier. But the harm at the wrong
  rate is to the car, and a warning in an activity log is read after
  the fact. v0.3.5 has to allow normal mode for requests and can say
  how; until then listening means listen-only.
- **The host's clock at read, as `elm327://` stamps.** Simpler, but a
  batch of frames read together would share one time, and SocketCAN is
  the one transport that sees every frame on the bus with the kernel's
  timing; the log viewer and the PCAP export want it.
- **A simulated ECU as a test helper only** (`tests/vcan_bus.h`). The
  tests need it either way; as a CLI command it also lets anyone run
  the SocketCAN path, the app on Linux included, with no hardware, and
  gives others a known-good source to test their own tools against.
  Restricting it to `vcan` keeps it from ever writing to a car.
- **`libsocketcan` for the netlink queries.** A dependency for about a
  hundred lines of attribute parsing; core has none today.

## Consequences

- **Good:**
  - Native CAN interfaces on Linux, and the whole bus as it is: every
    ID, error frames, the kernel's timing.
  - A safe default on a car: core won't listen through a controller
    that could transmit.
  - The path runs end to end with no hardware, on CI too.
- **Bad:**
  - The user configures the interface, with root, before a session;
    the refusal says how, but it's a step the other transports don't
    have.
  - Profiles built on requests don't run on SocketCAN until v0.3.5.
  - Nothing is verified against a real controller until a CAN adapter
    is on hand.
