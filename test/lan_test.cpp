//====== steamemu ============================================================
// Two-instance LAN integration test (the headline feature).
//
// Forks two child processes, each loading the emulator with a DISTINCT identity
// (STEAMEMU_STEAMID) but the same AppID. Each child must:
//   1. discover the other via the multicast beacon (it shows up in ISteamFriends),
//   2. send it a P2P packet through ISteamNetworking, and
//   3. receive the peer's packet and verify the sender + payload.
// Both children exiting 0 proves discovery + transport end-to-end on one host
// (multicast loopback, out every local interface). On a real LAN the same code
// reaches other machines -- the beacon is fanned out over every interface, so a
// multi-homed host (physical NIC + virtual adapters) still reaches the LAN NIC.
//
// Phase 2 (the listen-server shape): the "server"
// child also runs a game server and serves P2P through SteamGameServerNetworking
// under its GameServer-TYPE identity; the "client" child sends to that id (as
// learned from LobbyGameCreated_t in a real game) and must get the reply back
// FROM that id -- games filter server traffic by sender. The server also proves
// the client/server inboxes don't leak into each other.
//
// Portable: the child logic uses only the Steam API; the parent spawns two
// copies of itself (CreateProcess/_spawn on Windows, fork/exec on POSIX).
//============================================================================
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <string>
#include <thread>

#include "lan_spawn.h"

#include "steam/steam_api.h"
#include "steam/steam_gameserver.h"

