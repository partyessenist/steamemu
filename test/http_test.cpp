//====== steamemu ============================================================
// ISteamHTTP behavioral test.
//
// Part 1 (real client): spins up a one-shot HTTP/1.1 server on 127.0.0.1 and
// drives ISteamHTTP over TCP, verifying status/body/headers.
// Part 2 (mocked https): a steamemu.ini [http] section supplies canned responses
// so https:// requests (which we can't really perform without TLS) succeed for
// known endpoints, while an unmocked https:// request still fails.
//============================================================================
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "steam/steam_api.h"

static const char* kBody = "hello-from-steamemu-http";
static std::atomic<int> g_port{0};

static void server_thread() {
	int s = socket(AF_INET, SOCK_STREAM, 0);
	int yes = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
	sockaddr_in addr{}; addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); addr.sin_port = 0;
	bind(s, (sockaddr*)&addr, sizeof(addr));
	listen(s, 1);
	socklen_t sl = sizeof(addr);
	getsockname(s, (sockaddr*)&addr, &sl);
	g_port.store(ntohs(addr.sin_port));

	int c = accept(s, nullptr, nullptr);
	if (c >= 0) {
		char buf[2048];
		recv(c, buf, sizeof(buf), 0);  // read (and ignore) the request
		std::string body = kBody;
		std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: " +
		                   std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
		send(c, resp.data(), resp.size(), 0);
		close(c);
	}
	close(s);
}

