# JNI library for the native Android app: compiles the Qt-independent
# VideoVaultCore sources and links the NDK-built static dependencies from
# scripts/android/build-android-core.sh (MVP_ANDROID_PREFIX).
cmake_minimum_required(VERSION 3.22)
project(mvpcore LANGUAGES C CXX)

set(MVP_ANDROID_PREFIX "/repo/out/android-arm64/prefix" CACHE PATH
    "Android dependency prefix built by scripts/android/build-android-core.sh")

# Repo layout: android/app/src/main/cpp -> ../../../../../core
set(CORE_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../core")

add_library(mvpcore SHARED
    jni_bridge.cpp
    ${CORE_SRC}/src/crypto.cpp
    ${CORE_SRC}/src/database.cpp
    ${CORE_SRC}/src/media.cpp
    ${CORE_SRC}/src/metadata.cpp
    ${CORE_SRC}/src/package.cpp
    ${CORE_SRC}/src/vault.cpp
    ${CORE_SRC}/src/version.cpp
)

target_include_directories(mvpcore PRIVATE
    ${CORE_SRC}/include
    ${CORE_SRC}/src
    ${MVP_ANDROID_PREFIX}/include
)

target_compile_definitions(mvpcore PRIVATE SQLITE_HAS_CODEC)

target_link_libraries(mvpcore
    ${MVP_ANDROID_PREFIX}/lib/libsodium.a
    ${MVP_ANDROID_PREFIX}/lib/libargon2.a
    ${MVP_ANDROID_PREFIX}/lib/libsqlcipher.a
    ${MVP_ANDROID_PREFIX}/lib/libcrypto.a
    ${MVP_ANDROID_PREFIX}/lib/libavformat.a
    ${MVP_ANDROID_PREFIX}/lib/libavcodec.a
    ${MVP_ANDROID_PREFIX}/lib/libavutil.a
    ${MVP_ANDROID_PREFIX}/lib/libswscale.a
    ${MVP_ANDROID_PREFIX}/lib/libswresample.a
    log
    atomic
    m
)

# libsodium and libargon2 both carry the identical phc-winner-argon2 code.
target_link_options(mvpcore PRIVATE -Wl,--allow-multiple-definition)

set_target_properties(mvpcore PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)
