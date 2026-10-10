cmake_minimum_required(VERSION 3.24)
project(CrimsonDesertMultiplayer VERSION 0.2.0 LANGUAGES CXX)

if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Build with MSVC for x64 (the game is 64-bit Windows only). Use scripts/build.ps1.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: the .asi must not depend on whichever runtime the game ships.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _WIN32_WINNT=0x0A00
                        CDMP_VERSION="${PROJECT_VERSION}")
add_compile_options(/permissive- /utf-8 /EHsc /Zi)
add_link_options(/DEBUG)

# Strict warnings for our own code only (third-party code keeps its defaults).
function(cdmp_strict target)
    target_compile_options(${target} PRIVATE /W4)
endfunction()

# --------------------------------------------------------------- third party
include(FetchContent)
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.92.9b
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(imgui)

# Inline hooks with proper trampolines (Zydis disassembler, fetched too).
set(SAFETYHOOK_FETCH_ZYDIS ON CACHE BOOL "" FORCE)
FetchContent_Declare(safetyhook
    GIT_REPOSITORY https://github.com/cursey/safetyhook.git
    GIT_TAG        v0.7.0
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(safetyhook)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp
)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
target_link_libraries(imgui PUBLIC d3d12 dxgi d3dcompiler)

# ------------------------------------------------------------------ our code
# Shared by the mod and the tools: wire protocol, UDP, session logic.
add_library(cdmp_common STATIC
    src/common/pattern.cpp
    src/common/protocol.cpp
    src/common/udp.cpp
    src/common/client_session.cpp
    src/common/reliable.cpp
    src/common/enemy_load.cpp
)
cdmp_strict(cdmp_common)
target_include_directories(cdmp_common PUBLIC src)
target_link_libraries(cdmp_common PUBLIC ws2_32)

# The mod: cdmp.asi, loaded by Ultimate ASI Loader inside the game.
# The build id the two players compare when they connect: the first 8 hex
# digits of the commit (0 outside git), plus whether there were local changes.
set(CDMP_GIT_HASH 00000000)
set(CDMP_GIT_DIRTY 0)
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short=8 HEAD WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                    OUTPUT_VARIABLE _hash OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE _rc)
    if(_rc EQUAL 0 AND _hash MATCHES "^[0-9a-f]+$")
        set(CDMP_GIT_HASH ${_hash})
    endif()
    execute_process(COMMAND ${GIT_EXECUTABLE} status --porcelain --untracked-files=no -- src
                    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} OUTPUT_VARIABLE _dirty ERROR_QUIET)
    if(NOT _dirty STREQUAL "")
        set(CDMP_GIT_DIRTY 1)
    endif()
endif()
configure_file(cmake/build_id.h.in ${CMAKE_BINARY_DIR}/generated/build_id.h)

add_library(cdmp_mod SHARED
    src/mod/dllmain.cpp
    src/mod/mod.cpp
    src/mod/log.cpp
    src/mod/config.cpp
    src/mod/game/safe_memory.cpp
    src/mod/game/player_reader.cpp
    src/mod/game/camera_reader.cpp
    src/mod/game/rtti.cpp
    src/mod/net/troc_sniffer.cpp
    src/mod/net/local_capture.cpp
    src/mod/net/remote_puppet.cpp
    src/mod/net/puppet_minimap.cpp
    src/mod/net/horse_diag.cpp
    src/mod/net/aggro_test.cpp
    src/mod/net/enemy_relay.cpp
    src/mod/net/native_suppress.cpp
    src/mod/net/player_hits.cpp
    src/mod/net/piece4_test.cpp
    src/mod/net/guest_hits.cpp
    src/mod/net/learnt_store.cpp
    src/mod/net/loot_twin.cpp
    src/mod/net/attack_probe.cpp
    src/mod/net/coop.cpp
    src/mod/game/actor_lookup.cpp
    src/mod/game/game_errors.cpp
    src/mod/game/game_clock.cpp
    src/mod/net/steam_transport.cpp
    src/mod/net/time_sync.cpp
    src/mod/render/dx12_hook.cpp
    src/mod/render/hud.cpp
)
cdmp_strict(cdmp_mod)
target_link_libraries(cdmp_mod PRIVATE cdmp_common imgui safetyhook::safetyhook)
target_include_directories(cdmp_mod PRIVATE ${CMAKE_BINARY_DIR}/generated)
set_target_properties(cdmp_mod PROPERTIES OUTPUT_NAME "cdmp" PREFIX "" SUFFIX ".asi")
# Only the symbol file's name goes into cdmp.asi, not the path of the PC that
# built it (it named the user's folders).
target_link_options(cdmp_mod PRIVATE "/PDBALTPATH:cdmp.pdb")

# Tools for single-PC development.
add_executable(cdmp_peer tools/peer_sim/main.cpp)
cdmp_strict(cdmp_peer)
target_link_libraries(cdmp_peer PRIVATE cdmp_common)

add_executable(cdmp_host tools/host/main.cpp)
cdmp_strict(cdmp_host)

# D3D12 window that loads the mod, to test the overlay hook without the game.
add_executable(cdmp_dx12_test tools/dx12_test/main.cpp)
cdmp_strict(cdmp_dx12_test)
target_link_libraries(cdmp_dx12_test PRIVATE d3d12 dxgi)

# Read-only external memory inspector (hito 1).
add_executable(cdmp_probe tools/probe/main.cpp tools/probe/process.cpp)
cdmp_strict(cdmp_probe)
target_include_directories(cdmp_probe PRIVATE tools)
target_link_libraries(cdmp_probe PRIVATE cdmp_common)

# Tests.
enable_testing()
add_executable(cdmp_tests tests/tests.cpp)
cdmp_strict(cdmp_tests)
target_link_libraries(cdmp_tests PRIVATE cdmp_common)
add_test(NAME unit COMMAND cdmp_tests)
