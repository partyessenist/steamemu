//====== steamemu ============================================================
// Exported C entry points: init/shutdown lifecycle, the SteamInternal_*
// interface-location plumbing that the header-side accessors (SteamClient(),
// SteamUser(), ...) funnel through, and the callback-dispatch entry points that
// forward to emu::Dispatcher.
//
// Built with STEAM_API_EXPORTS defined so S_API resolves to a dll export and
// the reference headers omit their inline accessor definitions (which would
// otherwise collide with the ones we provide here).
//
// The concrete ISteam* classes, their flat C wrappers, and the version-string
// registry are generated from steam_api.json (see tools/gen.py -> src/generated).
//============================================================================
#include "emu_common.h"
#include "config.h"
#include "dispatch.h"
#include "net.h"
#include "generated/registry.h"

#include "steam/isteamuser.h"  // SteamServersConnected_t
#include "steam/steam_gameserver.h"  // SteamInternal_GameServer_Init_V2, EServerMode

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

// Bumped on Shutdown so header-side accessor caches (see SteamInternal_ContextInit)
// re-resolve their interface pointers after a re-Init.
uintptr_t g_contextGeneration = 1;

// A real client logs on to Steam right after init and the game learns of it via
// a SteamServersConnected_t callback; games gate online features (and show
// "Disconnected from Steam servers") on having received it. We have no backend,
// so we synthesize the logon: deliver the callback on the first several
// RunCallbacks pumps, which also covers games that register their listener a
// little after init. Reset on each Init so a re-Init re-announces.
std::atomic<int> g_connectPumps{0};
constexpr int kConnectPumps = 5;

// Queue the synthesized SteamServersConnected_t for the first few pumps. Called
// from BOTH pump paths -- SteamAPI_RunCallbacks (the classic CCallback path) and
// SteamAPI_ManualDispatch_RunFrame (used by Facepunch.Steamworks / Steamworks.NET)
// -- so online play works either way.
void AnnounceSteamConnection() {
	if (g_connectPumps.load() < kConnectPumps) {
		g_connectPumps.fetch_add(1);
		emu::QueueCallback(SteamServersConnected_t{});
	}
}

} // namespace

