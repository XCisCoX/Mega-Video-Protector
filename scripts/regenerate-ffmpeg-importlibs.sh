#!/usr/bin/env bash
# Regenerates MSVC import libraries for the vcpkg-built FFmpeg shared libs.
#
# WHY: the vcpkg ffmpeg port (baseline b1b19307, FFmpeg 7.1.1) installs stub
# .lib files that contain no symbol entries, so MSVC link.exe cannot resolve
# any FFmpeg function (LNK2019 for every av* symbol). The DLLs themselves
# export the symbols correctly. This script rebuilds each import lib from the
# DLL export table (dumpbin /exports -> .def -> lib.exe /def).
#
# NOTE: any `vcpkg install`/reinstall of ffmpeg overwrites these libs, so run
# this script again after reinstalling ffmpeg.
#
# ALSO: FFmpeg 7.x public headers carry NO extern "C" guards. C++ translation
# units must wrap FFmpeg includes in `extern "C" { ... }` or every reference
# will be C++-mangled and cannot match the DLLs' undecorated exports.
set -u
# Local defaults; CI overrides via environment (see .github/workflows/release.yml).
# MSVC is located dynamically: VCToolsInstallDir (set by msvc-dev-cmd on CI)
# wins, otherwise vswhere finds the newest VS installation with VC tools and
# the toolset version directory is resolved by glob (it differs per machine).
if [ -n "${VCToolsInstallDir:-}" ]; then
    MSVC_ROOT="${VCToolsInstallDir//\\//}"
else
    VSWHERE="${MVP_VSWHERE:-/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe}"
    VSROOT=$("$VSWHERE" -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>/dev/null | tr -d '\r')
    VSROOT="${VSROOT//\\//}"
    MSVC_VERSION_DIR=$(ls -d "$VSROOT/VC/Tools/MSVC/"*/ 2>/dev/null | head -1)
    MSVC_ROOT="${MSVC_VERSION_DIR%/}"
    if [ -z "$MSVC_ROOT" ] || [ ! -f "$MSVC_ROOT/bin/Hostx64/x64/dumpbin.exe" ]; then
        # Last resort: the machine's known BuildTools install.
        TOOLS="${MVP_MSVC_TOOLS:-/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools}"
        MSVC_ROOT="$TOOLS/VC/Tools/MSVC/14.44.35207"
    fi
fi
DUMPBIN="$MSVC_ROOT/bin/Hostx64/x64/dumpbin.exe"
LIBEXE="$MSVC_ROOT/bin/Hostx64/x64/lib.exe"
INSTALL="${MVP_VCPKG_INSTALLED:-C:\\Users\\cisco\\AppData\\Local\\MegaVideoProtect\\vcpkg_installed\\x64-windows}"
DEFDIR="${MVP_FFMPEG_DEFS:-C:\\Users\\cisco\\AppData\\Local\\MegaVideoProtect\\ffmpeg-defs}"
mkdir -p "$DEFDIR"

regenerate() {
    local dll="$1"
    local libname="$2"
    local def="$DEFDIR\\${libname}.def"
    local dllpath="$INSTALL\\bin\\$dll"
    local libpath="$INSTALL\\lib\\$libname.lib"

    {
        echo "LIBRARY $dll"
        echo "EXPORTS"
        "$DUMPBIN" /exports "$dllpath" 2>/dev/null \
            | awk '/^[[:space:]]*[0-9]+[[:space:]]+[0-9A-Fa-f]+[[:space:]]+[0-9A-Fa-f]+[[:space:]]+[A-Za-z_]/{ name=$4; sub(/=.*/, "", name); print name }' \
            | sort -u
    } > "$def"

    local count
    count=$(grep -vc '^$' "$def")
    echo "$libname: $((count - 2)) exported symbols"

    "$LIBEXE" /nologo /machine:x64 "/def:$def" "/out:$libpath" \
        > "$DEFDIR\\${libname}.log" 2>&1
    local status=$?
    if [ $status -ne 0 ] || [ ! -f "$libpath" ]; then
        echo "$libname: FAILED (status $status)"
        tail -5 "$DEFDIR\\${libname}.log"
        exit 1
    fi
    echo "$libname: ok -> $libpath"
}

regenerate avcodec-61.dll avcodec
regenerate avformat-61.dll avformat
regenerate avutil-59.dll avutil
regenerate swscale-8.dll swscale
regenerate swresample-5.dll swresample
echo "DONE"
