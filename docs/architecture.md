# Mega Video Protect architecture

## Phase boundary

Phase 1 established the build, module boundaries, dependency findings, and security design. Phase 2 pinned the production dependencies and implemented the secure vault core and the setup/login UI shell. Phase 3 implemented encrypted import, the chunked package format, and the gallery. Phase 4 implemented vault administration: video removal and password change. The project deliberately never substituted plain SQLite for SQLCipher or a fake vault for the real one.

## Phase 2 implementation status (verified by `VideoVaultCore.Smoke`)

- **Key derivation.** Argon2id runs through the pinned `argon2` port (`argon2id_hash_raw`) with safety bounds (memory 8 MiB–1 GiB, iterations 1–20, parallelism 1–16, password ≤ 4096 bytes). This is a deviation from the Phase 1 design note that suggested libsodium's `crypto_pwhash` wrapper; both are Argon2id and the direct port was chosen during pinning.
- **Subkeys.** Domain-separated 32-byte subkeys via libsodium `crypto_kdf`: database subkey id 1 (context `MVPDB001`), password-verifier subkey id 2 (context `MVPVEr01`).
- **Password verifier.** A random 16-byte value is sealed with XChaCha20-Poly1305 under the verifier subkey (random nonce). The 24-byte nonce and 48-byte ciphertext live in `vault.meta`. Open requires the verifier to authenticate; if the verifier rejects but the SQLCipher key still opens the database, the error is `AuthenticationFailed` (metadata damage) rather than `WrongPassword`.
- **Vault layout.** `vault.meta` (64-byte header + nonce + ciphertext), `vault.db` (SQLCipher), `vault.ready` (written last), `vault-data/`, and `.tmp/` staging. `Vault::exists` requires metadata, database, and ready marker.
- **Crash-consistent creation.** Build `vault.db` in `.tmp`, close and flush, atomically rename into place, atomically write `vault.meta`, then write `vault.ready`. Any failure removes all creation artifacts, so a partial vault cannot masquerade as complete.
- **SQLCipher connection.** Direct C API with RAII statements and transactions. Per-connection hardening: 4096-byte pages, HMAC-SHA512, PBKDF2-HMAC-SHA512 at 256000 iterations, `cipher_memory_security ON`, `secure_delete ON`, `foreign_keys ON`, 5 s busy timeout, extended result codes. The `SQLITE_HAS_CODEC` definition is applied to the core target so the header exposes `sqlite3_key`.
- **Schema.** `schema_migrations(version, applied_at)` and `vault_metadata(singleton, format_version, external_metadata, verifier_nonce, verifier_ciphertext, created_at)` created in a `BEGIN IMMEDIATE` transaction; `verify_vault_metadata` cross-checks the database row against the external metadata file after open.
- **Sensitive memory.** `SensitiveBuffer` uses `sodium_malloc`/`sodium_mlock`, wipes with `sodium_memzero`, and is non-copyable. Wiping remains best effort.
- **UI shell.** Setup, login, and unlocked pages consume only public core APIs. Core operations run via `QtConcurrent`; the UI thread does no crypto or database work. A 5-minute activity-based auto-lock and `QSettings`-remembered vault location are included. Qt SQL is not linked.
- **Known limitations.** No media import/gallery/playback yet; password policy is non-empty ≤ 4096 bytes; auto-lock is in-app activity based; Release build and crash-injection tests outstanding.

## Phase 3 implementation status (verified by `VideoVaultCore.Gallery`)

