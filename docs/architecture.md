# Mega Video Protect architecture

## Phase boundary

Phase 1 establishes the build, module boundaries, dependency findings, and security design. It deliberately does not create a fake vault or substitute plain SQLite for SQLCipher. Crypto, database, and media code begin only after compatible x64 dependencies are pinned and linked.

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

1. Argon2id (libsodium `crypto_pwhash`) derives a 256-bit root master key from the password and a random salt. Memory, operations, and parallelism/security-profile metadata are versioned. The password is not retained.
2. Domain-separated subkeys are derived from the root for SQLCipher, file-key wrapping, and password verification. Reusing one raw key across protocols is forbidden.
3. A dedicated random verifier value is authenticated/encrypted under the verifier subkey. Unlock requires both SQLCipher access and successful verifier authentication.
4. Every video receives an independent random 256-bit file key. File keys are wrapped with XChaCha20-Poly1305 using unique random nonces and authenticated context. Only wrapped keys are persisted.
5. Thumbnails are encrypted before storage in the SQLCipher database.

Password changes derive a new root with a new salt, unwrap and re-wrap only file keys, rekey SQLCipher, and replace the verifier inside a recoverable transaction protocol. Large video packages are not re-encrypted.

## Versioned package format (design baseline)

The first format will use libsodium XChaCha20-Poly1305 AEAD in independently authenticated fixed-size chunks rather than `secretstream`. `secretstream` is excellent for sequential data but its chained state prevents efficient random seeking; independently authenticated chunks satisfy secure seekable playback.

The authenticated fixed header contains magic, format version, algorithm identifier, header size, chunk size, plaintext size, chunk count, a random package identifier, and a random nonce prefix. Each 24-byte nonce is constructed from a per-package random 16-byte prefix plus the 64-bit chunk index. Since every package also has a unique random file key, `(key, nonce)` reuse is prevented. Chunk associated data binds the canonical header digest, package identifier, chunk index, plaintext length, and final-chunk flag. Each chunk has its own Poly1305 tag, which is verified before plaintext is returned.

Imports write to a vault-local temporary file, flush and close it, hash the complete ciphertext package with SHA-256, verify it, atomically rename it to `<h0h1>/<h2h3>/<64-lowercase-hex>.vvp`, then commit the database row. Cancellation or crashes cannot leave a committed row pointing at a partial package. Package hashes aid lookup and scans; AEAD tags provide modification security.

## Database

The core will link directly to SQLCipher's C API and use RAII connection/statement/transaction wrappers plus prepared statements. Qt's installed `QSQLITE` plugin is plain SQLite and is not an acceptable vault database. Schema migrations run under transactions and track a monotonic schema version. Planned tables are `vault_metadata`, `videos`, `video_keys`, `thumbnails`, `import_jobs`, `package_integrity`, and `settings`.

## Media and playback

FFmpeg libraries will inspect containers, read metadata, decode a representative frame, handle rotation, and encode WebP/JPEG through in-process APIs. Extensions are only discovery hints.

Playback uses `EncryptedVideoReader` -> bounded authenticated chunk cache -> `QtVaultIODevice`. Qt 5.12's Windows multimedia backend must be probed with a custom seekable `QIODevice` before it is declared supported. If it cannot reliably consume the device, the safe fallback is an in-process FFmpeg custom-AVIO decode path. Full plaintext files will not be written to normal temporary storage.

## Dependency policy

Use one pinned x64 dependency set compiled with the MSVC ABI. Prefer libsodium for Argon2id, random generation, XChaCha20-Poly1305, hashing, secure memory, and key derivation. SQLCipher brings its required crypto backend; this is a database implementation dependency, not a second application-level crypto design. FFmpeg is media-only. Dependencies will be checked for architecture and build configuration before Phase 2.
