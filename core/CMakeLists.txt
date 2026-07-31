add_library(VideoVaultCore STATIC
    src/version.cpp
)

add_library(VideoVault::Core ALIAS VideoVaultCore)

target_include_directories(VideoVaultCore
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(VideoVaultCore PUBLIC cxx_std_20)
set_target_properties(VideoVaultCore PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    FOLDER "Core"
)
