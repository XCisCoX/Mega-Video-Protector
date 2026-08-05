add_executable(VideoVaultCoreTests
    core_smoke.cpp
)

target_link_libraries(VideoVaultCoreTests PRIVATE VideoVault::Core)
target_compile_features(VideoVaultCoreTests PRIVATE cxx_std_20)
set_target_properties(VideoVaultCoreTests PROPERTIES FOLDER "Tests")

mvp_copy_runtime_dlls(VideoVaultCoreTests)

add_test(NAME VideoVaultCore.Smoke COMMAND VideoVaultCoreTests)

add_executable(VideoVaultCoreGalleryTests
    import_gallery.cpp
)

target_link_libraries(VideoVaultCoreGalleryTests PRIVATE VideoVault::Core)
target_compile_features(VideoVaultCoreGalleryTests PRIVATE cxx_std_20)
set_target_properties(VideoVaultCoreGalleryTests PROPERTIES FOLDER "Tests")

mvp_copy_runtime_dlls(VideoVaultCoreGalleryTests)

add_test(NAME VideoVaultCore.Gallery COMMAND VideoVaultCoreGalleryTests)

add_executable(VideoVaultCoreAdminTests
    vault_admin.cpp
)

target_link_libraries(VideoVaultCoreAdminTests PRIVATE VideoVault::Core)
target_compile_features(VideoVaultCoreAdminTests PRIVATE cxx_std_20)
set_target_properties(VideoVaultCoreAdminTests PROPERTIES FOLDER "Tests")

mvp_copy_runtime_dlls(VideoVaultCoreAdminTests)

add_test(NAME VideoVaultCore.Admin COMMAND VideoVaultCoreAdminTests)

add_executable(VideoVaultCoreReaderTests
    package_reader.cpp
)

target_link_libraries(VideoVaultCoreReaderTests PRIVATE VideoVault::Core)
target_compile_features(VideoVaultCoreReaderTests PRIVATE cxx_std_20)
set_target_properties(VideoVaultCoreReaderTests PROPERTIES FOLDER "Tests")

mvp_copy_runtime_dlls(VideoVaultCoreReaderTests)

add_test(NAME VideoVaultCore.Reader COMMAND VideoVaultCoreReaderTests)

add_executable(VideoVaultCoreMediaTests
    media.cpp
)

target_link_libraries(VideoVaultCoreMediaTests PRIVATE VideoVault::Core)
target_link_libraries(VideoVaultCoreMediaTests PRIVATE ${MVP_FFMPEG_LIBS})
if(WIN32)
    target_include_directories(VideoVaultCoreMediaTests PRIVATE "${MVP_VCPKG_PREFIX}/include")
endif()
target_compile_features(VideoVaultCoreMediaTests PRIVATE cxx_std_20)
set_target_properties(VideoVaultCoreMediaTests PROPERTIES FOLDER "Tests")

mvp_copy_runtime_dlls(VideoVaultCoreMediaTests)

add_test(NAME VideoVaultCore.Media COMMAND VideoVaultCoreMediaTests)

# The playback test target is defined in qt-app/ (it needs Qt + FFmpeg), but
# ctest registration must happen after enable_testing() at the top level.
add_test(NAME VideoVault.Playback COMMAND $<TARGET_FILE:VideoVaultPlaybackTests>)

add_executable(VideoVaultCoreTagTests
    tags.cpp
)

target_link_libraries(VideoVaultCoreTagTests PRIVATE VideoVault::Core)
target_compile_features(VideoVaultCoreTagTests PRIVATE cxx_std_20)
set_target_properties(VideoVaultCoreTagTests PROPERTIES FOLDER "Tests")

add_test(NAME VideoVaultCore.Tags COMMAND VideoVaultCoreTagTests)
