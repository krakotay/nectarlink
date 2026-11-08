# NectarLink for Android

Kotlin + Jetpack Compose (Material 3) on top of the shared Rust core,
called through UniFFI (`core/nectarlink-ffi`).

## Build

Needs the Android SDK (platform 37, NDK 30.0.16248370), Rust with the
Android targets, and `cargo-ndk`:

```
rustup target add aarch64-linux-android x86_64-linux-android
cargo install cargo-ndk
```

Then, from `android/`:

```
./gradlew assembleDebug                          # phones + x86_64 emulator
./gradlew assembleDebug -Pnectarlink.abis=x86_64 # emulator only (faster)
./gradlew testDebugUnitTest
```

Gradle builds the core with cargo-ndk (cargo profile `android`: release
optimizations, symbols kept for UniFFI and stripped when packaging) and
generates the Kotlin bindings from the built library. Both are generated
sources; nothing generated is checked in except the theme tokens
(`ui/theme/Tokens.kt`, from `cargo xtask tokens`).

### Experimental H.264 All-Intra build

```sh
./gradlew assembleRelease -Pnectarlink.abis=arm64-v8a -Pnectarlink.h264AllIntra=true
```

This opt-in build forces H.264 for phone screen mirroring (MediaProjection and
Elevated) and app windows, requests every frame as a keyframe with
`KEY_I_FRAME_INTERVAL = 0`, and keeps B frames disabled. The installed version
name ends in `-all-intra`. Resolution, frame rate and bitrate still follow the
PC's mirror quality setting. At the same bitrate, image quality may decrease;
this does not accelerate the PC's color conversion.

The encoder logs the requested mode and warns once per encoder instance if it
emits a non-keyframe. Packet flags still come from the encoder; the experiment
does not relabel dependent frames as independent ones. Hardware behavior needs
device validation. Normal builds omit the property and keep the usual codec
selection and keyframe interval.

Android's [MediaFormat documentation](https://developer.android.com/reference/android/media/MediaFormat#KEY_I_FRAME_INTERVAL)
defines a zero interval as a request for all keyframes.

## Layout

- `core/`: the bridge to the Rust core. `Core` owns the node and folds its
  events into `CoreState` (a pure reducer, unit-tested); `KeystoreKeyProtector`
  encrypts the identity key with a non-exportable Android Keystore key;
  `Ringer` rings on the alarm stream; battery and network monitors feed the
  core (Android doesn't let native code watch the network).
- `notifications/`: the notification listener (with the user's notification
  access) that hands notifications to the core for mirroring, and reads them
  (messaging-style content, actions, app icons); replies and actions from a
  PC run through `core/PhonePlatform`.
- `clipboard/`: sending text to the PC (share sheet, Quick Settings tile,
  the connection notification's button) through `SendActivity`, which reads
  the clipboard once it has focus, as Android requires; and writing text a
  PC sent. `SendActivity` also takes shared files.
- `files/`: opening files to send, publishing received files to
  Download/Nectarlink (MediaStore on Android 10+), and the transfer
  notifications with progress and Cancel.
- `media/`: this phone's media sessions for PCs (`PhoneMedia`, with
  notification access), and a PC's player in Android's media controls
  (`PcMedia`, a media session and its notification).
- `service/ConnectionService`: a "connected device" foreground service that
  keeps the phone reachable and holds the Wi-Fi multicast lock local
  discovery needs.
- `ui/`: pairing (QR scanner with CameraX + ZXing, no Google Play services;
  nearby pairing with a 6-digit code), home and settings. Bloom uses
  Material You colors from the wallpaper on Android 12+.

The identity lives in `noBackupFilesDir`, so it's never restored onto
another phone. `nectarlink://pair?...` links (a PC's QR code scanned with any
camera app) open the app and pair.

## Release & Signing

Release builds enable R8 code and resource shrinking (`isMinifyEnabled = true`,
`isShrinkResources = true` with `proguard-rules.pro` keeping JNA, UniFFI
bindings, Conscrypt, and the Elevated shell helper entrypoint).

No signing keys or passwords are stored in the repository:

- **Android:** set `NECTARLINK_KEYSTORE` (path to the `.jks` keystore, key alias
  `nectarlink`) and `NECTARLINK_KEYSTORE_PASSWORD` in the environment before
  running `./gradlew assembleRelease`. In CI (`.github/workflows/release.yml`),
  these come from the `ANDROID_KEYSTORE` (base64) and
  `ANDROID_KEYSTORE_PASSWORD` repository secrets, and the temporary `.jks` file
  is deleted immediately after `apksigner verify`. When `NECTARLINK_KEYSTORE` is
  unset locally, `assembleRelease` signs `app-release.apk` with the debug key,
  for testing only; never publish that build.
- **Windows:** `./scripts/package.ps1` builds `Nectarlink-<version>-x64-setup.exe`,
  and `.github/workflows/release.yml` publishes `SHA256SUMS.txt` alongside the
  release artifacts so both the Windows and Android in-app updaters verify the
  SHA-256 digest before installing.

