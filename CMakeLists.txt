cmake_minimum_required(VERSION 3.15)
project(steamemu VERSION 0.1.0 LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Version: SemVer, single source of truth is the project() line above (mirrored
# in the VERSION file for release tooling). Releases are cut from an annotated
# git tag `vMAJOR.MINOR.PATCH` (see .github/workflows/release.yml); the tag is
# authoritative for the published artifacts, this number for local builds.
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# Reference Steamworks SDK (headers are ground truth; treated as read-only).
# Override with -DSTEAMWORKS_SDK=/path/to/sdk if it lives elsewhere.
# ---------------------------------------------------------------------------
set(STEAMWORKS_SDK "${CMAKE_SOURCE_DIR}/third_party/SteamworksSDK" CACHE PATH
    "Path to the Steamworks SDK (expects <dir>/public/steam/*.h)")
if(NOT EXISTS "${STEAMWORKS_SDK}/public/steam/steam_api.h")
  message(FATAL_ERROR
      "Steamworks SDK not found under STEAMWORKS_SDK='${STEAMWORKS_SDK}'. "
      "Set -DSTEAMWORKS_SDK=/path/to/sdk (must contain public/steam/steam_api.h).")
endif()
set(SDK_INCLUDE "${STEAMWORKS_SDK}/public")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)   # only S_API-marked symbols are exported

# ---------------------------------------------------------------------------
# The Dear ImGui game overlay (shift-tab friends/lobby panel, invite/notify
# toasts). It hooks the game's D3D11 present path (Windows-only) and stands up
# its own Win32 window as a fallback, so it builds only on Windows; a headless /
# Linux build compiles the overlay's inert no-op path instead and the emulated
# ISteamFriends::ActivateGameOverlay* / ISteamUtils::IsOverlayEnabled methods
# degrade to "no overlay present". Compile-time gate; the runtime gate is the
# opt-in [overlay] enabled key in steamemu.ini. See src/overlay/.
# ---------------------------------------------------------------------------
option(STEAMEMU_ENABLE_OVERLAY "Build the Dear ImGui game overlay (Windows only)" ON)
if(NOT WIN32)
  set(STEAMEMU_ENABLE_OVERLAY OFF)
endif()

# ---------------------------------------------------------------------------
# The emulator library. Loaded by a game in place of the real steam_api.
#   Windows: steam_api.dll (32-bit) / steam_api64.dll (64-bit)
#   Linux:   libsteam_api.so
#
# NOTE: this machine only has a native 64-bit toolchain. 32-bit and Windows
# (MinGW / MSVC) targets are produced by configuring with the matching
# toolchain/generator; the naming below already accounts for them.
# ---------------------------------------------------------------------------
add_library(steam_api SHARED
    src/steam_api.cpp
    src/emu.cpp
    src/config.cpp
    src/auth.cpp
    src/dispatch.cpp
    src/net.cpp
    src/storage.cpp
    src/services.cpp
    src/generated/registry.cpp
    src/generated/flat_api.cpp
    src/compat_flat.cpp
    src/compat_init.cpp
    src/overlay/Overlay.cpp
    src/overlay/SwapchainHook.cpp)

target_include_directories(steam_api PRIVATE ${SDK_INCLUDE} src)
target_compile_definitions(steam_api PRIVATE STEAM_API_EXPORTS)
find_package(Threads REQUIRED)
target_link_libraries(steam_api PRIVATE Threads::Threads)

# The LAN backend (net.cpp) uses Winsock2 on Windows; link the import library so
# socket/bind/select/WSAStartup resolve. (POSIX has these in libc.) iphlpapi
# provides GetAdaptersAddresses, used to fan discovery multicast out every local
# interface (cross-machine discovery).
if(WIN32)
  target_link_libraries(steam_api PRIVATE ws2_32 iphlpapi)
endif()

# ---------------------------------------------------------------------------
# Dear ImGui overlay. Vendored under third_party/imgui (a git submodule pinned
# to v1.90.9-docking). Built as a separate static library so ImGui's own
# warnings never trip our warning flags, then linked into the DLL where its
# symbols stay internal (only S_API functions are exported). Only the overlay
# TUs include ImGui; no exported signature or generated stub TU may. The overlay
# facade (src/overlay/overlay.h) is ImGui-free, so the emulator links against it
# unconditionally -- when the overlay is compiled out it is an inert no-op.
# ---------------------------------------------------------------------------
if(STEAMEMU_ENABLE_OVERLAY)
  set(IMGUI_DIR "${CMAKE_SOURCE_DIR}/third_party/imgui")
  if(NOT EXISTS "${IMGUI_DIR}/imgui.cpp")
    message(FATAL_ERROR
        "STEAMEMU_ENABLE_OVERLAY is ON but ImGui is not vendored at ${IMGUI_DIR}. "
        "Run: git submodule update --init third_party/imgui")
  endif()
  add_library(steamemu_imgui STATIC
      "${IMGUI_DIR}/imgui.cpp"
      "${IMGUI_DIR}/imgui_draw.cpp"
      "${IMGUI_DIR}/imgui_tables.cpp"
      "${IMGUI_DIR}/imgui_widgets.cpp"
      "${IMGUI_DIR}/backends/imgui_impl_win32.cpp"
      "${IMGUI_DIR}/backends/imgui_impl_dx11.cpp")
  target_include_directories(steamemu_imgui PUBLIC "${IMGUI_DIR}" "${IMGUI_DIR}/backends")
  set_target_properties(steamemu_imgui PROPERTIES POSITION_INDEPENDENT_CODE ON)
  if(MSVC)
    target_compile_options(steamemu_imgui PRIVATE /W0)
  else()
    target_compile_options(steamemu_imgui PRIVATE -w)
  endif()

  target_link_libraries(steam_api PRIVATE steamemu_imgui d3d11 dxgi d3dcompiler dwmapi)
  target_include_directories(steam_api PRIVATE "${IMGUI_DIR}" "${IMGUI_DIR}/backends")
  target_compile_definitions(steam_api PRIVATE STEAMEMU_ENABLE_OVERLAY=1)
endif()

# ---------------------------------------------------------------------------
# Regenerate the interface/flat/registry surface from steam_api.json. The
# generated sources are checked in (so a plain build needs no Python), but run
# `cmake --build build --target regen` after bumping interface versions.
# ---------------------------------------------------------------------------
find_program(PYTHON3 python3)
if(PYTHON3)
  add_custom_target(regen
      COMMAND ${PYTHON3} ${CMAKE_SOURCE_DIR}/tools/gen.py ${STEAMWORKS_SDK} ${CMAKE_SOURCE_DIR}/src/generated
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
      COMMENT "Regenerating src/generated from steam_api.json")
endif()

if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
  set_target_properties(steam_api PROPERTIES OUTPUT_NAME steam_api64 PREFIX "")
elseif(WIN32)
  set_target_properties(steam_api PROPERTIES OUTPUT_NAME steam_api PREFIX "")
endif()

# ---------------------------------------------------------------------------
# Smoke-test harness. Compiled like a game (NO STEAM_API_EXPORTS) so it uses
# the inline header accessors and links against the emulator library.
# ---------------------------------------------------------------------------
add_executable(harness test/harness.cpp)
target_include_directories(harness PRIVATE ${SDK_INCLUDE})
target_link_libraries(harness PRIVATE steam_api)

add_executable(api_test test/api_test.cpp)
target_include_directories(api_test PRIVATE ${SDK_INCLUDE})
target_link_libraries(api_test PRIVATE steam_api)

enable_testing()
add_test(NAME identity_roundtrip COMMAND harness)
set_tests_properties(identity_roundtrip PROPERTIES
    PASS_REGULAR_EXPRESSION "PASS: identity round-trip consistent")

add_test(NAME api_behavior COMMAND api_test)
set_tests_properties(api_behavior PROPERTIES
    PASS_REGULAR_EXPRESSION "PASS: api_test all checks passed")

add_executable(async_test test/async_test.cpp)
target_include_directories(async_test PRIVATE ${SDK_INCLUDE})
target_link_libraries(async_test PRIVATE steam_api)
add_test(NAME async_callresults COMMAND async_test)
set_tests_properties(async_callresults PROPERTIES
    PASS_REGULAR_EXPRESSION "PASS: async_test all checks passed")

add_executable(version_dispatch_test test/version_dispatch_test.cpp)
target_include_directories(version_dispatch_test PRIVATE ${SDK_INCLUDE})
target_link_libraries(version_dispatch_test PRIVATE steam_api)
# This test deliberately reinterpret_casts real objects to locally-declared
# abstract structs (OldUserStats/OldUser) to invoke a specific legacy vtable
# slot the current SDK header no longer names. Such a local type has a pure
# virtual and no visible derived class, so at -O2 GCC's speculative
# devirtualization proves the "only" target is the pure method and emits a
# direct __cxa_pure_virtual call -> abort. The shipped DLL vtables are concrete
# and unaffected (the crash is purely this punning in the test TU); disable
# devirtualization here so the intended real-slot virtual dispatch survives.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  target_compile_options(version_dispatch_test PRIVATE
      -fno-devirtualize -fno-devirtualize-speculatively)
