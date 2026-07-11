//====== steamemu ============================================================
// Compatibility export: the bare SteamAPI_Init() symbol.
//
// Newer Steamworks SDKs turned SteamAPI_Init() into a header-side INLINE
// wrapper around SteamInternal_SteamAPI_Init(), so it is no longer a real
// export. Our generated surface is built from a newest-SDK header, so it never
// exported the bare `SteamAPI_Init`. But a game built against an older import
// lib still references that symbol.
// A missing delay-import makes the loader's failure thunk abort the game's
// platform startup before any of our code runs -- so no
// steamemu.log is even produced. Re-export the legacy symbol.
//
// This TU deliberately does NOT include steam_api.h: that header defines an
// inline C++ SteamAPI_Init(), which would clash with our extern "C" export
// (conflicting language linkage). Forward-declare only what we need.
//============================================================================
#include "steam/steam_api_common.h"   // S_API, S_CALLTYPE

namespace emu { void Log(const char* msg); }

extern "C" {

// Defined in steam_api.cpp. Real return type is ESteamAPIInitResult (an int-
// sized enum; k_ESteamAPIInitResult_OK == 0); declared int here to avoid
// pulling in steam_api.h. SteamErrMsg* -> void* (ABI-identical pointer).
S_API int S_CALLTYPE SteamInternal_SteamAPI_Init(
		const char* pszInternalCheckInterfaceVersions, void* pOutErrMsg);

// The legacy bool-returning SteamAPI_Init(): true on success.
S_API bool S_CALLTYPE SteamAPI_Init() {
	emu::Log("SteamAPI_Init (compat export)");
	return SteamInternal_SteamAPI_Init(nullptr, nullptr) == 0;  // k_ESteamAPIInitResult_OK
}

} // extern "C"
