//====== steamemu ============================================================
// Behavioral test for the generated surface + real callback dispatch.
//
// Compiled like a game (no STEAM_API_EXPORTS): uses the inline header accessors
// and the CCallback template machinery, links against the emulator library.
// Exercises the paths the walking-skeleton test does not:
//   * interface location for a NON-user interface (ISteamUtils / ISteamApps),
//   * the flat C API reaching the same state as the vtable,
//   * real callback dispatch: a CCallback fires from RunCallbacks(),
//   * a call-result / auth round-trip (GetAuthSessionTicket -> response callback).
//============================================================================
#include <cstdio>
#include <cstdlib>
#include <cstring>
#if !defined(_WIN32)
#include <dlfcn.h>
#endif

#include "steam/steam_api.h"
#include "steam/steam_api_flat.h"

static int g_failures = 0;
#define CHECK(cond, msg) do { \
	if (!(cond)) { printf("FAIL: %s\n", msg); ++g_failures; } \
	else { printf("ok  : %s\n", msg); } \
} while (0)

// A listener for the auth-ticket response, registered via the CCallback template
// exactly as a game would.
struct AuthListener {
	bool fired = false;
	HAuthTicket ticket = 0;
	EResult result = k_EResultNone;
	STEAM_CALLBACK(AuthListener, OnTicket, GetAuthSessionTicketResponse_t);
};
void AuthListener::OnTicket(GetAuthSessionTicketResponse_t *p) {
	fired = true;
	ticket = p->m_hAuthTicket;
	result = p->m_eResult;
}

// Games gate online features on a SteamServersConnected_t after init (else they
// show "Disconnected from Steam servers"); the emulator synthesizes the logon.
struct ConnectListener {
	bool connected = false;
	STEAM_CALLBACK(ConnectListener, OnConnected, SteamServersConnected_t);
};
void ConnectListener::OnConnected(SteamServersConnected_t *) { connected = true; }

