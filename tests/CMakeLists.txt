add_executable(VideoVaultCoreTests
    core_smoke.cpp
)

target_link_libraries(VideoVaultCoreTests PRIVATE VideoVault::Core)
target_compile_features(VideoVaultCoreTests PRIVATE cxx_std_20)
set_target_properties(VideoVaultCoreTests PROPERTIES FOLDER "Tests")

add_custom_command(TARGET VideoVaultCoreTests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_RUNTIME_DLLS:VideoVaultCoreTests>
        $<TARGET_FILE_DIR:VideoVaultCoreTests>
    COMMAND_EXPAND_LISTS
)

add_test(NAME VideoVaultCore.Smoke COMMAND VideoVaultCoreTests)
