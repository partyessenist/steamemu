//====== steamemu ============================================================
// Shutdown / re-init lifecycle test.
//
// Regression for: after SteamAPI_Shutdown the LAN backend's pump thread kept
// running, and at process exit its select() loop spun at 100% CPU so the process
// never terminated. Shutdown must stop the backend, and a subsequent Init must
// bring it back up. This test simply completes (and the process exits): the
// ctest TIMEOUT turns any hang into a failure.
//
// Portable (no fork/sockets), so it also builds/runs on Windows.
//============================================================================
#include <cstdio>

#include "steam/steam_api.h"

int main() {
	for (int i = 0; i < 3; ++i) {
		if (!SteamAPI_Init()) { printf("FAIL: init cycle %d\n", i); return 1; }
		// Touch interfaces that start the LAN pump thread.
		SteamFriends()->GetFriendCount(k_EFriendFlagImmediate);
		SteamMatchmaking()->RequestLobbyList();
		SteamNetworking()->IsP2PPacketAvailable(nullptr, 0);
		for (int k = 0; k < 5; ++k) SteamAPI_RunCallbacks();
		SteamAPI_Shutdown();   // must stop the pump thread (join returns)
		printf("cycle %d: shutdown returned\n", i);
	}
	printf("PASS: shutdown_test clean shutdown + re-init\n");
	return 0;
}
