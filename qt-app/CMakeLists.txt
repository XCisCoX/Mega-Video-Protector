find_package(Qt5 5.12.12 EXACT REQUIRED COMPONENTS
    Core
    Widgets
    Multimedia
    MultimediaWidgets
    Sql
)

add_executable(VideoVaultApp WIN32
    src/main.cpp
)

target_link_libraries(VideoVaultApp
    PRIVATE
        VideoVault::Core
        Qt5::Core
        Qt5::Widgets
        Qt5::Multimedia
        Qt5::MultimediaWidgets
        Qt5::Sql
)

target_compile_features(VideoVaultApp PRIVATE cxx_std_20)
set_target_properties(VideoVaultApp PROPERTIES
    OUTPUT_NAME "MegaVideoProtect"
    FOLDER "Application"
    VS_DEBUGGER_ENVIRONMENT "PATH=C:\\Qt\\Qt5.12.12\\5.12.12\\msvc2017_64\\bin;%PATH%"
)
