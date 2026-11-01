# ADR-0047: The phone relays a paired Bluetooth adapter to a computer over USB, on its own loopback address, for havne-cli

- **Status:** Accepted. Built; tested on the desktop with a fake adapter,
  not yet run on the phone.
- **Date:** 2026-11-01.
- **Builds on:** [ADR-0028](0028-app-fed-streams.md) (the phone's
  Bluetooth links), [ADR-0029](0029-android-background-sessions.md)
  (the foreground service), [ADR-0026](0026-elm327-transport.md)'s
  appendix "WiFi over TCP" (core's ELM327 over a socket),
  [ADR-0031](0031-command-line-client.md) (havne-cli).

## Context

Long runs from a computer (a DID sweep of every ECU, ADR-0020's
appendix of 2026-11-01) are havne-cli's work, but the adapter that
reaches the test car, the OBDLink MX+, is a Bluetooth one, and from
where the car is parked the desktop's Bluetooth doesn't reach it. The
phone does, while it's on USB to that computer. havne-cli doesn't build
for Android (root `CMakeLists.txt`), and on the phone a Bluetooth link
is the app's own (QtBluetooth through Java, ADR-0028): no program
outside the app can open one.

## Decision

### 1. A byte relay in the app, on the phone's loopback address

`AdapterRelay` (`app/adapterrelay.{h,cpp}`) listens on `127.0.0.1` (port
35000 by default) and passes bytes between one TCP client and the
adapter's link, reading none of them. The computer reaches it through
adb's forward over USB:

```sh
adb forward tcp:35000 tcp:35000
havne-cli stream "elm327://127.0.0.1:35000?profile=uds-generic&did=1000-13FF&scan=first" \
  --until-scanned --seconds 600
```

To core on the computer the phone is a WiFi adapter: the ELM327 setup,
the requests, the response-count digit and the wire log all run there,
so a relayed session behaves as one on a serial port, and nothing in
core or havne-cli changes. The phone's own app runs no session while it
relays.

### 2. One computer, and the adapter held between computers

- The adapter's link opens when the relay starts and stays open while
  computers come and go: a sweep script connects once per block, and an
  RFCOMM connect costs seconds.
- One computer at a time; a second is closed at once.
- A computer's bytes that arrive while the adapter opens are held, up
  to 4 KB (`kHeldMax`, far past a setup's first commands), and sent in
  order once it's open; past that the computer is disconnected.
- The adapter's link dropping ends the computer's, so the computer's
  core sees a failed port and connects again on its backoff; that
  connect opens the adapter again.
- The adapter's bytes with no computer connected are dropped: they
  answer nothing anyone is waiting for.

### 3. Classic Bluetooth only, through the link's own rules

`BtRelayAdapter` (`app/btrelay.{h,cpp}`, Android only) is an RFCOMM
socket to the paired adapter's SPP service, connected by
`btconnect.h`'s rules (one connect at a time, ADR-0029 section 3) and
retired by `btretire.h` (an aborted socket kept until Qt's reader has
stopped), as `btstream.cpp`'s classic link is. It doesn't reuse that
link, which is built around core's stream registry. BLE isn't relayed:
no BLE adapter is on hand to test it.

### 4. Off unless asked, and never on a network

The relay is a developer-mode control in Settings' Adapter section: the
selected `elm327://bt:` row, then **Relay**. It binds the loopback
address alone, so no other machine on the phone's WiFi can reach it;
adb's forward, which needs USB debugging on and the computer
authorized, is the way in. Another app on the phone could connect to
the loopback port while a relay runs, which is why it starts only by
hand, takes one client, and stops with **Stop relaying**. Connect is
disabled while it runs (the adapter takes one link), and a relay can't
start beside a session.

### 5. The foreground service holds it

`AndroidSession` holds ADR-0029's foreground service while a relay runs
as it does for an adapter session, its notification "Relaying
elm327://bt:... to a computer", so the relay survives the screen going
off for the hours a sweep takes. Battery saver can still stop a
foreground service; the run sheet says to turn it off, and the phone
charges over the same USB cable.

## Alternatives considered

- **havne-cli on Android** (an NDK executable over `adb shell`): it
  could reach a WiFi adapter, but not a Bluetooth one, which only an
  app can open.
- **The desktop's own Bluetooth:** out of range of the parked car.
- **A laptop in the car** on a battery tender: no code, and still an
  option; the relay keeps the run on the desktop with the phone near the
  car.
- **The relay on WiFi** (`0.0.0.0`): no cable, but an unauthenticated
  port to a car's bus on the network.
- **A third-party Bluetooth-to-TCP app:** an unknown program between
  the computer and the car, outside the project's tests.

## Consequences

- `app_ui` and `test_bridge` link Qt Network (part of qtbase in every
  kit).
- `BridgeTest.Relay*` (test_bridge) run the relay with a fake adapter
  and real loopback sockets on every desktop platform: bytes both ways
  in order, held bytes and their limit, a second computer refused, the
  adapter's loss ending the computer's link, a computer leaving keeping
  the adapter, stop, and the URIs it refuses. The Bluetooth half is
  compiled by CI's `android-build` and runs only on the phone.
- Left: the phone at the desk with the MX+ (a `havne-cli stream` through
  `adb forward`, the screen off for an hour), then the car.
