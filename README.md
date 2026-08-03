# Mega Video Protect

Production-oriented Windows video-vault project. Phase 1 (architecture and verified build foundation), Phase 2 (secure vault core + setup/login UI shell), Phase 3 (encrypted import + gallery), and Phase 4 (vault administration: removal and password change) are complete.

## Verified local toolchain

- Visual Studio 2022 Build Tools 17.14.21, MSVC toolset 14.44.35207
- CMake 3.31.6 and ctest (Visual Studio bundled)
- Windows SDK 10.0.22621.0 pinned by the preset for the current Qt kit
- Qt framework 5.12.12 at `C:/Qt/Qt5.12.12/5.12.12/msvc2017_64`
- Qt kit: desktop MSVC 2017 x64; used with the newer VS 2022 linker/toolset under Microsoft's supported v14x binary-compatibility model
- Qt Core, Concurrent, Widgets, Multimedia, and MultimediaWidgets are used. Qt SQL is intentionally not used: the core owns SQLCipher through its C API.
- vcpkg (bundled with VS 2022 Build Tools) provides `argon2`, `libsodium`, and `sqlcipher` as pinned x64 MSVC builds (see `vcpkg.json` baseline).

Qt Creator 5.0.2 is the IDE version, not the framework version. MinGW kits are intentionally not selected.

## Dependencies

Phase 2 pins the production cryptographic/database dependencies through the vcpkg manifest:

- `argon2` — Argon2id derivation (`argon2id_hash_raw`), pinned via manifest baseline
- `libsodium` — guarded memory, random generation, `crypto_kdf` subkey derivation, XChaCha20-Poly1305, constant-time compare
- `sqlcipher` — encrypted vault database, linked through its C API
- A vcpkg overlay port for `tcl` (`third_party/vcpkg-overlays/tcl`) carries the MSVC 14.44 build fixes SQLCipher's Windows build needs (implicit rules, shell install, generic Windows fixes)
- Strawberry Perl 5.42.0.1 portable x64 is required by the OpenSSL build that SQLCipher links; the file was verified by SHA-512 before use

vcpkg build trees and packages are redirected outside the project (`C:/Users/cisco/AppData/Local/MegaVideoProtect/...`) because the source path contains a space. `VCPKG_MANIFEST_INSTALL` is OFF in the preset; dependencies are installed once into `out/vcpkg_installed` (git-ignored).

## Build

From a Visual Studio developer shell, or with the bundled CMake executable:

```text
cmake --preset vs2022-x64
cmake --build --preset vs2022-x64-debug --target VideoVaultCoreTests VideoVaultApp
ctest --preset vs2022-x64-debug
```

The generated Visual Studio solution is under `out/build/vs2022-x64` and supports Debug and Release. Only Debug has been exercised; Release verification is scheduled for a later phase.

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

## Current targets

- `VideoVaultCore`: Qt-independent C++20 static library
- `VideoVaultApp`: Qt 5.12.12 Windows GUI linked to the core (output `MegaVideoProtect.exe`)
- `VideoVaultCoreTests`: smoke test for create/open/lock, wrong password, password validation, and verifier tamper detection
- `VideoVaultCoreGalleryTests`: import, gallery listing, package layout, decrypt round-trip (multi-chunk and empty), persistence across reopen, tamper (`PackageModified`), and missing-package (`PackageMissing`) tests
- `VideoVaultCoreAdminTests`: video removal (row, package file, survivor integrity, idempotency) and password change (old/new credential behavior, gallery survival, chained changes, wrong-current rejection, locked-vault failures)

## Limitations

- Thumbnails, media probing, and playback are not implemented (later phases; FFmpeg is pinned in Phase 5).
- `read_video_bytes` decrypts the whole package into memory; it is intended for verification/export of reasonably sized files. Playback will use a bounded streaming reader.
- Import has no cancellation or progress callback yet; the UI shows a busy state.
- A crash in the middle of a password change can leave the vault in a state where neither the old nor the new password cleanly unlocks it (data is intact; recovery tooling and crash-injection tests are outstanding). The happy path is fully verified.
- Argon2id is invoked through the `argon2` port directly rather than libsodium's `crypto_pwhash` wrapper; both were considered, the direct port was pinned. See `docs/architecture.md`.
- Secure wiping of sensitive memory is best effort: OS paging and compiler/runtime copies can leave residues.
- Password rules are non-empty and ≤ 4096 bytes; no strength meter or minimum length is enforced.
- Auto-lock is in-app activity based (keyboard/mouse events routed to the app), not an OS-level session lock.
- SQLCipher's own KDF (PBKDF2) protects the database file; the Argon2id-derived subkey is the key material handed to SQLCipher. A future phase may move the file-level KDF to Argon2 if SQLCipher configuration permits.
- Only the Debug build has been tested; Release build and crash-injection tests are outstanding.
