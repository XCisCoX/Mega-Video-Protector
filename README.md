# Mega Video Protect

Cross-platform (Windows x64 + Linux x64) secure video-vault desktop application. Phase 1 (architecture and verified build foundation), Phase 2 (secure vault core + setup/login UI shell), Phase 3 (encrypted import + gallery), Phase 4 (vault administration: removal and password change), Phase 5 (streaming authenticated reader, FFmpeg media probing, and encrypted thumbnails), Phase 6 (auto-thumbnails on import, thumbnail re-keying on password change, Explorer-style gallery views, and in-memory video playback), and Phase 7 (multi-tag organization with tag filtering, and a full Explorer-style unlocked view) are complete. Windows and Linux builds share the same codebase; platform-specific pieces are limited to atomic file I/O and dependency discovery (see below).

## Platforms

- **Windows x64** (primary): MSVC 2022 + Qt 5.12.12 + vcpkg manifest dependencies.
- **Linux x64**: GCC + distro Qt 5 / FFmpeg / SQLCipher / Argon2 / libsodium via pkg-config (validated on Ubuntu 24.04).
- The core (`VideoVaultCore`) is Qt-independent C++20 and is identical on both platforms. Platform-specific code is confined to `core/src/metadata.cpp` (Win32 vs POSIX atomic file I/O, selected by `_WIN32`) and the CMake dependency-discovery blocks.

## Verified Windows toolchain

- Visual Studio 2022 Build Tools 17.14.21, MSVC toolset 14.44.35207
- CMake 3.31.6 and ctest (Visual Studio bundled)
- Windows SDK 10.0.22621.0 pinned by the preset for the current Qt kit
- Qt framework 5.12.12 at `C:/Qt/Qt5.12.12/5.12.12/msvc2017_64`
- Qt kit: desktop MSVC 2017 x64; used with the newer VS 2022 linker/toolset under Microsoft's supported v14x binary-compatibility model
- Qt Core, Concurrent, Widgets, Multimedia, and MultimediaWidgets are used. Qt SQL is intentionally not used: the core owns SQLCipher through its C API.
- vcpkg (bundled with VS 2022 Build Tools) provides `argon2`, `libsodium`, `sqlcipher`, and `ffmpeg` as pinned x64 MSVC builds (see `vcpkg.json` baseline). FFmpeg 7.1.1 is installed lean: `avcodec`, `avformat`, `swscale`, and the `ffmpeg` CLI feature (used to validate test fixtures); `avutil` is implicit in the core.

Qt Creator 5.0.2 is the IDE version, not the framework version. MinGW kits are intentionally not selected.

## Dependencies

Phase 2 pins the production cryptographic/database dependencies through the vcpkg manifest:

- `argon2` — Argon2id derivation (`argon2id_hash_raw`), pinned via manifest baseline
- `libsodium` — guarded memory, random generation, `crypto_kdf` subkey derivation, XChaCha20-Poly1305, constant-time compare
- `sqlcipher` — encrypted vault database, linked through its C API
- A vcpkg overlay port for `tcl` (`third_party/vcpkg-overlays/tcl`) carries the MSVC 14.44 build fixes SQLCipher's Windows build needs (implicit rules, shell install, generic Windows fixes)
- Strawberry Perl 5.42.0.1 portable x64 is required by the OpenSSL build that SQLCipher links; the file was verified by SHA-512 before use
- `ffmpeg` (Phase 5): two Windows quirks are handled by `scripts/regenerate-ffmpeg-importlibs.sh` — the vcpkg port installs stub `.lib` import libraries (no symbol entries; MSVC link fails with LNK2019 on every `av*` symbol), so real import libs are regenerated from the DLL export tables with `dumpbin` + `lib.exe`; and FFmpeg 7.x public headers carry no `extern "C"` guards, so every FFmpeg include in C++ translation units is wrapped in `extern "C" { ... }`. Re-run the script after any vcpkg reinstall of ffmpeg.

vcpkg build trees, packages, AND the install root are redirected outside the project (`C:/Users/cisco/AppData/Local/MegaVideoProtect/...`) because the source path contains a space — FFmpeg's MSVC response files break on the space in "Mega King" (LNK1181 on `-libpath`). `VCPKG_MANIFEST_INSTALL` is OFF in the preset; `VCPKG_INSTALLED_DIR` points at the AppData install root (git-ignored).

