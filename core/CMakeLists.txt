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

target_link_libraries(VideoVaultCore
    PRIVATE
        MvpSodium
        unofficial::argon2::libargon2
        sqlcipher::sqlcipher
        "${MVP_VCPKG_PREFIX}/lib/avformat.lib"
        "${MVP_VCPKG_PREFIX}/lib/avcodec.lib"
        "${MVP_VCPKG_PREFIX}/lib/avutil.lib"
        "${MVP_VCPKG_PREFIX}/lib/swscale.lib"
)

target_compile_features(VideoVaultCore PUBLIC cxx_std_20)
target_compile_definitions(VideoVaultCore PRIVATE SQLITE_HAS_CODEC)
set_target_properties(VideoVaultCore PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    FOLDER "Core"
)
