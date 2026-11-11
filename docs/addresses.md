# Addresses

An address tells Havne where a session's frames come from and how to
read them. It is the text in the field on Settings' Connection page,
the argument `havne-cli` takes (`stream`, `log`, `clear` and the
rest), and what the Python bindings take to open a session. One grammar
covers all of them. The [user guide](user-guide.md) says where each
task starts; this page is every form and every option.

```
<scheme>://<where>[?<key>=<value>[&<key>=<value>...]]
```

- The scheme says which kind of source: `mock://`, `replay://`,
  `elm327://`, `socketcan://` or `j2534://`.
- `<where>` names the source: a label, a recording, a port, an
  interface or an adapter.
- Each `key=value` after the `?` is an option. Options are separated by
  `&`.

An address is checked when the session is created. A malformed one is
refused before any port, driver or interface is opened, with a message
that names the key and what it should hold. A well-formed address can
still fail after that, at the hardware: a port that won't open, an
adapter that doesn't answer, an interface that is down.

## Rules every address follows

- Keys are lower case. `Profile=obd2` is refused.
- A key the scheme doesn't take is refused, and the message lists the
  keys it does take.
- A key given twice takes its last value, except `did`, whose values add
  up. Every value given is checked, so a bad first value is refused even
  when a good one follows.
- An option with no `=`, an option with nothing after its `=`, an empty
  option (`&&`, a leading or trailing `&`) and a bare trailing `?` are
  refused.
- At most 16 `key=value` pairs.
- Numbers are decimal, or hexadecimal with a `0x` prefix (`0x7E8`). A DID
  in `did=` is bare hexadecimal with no prefix.
- Nothing is percent-decoded. The options start at the first `?`, so a
  path or a value can't hold a `?` or an `&`.
- Paths must be absolute: on Windows a drive (`C:\` or `C:/`) or a
  network share (`\\server\...`), elsewhere a leading `/`.

## Which scheme takes which option

| Option | `mock://` | `replay://` | `elm327://` | `socketcan://` | `j2534://` |
|---|---|---|---|---|---|
| `profile` | yes | yes | yes | yes | yes |
| `did` | yes | yes | yes | yes | yes |
| `diagnose` | yes | no | yes | requesting only | yes |
| `speed`, `loop`, `from`, `to`, `faults`, `seed` | no | yes | no | no | no |
| `baud` | no | no | yes | yes | yes |
| `serial` | no | no | yes | no | no |
| `flags` | no | no | no | no | yes |
| `filter` | no | no | yes | yes | yes |
| `txid`, `poll`, `probe`, `identify`, `pack`, `scan`, `session`, `keepalive_ms` | no | no | yes | requesting only | yes |

"Requesting only" on `socketcan://` means a profile that asks an ECU
(`obd2`, `uds-generic`, or a signal file's or a plugin's that declares
ECUs). A listening session on `socketcan://` refuses those keys, with a
message saying the profile only listens. `did` is taken everywhere so
that a profile that can't use it is refused for what it is: `did` with
any profile but `uds-generic` is refused.

## The sources

### mock://

```
mock://<label>[?profile=<name>][&did=<entries>][&diagnose=0|1]
```

A simulated vehicle that needs no hardware. The label is free text and
only names the session (`mock://demo`). The default profile is `mock`:
a drive cycle with engine, boost, temperatures and a diagnostic report.
`diagnose=0` turns the simulated ECU's trouble codes, readiness and
freeze frame off; the default is on.

```
mock://demo
mock://demo?diagnose=0
```

### replay://

```
replay://<absolute path>[?speed=<factor>][&loop=0|1][&from=<s>][&to=<s>][&faults=<list>][&seed=<n>][&profile=<name>][&did=<entries>]
```

Plays a recording back as a session. The path is a recording's folder,
or the `frames.csv` file inside it: a path ending `.csv` (or `.CSV`) is
taken as the file, anything else as the folder. A relative path is
refused. The default profile is `raw`, which shows the frames and
decodes nothing; a recording made under a profile reads with it through
`profile=`.

| Option | Holds | Default | Meaning |
|---|---|---|---|
| `speed` | 0.1 to 100, up to three decimals, or `max` | 1 | Playback speed as a factor of the recording's own timing. `max` plays with no waiting. |
| `loop` | `0` or `1` | 0 | `1` starts over at the end. A recording with no span of time can't loop and is refused. |
| `from` | seconds, up to three decimals | the start | Where to start, in the recording's own seconds (the viewer's, `view`'s and `info`'s). |
| `to` | seconds, up to three decimals | the end | Where to stop. Must be after `from`. A stretch with no frames in it is refused with the range the recording does hold. |
| `faults` | `<kind>:<percent>`, joined by `,` | none | Damages the frames on the way out, to test how a consumer copes. Kinds: `drop`, `overflow`, `corrupt`, `truncate`, and `stall:<percent>:<ms>` (1 to 10000 ms). Each kind once; a percent above 0 and at most 100, up to three decimals. |
| `seed` | 1 to 4294967295 | 1 | Seeds the choice of damaged frames, so the same address gives the same damage on every run and every platform. |

