//====== steamemu ============================================================
// Two-instance ISteamNetworkingSockets test (modern connection-oriented P2P).
//
// server: CreateListenSocketP2P, AcceptConnection on the incoming-connection
//         callback, then ReceiveMessagesOnConnection and verify the payload.
// client: discover the server, ConnectP2P, and on the Connected callback send a
//         message.
// Proves the connect handshake + status callbacks + message delivery end-to-end.
//============================================================================
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <thread>

#include <sys/wait.h>
#include <unistd.h>

#include "steam/steam_api.h"

static const char* kMsg = "ping-sockets";

struct SockListener {
	ISteamNetworkingSockets* ns = nullptr;
	HSteamNetConnection conn = k_HSteamNetConnection_Invalid;
	bool connected = false;
	STEAM_CALLBACK(SockListener, OnStatus, SteamNetConnectionStatusChangedCallback_t);
};
void SockListener::OnStatus(SteamNetConnectionStatusChangedCallback_t* p) {
	if (p->m_info.m_eState == k_ESteamNetworkingConnectionState_Connecting &&
	    p->m_info.m_hListenSocket != 0) {
		ns->AcceptConnection(p->m_hConn);      // inbound: accept it
	} else if (p->m_info.m_eState == k_ESteamNetworkingConnectionState_Connected) {
		conn = p->m_hConn;
		connected = true;
	}
}

static int run_server() {
	if (!SteamAPI_Init()) return 2;
	SockListener lis;
	lis.ns = SteamNetworkingSockets();
	lis.ns->CreateListenSocketP2P(0, 0, nullptr);

	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
	bool got = false;
	while (std::chrono::steady_clock::now() < deadline && !got) {
		SteamAPI_RunCallbacks();
		if (lis.connected) {
			SteamNetworkingMessage_t* msgs[8];
			int n = lis.ns->ReceiveMessagesOnConnection(lis.conn, msgs, 8);
			for (int i = 0; i < n; ++i) {
				char buf[64] = {0};
				int cb = msgs[i]->m_cbSize < 63 ? msgs[i]->m_cbSize : 63;
				std::memcpy(buf, msgs[i]->m_pData, cb);
				fprintf(stderr, "server: got '%s'\n", buf);
				if (std::strcmp(buf, kMsg) == 0) got = true;
				msgs[i]->Release();
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	SteamAPI_Shutdown();
	return got ? 0 : 1;
}

static int run_client() {
	if (!SteamAPI_Init()) return 2;
	ISteamNetworkingSockets* ns = SteamNetworkingSockets();
	ISteamFriends* fr = SteamFriends();
	SockListener lis;
	lis.ns = ns;

	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
	// Discover the server, then connect.
	CSteamID server;
	while (std::chrono::steady_clock::now() < deadline && !server.IsValid()) {
		SteamAPI_RunCallbacks();
		if (fr->GetFriendCount(k_EFriendFlagImmediate) >= 1)
			server = fr->GetFriendByIndex(0, k_EFriendFlagImmediate);
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	if (!server.IsValid()) { fprintf(stderr, "client: no server found\n"); return 3; }

	SteamNetworkingIdentity id;
	id.SetSteamID(server);
	HSteamNetConnection conn = ns->ConnectP2P(id, 0, 0, nullptr);

	bool sent = false;
	while (std::chrono::steady_clock::now() < deadline && !sent) {
		SteamAPI_RunCallbacks();
		if (lis.connected || conn != k_HSteamNetConnection_Invalid) {
			// Once connected, send. (Retry until it reports OK.)
			if (ns->SendMessageToConnection(conn, kMsg, (uint32)std::strlen(kMsg) + 1,
			                                k_nSteamNetworkingSend_Reliable, nullptr) == k_EResultOK)
				sent = true;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	// Give the packet time to arrive before teardown.
	for (int i = 0; i < 20; ++i) { SteamAPI_RunCallbacks(); std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
	SteamAPI_Shutdown();
	fprintf(stderr, "client: connected=%d sent=%d\n", lis.connected, sent);
	return sent ? 0 : 1;
}

int main(int argc, char** argv) {
	setenv("SteamAppId", "482", 1);
	if (argc > 1 && std::strcmp(argv[1], "server") == 0) return run_server();
	if (argc > 1 && std::strcmp(argv[1], "client") == 0) return run_client();

	struct { const char* role; const char* id; const char* name; } cfg[2] = {
		{"server", "3000001", "Server"}, {"client", "3000002", "Client"},
	};
	pid_t pids[2];
	for (int i = 0; i < 2; ++i) {
		pid_t pid = fork();
		if (pid == 0) {
			setenv("STEAMEMU_STEAMID", cfg[i].id, 1);
			setenv("STEAMEMU_NAME", cfg[i].name, 1);
			execl(argv[0], argv[0], cfg[i].role, (char*)nullptr);
			_exit(127);
		}
		pids[i] = pid;
	}
	int rc = 0;
	for (int i = 0; i < 2; ++i) {
		int status = 0;
		waitpid(pids[i], &status, 0);
		int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
		printf("%s exit=%d\n", cfg[i].role, code);
		if (code != 0) rc = 1;
	}
	if (rc == 0) printf("PASS: sockets_test connect + message delivery worked\n");
	else printf("FAILED: sockets_test\n");
	return rc;
}
