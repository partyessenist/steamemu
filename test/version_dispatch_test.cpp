//====== steamemu ============================================================
// Interface version-dispatch test.
//
// A game is compiled against ONE version of each ISteam* interface and calls
// its methods by vtable slot. Valve inserts/removes/retypes methods between
// versions, so slot N of an old version may be a different method than slot N
// of the newest. We must therefore hand a game the sub-object for the EXACT
// version it requests (its own correct sub-vtable), not our newest one.
//
// This verifies, via ISteamClient::GetISteamGenericInterface (the by-string
// path SteamInternal_* funnels through):
//   * distinct requested versions return distinct sub-object pointers (distinct
//     sub-vtables within the one multiply-inheriting concrete object),
//   * the same version string is stable (a cached singleton sub-object),
//   * an unknown/newer version of a known interface resolves to the newest
//     rather than NULL (so a game's wrapper does not treat init as failed),
//   * a slot-stable virtual called through an OLD version pointer returns our
//     real identity, i.e. the sub-vtable is wired correctly.
//
// Compiled like a game (no STEAM_API_EXPORTS).
//============================================================================
#include <cstdio>

#include "steam/steam_api.h"
#include "steam/steam_api_flat.h"

static int g_fail = 0;
static void check(bool ok, const char *what) {
	if (!ok) { printf("FAIL: %s\n", what); g_fail = 1; }
}

