//====== steamemu ============================================================
// Walking-skeleton smoke test.
//
// Compiled WITHOUT STEAM_API_EXPORTS (like a real game), so it picks up the
// inline header accessors (SteamAPI_Init, SteamUser(), SteamClient()) and links
// against libsteam_api for the exported plumbing they call into.
//
// Proves the identity round-trip over BOTH call paths that a game uses:
//   1. the header accessor -> SteamInternal_ContextInit chain, and
//   2. a virtual call through ISteamClient's vtable.
// If the vtable layout were off by a slot, GetISteamUser would dispatch to the
// wrong method and this would crash or print garbage.
//============================================================================
#include <cstdio>

#include "steam/steam_api.h"

int main() {
	if (SteamAPI_RestartAppIfNecessary(480)) {
		printf("FAIL: RestartAppIfNecessary asked us to relaunch\n");
		return 1;
	}

	if (!SteamAPI_Init()) {
		printf("FAIL: SteamAPI_Init returned false\n");
		return 1;
	}

	// Path 1: header accessor -> SteamInternal_FindOrCreateUserInterface.
	ISteamUser *user = SteamUser();
	if (!user) {
		printf("FAIL: SteamUser() returned null\n");
		return 1;
	}
	const uint64 id = user->GetSteamID().ConvertToUint64();
	printf("accessor : SteamID=%llu  BLoggedOn=%d\n",
	       (unsigned long long)id, (int)user->BLoggedOn());

	// Path 2: virtual dispatch through the ISteamClient vtable.
	ISteamClient *client = SteamClient();
	if (!client) {
		printf("FAIL: SteamClient() returned null\n");
		return 1;
	}
	ISteamUser *user2 = client->GetISteamUser(
		SteamAPI_GetHSteamUser(), SteamAPI_GetHSteamPipe(), STEAMUSER_INTERFACE_VERSION);
	if (!user2) {
		printf("FAIL: GetISteamUser returned null\n");
		return 1;
	}
	const uint64 id2 = user2->GetSteamID().ConvertToUint64();
	printf("vtable   : SteamID=%llu  same-object-as-accessor=%d\n",
	       (unsigned long long)id2, (int)(user == user2));

	if (id != id2 || user != user2) {
		printf("FAIL: identity disagreed across call paths\n");
		return 1;
	}

	SteamAPI_RunCallbacks();
	SteamAPI_Shutdown();
	printf("PASS: identity round-trip consistent across both paths\n");
	return 0;
}
