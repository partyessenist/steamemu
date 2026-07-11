//====== steamemu ============================================================
// Configuration subsystem. See config.h.
//============================================================================
#include "config.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace emu {
namespace {

// Directory containing our own shared library (steam_api64.dll / libsteam_api.so).
// The game may run with a different working directory than where the user dropped
// steam_api64.dll + steamemu.ini, so we also look for config next to ourselves.
std::string ModuleDir() {
	std::string path;
#if defined(_WIN32)
	HMODULE hm = nullptr;
	if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
	                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                       reinterpret_cast<LPCSTR>(&ModuleDir), &hm) && hm) {
		char buf[1024];
		DWORD n = GetModuleFileNameA(hm, buf, sizeof(buf));
		if (n > 0 && n < sizeof(buf)) path.assign(buf, n);
	}
#else
	Dl_info info;
	if (dladdr(reinterpret_cast<void*>(&ModuleDir), &info) && info.dli_fname)
		path = info.dli_fname;
#endif
	size_t slash = path.find_last_of("/\\");
	return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

bool FileReadable(const std::string& path) {
	if (path.empty()) return false;
	if (FILE* f = std::fopen(path.c_str(), "rb")) { std::fclose(f); return true; }
	return false;
}

bool IsAbsolute(const std::string& p) {
	if (p.empty()) return false;
	if (p[0] == '/' || p[0] == '\\') return true;
	return p.size() >= 2 && p[1] == ':';   // Windows drive letter
}

std::string Env(const char* key) {
	const char* v = std::getenv(key);
	return v ? std::string(v) : std::string();
}

std::string Trim(const std::string& s) {
	size_t b = 0, e = s.size();
	while (b < e && std::isspace((unsigned char)s[b])) ++b;
	while (e > b && std::isspace((unsigned char)s[e - 1])) --e;
	return s.substr(b, e - b);
}

std::string Lower(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(),
	               [](unsigned char c) { return (char)std::tolower(c); });
	return s;
}

// Read a whole file into a byte vector. Returns false if it can't be opened.
bool ReadFile(const std::string& path, std::vector<uint8_t>& out) {
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f) return false;
	std::fseek(f, 0, SEEK_END);
	long n = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	if (n > 0) {
		out.resize((size_t)n);
		size_t rd = std::fread(out.data(), 1, (size_t)n, f);
		out.resize(rd);
	}
	std::fclose(f);
	return true;
}

// Guess a Content-Type from the body so JSON auth responses come back typed.
std::string GuessContentType(const std::vector<uint8_t>& body) {
	size_t i = 0;
	while (i < body.size() && std::isspace(body[i])) ++i;
	if (i < body.size() && (body[i] == '{' || body[i] == '[')) return "application/json";
	if (i < body.size() && body[i] == '<') return "text/html";
	return "text/plain";
}

// AppID from steam_appid.txt in the cwd (development fallback). 0 if absent.
uint32_t ReadAppIDFromFile() {
	if (FILE* f = std::fopen("steam_appid.txt", "r")) {
		unsigned long id = 0;
		if (std::fscanf(f, "%lu", &id) != 1) id = 0;
		std::fclose(f);
		return (uint32_t)id;
	}
	return 0;
}

} // namespace

std::string ConfigData::GetRaw(const std::string& section, const std::string& key) const {
	auto s = m_sections.find(section);
	if (s == m_sections.end()) return std::string();
	auto k = s->second.find(key);
	return k == s->second.end() ? std::string() : k->second;
}

