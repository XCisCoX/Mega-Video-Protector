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
if(WIN32)
    if(NOT DEFINED VCPKG_INSTALLED_DIR OR NOT DEFINED VCPKG_TARGET_TRIPLET)
        message(FATAL_ERROR "Phase 2 requires the pinned vcpkg manifest and x64 triplet.")
    endif()
    set(MVP_VCPKG_PREFIX "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
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
