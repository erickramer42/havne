# ADR-0030: Signed Android releases, one release key, offline and in a protected environment, its certificate pinned in the repository

- **Status:** Accepted.
- **Date:** 2026-10-29.
- **Builds on:** [ADR-0028](0028-app-fed-streams.md) §3 (the debug APK
  from CI, which this record leaves as it is for development);
  [ADR-0029](0029-android-background-sessions.md) (the app the APK
  carries).

## Context

The Android app has run on a phone and a car (ADR-0028's and
ADR-0029's session sheets), but the only way onto a phone has been CI's
debug APK or a local build. Neither suits a user:

- **A debug APK changes key every CI run,** so each one is a new app to
  Android: the next install needs an uninstall, which deletes the app's
  settings, vehicles and recordings.
- **A debug build is debuggable.** `adb shell run-as` reads its private
  files, the settings and the wire logs, which can hold a VIN and
  calibration IDs, from any computer the phone is plugged into with USB
  debugging on.
- **A release from GitHub has nothing to check it against.** A user
  who downloads an APK can't tell this repository's build from someone
  else's, and Android itself only checks that an update carries the
  same key as the app it replaces.

Android binds an app to its signing key for good: an update must carry
the same certificate, or the phone refuses it. So the key is the
app's identity, and the decision is mostly about keeping it.

## Decision

### 1. One release key, made once, kept offline

- **The keystore** is PKCS12, holding one RSA 4096 key with the alias
  `havne`, one password for both store and key, and a validity of
  10,000 days. Its certificate names only `CN=Havne`: everyone who has
  the APK can read the certificate, so it carries no name, address or
  e-mail.
- **It lives off the development machine,** in two places: the
  password in a password manager, and the keystore with it and on an
  offline copy. Losing either means the app can never be updated in
  place again: every phone would have to uninstall it, losing its data,
  and install a new app.
- **The setup is printed, not run,** by the release skill's
  `android-key` (local tooling); `docs/development.md`, "The Android
  release key", has the same steps. The password is typed into the
  owner's shell and reaches keytool and the secret through environment
  variables and stdin, never a command line.

### 2. CI signs, in a protected environment

- **The keystore and its password are secrets of a GitHub environment,
  `release`** (`ANDROID_KEYSTORE_B64`, `ANDROID_KEYSTORE_PASS`), not of
  the repository. Only a job that names the environment can read them,
  and the environment admits only `v*` tags (a release) and `main` (a
  manual rebuild of a tag). No pull request, fork or `ci.yml` job can
  read them.
- **`release.yml`'s `android` job** is the only one that names the
  environment. It builds the Release APK (optimized, not debuggable,
  unsigned), decodes the keystore into the runner's temporary directory
  for the signing step only, and runs `packaging/android/sign-apk.sh`.
  The other jobs never see the key: `windows` builds the zip, and
  `publish` writes the checksums, attests both files' provenance and
  creates the release.
- **`sign-apk.sh`** checks the unsigned APK (package, version name and
  code, not debuggable, arm64-v8a only), aligns it on 16 KB pages,
  signs it with APK Signature Scheme v3 (`apksigner` adds no v1 or v2
  for a minimum SDK of 31, which v3 covers; no v4, which would need a
  sidecar file), and fails unless the signer's certificate SHA-256 is
  the pinned one. The same script signs a local Release build, with the
  release key or a throwaway one, so the release's checks run before a
  tag.

### 3. The certificate is pinned and published

`packaging/android/release-cert.sha256` holds the certificate's SHA-256
(64 hex digits). The signing step fails on any other certificate, so a
wrong or replaced keystore fails in CI, not on a phone that refuses the
update. Each release's notes give the same fingerprint, and a user can
check a download with `apksigner verify --print-certs`. The APK also
gets its own build provenance, as the zip does, so
`gh attestation verify` ties it to this repository's workflow.

### 4. What stays as it was

CI's `android-build` still uploads a debug APK for development, and a
local build still signs with the machine's debug key (ADR-0028 §3).
Phone runs that need the wire logs use those builds: a release build
isn't debuggable, so `run-as` can't read its private files. Each key is
a different app to Android, so moving a phone between a release and a
development build means an uninstall, and the app's data goes with it.

## Alternatives considered

- **Google Play, with Play App Signing.** Google would hold the signing
  key, and a lost upload key could be reset. But it needs a developer
  account, a store listing and Play's review for each release; a
  sideloaded APK from the releases page needs none of that while the
  app is pre-1.0.
  The key decided here can still become Play's upload key, or be
  handed to Play App Signing, later.
- **Signing through Qt's `QT_ANDROID_SIGN_APK`,** where androiddeployqt
  calls `apksigner` itself. It reads the same four values from the
  environment, but checks nothing about the result. A script of our
  own runs the same commands in CI and locally, and checks the
  certificate against the pin.
- **Repository secrets, no environment.** Simpler, but every workflow
  on every branch pushed to the repository could read them, `ci.yml`
  included. The environment costs one setup step.
- **A new key each release.** Every update would be a new app; it
  defeats the point.

## Consequences

- A user installs from the releases page, and each later release
  installs over the one before, keeping the app's data; Android's own
  check (same certificate, higher version code) is what allows it.
- The release workflow has four jobs; the Android build runs side by
  side with the Windows one.
- **The key is a liability that can't be rotated cheaply.** APK
  Signature Scheme v3 can rotate to a new key with a lineage signed by
  the old one, so a compromised key can be replaced while the old one
  still exists; a lost key can't be. Two offline copies are the
  mitigation.
- The phone runs that collect evidence keep using debuggable builds,
  so a session sheet's build isn't the release's build; the release
  notes say which build a phone has run.