void ConfigData::Load(const std::string& path) {
	// Remember the ini's directory so [http] @file bodies resolve relative to it.
	size_t slash = path.find_last_of("/\\");
	m_configDir = slash == std::string::npos ? std::string() : path.substr(0, slash);

	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f) return;
	char raw[8192];
	std::string section;  // "" is the top-level (unsectioned) block
	while (std::fgets(raw, sizeof(raw), f)) {
		std::string line = Trim(raw);
		if (line.empty() || line[0] == '#' || line[0] == ';') continue;
		if (line.front() == '[' && line.back() == ']') {
			section = Lower(Trim(line.substr(1, line.size() - 2)));
			continue;
		}
		size_t eq = line.find('=');
		if (eq == std::string::npos) continue;
		// URLs in [http] are case-sensitive; every other section lowercases keys.
		std::string rawKey = Trim(line.substr(0, eq));
		std::string key = section == "http" ? rawKey : Lower(rawKey);
		std::string val = Trim(line.substr(eq + 1));
		if (key.empty()) continue;
		m_sections[section][key] = val;
		if (section == "dlc") {
			// [dlc] entries are `<appid> = <name>`, kept in file order for
			// stable BGetDLCDataByIndex enumeration.
			uint32_t appID = (uint32_t)std::strtoul(key.c_str(), nullptr, 10);
			if (appID != 0) m_dlc.push_back({appID, val});
		} else if (section == "http") {
			ParseHttpMock(key, val);
		}
	}
	std::fclose(f);
}

void ConfigData::ParseHttpMock(const std::string& url, const std::string& spec) {
	// spec = "<status> [body]", where body is inline text or "@file" (resolved
	// relative to the ini). "<status>" alone yields an empty body.
	HttpMock m;
	m.urlPrefix = url;
	size_t sp = spec.find(' ');
	std::string statusTok = sp == std::string::npos ? spec : spec.substr(0, sp);
	m.status = std::atoi(statusTok.c_str());
	std::string bodySpec = sp == std::string::npos ? std::string() : Trim(spec.substr(sp + 1));
	if (!bodySpec.empty() && bodySpec[0] == '@') {
		std::string file = bodySpec.substr(1);
		if (!file.empty() && file[0] != '/' && !m_configDir.empty())
			file = m_configDir + "/" + file;
		ReadFile(file, m.body);   // missing file => empty body (still a valid mock)
	} else {
		m.body.assign(bodySpec.begin(), bodySpec.end());
	}
	m.contentType = GuessContentType(m.body);
	m_httpMocks.push_back(std::move(m));
}

bool ConfigData::MockHttp(const std::string& url, int* status, std::string* contentType,
                          std::vector<uint8_t>* body) const {
	const HttpMock* best = nullptr;
	for (const auto& m : m_httpMocks) {
		if (url.size() >= m.urlPrefix.size() &&
		    url.compare(0, m.urlPrefix.size(), m.urlPrefix) == 0) {
			if (!best || m.urlPrefix.size() > best->urlPrefix.size()) best = &m;
		}
	}
	if (!best) return false;
	if (status) *status = best->status;
	if (contentType) *contentType = best->contentType;
	if (body) *body = best->body;
	return true;
}