// CCallResult waiter for the request-completed async return.
struct CompletionWaiter {
	bool done = false;
	HTTPRequestCompleted_t result{};
	CCallResult<CompletionWaiter, HTTPRequestCompleted_t> cr;
	void OnDone(HTTPRequestCompleted_t* r, bool) { result = *r; done = true; }
	void Wait(SteamAPICall_t h) {
		cr.Set(h, this, &CompletionWaiter::OnDone);
		auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (!done && std::chrono::steady_clock::now() < deadline) {
			SteamAPI_RunCallbacks();
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
	}
};

int main() {
	int fails = 0;

	// A steamemu.ini with [http] mocks -- must be set before SteamAPI_Init so the
	// config singleton picks it up. One inline body and one @file body.
	char tmpl[] = "/tmp/steamemu_http_XXXXXX";
	char* dir = mkdtemp(tmpl);
	if (!dir) { printf("FAIL: mkdtemp\n"); return 1; }
	std::string bodyFile = std::string(dir) + "/profile.json";
	FILE* bf = std::fopen(bodyFile.c_str(), "w");
	std::fprintf(bf, "{\"name\":\"Ada\",\"level\":7}");
	std::fclose(bf);
	std::string ini = std::string(dir) + "/steamemu.ini";
	FILE* cf = std::fopen(ini.c_str(), "w");
	std::fprintf(cf,
		"[http]\n"
		"https://auth.example.com/login = 200 {\"token\":\"abc123\"}\n"
		"https://api.example.com/profile = 200 @profile.json\n");
	std::fclose(cf);
	setenv("STEAMEMU_CONFIG", ini.c_str(), 1);

	std::thread srv(server_thread);
	while (g_port.load() == 0) std::this_thread::sleep_for(std::chrono::milliseconds(5));

	if (!SteamAPI_Init()) { printf("FAIL: init\n"); return 1; }
	ISteamHTTP* http = SteamHTTP();
	if (!http) { printf("FAIL: no ISteamHTTP\n"); return 1; }

	// --- Part 1: real HTTP request over TCP ---------------------------------
	char url[128];
	std::snprintf(url, sizeof(url), "http://127.0.0.1:%d/hello", g_port.load());
	HTTPRequestHandle req = http->CreateHTTPRequest(k_EHTTPMethodGET, url);
	http->SetHTTPRequestHeaderValue(req, "X-Test", "steamemu");

	SteamAPICall_t call = 0;
	bool ok = http->SendHTTPRequest(req, &call);
	printf("SendHTTPRequest ok=%d call=%llu\n", (int)ok, (unsigned long long)call);
	if (!ok) ++fails;
	SteamAPI_RunCallbacks();

	uint32 bodySize = 0;
	http->GetHTTPResponseBodySize(req, &bodySize);
	char body[128] = {0};
	http->GetHTTPResponseBodyData(req, (uint8*)body, sizeof(body) - 1);
	printf("real: body(%u): '%s'\n", bodySize, body);
	if (std::strcmp(body, kBody) != 0) { printf("FAIL: body mismatch\n"); ++fails; }
	if (bodySize != std::strlen(kBody)) { printf("FAIL: body size\n"); ++fails; }
	uint8 hdr[64] = {0};
	if (http->GetHTTPResponseHeaderValue(req, "content-type", hdr, sizeof(hdr)))
		printf("real: content-type: '%s'\n", (char*)hdr);
	http->ReleaseHTTPRequest(req);

	// --- Part 2a: mocked https (inline body), matched via URL prefix + query --
	{
		HTTPRequestHandle r = http->CreateHTTPRequest(
			k_EHTTPMethodGET, "https://auth.example.com/login?user=bob&token=12345");
		CompletionWaiter w;
		SteamAPICall_t c = 0;
		http->SendHTTPRequest(r, &c);
		w.Wait(c);
		char b[128] = {0};
		http->GetHTTPResponseBodyData(r, (uint8*)b, sizeof(b) - 1);
		printf("mock login: done=%d success=%d status=%d body='%s'\n",
		       w.done, w.result.m_bRequestSuccessful, w.result.m_eStatusCode, b);
		if (!w.result.m_bRequestSuccessful) { printf("FAIL: mock not successful\n"); ++fails; }
		if (w.result.m_eStatusCode != 200) { printf("FAIL: mock status != 200\n"); ++fails; }
		if (std::strcmp(b, "{\"token\":\"abc123\"}") != 0) { printf("FAIL: mock body\n"); ++fails; }
		uint8 ct[64] = {0};
		http->GetHTTPResponseHeaderValue(r, "content-type", ct, sizeof(ct));
		if (std::strcmp((char*)ct, "application/json") != 0) { printf("FAIL: mock content-type '%s'\n", (char*)ct); ++fails; }
		http->ReleaseHTTPRequest(r);
	}

	// --- Part 2b: mocked https body loaded from a file ----------------------
	{
		HTTPRequestHandle r = http->CreateHTTPRequest(k_EHTTPMethodGET, "https://api.example.com/profile");
		CompletionWaiter w;
		SteamAPICall_t c = 0;
		http->SendHTTPRequest(r, &c);
		w.Wait(c);
		char b[128] = {0};
		http->GetHTTPResponseBodyData(r, (uint8*)b, sizeof(b) - 1);
		printf("mock profile: status=%d body='%s'\n", w.result.m_eStatusCode, b);
		if (std::strcmp(b, "{\"name\":\"Ada\",\"level\":7}") != 0) { printf("FAIL: @file mock body\n"); ++fails; }
	}

	// --- Part 2c: unmocked https still fails --------------------------------
	{
		HTTPRequestHandle r = http->CreateHTTPRequest(k_EHTTPMethodGET, "https://unmocked.example.com/x");
		CompletionWaiter w;
		SteamAPICall_t c = 0;
		http->SendHTTPRequest(r, &c);
		w.Wait(c);
		printf("unmocked https: success=%d status=%d\n", w.result.m_bRequestSuccessful, w.result.m_eStatusCode);
		if (w.result.m_bRequestSuccessful) { printf("FAIL: unmocked https should fail\n"); ++fails; }
		http->ReleaseHTTPRequest(r);
	}

	SteamAPI_Shutdown();
	srv.join();

	if (fails == 0) { printf("PASS: http_test real HTTP round-trip\n"); return 0; }
	printf("FAILED: http_test (%d)\n", fails);
	return 1;
}
