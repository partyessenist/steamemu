//====== steamemu ============================================================
// Backends for the remaining interfaces. See services.h. The HTTP client is a
// real (blocking) HTTP/1.1 implementation over TCP; https is reported as a
// failure (no TLS in this build). POSIX sockets; this host is Linux.
//============================================================================
#include "services.h"
#include "emu_common.h"
#include "config.h"
#include "storage.h"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sys/stat.h>

#if defined(_WIN32)
#include <direct.h>
#else
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include <map>

namespace emu {
namespace {
std::mutex g_httpMtx;

// Parse "http://host[:port]/path" -> (host, port, path). Returns false for a
// non-http scheme (e.g. https, which we cannot do without TLS).
bool ParseUrl(const std::string& url, std::string& host, int& port, std::string& path) {
	const std::string pfx = "http://";
	if (url.compare(0, pfx.size(), pfx) != 0) return false;
	std::string rest = url.substr(pfx.size());
	size_t slash = rest.find('/');
	std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
	path = slash == std::string::npos ? "/" : rest.substr(slash);
	size_t colon = hostport.find(':');
	if (colon == std::string::npos) { host = hostport; port = 80; }
	else { host = hostport.substr(0, colon); port = std::atoi(hostport.c_str() + colon + 1); }
	return !host.empty();
}

std::string Lower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }
} // namespace

HttpClient& HttpClient::Get() { static HttpClient c; return c; }

uint32_t HttpClient::Create(int method, const std::string& url) {
	std::lock_guard<std::mutex> lk(g_httpMtx);
	uint32_t h = m_next++;
	Req r; r.method = method; r.url = url;
	m_reqs[h] = std::move(r);
	return h;
}
bool HttpClient::SetHeader(uint32_t h, const std::string& k, const std::string& v) {
	std::lock_guard<std::mutex> lk(g_httpMtx);
	auto it = m_reqs.find(h); if (it == m_reqs.end()) return false;
	it->second.headers[k] = v; return true;
}
bool HttpClient::SetParam(uint32_t h, const std::string& k, const std::string& v) {
	std::lock_guard<std::mutex> lk(g_httpMtx);
	auto it = m_reqs.find(h); if (it == m_reqs.end()) return false;
	it->second.params[k] = v; return true;
}
bool HttpClient::SetContext(uint32_t h, uint64_t ctx) {
	std::lock_guard<std::mutex> lk(g_httpMtx);
	auto it = m_reqs.find(h); if (it == m_reqs.end()) return false;
	it->second.context = ctx; return true;
}
bool HttpClient::SetRawBody(uint32_t h, const std::string& ct, const uint8_t* body, uint32_t len) {
	std::lock_guard<std::mutex> lk(g_httpMtx);
	auto it = m_reqs.find(h); if (it == m_reqs.end()) return false;
	it->second.rawContentType = ct;
	it->second.rawBody.assign(body, body + len);
	return true;
}
uint64_t HttpClient::Context(uint32_t h) {
	std::lock_guard<std::mutex> lk(g_httpMtx);
	auto it = m_reqs.find(h); return it == m_reqs.end() ? 0 : it->second.context;
}