void ConfigData::Resolve() {
	auto top = [&](const char* k) { return GetRaw("", k); };

	// -- identity: env overrides file, for back-compat with existing setups.
	std::string sid = Env("STEAMEMU_STEAMID");
	if (sid.empty()) sid = top("steam_id");
	if (sid.empty()) sid = top("account_id");
	if (!sid.empty()) {
		uint64_t v = std::strtoull(sid.c_str(), nullptr, 10);
		if (v >= (uint64_t(1) << 32))
			m_steamID = CSteamID((uint64)v);                 // full 64-bit SteamID
		else
			m_steamID = CSteamID((uint32)v, k_EUniversePublic, k_EAccountTypeIndividual);
	} else {
		m_steamID = CSteamID((uint32)1000001, k_EUniversePublic, k_EAccountTypeIndividual);
	}

	// Reject an account id of 0: it is the "nobody" base SteamID (e.g. the literal
	// 76561197960265728), structurally an individual account but owned by no user.
	// Some games validate this and refuse to start, so fall back to the default
	// account rather than hand out an id the game will reject. Warn (deferred) with
	// the value we ignored so the misconfiguration is visible in the log.
	if (m_steamID.GetAccountID() == 0) {
		char buf[160];
		std::snprintf(buf, sizeof(buf),
		              "steam_id/account_id resolved to account id 0 (%llu, a 'nobody' "
		              "SteamID); using default account 1000001 instead",
		              (unsigned long long)m_steamID.ConvertToUint64());
		m_warning = buf;
		m_steamID = CSteamID((uint32)1000001, k_EUniversePublic, k_EAccountTypeIndividual);
	}

	m_personaName = Env("STEAMEMU_NAME");
	if (m_personaName.empty()) m_personaName = top("persona_name");
	if (m_personaName.empty()) m_personaName = "Player";

	m_language = top("language");
	if (m_language.empty()) m_language = "english";
	m_uiLanguage = top("ui_language");
	if (m_uiLanguage.empty()) m_uiLanguage = m_language;

	std::string app = Env("SteamAppId");
	if (app.empty()) app = Env("SteamGameId");
	if (app.empty()) app = top("app_id");
	if (!app.empty()) m_appID = (uint32_t)std::strtoul(app.c_str(), nullptr, 10);
	else m_appID = ReadAppIDFromFile();

	m_country = top("country");
	if (m_country.empty()) m_country = "US";

	// Log verbosity + file (env STEAMEMU_LOG_LEVEL / STEAMEMU_LOG take precedence,
	// applied in emu.cpp).
	m_logLevel = top("log_level");
	if (m_logLevel.empty()) m_logLevel = "info";
	m_logFile = top("log_file");
	// A relative log_file lands next to the config file (not the game's unknown
	// cwd), so the user can actually find it.
	if (!m_logFile.empty() && !IsAbsolute(m_logFile) && !m_configDir.empty())
		m_logFile = m_configDir + "/" + m_logFile;

	m_savePath = top("save_path");
	if (m_savePath.empty()) m_savePath = top("storage_dir");

	// -- networking overrides from the [net] section (defaults match net.cpp's
	// built-in constants). Accepted at the top level too, for convenience.
	auto net = [&](const char* k) {
		std::string v = GetRaw("net", k);
		return v.empty() ? top(k) : v;
	};
	std::string lp = net("listen_port");
	m_listenPort = lp.empty() ? 0 : (uint16_t)std::strtoul(lp.c_str(), nullptr, 10);
	m_discoveryAddr = net("discovery_address");
	if (m_discoveryAddr.empty()) m_discoveryAddr = "239.198.7.4";
	std::string dp = net("discovery_port");
	m_discoveryPort = dp.empty() ? (uint16_t)47854 : (uint16_t)std::strtoul(dp.c_str(), nullptr, 10);
}

bool ConfigData::HasDLC(uint32_t appID) const {
	for (const auto& d : m_dlc)
		if (d.appID == appID) return true;
	return false;
}

bool ConfigData::DLCByIndex(int i, uint32_t* appID, bool* available, char* name, int nameBuf) const {
	if (i < 0 || i >= (int)m_dlc.size()) return false;
	const DlcEntry& d = m_dlc[i];
	if (appID) *appID = d.appID;
	if (available) *available = true;   // configured DLC is "installed"
	if (name && nameBuf > 0) {
		std::snprintf(name, (size_t)nameBuf, "%s", d.name.c_str());
	}
	return true;
}

ConfigData::ConfigData() {
	// Resolution order: STEAMEMU_CONFIG (explicit), then steamemu.ini in the cwd,
	// then steamemu.ini next to our own module -- so a game launched with a
	// different working directory still finds the file dropped next to the DLL.
	std::string path = Env("STEAMEMU_CONFIG");
	if (path.empty() && FileReadable("steamemu.ini")) path = "steamemu.ini";
	if (path.empty()) {
		std::string md = ModuleDir();
		if (!md.empty() && FileReadable(md + "/steamemu.ini")) path = md + "/steamemu.ini";
	}
	if (!path.empty() && FileReadable(path)) {
		Load(path);
		m_loadedPath = path;
	}
	Resolve();
}

ConfigData& Config() {
	static ConfigData s;
	return s;
}

} // namespace emu