## Build (Windows)

From a Visual Studio developer shell, or with the bundled CMake executable:

```text
cmake --preset vs2022-x64
cmake --build --preset vs2022-x64-debug --target VideoVaultCoreTests VideoVaultApp
ctest --preset vs2022-x64-debug
```

The generated Visual Studio solution is under `out/build/vs2022-x64` and supports Debug and Release. Only Debug has been exercised; Release verification is scheduled for a later phase.

## Build (Linux)

Ubuntu 24.04 packages (other distros provide the same libraries; names may differ):

```text
sudo apt install qtbase5-dev qtmultimedia5-dev libqt5multimedia5-plugins \
  libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev \
  libsqlcipher-dev libargon2-dev libsodium-dev \
  gstreamer1.0-plugins-base gstreamer1.0-plugins-good
```

Build and test (all 7 suites run headless via the offscreen Qt platform):

```text
cmake --preset linux-debug            # or linux-release
cmake --build --preset linux-debug -j "$(nproc)"
ctest --preset linux-debug --output-on-failure
```

The app binary is `out/build/linux-debug/qt-app/MegaVideoProtect`. Runtime notes:

- Audio output uses Qt 5 Multimedia (GStreamer backend) — `libqt5multimedia5-plugins` and GStreamer base/good plugins are required.
- The Linux build links distro shared libraries (Qt, FFmpeg 6.1, SQLCipher, Argon2, libsodium). The vcpkg manifest and `scripts/regenerate-ffmpeg-importlibs.sh` are Windows-only concerns and are not used on Linux.

## Phase 2 verified behavior

`VideoVaultCore` (Qt-independent C++20) implements, and `VideoVaultCore.Smoke` verifies:

- Vault creation at a chosen directory: 136-byte `vault.meta` (header + sealed verifier), SQLCipher `vault.db`, `vault.ready` marker, and `vault-data/` staging area. Creation is crash-consistent: the database is built in `.tmp`, flushed, atomically renamed, metadata is written atomically, and the ready marker is written last.
- Argon2id master key derivation (memory 8 MiB–1 GiB, iterations 1–20, parallelism 1–16; password ≤ 4096 bytes), domain-separated subkeys via libsodium `crypto_kdf` (database subkey `MVPDB001`, password-verifier subkey `MVPVEr01`).
- Opening with the correct password, explicit `lock()`, and a `validate_password` convenience check.
- Typed errors: a wrong password returns `WrongPassword`; a modified password verifier returns `AuthenticationFailed` (distinguished by probing whether the SQLCipher key still opens the database).
- SQLCipher hardening: 4096-byte pages, HMAC-SHA512, PBKDF2-HMAC-SHA512 with 256000 iterations, `cipher_memory_security ON`, `secure_delete ON`, `foreign_keys ON`; schema (`schema_migrations`, `vault_metadata`) created in a `BEGIN IMMEDIATE` transaction.
- Sensitive buffers backed by libsodium guarded/locked memory, wiped on release (best effort, as documented).

`VideoVaultApp` is a Qt 5.12.12 shell that consumes only public core APIs: a setup page (location, password + confirmation, Argon2id security profile), a login page (wrong-password feedback), and an unlocked page with a gallery list, an "Import video…" button, a lock button, and a 5-minute activity-based auto-lock. Core operations (create, open, import, gallery listing) run on `QtConcurrent` threads; the UI thread performs no crypto or database work. The last opened vault location is remembered with `QSettings`.

## Phase 3 verified behavior

`VideoVaultCore.Gallery` verifies the encrypted import and gallery pipeline on top of Phase 2:

