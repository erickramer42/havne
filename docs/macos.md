# Havne on macOS

Havne runs on Macs with Apple silicon (M1 and later), as a zip of the
app, `havne-X.Y.Z-macos-arm64.zip`, on each release. Intel Macs are not
supported: the app and the Qt libraries inside it are arm64 only.

The app is **unsigned by Apple**: it has no Developer ID and isn't
notarized, because no Apple Developer account stands behind it. macOS
therefore blocks the first open, and [the steps below](#installing-it)
get past that once. The bundle carries an ad hoc signature, which holds
no identity: Apple silicon refuses to run a binary with no signature at
all, and an ad hoc one satisfies that and nothing more.

The macOS app is unvalidated
([Known Issue #50](../STATUS.md#known-issues-and-technical-debt)): CI
launches it on a runner and checks it stays up, and no person has run it
on a Mac yet. An install is a test of its own, and what it shows is
worth reporting, whatever it is.

## What you need

- A Mac with **Apple silicon** (M1 or later).
- **macOS 14.4 or later** (Qt 6.12's minimum). The app's `Info.plist`
  names the exact number.
- The file: `havne-X.Y.Z-macos-arm64.zip` from the
  [latest release](https://github.com/erickramer42/havne/releases/latest).
  A CI run's `havne-macos-arm64` artifact (Actions, a CI run,
  Artifacts; a GitHub login is needed, and its zip holds this file) is
  the same app for a commit that isn't released yet.

## Installing it

1. Open the zip (Safari unzips downloads by itself; otherwise
   double-click it in Finder). It holds `Havne.app`.
2. Move `Havne.app` to **Applications**.
3. Open it. macOS says it can't verify the app: press **Done**, not
   **Move to Trash**.
4. Open System Settings > **Privacy & Security**, scroll to Security,
   and press **Open Anyway** beside the message about Havne. Enter your
   password, then press **Open Anyway** once more. macOS 15 removed the
   older route of right-clicking the app and choosing Open, so this is
   the way. It is asked once; later opens need nothing.

Or, in Terminal, remove the quarantine mark the download put on the app
and open it as any other:

```
xattr -dr com.apple.quarantine /Applications/Havne.app
```

An update is the same: replace `Havne.app` with the new one. Havne's
settings and recordings are outside the app and stay.

To check the download first, the release's `SHA256SUMS.txt` has its
SHA-256 (`shasum -a 256 <file>`), and `gh attestation verify <file>
-R erickramer42/havne` checks its build provenance.

## What to try

| Connection | On macOS |
|---|---|
| `mock://demo`, a simulated car | Works with no hardware. Home's list ends with its Demo data row |
| An ELM327 adapter on a USB serial port | Plug it in, then pick its port in Havne's Settings. The adapter's driver must give the Mac a serial port (`/dev/cu.*`) |
| A WiFi ELM327 adapter | Join the adapter's WiFi, then pick its preset in Havne's Settings: "WiFi: OBDX (192.168.4.1:23)" or "WiFi: most others (192.168.0.10:35000)". macOS 15 asks once whether Havne may find devices on the local network: allow it |
| A Bluetooth adapter (the OBDLink MX+ and others) | Not on macOS: Havne's Bluetooth is Android's alone |
| J2534 and SocketCAN | Windows and Linux only |

Worth reporting: whether it opens after the steps above and draws, the
Dashboard and Live on `mock://demo`, resizing the window, and with an
adapter, whether it connects and the values match the car's.
Screenshots help (Shift-Command-4, then Space, then click the window).

## Reporting a problem

File an issue at
[github.com/erickramer42/havne/issues](https://github.com/erickramer42/havne/issues)
with your Mac's chip and macOS version (Apple menu > About This Mac),
the release, and what you did and saw. A screenshot of an error from
macOS itself helps most. An app that quits at once leaves a report in
Console > Crash Reports.

## Sending a session back

Turn on **Record sessions** in Havne's Settings before connecting. Each
session's files (the frames, the values and `events.log`, the activity
log) land in `~/Documents/Havne/recordings/`, a folder per session.

A recording of a real car holds its VIN and its ECUs' identification.
Send it only to the developer.
