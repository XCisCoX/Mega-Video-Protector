# Mega Video Protect

Production-oriented Windows video-vault project, currently at completed Phase 1: architecture and verified build foundation.

## Verified local toolchain

- Visual Studio 2022 Build Tools 17.14.21, MSVC toolset 14.44.35207
- CMake 3.31.6 (Visual Studio bundled)
- Windows SDKs 10.0.10240.0, 10.0.22621.0, and 10.0.26100.0; preset pins 10.0.22621.0 for the current Qt kit
- Qt framework 5.12.12 at `C:/Qt/Qt5.12.12/5.12.12/msvc2017_64`
- Qt kit: desktop MSVC 2017 x64; used with the newer VS 2022 linker/toolset under Microsoft's supported v14x binary-compatibility model
- Qt Core, Widgets, Multimedia, MultimediaWidgets, SQL, and release/debug SQLite plugins are present

Qt Creator 5.0.2 is the IDE version, not the framework version. The MinGW kits are intentionally not selected.

## Build

From a Visual Studio developer shell, or with the bundled CMake executable:

```text
cmake --preset vs2022-x64
cmake --build --preset vs2022-x64-debug --target VideoVaultCoreTests VideoVaultApp
ctest --preset vs2022-x64-debug
```

The generated Visual Studio solution is under `out/build/vs2022-x64` and supports Debug and Release.

## Dependency status

No compatible local x64 libsodium, SQLCipher, or FFmpeg libraries were found in the inspected project/common dependency roots. They are not silently downloaded, and Phase 1 does not substitute insecure implementations. The installed Qt `QSQLITE` plugin is useful for ordinary SQLite but is not SQLCipher.

Before Phase 2, pin an MSVC x64 dependency source and versions for libsodium and SQLCipher. FFmpeg is required in Phase 5. See `docs/architecture.md` for the security, package, database, threading, and playback design.

## Current targets

- `VideoVaultCore`: Qt-independent C++20 static library
- `VideoVaultApp`: Qt 5.12.12 Windows GUI linked to the core
- `VideoVaultCoreTests`: focused native smoke target

No vault or encryption feature is represented as available in Phase 1.