endif()
add_test(NAME version_dispatch COMMAND version_dispatch_test)
set_tests_properties(version_dispatch PROPERTIES
    PASS_REGULAR_EXPRESSION "PASS: version_dispatch all checks passed")

# Overlay activation -> GameOverlayActivated_t + BOverlayNeedsPresent. Portable:
# drives the overlay through the Steam API without a renderer (the D3D11/Win32
# backends are exercised by a live run). STEAMEMU_OVERLAY_BUILT tells the test
# whether to expect the active or the inert (compiled-out) behavior.
add_executable(overlay_test test/overlay_test.cpp)
target_include_directories(overlay_test PRIVATE ${SDK_INCLUDE})
target_link_libraries(overlay_test PRIVATE steam_api)
if(STEAMEMU_ENABLE_OVERLAY)
  target_compile_definitions(overlay_test PRIVATE STEAMEMU_OVERLAY_BUILT=1)
endif()
add_test(NAME overlay_activation COMMAND overlay_test)
set_tests_properties(overlay_activation PROPERTIES
    PASS_REGULAR_EXPRESSION "PASS: overlay_test all checks passed"
    TIMEOUT 20)

# Two-instance LAN discovery + P2P (portable: spawns two copies of itself). It
# exercises the multicast beacon fanned out over every local interface -- the
# path that made cross-machine discovery work. Same-host here (loopback); a real
# LAN reaches other machines with the same code.
add_executable(lan_test test/lan_test.cpp)
target_include_directories(lan_test PRIVATE ${SDK_INCLUDE})
target_link_libraries(lan_test PRIVATE steam_api)
add_test(NAME lan_discovery COMMAND lan_test)
set_tests_properties(lan_discovery PROPERTIES
    PASS_REGULAR_EXPRESSION "PASS: lan_test peers discovered"
    TIMEOUT 40)