- `Vault::import_file` streams any regular file into a chunked XChaCha20-Poly1305 package (default 64 KiB plaintext chunks) at `vault-data/<h0h1>/<h2h3>/<sha256-of-ciphertext>.vvp`. The package is staged in `vault-data/.tmp`, flushed, atomically renamed into place, and only then committed to the database together with its wrapped file key in one transaction; a failed commit removes the package.
- Each import draws a fresh random 256-bit file key and package id, so importing identical source content produces a distinct package and a new gallery entry (the design hashes the ciphertext, so content-level deduplication is not performed).
- Every package carries an authenticated 68-byte header (magic `MVPVVP01`, format version 1, algorithm 1, chunk size, plaintext size, chunk count, package id, nonce prefix) plus a header AEAD tag. Chunk associated data binds the header digest, package id, chunk index, plaintext length, and final-chunk flag; nonces are the random 16-byte prefix plus the 64-bit chunk index (the header uses the reserved all-ones index).
- File keys are wrapped under the domain-separated wrapping subkey (id 3, context `MVPRAP01`) with the package id as associated data; only the wrapped key and nonce are persisted in `video_keys`.
- Database schema migrated to version 2 (`videos`, `video_keys`); migrations run automatically on create and on open of existing vaults.
- `Vault::list_videos` returns the gallery; `Vault::package_path` resolves the stored package (reporting `PackageMissing` if gone); `Vault::read_video_bytes` unwraps the file key and decrypts the whole package, verifying the header tag and every chunk tag (`PackageModified` on tamper, `UnsupportedPackageVersion` on a newer format).
- Vault operations are now safe from worker threads: a per-vault mutex (stable address across moves) serializes `lock()`, gallery, and import calls; the Qt shell hands worker threads a shared vault.

## Phase 4 verified behavior

`VideoVaultCore.Admin` verifies vault administration on top of Phase 3:

- `Vault::remove_video` deletes the gallery row (the wrapped key cascades via `ON DELETE CASCADE`) and then removes the stored package file. Unknown ids return `InvalidArgument`; the remaining videos stay fully readable and the package file for the removed video is gone from disk.
- `Vault::change_password` verifies the caller knows the current password by re-deriving the master key from the on-disk metadata and comparing it in constant time with the live master key (`WrongPassword` on mismatch, with no state change). It then derives a fresh key hierarchy from the new password (new random salt, optional new Argon2id parameters), re-wraps every file key under the new wrapping subkey, writes the new external metadata with a freshly sealed password verifier, updates the database metadata record and all wrapped keys, and re-encrypts the whole SQLCipher file with `sqlite3_rekey`. On success the open session continues under the new credentials; the old password is rejected (`WrongPassword`) and the new one opens the vault with the gallery and every video intact. Chained changes work.
- Administration calls on a locked vault fail cleanly with `InvalidArgument`.

## Phase 5 verified behavior

`VideoVaultCore.Reader` verifies the streaming authenticated reader, and `VideoVaultCore.Media` verifies the FFmpeg media pipeline:

- `PackageReader` (internal) opens a package with the unwrapped file key, verifies the header, and serves authenticated plaintext on demand: `seek` to any offset and `read` across chunk boundaries, with a bounded LRU cache of decrypted chunks (4 x 64 KiB). Every touched chunk's AEAD tag is verified before its bytes are exposed (`PackageModified` on tamper, and untouched chunks in the same package stay readable). This is the foundation for probe/thumbnail/playback without ever decrypting a whole video to disk or to a plaintext temp file.
- `Vault::read_video_range(video_id, offset, size)` exposes the reader publicly (fewer bytes at EOF, empty at/past end); `read_video_bytes` now delegates to it. Cross-chunk ranges, chunk-boundary slices, the final partial chunk, EOF behavior, tamper detection, missing packages, unknown ids, and locked-vault failures are all covered.
- `Vault::media_info(video_id)` probes container metadata through FFmpeg streaming over the encrypted package via a custom AVIO context layered on `PackageReader` (no plaintext touches disk): duration (ms), width/height, rotation from the display matrix side data, and the codec name. Non-media content returns `UnsupportedVideoFormat`.
- `Vault::generate_thumbnail(video_id, max_dimension)` seeks to ~10% of the stream (capped at 10 s), decodes a representative frame, scales it to fit `max_dimension` (aspect-preserving, no upscaling), and encodes a JPEG (yuv420p, full-range). The JPEG is encrypted under the domain-separated thumbnail subkey (id 4, context `MVPTMB01`) with `video_id || package_id` as associated data and stored in the new `thumbnails` table (schema version 3, `ON DELETE CASCADE` from `videos`).
- `Vault::thumbnail(video_id)` returns the stored thumbnail decrypted for display; thumbnails persist across lock/reopen and cascade away with `remove_video`.
- The media test builds its own fixture at runtime with the FFmpeg libraries (MJPEG codec in a Matroska container, 64x64, 10 fps, 20 frames — the AVI/MKV muxers reject rawvideo, so the fixture uses a real codec with proper duration signaling).
- The Qt shell gained a "Generate thumbnail" button and asynchronously attaches stored thumbnails as 96 px icons to gallery rows (build-verified; UI behavior is user-tested).

