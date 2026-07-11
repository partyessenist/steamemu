//====== steamemu ============================================================
// Manual-dispatch call-result + pipe-separation regression test.
//
// Steamworks.NET games pump callbacks via
// SteamAPI_ManualDispatch_* and receive async call-results as a
// SteamAPICallCompleted_t whose payload they fetch with GetAPICallResult -- a
// path our RunCallbacks-based tests never exercised. A listen-server game also
// pumps TWO dispatch loops, one on the client pipe and one on the game-server
// pipe, and each must yield ONLY its own callbacks; when they shared a queue the
// game-server pump stole the client's LobbyCreated_t completion and hosting hung
// forever on "Creating lobby". This locks both behaviours down:
//
//   1. CreateLobby's LobbyCreated_t is delivered on the CLIENT pipe via
//      SteamAPICallCompleted_t + GetAPICallResult (result OK, valid lobby id).
//   2. The game server's SteamServersConnected_t + GSPolicyResponse_t are
//      delivered on the GAME-SERVER pipe only.
//   3. Neither pipe delivers the other's callbacks (no cross-consumption).
//   4. LobbyCreated_t is ALSO delivered as a plain callback message (real Steam
//      posts lobby results to callback listeners AND the call result), after
//      its completion but before LobbyEnter_t. Some game writes the lobby metadata
//      (name/gamemode/prefabdbversion) from a Callback<LobbyCreated_t> and
//      reads it back on LobbyEnter_t -- miss or reorder either and the hosted
//      server is invisible to the browser's filters.
//
// Portable (no fork), so it builds and runs on Windows too.
//============================================================================
#include <cstdio>
#include <chrono>
#include <thread>

#include "steam/steam_api.h"
#include "steam/steam_gameserver.h"
#include "steam/isteammatchmaking.h"
#include "steam/isteamgameserver.h"
#include "steam/isteamutils.h"

namespace {

// Drain one pipe for a few frames. Records whether the target ids were seen and,
// for the CreateLobby call handle, fetches + validates the LobbyCreated_t payload.
struct Drained {
	bool sawConnected = false;   // SteamServersConnected_t (101)
	bool sawPolicy = false;      // GSPolicyResponse_t (115)
	bool sawLobbyCreated = false;// LobbyCreated_t (513) via call-result completion
	bool lobbyResultOk = false;  // completion fetched, result OK, id non-zero
	int createdOrder = -1;       // delivery index of the LobbyCreated_t completion
	int plainCreatedOrder = -1;  // delivery index of the plain LobbyCreated_t message
	int enterOrder = -1;         // delivery index of LobbyEnter_t
	int nextOrder = 0;
};

Drained Pump(HSteamPipe pipe, SteamAPICall_t lobbyCall) {
	Drained d;
	// CreateLobby's completion is delayed (~0.75s, mimicking Steam latency), so pump
	// across real time -- a tight loop would finish before it's due.
	for (int frame = 0; frame < 60; ++frame) {
		if (frame) std::this_thread::sleep_for(std::chrono::milliseconds(30));
		SteamAPI_ManualDispatch_RunFrame(pipe);
		CallbackMsg_t msg;
		while (SteamAPI_ManualDispatch_GetNextCallback(pipe, &msg)) {
			const int order = d.nextOrder++;
			if (msg.m_iCallback == SteamServersConnected_t::k_iCallback)
				d.sawConnected = true;
			else if (msg.m_iCallback == GSPolicyResponse_t::k_iCallback)
				d.sawPolicy = true;
			else if (msg.m_iCallback == LobbyEnter_t::k_iCallback && d.enterOrder < 0)
				d.enterOrder = order;
			else if (msg.m_iCallback == LobbyCreated_t::k_iCallback && d.plainCreatedOrder < 0)
				d.plainCreatedOrder = order;
			else if (msg.m_iCallback == SteamAPICallCompleted_t::k_iCallback) {
				auto* c = reinterpret_cast<SteamAPICallCompleted_t*>(msg.m_pubParam);
				if (c->m_iCallback == LobbyCreated_t::k_iCallback && c->m_hAsyncCall == lobbyCall) {
					d.sawLobbyCreated = true;
					if (d.createdOrder < 0) d.createdOrder = order;
					LobbyCreated_t r{};
					bool failed = true;
					bool ok = SteamAPI_ManualDispatch_GetAPICallResult(
						pipe, c->m_hAsyncCall, &r, sizeof(r), LobbyCreated_t::k_iCallback, &failed);
					d.lobbyResultOk = ok && !failed &&
					                  r.m_eResult == k_EResultOK && r.m_ulSteamIDLobby != 0;
				}
			}
			SteamAPI_ManualDispatch_FreeLastCallback(pipe);
		}
	}
	return d;
}

} // namespace

