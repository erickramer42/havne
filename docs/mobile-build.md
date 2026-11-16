# Mobile builds

The app builds for Android and iOS from the desktop's sources. The desktop's
builds and tests are in [development.md](development.md).

Both mobile builds run in CI; Android runs on a phone, iOS only on
the Simulator. The QML, bridge and core are shared, and only the
kit differs. Use `qt-cmake` from
the mobile kit, which applies Qt's toolchain file and finds the host
desktop Qt. Mobile builds set `BUILD_TESTING=OFF`:
`gtest_discover_tests` runs each test binary at build time, and the
build host can't execute an Android or iOS binary. On a phone the
transports are `mock://`, `replay://` and `elm327://`, to a WiFi
adapter's address or, on Android, a Bluetooth adapter through the
phone ([ADR-0028](adr/0028-app-fed-streams.md); Known Issue #2 says
what has run where). Qt Creator with an Android or iOS kit is the
interactive alternative.

*Android* (this mirrors the `android-build` CI job). On Windows, add
`-G Ninja`, because the Visual Studio generator can't target Android:

```sh
<qt-install>/android_arm64_v8a/bin/qt-cmake -B build-android \
    -DBUILD_TESTING=OFF \
    -DANDROID_SDK_ROOT=<sdk> -DANDROID_NDK_ROOT=<sdk>/ndk/<27.x>
cmake --build build-android
cmake --build build-android --target apk
```

Leave `CMAKE_BUILD_TYPE` unset, as CI does: the build is then Debug,
and Gradle signs the APK with the machine's debug key. A Release build
makes an unsigned APK that a phone won't install until
`packaging/android/sign-apk.sh` signs it, as the release job does
([Releases](packaging.md#releases)); it isn't debuggable, so `run-as` can't read
its wire logs, and phone runs use a Debug build. For an emulator on
an x86-64 PC, use the `android_x86_64` kit. Since
Qt 6.7, Qt publishes Android kits under the `all_os` host (the name
aqtinstall and install-qt-action use). They need the build host's
desktop Qt, and `--autodesktop` installs it; when the kit names a host
Qt that isn't installed, pass `-DQT_HOST_PATH=<qt-install>/<desktop kit>`.

`app/android/` is the package source (`QT_ANDROID_PACKAGE_SOURCE_DIR`):
the icons in `res/`, the Java in `src/`, and the app's own
`AndroidManifest.xml`. That manifest is Qt 6.12's template with its
`%%INSERT_…%%` placeholders kept, plus what
[ADR-0029](adr/0029-android-background-sessions.md) adds: the
`android.app.background_running` meta-data and the session's
foreground service. On a Qt upgrade, diff it against the new kit's
`src/android/templates/AndroidManifest.xml`. The permissions come from
`qt_add_android_permission` in `app/CMakeLists.txt`, not the manifest,
rather than from the ones Qt adds by default: `INTERNET` for core's
TCP socket, then the Bluetooth and the foreground service ones.
`android-build` fails if the generated manifest lacks any of them.

To install on a phone without an Android kit, take the
`havne-android-arm64-v8a` artifact from a CI run (kept 14 days): the
debug-signed APK. Each run signs with a new debug key, so uninstall
the previous build before installing the next (Android refuses an
update signed by another key). Local builds share one key, so each
updates the last in place and the app keeps its recordings. A
release's APK carries a third key, the release key, so moving a phone
between it and a development build is an uninstall too.

*iOS, device* (macOS host; this mirrors `ios-build`). `qt-cmake` also
applies Qt's minimum deployment target, iOS 18 for Qt 6.12:

```sh
<qt-install>/ios/bin/qt-cmake -B build-ios -G Xcode \
    -DBUILD_TESTING=OFF -DCMAKE_OSX_ARCHITECTURES=arm64
cmake --build build-ios --config Release -- CODE_SIGNING_ALLOWED=NO
```

That build is unsigned and can't be installed. Running it on a phone
needs a development team for signing, and nobody has done that here
yet.

*iOS, Simulator* (this mirrors `ios-simulator-smoke`). Qt's iOS
simulator libraries are x86_64 only. On Apple silicon the simulator
therefore runs under Rosetta. With Xcode 26 and later, first delete
the default iOS platform component in Xcode's settings, then install
the universal one
(`xcodebuild -downloadPlatform iOS -architectureVariant universal`).
Boot a simulator before installing:

```sh
<qt-install>/ios/bin/qt-cmake -B build-iossim -G Xcode \
    -DBUILD_TESTING=OFF \
    -DCMAKE_OSX_SYSROOT=iphonesimulator -DCMAKE_OSX_ARCHITECTURES=x86_64
cmake --build build-iossim --config Release -- CODE_SIGNING_ALLOWED=NO
xcrun simctl install booted build-iossim/app/Release-iphonesimulator/Havne.app
```

Then launch it from the simulator's home screen. The CI job's launch
step shows how to read the bundle ID from `Info.plist`, launch the
app, and send its Qt log output to a file.

## The app icon

`packaging/icons/make_icons.py` draws it from one
geometry (the Home page's tachometer on the accent color's tile) and
writes every copy: the SVG masters in `packaging/icons/`, Android's
adaptive launcher icon as vector drawables in `app/android/res/`
(named in the manifest through `QT_ANDROID_APP_ICON`, which
`android-build` checks), iOS's asset catalog in
`app/ios/Assets.xcassets/`, and Windows' `app/windows/app.ico`, which
`app_add_version_resource` puts in `havne.exe`'s resource (Qt's
Windows plugin takes it for the window too). The iOS 1024 px PNG and
the `.ico`'s images (16 to 64 px as 32-bit bitmaps, 256 px as PNG,
the corners transparent) are rendered by a desktop Qt kit's `qml`
runtime, offscreen:

```sh
python packaging/icons/make_icons.py --qt C:/Qt/6.12.0/msvc2022_64
```

Change the script, not its output, and commit both.

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| A mobile build fails during test discovery, or tries to run a test binary | `BUILD_TESTING` is on. Cross builds must pass `-DBUILD_TESTING=OFF` (see above). |
| Re-running a skipped `ios-simulator-smoke` skips it again | A re-run replays the original event and commit, so the job's condition comes out the same. Start a manual run instead that asks for it (Actions > CI > Run workflow and tick `ios_smoke`, or `gh workflow run ci.yml --ref <branch> -f ios_smoke=true`); `smoke_only` instead skips the other platform jobs. A commit whose message contains `[ios-smoke]` also works. |
| `ios-simulator-smoke` passes, but the app screenshots are blank | Seen once, on the first run (2026-10-20). That run launched the app straight after a cold boot, and the frame was blank at 15 s. With the boot overlapping the build, the next run drew by 15 s. If it recurs: a blank `home.png` means the runner's simulator display is at fault. No `QSG_INFO` lines in stderr means the window was never exposed. `QSG_INFO` present with blank shots means frames aren't reaching the screen, so try another `QSG_RHI_BACKEND`. A blank 15 s shot with a drawn 45 s shot means a slow first frame. |
| `ios-simulator-smoke` fails with "Havne exited within 45 s", and the printed stderr says `Failed to initialize graphics backend` | Qt Quick couldn't get a Metal device from the runner VM. Check the boot step's `system_profiler` output. Before suspecting the app, try `SIMCTL_CHILD_QSG_RHI_BACKEND=opengl` on the launch line. |
| `ios-simulator-smoke` fails with "No available iPhone simulator on iOS 18 or later" | The runner image dropped its iOS 18+ runtimes, or the job moved to a runner without them. The step prints the available devices. |