bool HttpClient::Send(uint32_t h) {
	Req req;
	{
		std::lock_guard<std::mutex> lk(g_httpMtx);
		auto it = m_reqs.find(h); if (it == m_reqs.end()) return false;
		req = it->second;  // snapshot; perform outside the lock
	}
	HttpResponse resp;

	// Configured mock response first: serve canned status/body with no socket or
	// TLS. This is how https:// (which we can't really perform without a TLS lib)
	// is satisfied for endpoints the game knows -- see [http] in steamemu.ini.
	{
		int ms = 0; std::string mct; std::vector<uint8_t> mbody;
		if (Config().MockHttp(req.url, &ms, &mct, &mbody)) {
			resp.status = ms;
			resp.success = ms != 0;
			resp.body = std::move(mbody);
			if (!mct.empty()) resp.headers["content-type"] = mct;
			resp.headers["content-length"] = std::to_string(resp.body.size());
			resp.done = true;
			std::lock_guard<std::mutex> lk(g_httpMtx);
			auto it = m_reqs.find(h);
			if (it != m_reqs.end()) it->second.resp = std::move(resp);
			return true;
		}
	}

#if defined(_WIN32)
	// No real HTTP client in the Windows build; unmocked requests just fail.
	resp.done = true;
	{
		std::lock_guard<std::mutex> lk(g_httpMtx);
		auto it = m_reqs.find(h);
		if (it != m_reqs.end()) it->second.resp = std::move(resp);
	}
	return true;
#else
	std::string host, path; int port = 80;
	if (ParseUrl(req.url, host, port, path)) {
		// Build the query string / body.
		std::string query;
		for (auto& p : req.params) {
			if (!query.empty()) query += "&";
			query += p.first + "=" + p.second;  // callers pass pre-encoded values
		}
		bool post = req.method == 3 /*k_EHTTPMethodPOST*/ || req.method == 4 /*PUT*/;
		std::string body;
		std::string contentType = req.rawContentType;
		if (!req.rawBody.empty()) body.assign(req.rawBody.begin(), req.rawBody.end());
		else if (post && !query.empty()) { body = query; if (contentType.empty()) contentType = "application/x-www-form-urlencoded"; }
		else if (!query.empty()) path += (path.find('?') == std::string::npos ? "?" : "&") + query;

		const char* verb = post ? (req.method == 4 ? "PUT" : "POST")
		                         : (req.method == 2 ? "HEAD" : req.method == 5 ? "DELETE" : "GET");
		std::string httpReq = std::string(verb) + " " + path + " HTTP/1.1\r\n";
		httpReq += "Host: " + host + "\r\n";
		httpReq += "Connection: close\r\n";
		for (auto& hd : req.headers) httpReq += hd.first + ": " + hd.second + "\r\n";
		if (!body.empty()) {
			if (!contentType.empty()) httpReq += "Content-Type: " + contentType + "\r\n";
			httpReq += "Content-Length: " + std::to_string(body.size()) + "\r\n";
		}
		httpReq += "\r\n" + body;

		// Resolve + connect with a timeout.
		addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
		addrinfo* res = nullptr;
		if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) == 0 && res) {
			int s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
			if (s >= 0) {
				timeval tv{5, 0};
				setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
				setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
				if (connect(s, res->ai_addr, res->ai_addrlen) == 0) {
					send(s, httpReq.data(), httpReq.size(), 0);
					std::string raw;
					char buf[4096];
					for (;;) {
						int n = recv(s, buf, sizeof(buf), 0);
						if (n <= 0) break;
						raw.append(buf, n);
					}
					// Parse status line + headers + body.
					size_t hdrEnd = raw.find("\r\n\r\n");
					if (hdrEnd != std::string::npos) {
						std::string head = raw.substr(0, hdrEnd);
						std::string bodyStr = raw.substr(hdrEnd + 4);
						size_t sp = head.find(' ');
						if (sp != std::string::npos) resp.status = std::atoi(head.c_str() + sp + 1);
						size_t pos = head.find("\r\n");
						while (pos != std::string::npos) {
							size_t next = head.find("\r\n", pos + 2);
							std::string line = head.substr(pos + 2, (next == std::string::npos ? head.size() : next) - pos - 2);
							size_t c = line.find(':');
							if (c != std::string::npos) {
								std::string k = Lower(line.substr(0, c));
								size_t vs = line.find_first_not_of(' ', c + 1);
								resp.headers[k] = vs == std::string::npos ? "" : line.substr(vs);
							}
							pos = next;
						}
						resp.body.assign(bodyStr.begin(), bodyStr.end());
						resp.success = resp.status != 0;
					}
					if (resp.status == 0) { resp.timedOut = raw.empty(); }
				} else {
					resp.timedOut = true;
				}
				close(s);
			}
			freeaddrinfo(res);
		}
	}
	resp.done = true;
	{
		std::lock_guard<std::mutex> lk(g_httpMtx);
		auto it = m_reqs.find(h);
		if (it != m_reqs.end()) it->second.resp = std::move(resp);
	}
	return true;
#endif
}

