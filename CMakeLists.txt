cmake_minimum_required(VERSION 3.20)

project(MegaVideoProtect
    VERSION 0.1.0
    DESCRIPTION "Secure desktop video vault"
    LANGUAGES CXX
)

option(MVP_BUILD_APP "Build the Qt desktop application" ON)
option(MVP_BUILD_TESTS "Build focused core tests" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_DEBUG_POSTFIX d)

if(MSVC)
    # Qt's official MSVC packages use the dynamic MSVC runtime.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    add_compile_options(/W4 /permissive- /Zc:__cplusplus /utf-8)
    add_link_options(/DYNAMICBASE /NXCOMPAT)
endif()

if(WIN32 AND NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Mega Video Protect supports only x64 Windows builds.")
endif()

# Dependency discovery is platform-specific:
#   Windows: the pinned vcpkg manifest flow (see vcpkg.json) provides
#            argon2/libsodium/sqlcipher/ffmpeg; the triplet install root is
#            required so the imported DLL targets and .lib paths resolve.
#   Linux:   distro packages discovered through pkg-config (see
#            core/CMakeLists.txt); no vcpkg install root is required.
#   Android: NDK cross-build; dependencies compiled by
#            scripts/android/build-android-core.sh into MVP_ANDROID_PREFIX.
# The *_LIBS / include variables are defined HERE (root scope) so every
# subdirectory — core, qt-app, tests — sees the same dependency set.
if(WIN32)
    if(NOT DEFINED VCPKG_INSTALLED_DIR OR NOT DEFINED VCPKG_TARGET_TRIPLET)
        message(FATAL_ERROR "Phase 2 requires the pinned vcpkg manifest and x64 triplet.")
    endif()
    set(MVP_VCPKG_PREFIX "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
elseif(ANDROID)
    set(MVP_ANDROID_PREFIX
        "${CMAKE_SOURCE_DIR}/out/android-arm64/prefix" CACHE PATH
        "Android dependency install prefix (built by scripts/android/build-android-core.sh)")
    set(MVP_SODIUM_LIBS "${MVP_ANDROID_PREFIX}/lib/libsodium.a")
    # libsodium bundles the same phc-winner-argon2 code, so both archives
    # define the argon2* API; the core additionally needs argon2_error_message
    # which only libargon2 exports. The definitions are identical, so both
    # archives are linked and --allow-multiple-definition (see below) makes
    # the linker keep the first, identical, definition.
    set(MVP_ARGON2_LIBS "${MVP_ANDROID_PREFIX}/lib/libargon2.a")
    set(MVP_SQLCIPHER_LIBS
        "${MVP_ANDROID_PREFIX}/lib/libsqlcipher.a"
        "${MVP_ANDROID_PREFIX}/lib/libcrypto.a"
    )
    set(MVP_FFMPEG_LIBS
        "${MVP_ANDROID_PREFIX}/lib/libavformat.a"
        "${MVP_ANDROID_PREFIX}/lib/libavcodec.a"
        "${MVP_ANDROID_PREFIX}/lib/libavutil.a"
        "${MVP_ANDROID_PREFIX}/lib/libswscale.a"
        "${MVP_ANDROID_PREFIX}/lib/libswresample.a"
        # FFmpeg's PNG/TIFF decoders need zlib; Android has no linkable system
        # zlib for static builds, so the script builds it into the prefix.
        "${MVP_ANDROID_PREFIX}/lib/libz.a"
    )
    set(MVP_DEP_INCLUDE_DIRS "${MVP_ANDROID_PREFIX}/include")
    # libsodium and libargon2 both define the phc argon2* API (identical
    # code); the first definition wins, which both sodium_init and the core's
    # argon2_error_message need satisfied.
    add_link_options(-Wl,--allow-multiple-definition)
else()
    set(MVP_VCPKG_PREFIX "" CACHE INTERNAL "vcpkg install root (Windows only)" FORCE)
endif()

# Copies an executable's runtime DLLs next to it. Only meaningful on Windows
# (vcpkg shared builds); on Linux the system loader resolves shared libraries,
# and an empty $<TARGET_RUNTIME_DLLS> expansion would otherwise break the
# POST_BUILD command.
function(mvp_copy_runtime_dlls target)
    if(WIN32)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:${target}>
                $<TARGET_FILE_DIR:${target}>
            COMMAND_EXPAND_LISTS
        )
    endif()
endfunction()

add_subdirectory(core)

if(MVP_BUILD_APP)
    add_subdirectory(qt-app)
endif()

if(MVP_BUILD_TESTS)
    include(CTest)
    enable_testing()
    add_subdirectory(tests)
endif()
