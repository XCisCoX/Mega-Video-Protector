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
    include/videovault/app/main_window.hpp
)

target_include_directories(VideoVaultApp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(VideoVaultApp
    PRIVATE
        VideoVault::Core
        Qt5::Core
        Qt5::Concurrent
        Qt5::Widgets
        Qt5::Multimedia
        Qt5::MultimediaWidgets
)

target_compile_features(VideoVaultApp PRIVATE cxx_std_20)
set_target_properties(VideoVaultApp PROPERTIES
    OUTPUT_NAME "MegaVideoProtect"
    FOLDER "Application"
    VS_DEBUGGER_ENVIRONMENT "PATH=C:\\Qt\\Qt5.12.12\\5.12.12\\msvc2017_64\\bin;%PATH%"
)

add_custom_command(TARGET VideoVaultApp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_RUNTIME_DLLS:VideoVaultApp>
        $<TARGET_FILE_DIR:VideoVaultApp>
    COMMAND_EXPAND_LISTS
)