int main() {
	if (!SteamAPI_Init()) {
		printf("FAIL: SteamAPI_Init returned false\n");
		return 1;
	}
	ISteamClient *c = SteamClient();
	const HSteamUser u = SteamAPI_GetHSteamUser();
	const HSteamPipe p = SteamAPI_GetHSteamPipe();

	// -- ISteamUser: three historical versions must be three distinct sub-objects.
	void *u023 = c->GetISteamGenericInterface(u, p, "SteamUser023");
	void *u017 = c->GetISteamGenericInterface(u, p, "SteamUser017");
	void *u012 = c->GetISteamGenericInterface(u, p, "SteamUser012");
	check(u023 && u017 && u012, "a requested ISteamUser version resolved to NULL");
	check(u023 != u017 && u017 != u012 && u023 != u012,
	      "distinct ISteamUser versions returned the same sub-object");

	// Stable: same version string -> same pointer (cached singleton sub-object).
	check(u023 == c->GetISteamGenericInterface(u, p, "SteamUser023") &&
	      u017 == c->GetISteamGenericInterface(u, p, "SteamUser017"),
	      "the same version string returned different pointers");

	// -- ISteamFriends: SetPersonaName changed void -> SteamAPICall_t across
	// these versions, which is exactly the kind of slot/return divergence that
	// requires per-version vtables. They must still be distinct sub-objects.
	void *f018 = c->GetISteamGenericInterface(u, p, "SteamFriends018");
	void *f015 = c->GetISteamGenericInterface(u, p, "SteamFriends015");
	void *f003 = c->GetISteamGenericInterface(u, p, "SteamFriends003");
	check(f018 && f015 && f003, "a requested ISteamFriends version resolved to NULL");
	check(f018 != f015 && f015 != f003 && f018 != f003,
	      "distinct ISteamFriends versions returned the same sub-object");

	// -- Unknown/newer version of a KNOWN interface -> newest, never NULL.
	void *u099 = c->GetISteamGenericInterface(u, p, "SteamUser099");
	check(u099 == u023, "an unknown ISteamUser version did not fall back to newest");

	// -- A completely unknown interface family -> NULL (not a wrong object).
	check(c->GetISteamGenericInterface(u, p, "SteamNoSuchThing001") == nullptr,
	      "an unknown interface family did not resolve to NULL");

	// -- Call a slot-stable virtual through the OLD version pointer, reinterpreted
	// as the game would. GetSteamID is slot 2 across these versions; a wrong
	// sub-vtable would return garbage or crash.
	ISteamUser *old = reinterpret_cast<ISteamUser *>(u017);
	const CSteamID id = old->GetSteamID();
	check(id.IsValid() && id == SteamUser()->GetSteamID(),
	      "GetSteamID through an old ISteamUser sub-vtable disagreed");

	// -- SteamInternal_CreateInterface (the native bootstrap path every game's
	// SteamClient() accessor funnels through) must version-dispatch ISteamClient
	// itself, not short-circuit to the newest vtable.
	void *c023 = SteamInternal_CreateInterface(STEAMCLIENT_INTERFACE_VERSION);
	void *c012 = SteamInternal_CreateInterface("SteamClient012");
	check(c023 && c012, "a requested ISteamClient version resolved to NULL");
	check(c023 != c012,
	      "SteamInternal_CreateInterface did not version-dispatch SteamClient");
	check(c012 == c->GetISteamGenericInterface(u, p, "SteamClient012"),
	      "SteamClient012 differed between CreateInterface and GetISteamGenericInterface");

	// -- Flat exports must keep working when handed a LEGACY sub-object: a flat
	// binding (Steamworks.NET-style) locates an OLD version string, then calls
	// newest-layout flat exports on that pointer. STEAMUSERSTATS...011 has
	// RequestCurrentStats at slot 0 (removed in 013), so every later slot is
	// shifted: without normalization SetStatInt32 would land on the wrong
	// virtual (and crash), with it the value round-trips.
	ISteamUserStats *s011 = reinterpret_cast<ISteamUserStats *>(
	    c->GetISteamGenericInterface(u, p, "STEAMUSERSTATS_INTERFACE_VERSION011"));
	check(s011 != nullptr, "STEAMUSERSTATS_INTERFACE_VERSION011 resolved to NULL");
	check(SteamAPI_ISteamUserStats_SetStatInt32(s011, "vdt_flat", 77),
	      "flat SetStatInt32 on a legacy sub-object failed");
	int32 got = 0;
	check(SteamAPI_ISteamUserStats_GetStatInt32(s011, "vdt_flat", &got) && got == 77,
	      "flat GetStatInt32 on a legacy sub-object did not round-trip");

	// -- Old-only slots must forward to the real implementation, not sit as dead
	// stubs: RequestCurrentStats() is slot 0 of every ISteamUserStats <= 012 and
	// old games gate all stats/achievement work on it returning true.
	struct OldUserStats { virtual bool RequestCurrentStats() = 0; };
	check(reinterpret_cast<OldUserStats *>(s011)->RequestCurrentStats(),
	      "RequestCurrentStats through the v011 sub-vtable returned false");

	// -- Inter-release versions (absent from the SDK git history, vendored under
	// tools/inter_versions/) must resolve to their own sub-objects, not fall
	// back to the newest vtable. SteamFriends016 and SteamUser015 shipped in
	// real games between two SDK releases.
	void *f016 = c->GetISteamGenericInterface(u, p, "SteamFriends016");
	void *f017 = c->GetISteamGenericInterface(u, p, "SteamFriends017");
	check(f016 && f016 != f018 && f016 != f017 && f016 != f015,
	      "inter-release SteamFriends016 did not get its own sub-object");
	void *u015 = c->GetISteamGenericInterface(u, p, "SteamUser015");
	void *u016 = c->GetISteamGenericInterface(u, p, "SteamUser016");
	check(u015 && u015 != u023 && u015 != u016,
	      "inter-release SteamUser015 did not get its own sub-object");
	// GetSteamID sits at slot 2 of SteamUser015 exactly as it does today; a
	// mis-wired vendored sub-vtable would return garbage here.
	struct OldUser {
		virtual int GetHSteamUser_() = 0;
		virtual bool BLoggedOn() = 0;
		virtual CSteamID GetSteamID() = 0;
	};
	check(reinterpret_cast<OldUser *>(u015)->GetSteamID() == SteamUser()->GetSteamID(),
	      "GetSteamID through the vendored SteamUser015 sub-vtable disagreed");

	// -- SDK 1.24-1.34 define the DIGITLESS "STEAMCONTROLLER_INTERFACE_VERSION"
	// (the only such string in history); that era's ISteamController is
	// structurally different from every numbered version, so it must resolve to
	// its own sub-object, and never to NULL (that fails init for those games).
	void *ctrl_old = c->GetISteamGenericInterface(u, p, "STEAMCONTROLLER_INTERFACE_VERSION");
	void *ctrl_new = c->GetISteamGenericInterface(u, p, "SteamController008");
	check(ctrl_old != nullptr, "digitless STEAMCONTROLLER_INTERFACE_VERSION resolved to NULL");
	check(ctrl_old != ctrl_new, "digitless controller version fell back to the newest vtable");

	// -- SteamController001/002 (and SteamNetworkingSockets007) are strings the
	// REAL steamclient returns NULL for; games built against them handle NULL,
	// and any vtable we could serve instead would have wrong slots.
	check(c->GetISteamGenericInterface(u, p, "SteamController001") == nullptr,
	      "SteamController001 should be NULL (matches the real client)");

	// -- SteamGameServer006/007 are known to share
	// SteamGameServer008's vtable: alias, don't fall back to newest.
	void *gs006 = c->GetISteamGenericInterface(u, p, "SteamGameServer006");
	void *gs008 = c->GetISteamGenericInterface(u, p, "SteamGameServer008");
	void *gs015 = c->GetISteamGenericInterface(u, p, "SteamGameServer015");
	check(gs006 && gs006 == gs008, "SteamGameServer006 did not alias the 008 vtable");
	check(gs006 != gs015, "SteamGameServer006 fell back to the newest vtable");

	// -- Vtable-identical versions are deduplicated onto one class: both strings
	// resolve (to the same sub-object), and one identical to the newest resolves
	// to the newest sub-object itself.
	void *in1 = c->GetISteamGenericInterface(u, p, "SteamInput001");
	void *in2 = c->GetISteamGenericInterface(u, p, "SteamInput002");
	void *in6 = c->GetISteamGenericInterface(u, p, "SteamInput006");
	check(in1 && in1 == in2, "vtable-identical SteamInput001/002 did not share a sub-object");
	check(in1 != in6, "SteamInput001 (different layout) fell back to newest");
	check(c->GetISteamGenericInterface(u, p, "STEAMHTTP_INTERFACE_VERSION002") ==
	      c->GetISteamGenericInterface(u, p, "STEAMHTTP_INTERFACE_VERSION003"),
	      "STEAMHTTP 002 (vtable-identical to 003) did not dedupe onto the newest");

	// -- A NULL version string means "the default" (the real client tolerates
	// it); it must resolve to the newest, not crash in a strcmp.
	check(c->GetISteamUtils(p, nullptr) == c->GetISteamUtils(p, "SteamUtils010"),
	      "a NULL version string did not resolve to the newest interface");

	SteamAPI_Shutdown();
	if (g_fail) return 1;
	printf("PASS: version_dispatch all checks passed\n");
	return 0;
}
