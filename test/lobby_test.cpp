//====== steamemu ============================================================
// Two-instance LAN lobby test (ISteamMatchmaking over the discovery backend).
//
// One child creates a lobby and sets lobby data; the other lists lobbies, joins,
// and must observe the synced data + a member count of 2. Both children reaching
// that state proves create/list/join/data-sync/member-enumeration end-to-end.
// The owner then registers a game server (SetLobbyGameServer, with a
// GameServer-TYPE SteamID exactly as a real host does) and the joiner must
// receive LobbyGameCreated_t and read the same triple back via
// GetLobbyGameServer -- the "host started the game, connect now" signal
// lobby members wait on.
//
// POSIX-only (fork/exec).
//============================================================================
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>

#include "lan_spawn.h"

#include "steam/steam_api.h"

static const uint32 kGsIP = 0x01020304;
static const uint16 kGsPort = 27015;

// Joiner-side listener for the game-server announcement.
struct GameCreatedListener {
	bool fired = false;
	LobbyGameCreated_t last{};
	CCallback<GameCreatedListener, LobbyGameCreated_t> cb;
	GameCreatedListener() : cb(this, &GameCreatedListener::On) {}
	void On(LobbyGameCreated_t* p) { fired = true; last = *p; }
};

static void pump_ms(int ms) {
	auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
	while (std::chrono::steady_clock::now() < end) {
		SteamAPI_RunCallbacks();
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
}

static int run_owner() {
	if (!SteamAPI_Init()) return 2;
	ISteamMatchmaking* mm = SteamMatchmaking();
	CSteamID lobby = CSteamID();
	// CreateLobby completes via a call-result; for the test we read the id back
	// from the first (and only) lobby we own once it exists in our table.
	mm->CreateLobby(k_ELobbyTypePublic, 4);
	pump_ms(300);
	// Our own lobby shows up in the joinable list.
	lobby = mm->GetLobbyByIndex(0);
	if (!lobby.IsValid()) { fprintf(stderr, "owner: no lobby after create\n"); return 3; }
	mm->SetLobbyData(lobby, "map", "de_dust");
	fprintf(stderr, "owner: created lobby %llu\n", (unsigned long long)lobby.ConvertToUint64());

	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
	int members = 0;
	while (std::chrono::steady_clock::now() < deadline) {
		SteamAPI_RunCallbacks();
		members = mm->GetNumLobbyMembers(lobby);
		if (members >= 2) break;
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	if (members >= 2) {
		// "Start the game": register the server under our GameServer-type id,
		// then stay up so the gossip carries it to the joiner.
		CSteamID gsID(SteamUser()->GetSteamID().GetAccountID(), k_EUniversePublic,
		              k_EAccountTypeGameServer);
		mm->SetLobbyGameServer(lobby, kGsIP, kGsPort, gsID);
		fprintf(stderr, "owner: registered game server %llu\n",
		        (unsigned long long)gsID.ConvertToUint64());
		pump_ms(4000);
	}
	SteamAPI_Shutdown();
	fprintf(stderr, "owner: members=%d\n", members);
	return members >= 2 ? 0 : 1;
}

static int run_joiner() {
	if (!SteamAPI_Init()) return 2;
	ISteamMatchmaking* mm = SteamMatchmaking();

	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
	CSteamID lobby;
	// Find a lobby.
	while (std::chrono::steady_clock::now() < deadline && !lobby.IsValid()) {
		mm->RequestLobbyList();
		pump_ms(300);
		lobby = mm->GetLobbyByIndex(0);
	}
	if (!lobby.IsValid()) { fprintf(stderr, "joiner: no lobby found\n"); return 3; }
	fprintf(stderr, "joiner: found lobby %llu, joining\n", (unsigned long long)lobby.ConvertToUint64());
	mm->JoinLobby(lobby);

	GameCreatedListener gameCreated;
	bool ok = false;
	while (std::chrono::steady_clock::now() < deadline) {
		SteamAPI_RunCallbacks();
		const char* map = mm->GetLobbyData(lobby, "map");
		int members = mm->GetNumLobbyMembers(lobby);
		if (map && std::strcmp(map, "de_dust") == 0 && members >= 2) {
			fprintf(stderr, "joiner: map='%s' members=%d\n", map, members);
			ok = true;
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	// The owner registers its game server once it sees us -- wait for the
	// LobbyGameCreated_t announcement and verify GetLobbyGameServer agrees.
	bool gsOk = false;
	while (ok && std::chrono::steady_clock::now() < deadline) {
		SteamAPI_RunCallbacks();
		uint32 ip = 0; uint16 port = 0; CSteamID srv;
		if (gameCreated.fired && mm->GetLobbyGameServer(lobby, &ip, &port, &srv)) {
			uint32 ownerAcct = mm->GetLobbyOwner(lobby).GetAccountID();
			gsOk = gameCreated.last.m_unIP == kGsIP && gameCreated.last.m_usPort == kGsPort &&
			       CSteamID(static_cast<uint64>(gameCreated.last.m_ulSteamIDGameServer)).GetAccountID() == ownerAcct &&
			       ip == kGsIP && port == kGsPort &&
			       srv.ConvertToUint64() == gameCreated.last.m_ulSteamIDGameServer;
			fprintf(stderr, "joiner: LobbyGameCreated server=%llu %u:%u ok=%d\n",
			        (unsigned long long)gameCreated.last.m_ulSteamIDGameServer, ip, port, gsOk);
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	SteamAPI_Shutdown();
	return (ok && gsOk) ? 0 : 1;
}

int main(int argc, char** argv) {
	if (argc > 1 && std::strcmp(argv[1], "owner") == 0) return run_owner();
	if (argc > 1 && std::strcmp(argv[1], "joiner") == 0) return run_joiner();

	LAN_SETENV("SteamAppId", "481");
	const LanChild children[2] = {
		{"owner", "2000001", "Host"}, {"joiner", "2000002", "Guest"},
	};
	int rc = LanRunTwoChildren(argv[0], children);
	if (rc == 0) printf("PASS: lobby_test create/join/data-sync worked\n");
	else printf("FAILED: lobby_test\n");
	return rc;
}