## Phase 6 verified behavior

`VideoVault.Playback` verifies the in-memory player engine; the gallery and thumbnail behaviors are build-verified and user-tested:

- **Thumbnails at import time.** `Vault::import_file` now generates and stores a thumbnail immediately after the database commit (best effort — non-video imports and undecodable formats fail quietly and never fail the import). The gallery therefore shows media pictures without any manual step; the "Regenerate thumbnail" button remains for replacing a bad frame.
- **Thumbnails survive password changes.** A Phase 5 gap is closed: thumbnails are encrypted under a subkey derived from the master key, so `change_password` now re-encrypts every stored thumbnail under the new thumbnail subkey (best effort per row; a damaged thumbnail is dropped rather than failing the change). The media test asserts byte-for-byte thumbnail equality across a password change and reopen.
- **Explorer-style gallery.** The unlocked page has a view switcher with three modes like Windows Explorer: "Details" (columns: Name, Size, Duration, Resolution, Codec, Imported — metadata filled asynchronously per video), "Large icons" (128 px thumbnails on a grid), and "List" (small icons). Thumbnails attach to both views; double-clicking any entry opens the player.
- **In-memory playback.** `MediaDecoder` (in `qt-app`) streams the plaintext of an encrypted video through `Vault::read_video_range` (bounded, per-chunk authenticated) via a custom FFmpeg AVIO context — the same technique the Phase 5 probe path proved — and decodes video + audio entirely in memory: nothing is written to disk and no whole-file plaintext buffer exists. `PlayerWindow` shows frames on a timer paced by the audio clock (wall clock when silent), with play/pause, a seek slider, and a position label; decoded audio (resampled to s16le via swresample) feeds a `QAudioOutput` through a pull-mode `AudioSink` QIODevice. The playback test imports a fixture with MJPEG video + PCM audio and verifies duration, resolution, ≥10 decoded frames, a valid first frame, post-seek decoding, and non-empty audio samples — all headless.
- FFmpeg now also builds the `swresample` feature; `scripts/regenerate-ffmpeg-importlibs.sh` covers the fifth import library.

## Phase 7 verified behavior

`VideoVaultCore.Tags` verifies the tagging core; the explorer UI is build-verified and user-tested:

