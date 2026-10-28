# ADR-0028: App-fed streams — the app opens the Bluetooth links core can't, and an `elm327` session runs over them

- **Status:** Accepted.
- **Date:** 2026-10-28.
- **Builds on:** [ADR-0026](0026-elm327-transport.md) (the ELM327
  transport, whose §1 leaves the byte stream as the seam for this
  record, and whose appendix "WiFi over TCP" gives the mobile apps
  their first route to a car); [ADR-0001](0001-qt-free-core-c-abi.md)
  (the C ABI is the boundary, and core never links Qt).

## Context

`elm327://` runs over a port core opens itself: a serial port (USB,
or classic Bluetooth that the OS turns into one) or a TCP socket. Two
kinds of link don't fit that:

- **Bluetooth on Android.** Android has no serial device for a paired
  adapter. A classic Bluetooth (SPP) adapter such as the OBDLink MX+
  is an RFCOMM socket opened through the Java API.
- **Bluetooth LE everywhere.** A BLE adapter (the OBDLink CX, most
  cheap BLE dongles, the OBDX family's BLE models) carries the ELM327
  text over a GATT service: one characteristic the host writes and
  one that notifies. No OS presents that as a port.

Qt reaches both through QtBluetooth (`QBluetoothSocket`,
`QLowEnergyController`), which lives in the app layer. Core reaching
them itself would mean four native Bluetooth stacks (BlueZ, WinRT,
CoreBluetooth, Android's Java API) in a Qt-free library, and Android's
needs Java regardless.

What is on hand decides what can be verified: an Android phone, the
OBDX Pro VX (USB and WiFi), and the OBDLink MX+ (classic Bluetooth,
powered from the OBD connector only, so a car is its bench). There is
no BLE adapter, no Mac and no iPhone. The MX+ reaches an iPhone
through Apple's accessory protocol, as OBDLink documents it, which
QtBluetooth doesn't speak; on iOS only BLE adapters and WiFi are
reachable.

Known Issue #2 records that the Android app has never been launched.
Since ADR-0026's WiFi appendix, a phone on an adapter's WiFi could
open `elm327://192.168.4.1:23` with no new code. Nobody has tried
that.

## Decision

### 1. A stream the app registers by name, fed by pushes

A new public header, `ecuio/stream.h`, lets the app stand behind an
`elm327` port:

```c
typedef struct ecuio_stream ecuio_stream_t;   /* opaque */

typedef struct ecuio_stream_callbacks {
    /* Core wants the link: start connecting and return at once. */
    void (*open)(void* user);
    /* Bytes for the adapter: queue them and return at once. */
    void (*write)(void* user, const uint8_t* data, uint32_t len);
    /* Core is done with the link: disconnect and return at once. */
    void (*close)(void* user);
    void*    user;
    uint32_t flags;          /* 0 */
    uint32_t reserved[4];    /* 0 */
} ecuio_stream_callbacks_t;

ecuio_status_t ecuio_stream_register(const char* name,
                                     const ecuio_stream_callbacks_t* cb,
                                     ecuio_stream_t** out);
ecuio_status_t ecuio_stream_unregister(ecuio_stream_t* stream);

/* The app's pushes, from any thread: */
void ecuio_stream_opened(ecuio_stream_t* stream);
void ecuio_stream_received(ecuio_stream_t* stream,
                           const uint8_t* data, uint32_t len);
void ecuio_stream_failed(ecuio_stream_t* stream, const char* reason);
```

- **Names:** `<kind>:<id>`, the kind in lower-case letters. The app
  uses `bt:<address>` for classic Bluetooth and `ble:<address>` for
  BLE. Core doesn't interpret the id. A registered name is a port to
  `elm327://`, so `elm327://bt:00:11:22:33:44:55?profile=obd2` reads
  as every other `elm327://` URI does, keys included. A name with a
  kind that isn't registered is `ECUIO_ERR_LOAD_FAILED`, "no such
  port", as a missing COM port is.
- **Open:** the port's opener calls `open`, then waits up to 10 s for
  `opened` or `failed`. A BLE connect with its service discovery takes
  seconds. Attach runs on the bridge's connect thread and the
  reconnect on the worker, so neither wait blocks the GUI thread,
  where QtBluetooth delivers the app's signals. A session stop ends
  the wait. `failed`'s reason, or "the link didn't open within 10 s",
  is the `LOAD_FAILED` detail.
- **Bytes in:** `received` copies into the port's buffer, which the
  worker's reads drain under ADR-0026's timeouts. The buffer is a
  fixed 64 KB, allocated when the stream registers, far more than an
  adapter sends between two of the worker's reads. Bytes past it are
  dropped and the read after reports the port failed, because a lost
  run of ELM327 text can't be resynchronized. Bytes received while no
  session holds the stream are discarded.
- **Bytes out:** `write` runs on the thread that writes (connect
  thread, then worker) and must not block. The app queues the bytes to
  its own thread and splits them to its link's size (a BLE write's
  payload is the ATT MTU less 3, 20 bytes before negotiation). A write
  that fails afterward is a `failed`.
- **Failure:** `failed` ends the stream's link. The worker's next read
  or write sees the port failed, which ADR-0026 §6 maps to a hard
  failure: an episode, then a reopen on the backoff. The reopen calls
  `open` again, so the backoff paces the app's reconnects, and the app
  never retries on its own.
- **Close:** core calls `close` when the session closes or before a
  reopen. Pushes after it are discarded until the next `open`.
- **Lifetime:** `unregister` returns `ecuio_status_t` and is refused
  while a session holds the stream (`ECUIO_ERR_INVALID_ARGUMENT`): the
  bridge closes the session first. After a successful `unregister`, no
  callback runs. Callbacks may run on
  any core thread and must never wait on the thread that unregisters.
- **ABI:** `flags` and `reserved` are zero. The header compiles as
  strict C (`test_c_abi`), and enum values and statuses are
  append-only, as for every public header.

Core's port glue is a third opener beside the serial port and the TCP
socket (`core/src/elm327_stream.{h,cpp}`), so the protocol layer, the
worker, link health, the wire log and passive listening run unchanged.
The wire log names the link "bt:… (app stream)".

### 2. The app's Bluetooth, Android first

The bridge gains the app side (`app/btstream.{h,cpp}`, built when
QtBluetooth is found; on Android only for now):

- **Classic Bluetooth:** a `QBluetoothSocket` (RFCOMM) to the SPP
  service UUID `00001101-0000-1000-8000-00805F9B34FB` on a paired
  adapter. Pairing stays in Android's settings, as it is on the
  desktop systems.
- **BLE:** a `QLowEnergyController`. The service is the first match
  in a fixed list, checked in order:
  1. the Nordic UART service (`6E400001-…`; write `6E400002-…`,
     notify `6E400003-…`), which the OBDX manual names;
  2. `FFF0` (write `FFF2`, notify `FFF1`);
  3. `FFE0` (`FFE1` both writes and notifies);
  4. otherwise the first service with one notifying characteristic
     and one writable characteristic.

  The list comes from what adapters are commonly reported to use. None
  is verified here (§5). The picker is a pure function over the
  discovered services, so a test can check it without hardware.
- **Settings' device list** gains the phone's Bluetooth adapters: the
  paired classic devices, and BLE devices found by a scan started
  from a button (a scan costs battery and needs the permission
  below). Each row composes the `bt:` or `ble:` URI, as a serial
  port's row does.
- **Permissions:** Android 12 and later ask for `BLUETOOTH_CONNECT` and
  `BLUETOOTH_SCAN` (declared "never for location"), through Qt's
  `QBluetoothPermission` when the list first needs them. The manifest
  declares `INTERNET` explicitly, because core's TCP socket needs it,
  rather than leaving it to the permissions Qt adds by default. Android 11 and earlier need location permission for a
  scan; the minimum SDK decides whether that path exists, and the
  phone run settles it.

Desktop BLE uses the same code once a BLE adapter is on hand to verify
it; until then the desktop builds don't compile it, so nothing ships
unverified. iOS (BLE through `QLowEnergyController`, WiFi with the
local-network permission's `NSLocalNetworkUsageDescription`) is a later
step of this record; with no Mac it stays CI-built only, and Known
Issue #2 says so.

### 3. Getting the APK onto a phone

CI's `android-build` uploads the debug-signed APK (`apk` target) as a
run artifact, kept for a short period. That is the only way onto the
phone while no Android kit is installed locally. Release builds and a
signing key are not part of this record: the release workflow stays
Windows-only until Known Issue #2 closes.

On Android, `TMPDIR` (the wire log's directory, `elm327_log.cpp`) and
`DocumentsLocation` (recordings) both point into the app's private
storage. The phone run records where each lands, and the run sheet
says how to copy them off with `adb` (`run-as` works on a debug
build).

### 4. Verification

| Tier | What |
|---|---|
| HARNESS (`test_elm327`, Qt-free, every CI job) | The fake ELM327 behind `ecuio_stream_*`: a pump thread plays the app (`open` → `opened`, `write` → the fake → `received`). A whole session's setup and polling through it; `failed` mid-session → hard failure, `close`, then `open` again on the backoff and recovery; an open that never answers, refused after its wait; a stop during that wait; an unregistered kind; `unregister` refused while held; bytes past the buffer; pushes after `close` discarded |
| UNIT (`test_c_abi`) | `stream.h` as strict C |
| UNIT (`test_bridge`) | The BLE service picker over made-up service lists; the device list's URI rows |
| BENCH (the phone) | The APK launches; `mock://` and `replay://`; the VX's WiFi, `elm327://192.168.4.1:23`; the permissions prompts; where the logs and recordings land |
| Vehicle | The MX+ over Android's classic Bluetooth on the car (its bench, since it takes power only from the car) |

BLE stays fake-tested until a BLE adapter is on hand; a BENCH prep
item records it as open.

### 5. Order of work

1. **Android baseline:** the APK artifact; `INTERNET` declared; the
   phone run of `mock://`, `replay://` and the VX's WiFi. Known Issue
   #2 narrows to what remains.
2. **Core:** `stream.h`, the stream opener, the HARNESS cases.
3. **App:** `btstream`, the device list, the permissions, the
   picker's UNIT cases.
4. **The car:** the MX+ through the phone.
5. **Later:** iOS (BLE and WiFi; CI-built only), desktop BLE (once a
   BLE adapter is on hand).

## Alternatives considered

- **Make the internal port ops public: the app supplies blocking
  `read` and `write`.** Core's worker would then wait inside the app's
  read, which has to wait on a queue that the GUI thread fills. Every
  front end would write that queue again, with core's timeouts, and a
  front end's mistake would hang the worker. With pushes, core owns
  the buffer and the timing, and the app never blocks.
- **The app connects first and hands core an open stream.** That works
  for the connect, but a dropped link then needs a second path for the
  reconnect, outside the backoff ADR-0026 §6 already gives every port.
  With `open` as a callback, one path serves both, and the backoff
  paces the app.
- **Bluetooth in core.** Four native stacks, one of them Java-only.
  Core would gain a Bluetooth dependency on every platform for a link
  the app's framework already reaches.
- **A scheme of its own (`ble://`).** The protocol, the keys and the
  worker are the ELM327's; only the port differs, as with TCP.
- **Name the link by the adapter's advertised name, not its address.**
  It reads better, but it isn't unique, and the URI must reopen the
  same adapter. The address is what the platform reconnects by (on
  iOS a per-phone UUID). It is device-identifying, like an adapter's
  serial number, so `bench-sessions.md`'s scrubbing covers Bluetooth
  addresses too.

## Consequences

- An Android phone can read a car through the MX+, and through any
  WiFi ELM327, once the steps land. The request engine, the profiles
  and link health are the desktop's.
- A new public header and five calls join the C ABI: a non-Qt front
  end gets the same seam.
- QtBluetooth (Qt's `qtconnectivity` module) becomes a dependency of
  the Android build, installed in CI. The desktop builds and the
  Qt-free jobs don't need it.
- URIs, vehicle profiles and recordings' `events.log` can carry a
  Bluetooth address. It stays on the user's device; committed session
  records scrub it.
- BLE is built but verified only against the fake until a BLE adapter
  is on hand. iOS remains unverified on a device.
- The 10 s open wait means a session whose adapter is off takes 10 s
  to refuse, where a missing COM port refuses at once.
