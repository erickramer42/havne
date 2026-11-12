# Havne on iOS

Havne builds for iPhone (iOS 18 or later), but there's no App Store or
TestFlight build. CI makes an **unsigned** `.ipa` on every run, and a
tester installs it with a sideloading tool that signs it with their own
Apple ID. The file holds no signature and no device's ID, so anyone can
use the same one.

The iOS app hasn't been checked on an iPhone yet
([Known Issue #2](../STATUS.md#known-issues-and-technical-debt)): the
Simulator is all it has run on. A first install is a test of its own,
and what it shows is worth reporting, whatever it is.

## What you need

- An iPhone on **iOS 18 or later**.
- An **Apple ID**. A free one is enough; a spare one is a good idea,
  since the tool signs in to Apple with it. One in the paid Apple
  Developer Program works the same way and lasts longer
  ([below](#with-a-paid-developer-account)).
- A **Windows or Mac computer** and a USB cable for the first install.
- The file, `havne-ios-unsigned.ipa`: from a CI run's
  `havne-ios-arm64-unsigned` artifact (Actions, a CI run, Artifacts; a
  GitHub login is needed, and the zip holds the `.ipa`), or from the
  developer.

## Installing it with Sideloadly

[AltStore](https://altstore.io) works too; these steps use
[Sideloadly](https://sideloadly.io), on Windows or macOS.

1. **On Windows only:** install iTunes and iCloud from
   [apple.com](https://www.apple.com/itunes/), **not the Microsoft
   Store** versions. Sideloadly needs the drivers the web installers
   bring.
2. Install Sideloadly.
3. Connect the iPhone, unlock it and tap **Trust This Computer**.
4. Open Sideloadly, drag `havne-ios-unsigned.ipa` into it, pick the
   iPhone, enter the Apple ID and press **Start**. Enter the two-factor
   code if Apple asks for one. Sideloadly signs the app and installs it.
5. **The first time:** on the iPhone, open Settings > General > VPN &
   Device Management, tap the Apple ID and **Trust** it.
6. **The first time:** turn on Settings > Privacy & Security >
   **Developer Mode** (the entry appears after the first sideloaded
   install). The iPhone restarts; confirm **Turn On** after it does.
7. Open Havne.

### Every seven days

An app signed with a free Apple ID stops opening after seven days.
Nothing on the phone is lost: install the same `.ipa` again with
Sideloadly to sign it for another seven. Sideloadly can also refresh it
over WiFi while it runs on the computer, and AltStore does the same.

A free Apple ID holds three sideloaded apps at a time.

### With a paid developer account

An Apple ID in the paid Apple Developer Program signs with the same
steps, and neither limit above applies: the app keeps opening for up to
a year, until the membership's own end date, and the three-app limit is
a free account's. Install the next `.ipa` the same way to update.

A developer can instead sign `Payload/Havne.app` (inside the `.ipa`)
with their own development certificate and a provisioning profile that
lists the iPhone, then install it from Xcode's Devices and Simulators
window. That signature holds the profile and so the iPhone's ID: keep
that signed copy to yourself.

## What to try

| Connection | On iOS |
|---|---|
| `mock://demo`, a simulated car | Works with no hardware. Home's list ends with its Demo data row |
| A WiFi ELM327 adapter | Join the adapter's WiFi, then pick its preset in Havne's Settings: "WiFi: OBDX (192.168.4.1:23)" or "WiFi: most others (192.168.0.10:35000)". iOS asks once whether Havne may find devices on the local network: allow it |
| A Bluetooth adapter (the OBDLink MX+ and others) | Not on iOS: Havne's Bluetooth is Android's alone, and iOS opens classic Bluetooth serial links only to Apple-certified (MFi) accessories |
| J2534 and SocketCAN | Windows and Linux only |

Worth reporting: whether it opens and draws, the Dashboard and Live on
`mock://demo`, rotating the phone, and with an adapter, whether it
connects and the values match the car's. Screenshots help (the side
button with volume up, or with Home on an iPhone that has one).

## Sending a session back

Turn on **Record sessions** in Havne's Settings before connecting. Each
session's files (the frames, the values and `events.log`, the activity
log) land in the Files app under On My iPhone > Havne > Havne >
recordings, a folder per session; share a folder from there by AirDrop
or mail.

A recording of a real car holds its VIN and its ECUs' identification.
Send it only to the developer. The Documents folder is also in the
iPhone's own backups, as any app's is.