# Two-instance lobby create/join/data-sync, and the rich variant (member-data,
# chat, filters). Portable (test/lan_spawn.h). These exercise lobby gossip and
# chat over the discovery multicast -- including chat de-duplication now that the
# beacon is fanned out over every interface.
add_executable(lobby_test test/lobby_test.cpp)
target_include_directories(lobby_test PRIVATE ${SDK_INCLUDE})
target_link_libraries(lobby_test PRIVATE steam_api)
add_test(NAME lan_lobbies COMMAND lobby_test)
set_tests_properties(lan_lobbies PROPERTIES
    PASS_REGULAR_EXPRESSION "PASS: lobby_test create/join/data-sync worked"
    TIMEOUT 40)

add_executable(lobby_rich_test test/lobby_rich_test.cpp)
target_include_directories(lobby_rich_test PRIVATE ${SDK_INCLUDE})
target_link_libraries(lobby_rich_test PRIVATE steam_api)
add_test(NAME lan_lobbies_rich COMMAND lobby_rich_test)
set_tests_properties(lan_lobbies_rich PROPERTIES
    PASS_REGULAR_EXPRESSION "PASS: lobby_rich_test member-data/chat/filters/request worked"
    TIMEOUT 45)

# Portable (no fork/sockets): guards clean shutdown + re-init on every platform.
# The TIMEOUT makes a shutdown hang (the bug this covers) a failure.
add_executable(shutdown_test test/shutdown_test.cpp)
target_include_directories(shutdown_test PRIVATE ${SDK_INCLUDE})
target_link_libraries(shutdown_test PRIVATE steam_api)
add_test(NAME shutdown_lifecycle COMMAND shutdown_test)
set_tests_properties(shutdown_lifecycle PROPERTIES
    PASS_REGULAR_EXPRESSION "PASS: shutdown_test clean shutdown \\+ re-init"
    TIMEOUT 20)