static void sleep_ms(int ms) {
	std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

static const int kGsChannel = 7;

// Phase 2, server side: serve "gs-ping" on the game-server networking interface,
// replying "gs-pong" from the GameServer identity. Returns 0 on success.
static int serve_gs(ISteamNetworking* clientNet) {
	if (!SteamGameServer_Init(0, 0, 0, eServerModeNoAuthentication, "1.0.0.0")) {
		fprintf(stderr, "server: gameserver init failed\n"); return 4;
	}
	auto* gsNet = static_cast<ISteamNetworking*>(SteamInternal_FindOrCreateGameServerInterface(
		SteamGameServer_GetHSteamUser(), "SteamNetworking006"));
	if (!gsNet) { fprintf(stderr, "server: no gameserver networking\n"); return 4; }
	if (gsNet == clientNet) { fprintf(stderr, "server: gameserver networking is the client instance\n"); return 4; }

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
	auto linger = deadline;   // keep serving briefly after the first ping
	bool got = false, leaked = false;
	while (std::chrono::steady_clock::now() < std::min(deadline, linger)) {
		SteamAPI_RunCallbacks();
		SteamGameServer_RunCallbacks();
		// The client's ping is addressed to our GameServer identity: it must be
		// readable on the game-server interface and NOT on the client one.
		uint32 sz = 0;
		if (clientNet->IsP2PPacketAvailable(&sz, kGsChannel)) leaked = true;
		char buf[64] = {0}; uint32 read = 0; CSteamID sender;
		while (gsNet->ReadP2PPacket(buf, sizeof(buf), &read, &sender, kGsChannel)) {
			if (std::strcmp(buf, "gs-ping") == 0) {
				if (!got) linger = std::chrono::steady_clock::now() + std::chrono::seconds(2);
				got = true;
				gsNet->SendP2PPacket(sender, "gs-pong", 8, k_EP2PSendReliable, kGsChannel);
			}
		}
		sleep_ms(50);
	}
	SteamGameServer_Shutdown();
	fprintf(stderr, "server: gs ping=%d leaked=%d\n", got, leaked);
	return (got && !leaked) ? 0 : 5;
}

// Phase 2, client side: reach the server's GameServer-TYPE identity (as a real
// game does after LobbyGameCreated_t) and require the reply to come FROM it.
static int query_gs(ISteamNetworking* net, CSteamID peer) {
	CSteamID serverID(peer.GetAccountID(), k_EUniversePublic, k_EAccountTypeGameServer);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
	while (std::chrono::steady_clock::now() < deadline) {
		SteamAPI_RunCallbacks();
		net->SendP2PPacket(serverID, "gs-ping", 8, k_EP2PSendReliable, kGsChannel);
		char buf[64] = {0}; uint32 read = 0; CSteamID sender;
		while (net->ReadP2PPacket(buf, sizeof(buf), &read, &sender, kGsChannel)) {
			if (std::strcmp(buf, "gs-pong") == 0) {
				bool ok = sender == serverID;
				fprintf(stderr, "client: gs-pong from %llu (want %llu) ok=%d\n",
				        (unsigned long long)sender.ConvertToUint64(),
				        (unsigned long long)serverID.ConvertToUint64(), ok);
				return ok ? 0 : 5;
			}
		}
		sleep_ms(100);
	}
	fprintf(stderr, "client: no gs-pong\n");
	return 5;
}

// One emulator instance: discover the peer, exchange one packet each way.
static int run_child(bool server) {
	if (!SteamAPI_Init()) { fprintf(stderr, "child: init failed\n"); return 2; }
	ISteamFriends* fr = SteamFriends();
	ISteamNetworking* net = SteamNetworking();
	ISteamUser* usr = SteamUser();
	if (!fr || !net || !usr) return 3;

	const uint64 me = usr->GetSteamID().ConvertToUint64();
	char msg[64];
	std::snprintf(msg, sizeof(msg), "hello-from-%llu", (unsigned long long)me);

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
	bool received = false;
	bool sent = false;
	CSteamID peer;
	uint64 gotFrom = 0;

	// Succeed only once we have BOTH sent to and received from the peer, so a
	// fast receiver doesn't exit before it has replied.
	while (std::chrono::steady_clock::now() < deadline && !(received && sent)) {
		SteamAPI_RunCallbacks();

		// Discover the peer (any other instance in our AppID).
		if (fr->GetFriendCount(k_EFriendFlagImmediate) >= 1)
			peer = fr->GetFriendByIndex(0, k_EFriendFlagImmediate);

		// Keep sending; the peer may not be up yet or may need several tries.
		if (peer.IsValid())
			sent |= net->SendP2PPacket(peer, msg, (uint32)std::strlen(msg) + 1, k_EP2PSendReliable, 0);

		uint32 sz = 0;
		while (net->IsP2PPacketAvailable(&sz, 0)) {
			char buf[128] = {0};
			uint32 read = 0;
			CSteamID sender;
			if (net->ReadP2PPacket(buf, sizeof(buf), &read, &sender, 0)) {
				gotFrom = sender.ConvertToUint64();
				fprintf(stderr, "child %llu: received '%s' from %llu\n",
				        (unsigned long long)me, buf, (unsigned long long)gotFrom);
				if (std::strncmp(buf, "hello-from-", 11) == 0 && gotFrom != 0 && gotFrom != me)
					received = true;
			}
		}
		sleep_ms(100);
	}

	int rc = 1;
	if (received && sent) {
		fprintf(stderr, "child %llu: phase1 OK\n", (unsigned long long)me);
		// Phase 2: game-server identity round-trip (see file header).
		rc = server ? serve_gs(net) : query_gs(net, peer);
	} else {
		fprintf(stderr, "child %llu: TIMEOUT (peer=%llu)\n",
		        (unsigned long long)me, (unsigned long long)peer.ConvertToUint64());
	}
	SteamAPI_Shutdown();
	return rc;
}

int main(int argc, char** argv) {
	if (argc > 1 && std::strcmp(argv[1], "server") == 0)
		return run_child(true);
	if (argc > 1 && std::strcmp(argv[1], "client") == 0)
		return run_child(false);

	// Parent: spawn two children with distinct identities, same AppID.
	LAN_SETENV("SteamAppId", "480");
	const LanChild children[2] = {
		{"server", "1000001", "Alice"}, {"client", "1000002", "Bob"},
	};
	int rc = LanRunTwoChildren(argv[0], children);

	if (rc == 0) printf("PASS: lan_test peers discovered and exchanged packets\n");
	else printf("FAILED: lan_test\n");
	return rc;
}