- **Package format implemented as designed.** `vault-data/<h0h1>/<h2h3>/<sha256-of-ciphertext>.vvp` holds an authenticated 68-byte canonical header (`MVPVVP01` magic, format version 1, algorithm 1 = XChaCha20-Poly1305 chunked, chunk size, plaintext size, chunk count, 16-byte package id, 16-byte nonce prefix), a 16-byte header AEAD tag, then one independently authenticated ciphertext per chunk. Chunk nonce = nonce prefix || 64-bit chunk index; the header uses the reserved all-ones index. Chunk AD binds header SHA-256 digest, package id, chunk index, plaintext length, and the final-chunk flag. Chunk size is validated to 1..16 MiB on read; the importer uses 64 KiB.
- **Import flow.** `Vault::import_file` streams the source into `vault-data/.tmp/import_*.vvp`, hashes the complete ciphertext incrementally (SHA-256), flushes with write-through, atomically renames into the hash-derived path, then commits the `videos` row and the wrapped file key in a single `BEGIN IMMEDIATE` transaction. Any failure removes the staged or renamed package, so a committed row cannot point at a partial package. Each import draws a fresh random 256-bit file key and package id: identical source content imports as a distinct package (the design hashes ciphertext; content deduplication is intentionally absent).
- **Key wrapping.** File keys are sealed with XChaCha20-Poly1305 under the wrapping subkey (id 3, context `MVPRAP01`), with the package id as associated data, into a random nonce. Only the 48-byte wrapped key and nonce are stored in `video_keys`; unwrapping happens per read under the wrapping subkey, and the transient plaintext key copy is wiped best-effort.
- **Schema migration v2.** `videos` (id, display_name, original_size, package_relative_path UNIQUE, package_size, package_sha256, format_version, algorithm_id, chunk_size, package_id, imported_at) and `video_keys` (video_id PK → videos ON DELETE CASCADE, wrapped_key, wrap_nonce). Migration runs in its own transaction on both create and open of existing Phase 2 vaults, tracked by `schema_migrations`.
- **Read and integrity.** `Vault::read_video_bytes` verifies the file size against the header, the header tag, and every chunk tag before returning plaintext (`PackageModified` on any mismatch, `PackageMissing` if the file is gone, `UnsupportedPackageVersion` for a newer format). `Vault::package_path` resolves and existence-checks a package. `Vault::list_videos` returns the gallery ordered by import time.
- **Threading.** A per-vault `std::mutex` held by `unique_ptr` (stable address across `Vault` moves) serializes `lock()`, `is_unlocked()`, `kdf_parameters()`, and all gallery/import operations. Worker threads hold a `shared_ptr` to the vault; the owner may `lock()` concurrently and the workers observe either the completed operation or a locked error. The Qt shell runs import and listing on `QtConcurrent` threads and performs no crypto or database work on the UI thread.
- **Known limitations.** No thumbnails/probing/playback (FFmpeg in Phase 5); `read_video_bytes` is whole-file-in-memory (verification/export only; playback gets a bounded streaming reader); import has no cancellation/progress callback; Release build and crash-injection tests outstanding.

## Phase 4 implementation status (verified by `VideoVaultCore.Admin`)

