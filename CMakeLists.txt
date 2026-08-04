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

if(NOT DEFINED VCPKG_INSTALLED_DIR OR NOT DEFINED VCPKG_TARGET_TRIPLET)
    message(FATAL_ERROR "Phase 2 requires the pinned vcpkg manifest and x64 triplet.")
endif()
set(MVP_VCPKG_PREFIX "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")

add_subdirectory(core)

if(MVP_BUILD_APP)
    add_subdirectory(qt-app)
endif()

if(MVP_BUILD_TESTS)
    include(CTest)
    enable_testing()
    add_subdirectory(tests)
endif()
