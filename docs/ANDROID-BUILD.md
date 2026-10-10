# Building the Android app

The Android app is a **native Kotlin/Jetpack Compose application** that reuses
the Qt-independent C++ core (`core/`) through a JNI bridge
(`android/app/src/main/cpp/jni_bridge.cpp`, built as `libmvpcore.so`). The UI
mirrors the desktop Qt UI 1:1 (same screens, strings, and dark theme).

The vault lives in the app's private sandbox (`filesDir`), videos are imported
through the Storage Access Framework, and playback decodes in memory via the
core's streaming reader — nothing is ever decrypted to disk, matching the
desktop app's security model.

## Artifacts

| Artifact | Path |
|---|---|
| Debug APK (arm64-v8a) | `out/android-arm64/MegaVaultProtect-native-debug.apk` |
| Native libs (deps + core) | `out/android-arm64/prefix/lib/*.a` |
| Build log | `out/android-arm64/gradle-build.log` |

The APK is ~13 MB: the Compose app (classes.dex) plus `libmvpcore.so`
(8.4 MB — the entire core + FFmpeg, statically linked, needing only
`liblog`/`libm`/`libdl`/`libc` at runtime).

## Prerequisites

* Docker (the build runs in a pinned NDK container).
* ~6 GB free disk for the SDK/dependency build trees.
* A phone (arm64-v8a, Android 8.0+/API 26+) with `adb` for on-device testing.

The whole pipeline is offline-friendly: every dependency is fetched from
mirrors reachable from Iran (codeload for sources, Aliyun/Huawei for Maven
artifacts and Gradle) and cached under `out/android-arm64/`.

## Build steps

```text
# 1. Cross-compile the dependencies + core for arm64-v8a (once; ~15-20 min).
#    Builds openssl 3.0.16, libsodium 1.0.18, sqlcipher v4.5.6, ffmpeg n7.1.1
#    into out/android-arm64/prefix, then runs the aarch64 test suites under
#    qemu-user. Idempotent — re-run to skip finished steps.
docker run --rm --user root -e HOST_UID="$(id -u)" \
  -v "$PWD":/repo \
  saschpe/android-ndk:36.1-jdk25.0.3_9-ndk30.0.14904198-cmake3.31.6 \
  bash scripts/android/build-android-core.sh

# 2. Build the APK (~3 min after the first Gradle sync). The script installs
#    JDK 21, wires the SDK (build-tools 35.0.0, platform android-36, CMake
#    3.22.1), and runs `gradle assembleDebug` with a persisted Gradle cache.
docker run --rm --user root -e HOST_UID="$(id -u)" \
  -v "$PWD":/repo \
  saschpe/android-ndk:36.1-jdk25.0.3_9-ndk30.0.14904198-cmake3.31.6 \
  bash out/android-arm64/build-android-app.sh

# 3. Install on a device (debug-signed).
adb install -r out/android-arm64/MegaVaultProtect-native-debug.apk
```

## How the SDK wiring works (and why)

The NDK image (`saschpe/android-ndk`) ships an Android SDK with **only** the
NDK, CMake, and platform `android-36.1` — no build-tools, and it cannot reach
`dl.google.com` to install any (the developer network geo-blocks it). The app
build script fakes the missing pieces instead:

* **build-tools 35.0.0** — AGP 8.13 refuses to build without it and validates
  the directory against sdklib's full tool walk (`aapt`, `aapt2`, `aidl`,
  `dexdump`, `zipalign`, `core-lambda-stubs.jar`, `split-select`, the
  `ld-*`/`lld` linkers, ...). The script creates the directory with every tool
  present (copies of the aapt2 binary / no-op scripts — AGP 8 uses its own
  aapt2 and D8 from Maven, these files only need to exist).
* **platform android-36** — AGP resolves `compileSdk 36` to the literal
  `platforms/android-36`; the image ships `android-36.1`. The script clones
  the platform, deletes `package.xml` (forcing sdklib's legacy
  `source.properties` loader) and sets `AndroidVersion.ApiLevel=36`.
* **CMake 3.22.1** — AGP's default; the image ships 3.31.6. The script clones
  the installed CMake under the expected directory name.

On GitHub Actions (unrestricted network) none of this is needed — the
workflow installs the real SDK packages with `sdkmanager`.

## Building on a Windows host (no Docker, restricted network)

`scripts/android/build-android-core.sh` also runs on a Windows host with an NDK
installed (`ANDROID_NDK_ROOT=/c/path/to/ndk`); it detects the host toolchain
directory (`windows-x86_64`), resolves `make` as `mingw32-make`, and drives
clang directly with an explicit `--target=` instead of the NDK's launcher
scripts (POSIX wrappers on Linux, `.cmd` on Windows). Points that matter:

* **Paths must not contain spaces.** OpenSSL's perl `Configure` and FFmpeg's
  makefiles break on them (perl `@INC` splitting, make word splitting), so build
  from a space-free copy of the tree — e.g. copy the sources to `C:/mvpbuild`
  and run the build there. A directory junction is *not* enough: `getcwd()`
  resolves back to the real (spaced) path.
