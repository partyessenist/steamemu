cmake_minimum_required(VERSION 3.15)
project(steamemu LANGUAGES CXX)

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
    src/compat_init.cpp)

target_include_directories(steam_api PRIVATE ${SDK_INCLUDE} src)
target_compile_definitions(steam_api PRIVATE STEAM_API_EXPORTS)
find_package(Threads REQUIRED)
target_link_libraries(steam_api PRIVATE Threads::Threads)

# The LAN backend (net.cpp) uses Winsock2 on Windows; link the import library so
# socket/bind/select/WSAStartup resolve. (POSIX has these in libc.)
if(WIN32)
  target_link_libraries(steam_api PRIVATE ws2_32)
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
add_test(NAME version_dispatch COMMAND version_dispatch_test)
set_tests_properties(version_dispatch PROPERTIES
    PASS_REGULAR_EXPRESSION "PASS: version_dispatch all checks passed")

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

  add_executable(lan_test test/lan_test.cpp)
  target_include_directories(lan_test PRIVATE ${SDK_INCLUDE})
  target_link_libraries(lan_test PRIVATE steam_api)
  add_test(NAME lan_discovery COMMAND lan_test)
  set_tests_properties(lan_discovery PROPERTIES
      PASS_REGULAR_EXPRESSION "PASS: lan_test peers discovered"
      TIMEOUT 40)

  add_executable(lobby_test test/lobby_test.cpp)
  target_include_directories(lobby_test PRIVATE ${SDK_INCLUDE})
  target_link_libraries(lobby_test PRIVATE steam_api)
  add_test(NAME lan_lobbies COMMAND lobby_test)
  set_tests_properties(lan_lobbies PROPERTIES
      PASS_REGULAR_EXPRESSION "PASS: lobby_test create/join/data-sync worked"
      TIMEOUT 40)

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

  add_executable(lobby_rich_test test/lobby_rich_test.cpp)
  target_include_directories(lobby_rich_test PRIVATE ${SDK_INCLUDE})
  target_link_libraries(lobby_rich_test PRIVATE steam_api)
  add_test(NAME lan_lobbies_rich COMMAND lobby_rich_test)
  set_tests_properties(lan_lobbies_rich PROPERTIES
      PASS_REGULAR_EXPRESSION "PASS: lobby_rich_test member-data/chat/filters/request worked"
      TIMEOUT 45)
endif()
