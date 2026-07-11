//====== steamemu ============================================================
// Manual callback dispatch test.
//
// Facepunch.Steamworks / Steamworks.NET
// pump callbacks via SteamAPI_ManualDispatch_* instead of SteamAPI_RunCallbacks.
// This verifies that (a) manual dispatch delivers callbacks, and (b) the
// synthesized SteamServersConnected_t logon arrives on that path too -- without
// it, games show "Disconnected from Steam servers".
//
// Portable (no fork/sockets), so it also builds/runs on Windows.
//============================================================================
#include <cstdio>

#include <chrono>
#include <thread>

#include "steam/steam_api.h"

int main() {
	if (!SteamAPI_Init()) { printf("FAIL: init\n"); return 1; }
	SteamAPI_ManualDispatch_Init();
	HSteamPipe pipe = SteamAPI_GetHSteamPipe();

	bool gotConnected = false;
	// Callbacks carry a small delivery latency, so pump across real time like a game.
	for (int frame = 0; frame < 40 && !gotConnected; ++frame) {
		if (frame) std::this_thread::sleep_for(std::chrono::milliseconds(20));
		SteamAPI_ManualDispatch_RunFrame(pipe);
		CallbackMsg_t msg;
		while (SteamAPI_ManualDispatch_GetNextCallback(pipe, &msg)) {
			if (msg.m_iCallback == SteamServersConnected_t::k_iCallback)
				gotConnected = true;
			SteamAPI_ManualDispatch_FreeLastCallback(pipe);
		}
	}

	printf("manual dispatch: SteamServersConnected_t=%d\n", gotConnected);
	SteamAPI_Shutdown();
	if (gotConnected) { printf("PASS: manual_dispatch_test logon delivered\n"); return 0; }
	printf("FAILED: manual_dispatch_test\n");
	return 1;
}