* **Drive-letter vs POSIX paths.** Native tools (clang, cmake, perl) need
  `C:/...`; MSYS tools (`tar`) and the shell's own `PATH` lookups need
  `/c/...`. The script's `win_path` / `posix_path` helpers convert, and
  `ANDROID_NDK_ROOT` is handed to OpenSSL's `Configure` in POSIX form because
  that script matches `which("clang")` against `$ndk/.../prebuilt/`.
* **Perl.** OpenSSL's `Configure` must run under the MSYS/Cygwin-style perl
  (which splits `PATH` on `:` and returns POSIX paths). Git for Windows ships a
  cut-down perl missing `Locale::Maketext::Simple`, `ExtUtils::MakeMaker` and
  `Pod::Usage`/`Pod::Text`; copy those pure-Perl modules out of a Strawberry
  Perl install into a directory and point `PERL5LIB` at it. A native Windows
  perl cannot be used at all here — it splits `PATH` on `:` and so never finds
  a drive-letter path.
* **curl.** Git for Windows' curl aborts every HTTPS transfer with
  `CRYPT_E_REVOCATION_OFFLINE` when the CRL server is unreachable; the script
  adds `--ssl-no-revoke` on Windows hosts.
* **Gradle wrapper.** If the wrapper's own download fails (transient Java TLS
  error), fetch the distribution zip with curl into
  `~/.gradle/wrapper/dists/gradle-<version>-bin/<hash>/gradle-<version>-bin.zip`,
  delete any stale `.part`/`.lck`, and re-run `./gradlew`.

Mirrors that work from Iran (the paths this build was validated against):
`mirrors.cloud.tencent.com/AndroidSDK` (SDK + NDK + build-tools), Adoptium via
`api.adoptium.net` (JDK), `mirrors.huaweicloud.com/gradle` (Gradle),
`maven.aliyun.com/repository/{google,central,gradle-plugin}` (Maven), and
`codeload.github.com` (dependency sources). `dl.google.com` is filtered.

## CI (GitHub Actions)

`.github/workflows/android-build.yml` builds the same arm64-v8a debug APK on
every push to `main` (plus PRs and manual `workflow_dispatch`), and is the only
workflow that touches Android — `release.yml`/`beta.yml` are desktop releases
triggered by tag or by hand.

What it does, in order: JDK 21 and the real Android SDK (platform 36,
build-tools 35.0.0, NDK 30.0.14904198 from the stable channel, CMake 3.22.1) via
`sdkmanager`; `scripts/android/build-android-core.sh`, which also **runs the
aarch64 test suites under qemu-user and fails the job if one fails**; then
`./gradlew assembleDebug` with `MVP_ANDROID_PREFIX` pointing at the freshly built
`out/android-arm64/prefix`.

Three caches keep it bearable: the SDK packages (the NDK alone is ~1 GB), the
cross-built deps + core (`out/android-arm64`, keyed on the build script and
`core/**`), and `~/.gradle`. A cold run is ~20-25 min; a warm one is ~5 min. A
newer push cancels an in-flight run.

Before uploading, the job asserts the APK really carries a working native core:
`lib/arm64-v8a/libmvpcore.so` and `classes.dex` present, and every JNI entry
point the Kotlin side declares exported from the `.so`. A broken native link
otherwise produces a "successful" build with a dead core.

The artifact is `MegaVaultProtect-native-debug` (30-day retention). To build
without pushing: Actions → Android Build → Run workflow.

## Project layout (Android)

```text
android/
  settings.gradle.kts        # Aliyun-mirrored repositories (google/central/plugin)
  app/
    build.gradle.kts         # AGP 8.13, Kotlin 2.1.20, Compose BOM, JNI CMake
    src/main/
      AndroidManifest.xml
      java/org/megavideoprotect/app/
        MainActivity.kt      # screen state machine (setup/login/vault/player)
        Theme.kt             # exact desktop Qt color tokens
        Components.kt        # MvpCard / MvpButton / MvpInput / MvpErrorBanner
        CoreBridge.kt        # JNI surface (18 externals, JSON-marshalled)
        Models.kt            # VideoEntry / TagEntry (org.json parsing)
        SetupScreen.kt       # "Create your encrypted vault"
        LoginScreen.kt       # "Unlock Mega Vault Protect"
        VaultScreen.kt       # toolbar + Details/Icons/List + SAF import
        SettingsDialog.kt    # change password + tag management
        PlayerScreen.kt      # seekable frame preview (decode via JNI)
      cpp/
        jni_bridge.cpp       # Vault wrapper + FFmpeg frame decoder over
                             # Vault::read_video_range (never writes plaintext)
        CMakeLists.txt       # compiles core/src/*.cpp, links prefix static libs
      res/                   # theme, strings, launcher icon
```

## Notes / limitations

* The player currently decodes **frames** on demand (seek + preview); audio
  playback is next on the roadmap.
* Import copies the picked file to a temp path inside the app sandbox, then
  imports it into the vault (the encrypted `.vvp` package). Streaming import
  straight from the content URI is planned.
* `android/local.properties` is machine-specific and gitignored; the build
  script writes `sdk.dir` automatically.
* CI builds the same APK on every push — see
  `.github/workflows/android-build.yml`.
