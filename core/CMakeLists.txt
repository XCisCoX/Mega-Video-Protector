find_package(unofficial-argon2 CONFIG REQUIRED)
find_package(sqlcipher CONFIG REQUIRED)

if(NOT DEFINED VCPKG_INSTALLED_DIR OR NOT DEFINED VCPKG_TARGET_TRIPLET)
    message(FATAL_ERROR "Phase 2 requires the pinned vcpkg manifest and x64 triplet.")
endif()

set(MVP_VCPKG_PREFIX "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
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
)

target_compile_features(VideoVaultCore PUBLIC cxx_std_20)
target_compile_definitions(VideoVaultCore PRIVATE SQLITE_HAS_CODEC)
set_target_properties(VideoVaultCore PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    FOLDER "Core"
)
