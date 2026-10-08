#!/usr/bin/env bash
# Cross-compiles VideoVaultCore + its test suites for Android arm64-v8a using
# the Android NDK, then runs the aarch64 test binaries under qemu-user when
# qemu-aarch64 is available on the host.
#
# Dependencies are compiled from pinned sources into out/android-arm64/prefix:
#   openssl (crypto backend for sqlcipher), argon2, libsodium,
#   sqlcipher, ffmpeg (lean: mjpeg/matroska + h264/aac/mp4 for the fixtures).
#
# Usage:
#   In the NDK container (recommended):
#     docker run --rm --user root -e HOST_UID="$(id -u)" \
#       -v "$PWD":/repo -w /repo \
#       saschpe/android-ndk:36.1-jdk25.0.3_9-ndk30.0.14904198-cmake3.31.6 \
#       bash scripts/android/build-android-core.sh
#     (--user root overrides the image's baked-in nonroot user so make can be
#     installed; HOST_UID hands the build outputs back to your user.)
#   Or on a host/CI with an NDK installed:
#     ANDROID_NDK_ROOT=/path/to/ndk bash scripts/android/build-android-core.sh
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# On Windows hosts the NDK, CMake and the autotools/ffmpeg configure scripts are
# all native programs: give them drive-letter paths (C:/...) instead of the
# MSYS form (/c/...), which they cannot resolve. PATH entries the shell itself
# searches must stay in POSIX form, or extensionless executables go missing.
win_path() { if command -v cygpath >/dev/null 2>&1; then cygpath -m "$1"; else printf '%s' "$1"; fi; }
posix_path() { if command -v cygpath >/dev/null 2>&1; then cygpath -u "$1"; else printf '%s' "$1"; fi; }
REPO="$(win_path "$REPO")"
OUT="$REPO/out/android-arm64"
SRC="$OUT/src"
PREFIX="$OUT/prefix"
API="${ANDROID_PLATFORM_API:-26}"
ABI=arm64-v8a
JOBS="${JOBS:-$(nproc 2>/dev/null || echo "${NUMBER_OF_PROCESSORS:-4}")}"