```
replay:///data/recordings/2026-11-02-drive
replay://D:/recordings/run1/frames.csv?speed=4
replay:///data/recordings/run1?from=100&to=160&loop=1
replay:///data/recordings/run1?faults=drop:2,stall:0.5:200&seed=7
```

The second example is a Windows path; the others are Linux or macOS
paths.

### elm327:// over a serial port

```
elm327://<port>[?baud=<rate>][&serial=<rate>][&profile=...][&<request options>]
```

An ELM327 or STN adapter on a USB port, or on a Bluetooth port the
computer has paired. The port is `COM1` to `COM255` on Windows (no
leading zero: `COM05` is refused), and a `/dev/` path elsewhere
(`/dev/ttyUSB0`, `/dev/rfcomm0`). The default profile is `obd2`.

| Option | Holds | Default | Meaning |
|---|---|---|---|
| `baud` | `500000` or `250000` | 500000 | The vehicle's CAN rate, which picks the adapter's protocol. |
| `serial` | 300 to 4000000 | 38400 | The serial port's own bit rate. A Bluetooth port ignores it. |

```
elm327://COM5
elm327://COM5?serial=115200
elm327:///dev/ttyUSB0?baud=250000&profile=raw
```

### elm327:// over TCP (a WiFi adapter)

```
elm327://<a.b.c.d>:<port>[?baud=<rate>][&profile=...][&<request options>]
```

A WiFi adapter, or `havne-cli sim --elm327`, reached as a socket: four
decimal numbers from 0 to 255, a colon and a port from 1 to 65535. Host
names, IPv6 and leading zeros aren't taken. `serial` is refused, since a
socket has no bit rate. The other options are the serial form's.

```
elm327://192.168.0.10:35000
elm327://192.168.4.1:23?profile=raw&poll=0x0C
```

### elm327:// over an app's stream

```
elm327://<kind>:<id>[?...]
```

A link the app opens itself and hands to the session, as the phone app
does for Bluetooth: `bt:<address>` for a paired adapter and
`ble:<address>` for a Bluetooth LE adapter. The app makes these
addresses when an adapter is picked from its list; only a stream the
app has opened connects. `serial` is refused. All other options are the
serial form's.

```
elm327://bt:<the adapter's address>
```

### socketcan:// (Linux)

```
socketcan://<interface>[?profile=...][&baud=<rate>][&filter=<ids>][&<request options>]
```

A CAN interface that is already up (`can0`, `vcan0`); Havne never
configures it. The interface name is 1 to 15 letters, digits, `_`, `-`
or `.`. Not available on Windows, macOS or Android.

The default profile is `raw`, which listens: the interface must be a
listen-only controller (or a virtual `vcan`), and nothing is sent. A
profile that asks an ECU (`profile=obd2`, `uds-generic`) sends
requests, which needs a controller that can send, at the right rate.

| Option | Holds | Default | Meaning |
|---|---|---|---|
| `baud` | `500000` or `250000` | none | The rate the controller must already be running at. Refused if it differs. A requesting session on a real controller needs it, so the rate is proven rather than assumed. A virtual interface has no rate and ignores it. |
| `filter` | listening: 1 to 8 CAN IDs joined by `,`, each with an optional `/mask`; requesting: one receive ID | the whole bus (listening) | Listening: which frames to hear (`0x7E8,0x18DA00F1/0x1FFF00FF`). An ID above 0x7FF is 29-bit; a mask left out matches the whole ID. Requesting: the one ID replies arrive on. |

