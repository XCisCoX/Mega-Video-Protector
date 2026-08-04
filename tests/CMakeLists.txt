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

add_executable(VideoVaultCoreGalleryTests
    import_gallery.cpp
)

target_link_libraries(VideoVaultCoreGalleryTests PRIVATE VideoVault::Core)
target_compile_features(VideoVaultCoreGalleryTests PRIVATE cxx_std_20)
set_target_properties(VideoVaultCoreGalleryTests PROPERTIES FOLDER "Tests")

add_custom_command(TARGET VideoVaultCoreGalleryTests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_RUNTIME_DLLS:VideoVaultCoreGalleryTests>
        $<TARGET_FILE_DIR:VideoVaultCoreGalleryTests>
    COMMAND_EXPAND_LISTS
)

add_test(NAME VideoVaultCore.Gallery COMMAND VideoVaultCoreGalleryTests)

add_executable(VideoVaultCoreAdminTests
    vault_admin.cpp
)

target_link_libraries(VideoVaultCoreAdminTests PRIVATE VideoVault::Core)
target_compile_features(VideoVaultCoreAdminTests PRIVATE cxx_std_20)
set_target_properties(VideoVaultCoreAdminTests PROPERTIES FOLDER "Tests")

add_custom_command(TARGET VideoVaultCoreAdminTests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_RUNTIME_DLLS:VideoVaultCoreAdminTests>
        $<TARGET_FILE_DIR:VideoVaultCoreAdminTests>
    COMMAND_EXPAND_LISTS
)

add_test(NAME VideoVaultCore.Admin COMMAND VideoVaultCoreAdminTests)

add_executable(VideoVaultCoreReaderTests
    package_reader.cpp
)

target_link_libraries(VideoVaultCoreReaderTests PRIVATE VideoVault::Core)
target_compile_features(VideoVaultCoreReaderTests PRIVATE cxx_std_20)
set_target_properties(VideoVaultCoreReaderTests PROPERTIES FOLDER "Tests")

add_custom_command(TARGET VideoVaultCoreReaderTests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_RUNTIME_DLLS:VideoVaultCoreReaderTests>
        $<TARGET_FILE_DIR:VideoVaultCoreReaderTests>
    COMMAND_EXPAND_LISTS
)

add_test(NAME VideoVaultCore.Reader COMMAND VideoVaultCoreReaderTests)

add_executable(VideoVaultCoreMediaTests
    media.cpp
)

target_link_libraries(VideoVaultCoreMediaTests PRIVATE VideoVault::Core)
target_include_directories(VideoVaultCoreMediaTests PRIVATE "${MVP_VCPKG_PREFIX}/include")
target_link_libraries(VideoVaultCoreMediaTests PRIVATE
    "${MVP_VCPKG_PREFIX}/lib/avformat.lib"
    "${MVP_VCPKG_PREFIX}/lib/avcodec.lib"
    "${MVP_VCPKG_PREFIX}/lib/avutil.lib"
    "${MVP_VCPKG_PREFIX}/lib/swscale.lib"
)
target_compile_features(VideoVaultCoreMediaTests PRIVATE cxx_std_20)
set_target_properties(VideoVaultCoreMediaTests PROPERTIES FOLDER "Tests")

add_custom_command(TARGET VideoVaultCoreMediaTests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_RUNTIME_DLLS:VideoVaultCoreMediaTests>
        $<TARGET_FILE_DIR:VideoVaultCoreMediaTests>
    COMMAND_EXPAND_LISTS
)

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