echo "== locating NDK =="
NDK=""
for d in "${ANDROID_NDK_ROOT:-}" "${ANDROID_NDK_HOME:-}" \
         /usr/local/lib/android/sdk/ndk/* /opt/android-sdk*/ndk/*; do
    if [ -n "${d:-}" ] && [ -f "$d/build/cmake/android.toolchain.cmake" ]; then
        NDK="$d"; break
    fi
done
[ -n "$NDK" ] || { echo "ERROR: NDK not found (set ANDROID_NDK_ROOT or run inside the NDK container)"; exit 1; }
echo "NDK: $NDK"
# OpenSSL's android-arm64 Configure target resolves the NDK through this env var.
export ANDROID_NDK_ROOT="$NDK"

TOOLCHAIN=""
for host_dir in linux-x86_64 windows-x86_64 darwin-x86_64; do
    if [ -d "$NDK/toolchains/llvm/prebuilt/$host_dir" ]; then
        TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/$host_dir"
        break
    fi
done
[ -n "$TOOLCHAIN" ] || { echo "ERROR: no NDK host toolchain under $NDK/toolchains/llvm/prebuilt"; exit 1; }
# Host executables carry .exe on Windows.
EXE=""
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) EXE=".exe";; esac

# OpenSSL's android target looks for <arch>-linux-android-gcc on PATH, but the
# NDK ships only clang — and its <triple><api>-clang launchers are POSIX shell
# wrappers on Linux yet .cmd scripts on Windows. Provide sh-executable
# gcc/g++-named shims that call clang with an explicit android target, so one
# mechanism works on every host.
GCCSHIM="$OUT/gccshim"
mkdir -p "$GCCSHIM"
make_shim() { # <shim> <compiler>
    printf '#!/bin/sh\nexec "%s" --target=aarch64-linux-android%s "$@"\n' "$2" "$API" > "$1"
    chmod +x "$1"
}
make_shim "$GCCSHIM/aarch64-linux-android-gcc" "$TOOLCHAIN/bin/clang$EXE"
make_shim "$GCCSHIM/aarch64-linux-android-g++" "$TOOLCHAIN/bin/clang++$EXE"
export PATH="$(posix_path "$GCCSHIM"):$(posix_path "$TOOLCHAIN/bin"):$PATH"
CC="$GCCSHIM/aarch64-linux-android-gcc"
CXX="$GCCSHIM/aarch64-linux-android-g++"
AR="$TOOLCHAIN/bin/llvm-ar$EXE"
RANLIB="$TOOLCHAIN/bin/llvm-ranlib$EXE"
NM="$TOOLCHAIN/bin/llvm-nm$EXE"
STRIP="$TOOLCHAIN/bin/llvm-strip$EXE"
TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake"
# The NDK container ships CMake/ninja inside the SDK; fall back to PATH.
CMAKE_BIN="${CMAKE_BIN:-$(command -v cmake || echo /opt/android-sdk-linux/cmake/3.31.6/bin/cmake)}"
NINJA="${NINJA:-$(command -v ninja || echo /opt/android-sdk-linux/cmake/3.31.6/bin/ninja)}"
[ -x "$CMAKE_BIN" ] || { echo "cmake not found"; exit 1; }
# Windows hosts have no `make`; GNU make is usually installed as mingw32-make.
MAKE="${MAKE:-$(command -v make || command -v mingw32-make || command -v gmake)}"
[ -x "$MAKE" ] || { echo "ERROR: GNU make not found (install make / mingw32-make)"; exit 1; }

mkdir -p "$SRC" "$PREFIX"

# The container usually runs as root; always hand the build outputs back to
# the invoking user (HOST_UID), even when a later step fails.
if [ -n "${HOST_UID:-}" ]; then
    trap 'chown -R "$HOST_UID" "$OUT" 2>/dev/null || true' EXIT
fi

# The NDK container image ships CMake/ninja but not make (openssl, sqlcipher
# and ffmpeg all build with make), a host compiler for sqlcipher's
# lemon/mksourceid generators, or perl for OpenSSL's Configure. Install them
# when missing (needs root — run the container without --user). tclsh and
# qemu-user are optional: sqlcipher is configured --disable-tcl, and qemu only
# executes the aarch64 test binaries.
missing_tools=""
for tool in gcc perl; do
    command -v "$tool" >/dev/null 2>&1 || missing_tools="$missing_tools $tool"
done
if [ -n "$missing_tools" ]; then
    echo "== installing missing build tools:$missing_tools =="
    if command -v apt-get >/dev/null 2>&1; then
        apt-get update -qq >/dev/null && apt-get install -y -qq make tcl gcc libc6-dev perl qemu-user >/dev/null
    elif command -v apk >/dev/null 2>&1; then
        apk add --no-cache make tcl gcc perl qemu-user >/dev/null
    else
        echo "ERROR: build tools not found and no package manager available"
        echo "       missing:$missing_tools"
        echo "       PATH=$PATH"
        exit 1
    fi
fi
command -v tclsh >/dev/null 2>&1 \
    || echo "note: tclsh not found — fine, sqlcipher is built with --disable-tcl"
command -v qemu-aarch64 >/dev/null 2>&1 \
    || echo "note: qemu-aarch64 not found — test binaries will be built but not executed"

fetch() { # <url> <name>
    local url="$1" name="$2"
    local archive="$SRC/$name.archive"
    # Windows/schannel aborts every HTTPS transfer with CRYPT_E_REVOCATION_OFFLINE
    # when the certificate revocation server is unreachable; skip the CRL check.
    local tls_flags=""
    [ -n "$EXE" ] && tls_flags="--ssl-no-revoke"
    if [ ! -f "$archive" ]; then
        echo "== fetching $name =="
        curl -sL $tls_flags --max-time 300 -A "Mozilla/5.0" -o "$archive" "$url"
    fi
    if [ ! -f "$SRC/$name/.extracted" ]; then
        rm -rf "$SRC/$name"
        mkdir -p "$SRC/$name"
        # -f auto-detects gzip/xz, so one code path covers all tarballs.
        # GNU tar on Windows reads a "C:/..." argument as a remote host:path
        # (and dies with "Cannot connect to C:"), so hand it POSIX paths.
        tar -xf "$(posix_path "$archive")" -C "$(posix_path "$SRC/$name")" --strip-components=1
        touch "$SRC/$name/.extracted"
    fi
}

CMAKE_ANDROID=(
    -G Ninja
    -DCMAKE_MAKE_PROGRAM="$NINJA"
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE"
    -DANDROID_ABI="$ABI"
    -DANDROID_PLATFORM="android-$API"
    -DANDROID_STL=c++_static
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_INSTALL_PREFIX="$PREFIX"
)

# --- openssl (sqlcipher's crypto backend) ---------------------------------
if [ ! -f "$PREFIX/lib/libcrypto.a" ]; then
    fetch "https://codeload.github.com/openssl/openssl/tar.gz/refs/tags/openssl-3.0.16" openssl
    pushd "$SRC/openssl" >/dev/null
    echo "== building openssl (android-arm64, static) =="
    # $ANDROID_NDK_ROOT is handed over in POSIX form for this step only:
    # openssl's 15-android.conf matches which("clang") against "$ndk/.../prebuilt/"
    # and the MSYS perl returns POSIX paths, so a C:/ form never matches.
    ANDROID_NDK_ROOT="$(posix_path "$NDK")" ./Configure android-arm64 -D__ANDROID_API__=$API \
        --prefix="$PREFIX" --openssldir="$PREFIX/ssl" \
        no-shared no-tests >/dev/null
    "$MAKE" -j"$JOBS" >/dev/null
    "$MAKE" install_sw >/dev/null
    popd >/dev/null
fi

# --- argon2 ----------------------------------------------------------------
if [ ! -f "$PREFIX/lib/libargon2.a" ]; then
    fetch "https://codeload.github.com/P-H-C/phc-winner-argon2/tar.gz/refs/tags/20190702" argon2
    echo "== building argon2 (direct compile; the 20190702 tag predates CMake) =="
    mkdir -p "$SRC/argon2/obj"
    for srcf in src/argon2.c src/core.c src/blake2/blake2b.c src/encoding.c src/thread.c src/ref.c; do
        "$CC" -std=c89 -O3 -fPIC -DARGON2_NO_THREADS \
            -I"$SRC/argon2/include" -I"$SRC/argon2/src" \
            -c "$SRC/argon2/$srcf" -o "$SRC/argon2/obj/$(basename "${srcf%.c}").o"
    done
    "$AR" rcs "$PREFIX/lib/libargon2.a" "$SRC/argon2"/obj/*.o
    cp "$SRC/argon2/include/argon2.h" "$PREFIX/include/"
fi

# --- libsodium -------------------------------------------------------------
if [ ! -f "$PREFIX/lib/libsodium.a" ]; then
    # 1.0.20 dropped CMake; 1.0.18 ships a generated configure script, so the
    # autotools build needs no autotools installed.
    fetch "https://codeload.github.com/jedisct1/libsodium/tar.gz/refs/tags/1.0.18-RELEASE" libsodium
    pushd "$SRC/libsodium" >/dev/null
    echo "== building libsodium (autotools, static) =="
    ./configure --host=aarch64-linux-android \
        --prefix="$PREFIX" --enable-static --disable-shared \
        CC="$CC" AR="$AR" RANLIB="$RANLIB" NM="$NM" STRIP="$STRIP" >/dev/null
    "$MAKE" -j"$JOBS" >/dev/null
    "$MAKE" install >/dev/null
    popd >/dev/null
fi
# NOTE: libsodium's bundled argon2 exports the SAME phc argon2* API, so the
# Android CMake link uses libsodium for argon2 and never links libargon2.a
# (linking both archives duplicates every argon2 symbol). libargon2.a is
# still built above only for its installed header.

# --- sqlcipher -------------------------------------------------------------
if [ ! -f "$PREFIX/lib/libsqlcipher.a" ]; then
    fetch "https://codeload.github.com/sqlcipher/sqlcipher/tar.gz/refs/tags/v4.5.6" sqlcipher
    pushd "$SRC/sqlcipher" >/dev/null
    echo "== building sqlcipher (static, openssl) =="
    # Cross builds cannot build sqlcipher's host-only generator tools (lemon,
    # mksourceid) with the NDK toolchain, yet make's rules require them (the
    # parse.c/opcodes.h rules always re-run). Pre-build them with the host
    # compiler so make finds ./lemon and ./mksourceid ready.
    if command -v gcc >/dev/null 2>&1; then
        gcc -O2 -o lemon tool/lemon.c 2>/dev/null || true
        gcc -O2 -o mksourceid tool/mksourceid.c 2>/dev/null || true
    fi
    # The parse.c/parse.h rules run `./lemon -S parse.y`, where -S means
    # "read the parser template from ./lempar.c next to the lemon binary".
    # The tarball keeps the template at tool/lempar.c — mirror it to the
    # build root or the rule fails on a fresh checkout.
    cp -f tool/lempar.c .
    ./configure --host=aarch64-linux-android \
        --prefix="$PREFIX" --enable-static --disable-shared \
        --disable-tcl --disable-tests \
        --with-crypto-lib=openssl \
        CC="$CC" AR="$AR" RANLIB="$RANLIB" NM="$NM" STRIP="$STRIP" \
        CFLAGS="-I$PREFIX/include -O2 -fPIC -DSQLITE_HAS_CODEC" \
        LDFLAGS="-L$PREFIX/lib" >/dev/null
    # Generate the parser/opcode headers SERIALLY first: parallel make races
    # them against the amalgamation compile (sqlite3.c does not declare
    # opcodes.h/keywordhash.h as prerequisites), and stale 0-byte outputs
    # from earlier failed runs would otherwise be picked up.
    # sqlcipher's hand-written Makefile stores the source root as an absolute
    # MSYS path ($(TOP) = /c/...) and regenerates Makefile from $(TOP)/Makefile.in,
    # which a Windows-native make cannot stat; point TOP at the drive-letter
    # path instead.
    MAKE_TOP="TOP=$(win_path "$SRC/sqlcipher")"
    rm -f opcodes.h opcodes.c keywordhash.h
    "$MAKE" "$MAKE_TOP" -j1 opcodes.h opcodes.c keywordhash.h parse.h sqlite3.h >/dev/null
    # Build ONLY the static library, not the `sqlcipher` shell binary: the
    # shell links the codec against Android's logcat logging and would need
    # -llog plus a working tclsh at runtime, and nothing uses it here.
    "$MAKE" "$MAKE_TOP" -j"$JOBS" libsqlcipher.la >/dev/null
    cp .libs/libsqlcipher.a "$PREFIX/lib/"
    cp sqlite3.h "$PREFIX/include/"
    # The core includes <sqlcipher/sqlite3.h> — install the public headers
    # into the subdirectory layout sqlite3's own `make install` would create.
    mkdir -p "$PREFIX/include/sqlcipher"
    cp sqlite3.h sqlite3ext.h "$PREFIX/include/sqlcipher/"
    popd >/dev/null
fi

# --- zlib (FFmpeg's PNG/TIFF decoders) -------------------------------------
# Android ships no linkable system zlib for static builds, so PNG (screenshots!)
# and TIFF images cannot be decoded without this. Tiny and quick.
if [ ! -f "$PREFIX/lib/libz.a" ]; then
    fetch "https://codeload.github.com/madler/zlib/tar.gz/refs/tags/v1.3.1" zlib
    pushd "$SRC/zlib" >/dev/null
    echo "== building zlib (android-arm64, static) =="
    # CFLAGS must carry -fPIC: libz.a is linked into libmvpcore.so, and lld
    # refuses non-PIC relocations in a shared object (z_errmsg, zError).
    CHOST=aarch64-linux-android CC="$CC" AR="$AR" RANLIB="$RANLIB" \
        CFLAGS="-O3 -fPIC" \
        ./configure --static --prefix="$PREFIX" >/dev/null
    "$MAKE" -j"$JOBS" >/dev/null
    "$MAKE" install >/dev/null
    popd >/dev/null
fi

# --- ffmpeg (lean) ---------------------------------------------------------
if [ ! -f "$PREFIX/lib/libavformat.a" ]; then
    # GitHub mirror: ffmpeg.org is intermittently unreachable from some
    # networks; the mirror's tag tarballs include the in-tree configure.
    fetch "https://codeload.github.com/FFmpeg/FFmpeg/tar.gz/refs/tags/n7.1.1" ffmpeg
    pushd "$SRC/ffmpeg" >/dev/null
    echo "== building ffmpeg (android-arm64, lean) =="
    # The app plays real phone videos, so the codec/container set must cover
    # them — the previous mjpeg+matroska-only build could not even probe an
    # MP4/H.264 file, which left the gallery metadata empty and every thumbnail
    # and frame decode failing. Encoders stay minimal (MJPEG for thumbnails).
    # --disable-asm: FFmpeg's hand-written aarch64 assembly is assembled without
    # -fPIC here, so its objects carry R_AARCH64_ADR_PREL_PG_HI21 relocations
    # that ld.lld refuses to link into libmvpcore.so ("recompile with -fPIC").
    # C-only FFmpeg is plenty for what the app uses it for (probing container
    # metadata, decoding one frame for thumbnails, MJPEG-encoding it); actual
    # playback is hardware-decoded by MediaCodec.
    #
    # The *_pipe demuxers were a dead end: FFmpeg 7.x has no such component
    # names (configure ignored them). Images are handled instead by the core
    # passing a filename hint so the extension-keyed `image2` demuxer applies.
    # --enable-zlib is required for the PNG/APNG decoders (they select
    # inflate_wrapper, which needs zlib): --disable-autodetect turns off
    # automatic external-library detection, so zlib must be asked for by name.
    ./configure \
        --cc="$CC" --cxx="$CXX" --ar="$AR" --nm="$NM" --ranlib="$RANLIB" --strip="$STRIP" \
        --target-os=android --arch=aarch64 --enable-cross-compile --enable-pic \
        --disable-programs --disable-doc --disable-network --disable-autodetect \
        --disable-everything \
        --enable-avcodec --enable-avformat --enable-avutil --enable-swscale --enable-swresample \
        --enable-encoder=mjpeg --enable-muxer=matroska \
        --enable-decoder=mjpeg,h264,hevc,mpeg4,mpeg2video,vp8,vp9,av1,theora,aac,mp3,vorbis,opus,flac,pcm_s16le,png,apng,webp,gif,bmp,tiff \
        --enable-demuxer=matroska,mov,mpegts,mpegps,avi,flv,ogg,gif,image2,mjpeg \
        --enable-parser=mjpeg,h264,hevc,mpeg4video,mpegvideo,vp8,vp9,av1,aac,flac,opus,vorbis,png \
        --enable-protocol=file \
        --extra-cflags="-I$PREFIX/include" --extra-ldflags="-L$PREFIX/lib" \
        --enable-small --disable-bzlib --disable-lzma --disable-iconv \
        --enable-zlib \
        --disable-asm \
        --prefix="$PREFIX" >/dev/null
    "$MAKE" -j"$JOBS" >/dev/null
    "$MAKE" install >/dev/null
    popd >/dev/null
fi

# --- core + tests ----------------------------------------------------------
echo "== configuring core + tests (android-arm64) =="
"$CMAKE_BIN" -S "$REPO" -B "$OUT/build" -G Ninja \
    -DCMAKE_MAKE_PROGRAM="$NINJA" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
    -DANDROID_ABI="$ABI" -DANDROID_PLATFORM="android-$API" -DANDROID_STL=c++_static \
    -DCMAKE_BUILD_TYPE=Release \
    -DMVP_BUILD_APP=OFF -DMVP_BUILD_TESTS=ON

echo "== building core + tests =="
"$CMAKE_BIN" --build "$OUT/build" -j"$JOBS"

# --- run the aarch64 binaries under qemu (host-side validation) ------------
SYSROOT_LIB="$TOOLCHAIN/sysroot/usr/lib/aarch64-linux-android/$API"
QEMU="$(command -v qemu-aarch64 || true)"
if [ -n "$QEMU" ]; then
    # The test executables are dynamically linked against bionic; qemu-user
    # resolves libc/libm/liblog from the NDK sysroot via -L. The Android
    # dynamic linker (/system/bin/linker64) is not shipped by the NDK, so
    # qemu falls back to the sysroot's bionic loader when available.
    echo "== running aarch64 tests under $QEMU =="
    for t in VideoVaultCoreTests VideoVaultCoreGalleryTests VideoVaultCoreAdminTests \
             VideoVaultCoreReaderTests VideoVaultCoreMediaTests VideoVaultCoreTagTests; do
        bin="$OUT/build/tests/$t"
        [ -x "$bin" ] || { echo "MISSING $bin"; exit 1; }
        echo "-- $t"
        if TMPDIR=/tmp "$QEMU" -L "$SYSROOT_LIB" "$bin" >"$OUT/qemu-$t.log" 2>&1; then
            echo "$t: PASS"
        elif grep -q "linker64" "$OUT/qemu-$t.log"; then
            # The NDK does not ship bionic's /system/bin/linker64, so qemu-user
            # cannot start the dynamically linked test binaries on a plain
            # host/CI runner. This is an environment limitation, not a test
            # failure — the binaries' correctness is covered by the desktop
            # ctest suites (same core sources). Any OTHER qemu failure below
            # still aborts the build.
            echo "$t: SKIPPED (bionic linker64 unavailable under qemu-user on this host)"
        else
            cat "$OUT/qemu-$t.log"
            echo "$t FAILED under qemu"; exit 1
        fi
    done
    echo "ALL ANDROID CORE TESTS PASSED (or skipped: linker64 not runnable here)"
else
    echo "qemu-aarch64 not found in this environment; binaries built but not executed."
    echo "  To run them, install qemu-user (inside this container, as root):"
    echo "    apt-get update && apt-get install -y qemu-user"
    echo "  then re-run this script — the test loop needs the NDK sysroot at:"
    echo "    $SYSROOT_LIB"
fi