```
socketcan://can0
socketcan://can0?baud=500000&filter=0x7E8,0x7E9
socketcan://can0?profile=obd2&baud=500000
socketcan://vcan0?profile=obd2
```

### j2534:// (Windows)

```
j2534://[<selector>][?baud=<rate>][&flags=<bits>][&profile=...][&<request options>]
```

A J2534 pass-thru adapter through its vendor's driver. Not available
off Windows. The default profile is `obd2`.

| Selector | Meaning |
|---|---|
| (empty) | The first adapter found. |
| a name | An installed adapter whose name matches, without regard to case. |
| a path | A driver DLL loaded directly. Anything holding `\` or `/`, or ending `.dll`, is a path, and must be absolute. |

| Option | Holds | Default | Meaning |
|---|---|---|---|
| `baud` | a nonzero number | 500000 | The CAN rate passed to the adapter. |
| `flags` | a number | 0 | Connect flags, passed to the adapter as given. The 29-bit flag is added when the channels use 29-bit IDs. |

```
j2534://
j2534://<the adapter's name>
j2534://?profile=raw&poll=0x0C
```

## Options every scheme with a profile takes

### profile

`profile=<name>` chooses how frames are read. Left out, each scheme has
its own default: `mock` for `mock://`, `raw` for `replay://` and
`socketcan://`, `obd2` for `elm327://` and `j2534://`. The name is 1 to
63 characters and matched exactly, so `OBD2` is refused.

| Profile | What it does |
|---|---|
| `mock` | The simulated vehicle's signals. |
| `raw` | Frames only: no decoding, no requests of its own. On `elm327://` and `j2534://` it listens to the engine ECU, and with `poll=` it also asks. |
| `obd2` | The standard OBD-II readings, with the requests, the supported-PID probe, vehicle identification and trouble codes. |
| `uds-generic` | Service 0x22 reads of the DIDs `did=` lists, for finding a manufacturer's data. |
| `user.<name>` | A signal file loaded by the app or by `havne-cli --signals` ([signal-files.md](signal-files.md)). |
| `plugin.<id>.<variant>` | A variant of a loaded plugin. |

An unknown name is refused.

### did

`did=<entry>[,<entry>...]` lists the DIDs `uds-generic` reads. It is
required with that profile and refused with any other.

```
entry := [<ecu>:]<DID>[-<DID>][:<type>]
```

- A DID is 1 to 4 hexadecimal digits, with no prefix.
- `<ecu>` is the ECU's response ID in bare hexadecimal (`7E9`, not
  `0x7E9`). An 11-bit ID from `9` to `7FF` is asked on the ID 8 below it
  (`7E9` is asked on `7E1`); a 29-bit `18DA<a><b>` is asked on
  `18DA<b><a>`. With no ECU, `7E8` is used and asked on `7E0`.
- A single DID is polled over and over. A range (`1E00-1EFF`) is
  scanned once, and each DID the ECU answers joins the polled set.
