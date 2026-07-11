//====== steamemu ============================================================
// Emulator core services: fabricated identity, AppID resolution, config, and
// the call log. Kept free of any Steam interface so it can be shared by the
// generated interface classes, the networking backend, and the entry points.
//============================================================================
#include "emu_common.h"
#include "config.h"

#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <set>
#include <string>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#define EMU_GETPID _getpid
#else
#include <cstdlib>
#include <unistd.h>
#define EMU_GETPID getpid
#endif

namespace emu {

// Identity, persona and AppID all funnel through the config subsystem, which
// keeps env-var precedence (STEAMEMU_STEAMID / STEAMEMU_NAME / SteamAppId) for
// back-compat and adds steamemu.ini keys on top. See config.{h,cpp}.
CSteamID LocalSteamID() { return Config().SteamID(); }

CSteamID LocalGSSteamID() {
	return CSteamID(LocalSteamID().GetAccountID(), k_EUniversePublic, k_EAccountTypeGameServer);
}

const char* PersonaName() { return Config().PersonaName().c_str(); }

uint32 AppID() { return Config().AppID(); }

namespace {

void SetEnv(const char* name, const char* value) {
#if defined(_WIN32)
	SetEnvironmentVariableA(name, value);
	// The game's CRT snapshots the environment at startup; _putenv_s updates the
	// CRT copy too so getenv() (not just GetEnvironmentVariable) sees the value.
	_putenv_s(name, value);
#else
	setenv(name, value, 1);
#endif
}

// Directory of the running executable (the game), trailing separator included.
std::string ExecutableDir() {
	char buf[1024] = {};
#if defined(_WIN32)
	DWORD n = GetModuleFileNameA(nullptr, buf, sizeof(buf));
	if (n == 0 || n >= sizeof(buf)) return {};
#else
	ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
	if (n <= 0) return {};
	buf[n] = '\0';
#endif
	std::string path(buf);
	size_t slash = path.find_last_of("/\\");
	return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

} // namespace

// A game launched by the real Steam client inherits a set of environment
// variables, and some games trust those over the API: Arma 3's server compares
// getenv("SteamAppId") against its known appid ("Current Steam AppId doesn't
// match expected value") and its client reports the Steam layer disabled
// without SteamEnv/SteamClientLaunch. 
void PublishSteamEnvironment() {
	if (uint32 appid = AppID()) {
		char s[16];
		std::snprintf(s, sizeof(s), "%u", appid);
		SetEnv("SteamAppId", s);
		SetEnv("SteamGameId", s);
		SetEnv("SteamOverlayGameId", s);
	}
	SetEnv("SteamAppUser", PersonaName());
	SetEnv("SteamUser", PersonaName());
	SetEnv("SteamClientLaunch", "1");
	SetEnv("SteamEnv", "1");
	// Only if absent -- a real client launch (or the user) may have set it.
	if (!std::getenv("SteamPath")) {
		std::string dir = ExecutableDir();
		if (!dir.empty()) SetEnv("SteamPath", dir.c_str());
	}
}

uint32 UnixTime() {
	return static_cast<uint32>(std::time(nullptr));
}

bool FirstUserInfoRequest(CSteamID id) {
	static std::mutex m;
	static std::set<uint64> seen;
	std::lock_guard<std::mutex> lk(m);
	return seen.insert(id.ConvertToUint64()).second;
}

namespace {

LogLevel ParseLevel(std::string s) {
	for (char& c : s) c = (char)std::tolower((unsigned char)c);
	if (s == "off" || s == "none" || s == "0") return LogLevel::Off;
	if (s == "error" || s == "err" || s == "1") return LogLevel::Error;
	if (s == "warn" || s == "warning" || s == "2") return LogLevel::Warn;
	if (s == "info" || s == "3") return LogLevel::Info;
	if (s == "debug" || s == "4") return LogLevel::Debug;
	if (s == "trace" || s == "verbose" || s == "5") return LogLevel::Trace;
	return LogLevel::Info;   // default
}

const char* LevelTag(LogLevel l) {
	switch (l) {
		case LogLevel::Error: return "error";
		case LogLevel::Warn:  return "warn";
		case LogLevel::Info:  return "info";
		case LogLevel::Debug: return "debug";
		case LogLevel::Trace: return "trace";
		default:              return "";
	}
}

std::mutex& LogMutex() { static std::mutex m; return m; }

FILE* LogFile() {
	static FILE* f = [] () -> FILE* {
		// Path from STEAMEMU_LOG (env, wins) or `log_file` in steamemu.ini -- the
		// config path works even when env vars don't reach the process (e.g. an
		// anti-cheat relaunch).
		std::string path;
		if (const char* env = std::getenv("STEAMEMU_LOG")) path = env;
		else path = Config().LogFileName();
		if (path.empty()) return nullptr;
		// Suffix the PID so a launcher, anti-cheat and the game process each get
		// their OWN file instead of truncating a shared one. Open the biggest.
		char full[1200];
		std::snprintf(full, sizeof(full), "%s.%ld", path.c_str(), (long)EMU_GETPID());
		FILE* fp = std::fopen(full, "w");
		if (fp) {
			std::fprintf(fp, "[steamemu] log start: pid=%ld appid=%u user='%s'\n",
			             (long)EMU_GETPID(), AppID(), PersonaName());
			std::fflush(fp);
		}
		return fp;
	}();
	return f;
}

// Wall-clock HH:MM:SS.mmm, so timing is visible in the log (e.g. how long a
// process ran, or the gap between calls while a menu is open).
void Timestamp(char* out, size_t n) {
	using namespace std::chrono;
	auto now = system_clock::now();
	std::time_t t = system_clock::to_time_t(now);
	int ms = (int)(duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000);
	std::tm tmv{};
#if defined(_WIN32)
	localtime_s(&tmv, &t);
#else
	localtime_r(&t, &tmv);
#endif
	std::snprintf(out, n, "%02d:%02d:%02d.%03d",
	              tmv.tm_hour, tmv.tm_min, tmv.tm_sec, ms);
}

// Emit one already-formatted line (timestamped) to stderr and the log file.
void Emit(const char* line) {
	char ts[16];
	Timestamp(ts, sizeof(ts));
	std::lock_guard<std::mutex> lk(LogMutex());
	std::fprintf(stderr, "%s %s\n", ts, line);
	if (FILE* f = LogFile()) { std::fprintf(f, "%s %s\n", ts, line); std::fflush(f); }
}

} // namespace

LogLevel CurrentLogLevel() {
	// Resolved once: env STEAMEMU_LOG_LEVEL wins, else config log_level, else Info.
	static LogLevel level = [] {
		if (const char* env = std::getenv("STEAMEMU_LOG_LEVEL"))
			return ParseLevel(env);
		return ParseLevel(Config().LogLevelName());
	}();
	return level;
}

void LogAt(LogLevel level, const char* fmt, ...) {
	if (level > CurrentLogLevel()) return;   // below threshold: suppressed
	char msg[1024];
	va_list ap;
	va_start(ap, fmt);
	std::vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	char line[1152];
	std::snprintf(line, sizeof(line), "[steamemu][%s] %s", LevelTag(level), msg);
	Emit(line);
}

void Log(const char *method) {
	// Per-method announce: silent below debug, deduped at debug (once per method
	// = coverage view), and EVERY call at trace (so the exact sequence during a
	// menu/flow is visible for diagnosis).
	LogLevel lvl = CurrentLogLevel();
	if (LogLevel::Debug > lvl) return;
	if (lvl < LogLevel::Trace) {
		static std::set<std::string> seen;
		std::lock_guard<std::mutex> lk(LogMutex());
		if (!seen.insert(method).second) return;  // already announced this method
	}
	char line[512];
	std::snprintf(line, sizeof(line), "[steamemu] %s", method);
	Emit(line);
}

} // namespace emu