bool HttpClient::GetBodySize(uint32_t h, uint32_t* out) {
	std::lock_guard<std::mutex> lk(g_httpMtx);
	auto it = m_reqs.find(h); if (it == m_reqs.end()) return false;
	if (out) *out = (uint32_t)it->second.resp.body.size();
	return true;
}
bool HttpClient::GetBodyData(uint32_t h, uint8_t* buf, uint32_t bufSize) {
	std::lock_guard<std::mutex> lk(g_httpMtx);
	auto it = m_reqs.find(h); if (it == m_reqs.end() || !buf) return false;
	uint32_t n = (uint32_t)it->second.resp.body.size();
	if (n > bufSize) n = bufSize;
	std::memcpy(buf, it->second.resp.body.data(), n);
	return true;
}
bool HttpClient::GetHeaderSize(uint32_t h, const std::string& name, uint32_t* out) {
	std::lock_guard<std::mutex> lk(g_httpMtx);
	auto it = m_reqs.find(h); if (it == m_reqs.end()) return false;
	auto hd = it->second.resp.headers.find(Lower(name));
	if (out) *out = hd == it->second.resp.headers.end() ? 0 : (uint32_t)hd->second.size() + 1;
	return hd != it->second.resp.headers.end();
}
bool HttpClient::GetHeaderValue(uint32_t h, const std::string& name, uint8_t* buf, uint32_t bufSize) {
	std::lock_guard<std::mutex> lk(g_httpMtx);
	auto it = m_reqs.find(h); if (it == m_reqs.end() || !buf) return false;
	auto hd = it->second.resp.headers.find(Lower(name));
	if (hd == it->second.resp.headers.end()) return false;
	std::snprintf((char*)buf, bufSize, "%s", hd->second.c_str());
	return true;
}
bool HttpClient::WasTimedOut(uint32_t h, bool* out) {
	std::lock_guard<std::mutex> lk(g_httpMtx);
	auto it = m_reqs.find(h); if (it == m_reqs.end()) return false;
	if (out) *out = it->second.resp.timedOut;
	return true;
}
int HttpClient::Status(uint32_t h) {
	std::lock_guard<std::mutex> lk(g_httpMtx);
	auto it = m_reqs.find(h); return it == m_reqs.end() ? 0 : it->second.resp.status;
}
bool HttpClient::Release(uint32_t h) {
	std::lock_guard<std::mutex> lk(g_httpMtx);
	return m_reqs.erase(h) != 0;
}

// ---------------- MusicPlayer -----------------------------------------------
MusicPlayer& MusicPlayer::Get() { static MusicPlayer m; return m; }
void MusicPlayer::Play() { playing = true; status = 1; }
void MusicPlayer::Pause() { playing = false; status = 2; }
void MusicPlayer::Next() { status = 1; playing = true; }
void MusicPlayer::Prev() { status = 1; playing = true; }

// ---------------- ServerStats -----------------------------------------------
ServerStats& ServerStats::Get() { static ServerStats s; return s; }
bool ServerStats::GetInt(CSteamID u, const std::string& n, int32_t* out) {
	auto& m = m_users[u.ConvertToUint64()].i; auto it = m.find(n);
	if (out) *out = it == m.end() ? 0 : it->second; return it != m.end();
}
bool ServerStats::GetFloat(CSteamID u, const std::string& n, float* out) {
	auto& m = m_users[u.ConvertToUint64()].f; auto it = m.find(n);
	if (out) *out = it == m.end() ? 0.f : it->second; return it != m.end();
}
bool ServerStats::SetInt(CSteamID u, const std::string& n, int32_t v) { m_users[u.ConvertToUint64()].i[n] = v; return true; }
bool ServerStats::SetFloat(CSteamID u, const std::string& n, float v) { m_users[u.ConvertToUint64()].f[n] = v; return true; }
bool ServerStats::GetAch(CSteamID u, const std::string& n, bool* out) {
	auto& m = m_users[u.ConvertToUint64()].a; auto it = m.find(n);
	if (out) *out = it != m.end() && it->second; return true;
}
bool ServerStats::SetAch(CSteamID u, const std::string& n, bool v) { m_users[u.ConvertToUint64()].a[n] = v; return true; }

// ---------------- Screenshots / handle allocators ---------------------------
static std::atomic<uint32_t> g_shotHandle{1};
static std::atomic<uint32_t> g_browserHandle{1};
static std::atomic<uint64_t> g_timelineEvent{1};
static std::atomic<uint64_t> g_ugcHandle{1};

