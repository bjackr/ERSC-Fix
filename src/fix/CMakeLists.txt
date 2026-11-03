add_library(ersc_fix SHARED
    dllmain.cpp
    log.cpp
    config.cpp
    guard.cpp
    ${CMAKE_SOURCE_DIR}/src/common/sha256.cpp
    crash_log.cpp
    hooks/hooks.cpp
    hooks/exit_abort.cpp
    hooks/steam_log.cpp
    hooks/ride_probe.cpp
    hooks/block_reclaim.cpp
    hooks/inject.cpp
    hooks/away_relocate.cpp
    hooks/summon_replay.cpp
    hooks/enemy_probe.cpp
    hooks/flag_probe.cpp
    hooks/flag_trace.cpp
    hooks/bullet_guard.cpp
    hooks/main_thread.cpp
    hooks/mount_probe.cpp
    hooks/mount_replay.cpp
    hooks/spawn_repair.cpp
    game/event_flags.cpp
    game/flag_storage.cpp
    game/build.cpp
    game/signatures.cpp
)

target_include_directories(ersc_fix PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(ersc_fix PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
    ERSC_FIX_VERSION="${PROJECT_VERSION}")
target_compile_options(ersc_fix PRIVATE /W4 /permissive- /utf-8 /MP)
target_link_libraries(ersc_fix PRIVATE safetyhook bcrypt version)
