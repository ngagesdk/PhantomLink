# PhantomLink Bridge for Android

An Android app that does the job of the ESP32 bridge and the PC bridge
(`gcbridge`): the N-Gage connects to the phone over Bluetooth, and the
phone forwards the game to a PhantomLink server over Wi-Fi or mobile data.

- The forwarding is done by the same C engine as the ESP32 and PC bridges
  (`firmware/src/bridge_core.c` and `protocol/`), compiled with the NDK and
  called through JNI. Resume after connection drops, local answers while the
  server is unreachable, access key and device address handling are
  therefore identical.
- `BridgeEngine.java` is the Android counterpart of `pcbridge/src/pcb_main.c`:
  one engine per N-Gage (up to 7), one TCP connection per N-Gage, LAN
  discovery (`GCDISCOVER1`), removal of N-Gages that do not come back. It is
  plain Java, so the host tests run it unchanged.
- Bluetooth: publishes the Serial Port service (UUID 0x1101) and accepts
  RFCOMM connections; pairing PIN requests are answered automatically
  (default 1234), like the PC bridge's BlueZ agent.
- Runs as a foreground service with a wake lock and a low-latency Wi-Fi
  lock, so it keeps working with the screen off.
- UI: Jetpack Compose, Material 3; light (default), dark or system theme.

| Setting | gcbridge.ini | Default |
|---|---|---|
| Find server automatically / Server address | `server` | auto |
| Port | `port` | 9888 |
| Access key | `key` | empty |
| Pairing PIN | `pin` | 1234 |
| Service name | `name` | GameComms Bridge |
| N-Gages at the same time | `max_devices` | 4 |
| Log detail | `log_level` | Info |

## Using it

1. Tap the start button. Allow "Nearby devices" (and notifications).
2. On the N-Gage, start a multiplayer game and select the phone in the
   Bluetooth list. If the phone is not listed, tap **Make visible**.
3. If the N-Gage asks for a PIN, enter the PIN from the settings (1234).

The N-Gage shows the phone under the phone's Bluetooth name (Android
Settings > Connected devices > Device name). On phones with aggressive
battery savers, exempt PhantomLink from battery optimization.

## Building

Requirements: Android Studio (Ladybug or newer) or the Android SDK with
platform 36, NDK and CMake 3.22.1 (Gradle installs NDK and CMake on first
build if the SDK licences are accepted), JDK 17 or newer.

Open the `android/` folder in Android Studio and press Run, or:

```sh
cd android
./gradlew assembleDebug           # app/build/outputs/apk/debug/app-debug.apk
./gradlew installDebug            # onto a connected phone
```

`gradlew.bat` on Windows. The native code is built from
`app/src/main/cpp/CMakeLists.txt`, which pulls in `../protocol` and
`../firmware/src`, so build from a full checkout.

## Version number and app identifier

All in `android/app/build.gradle.kts`, block `defaultConfig`:

| What | Line | Notes |
|---|---|---|
| App identifier (package name on Google Play) | `applicationId = "org.ngagesdk.phantomlink.bridge"` | Cannot be changed after the first upload to Google Play. |
| Version code | `versionCode = 1` | Whole number. Must go up with every upload to Google Play. |
| Version name | `versionName = "1.0.0"` | The version users see. |
| Target / minimum Android | `targetSdk = 36`, `minSdk = 26` | Google Play requires target 36 for new apps and updates. |

`namespace = "org.ngagesdk.phantomlink"` (same file) is the Kotlin/Java
package of the code and the `R` class. It is independent of the app
identifier; leave it unless you move the sources.

## Release build

1. Create an upload key once (keep the file and passwords safe, back them up):

   ```sh
   keytool -genkeypair -v -keystore phantomlink-upload.jks -alias upload \
           -keyalg RSA -keysize 4096 -validity 10000
   ```

2. Create `android/keystore.properties` (ignored by git):

   ```properties
   storeFile=phantomlink-upload.jks
   storePassword=...
   keyAlias=upload
   keyPassword=...
   ```

   `storeFile` is relative to the `android/` folder.

3. Build:

   ```sh
   ./gradlew bundleRelease     # app/build/outputs/bundle/release/app-release.aab  (Google Play)
   ./gradlew assembleRelease   # app/build/outputs/apk/release/app-release.apk     (direct install)
   ```

CI (`.github/workflows/continuous-integration.yml`, job `android`) builds
the debug APK, release APK and bundle on every push and uploads them as the
artifact `phantomlink-android`. To have CI sign them, add these repository
secrets: `ANDROID_KEYSTORE_BASE64` (output of `base64 -w0 phantomlink-upload.jks`),
`ANDROID_KEYSTORE_PASSWORD`, `ANDROID_KEY_ALIAS`, `ANDROID_KEY_PASSWORD`.

Without `keystore.properties` the release build is unsigned; Android
Studio's *Build > Generate Signed App Bundle / APK* works too.

Publishing on Google Play: see [store/PLAY_CONSOLE.md](store/PLAY_CONSOLE.md).
Store texts and graphics: `fastlane/metadata/android/`. Privacy policy:
[PRIVACY.md](PRIVACY.md).

## Tests

The bridge engine and its JNI layer are tested on a PC, against the real
server and simulated N-Gages (`gcsim`), with the same cases as the PC
bridge (both API generations, explicit server and LAN discovery, an N-Gage
dropping out and coming back). It runs as part of the normal test suite
when a JDK is installed:

```sh
cmake -S tests -B build/tests && cmake --build build/tests
ctest --test-dir build/tests -R android --output-on-failure
```

Not yet tested: a real N-Gage pairing with and connecting to an Android
phone. Some Android Bluetooth stacks may refuse Bluetooth 1.1 legacy
pairing or the insecure RFCOMM connection; the log shows how far it gets.

## Layout

```
android/
  app/src/main/cpp/            JNI wrapper + CMake (shared C engine)
  app/src/main/java/.../bridge/ BridgeEngine, NativeBridge (plain Java)
  app/src/main/java/.../       service, Bluetooth listener, settings, UI
  tests/                       host test (HostBridge.java, run_host_test.py)
  fastlane/metadata/android/   store texts, icon, feature graphic, screenshots
  store/PLAY_CONSOLE.md        Play Console walkthrough
  PRIVACY.md                   privacy policy
```
