add_executable(VideoVaultCoreTests
    core_smoke.cpp
)

target_link_libraries(VideoVaultCoreTests PRIVATE VideoVault::Core)
target_compile_features(VideoVaultCoreTests PRIVATE cxx_std_20)
set_target_properties(VideoVaultCoreTests PROPERTIES FOLDER "Tests")

add_test(NAME VideoVaultCore.Smoke COMMAND VideoVaultCoreTests)