int main() {
	if (!SteamAPI_Init()) { printf("FAILED: init\n"); return 1; }
	// Stand up an integrated game server, exactly as a listen-server game does.
	if (!SteamGameServer_Init(0, 0, 0, eServerModeAuthenticationAndSecure, "1.0.0.0")) {
		printf("FAILED: gameserver init\n"); return 1;
	}
	SteamAPI_ManualDispatch_Init();

	auto* mm = static_cast<ISteamMatchmaking*>(
		SteamInternal_FindOrCreateUserInterface(SteamAPI_GetHSteamUser(), STEAMMATCHMAKING_INTERFACE_VERSION));
	auto* gs = static_cast<ISteamGameServer*>(
		SteamInternal_FindOrCreateGameServerInterface(SteamGameServer_GetHSteamUser(), STEAMGAMESERVER_INTERFACE_VERSION));
	if (!mm || !gs) { printf("FAILED: interfaces mm=%p gs=%p\n", (void*)mm, (void*)gs); return 1; }

	// Game server logs on (queues its connected + policy callbacks to the GS pipe);
	// client creates a lobby (queues the LobbyCreated_t completion to the client pipe).
	gs->LogOnAnonymous();
	SteamAPICall_t lobbyCall = mm->CreateLobby(k_ELobbyTypePublic, 4);

	HSteamPipe clientPipe = SteamAPI_GetHSteamPipe();
	HSteamPipe serverPipe = SteamGameServer_GetHSteamPipe();
	if (clientPipe == serverPipe) {
		printf("FAILED: client and game-server pipes are identical (%d)\n", clientPipe);
		return 1;
	}

	Drained client = Pump(clientPipe, lobbyCall);
	Drained server = Pump(serverPipe, lobbyCall);

	printf("client pipe:  LobbyCreated=%d resultOk=%d policy=%d createdOrder=%d plainCreatedOrder=%d enterOrder=%d\n",
	       client.sawLobbyCreated, client.lobbyResultOk, client.sawPolicy,
	       client.createdOrder, client.plainCreatedOrder, client.enterOrder);
	printf("server pipe:  connected=%d policy=%d lobbyCreated=%d plainCreated=%d\n",
	       server.sawConnected, server.sawPolicy, server.sawLobbyCreated,
	       server.plainCreatedOrder >= 0);

	bool pass = client.sawLobbyCreated && client.lobbyResultOk  // client got its completion
	         && !client.sawPolicy                               // GS callback didn't leak to client
	         // LobbyCreated_t must precede LobbyEnter_t, or the game runs its "joined"
	         // path and never populates the lobby (empty server browser entry).
	         && client.enterOrder >= 0 && client.createdOrder < client.enterOrder
	         // The plain LobbyCreated_t shadow: after its completion, before LobbyEnter_t.
	         && client.plainCreatedOrder > client.createdOrder
	         && client.plainCreatedOrder < client.enterOrder
	         && server.sawConnected && server.sawPolicy         // GS pipe got its callbacks
	         && !server.sawLobbyCreated                         // client completion didn't leak to GS
	         && server.plainCreatedOrder < 0;                   // ...nor its plain shadow

	SteamGameServer_Shutdown();
	SteamAPI_Shutdown();

	if (pass) { printf("PASS: manual_dispatch_result_test\n"); return 0; }
	printf("FAILED: manual_dispatch_result_test\n");
	return 1;
}
