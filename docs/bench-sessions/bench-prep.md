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

So far all the hardware is one adapter, the OBDX Pro VX, through the
x86 surrogate, and one car, its engine ECU on 11-bit `0x7E8`. The
ELM327 items (#11 on) have met no adapter yet: their run sheets are
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

## The list

| # | Assumption in the code | State | Evidence |
|---|---|---|---|
| 1 | **Flow-control filter shape.** Mask, pattern and flow-control messages are bare 4-byte big-endian CAN IDs: the adapter's ISO-TP stack does the framing, so no PCI byte belongs in the flow-control message. The loader seam requires the flow-control length to equal the mask's, which 4 bytes meets trivially | CONFIRMED for the VX. It replaces the 8-byte shape (`[ID][30 00 00][pad]`) the first session accepted | Accepted through a bare `j2534://` (`OP_START_FILTER` status 0, filter_id 1) on [2026-10-20](2026-10-20.md); carried every reply on both vehicle sessions |
| 2 | **Request framing.** The transport writes the logical message, `[4-byte tx ID][service][key]` (6 bytes for Service 01, 7 for 0x22), with `ISO15765_FRAME_PAD`, so the adapter adds the PCI byte and pads the frame. Some drivers expect both in-band (a 12-byte message) | CONFIRMED for the VX | Service 01 answered on [2026-10-22-vehicle](2026-10-22-vehicle.md); Service 09 and 0x22 on [2026-10-25-vehicle](2026-10-25-vehicle.md) |
| 3 | **No checksum byte.** ISO 15765-4 over CAN, so the K-line checksum doesn't apply | CONFIRMED for the VX | Each reply is the CAN ID and the service data, no PCI, padding or checksum ([2026-10-22-vehicle](2026-10-22-vehicle.md)); segmented replies too ([2026-10-25-vehicle](2026-10-25-vehicle.md)) |
| 4 | **29-bit addressing** whenever either ID is above `0x7FF`: writes carry `CAN_29BIT_ID`, and the channel connects with it | STILL UNKNOWN | Unexercised: the car uses 11-bit IDs |
| 5 | **Timestamps** are microseconds, as the spec says; the transport turns them into session time, counting the 32-bit wrap with the host's clock ([j2534.md](../j2534.md#session-lifecycle), "Frame time") | CONFIRMED for the VX. What sets its clock's zero is unknown: it doesn't reset at open or connect, and it moved across a laptop hibernation | 5,539 stamps over 932 s fit the host clock with a slope of 0.9999993 ([2026-10-22-vehicle](2026-10-22-vehicle.md)); the wrap came mid-connect and session time stayed within 15 ms of the host's ([2026-10-25-vehicle](2026-10-25-vehicle.md)) |
| 6 | **ReadMsgs statuses.** `ERR_TIMEOUT` is a vehicle timeout, `ERR_BUFFER_EMPTY` no frame, anything else a hard failure. Frames are delivered under `STATUS_NOERROR` or `ERR_TIMEOUT`; under any other status the count is ignored, because a vendor may leave it untouched | CONFIRMED for the VX, which never returns `ERR_TIMEOUT` | An empty read returns `ERR_BUFFER_EMPTY` and a short batch `STATUS_NOERROR`, both after the read's full timeout, its TX indications dropped before counting ([2026-10-22](2026-10-22.md)); every reply on both vehicle sessions came with `STATUS_NOERROR` |
| 7 | **Struct layout** conforms to J2534-1 v04.04 ([ADR-0010](../adr/0010-j2534-v0404-shared-types.md)) | CONFIRMED for the VX | Replies decode correctly on a vehicle ([2026-10-22-vehicle](2026-10-22-vehicle.md)) |
| 8 | **Vendor ioctl, filter and timeout behavior** | OBSERVED for the VX | Reads run their full timeout unless the batch fills. With nothing on the bus to acknowledge them, writes sent with timeout 0 are accepted three deep, each with a `TX_INDICATION`; the fourth and every later one blocks about 0.5 s and returns `ERR_FAILED` until a new connect. Filter start and stop and disconnect return status 0 at once ([2026-10-22](2026-10-22.md)). On a vehicle every write returns 0, the ignition off too. After the laptop resumed from hibernation, writes and reads failed with 27 (vendor-specific) until a new connect ([2026-10-25-vehicle](2026-10-25-vehicle.md)). The one ioctl the transport issues, `READ_VBATT`, is #10 |
| 9 | **Export names.** Both backends try the spec spelling `PassThruIoctl` (J2534-1_0500 §7.3.19) first and the legacy `PassThruIoCtl` second, pinned in both directions by the two fakes | RESOLVED | The VX exports the spec spelling ([2026-10-16](2026-10-16.md)) |
| 10 | **Session info readings** ([ADR-0019](../adr/0019-session-info.md)): each connect asks `READ_VBATT` and `PassThruReadVersion`, and the "Session:" line shows them | CONFIRMED for the VX | The voltage is in millivolts: 0.0 V on USB power alone, within 0.1 V of the ECU's own reading (PID 0x42) on the car; versions `FW:1.0.1.0 HW:1.0.0.0`, DLL `1.8.9586.12845`, API `04.04` ([2026-10-25-vehicle](2026-10-25-vehicle.md)) |

### ELM327 ([ADR-0026](../adr/0026-elm327-transport.md))

| # | Assumption in the code | State | Evidence |
|---|---|---|---|
| 11 | **The VX gets a serial port over USB**, which `elm327://COMn` opens at 38400 baud unless `?serial=` says otherwise | STILL UNKNOWN | The dev machine lists only `COM1` with no adapter attached |
| 12 | **The setup's contract commands each answer OK**: `AT Z` with an `ELM327` version, then `AT E0 L0 S0 H1 D0 CAF1 CFC1 SP<n> AT1 ST19`, the header and the receive filter | STILL UNKNOWN | The OBDX manual lists `AT D0` as a compatibility command answering OK; the FRPM lists every one for the STN |
| 13 | **The readings' forms:** `AT I` an `ELM327` version, `STI` `STN<id> vX.Y.Z` on an STN only, `STVR` `12.34` or `AT RV` `12.6V`, read at the connector; an STN's parameter 2A at its default | STILL UNKNOWN | The forms are the datasheet's, the FRPM's and the OBDX manual's; the VX's `READ_VBATT` read 0.0 V on USB power (#10) |
| 14 | **Frame lines with `AT H1`:** an 11-bit ID of three hex digits (or four with a leading 0), the PCI byte, the data, no length digit; replies reassemble and decode to what the VX gave through J2534 | STILL UNKNOWN | The OBDX manual shows no 11-bit line; the parser takes both widths |
| 15 | **No bus** answers `CAN ERROR` or a word like it, which the worker counts as a failed write ("Requests failing") | STILL UNKNOWN | The datasheet: "often … simply from not being connected to a CAN system" |
| 16 | **A lost link** (a cable pulled, Bluetooth dropped) fails the port's reads or writes, or leaves it silent for 5 s, and reopening the port on the backoff brings the adapter back | STILL UNKNOWN | The pseudo-terminal test covers a far side that goes away on Linux and macOS; nothing covers Windows' ports |
| 17 | **Response pending:** an adapter that prints `7F xx 78` goes on listening for the answer (an STN with parameter 2A's bit 2 set does, by the FRPM; a plain ELM327's datasheet doesn't say) | STILL UNKNOWN | The car never sent a `78` to the J2534 sessions |

## Still open

- **#4, 29-bit addressing:** needs a 29-bit vehicle.
- **#5, the VX clock's zero:** nothing depends on it, because session
  time counts from the first frame.
- **#11–#17, the ELM327 items:** the VX at the bench, then the MX+ on
  the car.
- **Every item for any other adapter.** Each CONFIRMED above is the
  VX's. A second adapter, x64 or another vendor's, walks the whole
  list again; the x64 surrogate it would load in has met only the fake
  ([testing.md](../testing.md#gaps-not-verified)).
