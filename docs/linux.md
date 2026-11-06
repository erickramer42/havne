# Havne on Linux

Havne runs on Linux as an AppImage, and its core and command-line client
(`havne-cli`) come as a deb and a tarball for scripts and other programs
([ADR-0035](adr/0035-linux-downloads-sbom-reproducible.md)); releases
carry them from v0.3.4, for x86_64, and from v0.5.0 for 64-bit ARM
(aarch64) as well. Both are built on Ubuntu 24.04 and run on
distributions whose glibc is 2.39 or later: Ubuntu 24.04 and newer,
Debian 13, Fedora 40 and newer, Arch, Kali and their kin, and on a
Raspberry Pi 3 or later, the 64-bit Raspberry Pi OS 13 (trixie) or
Ubuntu 24.04 and newer. Raspberry Pi OS 12 (bookworm) has glibc 2.36,
too old for them: upgrade it to 13, or build from source. 32-bit
Raspberry Pi OS isn't built for. Building from source is in
[development.md](development.md).

## The app: the AppImage

Download `havne-X.Y.Z-linux-x86_64.AppImage` from the
[latest release](https://github.com/erickramer42/havne/releases/latest)
(`havne-X.Y.Z-linux-aarch64.AppImage` on 64-bit ARM; `uname -m` names
the machine's), then:

```sh
chmod +x havne-X.Y.Z-linux-x86_64.AppImage
./havne-X.Y.Z-linux-x86_64.AppImage
```

It carries Qt, its QML modules and the X11 libraries Qt needs. It takes
the rest from the system:

- **OpenGL and fonts:** `libGL`, `libEGL` and `libGLX` belong to the
  machine's graphics driver, and fontconfig and freetype to the
  desktop, so every desktop install has them. A minimal one may not, and
  the app then stops at once with "error while loading shared libraries:
  libGLX.so.0" (or `libfreetype.so.6`, or another). Install Mesa and the
  font libraries: `sudo apt install libgl1 libglx0 libegl1 libopengl0
  libfontconfig1 libfreetype6 libgpg-error0` on Debian, Ubuntu and Kali,
  `sudo pacman -S --needed libglvnd mesa fontconfig freetype2` on Arch.
- **FUSE,** which an AppImage uses to mount itself. Without it, run
  `./havne-….AppImage --appimage-extract-and-run`, which unpacks it to a
  temporary folder each time instead.
- **The display:** the AppImage carries Qt's X11 (xcb) platform plugin,
  so on a Wayland desktop it runs through XWayland, which GNOME and KDE
  provide by default.

To check that the download is the one the release built:

```sh
sha256sum -c --ignore-missing SHA256SUMS.txt
gh attestation verify havne-X.Y.Z-linux-x86_64.AppImage -R erickramer42/havne
```

## Core alone: the deb and the tarball

For `havne-cli` and for programs of your own that link the core
(`#include <ecuio/core.h>`):

```sh
sudo apt install ./havne-core_X.Y.Z_amd64.deb    # havne-core_X.Y.Z_arm64.deb on 64-bit ARM
havne-cli stream mock://demo --values --count 20
```

Signals of your own, a manufacturer's DID or one computed from others, go in a
signal file ([signal-files.md](signal-files.md)), which `havne-cli` and
the app read alike.

The deb installs `/usr/bin/havne-cli`, the static library
`/usr/lib/libecuio-core.a`, the public headers in `/usr/include/ecuio/`,
a CMake package and a pkg-config file:

```cmake
find_package(ecuio REQUIRED)
target_link_libraries(myprogram PRIVATE ecuio::core)
```

```sh
cc -o myprogram myprogram.c $(pkg-config --cflags --libs ecuio)
```

On a distribution without `apt`, the tarball
`havne-core-X.Y.Z-linux-x86_64.tar.gz` (`-linux-aarch64.tar.gz` on
64-bit ARM) holds the same tree under one folder: unpack it anywhere, and point CMake at it with
`-DCMAKE_PREFIX_PATH=<folder>` or pkg-config with
`PKG_CONFIG_PATH=<folder>/lib/pkgconfig`. Before 1.0 a minor version may
change the C ABI, so the CMake package answers a version request only
with the same minor: `find_package(ecuio 0.3)` takes any 0.3.x.

The core packages are reproducible: the same release tag builds the
same bytes, which CI checks on every change. The AppImage is too, as
long as Ubuntu's packages it bundles haven't changed since the release
was built.

## Adapters

| Adapter | On Linux |
|---|---|
| ELM327 over USB (an OBDLink, the OBDX Pro in its ELM mode) | `elm327:///dev/ttyUSB0` (or `ttyACM0`). Settings lists `ttyUSB*`, `ttyACM*` and `rfcomm*` |
| ELM327 over WiFi | `elm327://192.168.0.10:35000` once the computer has joined the adapter's network |
| ELM327 over Bluetooth Classic | bind the paired adapter to a serial device, `sudo rfcomm bind 0 <address>`, then `elm327:///dev/rfcomm0` |
| A CAN interface (candleLight, CANable, PEAK, Kvaser) | `socketcan://can0`, listen-only, or `socketcan://can0?profile=obd2&baud=500000` to ask the car (below) |
| J2534 | Windows only: J2534 drivers are Windows DLLs |

**Serial ports need a group.** A serial device belongs to the `dialout`
group on Debian, Ubuntu and Fedora, and `uucp` on Arch. Without it a
connect fails with "permission denied: the user needs access to the
port". Add yourself once, then log out and back in:

```sh
sudo usermod -aG dialout "$USER"     # Arch: uucp
```

The Bluetooth adapter picker and BLE adapters are Android's; on the
Linux desktop a Classic adapter goes through `rfcomm` as above.

### SocketCAN

`socketcan://` uses a CAN interface you have brought up; Havne never
configures one. Settings and `havne-cli devices` list them, each with
its bit rate, the controller's state, the modes it's in and, from Linux
6.0, the modes its driver offers, so a connect's refusal is explained
before you try it.

**Listening** (`raw`, the default, or `mock`) sends nothing and needs a
controller that can't transmit, so bring it up in listen-only mode at
the bus's rate:

```sh
sudo ip link set can0 up type can bitrate 500000 listen-only on
havne-cli stream socketcan://can0
```

**Asking the car** (`obd2`, `uds-generic`) sends requests through ISO-TP
in core ([ADR-0040](adr/0040-socketcan-requests-isotp.md)), so the
controller transmits. A controller at the wrong rate disturbs the car's
bus, so Havne sends only through one brought up to stop at once: in
normal mode at the rate `?baud=` states, never restarting itself after
a bus-off, in one-shot mode (a failed frame goes out once) and with no
errors counted. Until its first request comes back from the bus, a CAN
error ends the session's sending. Check the rate with a listening
session first, while the car's bus has traffic, then:

```sh
sudo ip link set can0 down
sudo ip link set can0 up type can bitrate 500000 restart-ms 0 one-shot on
havne-cli stream "socketcan://can0?profile=obd2&baud=500000" --values
```

Not every adapter's driver offers one-shot mode (`havne-cli devices`
and `ip -details link show can0` list what it supports); one without
it can listen but not ask, and Havne refuses it for asking: without
it, a frame nobody acknowledges is sent again until the interface goes
down
([2026-11-02-socketcan-desk](bench-sessions/2026-11-02-socketcan-desk.md)).
When an interface goes from listening to asking, say `listen-only off`:
`ip link set` keeps the mode it last had. No real controller has sent
through Havne yet.

**Without hardware,** Linux's virtual CAN bus carries the simulated car:

```sh
sudo modprobe vcan
sudo ip link add dev vcan0 type vcan
sudo ip link set vcan0 mtu 72        # CAN FD: the simulated car's frames are 16 bytes
sudo ip link set up vcan0
havne-cli play mock://demo --to vcan0 &
havne-cli stream "socketcan://vcan0?profile=mock" --values
```

or connect the app to `socketcan://vcan0?profile=mock` while `play`
runs. `play` writes only to virtual interfaces, never to a real bus.

**A simulated ECU to ask** ([ADR-0046](adr/0046-simulated-ecu.md)):
`havne-cli sim` answers OBD-II requests on the virtual bus as the
simulated car's engine ECU, so the requesting profiles run end to end:

```sh
havne-cli sim mock://sim --on vcan0 &
havne-cli stream "socketcan://vcan0?profile=obd2" --values
```

The app connects to `socketcan://vcan0?profile=obd2` the same way.
`mock://sim` answers from the simulated car's drive cycle (the engine
starts after 15 s with the key on); `?values=canned` answers every
linear J1979 PID with one reading instead, `?vin=` sets the VIN it
gives, and `?faults=silent:5,late:7,pending:3,bad_sequence:5,stalled:7`
breaks every Nth reply (`pending` answers `7F <service> 78` first and
the reply 300 ms later). `?modules=2` adds a second module on
0x7E1/0x7E9 with three made-up Service 22 DIDs, D100, D101 and D102,
refusing everything else, so one session can ask two ECUs:

