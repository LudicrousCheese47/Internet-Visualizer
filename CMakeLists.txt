# 3.22 is raylib's own minimum; the upper bound records the newest policies
# this project has been validated against. Do not raise the floor casually -
# standard toolchains and CI runners are typically on CMake 3.x.
cmake_minimum_required(VERSION 3.22...4.2)
project(Internet_Visualizer)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)

include(FetchContent)

set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

# raylib is pinned to a specific commit so every build (and the CI build)
# fetches byte-identical sources. Bump deliberately, then re-run the tests.
set(RAYLIB_PINNED_COMMIT "fdc8b37391b52fdc8cc62af37d02235c9ec59daa") # master @ 2026-10-11, v6.1-dev

FetchContent_Declare(
        raylib
        GIT_REPOSITORY "https://github.com/raysan5/raylib.git"
        GIT_TAG "${RAYLIB_PINNED_COMMIT}"
)
FetchContent_MakeAvailable(raylib)

# Application icon embedded into the exe and used for the Start Menu shortcut.
set(APP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.ico")
configure_file(app.rc.in "${CMAKE_CURRENT_BINARY_DIR}/app.rc" @ONLY)

add_executable(Internet_Visualizer
        main.cpp
        "${CMAKE_CURRENT_BINARY_DIR}/app.rc"
        # networking / geolocation core (C11)
        src/NetTypes.c
        src/NetPlatform.c
        src/NetworkMonitor.c
        src/ConnectionArch.c
        src/ConnectionTracker.c
        src/GeoLocation.c
        # rendering (C11)
        src/MapProjection.c
        src/MapRenderer.c
        src/ConnectionArchRenderer.c
        src/UiText.c
)

target_include_directories(Internet_Visualizer PRIVATE src Include)

add_custom_command(TARGET Internet_Visualizer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/assets"
        "$<TARGET_FILE_DIR:Internet_Visualizer>/assets"
)

target_link_libraries(Internet_Visualizer PRIVATE raylib)

if (WIN32)
    # iphlpapi : IP Helper API (interfaces, routes, TCP/UDP tables)
    # ws2_32   : Winsock (address formatting, reverse DNS)
    # winhttp  : HTTPS client used by the geolocation worker
    target_link_libraries(Internet_Visualizer PRIVATE iphlpapi ws2_32 winhttp)
endif()