// ---------------------------------------------------------------------------
// Init / shutdown / lifecycle
// ---------------------------------------------------------------------------
extern "C" {

S_API ESteamAPIInitResult S_CALLTYPE SteamInternal_SteamAPI_Init(
		const char *pszInternalCheckInterfaceVersions, SteamErrMsg *pOutErrMsg) {
	(void)pszInternalCheckInterfaceVersions;
	if (pOutErrMsg) (*pOutErrMsg)[0] = '\0';
	g_connectPumps.store(0);   // re-announce the Steam logon on (re-)init
	{
		const std::string& cfg = emu::Config().LoadedPath();
		EMU_INFO("SteamAPI init (appid %u, user '%s', config '%s')",
		         emu::AppID(), emu::PersonaName(), cfg.empty() ? "(none found)" : cfg.c_str());
		// Resolve() can't log (it runs during Config()'s construction); surface any
		// deferred config warning now, e.g. an account-id-0 steam_id we overrode.
		const std::string& warn = emu::Config().Warning();
		if (!warn.empty()) EMU_WARN("config: %s", warn.c_str());
	}
	// Games read the real client's launch env vars directly (Arma 3 checks
	// SteamAppId / the Steam layer markers); publish them like the client would.
	emu::PublishSteamEnvironment();
	// Start LAN discovery now, at init -- not lazily on the first lobby call. A game
	// that browses for servers issues a single RequestLobbyList and expects results
	// immediately; if the backend only started at that moment it has received no
	// beacons yet and finds nothing. Starting here means both hosts and browsers are
	// discovering peers/lobbies for the whole time the player sits in the menu.
	emu::Net().EnsureStarted();
	return k_ESteamAPIInitResult_OK;
}

S_API ESteamAPIInitResult S_CALLTYPE SteamAPI_InitFlat(SteamErrMsg *pOutErrMsg) {
	return SteamInternal_SteamAPI_Init(nullptr, pOutErrMsg);
}

// Legacy exported init still referenced by some older games (SteamAPI_Init() and
// SteamAPI_InitEx() are inline wrappers in the header and must not be redefined).
S_API bool S_CALLTYPE SteamAPI_InitSafe() {
	return SteamInternal_SteamAPI_Init(nullptr, nullptr) == k_ESteamAPIInitResult_OK;
}

S_API void S_CALLTYPE SteamAPI_Shutdown() {
	EMU_INFO("SteamAPI shutdown");
	// Tear down the LAN backend's pump thread + sockets. Without this the thread
	// keeps running after the game thinks Steam is gone; once the sockets are
	// torn down at process exit its select() loop spins at 100% CPU and the
	// process never terminates. Real Steam stops its background work on shutdown.
	emu::Net().Stop();
	++g_contextGeneration;
}

// Never ask the game to relaunch through Steam; it is already running fine.
S_API bool S_CALLTYPE SteamAPI_RestartAppIfNecessary(uint32 unOwnAppID) {
	(void)unOwnAppID;
	emu::Log("SteamAPI_RestartAppIfNecessary");
	return false;
}

S_API bool S_CALLTYPE SteamAPI_IsSteamRunning() { return true; }
S_API void S_CALLTYPE SteamAPI_ReleaseCurrentThreadMemory() {}
S_API const char *S_CALLTYPE SteamAPI_GetSteamInstallPath() { return ""; }
S_API void S_CALLTYPE SteamAPI_SetTryCatchCallbacks(bool) {}
S_API void S_CALLTYPE SteamAPI_WriteMiniDump(uint32, void *, uint32) {}
S_API void S_CALLTYPE SteamAPI_SetMiniDumpComment(const char *) {}
S_API void S_CALLTYPE SteamAPI_SetBreakpadAppID(uint32) {}
S_API void S_CALLTYPE SteamAPI_UseBreakpadCrashHandler(
		char const *, char const *, char const *, bool, void *, PFNPreMinidumpCallback) {}

// ---------------------------------------------------------------------------
// Pipe / user handles
// ---------------------------------------------------------------------------
S_API HSteamPipe S_CALLTYPE SteamAPI_GetHSteamPipe() { emu::Log("SteamAPI_GetHSteamPipe"); return emu::kHSteamPipe; }
S_API HSteamUser S_CALLTYPE SteamAPI_GetHSteamUser() { emu::Log("SteamAPI_GetHSteamUser"); return emu::kHSteamUser; }
S_API HSteamPipe S_CALLTYPE SteamGameServer_GetHSteamPipe() { return emu::kHSteamPipeGameServer; }
S_API HSteamUser S_CALLTYPE SteamGameServer_GetHSteamUser() { return emu::kHSteamUserGameServer; }

// ---------------------------------------------------------------------------
// Game-server lifecycle
//
// Games with an integrated listen server stand up a Steam game server as part 
// of their *client* Steam init. Their managed wrapper P/Invokes these lifecycle exports; 
// a MISSING export throws EntryPointNotFoundException in the wrapper and aborts the whole
// Steam init before our code is ever reached -- so the client never starts
// pumping callbacks and the game reports "Disconnected from Steam servers".
// We must export the full set the real steam_api does. Init succeeds locally
// (no Valve backend) and stands up the shared LAN backend so hosting works.
// ---------------------------------------------------------------------------
S_API ESteamAPIInitResult S_CALLTYPE SteamInternal_GameServer_Init_V2(
		uint32 unIP, uint16 usGamePort, uint16 usQueryPort, EServerMode eServerMode,
		const char *pchVersionString, const char *pszInternalCheckInterfaceVersions,
		SteamErrMsg *pOutErrMsg) {
	(void)unIP; (void)usQueryPort; (void)pchVersionString; (void)pszInternalCheckInterfaceVersions;
	if (pOutErrMsg) (*pOutErrMsg)[0] = '\0';
	EMU_INFO("SteamGameServer init (mode %d, game port %u, query port %u)",
	         (int)eServerMode, usGamePort, usQueryPort);
	// A dedicated server process may never call SteamAPI_Init; it needs the
	// launch env vars too (Arma 3's server validates getenv("SteamAppId")).
	emu::PublishSteamEnvironment();
	emu::Net().EnsureStarted();
	return k_ESteamAPIInitResult_OK;
}

S_API void S_CALLTYPE SteamGameServer_Shutdown() {
	EMU_INFO("SteamGameServer shutdown");
	// Do not tear down the LAN backend here -- the client half may still be
	// using it; SteamAPI_Shutdown owns that.
}

S_API bool S_CALLTYPE SteamGameServer_BSecure() { return false; }
S_API uint64 S_CALLTYPE SteamGameServer_GetSteamID() { return emu::LocalGSSteamID().ConvertToUint64(); }

// ---------------------------------------------------------------------------
// Interface location
//
// The header-side accessor macro stores, per call site, a static
//   { void (*pfnInit)(void*); uintptr_t counter; void *pInterface; }
// and passes its address here. On first use (or after a generation bump) we run
// pfnInit, which writes the interface pointer into pInterface, then hand back a
// pointer to that slot. Layout must match the macro exactly -- do not reorder.
// ---------------------------------------------------------------------------
S_API void *S_CALLTYPE SteamInternal_ContextInit(void *pContextInitData) {
	struct Context {
		void (S_CALLTYPE *pfnInit)(void *);
		uintptr_t counter;
		void *pInterface;
	};
	Context *ctx = static_cast<Context *>(pContextInitData);
	if (ctx->counter != g_contextGeneration) {
		ctx->pfnInit(&ctx->pInterface);
		ctx->counter = g_contextGeneration;
	}
	return &ctx->pInterface;
}

S_API void *S_CALLTYPE SteamInternal_CreateInterface(const char *ver) {
	// SteamClient is the entry object; everything else is located from it.
	// Route through the per-version dispatch: a native game compiled against an
	// older ISteamClient calls its slots on whatever we return here, so it must
	// get that version's exact sub-vtable (an unknown/newer SteamClient* string
	// still resolves to the newest -- never NULL).
	if (ver && std::strncmp(ver, "SteamClient", 11) == 0) {
		EMU_INFO("SteamInternal_CreateInterface('%s') -> SteamClient", ver);
		return emu::SteamClient_ForVersion(ver);
	}
	void *p = emu::FindInterface(ver);
	EMU_INFO("SteamInternal_CreateInterface('%s') -> %s", ver ? ver : "(null)", p ? "ok" : "NULL");
	return p;
}

S_API void *S_CALLTYPE SteamInternal_FindOrCreateUserInterface(HSteamUser, const char *pszVersion) {
	void *p = emu::FindInterface(pszVersion);
	// Log the exact version + whether we resolved it: a NULL here makes the game's
	// C# wrapper hold an invalid interface and can silently disable its callback pump.
	EMU_INFO("SteamInternal_FindOrCreateUserInterface('%s') -> %s",
	         pszVersion ? pszVersion : "(null)", p ? "ok" : "NULL");
	return p;
}

S_API void *S_CALLTYPE SteamInternal_FindOrCreateGameServerInterface(HSteamUser, const char *pszVersion) {
	// Shared interfaces resolve to their game-server-side instance (which sends
	// and receives P2P with the game-server identity).
	void *p = emu::FindInterfaceGS(pszVersion);
	EMU_INFO("SteamInternal_FindOrCreateGameServerInterface('%s') -> %s",
	         pszVersion ? pszVersion : "(null)", p ? "ok" : "NULL");
	return p;
}

// ---------------------------------------------------------------------------
// Callback dispatch -> emu::Dispatcher
// ---------------------------------------------------------------------------
S_API void S_CALLTYPE SteamAPI_RunCallbacks() {
	EMU_LOG("SteamAPI_RunCallbacks");
	AnnounceSteamConnection();   // synthesize the Steam logon
	emu::Dispatch().RunCallbacks(false);
}
S_API void S_CALLTYPE SteamGameServer_RunCallbacks() {
	EMU_LOG("SteamGameServer_RunCallbacks");
	emu::Dispatch().RunCallbacks(true);
}

S_API void S_CALLTYPE SteamAPI_RegisterCallback(class CCallbackBase *pCallback, int iCallback) {
	// Log the callback id (deduped) so a trace shows exactly which callbacks the
	// game subscribes to -- e.g. 101 = SteamServersConnected_t.
	char what[48];
	std::snprintf(what, sizeof(what), "SteamAPI_RegisterCallback(%d)", iCallback);
	emu::Log(what);
	emu::Dispatch().RegisterCallback(pCallback, iCallback);
	// A game registering a connection listener expects to be told it's connected.
	// Queue one now (regardless of how late this is) so it fires on the next pump
	// -- the first-few-pumps announce alone can miss a listener registered late.
	if (iCallback == SteamServersConnected_t::k_iCallback)
		emu::QueueCallback(SteamServersConnected_t{});
}
S_API void S_CALLTYPE SteamAPI_UnregisterCallback(class CCallbackBase *pCallback) {
	emu::Dispatch().UnregisterCallback(pCallback);
}
S_API void S_CALLTYPE SteamAPI_RegisterCallResult(class CCallbackBase *pCallback, SteamAPICall_t hAPICall) {
	emu::Dispatch().RegisterCallResult(pCallback, hAPICall);
}
S_API void S_CALLTYPE SteamAPI_UnregisterCallResult(class CCallbackBase *pCallback, SteamAPICall_t hAPICall) {
	emu::Dispatch().UnregisterCallResult(pCallback, hAPICall);
}

S_API void S_CALLTYPE SteamAPI_ManualDispatch_Init() { EMU_LOG("SteamAPI_ManualDispatch_Init"); }
S_API void S_CALLTYPE SteamAPI_ManualDispatch_RunFrame(HSteamPipe) {
	EMU_LOG("SteamAPI_ManualDispatch_RunFrame");
	// Manual dispatch is the other pump: the game then drains callbacks via
	// GetNextCallback. Announce the Steam logon here too so it is delivered.
	AnnounceSteamConnection();
}
S_API bool S_CALLTYPE SteamAPI_ManualDispatch_GetNextCallback(HSteamPipe hPipe, CallbackMsg_t *pMsg) {
	EMU_LOG("SteamAPI_ManualDispatch_GetNextCallback");
	return emu::Dispatch().GetNextCallback(hPipe, pMsg);
}
S_API void S_CALLTYPE SteamAPI_ManualDispatch_FreeLastCallback(HSteamPipe hPipe) {
	emu::Dispatch().FreeLastCallback(hPipe);
}
S_API bool S_CALLTYPE SteamAPI_ManualDispatch_GetAPICallResult(
		HSteamPipe, SteamAPICall_t hCall, void *pCallback, int cubCallback,
		int iCallbackExpected, bool *pbFailed) {
	return emu::Dispatch().GetAPICallResult(hCall, pCallback, cubCallback, iCallbackExpected, pbFailed);
}

} // extern "C"
