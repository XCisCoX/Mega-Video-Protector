# Windows CI/local uses Qt 5.12.12 (msvc2017_64); Linux distros ship newer
# Qt 5.x (Ubuntu 24.04: 5.15.13). The code targets stable 5.12-era APIs, so a
# minimum-version find is used instead of the former EXACT 5.12.12 pin.
find_package(Qt5 5.12 REQUIRED COMPONENTS
    Core
    Concurrent
    Widgets
    Network
    Multimedia
    MultimediaWidgets
)

# The Windows resource compiler language is only needed for app.rc
# (the exe icon); harmless to enable on MSVC, required for MinGW.
if(WIN32)
    enable_language(RC)
endif()

set(MVP_APP_SOURCES
    src/main.cpp
    src/main_window.cpp
    src/media_decoder.cpp
    src/player_window.cpp
    src/settings_dialog.cpp
    src/batch_worker.cpp
    src/share_server.cpp
    third_party/qrcodegen/qrcodegen.cpp
    include/videovault/app/main_window.hpp
    include/videovault/app/media_decoder.hpp
    include/videovault/app/player_window.hpp
    include/videovault/app/settings_dialog.hpp
    include/videovault/app/batch_worker.hpp
    include/videovault/app/share_server.hpp
    resources/resources.qrc
)

if(ANDROID)
    # Android apps are shared libraries: androiddeployqt packages the .so into
    # the APK, where the Qt Activity (android.app.lib_name = MegaVideoProtect)
    # loads it. There is no WIN32 subsystem or .rc resource on Android.
    add_library(VideoVaultApp SHARED ${MVP_APP_SOURCES})
    set_target_properties(VideoVaultApp PROPERTIES
        QT_ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/android")
else()
    add_executable(VideoVaultApp WIN32 ${MVP_APP_SOURCES})
endif()

if(WIN32)
    # Embeds the application icon into MegaVideoProtect.exe (Explorer,
    # taskbar, and the Alt-Tab icon all come from this resource).
    target_sources(VideoVaultApp PRIVATE resources/app.rc)
endif()

target_include_directories(VideoVaultApp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/qrcodegen
)
if(WIN32)
    # vcpkg FFmpeg headers (Linux pkg-config targets carry their own includes).
    target_include_directories(VideoVaultApp PRIVATE "${MVP_VCPKG_PREFIX}/include")
elseif(ANDROID)
    # NDK-built FFmpeg/sodium/sqlcipher headers live in the dep prefix.
    target_include_directories(VideoVaultApp PRIVATE ${MVP_DEP_INCLUDE_DIRS})
endif()

target_link_libraries(VideoVaultApp
    PRIVATE
        VideoVault::Core
        Qt5::Core
        Qt5::Concurrent
        Qt5::Widgets
        Qt5::Network
        Qt5::Multimedia
        Qt5::MultimediaWidgets
        ${MVP_FFMPEG_LIBS}
)

target_compile_features(VideoVaultApp PRIVATE cxx_std_20)
set_target_properties(VideoVaultApp PROPERTIES
    OUTPUT_NAME "MegaVideoProtect"
    FOLDER "Application"
    AUTOMOC ON
    AUTORCC ON
)
if(MSVC)
    set_target_properties(VideoVaultApp PROPERTIES
        VS_DEBUGGER_ENVIRONMENT "PATH=C:\\Qt\\Qt5.12.12\\5.12.12\\msvc2017_64\\bin;%PATH%")
endif()

mvp_copy_runtime_dlls(VideoVaultApp)

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
            ${MVP_FFMPEG_LIBS}
    )
    target_include_directories(VideoVaultPlaybackTests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    if(WIN32)
        target_include_directories(VideoVaultPlaybackTests PRIVATE "${MVP_VCPKG_PREFIX}/include")
    endif()
    target_compile_features(VideoVaultPlaybackTests PRIVATE cxx_std_20)
    set_target_properties(VideoVaultPlaybackTests PROPERTIES FOLDER "Tests")

    mvp_copy_runtime_dlls(VideoVaultPlaybackTests)
endif()
