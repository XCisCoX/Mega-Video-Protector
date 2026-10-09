# Dependency discovery differs by platform:
#   Windows: vcpkg manifest installs (vcpkg.json baseline) expose CMake
#            config targets and literal .lib paths under MVP_VCPKG_PREFIX.
#   Linux:   distro packages (qtbase5-dev, libavcodec-dev, libsqlcipher-dev,
#            libargon2-dev, libsodium-dev, ...) discovered via pkg-config.
#   Android: NDK cross-build; the dependencies are compiled by
#            scripts/android/build-android-core.sh into MVP_ANDROID_PREFIX
#            (static libs: argon2, libsodium, sqlcipher+openssl, ffmpeg).
if(WIN32)
    find_package(unofficial-argon2 CONFIG REQUIRED)
    find_package(sqlcipher CONFIG REQUIRED)

    add_library(MvpSodium SHARED IMPORTED GLOBAL)
    set_target_properties(MvpSodium PROPERTIES
        IMPORTED_CONFIGURATIONS "Debug;Release"
        IMPORTED_IMPLIB_DEBUG "${MVP_VCPKG_PREFIX}/debug/lib/libsodium.lib"
        IMPORTED_LOCATION_DEBUG "${MVP_VCPKG_PREFIX}/debug/bin/libsodium.dll"
        IMPORTED_IMPLIB_RELEASE "${MVP_VCPKG_PREFIX}/lib/libsodium.lib"
        IMPORTED_LOCATION_RELEASE "${MVP_VCPKG_PREFIX}/bin/libsodium.dll"
        MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release
        MAP_IMPORTED_CONFIG_MINSIZEREL Release
        INTERFACE_INCLUDE_DIRECTORIES "${MVP_VCPKG_PREFIX}/include"
    )

    set(MVP_SODIUM_LIBS MvpSodium)
    set(MVP_ARGON2_LIBS unofficial::argon2::libargon2)
    set(MVP_SQLCIPHER_LIBS sqlcipher::sqlcipher)

    # Imported shared libs (not bare .lib paths) so POST_BUILD copies the DLLs
    # next to the exe. The app links the release FFmpeg build in every
    # configuration: the vcpkg port's debug import libs are stubs, and the
    # regenerate-ffmpeg-importlibs script rebuilds the release ones.
    # zlib1.dll has to travel with avcodec — the PNG/APNG decoders are compiled
    # out unless FFmpeg is built with the zlib feature (see vcpkg.json).
    function(mvp_import_release_dll target dll implib)
        add_library(${target} SHARED IMPORTED GLOBAL)
        set(_implib "${MVP_VCPKG_PREFIX}/lib/${implib}.lib")
        set(_dll "${MVP_VCPKG_PREFIX}/bin/${dll}")
        set_target_properties(${target} PROPERTIES
            IMPORTED_CONFIGURATIONS "Debug;Release"
            IMPORTED_IMPLIB_DEBUG "${_implib}"
            IMPORTED_LOCATION_DEBUG "${_dll}"
            IMPORTED_IMPLIB_RELEASE "${_implib}"
            IMPORTED_LOCATION_RELEASE "${_dll}"
            MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release
            MAP_IMPORTED_CONFIG_MINSIZEREL Release
            INTERFACE_INCLUDE_DIRECTORIES "${MVP_VCPKG_PREFIX}/include"
        )
    endfunction()
    mvp_import_release_dll(MvpAvutil avutil-59.dll avutil)
    mvp_import_release_dll(MvpSwresample swresample-5.dll swresample)
    mvp_import_release_dll(MvpSwscale swscale-8.dll swscale)
    mvp_import_release_dll(MvpAvcodec avcodec-61.dll avcodec)
    mvp_import_release_dll(MvpAvformat avformat-61.dll avformat)
    mvp_import_release_dll(MvpZlib zlib1.dll zlib)
    set_property(TARGET MvpAvcodec APPEND PROPERTY
        INTERFACE_LINK_LIBRARIES MvpZlib)
    set(MVP_FFMPEG_LIBS
        MvpAvformat
        MvpAvcodec
        MvpAvutil
        MvpSwscale
        MvpSwresample
    )
elseif(ANDROID)
    # Dependency paths are defined at the top level (CMakeLists.txt) so that
    # tests/ and qt-app/ resolve the same NDK-built static libraries.
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SQLCIPHER REQUIRED IMPORTED_TARGET sqlcipher)
    pkg_check_modules(ARGON2 REQUIRED IMPORTED_TARGET libargon2)
    pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
        libavformat libavcodec libavutil libswscale libswresample)

    set(MVP_SODIUM_LIBS PkgConfig::SODIUM)
    set(MVP_ARGON2_LIBS PkgConfig::ARGON2)
    set(MVP_SQLCIPHER_LIBS PkgConfig::SQLCIPHER)
    set(MVP_FFMPEG_LIBS PkgConfig::FFMPEG)
endif()

add_library(VideoVaultCore STATIC
    src/crypto.cpp
    src/database.cpp
    src/media.cpp
    src/metadata.cpp
    src/package.cpp
    src/vault.cpp
    src/version.cpp
)

add_library(VideoVault::Core ALIAS VideoVaultCore)

target_include_directories(VideoVaultCore
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(ANDROID)
    target_include_directories(VideoVaultCore PRIVATE ${MVP_DEP_INCLUDE_DIRS})
endif()

target_link_libraries(VideoVaultCore
    PRIVATE
        ${MVP_SODIUM_LIBS}
        ${MVP_ARGON2_LIBS}
        ${MVP_SQLCIPHER_LIBS}
        ${MVP_FFMPEG_LIBS}
)
if(ANDROID)
    # sqlcipher's codec routes its diagnostics through Android's logcat API
    # (bionic liblog); the static library carries the reference.
    target_link_libraries(VideoVaultCore PRIVATE log)
endif()

target_compile_features(VideoVaultCore PUBLIC cxx_std_20)
target_compile_definitions(VideoVaultCore PRIVATE SQLITE_HAS_CODEC)
set_target_properties(VideoVaultCore PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    FOLDER "Core"
)
