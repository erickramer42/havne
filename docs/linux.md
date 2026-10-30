# Havne on Linux

Havne runs on Linux as an AppImage, and its core and command-line client
(`havne-cli`) come as a deb and a tarball for scripts and other programs
([ADR-0035](adr/0035-linux-downloads-sbom-reproducible.md)); releases
carry them from v0.3.4. Both are
built on Ubuntu 24.04 and run on x86_64 distributions whose glibc is
2.39 or later: Ubuntu 24.04 and newer, Debian 13, Fedora 40 and newer,
Arch, Kali and their kin. Building from source is in
[development.md](development.md).

## The app: the AppImage

Download `havne-X.Y.Z-linux-x86_64.AppImage` from the
[latest release](https://github.com/erickramer42/havne/releases/latest),
then:

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
sudo apt install ./havne-core_X.Y.Z_amd64.deb
havne-cli stream mock://demo --values --count 20
```

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
`havne-core-X.Y.Z-linux-x86_64.tar.gz` holds the same tree under one
folder: unpack it anywhere, and point CMake at it with
`-DCMAKE_PREFIX_PATH=<folder>` or pkg-config with
`PKG_CONFIG_PATH=<folder>/lib/pkgconfig`. Before 1.0 a minor version may
change the C ABI, so the CMake package answers a version request only
with the same minor: `find_package(ecuio 0.3)` takes any 0.3.x.

The core packages are reproducible: the same release tag builds the
same bytes, which CI checks on every change.

## Adapters

| Adapter | On Linux |
|---|---|
| ELM327 over USB (an OBDLink, the OBDX Pro in its ELM mode) | `elm327:///dev/ttyUSB0` (or `ttyACM0`). Settings lists `ttyUSB*`, `ttyACM*` and `rfcomm*` |
| ELM327 over WiFi | `elm327://192.168.0.10:35000` once the computer has joined the adapter's network |
| ELM327 over Bluetooth Classic | bind the paired adapter to a serial device, `sudo rfcomm bind 0 <address>`, then `elm327:///dev/rfcomm0` |
| A CAN interface (candleLight, CANable, PEAK, Kvaser) | `socketcan://can0`, listen-only (below) |
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

`socketcan://` reads a CAN interface you have brought up, and sends
nothing; Havne never configures an interface. It refuses a controller
that could transmit, so bring it up in listen-only mode at the bus's
rate:

```sh
sudo ip link set can0 up type can bitrate 500000 listen-only on
havne-cli stream socketcan://can0
```

Settings and `havne-cli devices` list the CAN interfaces. Profiles that
ask the car (`obd2`, `uds-generic`) don't run on SocketCAN yet: a
session there logs the bus (`raw`) or decodes the simulated car
(`mock`).

**Without hardware,** Linux's virtual CAN bus carries the simulated car:

```sh
sudo modprobe vcan
sudo ip link add dev vcan0 type vcan
sudo ip link set vcan0 mtu 72        # CAN FD: the simulated car's frames are 12 bytes
sudo ip link set up vcan0
havne-cli play mock://demo --to vcan0 &
havne-cli stream "socketcan://vcan0?profile=mock" --values
```

or connect the app to `socketcan://vcan0?profile=mock` while `play`
runs. `play` writes only to virtual interfaces, never to a real bus.

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
  Arch and Kali; the app and the core built and their tests run with
  GCC on Ubuntu 26.04; SocketCAN on `vcan0`.
- **Not yet:** the app's window on a Linux desktop, an ELM327 adapter
  on a Linux machine (the serial code is tested against a fake adapter
  on a pseudo-terminal), a real CAN controller, and a car through
  SocketCAN. [testing.md](testing.md) keeps the details.
