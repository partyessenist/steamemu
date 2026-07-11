//====== steamemu ============================================================
// Backends for the remaining interfaces so every one carries real behavior:
//   HttpClient   - a real HTTP/1.1 client over TCP (ISteamHTTP)
//   MusicPlayer  - playback state machine (ISteamMusic)
//   ServerStats  - per-user stat/achievement store (ISteamGameServerStats)
//   Shots        - screenshot capture to disk (ISteamScreenshots)
//   Html         - browser handle bookkeeping (ISteamHTMLSurface)
//   Timeline     - event-handle bookkeeping (ISteamTimeline)
//============================================================================
#pragma once

#include "steam/steamclientpublic.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace emu {

// ---------------- ISteamHTTP: real HTTP/1.1 client --------------------------
struct HttpResponse {
	bool done = false;
	bool success = false;
	bool timedOut = false;
	int status = 0;
	std::map<std::string, std::string> headers;  // lowercased keys
	std::vector<uint8_t> body;
};

class HttpClient {
public:
	static HttpClient& Get();

	uint32_t Create(int method, const std::string& url);
	bool SetHeader(uint32_t h, const std::string& k, const std::string& v);
	bool SetParam(uint32_t h, const std::string& k, const std::string& v);
	bool SetContext(uint32_t h, uint64_t ctx);
	bool SetRawBody(uint32_t h, const std::string& contentType, const uint8_t* body, uint32_t len);
	// Perform the request synchronously; fills the response. Returns false for an
	// unknown handle. success/status are in the stored response afterwards.
	bool Send(uint32_t h);
	uint64_t Context(uint32_t h);
	bool GetBodySize(uint32_t h, uint32_t* out);
	bool GetBodyData(uint32_t h, uint8_t* buf, uint32_t bufSize);
	bool GetHeaderSize(uint32_t h, const std::string& name, uint32_t* out);
	bool GetHeaderValue(uint32_t h, const std::string& name, uint8_t* buf, uint32_t bufSize);
	bool WasTimedOut(uint32_t h, bool* out);
	int  Status(uint32_t h);
	bool Release(uint32_t h);

private:
	struct Req {
		int method;
		std::string url;
		std::map<std::string, std::string> headers;
		std::map<std::string, std::string> params;
		std::string rawContentType;
		std::vector<uint8_t> rawBody;
		uint64_t context = 0;
		HttpResponse resp;
	};
	std::map<uint32_t, Req> m_reqs;
	uint32_t m_next = 1;
	HttpClient() = default;
};

// ---------------- ISteamMusic -----------------------------------------------
class MusicPlayer {
public:
	static MusicPlayer& Get();
	bool enabled = true;
	int status = 3;       // AudioPlayback_Idle
	float volume = 1.0f;
	bool playing = false;
	void Play(); void Pause(); void Next(); void Prev();
};

// ---------------- ISteamGameServerStats -------------------------------------
class ServerStats {
public:
	static ServerStats& Get();
	bool GetInt(CSteamID u, const std::string& n, int32_t* out);
	bool GetFloat(CSteamID u, const std::string& n, float* out);
	bool SetInt(CSteamID u, const std::string& n, int32_t v);
	bool SetFloat(CSteamID u, const std::string& n, float v);
	bool GetAch(CSteamID u, const std::string& n, bool* out);
	bool SetAch(CSteamID u, const std::string& n, bool v);
private:
	struct U { std::map<std::string, int32_t> i; std::map<std::string, float> f; std::map<std::string, bool> a; };
	std::map<uint64_t, U> m_users;
	ServerStats() = default;
};

// ---------------- ISteamScreenshots -----------------------------------------
// Save a raw RGB buffer to steamemu_storage/<appid>/screenshots and return a
// handle. Returns 0 on failure.
uint32_t WriteScreenshotRGB(const void* rgb, uint32_t cb, int w, int h);
uint32_t NextScreenshotHandle();

// ---------------- ISteamFriends avatars / ISteamUtils images ----------------
// Games poll GetSmall/Medium/LargeFriendAvatar then GetImageSize/GetImageRGBA
// for every lobby member; returning 0/false leaves permanent spinners or
// invisible player entries. Hand out a stable nonzero handle per (user, size)
// and fill a solid-color placeholder derived from the SteamID. sizeClass:
// 0 = 32x32 (small), 1 = 64x64 (medium), 2 = 184x184 (large).
int AvatarImageHandle(CSteamID user, int sizeClass);
bool AvatarImageSize(int handle, uint32_t* w, uint32_t* h);
bool AvatarImageRGBA(int handle, uint8_t* dst, int dstSize);

// Stable install dir for ISteamApps::GetAppInstallDir (the cwd: games locate
// content/config with it, and we run from the game's directory).
const char* AppInstallDirC();

// ---------------- ISteamHTMLSurface / ISteamTimeline handle allocators ------
uint32_t NextBrowserHandle();
uint64_t NextTimelineEvent();
// Fake-but-unique UGC content handle (FileShare and friends promise one).
uint64_t NextUGCHandle();

// Free accessors used by the generated interface bodies.
inline HttpClient& Http() { return HttpClient::Get(); }
inline MusicPlayer& Music() { return MusicPlayer::Get(); }
inline ServerStats& SvStats() { return ServerStats::Get(); }

} // namespace emu
