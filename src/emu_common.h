//====== steamemu ============================================================
// Shared helpers for the emulator implementation: local identity, fixed
// handles, config, and a lightweight call log.
//============================================================================
#pragma once

#include "steam/steam_api.h"
#include "steam/isteamclient.h"

#include <cstdint>

namespace emu {

// The emulator presents a single local user on a single pipe. Real Steam can
// multiplex several, but a game only ever sees one, so we hand back fixed,
// non-zero handles everywhere. (0 is treated as invalid by callers.)
constexpr HSteamUser kHSteamUser = 1;
constexpr HSteamPipe kHSteamPipe = 1;

// A game that runs an integrated game server pumps TWO
// manual-dispatch loops -- one on the client pipe, one on the game-server pipe --
// and expects each to yield only its own callbacks. So the game-server pipe/user
// MUST be distinct from the client's, or one pump steals the other's callbacks
// (e.g. the game-server pump consuming the client's LobbyCreated_t completion).
constexpr HSteamUser kHSteamUserGameServer = 2;
constexpr HSteamPipe kHSteamPipeGameServer = 2;

// Fabricated identity for this instance. Config-driven (STEAMEMU_STEAMID env or
// steamemu.ini) so two instances on a LAN get distinct IDs; falls back to a
// fixed individual account in the public universe.
CSteamID LocalSteamID();

// This instance's game-server identity: same account id, GameServer type.
// It's what ISteamGameServer::GetSteamID returns, what games publish via
// SetLobbyGameServer, and the sender id on P2P sent from the server side
// (clients filter server traffic by it).
CSteamID LocalGSSteamID();

// The persona (display) name this instance advertises (STEAMEMU_NAME / config).
const char* PersonaName();

// AppID for this run: honors steam_appid.txt in the cwd and the SteamAppId /
// SteamGameId env vars, so no real client is required during development.
uint32 AppID();

// Publish the environment variables the real Steam client sets when launching
// a game (SteamAppId, SteamGameId, SteamEnv, ...). Games read these directly:
// Arma 3's server compares getenv("SteamAppId") against its appid and its
// client gates the "Steam layer" on them. Called from both init paths.
void PublishSteamEnvironment();

// Current wall-clock time as a Unix timestamp (uint32), for the several APIs
// that expose server/real time.
uint32 UnixTime();

// True the FIRST time user info is requested for a given id (records it), false
// after. RequestUserInformation posts a PersonaStateChange_t only on the first
// request -- otherwise a game that re-requests from its PersonaStateChange handler
// spins forever posting/handling the same callback.
bool FirstUserInfoRequest(CSteamID id);

// Severity levels, most-severe first. A message is emitted iff its level is at
// or above (numerically <=) the current threshold.
enum class LogLevel { Off = 0, Error = 1, Warn = 2, Info = 3, Debug = 4, Trace = 5 };

// Current threshold, resolved once from STEAMEMU_LOG_LEVEL (env) or `log_level`
// in steamemu.ini; defaults to Info. Accepts names (off/error/warn/info/debug/
// trace) or the numbers 0-5.
LogLevel CurrentLogLevel();

// Leveled, printf-style log (no dedup). Emitted only if enabled by the level.
// Output goes to stderr and, if STEAMEMU_LOG is set, that file.
void LogAt(LogLevel level, const char *fmt, ...);

// Per-method coverage announce: deduplicates by method name so each method logs
// once. Sits at Debug level, so it's silent unless log_level is debug/trace --
// coverage gaps stay discoverable without drowning a normal run.
void Log(const char *method);

} // namespace emu

// Every emulated interface method calls this first so unimplemented surface is
// observable at runtime (at debug level) rather than silently returning empty data.
#define EMU_LOG(method) ::emu::Log(method)

// Leveled logging for real events (network up/down, errors, notable state).
#define EMU_ERROR(...) ::emu::LogAt(::emu::LogLevel::Error, __VA_ARGS__)
#define EMU_WARN(...)  ::emu::LogAt(::emu::LogLevel::Warn,  __VA_ARGS__)
#define EMU_INFO(...)  ::emu::LogAt(::emu::LogLevel::Info,  __VA_ARGS__)
#define EMU_DEBUG(...) ::emu::LogAt(::emu::LogLevel::Debug, __VA_ARGS__)
#define EMU_TRACE(...) ::emu::LogAt(::emu::LogLevel::Trace, __VA_ARGS__)
