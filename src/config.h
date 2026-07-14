//====== steamemu ============================================================
// Configuration subsystem.
//
// A user drops a  steamemu.ini  next to the game to pick their identity and
// environment. The file is a small INI: [sections], `key = value`, and `#`/`;`
// comments. It is loaded ONCE (per process) from the cwd, or from the path in
// the STEAMEMU_CONFIG environment variable.
//
// Environment variables still win for the handful of keys the emulator has
// always honored (STEAMEMU_STEAMID, STEAMEMU_NAME, SteamAppId/SteamGameId) so
// existing setups and the test harness keep working.
//
// String getters hand back const char*/const std::string& backed by members of
// the process-life singleton, so the borrowed pointers Steam interfaces return
// stay valid for the lifetime of the process.
//============================================================================
#pragma once

#include "steam/steamclientpublic.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace emu {

class ConfigData {
public:
	// --- identity / environment ---------------------------------------------
	// Fabricated local identity. From STEAMEMU_STEAMID, else `steam_id` /
	// `account_id` (a value >= 2^32 is treated as a full 64-bit SteamID, a
	// smaller one as a 32-bit account id in the public individual universe).
	CSteamID SteamID() const { return m_steamID; }
	// Persona (display) name. STEAMEMU_NAME, else `persona_name`, else "Player".
	const std::string& PersonaName() const { return m_personaName; }
	// Game language (`language`, default "english").
	const std::string& Language() const { return m_language; }
	// Steam UI language (`ui_language`, defaults to Language()).
	const std::string& UiLanguage() const { return m_uiLanguage; }
	// AppID: SteamAppId/SteamGameId env, else `app_id`, else steam_appid.txt.
	uint32_t AppID() const { return m_appID; }
	// ISO country code for GetIPCountry (`country`, default "US").
	const std::string& Country() const { return m_country; }
	// Log verbosity name (`log_level`, default "info"); parsed by the logger.
	const std::string& LogLevelName() const { return m_logLevel; }
	// Log file path (`log_file`); empty = file logging off. Lets a game process
	// enable logging via steamemu.ini even when env vars don't reach it (e.g. an
	// anti-cheat relaunch). STEAMEMU_LOG env takes precedence (handled in emu.cpp).
	const std::string& LogFileName() const { return m_logFile; }
	// Storage root override (`save_path` / `storage_dir`); empty => the default
	// steamemu_storage/<appid> is used by StorageRoot().
	const std::string& SavePath() const { return m_savePath; }

	// --- canned HTTP responses (the [http] section) -------------------------
	// We can't do real TLS, so https requests fail by default. A game that hits
	// a known endpoint can instead be served a configured response: match the
	// request URL against the [http] entries (longest URL-prefix wins, so query
	// strings match) and return its status/content-type/body. Returns false if
	// no entry matches (caller falls back to the real client / https failure).
	bool MockHttp(const std::string& url, int* status, std::string* contentType,
	              std::vector<uint8_t>* body) const;
	bool HasHttpMocks() const { return !m_httpMocks.empty(); }

	// --- installed DLC (the [dlc] section) ----------------------------------
	bool HasDLC(uint32_t appID) const;
	int DLCCount() const { return (int)m_dlc.size(); }
	// Fill the i-th DLC's appID/availability/name (name truncated to nameBuf).
	// Returns false for an out-of-range index.
	bool DLCByIndex(int i, uint32_t* appID, bool* available, char* name, int nameBuf) const;

	// --- overlay (the [overlay] section) ------------------------------------
	// The Dear ImGui overlay hooks the game's render loop, so it is OPT-IN:
	// `enabled` (env STEAMEMU_OVERLAY wins) defaults false. When off, the whole
	// overlay module stays inert and IsOverlayEnabled() reports false.
	bool OverlayEnabled() const { return m_overlayEnabled; }
	// Toggle shortcut for showing/hiding the overlay (`hotkey`, default the real
	// Steam overlay chord "shift+tab"). Parsed by the overlay module into a
	// virtual-key + modifiers; an unparsable value leaves the overlay with no
	// hotkey (it can still be driven by ActivateGameOverlay*).
	const std::string& OverlayHotkey() const { return m_overlayHotkey; }

	// --- networking overrides (nice-to-have; safe defaults) -----------------
	// Fixed UDP port for the P2P data socket (`listen_port`); 0 => ephemeral
	// (the default, required so two instances on one host don't collide).
	uint16_t ListenPort() const { return m_listenPort; }
	// Multicast discovery group/port (`discovery_address` / `discovery_port`).
	const std::string& DiscoveryAddress() const { return m_discoveryAddr; }
	uint16_t DiscoveryPort() const { return m_discoveryPort; }

	// Path of the steamemu.ini that was actually loaded ("" if none was found).
	const std::string& LoadedPath() const { return m_loadedPath; }

	// A deferred warning about the resolved config ("" if none), emitted by the
	// init path. Resolve() runs during Config()'s first construction and so must
	// not log itself (that would re-enter Config() and deadlock).
	const std::string& Warning() const { return m_warning; }

	// Raw lookup into a parsed section (both lowercased on load). "" if absent.
	std::string GetRaw(const std::string& section, const std::string& key) const;

private:
	friend ConfigData& Config();
	ConfigData();
	void Load(const std::string& path);
	void ParseHttpMock(const std::string& url, const std::string& spec);
	void Resolve();

	struct DlcEntry { uint32_t appID; std::string name; };
	struct HttpMock { std::string urlPrefix; int status; std::string contentType; std::vector<uint8_t> body; };

	// section -> key -> value (keys lowercased except in [http], where URLs keep
	// their case; [dlc] and [http] are also kept in ordered vectors below).
	std::map<std::string, std::map<std::string, std::string>> m_sections;
	std::string m_configDir;    // directory of the loaded ini, for @file resolution
	std::string m_loadedPath;   // full path of the loaded ini ("" if none)
	std::string m_warning;      // deferred config warning, emitted at init

	CSteamID m_steamID;
	std::string m_personaName;
	std::string m_language;
	std::string m_uiLanguage;
	uint32_t m_appID = 0;
	std::string m_country;
	std::string m_logLevel;
	std::string m_logFile;
	std::string m_savePath;
	std::vector<DlcEntry> m_dlc;
	std::vector<HttpMock> m_httpMocks;
	bool m_overlayEnabled = false;
	std::string m_overlayHotkey;
	uint16_t m_listenPort = 0;
	std::string m_discoveryAddr;
	uint16_t m_discoveryPort = 0;
};

// Process-life singleton, loaded on first use.
ConfigData& Config();

} // namespace emu
