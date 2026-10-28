# BENCH prep — what only real hardware can confirm

The J2534 and ELM327 code rests on assumptions about adapters and
buses that the fakes can't test, because each fake is written from the
same assumptions. Each is listed here with what real hardware has shown.
Every session walks this list: the sheet's "Against the BENCH prep
list" table marks each item CONFIRMED, REFUTED (with evidence) or
STILL UNKNOWN, and a refuted item becomes a fix or a known issue in
[STATUS.md](../../STATUS.md#known-issues-and-technical-debt).

The numbers are stable: run sheets, session sheets and ADRs cite them
("BENCH prep #5"). A settled item keeps its row and its number.

So far the J2534 hardware is one adapter, the OBDX Pro VX, through the
x86 surrogate, and one car, its engine ECU on 11-bit `0x7E8`. The
ELM327 items (#11 on) are settled but #17 and passive listening's #18
and #19: the VX on the bench (its
port, answers and a cable pull, no bus) and both adapters on the car
(2026-10-27); their run sheets are
[elm327-bench-runsheet.md](elm327-bench-runsheet.md) (the VX) and
[elm327-vehicle-runsheet.md](elm327-vehicle-runsheet.md) (the MX+,
which has no bench session):

| Session | Kind |
|---|---|
| [2026-10-16](2026-10-16.md) | Bench, quiet bus: first load and connect |
| [2026-10-20](2026-10-20.md) | Bench, quiet bus |
| [2026-10-22](2026-10-22.md) | Bench, quiet bus, the vehicle session's URIs |
| [2026-10-22-vehicle](2026-10-22-vehicle.md) | Vehicle, before the request engine |
| [2026-10-25-bench](2026-10-25-bench.md) | Bench, quiet bus, the laptop that goes to the car |
| [2026-10-25-vehicle](2026-10-25-vehicle.md) | Vehicle: the request engine, the probe, identification, `uds-generic` |
| [2026-10-27-elm327-bench](2026-10-27-elm327-bench.md) | Bench, no bus: the VX as an ELM327, refused at `AT Z`; a probe of its answers |
| [2026-10-27-elm327-bench-2](2026-10-27-elm327-bench-2.md) | Bench, no bus: the VX as an ELM327 connects, goes quiet and recovers from a cable pull |
| [2026-10-27-elm327-vehicle](2026-10-27-elm327-vehicle.md) | Vehicle: the MX+ over Bluetooth and the VX in ELM mode read the car; first ELM327 vehicle read |

## The list

| # | Assumption in the code | State | Evidence |
|---|---|---|---|
| 1 | **Flow-control filter shape.** Mask, pattern and flow-control messages are bare 4-byte big-endian CAN IDs: the adapter's ISO-TP stack does the framing, so no PCI byte belongs in the flow-control message. The loader seam requires the flow-control length to equal the mask's, which 4 bytes meets trivially | CONFIRMED for the VX. It replaces the 8-byte shape (`[ID][30 00 00][pad]`) the first session accepted | Accepted through a bare `j2534://` (`OP_START_FILTER` status 0, filter_id 1) on [2026-10-20](2026-10-20.md); carried every reply on both vehicle sessions |
| 2 | **Request framing.** The transport writes the logical message, `[4-byte tx ID][service][key]` (6 bytes for Service 01, 7 for 0x22), with `ISO15765_FRAME_PAD`, so the adapter adds the PCI byte and pads the frame. Some drivers expect both in-band (a 12-byte message) | CONFIRMED for the VX | Service 01 answered on [2026-10-22-vehicle](2026-10-22-vehicle.md); Service 09 and 0x22 on [2026-10-25-vehicle](2026-10-25-vehicle.md) |
| 3 | **No checksum byte.** ISO 15765-4 over CAN, so the K-line checksum doesn't apply | CONFIRMED for the VX | Each reply is the CAN ID and the service data, no PCI, padding or checksum ([2026-10-22-vehicle](2026-10-22-vehicle.md)); segmented replies too ([2026-10-25-vehicle](2026-10-25-vehicle.md)) |
| 4 | **29-bit addressing** whenever either ID is above `0x7FF`: writes carry `CAN_29BIT_ID`, and the channel connects with it | STILL UNKNOWN | Unexercised: the car uses 11-bit IDs |
| 5 | **Timestamps** are microseconds, as the spec says; the transport turns them into session time, counting the 32-bit wrap with the host's clock ([j2534.md](../j2534.md#session-lifecycle), "Frame time") | CONFIRMED for the VX. What sets its clock's zero is unknown: it doesn't reset at open or connect, and it moved across a laptop hibernation | 5,539 stamps over 932 s fit the host clock with a slope of 0.9999993 ([2026-10-22-vehicle](2026-10-22-vehicle.md)); the wrap came mid-connect and session time stayed within 15 ms of the host's ([2026-10-25-vehicle](2026-10-25-vehicle.md)) |
| 6 | **ReadMsgs statuses.** `ERR_TIMEOUT` is a vehicle timeout, `ERR_BUFFER_EMPTY` no frame, `ERR_BUFFER_OVERFLOW` frames lost (the next frame flagged), anything else a hard failure. Frames are delivered under `STATUS_NOERROR`, `ERR_TIMEOUT` or `ERR_BUFFER_OVERFLOW`; under any other status the count is ignored, because a vendor may leave it untouched | CONFIRMED for the VX, which never returns `ERR_TIMEOUT`; it has never returned `ERR_BUFFER_OVERFLOW` | An empty read returns `ERR_BUFFER_EMPTY` and a short batch `STATUS_NOERROR`, both after the read's full timeout, its TX indications dropped before counting ([2026-10-22](2026-10-22.md)); every reply on both vehicle sessions came with `STATUS_NOERROR` |
| 7 | **Struct layout** conforms to J2534-1 v04.04 ([ADR-0010](../adr/0010-j2534-v0404-shared-types.md)) | CONFIRMED for the VX | Replies decode correctly on a vehicle ([2026-10-22-vehicle](2026-10-22-vehicle.md)) |
| 8 | **Vendor ioctl, filter and timeout behavior** | OBSERVED for the VX | Reads run their full timeout unless the batch fills. With nothing on the bus to acknowledge them, writes sent with timeout 0 are accepted three deep, each with a `TX_INDICATION`; the fourth and every later one blocks about 0.5 s and returns `ERR_FAILED` until a new connect. Filter start and stop and disconnect return status 0 at once ([2026-10-22](2026-10-22.md)). On a vehicle every write returns 0, the ignition off too. After the laptop resumed from hibernation, writes and reads failed with 27 (vendor-specific) until a new connect ([2026-10-25-vehicle](2026-10-25-vehicle.md)). The one ioctl the transport issues, `READ_VBATT`, is #10 |
| 9 | **Export names.** Both backends try the spec spelling `PassThruIoctl` (J2534-1_0500 §7.3.19) first and the legacy `PassThruIoCtl` second, pinned in both directions by the two fakes | RESOLVED | The VX exports the spec spelling ([2026-10-16](2026-10-16.md)) |
| 10 | **Session info readings** ([ADR-0019](../adr/0019-session-info.md)): each connect asks `READ_VBATT` and `PassThruReadVersion`, and the "Session:" line shows them | CONFIRMED for the VX | The voltage is in millivolts: 0.0 V on USB power alone, within 0.1 V of the ECU's own reading (PID 0x42) on the car; versions `FW:1.0.1.0 HW:1.0.0.0`, DLL `1.8.9586.12845`, API `04.04` ([2026-10-25-vehicle](2026-10-25-vehicle.md)) |

### ELM327 ([ADR-0026](../adr/0026-elm327-transport.md))

| # | Assumption in the code | State | Evidence |
|---|---|---|---|
| 11 | **The VX gets a serial port over USB**, which `elm327://COMn` opens at 38400 baud unless `?serial=` says otherwise | CONFIRMED for the VX on Windows, with the OBDX software installed. Whether the port appears on a machine without OBDX's INF is unknown | The VX is USB `VID_0483&PID_5740`, STMicroelectronics' virtual COM port (USB CDC): "OBDX Pro Serial (COM4)", `\Device\USBSER000`, Microsoft's `usbser.sys` bound by OBDX's `obdxdriver.inf`. `elm327://COM4` opened it, wrote and read it and closed it ([2026-10-27-elm327-bench](2026-10-27-elm327-bench.md)). A CDC device needs no vendor driver on Windows 10 and later, macOS (`/dev/cu.usbmodem*`) or Linux (`/dev/ttyACM*`) |
| 12 | **The setup's contract commands each answer OK**: `AT Z` with an `ELM327` version, or `OK` alone and then `AT I` with one; then `AT E0 L0 S0 H1 D0 CAF1 CFC1 SP<n> AT1 ST19`, the header and the receive filter | CONFIRMED for the VX, in the rule's second form (`AT Z` first had to answer a version: ADR-0026's appendix of 2026-10-27, "the VX's answers"); CONFIRMED for the MX+ (STN2255) on the car | The VX answers `AT Z` with `OK` alone, as its Command Set Reference has it, which refused the first session ([2026-10-27-elm327-bench](2026-10-27-elm327-bench.md)); in the second, `AT I` gave `ELM327 V2.1` and every contract command `OK`, `AT SP 8` too, each prompt within 0.3 ms ([2026-10-27-elm327-bench-2](2026-10-27-elm327-bench-2.md)); on the car the MX+ ran `AT Z` through `AT ST 19` all `OK` ([2026-10-27-elm327-vehicle](2026-10-27-elm327-vehicle.md)). The FRPM lists every one for the STN |
| 13 | **The readings' forms:** `AT I` an `ELM327` version, `STI` `STN<id> vX.Y.Z` on an STN only, `STVR` `12.34` or `AT RV` `12.6V`, read at the connector; an STN's parameter 2A at its default | CONFIRMED for both | The VX: `AT I` `ELM327 V2.1`, `STI` `?`, `AT RV` `0.00V` on USB power, as its J2534 `READ_VBATT` read (#10); the session line "ELM327 adapter (ELM327 V2.1); …; battery 0.0 V" ([2026-10-27-elm327-bench-2](2026-10-27-elm327-bench-2.md)); the MX+ (STN2255 v5.12.2): `AT I`/`STI` versions, `STVR` 12.1 V with the ignition off, the VX's Phase 0 reading on the same car (11.6 V with the ignition on), parameter 2A at default ([2026-10-27-elm327-vehicle](2026-10-27-elm327-vehicle.md)). The STN's forms are the FRPM's |
| 14 | **Frame lines with `AT H1`:** an 11-bit ID of three hex digits (or four with a leading 0), the PCI byte, the data, no length digit; replies reassemble and decode to what the VX gave through J2534 | CONFIRMED for both | Both adapters' 11-bit replies print as `7E8` (three hex digits, no length digit), the multi-frame ones (the VIN, the CALID, the ECU name, the DIDs) reassemble, and the values decode as the VX's J2534 sessions' did: the MX+ through a cold start and idle, the VX in ELM mode through a start and idle ([2026-10-27-elm327-vehicle](2026-10-27-elm327-vehicle.md), its `vx_elm/` logs). The OBDX manual shows no 11-bit line; the parser takes both widths |
| 15 | **No bus** answers `CAN ERROR` or a word like it, which the worker counts as a failed write ("Requests failing") | REFUTED for the VX: it answers `NO DATA`, which the worker leaves to the engine's wait, so no bus reads as quiet. Nothing to fix: it can't tell a missing bus from a silent ECU. The MX+ never meets a missing bus (it powers up only on the car) | Every request `NO DATA`, the link quiet 2.4 s after the connect with no episode. The first three after each setup 100 ms after the request (`AT ST 19`), every later one 300 ms: with nothing to acknowledge them the VX holds three frames, as its J2534 writes do (#8) ([2026-10-27-elm327-bench](2026-10-27-elm327-bench.md), [2026-10-27-elm327-bench-2](2026-10-27-elm327-bench-2.md)). The datasheet: `CAN ERROR` "often … simply from not being connected to a CAN system" |
| 16 | **A lost link** (a cable pulled, Bluetooth dropped) fails the port's reads or writes, or leaves it silent for 5 s, and reopening the port on the backoff brings the adapter back | CONFIRMED for the VX on USB and the MX+'s Bluetooth | Unplugged, the read failed at once and the link went to "Adapter fault"; the reopens failed "no such port" on the backoff, and the first after the plug went back in reopened COM4 and ran the setup, with no reconnect and nothing printed unasked ([2026-10-27-elm327-bench-2](2026-10-27-elm327-bench-2.md)); the MX+'s Bluetooth turned off failed the port at once, the reopens failed "no such port" until it was back, and the next reopened COM6 and ran the setup, the first value within ~6.5 s of Bluetooth on (the backoff's 5 s and the setup), with no reconnect ([2026-10-27-elm327-vehicle](2026-10-27-elm327-vehicle.md)). The pseudo-terminal test covers a far side that goes away on Linux and macOS |
| 17 | **Response pending:** an adapter that prints `7F xx 78` goes on listening for the answer (an STN with parameter 2A's bit 2 set does, by the FRPM; a plain ELM327's datasheet doesn't say) | STILL UNKNOWN | The car sent no `7F xx 78` to the J2534 sessions or to the MX+ ([2026-10-27-elm327-vehicle](2026-10-27-elm327-vehicle.md)) |
| 18 | **The monitor's start and stop** (ADR-0026's appendix "passive listening"): `AT MA` prints nothing until a frame passes the receive filter; one space stops it with `STOPPED` and the prompt, and a space at the prompt is ignored; the 5 s check stops and restarts it on a quiet bus with no failure | STILL UNKNOWN | Fake-tested only. The datasheet and the FRPM give the stop (any character, discarded, `STOPPED` then the prompt) and that a space is ignored at the prompt; the OBDX manual gives `STOPPED`. To run: the VX on USB, no bus, `elm327://COMn?profile=raw`, the wire log's monitor lines |
| 19 | **Listening hears another tester's replies:** `AT MA` with the setup's `AT CRA` passes the channel's reply ID and nothing else, the monitor never acknowledges, and the replies reassemble as a request's do, multi-frame ones too once the other tester sends the flow control | STILL UNKNOWN | Fake-tested only. Needs an OBD Y-splitter: the VX polling through J2534 and the MX+ listening through `elm327://COMn?profile=raw`, the listener's messages compared with the poller's recording |

## Still open

- **#4, 29-bit addressing:** needs a 29-bit vehicle.
- **#5, the VX clock's zero:** nothing depends on it, because session
  time counts from the first frame.
- **#11, a machine without OBDX's INF:** whether the VX's port appears
  there.
- **#17, response pending:** needs an ECU that answers `7F xx 78`.
- **#18, the monitor's start and stop:** a VX bench session, no bus.
- **#19, listening to another tester:** a car and an OBD Y-splitter.
- **Every item for any other adapter.** Each CONFIRMED above is the
  VX's. A second adapter, x64 or another vendor's, walks the whole
  list again; the x64 surrogate it would load in has met only the fake
  ([testing.md](../testing.md#gaps-not-verified)).