```sh
havne-cli sim "mock://sim?modules=2" --on vcan0 &
havne-cli stream "socketcan://vcan0?profile=uds-generic&did=7E9:D100:u8,7E9:D102:text" --values
```

`replay:///path/to/recording` answers as the car in
a recording did instead: each request gets the reply the recording
holds for it at the sim's time since it started (the car's own
bitmaps, values, identification and codes; `?loop=1` goes round the
recording, `?from=` and `?to=` answer from a stretch of it alone, in
the seconds `havne-cli view` shows, `?vin=` replaces its VIN; a clear
is refused). It answers
only on a `vcan` interface, never on a car's bus, unless
`--desk-bus` says the bus is a desk's with no car on it: then a
real controller answers too, if it is up, not listen-only and
error-active, one-shot mode or not (each refusal gives the `ip link`
commands). On any bus it goes silent for good, and `havne-cli sim`
ends with `yielded`, once another node sends on 0x7E8 (or 0x7E9 with
`?modules=2`): an ECU answers there already.

## Where Havne keeps things

| What | Where |
|---|---|
| Preferences | `~/.config/Havne/Havne.conf` (`HAVNE_SETTINGS_DIR=<dir>` keeps one run's elsewhere) |
| Recordings | `~/Documents/Havne/recordings/` (the desktop's documents folder) |
| ELM327 wire logs | `$TMPDIR`, or `/tmp`: `ecuio-elm327.<date-time>.log`, the newest 50 kept |

## Debugging and tracing

The release's `havne-core-dbgsym_X.Y.Z_amd64.deb` (or the
`-debug.tar.gz`, unpacked under `/`) holds `havne-cli`'s debug info,
found by its build ID, so `gdb` and `perf report` read a crash or a
profile of the installed `havne-cli` against the source.

Core marks a session's start and close and every frame with USDT probes
(provider `ecuio`), which `bpftrace` and `perf` attach to without a
debug build ([ADR-0034](adr/0034-debug-symbols-and-probes.md)):

```sh
sudo bpftrace -e 'usdt:/usr/bin/havne-cli:ecuio:frame { @[arg1] = count(); }' \
    -c '/usr/bin/havne-cli stream mock://demo --count 200'
```

counts the frames per CAN ID.

## What has run where

- **CI,** on every change: the app built and its tests run on Ubuntu
  (GCC); the AppImage built on Ubuntu 24.04 and started offscreen; the
  core packages built twice and compared byte for byte, installed and
  used; SocketCAN against `vcan0`; the USDT probes traced with
  `bpftrace`.
- **WSL** on Windows: the AppImage started offscreen on Ubuntu 26.04,
  Arch and Kali, and with a window under WSLg (through XWayland) on all
  three: the simulated car connected with recording on, the pages drew
  and took input, and closing the window ended the app; the app and the
  core built and their tests run with GCC on Ubuntu 26.04; SocketCAN on
  `vcan0`, and on a real controller (an Innomaker USB2CAN through
  `usbipd-win`) on a desk bus beside the OBDX Pro VX: listening, with
  sending tried through `can-utils`
  ([2026-11-02-socketcan-desk](bench-sessions/2026-11-02-socketcan-desk.md)),
  and listening on the test car beside the VX
  ([2026-11-03-socketcan-car](bench-sessions/2026-11-03-socketcan-car.md)).
- **A Linux desktop:** v0.3.4's AppImage on Xubuntu 26.10 (Xfce, an X11
  session) on a 2011 desktop with a second-generation Core i7 and 8 GB
  (2026-10-30). Made executable and started as it came, with no FUSE
  workaround; `mock://demo` ran smoothly, and its recording landed in
  `~/Documents/Havne/recordings/`. With Live showing, the app took
  about 12 % of the CPU, and 9-10 % once the OpenGL and font packages
  above were installed. On the same desktop, the tree builds with GCC
  15.3 and Qt 6.12.0 (from `aqtinstall`, in the home folder) with no
  warnings, and its suites pass: the core alone, 728 cases, and the
  whole app, 877, but for the locale case and the real-car recordings'
  diff. With `vcan0` up and the kernel's ISO-TP loaded, the simulated
  bus's 63 cases pass too, but for the one that takes the interface
  down, which needs root; and `havne-cli`, end to end: a replay of the
  committed fixture (7,074 frames, none dropped), the mock car played
  onto `vcan0` and heard by a listening session, and the simulated ECU
  answering an `obd2` session through ISO-TP while it recorded (4,259
  replies, no timeout), the recording then passing `info` and `view`
  (2026-11-05, `dev` at 8d996cc).
- **A Raspberry Pi 3** (aarch64, Kali Linux, GCC 15.2, 906 MB of memory
  and a 2 GB swap file): the core and `havne-cli` build (about 27
  minutes at `-j2`, into the swap but never out of memory) with no
  warnings, and the core's suite passes, 728 cases; with `vcan0` and
  ISO-TP, the simulated bus's 63 and the same `havne-cli` run as the
  desktop's, with the same counts (2026-11-05, `dev` at 8d996cc). It is
  the only ARM Linux that has run Havne's tests: CI runs them on
  x86_64, and on GitHub's ARM runner builds and checks the aarch64
  AppImage and core packages only. The release's own arm64 files run
  there too: the deb's `havne-cli` streams the mock at the release's
  version, and the AppImage stays up 10 s offscreen (v0.5.0,
  2026-11-06).
- **Not yet:** an ELM327 adapter
  on a Linux machine (the serial code is tested against a fake adapter
  on a pseudo-terminal), and a real CAN controller sending through
  Havne. [testing.md](testing.md) keeps the details.
