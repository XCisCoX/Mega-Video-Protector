<div align="center">

<img src="docs/MVP-logo.png" alt="Mega Video Protect" width="130" height="130" />

# Mega Video Protect

<p align="center">
  <img src="https://readme-typing-svg.demolab.com?font=Fira+Code&weight=600&size=20&pause=1000&color=5B7CFA&center=true&vCenter=true&width=580&lines=Your+private+video+vault.;Encrypted+on+your+machine.;Playable+without+ever+decrypting+to+disk." alt="Typing SVG" />
</p>

[![C++20](https://img.shields.io/badge/Core-C%2B%2B%2020-536fe8?style=for-the-badge&logo=cplusplus&logoColor=white)](https://en.cppreference.com/w/cpp/20)
[![Qt 5](https://img.shields.io/badge/UI-Qt%205-41cd52?style=for-the-badge&logo=qt&logoColor=white)](https://www.qt.io/)
[![SQLCipher](https://img.shields.io/badge/Database-SQLCipher-65a30d?style=for-the-badge&logo=sqlite&logoColor=white)](https://www.zetetic.net/sqlcipher/)
[![Argon2id](https://img.shields.io/badge/KDF-Argon2id-8b5cf6?style=for-the-badge)](https://en.wikipedia.org/wiki/Argon2)
[![libsodium](https://img.shields.io/badge/Crypto-libsodium-4f8ef7?style=for-the-badge)](https://doc.libsodium.org/)
[![FFmpeg](https://img.shields.io/badge/Media-FFmpeg-d63031?style=for-the-badge&logo=ffmpeg&logoColor=white)](https://ffmpeg.org/)
[![CMake](https://img.shields.io/badge/Build-CMake-064f8c?style=for-the-badge&logo=cmake&logoColor=white)](https://cmake.org/)

</div>

---
<div align="center">

[![Platform](https://img.shields.io/badge/Platform-Linux%20%7C%20Windows-informational?style=flat-square)]() &nbsp;
[![Status](https://img.shields.io/badge/Status-Active-brightgreen?style=flat-square)]() &nbsp;
[![PRs](https://img.shields.io/badge/PRs-welcome-brightgreen?style=flat-square)]()

**[Features](#features)** · **[Screenshots](#screenshots)** · **[Security model](#security-model)** · **[Build & run](#build--run)** · **[Project layout](#project-layout)**

</div>

**Your private video vault — encrypted on your machine, playable without ever decrypting to disk.**

Mega Video Protect is a desktop video organizer that treats privacy as the default. Every video you import is split into authenticated, encrypted packages stored inside a vault locked by a password you choose; nothing leaves your computer. The app plays back your videos **in memory** through a streaming, tamper-checked reader — the plaintext never touches your disk.

## Features

- 🔐 **Real encryption, not hiding.** Argon2id key derivation, an SQLCipher-encrypted database, domain-separated subkeys, and XChaCha20-Poly1305 authenticated packages (`.vvp`) for every video. Each package carries per-chunk authentication — tampering is detected, not assumed.
- 🎞️ **Explorer-style gallery.** Details / Large icons / List views, auto-generated thumbnails at import, and per-video metadata (duration, resolution, codec) — all dark-themed.
- 🏷️ **Tags.** Multi-tag per video, tag filtering + live search, and a tag editor — no folders to break, tags just work.
- ▶️ **In-memory player.** Streaming authenticated reader with a read-ahead cache (4–128 MB), A/V-synced playback, volume/mute, fullscreen, and full keyboard shortcuts. Nothing is decrypted to a temp file.
- 🛡️ **Vault administration.** Password change re-keys the entire vault (database + every wrapped key + thumbnails), and removal is permanent (package deleted, not just unlinked).
- ⏱️ **Auto-lock.** 5 minutes of inactivity locks the vault; re-open with your password.

## Screenshots

| | |
|---|---|
| **Create a vault** — choose a location, password, and security profile. | **Unlock** — the last vault location is remembered. |
| ![Setup](docs/screenshots/01-setup.png) | ![Login](docs/screenshots/02-login.png) |
| **Details view** — name, size, duration, resolution, codec, tags. | **Large icons** — thumbnail grid with live tag filter and search. |
| ![Details](docs/screenshots/03-gallery-details.png) | ![Icons](docs/screenshots/04-gallery-icons.png) |
| **In-memory player** — seek bar, volume, fullscreen, keyboard shortcuts. | **Settings** — cache budget and playback preferences. |
| ![Player](docs/screenshots/05-player.png) | ![Settings](docs/screenshots/06-settings.png) |

## Security model

- **Master key** is derived from your password with **Argon2id** (8 MiB–1 GiB memory, 1–20 iterations, 1–16 lanes — you pick the profile).
- Domain-separated **subkeys** (libsodium `crypto_kdf`) are derived for the database, password verifier, file-key wrapping, and thumbnails — one compromised key never leaks the others.
- The vault database is encrypted with **SQLCipher** (4096-byte pages, HMAC-SHA512, `secure_delete ON`).
- Every video is stored as a `.vvp` package: random 256-bit file key, wrapped under your vault key, chunked and **authenticated per chunk** (header tag + per-chunk AEAD tags). Seeking plays only the chunks it needs, verifying each before its bytes are used.
- Vault creation is **crash-consistent**: the database is built in a temp file, flushed, atomically renamed, and only then is the ready marker written.
- Sensitive buffers live in **guarded/locked memory** (libsodium) and are wiped on release.

## Platforms

- **Linux x64** — validated on Ubuntu 24.04 (GCC 13, Qt 5.15, distro FFmpeg 6.1 / SQLCipher / Argon2 / libsodium).
- **Windows x64** — MSVC 2022 + Qt 5.12.12 + pinned vcpkg dependencies.
- One codebase; platform-specific code is confined to atomic file I/O (`_WIN32` vs POSIX) and dependency discovery (vcpkg vs pkg-config). CI builds and tests both platforms on every release.

## Build & run

### Linux (Ubuntu 24.04)

```text
sudo apt install qtbase5-dev qtmultimedia5-dev libqt5multimedia5-plugins \
  libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev \
  libsqlcipher-dev libargon2-dev libsodium-dev \
  gstreamer1.0-plugins-base gstreamer1.0-plugins-good

cmake --preset linux-debug            # or linux-release
cmake --build --preset linux-debug -j "$(nproc)"
ctest --preset linux-debug --output-on-failure   # all 7 suites, headless

out/build/linux-debug/qt-app/MegaVideoProtect    # run
```

Audio output needs Qt 5 Multimedia's GStreamer backend (`libqt5multimedia5-plugins` + base/good plugins).

### Windows

From a Visual Studio developer shell with Qt 5.12.12 (msvc2017_64) and the vcpkg manifest installed:

```text
cmake --preset vs2022-x64
cmake --build --preset vs2022-x64-debug --target VideoVaultCoreTests VideoVaultApp
ctest --preset vs2022-x64-debug
```

Windows-only notes: the vcpkg FFmpeg port installs stub import libraries — re-run `scripts/regenerate-ffmpeg-importlibs.sh` after any ffmpeg reinstall; FFmpeg 7.x headers carry no `extern "C"` guards (the sources wrap every include).

### Tests

Seven focused suites (Smoke, Gallery, Admin, Reader, Media, Playback, Tags) cover vault creation/opening, wrong-password and tamper detection, import/export round-trips, password re-keying, streaming reads, thumbnails, real (headless) playback, and tag behavior.

## Project layout

```text
core/        Qt-independent C++20 library: crypto, SQLCipher database, .vvp
             packages, streaming reader, FFmpeg probing, thumbnails, tags
qt-app/      Qt 5 desktop application: setup/login, gallery, player, settings
tests/       ctest suites for the core (headless)
scripts/     Windows-only FFmpeg import-library regeneration
third_party/ vcpkg overlay ports + release-only CI triplet (Windows builds)
.github/     Release CI: Windows x64 + Linux x64, both test suites
```

## Tech stack

C++20 · Qt 5 (Widgets, Multimedia) · Argon2id · libsodium (XChaCha20-Poly1305, crypto_kdf) · SQLCipher · FFmpeg (libavcodec/libavformat/swscale/swresample) · CMake · vcpkg (Windows) / pkg-config (Linux)
