cmake_minimum_required(VERSION 3.24)
project(Animunculi VERSION 0.3.6 LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Animunculi - SKSE plugin that lets an external AI agent inhabit a Skyrim actor
# (perception, events, voice with lip sync, vision, actions) over a local
# JSON-lines TCP protocol. See docs/PROTOCOL.md; the MCP server lives in mcp/.
#
# Build dependencies are borrowed from a skyrim-community-shaders checkout that has been
# built once (it fetches CommonLibSSE-NG 6.7.1 prebuilt and a vcpkg tree), so no vcpkg run of
# our own is required:
#   CS_ROOT                 skyrim-community-shaders repo (or the ANIMUNCULI_CS_ROOT environment variable)
#   COMMONLIB_DIR           its extern/CommonLibSSE-NG submodule (v6.7.1)
#   COMMONLIB_PREBUILT_DIR  the prebuilt CommonLibSSE bundle CS already fetched
#   VCPKG_INSTALLED         CS's vcpkg_installed triplet (spdlog, fmt, nlohmann, stb)
# Override any of them with -D<NAME>=...
# ---------------------------------------------------------------------------
set(CS_ROOT "$ENV{ANIMUNCULI_CS_ROOT}" CACHE PATH "skyrim-community-shaders checkout (built once)")
set(COMMONLIB_DIR "${CS_ROOT}/extern/CommonLibSSE-NG" CACHE PATH "CommonLibSSE-NG source")
set(COMMONLIB_PREBUILT_DIR "${CS_ROOT}/build/ALL/CommonLibSSE/.prebuilt/v6.7.1" CACHE PATH "Prebuilt CommonLibSSE bundle")
set(VCPKG_INSTALLED "${CS_ROOT}/build/ALL/vcpkg_installed/x64-windows-static-md-release" CACHE PATH "vcpkg installed triplet dir")
option(ANIMUNCULI_TESTS "Build the native unit tests (no game required)" ON)
if(NOT EXISTS "${COMMONLIB_DIR}/CMakeLists.txt" OR NOT EXISTS "${COMMONLIB_PREBUILT_DIR}" OR NOT EXISTS "${VCPKG_INSTALLED}")
    message(FATAL_ERROR
        "Animunculi builds against CommonLibSSE-NG 6.7.1 as set up by a built skyrim-community-shaders "
        "checkout. Set CS_ROOT (-DCS_ROOT=... or the ANIMUNCULI_CS_ROOT environment variable), or "
        "COMMONLIB_DIR, COMMONLIB_PREBUILT_DIR and VCPKG_INSTALLED individually. See README.md.\n"
        "  COMMONLIB_DIR=${COMMONLIB_DIR}\n  COMMONLIB_PREBUILT_DIR=${COMMONLIB_PREBUILT_DIR}\n  VCPKG_INSTALLED=${VCPKG_INSTALLED}")
endif()

list(APPEND CMAKE_PREFIX_PATH "${VCPKG_INSTALLED}")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
set(CMAKE_MAP_IMPORTED_CONFIG_DEBUG Release)
set(CMAKE_MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release)

# CommonLibSSE-NG options (match the prebuilt bundle's baked configuration)
set(COMMONLIB_PREBUILT ON CACHE BOOL "" FORCE)
set(COMMONLIB_PREBUILT_MULTICONFIG ON CACHE BOOL "" FORCE)
set(SKSE_SUPPORT_XBYAK ON CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_SE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_AE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_VR ON CACHE BOOL "" FORCE)

add_compile_definitions(_UNICODE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

# --- pure core (no game dependencies): shared by the plugin and the tests ----
set(CORE_SOURCES
    src/core/Wav.cpp
    src/core/Text.cpp
    src/core/EventLog.cpp
    src/net/Rpc.cpp
    src/net/Server.cpp
)

add_subdirectory("${COMMONLIB_DIR}" CommonLibSSE EXCLUDE_FROM_ALL)
find_package(spdlog CONFIG REQUIRED)

add_library(${PROJECT_NAME} SHARED
    ${CORE_SOURCES}
    src/main.cpp
    src/Settings.cpp
    src/game/Actions.cpp
    src/game/Agents.cpp
    src/game/Behavior.cpp
    src/game/Body.cpp
    src/game/ChatUI.cpp
    src/game/Describe.cpp
    src/game/Events.cpp
    src/game/Guard.cpp
    src/game/Knowledge.cpp
    src/game/MainThread.cpp
    src/game/Methods.cpp
    src/game/Papyrus.cpp
    src/game/Targets.cpp
    src/game/Vision.cpp
    src/game/Voice.cpp
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    SKYRIM
    ANIMUNCULI_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    ANIMUNCULI_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    ANIMUNCULI_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    ANIMUNCULI_VERSION_STRING="${PROJECT_VERSION}"
)

target_include_directories(${PROJECT_NAME} PRIVATE src extern "${VCPKG_INSTALLED}/include")
target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)
set_source_files_properties(${CORE_SOURCES} src/game/Guard.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

target_compile_options(${PROJECT_NAME} PRIVATE
    /W4 /permissive- /Zc:preprocessor /EHsc /MP /Zi /utf-8
    $<$<CONFIG:Release>:/O2 /Oi /Gy>
    /wd4200 /wd4324
)
target_link_options(${PROJECT_NAME} PRIVATE /DEBUG:FULL /MAP $<$<CONFIG:Release>:/OPT:REF /OPT:ICF>)

target_link_libraries(${PROJECT_NAME} PRIVATE
    CommonLibSSE::CommonLibSSE
    spdlog::spdlog
    ws2_32
    xaudio2
    d3d11
    dxgi
)

# Stage a ready-to-install mod folder under dist/Animunculi (MO2 layout).
set(STAGE "${CMAKE_SOURCE_DIR}/dist/Animunculi")
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${STAGE}/SKSE/Plugins"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:${PROJECT_NAME}>" "${STAGE}/SKSE/Plugins/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_PDB_FILE:${PROJECT_NAME}>" "${STAGE}/SKSE/Plugins/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/data/SKSE/Plugins/Animunculi.ini" "${STAGE}/SKSE/Plugins/"
)

# --- native unit tests ------------------------------------------------------
if(ANIMUNCULI_TESTS)
    add_executable(animunculi_tests ${CORE_SOURCES} src/game/Guard.cpp tests/native/test_main.cpp)
    target_include_directories(animunculi_tests PRIVATE src "${VCPKG_INSTALLED}/include")
    target_compile_options(animunculi_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
    target_link_libraries(animunculi_tests PRIVATE ws2_32)
endif()