uint32_t NextScreenshotHandle() { return g_shotHandle.fetch_add(1); }
uint32_t NextBrowserHandle() { return g_browserHandle.fetch_add(1); }
uint64_t NextTimelineEvent() { return g_timelineEvent.fetch_add(1); }
uint64_t NextUGCHandle() { return g_ugcHandle.fetch_add(1); }

// ------------------------- avatar / image registry --------------------------
namespace {
std::mutex g_imgMtx;
std::map<std::pair<uint64_t, int>, int> g_imgByUser;   // (steamid, sizeClass) -> handle
std::map<int, std::pair<uint64_t, int>> g_imgInfo;     // handle -> (steamid, sizeClass)
int g_nextImg = 1;
int ImageDim(int sizeClass) { return sizeClass >= 2 ? 184 : (sizeClass == 1 ? 64 : 32); }
} // namespace

int AvatarImageHandle(CSteamID user, int sizeClass) {
	if (sizeClass < 0) sizeClass = 0;
	if (sizeClass > 2) sizeClass = 2;
	std::lock_guard<std::mutex> lk(g_imgMtx);
	auto key = std::make_pair(user.ConvertToUint64(), sizeClass);
	auto it = g_imgByUser.find(key);
	if (it != g_imgByUser.end()) return it->second;
	int h = g_nextImg++;
	g_imgByUser[key] = h;
	g_imgInfo[h] = key;
	return h;
}

bool AvatarImageSize(int handle, uint32_t* w, uint32_t* h) {
	std::lock_guard<std::mutex> lk(g_imgMtx);
	auto it = g_imgInfo.find(handle);
	if (it == g_imgInfo.end()) return false;
	uint32_t d = (uint32_t)ImageDim(it->second.second);
	if (w) *w = d;
	if (h) *h = d;
	return true;
}

bool AvatarImageRGBA(int handle, uint8_t* dst, int dstSize) {
	uint64_t id;
	int dim;
	{
		std::lock_guard<std::mutex> lk(g_imgMtx);
		auto it = g_imgInfo.find(handle);
		if (it == g_imgInfo.end()) return false;
		id = it->second.first;
		dim = ImageDim(it->second.second);
	}
	if (!dst || dstSize < dim * dim * 4) return false;
	// Deterministic per-user placeholder color (kept bright enough to see).
	uint8_t r = 64 + (uint8_t)(id % 191), g = 64 + (uint8_t)((id >> 8) % 191),
	        b = 64 + (uint8_t)((id >> 16) % 191);
	for (int i = 0; i < dim * dim; ++i) {
		dst[i * 4 + 0] = r;
		dst[i * 4 + 1] = g;
		dst[i * 4 + 2] = b;
		dst[i * 4 + 3] = 255;
	}
	return true;
}

const char* AppInstallDirC() {
	static std::string dir = [] {
		char buf[4096] = {};
#if defined(_WIN32)
		if (!_getcwd(buf, sizeof(buf))) buf[0] = 0;
#else
		if (!getcwd(buf, sizeof(buf))) buf[0] = 0;
#endif
		return std::string(buf[0] ? buf : ".");
	}();
	return dir.c_str();
}

uint32_t WriteScreenshotRGB(const void* rgb, uint32_t cb, int w, int h) {
	uint32_t handle = NextScreenshotHandle();
	std::string dir = StorageRoot() + "/screenshots";
	char name[128];
	std::snprintf(name, sizeof(name), "screenshots/shot_%u_%dx%d.rgb", handle, w, h);
	// Reuse the storage layer's remote dir writer via a direct file under root.
	std::string path = StorageRoot() + "/" + name;
	// Ensure the screenshots subdir exists by writing through StorageBackend-like mkdir.
#if !defined(_WIN32)
	std::string sd = StorageRoot() + "/screenshots";
	::mkdir(sd.c_str(), 0755);
#endif
	if (FILE* f = std::fopen(path.c_str(), "wb")) {
		if (cb) std::fwrite(rgb, 1, cb, f);
		std::fclose(f);
		return handle;
	}
	return 0;
}

} // namespace emu
