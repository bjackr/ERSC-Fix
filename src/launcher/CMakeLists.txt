add_executable(ersc_fix_launcher
    main.cpp
    ${CMAKE_SOURCE_DIR}/src/common/sha256.cpp
)

target_compile_definitions(ersc_fix_launcher PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
    ERSC_FIX_VERSION="${PROJECT_VERSION}")
target_compile_options(ersc_fix_launcher PRIVATE /W4 /permissive- /utf-8 /MP)
target_include_directories(ersc_fix_launcher PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(ersc_fix_launcher PRIVATE shell32 version bcrypt)
