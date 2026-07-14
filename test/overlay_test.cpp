//====== steamemu ============================================================
// Overlay activation test.
//
// Compiled like a game (no STEAM_API_EXPORTS): drives the overlay through the
// public Steam API and verifies the async ABI around it.
//
// It forces the overlay ON (STEAMEMU_OVERLAY=1) so SteamAPI_Init brings up a
// backend -- on Windows the in-game swapchain hook, which reports "available"
// without popping a window. What it checks when the overlay is up:
//   * ISteamUtils::IsOverlayEnabled() true, BOverlayNeedsPresent() false idle,
//   * ISteamFriends::ActivateGameOverlay -> GameOverlayActivated_t carrying
//     m_bActive=1, m_bUserInitiated=0 (game-initiated) and the current appid,
//   * BOverlayNeedsPresent() tracks visibility; the flat export agrees,
//   * a second activation while already shown does NOT re-fire the callback.
//
// The overlay's D3D backend can fail to come up on a windowless CI agent (no
// desktop); the test then verifies the INERT path instead of flaking. When the
// overlay is compiled out entirely (headless/Linux build, no STEAMEMU_OVERLAY_BUILT)
// activation is likewise inert. Either way there is a soft-lock guard: no
// GameOverlayActivated_t fires for an overlay that cannot draw.
//============================================================================
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "steam/steam_api.h"
#include "steam/steam_api_flat.h"

static int g_failures = 0;
#define CHECK(cond, msg) do { \
	if (!(cond)) { printf("FAIL: %s\n", msg); ++g_failures; } \
	else { printf("ok  : %s\n", msg); } \
} while (0)

// Listener for the overlay activate/deactivate signal, registered via the
// CCallback template exactly as a game would.
struct OverlayListener {
	bool fired = false;
	uint8 active = 0;
	bool userInitiated = true;
	AppId_t appID = 0;
	STEAM_CALLBACK(OverlayListener, OnOverlay, GameOverlayActivated_t);
};
void OverlayListener::OnOverlay(GameOverlayActivated_t *p) {
	fired = true;
	active = p->m_bActive;
	userInitiated = p->m_bUserInitiated;
	appID = p->m_nAppID;
}

static void PumpFor(int iterations) {
	for (int i = 0; i < iterations; ++i) {
		SteamAPI_RunCallbacks();
		std::this_thread::sleep_for(std::chrono::milliseconds(15));
	}
}

// Verify activation is a no-op: no callback, nothing to present. Shared by the
// compiled-out build and the (rare) windowless-agent case.
static void CheckInert(ISteamUtils *utils, ISteamFriends *friends) {
	OverlayListener ov;
	friends->ActivateGameOverlay("friends");
	PumpFor(10);
	CHECK(!ov.fired, "no GameOverlayActivated_t for an overlay that cannot draw");
	CHECK(!utils->BOverlayNeedsPresent(), "BOverlayNeedsPresent stays false (nothing to draw)");
}

int main() {
#if defined(_WIN32)
	_putenv_s("SteamAppId", "480");
	_putenv_s("STEAMEMU_OVERLAY", "1");
#else
	setenv("SteamAppId", "480", 1);
	setenv("STEAMEMU_OVERLAY", "1", 1);
#endif

	if (SteamAPI_Init() == false) { printf("FAIL: init\n"); return 1; }

	ISteamUtils *utils = SteamUtils();
	ISteamFriends *friends = SteamFriends();
	CHECK(utils != nullptr, "SteamUtils() resolves");
	CHECK(friends != nullptr, "SteamFriends() resolves");
	if (!utils || !friends) { SteamAPI_Shutdown(); return 1; }

	const bool overlayUp = utils->IsOverlayEnabled();
	printf("     IsOverlayEnabled=%d\n", overlayUp ? 1 : 0);

#if defined(STEAMEMU_OVERLAY_BUILT)
	if (overlayUp) {
		// A live backend: exercise the full activation -> callback path.
		CHECK(!utils->BOverlayNeedsPresent(), "BOverlayNeedsPresent false before activation");

		OverlayListener ov;
		friends->ActivateGameOverlay("friends");  // what a game's shift-tab handler calls
		PumpFor(20);
		CHECK(ov.fired, "GameOverlayActivated_t delivered after ActivateGameOverlay");
		CHECK(ov.active == 1, "callback reports overlay active (m_bActive=1)");
		CHECK(ov.userInitiated == false, "game-initiated activation flagged not-user");
		CHECK(ov.appID == utils->GetAppID(), "callback carries the current appid");

		CHECK(utils->BOverlayNeedsPresent(), "BOverlayNeedsPresent true while overlay shown");
		CHECK(SteamAPI_ISteamUtils_BOverlayNeedsPresent(utils) == utils->BOverlayNeedsPresent(),
		      "flat BOverlayNeedsPresent == vtable");

		// A second activation while already shown must NOT re-fire (no edge).
		ov.fired = false;
		friends->ActivateGameOverlayInviteDialog(CSteamID());
		PumpFor(10);
		CHECK(!ov.fired, "no duplicate GameOverlayActivated_t when already shown");
	} else {
		printf("note: overlay backend did not start (windowless agent); checking inert path\n");
		CheckInert(utils, friends);
	}
#else
	// Overlay compiled out entirely: activation is inert regardless of config.
	CHECK(!overlayUp, "IsOverlayEnabled false when overlay compiled out");
	CheckInert(utils, friends);
#endif

	SteamAPI_Shutdown();
	if (g_failures == 0) { printf("PASS: overlay_test all checks passed\n"); return 0; }
	printf("FAILED: %d check(s)\n", g_failures);
	return 1;
}
