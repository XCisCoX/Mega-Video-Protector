find_package(Qt5 5.12.12 EXACT REQUIRED COMPONENTS
    Core
    Concurrent
    Widgets
    Multimedia
    MultimediaWidgets
)

add_executable(VideoVaultApp WIN32
    src/main.cpp
    src/main_window.cpp
    src/media_decoder.cpp
    src/player_window.cpp
    src/settings_dialog.cpp
    src/batch_worker.cpp
    include/videovault/app/main_window.hpp
    include/videovault/app/media_decoder.hpp
    include/videovault/app/player_window.hpp
    include/videovault/app/settings_dialog.hpp
    include/videovault/app/batch_worker.hpp
)

target_include_directories(VideoVaultApp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    "${MVP_VCPKG_PREFIX}/include"
)

target_link_libraries(VideoVaultApp
    PRIVATE
        VideoVault::Core
        Qt5::Core
        Qt5::Concurrent
        Qt5::Widgets
        Qt5::Multimedia
        Qt5::MultimediaWidgets
        "${MVP_VCPKG_PREFIX}/lib/avformat.lib"
        "${MVP_VCPKG_PREFIX}/lib/avcodec.lib"
        "${MVP_VCPKG_PREFIX}/lib/avutil.lib"
        "${MVP_VCPKG_PREFIX}/lib/swscale.lib"
        "${MVP_VCPKG_PREFIX}/lib/swresample.lib"
)

target_compile_features(VideoVaultApp PRIVATE cxx_std_20)
set_target_properties(VideoVaultApp PROPERTIES
    OUTPUT_NAME "MegaVideoProtect"
    FOLDER "Application"
    AUTOMOC ON
    VS_DEBUGGER_ENVIRONMENT "PATH=C:\\Qt\\Qt5.12.12\\5.12.12\\msvc2017_64\\bin;%PATH%"
)

add_custom_command(TARGET VideoVaultApp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_RUNTIME_DLLS:VideoVaultApp>
        $<TARGET_FILE_DIR:VideoVaultApp>
    COMMAND_EXPAND_LISTS
)

# Headless playback test: decodes frames from an imported video through the
# same MediaDecoder the player window uses, without any GUI.
if(MVP_BUILD_TESTS)
    add_executable(VideoVaultPlaybackTests
        tests/playback.cpp
        src/media_decoder.cpp
        src/player_window.cpp
        include/videovault/app/media_decoder.hpp
        include/videovault/app/player_window.hpp
    )
    set_target_properties(VideoVaultPlaybackTests PROPERTIES
        AUTOMOC ON
    )
    target_link_libraries(VideoVaultPlaybackTests
        PRIVATE
            VideoVault::Core
            Qt5::Core
            Qt5::Gui
            Qt5::Widgets
            Qt5::Multimedia
            "${MVP_VCPKG_PREFIX}/lib/avformat.lib"
            "${MVP_VCPKG_PREFIX}/lib/avcodec.lib"
            "${MVP_VCPKG_PREFIX}/lib/avutil.lib"
            "${MVP_VCPKG_PREFIX}/lib/swscale.lib"
            "${MVP_VCPKG_PREFIX}/lib/swresample.lib"
    )
    target_include_directories(VideoVaultPlaybackTests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        "${MVP_VCPKG_PREFIX}/include"
    )
    target_compile_features(VideoVaultPlaybackTests PRIVATE cxx_std_20)
    set_target_properties(VideoVaultPlaybackTests PROPERTIES FOLDER "Tests")

    add_custom_command(TARGET VideoVaultPlaybackTests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:VideoVaultPlaybackTests>
            $<TARGET_FILE_DIR:VideoVaultPlaybackTests>
        COMMAND_EXPAND_LISTS
    )
endif()