int main() {
	// Feed the appid through SteamGameId (one of the resolution inputs) so we can
	// verify init republishes it as SteamAppId, the var games read directly.
#if defined(_WIN32)
	_putenv_s("SteamGameId", "480");
#else
	setenv("SteamGameId", "480", 1);
#endif

	if (SteamAPI_Init() == false) { printf("FAIL: init\n"); return 1; }

	// --- ISteamUtils located as a non-user (global) interface ---------------
	ISteamUtils *utils = SteamUtils();
	CHECK(utils != nullptr, "SteamUtils() resolves");
	if (utils) {
		uint32 appid = utils->GetAppID();
		printf("     GetAppID=%u\n", appid);
		CHECK(SteamAPI_ISteamUtils_GetAppID(utils) == appid, "flat GetAppID == vtable GetAppID");
		CHECK(std::strcmp(utils->GetIPCountry(), "US") == 0, "GetIPCountry defaulted");

		// Init publishes the real client's launch env vars; games (Arma 3) read
		// them directly instead of calling GetAppID.
		char expected[16];
		std::snprintf(expected, sizeof(expected), "%u", appid);
		const char *envAppId = std::getenv("SteamAppId");
		CHECK(envAppId && std::strcmp(envAppId, expected) == 0, "SteamAppId env published by init");
		const char *envSteamEnv = std::getenv("SteamEnv");
		CHECK(envSteamEnv && std::strcmp(envSteamEnv, "1") == 0, "SteamEnv env published by init");
	}

	// --- ISteamApps ownership defaults --------------------------------------
	ISteamApps *apps = SteamApps();
	CHECK(apps != nullptr, "SteamApps() resolves");
	if (apps) {
		CHECK(apps->BIsSubscribed(), "BIsSubscribed -> true");
		CHECK(std::strcmp(apps->GetCurrentGameLanguage(), "english") == 0, "language defaulted");
	}

	// --- ISteamFriends persona ----------------------------------------------
	ISteamFriends *friends = SteamFriends();
	CHECK(friends != nullptr, "SteamFriends() resolves");
	if (friends)
		printf("     PersonaName='%s' state=%d\n", friends->GetPersonaName(), (int)friends->GetPersonaState());

	// --- flat vs vtable identity --------------------------------------------
	ISteamUser *user = SteamUser();
	CHECK(user != nullptr, "SteamUser() resolves");
	if (user) {
		uint64 vt = user->GetSteamID().ConvertToUint64();
		uint64 fl = SteamAPI_ISteamUser_GetSteamID(user);
		CHECK(vt == fl && vt != 0, "flat GetSteamID == vtable GetSteamID");
	}

	// --- synthesized Steam logon: SteamServersConnected_t after init --------
	{
		ConnectListener conn;
		CHECK(!conn.connected, "not connected before RunCallbacks");
		SteamAPI_RunCallbacks();
		CHECK(conn.connected, "SteamServersConnected_t delivered (online features enabled)");
	}

	// --- real callback dispatch: auth ticket round-trip ---------------------
	{
		AuthListener listener;
		uint8 ticketBuf[256];
		uint32 cbTicket = 0;
		HAuthTicket h = user->GetAuthSessionTicket(ticketBuf, sizeof(ticketBuf), &cbTicket, nullptr);
		CHECK(h != k_HAuthTicketInvalid, "GetAuthSessionTicket returns a handle");
		CHECK(cbTicket > 0, "ticket has nonzero size");
		CHECK(!listener.fired, "callback NOT delivered before RunCallbacks");
		SteamAPI_RunCallbacks();
		CHECK(listener.fired, "callback delivered by RunCallbacks");
		CHECK(listener.ticket == h, "callback carried the right ticket handle");
		CHECK(listener.result == k_EResultOK, "callback reported success");

		// A peer validating our ticket recovers our identity.
		EBeginAuthSessionResult r = user->BeginAuthSession(ticketBuf, cbTicket, user->GetSteamID());
		CHECK(r == k_EBeginAuthSessionResultOK, "BeginAuthSession accepts our own ticket");
	}

	// --- interfaces whose default-neutral return would be WRONG -------------
	ISteamInput* input = SteamInput();
	CHECK(input && input->Init(false), "ISteamInput::Init succeeds");
	ISteamController* controller = SteamController();
	CHECK(controller && controller->Init(), "ISteamController::Init succeeds");
	ISteamInventory* inv = SteamInventory();
	CHECK(inv && inv->GetResultStatus(1) == k_EResultOK, "ISteamInventory result status OK");
	CHECK(apps->GetDLCCount() == 0, "ISteamApps::GetDLCCount is 0 (no DLC)");
	CHECK(apps->GetAppBuildId() != 0, "ISteamApps::GetAppBuildId nonzero");

	// --- poll-loop getters games spin on -------------------------------------
	{
		// Avatars: stable nonzero handle per (user, size); pixels are readable.
		CSteamID me = user->GetSteamID();
		int img = friends->GetMediumFriendAvatar(me);
		CHECK(img != 0 && img != -1, "GetMediumFriendAvatar returns a real handle");
		CHECK(friends->GetMediumFriendAvatar(me) == img, "avatar handle is stable");
		uint32 w = 0, h = 0;
		CHECK(utils->GetImageSize(img, &w, &h) && w == 64 && h == 64,
		      "GetImageSize reports 64x64 for a medium avatar");
		static uint8 rgba[64 * 64 * 4];
		CHECK(utils->GetImageRGBA(img, rgba, sizeof(rgba)) && rgba[3] == 255,
		      "GetImageRGBA fills an opaque placeholder");
		CHECK(friends->GetLargeFriendAvatar(me) > 0,
		      "GetLargeFriendAvatar never answers -1 (pending) without a callback");

		// Directories games locate content/saves with.
		char dir[512] = {};
		CHECK(SteamApps()->GetAppInstallDir(utils->GetAppID(), dir, sizeof(dir)) > 1 && dir[0],
		      "GetAppInstallDir reports a real directory");
		char udir[512] = {};
		CHECK(user->GetUserDataFolder(udir, sizeof(udir)) && udir[0],
		      "GetUserDataFolder reports a real directory");

		// Voice: terminate, don't feed, the poll loop.
		uint32 cb = 1;
		CHECK(user->GetAvailableVoice(&cb) == k_EVoiceResultNotRecording && cb == 0,
		      "GetAvailableVoice reports not-recording with 0 bytes");
	}

	// --- ISteamNetworkingUtils config store (typed helpers funnel into it) ---
	{
		ISteamNetworkingUtils* nu = SteamNetworkingUtils();
		CHECK(nu->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_TimeoutInitial, 7000),
		      "SetGlobalConfigValueInt32 accepted");
		int32 v = 0;
		size_t cbv = sizeof(v);
		ESteamNetworkingConfigDataType dt{};
		CHECK(nu->GetConfigValue(k_ESteamNetworkingConfig_TimeoutInitial,
		                         k_ESteamNetworkingConfig_Global, 0, &dt, &v, &cbv) ==
		          k_ESteamNetworkingGetConfigValue_OK &&
		      v == 7000 && dt == k_ESteamNetworkingConfig_Int32,
		      "GetConfigValue reads the stored value back");
	}

#if !defined(_WIN32)
	// The bare SteamAPI_Init C symbol must be EXPORTED, not merely the header's
	// inline wrapper: games built against an older import lib delay-load it by
	// name, and a missing export aborts their
	// startup before our code runs. Resolve it dynamically -- the header's inline
	// SteamAPI_Init() has C++ linkage and does not shadow the C export.
	{
		bool (*pfnInit)() = reinterpret_cast<bool(*)()>(dlsym(RTLD_DEFAULT, "SteamAPI_Init"));
		CHECK(pfnInit != nullptr, "bare SteamAPI_Init C symbol is exported");
		if (pfnInit) CHECK(pfnInit(), "exported SteamAPI_Init() returns true");
	}
#endif

	SteamAPI_Shutdown();
	if (g_failures == 0) { printf("PASS: api_test all checks passed\n"); return 0; }
	printf("FAILED: %d check(s)\n", g_failures);
	return 1;
}