- **Tags.** Schema version 4 adds `tags` (case-insensitively unique names) and `video_tags` (many-to-many, both FKs `ON DELETE CASCADE`). `Vault::add_tag(video_id, name)` trims the name, rejects empty / over-64-byte / control-character names (`InvalidArgument`), creates the tag on demand, and attaches it idempotently — re-adding "family" to "Family" returns the same tag id. `remove_tag`, `tags_for_video`, and `list_tags` (with per-tag video counts) round out the API; `VideoInfo` now carries each video's tag names, so gallery listing and client-side filtering need no extra queries per video.
- **Tag hygiene.** Removing a video cascades its tag associations (the tag itself survives with a zero count); tags persist across lock/reopen; a locked vault rejects all tag operations with `InvalidArgument`.
- **Explorer-style unlocked view.** The unlocked page is now a full-bleed file-explorer layout: a slim command bar (view mode + tag filter on the left, Import video…, Change password…, and a small flat Lock button on the right), the gallery filling the whole window, and a status bar (vault path, "N videos · X MB", inline messages). The old centered card, headings, and big buttons are gone. The view mode is remembered in `QSettings`.
- **Tag filtering and editing.** The toolbar has a tag filter dropdown (every tag with its count, plus "All videos") AND a live search box: typing filters by tag substring or video name (case-insensitive, combined with the dropdown when both are active); the status bar shows the filtered count, and an empty result says so. Right-clicking any entry opens an Explorer-style context menu: Play, Edit tags…, Regenerate thumbnail, Remove. The tag editor dialog shows all tags as checkboxes (checked = applied) with a "new tag" input; changes are applied on a worker thread and both the filter and gallery refresh automatically. Tags also appear as a column in Details view.
- **Dark-theme and icon fixes.** Details view and both gallery views now carry explicit dark styling (tree background, alternate rows, selected/hover items, header sections, scrollbars); large-icon mode no longer clips icons — the grid accounts for wrapped text, uniform item sizing is off, scrolling is per-pixel, and switching views relayouts from the top.
- **Upgraded player.** The player window is PotPlayer-flavored: a bottom control bar with a live seek bar, "m:ss / h:mm:ss" time, a volume slider with mute (persisted in `QSettings`), and a fullscreen button. Double-click the video or press F for fullscreen (controls auto-hide after 2.5 s while playing; mouse movement brings them back, Esc exits). Keyboard: Space play/pause (replays from the start when the video ended), Left/Right ±5 s, Up/Down volume. Seeking lands exactly where the slider points (offset-based audio clock; the first frame at the target is decoded and shown immediately), and scrubbing while paused stays paused.
- **A/V sync fixes.** Two playback bugs fixed in this round. (1) Sound did not start at the beginning: `QAudioOutput` enters `IdleState` and stops pulling when its very first read returns 0 bytes, so an empty sink meant silence until an explicit resume (the Space workaround). The sink is now prefilled with ~200 ms of decoded audio before `start()`, and `feedAudio()` wakes the output whenever it idles again. (2) Video raced ahead of audio ("sound is not mixed with video"): the tick loop decoded one full frame per 16 ms tick regardless of the playhead, so low-fps content played 2–6x real speed. Video now only decodes while its last shown timestamp is behind the playhead, and the playhead uses the audio clock only while the device is actively consuming (wall clock otherwise) — A/V stays locked. The device buffer was also cut from ~0.5 s to ~125 ms to tighten sync.
- **Streaming buffer (selectable memory budget).** Streaming is no longer per-read: `MediaDecoder` now serves every FFmpeg AVIO read from a read-ahead window of the plaintext package (`StreamCache`) — 1 MiB aligned, refilled with a single `read_video_range` call as the playhead approaches the end, and re-anchored on seeks. Previously each tiny AVIO read (often 4 KB) caused a fresh package open (DB query + key unwrap) plus per-chunk decryption — the constant disk/crypto hits that made playback stutter. The player has a "Cache" selector (4/8/16/32/64/128 MB, default 32 MB, persisted in `QSettings`) that sets the window size live; the AVIO buffer was raised to 128 KB. The headless playback test exercises the budget API and decoding through the cache.
- **Worker-thread decode pipeline.** Playback no longer decodes on the UI thread: a dedicated worker thread streams through `MediaDecoder` (blocking cache refills and frame scaling now happen off the UI), publishes each frame to a thread-safe slot, and appends decoded PCM directly to the audio sink (thread-safe). The UI thread only paints the latest frame, drives the seek bar, and owns the audio device. Pacing is driven by a monotonic real-time clock (`QElapsedTimer`) with the audio device as A/V master when the clocks drift >250 ms — decode speed and window size can no longer speed up or stall the video, which fixes both the "pause/play every 0.1 s" freezing (UI never blocks) and "smaller window = faster video" (pacing no longer follows the device's processed-time count, which previously leapt whenever the UI decoded fast).
- **Performance pass (one-scale decode + direct paint).** Two heavy costs removed from the frame path. (1) `MediaDecoder` now converts frames straight to the display size: the worker tells it the surface size (`set_display_size`) and sws scales decode→surface in a single pass (rebuilt only on resize) — previously the pipeline did decode-scale to source size AND a second QImage scale per frame. (2) The player surface is a custom `VideoSurface` widget that paints the (already display-sized) frame directly with `QPainter::drawImage` — the old QLabel path did a `QPixmap::fromImage` deep copy (8 MB for 1080p) plus another scale per frame. Pacing no longer reads Qt's flaky `processedUSecs` clock at all (it throttled video whenever the UI thread was busy): the worker holds video only when the audio sink exceeds ~0.8 s of buffered samples, which keeps A/V locked without letting a busy UI or a slow device clock slow the picture. This fixes the "1080p plays at 4–10 fps / fewer frames than the video" report: at default window size the video is now decoded at ~window resolution (e.g. 960x540) in one scale, and fullscreen needs no scaling at all.

- **Seek fixes.** Scrub-to-position is repaired: the UI tick no longer overwrites the slider while the thumb is being dragged (that made the slider snap back and look like the seek never happened), a live position preview shows while dragging, and a manual seek clears the ended state — previously, after a video finished, the tick kept forcing the slider to the end and pressing Play force-restarted from 0. The playback test now asserts the first post-seek frame is at/near the target, guarding against silent seek-to-start.
- **Seek lands where you drag.** Two real causes of "the video won't go forward/backward" fixed. (1) The AVIO layer advertised the ENCRYPTED package size as the stream size — the `.vvp` file is larger than the plaintext (header + per-chunk AEAD overhead), which skewed `SEEK_END`/`AVSEEK_SIZE` and the stream cache's EOF clamping; the decoder now uses the original imported size (`VideoInfo.original_size`). (2) After a seek the demuxer starts at the last keyframe BEFORE the target — for videos with sparse keyframes the player showed that (possibly seconds-earlier) frame, looking like the seek didn't happen. The worker now decodes-and-skips forward from the keyframe to within 250 ms of the target before showing a frame (gap audio discarded, bounded at 120 frames), so the picture lands where the slider points. The seek API also falls back from `av_seek_frame` (backward) to `avformat_seek_file`, `seek_to` returns the FFmpeg error code, and a failed seek shows a transient "Seek failed: …" overlay on the picture (plus a qWarning) instead of silently continuing.
- **Player keyboard shortcuts work with any focus.** Space/←/→/↑/↓/F/Escape were handled in the dialog's `keyPressEvent`, which never fires when a child widget (the seek slider or a button) holds focus — arrows did nothing (or nudged the slider by 1 ms). A qApp-level event filter now intercepts player keys before any child sees them, so the shortcuts always work; the dialog override remains as a fallback. Verified by the headless player test.
- **Headless player test + interframe fixture.** The playback suite now generates an MPEG-4-in-MP4 fixture with AAC audio and a keyframe every 1.0 s (the shape of real user videos; the old MJPEG fixture is all-keyframes and cannot reproduce sparse-keyframe seek bugs) and runs strict post-seek checks (first frame at/near the target — catches silent restarts and stale positions; pts must advance through the target). It then instantiates the REAL `PlayerWindow` on the offscreen platform — worker thread, tick timer, clock re-anchor — and drives the slider-release → doSeek path programmatically, asserting the position jumps to the middle, keeps advancing, and jumps back.

## Releases (GitHub Actions)

`.github/workflows/release.yml` builds Release packages for **Windows x64 and Linux x64** on GitHub and publishes them as a GitHub Release. Two ways to trigger it:

1. **Manual** — GitHub → Actions → "Release" → *Run workflow* → optionally type a version (e.g. `0.4.0`; empty = auto `vYYYY.MM.DD.HHMM`). The workflow creates the tag itself.
2. **Tag push** — `git tag v0.4.0 && git push origin v0.4.0`.

What it does:
- **Windows job**: fresh vcpkg bootstrap (the repo pins baseline `b1b19307…`), installs argon2/libsodium/sqlcipher/ffmpeg (lean, with `swresample`), regenerates the FFmpeg import libraries from the DLL export tables (vcpkg's are stubs — same step local builds require), configures with the `vs2022-x64` preset overridden for CI paths (Qt from `jurplel/install-qt-action`, space-free `RUNNER_TEMP` install root), builds **Release**, runs all 7 ctest suites, and packages `MegaVideoProtect-windows-x64.zip` (exe + Qt runtime via `windeployqt` + the five FFmpeg DLLs).
- **Linux job**: installs distro Qt 5 / FFmpeg / SQLCipher / Argon2 / libsodium via apt, builds the `linux-release` preset, runs all 7 ctest suites headless (offscreen Qt platform), and packages `MegaVideoProtect-linux-x64.tar.gz` (the binary; it links distro shared libraries).
- **Release job**: collects both artifacts and attaches them to the GitHub Release.

Notes:
- The Release configuration is verified locally before shipping (see the test targets); the workflow mirrors the local build chain, so a green local build+ctest is a strong predictor of a green CI run.
- The first CI run also validates the pieces that only exist on the runner (fresh vcpkg clone resolving the pinned baseline, `windeployqt` output, Release link).
- Requires the repository to be on GitHub with Actions enabled; push the workflow file with the rest of your commit.

## Current targets

- `VideoVaultCore`: Qt-independent C++20 static library
- `VideoVaultApp`: Qt 5 (5.12+; Windows GUI linked to the core; output `MegaVideoProtect.exe` on Windows, `MegaVideoProtect` on Linux)
- `VideoVaultCoreTests`: smoke test for create/open/lock, wrong password, password validation, and verifier tamper detection
- `VideoVaultCoreGalleryTests`: import, gallery listing, package layout, decrypt round-trip (multi-chunk and empty), persistence across reopen, tamper (`PackageModified`), and missing-package (`PackageMissing`) tests
- `VideoVaultCoreAdminTests`: video removal (row, package file, survivor integrity, idempotency) and password change (old/new credential behavior, gallery survival, chained changes, wrong-current rejection, locked-vault failures)
- `VideoVaultCoreReaderTests`: streaming range reads (cross-chunk, boundaries, EOF), tamper (`PackageModified`) with untouched-chunk survival, missing package, unknown id, locked vault
- `VideoVaultCoreMediaTests`: in-test MJPEG fixture generation, `media_info` (resolution, codec, duration), thumbnail generation at native and scaled sizes, encrypted thumbnail storage/persistence/cascade, non-media rejection (`UnsupportedVideoFormat`), unknown id and locked-vault failures
- `VideoVaultPlaybackTests` (in `qt-app`): headless in-memory playback — duration, resolution, ≥10 decoded frames, valid first frame, seeking, and non-empty audio samples from an encrypted import
- `VideoVaultCoreTagTests`: tag creation/deduplication (case-insensitive), trimming, invalid-name rejection, shared tags across videos, per-video and global listings with counts, untagging, cascade on removal, persistence across reopen, locked-vault failures

## Limitations

- Playback is in-memory (never a whole-file plaintext temp file) but the player is a first cut: A/V sync is basic (frames paced against the audio clock; video-only files use the wall clock), there is no audio volume control, no subtitle support, and codecs outside the lean FFmpeg build (e.g., hardware-accelerated H.264/HEVC) fall back to software decode when an internal decoder exists. Qt's WMF multimedia backend was deliberately not used for the vault device.
- Audio requires the `swresample` FFmpeg feature (now in the manifest); re-run `scripts/regenerate-ffmpeg-importlibs.sh` after any ffmpeg reinstall (it now covers swresample too).
- `read_video_bytes` decrypts a whole package into memory; it is intended for verification/export of reasonably sized files. `read_video_range` and `PackageReader` are the bounded paths.
- Thumbnails are generated at import time (best effort for media content) and can be regenerated per video.
- The media test fixture uses MJPEG-in-Matroska; real-world container/codec variety (H.264/MP4, rotation metadata, audio-only streams) is exercised only as far as the FFmpeg build's internal codecs allow. The lean FFmpeg build has no external codec libraries (no H.264/HEVC encoders; decoders that ship inside FFmpeg remain available).
- FFmpeg quirks: the 7.x public headers carry no `extern "C"` guards (the sources wrap every FFmpeg include). On Windows only, the vcpkg port installs stub import libraries that must be regenerated from the DLL export tables (`scripts/regenerate-ffmpeg-importlibs.sh` must be re-run after any vcpkg reinstall of ffmpeg); Linux links the distro shared libraries directly and needs no such step.
- Import has no cancellation or progress callback yet; the UI shows a busy state.
- A crash in the middle of a password change can leave the vault in a state where neither the old nor the new password cleanly unlocks it (data is intact; recovery tooling and crash-injection tests are outstanding). The happy path is fully verified.
- Argon2id is invoked through the `argon2` port directly rather than libsodium's `crypto_pwhash` wrapper; both were considered, the direct port was pinned. See `docs/architecture.md`.
- Secure wiping of sensitive memory is best effort: OS paging and compiler/runtime copies can leave residues.
- Password rules are non-empty and ≤ 4096 bytes; no strength meter or minimum length is enforced.
- Auto-lock is in-app activity based (keyboard/mouse events routed to the app), not an OS-level session lock.
- SQLCipher's own KDF (PBKDF2) protects the database file; the Argon2id-derived subkey is the key material handed to SQLCipher. A future phase may move the file-level KDF to Argon2 if SQLCipher configuration permits.
- Only the Debug build has been tested; Release build and crash-injection tests are outstanding.