- **Video removal.** `Vault::remove_video` queries the row, deletes it in a transaction (the wrapped key cascades through `video_keys.video_id → videos.id ON DELETE CASCADE`), then removes the package file. The row is committed before the file is deleted, so a committed row never points at a removed package; a failed file removal leaves a replaceable orphan. Unknown ids return `InvalidArgument`; locked vaults return `InvalidArgument`.
- **Password change.** `Vault::change_password` first proves knowledge of the current password: it re-derives the master key from the on-disk metadata salt/parameters and compares it in constant time with the live master key, returning `WrongPassword` without touching any state. On success it: derives a fresh hierarchy (new random salt, new Argon2id parameters, new database/verifier/wrapping subkeys), re-wraps every file key under the new wrapping subkey (same package-id AD), builds new metadata that keeps the vault id (ready marker stays valid) with a freshly sealed verifier, writes the new `vault.meta` atomically, updates the `vault_metadata` record and all `video_keys` rows, re-encrypts the database with `sqlite3_rekey`, and swaps the session's master key and parameters. Large packages are never re-encrypted; only wrapped keys change.
- **Ordering and crash window.** The external metadata is published before the database record/keys are updated, and the SQLCipher rekey is last. A crash between these steps can leave the vault in a state where neither password cleanly unlocks it (all data remains intact). Recovery tooling and crash-injection tests are outstanding; the happy path, wrong-current rejection, and chained changes are verified.
- **UI shell.** The unlocked page adds "Remove selected" (uses the gallery selection's video id) and "Change password…" (modal dialog with current/new/confirm and the Argon2id profile). Both run on `QtConcurrent` workers against the shared vault, like import and listing.

## Components

- `VideoVaultCore`: C++20 static library with no Qt dependency. It will own vault lifecycle, key management, SQLCipher access, package I/O, import jobs, integrity checks, and decrypted random-access readers.
- `VideoVaultApp`: Qt 5.12.12 desktop UI. It depends on the core through standard C++ interfaces. A narrow adapter will translate core events/results to Qt signals, models, and `QIODevice`.
- `VideoVaultCoreTests`: focused native tests. Crypto and crash-consistency cases will be added with each implementation phase rather than as placeholders.

Public core headers will expose value types, opaque handles, RAII objects, cancellation tokens, callbacks, and typed `Result<T, Error>`-style outcomes. No libsodium, SQLCipher, FFmpeg, SQLite, or Qt type will cross the public core ABI.

## Threading and ownership

The UI thread performs no encryption, hashing, probing, or database I/O. Core jobs run on owned worker threads with cooperative cancellation and progress callbacks. SQLCipher connections are not shared concurrently; a bounded connection strategy gives each worker exclusive connection ownership. Transactions establish the database/package consistency boundary.

Sensitive buffers are non-copyable RAII objects backed by libsodium guarded/locked memory where available and wiped on release. OS paging and compiler/runtime copies mean wiping remains best effort and will be documented as such.

## Cryptographic design

No custom primitive is introduced.

1. Argon2id (pinned `argon2` port, `argon2id_hash_raw`) derives a 256-bit root master key from the password and a random 16-byte salt. Memory, operations, and parallelism/security-profile metadata are versioned. The password is not retained.
2. Domain-separated subkeys are derived from the root for SQLCipher, file-key wrapping, and password verification. Reusing one raw key across protocols is forbidden.
3. A dedicated random verifier value is authenticated/encrypted under the verifier subkey. Unlock requires both SQLCipher access and successful verifier authentication.
4. Every video receives an independent random 256-bit file key. File keys are wrapped with XChaCha20-Poly1305 using unique random nonces and authenticated context. Only wrapped keys are persisted.
5. Thumbnails are encrypted before storage in the SQLCipher database.

Password changes derive a new root with a new salt, unwrap and re-wrap only file keys, rekey SQLCipher, and replace the verifier. Large video packages are not re-encrypted. Implemented in Phase 4 (see the Phase 4 status section; the "recoverable transaction protocol" is currently best-effort with a documented crash window).

## Versioned package format (design baseline)

Implemented in Phase 3 (`internal/package.hpp`, `core/src/package.cpp`); see the Phase 3 status section for the exact on-disk layout and verified behavior.

The first format uses libsodium XChaCha20-Poly1305 AEAD in independently authenticated fixed-size chunks rather than `secretstream`. `secretstream` is excellent for sequential data but its chained state prevents efficient random seeking; independently authenticated chunks satisfy secure seekable playback.

The authenticated fixed header contains magic, format version, algorithm identifier, header size, chunk size, plaintext size, chunk count, a random package identifier, and a random nonce prefix. Each 24-byte nonce is constructed from a per-package random 16-byte prefix plus the 64-bit chunk index. Since every package also has a unique random file key, `(key, nonce)` reuse is prevented. Chunk associated data binds the canonical header digest, package identifier, chunk index, plaintext length, and final-chunk flag. Each chunk has its own Poly1305 tag, which is verified before plaintext is returned.

Imports write to a vault-local temporary file, flush and close it, hash the complete ciphertext package with SHA-256, verify it, atomically rename it to `<h0h1>/<h2h3>/<64-lowercase-hex>.vvp`, then commit the database row. Cancellation or crashes cannot leave a committed row pointing at a partial package. Package hashes aid lookup and scans; AEAD tags provide modification security.

## Database

The core links directly to SQLCipher's C API and uses RAII connection/statement/transaction wrappers plus prepared statements. Qt's installed `QSQLITE` plugin is plain SQLite and is not an acceptable vault database. Schema migrations run under transactions and track a monotonic schema version. Phase 2 created `schema_migrations` and `vault_metadata`; Phase 3 added `videos` and `video_keys`. The `thumbnails`, `import_jobs`, `package_integrity`, and `settings` tables are planned with the import/gallery phases.

## Media and playback

FFmpeg libraries will inspect containers, read metadata, decode a representative frame, handle rotation, and encode WebP/JPEG through in-process APIs. Extensions are only discovery hints.

Playback uses `EncryptedVideoReader` -> bounded authenticated chunk cache -> `QtVaultIODevice`. Qt 5.12's Windows multimedia backend must be probed with a custom seekable `QIODevice` before it is declared supported. If it cannot reliably consume the device, the safe fallback is an in-process FFmpeg custom-AVIO decode path. Full plaintext files will not be written to normal temporary storage.

## Dependency policy

Use one pinned x64 dependency set compiled with the MSVC ABI. Prefer libsodium for Argon2id, random generation, XChaCha20-Poly1305, hashing, secure memory, and key derivation. SQLCipher brings its required crypto backend; this is a database implementation dependency, not a second application-level crypto design. FFmpeg is media-only. Dependencies will be checked for architecture and build configuration before Phase 2.