- `<type>` is `hex` (the default: the reply's bytes as pairs), `text`,
  `u8`, `u16`, `u24`, `u32`, `i8`, `i16`, `i24` or `i32` (big-endian, no
  scaling).
- Several `did=` keys add up. A DID listed twice is kept once. At most
  256 DIDs to poll, 1024 to scan and 4 ECUs.

```
elm327://COM5?profile=uds-generic&did=1E1C,1E1D:u16
j2534://?profile=uds-generic&did=7E9:1E00-1E1F
```

## Request options

These apply to `elm327://`, `j2534://` and a requesting `socketcan://`.
They shape what Havne asks the car.

| Option | Holds | Default | Meaning |
|---|---|---|---|
| `filter` | a CAN ID, 1 to 0x1FFFFFFF | the profile's | The receive ID. Replaces the profile's ECUs with this one. |
| `txid` | a CAN ID, 1 to 0x1FFFFFFF | the profile's, else 0x7E0 | The transmit ID. With no `filter`, every ECU of the profile asks on it. |
| `poll` | a PID, 0x00 to 0xFF | none | Asks this one Service 01 PID, over and over, instead of the profile's rotation. `0x00` asks the supported-PID bitmap. Needs a channel, from `filter` or the profile. On `raw` over `elm327://` or `j2534://` it is what makes the session ask; `socketcan://` refuses it with `raw`, which only listens there. |
| `probe` | `0` or `1` | 1 | Walk the car's supported-PID bitmaps first and drop what it can't answer. `0` skips the walk: the profile's requests go out as written. Off with `poll`. |
| `identify` | `0` or `1` | 1 | Read the VIN, calibration IDs and other identification at the start. Off with `poll`, and only in a session that asks something. |
| `diagnose` | `0` or `1` | 1 | Read trouble codes, readiness and the freeze frame at the start. Off with `poll`, and only in a session that asks something. |
| `pack` | `0` or `1` | 1 | Ask several Service 01 PIDs in one request, when the car answers that way. Only with the probe on. `0` asks one PID per request. |
| `scan` | `turns` or `first` | `turns` | With `uds-generic` ranges: `turns` alternates the scan with the polled DIDs, `first` asks the whole scan before polling. `first` is refused with any other profile. |
| `session` | `01` or `03` | `01` | With `uds-generic`: `03` enters the extended diagnostic session before the reads, keeps it alive, and returns to the default session at the end. A module that refuses it is read in the default session, with the refusal named. `01` sends no session request. `03` is refused with any other profile. The value is a number, so `3` and `0x03` are `03`. |
| `keepalive_ms` | 500 to 4500 | 2000 | How long with nothing sent before a tester-present goes out. Needs `session=03`. |

Channels, whether from the profile or from `filter` and `txid`, must all
be 11-bit or all 29-bit. A session with nothing to ask and no channel
is refused.

```
elm327://COM5?poll=0x0C
elm327://COM5?profile=obd2&probe=0&identify=0
j2534://?profile=uds-generic&did=7E9:D200-D203&session=03&keepalive_ms=1500
socketcan://can0?profile=obd2&baud=500000&diagnose=0
```

## The simulated ECU (havne-cli sim)

`havne-cli sim` takes addresses of its own, which describe the car it
plays, not a session to read one with.

| Address | Meaning |
|---|---|
| `mock://sim[?values=drive\|canned][&modules=1\|2][&profile=<name>][&vin=<VIN>][&faults=<list>]` | The simulated car: moving `drive` values (the default) or fixed `canned` ones; `modules=2` adds a second module; `profile=` serves a profile's DIDs, and isn't taken with `modules=2`. |
| `replay://<absolute path>[?loop=0\|1][&from=<s>][&to=<s>][&vin=<VIN>][&faults=<list>]` | A recording's replies, played as an engine ECU. |

- `vin=` is 17 characters of the VIN alphabet, upper case, with no I, O
  or Q. Left out, the simulated car's own made-up VIN is used.
- `faults=` takes the simulated ECU's own kinds, each `<kind>:<n>`
  (every n-th reply, n from 1 to 10000), joined by `,`: `silent` (no
  reply), `late`, `pending` (a "response pending" first, then the
  reply), `bad_sequence` and `stalled`. The last two touch only replies
  of more than one frame. It isn't the replay session's `faults`.

`havne-cli --help` gives the command's own flags (`--on`, `--elm327`,
`--desk-bus` and the rest).

## Where each option's reasons live

The design decisions behind the grammar are in the ADRs:
[profiles](adr/0013-protocol-profiles.md),
[the request engine](adr/0015-request-engine.md),
[uds-generic](adr/0020-uds-generic.md),
[replay](adr/0023-replay-transport.md),
[J2534](adr/0025-one-j2534-backend.md),
[ELM327](adr/0026-elm327-transport.md),
[app streams](adr/0028-app-fed-streams.md),
[SocketCAN listening](adr/0032-socketcan-listening.md) and
[requests](adr/0040-socketcan-requests-isotp.md),
[Service 01 coverage](adr/0039-full-service01-coverage.md),
[replay faults](adr/0041-replay-fault-injection.md),
[the simulated ECU](adr/0046-simulated-ecu.md) and
[the extended session](adr/0060-extended-session-reads.md).