# Portable: manual callback dispatch (the pump Facepunch/Steamworks.NET games use).
add_executable(manual_dispatch_test test/manual_dispatch_test.cpp)
target_include_directories(manual_dispatch_test PRIVATE ${SDK_INCLUDE})
target_link_libraries(manual_dispatch_test PRIVATE steam_api)
add_test(NAME manual_dispatch COMMAND manual_dispatch_test)
set_tests_properties(manual_dispatch PROPERTIES
    PASS_REGULAR_EXPRESSION "PASS: manual_dispatch_test logon delivered"
    TIMEOUT 20)

# Manual-dispatch call-results + client/game-server pipe separation (the listen-
# server "Creating lobby" hang). Portable, so it runs on Windows too.
add_executable(manual_dispatch_result_test test/manual_dispatch_result_test.cpp)
target_include_directories(manual_dispatch_result_test PRIVATE ${SDK_INCLUDE})
target_link_libraries(manual_dispatch_result_test PRIVATE steam_api)
add_test(NAME manual_dispatch_results COMMAND manual_dispatch_result_test)
set_tests_properties(manual_dispatch_results PROPERTIES
    PASS_REGULAR_EXPRESSION "PASS: manual_dispatch_result_test"
    TIMEOUT 20)

# The remaining tests use POSIX facilities (fork/exec for per-config child
# processes, and BSD sockets for the loopback HTTP server / LAN transport), so
# they are POSIX-only. On Windows only the library + the portable harness/
# api_test build; the emulator DLL itself is fully cross-platform.
if(NOT WIN32)
  add_executable(storage_test test/storage_test.cpp)
  target_include_directories(storage_test PRIVATE ${SDK_INCLUDE})
  target_link_libraries(storage_test PRIVATE steam_api)
  add_test(NAME storage_persistence COMMAND storage_test)
  set_tests_properties(storage_persistence PROPERTIES
      PASS_REGULAR_EXPRESSION "PASS: storage_test persistence round-trip")

  add_executable(http_test test/http_test.cpp)
  target_include_directories(http_test PRIVATE ${SDK_INCLUDE})
  target_link_libraries(http_test PRIVATE steam_api)
  add_test(NAME http_client COMMAND http_test)
  set_tests_properties(http_client PROPERTIES
      PASS_REGULAR_EXPRESSION "PASS: http_test real HTTP round-trip"
      TIMEOUT 30)

  add_executable(config_test test/config_test.cpp)
  target_include_directories(config_test PRIVATE ${SDK_INCLUDE})
  target_link_libraries(config_test PRIVATE steam_api)
  add_test(NAME config_file COMMAND config_test)
  set_tests_properties(config_file PROPERTIES
      PASS_REGULAR_EXPRESSION "PASS: config_test all checks passed")

  add_executable(sockets_test test/sockets_test.cpp)
  target_include_directories(sockets_test PRIVATE ${SDK_INCLUDE})
  target_link_libraries(sockets_test PRIVATE steam_api)
  add_test(NAME lan_sockets COMMAND sockets_test)
  set_tests_properties(lan_sockets PROPERTIES
      PASS_REGULAR_EXPRESSION "PASS: sockets_test connect \\+ message delivery worked"
      TIMEOUT 40)

  add_executable(leaderboard_test test/leaderboard_test.cpp)
  target_include_directories(leaderboard_test PRIVATE ${SDK_INCLUDE})
  target_link_libraries(leaderboard_test PRIVATE steam_api)
  add_test(NAME leaderboards COMMAND leaderboard_test)
  set_tests_properties(leaderboards PROPERTIES
      PASS_REGULAR_EXPRESSION "PASS: leaderboard_test upload/rank/persist worked"
      TIMEOUT 30)
endif()
