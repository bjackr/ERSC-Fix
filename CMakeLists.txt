cmake_minimum_required(VERSION 3.25)
# Keep in step with the git tag: the DLL logs this on startup, which is how we tell builds apart.
project(ersc_fix VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static CRT so the build runs on another PC without the VC++ redistributable.
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Embed only the PDB file name in the binaries, not the absolute path of the build machine.
if(MSVC)
    add_link_options("/PDBALTPATH:%_PDB%")
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

include(FetchContent)
set(SAFETYHOOK_FETCH_ZYDIS ON CACHE BOOL "" FORCE)
FetchContent_Declare(safetyhook
    GIT_REPOSITORY https://github.com/cursey/safetyhook.git
    GIT_TAG v0.6.10)
FetchContent_MakeAvailable(safetyhook)

add_subdirectory(src/fix)
add_subdirectory(src/launcher)

# Release zip: build/ersc-fix-<version>.zip holding SeamlessCoop/fix/{ersc_fix.dll, ersc_fix_launcher.exe},
# so it extracts straight into ELDEN RING\Game\ and can be uploaded to a GitHub release as is.
# Named after the project version (the export tool sets that to the release version).
set(ERSC_FIX_STAGE "${CMAKE_BINARY_DIR}/stage/SeamlessCoop/fix")
set(ERSC_FIX_ZIP "${CMAKE_BINARY_DIR}/ersc-fix-v${PROJECT_VERSION}.zip")
add_custom_target(package ALL
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${CMAKE_BINARY_DIR}/stage"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${ERSC_FIX_STAGE}"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:ersc_fix>" "$<TARGET_FILE:ersc_fix_launcher>" "${ERSC_FIX_STAGE}"
    COMMAND ${CMAKE_COMMAND} -E chdir "${CMAKE_BINARY_DIR}/stage" ${CMAKE_COMMAND} -E tar cf "${ERSC_FIX_ZIP}" --format=zip SeamlessCoop
    DEPENDS ersc_fix ersc_fix_launcher
    COMMENT "Packaging ${ERSC_FIX_ZIP}"
    VERBATIM)
