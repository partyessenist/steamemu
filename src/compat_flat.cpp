//====== steamemu ============================================================
// Compatibility flat exports for OLDER interface versions.
//
// We generate the flat C API from the newest Steamworks SDK, which has
// DROPPED methods that older games still P/Invoke. A P/Invoke to a MISSING 
// export throws EntryPointNotFoundException in
// the managed wrapper and aborts the whole Steam init -> the game never pumps
// callbacks and shows "Disconnected from Steam servers". These stubs make the
// exports resolve; params use ABI-identical simplified types (pointers->void*,
// enums->int, uint64_steamid->uint64). Behaviour is neutral except where noted.
//============================================================================
#include "emu_common.h"
#include "dispatch.h"
#include "steam/steam_api.h"
#include "steam/isteamuserstats.h"  // UserStatsReceived_t

extern "C" {

// Some games fetch these interfaces during their multiplayer
// subsystem init and treat a NULL pointer as "Steam unavailable", which disables
// their callback pump (so SteamServersConnected_t never gets delivered and the game
// shows "Disconnected from Steam servers"). We don't implement these interfaces, but
// Steamworks.NET reaches them only through the flat API -- the `self` pointer is
// ignored by every stub below -- so a stable NON-NULL handle is enough to keep the
// subsystem alive. (Returning our own address avoids ever handing back a bogus vtable.)
static char g_gameSearchStub;
static char g_musicRemoteStub;

S_API void * S_CALLTYPE SteamAPI_ISteamClient_GetISteamGameSearch( void * self, int a1, int a2, const void * a3 ) { EMU_LOG("compat:SteamAPI_ISteamClient_GetISteamGameSearch"); return &g_gameSearchStub; }
S_API void * S_CALLTYPE SteamAPI_ISteamClient_GetISteamMusicRemote( void * self, int a1, int a2, const void * a3 ) { EMU_LOG("compat:SteamAPI_ISteamClient_GetISteamMusicRemote"); return &g_musicRemoteStub; }
S_API uint32 S_CALLTYPE SteamAPI_ISteamFriends_GetUserRestrictions( void * self ) { EMU_LOG("compat:SteamAPI_ISteamFriends_GetUserRestrictions"); return 0; }
S_API SteamAPICall_t S_CALLTYPE SteamAPI_ISteamFriends_SetPersonaName( void * self, const void * a1 ) { EMU_LOG("compat:SteamAPI_ISteamFriends_SetPersonaName"); return 0; }
S_API int S_CALLTYPE SteamAPI_ISteamGameSearch_AcceptGame( void * self ) { EMU_LOG("compat:SteamAPI_ISteamGameSearch_AcceptGame"); return 1; }
S_API int S_CALLTYPE SteamAPI_ISteamGameSearch_AddGameSearchParams( void * self, const void * a1, const void * a2 ) { EMU_LOG("compat:SteamAPI_ISteamGameSearch_AddGameSearchParams"); return 1; }
S_API int S_CALLTYPE SteamAPI_ISteamGameSearch_CancelRequestPlayersForGame( void * self ) { EMU_LOG("compat:SteamAPI_ISteamGameSearch_CancelRequestPlayersForGame"); return 1; }
S_API int S_CALLTYPE SteamAPI_ISteamGameSearch_DeclineGame( void * self ) { EMU_LOG("compat:SteamAPI_ISteamGameSearch_DeclineGame"); return 1; }
S_API int S_CALLTYPE SteamAPI_ISteamGameSearch_EndGame( void * self, uint64 a1 ) { EMU_LOG("compat:SteamAPI_ISteamGameSearch_EndGame"); return 1; }
S_API int S_CALLTYPE SteamAPI_ISteamGameSearch_EndGameSearch( void * self ) { EMU_LOG("compat:SteamAPI_ISteamGameSearch_EndGameSearch"); return 1; }
S_API int S_CALLTYPE SteamAPI_ISteamGameSearch_HostConfirmGameStart( void * self, uint64 a1 ) { EMU_LOG("compat:SteamAPI_ISteamGameSearch_HostConfirmGameStart"); return 1; }
S_API int S_CALLTYPE SteamAPI_ISteamGameSearch_RequestPlayersForGame( void * self, int a1, int a2, int a3 ) { EMU_LOG("compat:SteamAPI_ISteamGameSearch_RequestPlayersForGame"); return 1; }
S_API int S_CALLTYPE SteamAPI_ISteamGameSearch_RetrieveConnectionDetails( void * self, uint64 a1, void * a2, int a3 ) { EMU_LOG("compat:SteamAPI_ISteamGameSearch_RetrieveConnectionDetails"); return 1; }
S_API int S_CALLTYPE SteamAPI_ISteamGameSearch_SearchForGameSolo( void * self, int a1, int a2 ) { EMU_LOG("compat:SteamAPI_ISteamGameSearch_SearchForGameSolo"); return 1; }
S_API int S_CALLTYPE SteamAPI_ISteamGameSearch_SearchForGameWithLobby( void * self, uint64 a1, int a2, int a3 ) { EMU_LOG("compat:SteamAPI_ISteamGameSearch_SearchForGameWithLobby"); return 1; }
S_API int S_CALLTYPE SteamAPI_ISteamGameSearch_SetConnectionDetails( void * self, const void * a1, int a2 ) { EMU_LOG("compat:SteamAPI_ISteamGameSearch_SetConnectionDetails"); return 1; }
S_API int S_CALLTYPE SteamAPI_ISteamGameSearch_SetGameHostParams( void * self, const void * a1, const void * a2 ) { EMU_LOG("compat:SteamAPI_ISteamGameSearch_SetGameHostParams"); return 1; }
S_API int S_CALLTYPE SteamAPI_ISteamGameSearch_SubmitPlayerResult( void * self, uint64 a1, uint64 a2, int a3 ) { EMU_LOG("compat:SteamAPI_ISteamGameSearch_SubmitPlayerResult"); return 1; }
S_API bool S_CALLTYPE SteamAPI_ISteamMusicRemote_BActivationSuccess( void * self, bool a1 ) { EMU_LOG("compat:SteamAPI_ISteamMusicRemote_BActivationSuccess"); return false; }
S_API bool S_CALLTYPE SteamAPI_ISteamMusicRemote_BIsCurrentMusicRemote( void * self ) { EMU_LOG("compat:SteamAPI_ISteamMusicRemote_BIsCurrentMusicRemote"); return false; }
S_API bool S_CALLTYPE SteamAPI_ISteamMusicRemote_CurrentEntryDidChange( void * self ) { EMU_LOG("compat:SteamAPI_ISteamMusicRemote_CurrentEntryDidChange"); return false; }
S_API bool S_CALLTYPE SteamAPI_ISteamMusicRemote_CurrentEntryIsAvailable( void * self, bool a1 ) { EMU_LOG("compat:SteamAPI_ISteamMusicRemote_CurrentEntryIsAvailable"); return false; }
S_API bool S_CALLTYPE SteamAPI_ISteamMusicRemote_CurrentEntryWillChange( void * self ) { EMU_LOG("compat:SteamAPI_ISteamMusicRemote_CurrentEntryWillChange"); return false; }
S_API bool S_CALLTYPE SteamAPI_ISteamMusicRemote_DeregisterSteamMusicRemote( void * self ) { EMU_LOG("compat:SteamAPI_ISteamMusicRemote_DeregisterSteamMusicRemote"); return false; }
S_API bool S_CALLTYPE SteamAPI_ISteamMusicRemote_EnableLooped( void * self, bool a1 ) { EMU_LOG("compat:SteamAPI_ISteamMusicRemote_EnableLooped"); return false; }
S_API bool S_CALLTYPE SteamAPI_ISteamMusicRemote_EnablePlayNext( void * self, bool a1 ) { EMU_LOG("compat:SteamAPI_ISteamMusicRemote_EnablePlayNext"); return false; }
S_API bool S_CALLTYPE SteamAPI_ISteamMusicRemote_EnablePlayPrevious( void * self, bool a1 ) { EMU_LOG("compat:SteamAPI_ISteamMusicRemote_EnablePlayPrevious"); return false; }
S_API bool S_CALLTYPE SteamAPI_ISteamMusicRemote_EnablePlaylists( void * self, bool a1 ) { EMU_LOG("compat:SteamAPI_ISteamMusicRemote_EnablePlaylists"); return false; }
S_API bool S_CALLTYPE SteamAPI_ISteamMusicRemote_EnableQueue( void * self, bool a1 ) { EMU_LOG("compat:SteamAPI_ISteamMusicRemote_EnableQueue"); return false; }
S_API bool S_CALLTYPE SteamAPI_ISteamMusicRemote_EnableShuffled( void * self, bool a1 ) { EMU_LOG("compat:SteamAPI_ISteamMusicRemote_EnableShuffled"); return false; }
S_API bool S_CALLTYPE SteamAPI_ISteamMusicRemote_PlaylistDidChange( void * self ) { EMU_LOG("compat:SteamAPI_ISteamMusicRemote_PlaylistDidChange"); return false; }
S_API bool S_CALLTYPE SteamAPI_ISteamMusicRemote_PlaylistWillChange( void * self ) { EMU_LOG("compat:SteamAPI_ISteamMusicRemote_PlaylistWillChange"); return false; }
S_API bool S_CALLTYPE SteamAPI_ISteamMusicRemote_QueueDidChange( void * self ) { EMU_LOG("compat:SteamAPI_ISteamMusicRemote_QueueDidChange"); return false; }
S_API bool S_CALLTYPE SteamAPI_ISteamMusicRemote_QueueWillChange( void * self ) { EMU_LOG("compat:SteamAPI_ISteamMusicRemote_QueueWillChange"); return false; }
S_API bool S_CALLTYPE SteamAPI_ISteamMusicRemote_RegisterSteamMusicRemote( void * self, const void * a1 ) { EMU_LOG("compat:SteamAPI_ISteamMusicRemote_RegisterSteamMusicRemote"); return false; }
S_API bool S_CALLTYPE SteamAPI_ISteamMusicRemote_ResetPlaylistEntries( void * self ) { EMU_LOG("compat:SteamAPI_ISteamMusicRemote_ResetPlaylistEntries"); return false; }
S_API bool S_CALLTYPE SteamAPI_ISteamMusicRemote_ResetQueueEntries( void * self ) { EMU_LOG("compat:SteamAPI_ISteamMusicRemote_ResetQueueEntries"); return false; }
S_API bool S_CALLTYPE SteamAPI_ISteamMusicRemote_SetCurrentPlaylistEntry( void * self, int a1 ) { EMU_LOG("compat:SteamAPI_ISteamMusicRemote_SetCurrentPlaylistEntry"); return false; }
S_API bool S_CALLTYPE SteamAPI_ISteamMusicRemote_SetCurrentQueueEntry( void * self, int a1 ) { EMU_LOG("compat:SteamAPI_ISteamMusicRemote_SetCurrentQueueEntry"); return false; }
S_API bool S_CALLTYPE SteamAPI_ISteamMusicRemote_SetDisplayName( void * self, const void * a1 ) { EMU_LOG("compat:SteamAPI_ISteamMusicRemote_SetDisplayName"); return false; }
S_API bool S_CALLTYPE SteamAPI_ISteamMusicRemote_SetPNGIcon_64x64( void * self, void * a1, uint32 a2 ) { EMU_LOG("compat:SteamAPI_ISteamMusicRemote_SetPNGIcon_64x64"); return false; }
S_API bool S_CALLTYPE SteamAPI_ISteamMusicRemote_SetPlaylistEntry( void * self, int a1, int a2, const void * a3 ) { EMU_LOG("compat:SteamAPI_ISteamMusicRemote_SetPlaylistEntry"); return false; }
S_API bool S_CALLTYPE SteamAPI_ISteamMusicRemote_SetQueueEntry( void * self, int a1, int a2, const void * a3 ) { EMU_LOG("compat:SteamAPI_ISteamMusicRemote_SetQueueEntry"); return false; }
S_API bool S_CALLTYPE SteamAPI_ISteamMusicRemote_UpdateCurrentEntryCoverArt( void * self, void * a1, uint32 a2 ) { EMU_LOG("compat:SteamAPI_ISteamMusicRemote_UpdateCurrentEntryCoverArt"); return false; }
S_API bool S_CALLTYPE SteamAPI_ISteamMusicRemote_UpdateCurrentEntryElapsedSeconds( void * self, int a1 ) { EMU_LOG("compat:SteamAPI_ISteamMusicRemote_UpdateCurrentEntryElapsedSeconds"); return false; }
S_API bool S_CALLTYPE SteamAPI_ISteamMusicRemote_UpdateCurrentEntryText( void * self, const void * a1 ) { EMU_LOG("compat:SteamAPI_ISteamMusicRemote_UpdateCurrentEntryText"); return false; }
S_API bool S_CALLTYPE SteamAPI_ISteamMusicRemote_UpdateLooped( void * self, bool a1 ) { EMU_LOG("compat:SteamAPI_ISteamMusicRemote_UpdateLooped"); return false; }
S_API bool S_CALLTYPE SteamAPI_ISteamMusicRemote_UpdatePlaybackStatus( void * self, int a1 ) { EMU_LOG("compat:SteamAPI_ISteamMusicRemote_UpdatePlaybackStatus"); return false; }
S_API bool S_CALLTYPE SteamAPI_ISteamMusicRemote_UpdateShuffled( void * self, bool a1 ) { EMU_LOG("compat:SteamAPI_ISteamMusicRemote_UpdateShuffled"); return false; }
S_API bool S_CALLTYPE SteamAPI_ISteamMusicRemote_UpdateVolume( void * self, float a1 ) { EMU_LOG("compat:SteamAPI_ISteamMusicRemote_UpdateVolume"); return false; }
S_API bool S_CALLTYPE SteamAPI_ISteamRemotePlay_BStartRemotePlayTogether( void * self, bool a1 ) { EMU_LOG("compat:SteamAPI_ISteamRemotePlay_BStartRemotePlayTogether"); return false; }
S_API void S_CALLTYPE SteamAPI_ISteamTimeline_AddTimelineEvent( void * self, const void * a1, const void * a2, const void * a3, uint32 a4, float a5, float a6, int a7 ) { EMU_LOG("compat:SteamAPI_ISteamTimeline_AddTimelineEvent"); }
S_API void S_CALLTYPE SteamAPI_ISteamTimeline_ClearTimelineStateDescription( void * self, float a1 ) { EMU_LOG("compat:SteamAPI_ISteamTimeline_ClearTimelineStateDescription"); }
S_API void S_CALLTYPE SteamAPI_ISteamTimeline_SetTimelineStateDescription( void * self, const void * a1, float a2 ) { EMU_LOG("compat:SteamAPI_ISteamTimeline_SetTimelineStateDescription"); }
S_API bool S_CALLTYPE SteamAPI_ISteamUserStats_RequestCurrentStats( void * self ) { EMU_LOG("compat:SteamAPI_ISteamUserStats_RequestCurrentStats"); UserStatsReceived_t r{}; r.m_eResult = k_EResultOK; r.m_nGameID = emu::AppID(); r.m_steamIDUser = emu::LocalSteamID(); emu::QueueCallback(r); return true; }
S_API void S_CALLTYPE SteamAPI_SteamNetworkingIPAddr_Clear( void * self ) { EMU_LOG("compat:SteamAPI_SteamNetworkingIPAddr_Clear"); }
S_API int S_CALLTYPE SteamAPI_SteamNetworkingIPAddr_GetFakeIPType( void * self ) { EMU_LOG("compat:SteamAPI_SteamNetworkingIPAddr_GetFakeIPType"); return 0; }
S_API uint32 S_CALLTYPE SteamAPI_SteamNetworkingIPAddr_GetIPv4( void * self ) { EMU_LOG("compat:SteamAPI_SteamNetworkingIPAddr_GetIPv4"); return 0; }
S_API bool S_CALLTYPE SteamAPI_SteamNetworkingIPAddr_IsEqualTo( void * self, const void * a1 ) { EMU_LOG("compat:SteamAPI_SteamNetworkingIPAddr_IsEqualTo"); return false; }
S_API bool S_CALLTYPE SteamAPI_SteamNetworkingIPAddr_IsFakeIP( void * self ) { EMU_LOG("compat:SteamAPI_SteamNetworkingIPAddr_IsFakeIP"); return false; }
S_API bool S_CALLTYPE SteamAPI_SteamNetworkingIPAddr_IsIPv4( void * self ) { EMU_LOG("compat:SteamAPI_SteamNetworkingIPAddr_IsIPv4"); return false; }
S_API bool S_CALLTYPE SteamAPI_SteamNetworkingIPAddr_IsIPv6AllZeros( void * self ) { EMU_LOG("compat:SteamAPI_SteamNetworkingIPAddr_IsIPv6AllZeros"); return false; }
S_API bool S_CALLTYPE SteamAPI_SteamNetworkingIPAddr_IsLocalHost( void * self ) { EMU_LOG("compat:SteamAPI_SteamNetworkingIPAddr_IsLocalHost"); return false; }
S_API bool S_CALLTYPE SteamAPI_SteamNetworkingIPAddr_ParseString( void * self, const void * a1 ) { EMU_LOG("compat:SteamAPI_SteamNetworkingIPAddr_ParseString"); return false; }
S_API void S_CALLTYPE SteamAPI_SteamNetworkingIPAddr_SetIPv4( void * self, uint32 a1, uint16 a2 ) { EMU_LOG("compat:SteamAPI_SteamNetworkingIPAddr_SetIPv4"); }
S_API void S_CALLTYPE SteamAPI_SteamNetworkingIPAddr_SetIPv6( void * self, const void * a1, uint16 a2 ) { EMU_LOG("compat:SteamAPI_SteamNetworkingIPAddr_SetIPv6"); }
S_API void S_CALLTYPE SteamAPI_SteamNetworkingIPAddr_SetIPv6LocalHost( void * self, uint16 a1 ) { EMU_LOG("compat:SteamAPI_SteamNetworkingIPAddr_SetIPv6LocalHost"); }
S_API void S_CALLTYPE SteamAPI_SteamNetworkingIPAddr_ToString( void * self, void * a1, uint32 a2, bool a3 ) { EMU_LOG("compat:SteamAPI_SteamNetworkingIPAddr_ToString"); }
S_API void S_CALLTYPE SteamAPI_SteamNetworkingIdentity_Clear( void * self ) { EMU_LOG("compat:SteamAPI_SteamNetworkingIdentity_Clear"); }
S_API int S_CALLTYPE SteamAPI_SteamNetworkingIdentity_GetFakeIPType( void * self ) { EMU_LOG("compat:SteamAPI_SteamNetworkingIdentity_GetFakeIPType"); return 0; }
S_API void * S_CALLTYPE SteamAPI_SteamNetworkingIdentity_GetGenericBytes( void * self, void * a1 ) { EMU_LOG("compat:SteamAPI_SteamNetworkingIdentity_GetGenericBytes"); return nullptr; }
S_API const char * S_CALLTYPE SteamAPI_SteamNetworkingIdentity_GetGenericString( void * self ) { EMU_LOG("compat:SteamAPI_SteamNetworkingIdentity_GetGenericString"); return ""; }
S_API void * S_CALLTYPE SteamAPI_SteamNetworkingIdentity_GetIPAddr( void * self ) { EMU_LOG("compat:SteamAPI_SteamNetworkingIdentity_GetIPAddr"); return nullptr; }
S_API uint32 S_CALLTYPE SteamAPI_SteamNetworkingIdentity_GetIPv4( void * self ) { EMU_LOG("compat:SteamAPI_SteamNetworkingIdentity_GetIPv4"); return 0; }
S_API uint64 S_CALLTYPE SteamAPI_SteamNetworkingIdentity_GetPSNID( void * self ) { EMU_LOG("compat:SteamAPI_SteamNetworkingIdentity_GetPSNID"); return 0; }
S_API uint64 S_CALLTYPE SteamAPI_SteamNetworkingIdentity_GetStadiaID( void * self ) { EMU_LOG("compat:SteamAPI_SteamNetworkingIdentity_GetStadiaID"); return 0; }
S_API uint64 S_CALLTYPE SteamAPI_SteamNetworkingIdentity_GetSteamID( void * self ) { EMU_LOG("compat:SteamAPI_SteamNetworkingIdentity_GetSteamID"); return 0; }
S_API uint64 S_CALLTYPE SteamAPI_SteamNetworkingIdentity_GetSteamID64( void * self ) { EMU_LOG("compat:SteamAPI_SteamNetworkingIdentity_GetSteamID64"); return 0; }
S_API const char * S_CALLTYPE SteamAPI_SteamNetworkingIdentity_GetXboxPairwiseID( void * self ) { EMU_LOG("compat:SteamAPI_SteamNetworkingIdentity_GetXboxPairwiseID"); return ""; }
S_API bool S_CALLTYPE SteamAPI_SteamNetworkingIdentity_IsEqualTo( void * self, const void * a1 ) { EMU_LOG("compat:SteamAPI_SteamNetworkingIdentity_IsEqualTo"); return false; }
S_API bool S_CALLTYPE SteamAPI_SteamNetworkingIdentity_IsFakeIP( void * self ) { EMU_LOG("compat:SteamAPI_SteamNetworkingIdentity_IsFakeIP"); return false; }
S_API bool S_CALLTYPE SteamAPI_SteamNetworkingIdentity_IsInvalid( void * self ) { EMU_LOG("compat:SteamAPI_SteamNetworkingIdentity_IsInvalid"); return false; }
S_API bool S_CALLTYPE SteamAPI_SteamNetworkingIdentity_IsLocalHost( void * self ) { EMU_LOG("compat:SteamAPI_SteamNetworkingIdentity_IsLocalHost"); return false; }
S_API bool S_CALLTYPE SteamAPI_SteamNetworkingIdentity_ParseString( void * self, const void * a1 ) { EMU_LOG("compat:SteamAPI_SteamNetworkingIdentity_ParseString"); return false; }
S_API bool S_CALLTYPE SteamAPI_SteamNetworkingIdentity_SetGenericBytes( void * self, const void * a1, uint32 a2 ) { EMU_LOG("compat:SteamAPI_SteamNetworkingIdentity_SetGenericBytes"); return false; }
S_API bool S_CALLTYPE SteamAPI_SteamNetworkingIdentity_SetGenericString( void * self, const void * a1 ) { EMU_LOG("compat:SteamAPI_SteamNetworkingIdentity_SetGenericString"); return false; }
S_API void S_CALLTYPE SteamAPI_SteamNetworkingIdentity_SetIPAddr( void * self, const void * a1 ) { EMU_LOG("compat:SteamAPI_SteamNetworkingIdentity_SetIPAddr"); }
S_API void S_CALLTYPE SteamAPI_SteamNetworkingIdentity_SetIPv4Addr( void * self, uint32 a1, uint16 a2 ) { EMU_LOG("compat:SteamAPI_SteamNetworkingIdentity_SetIPv4Addr"); }
S_API void S_CALLTYPE SteamAPI_SteamNetworkingIdentity_SetLocalHost( void * self ) { EMU_LOG("compat:SteamAPI_SteamNetworkingIdentity_SetLocalHost"); }
S_API void S_CALLTYPE SteamAPI_SteamNetworkingIdentity_SetPSNID( void * self, uint64 a1 ) { EMU_LOG("compat:SteamAPI_SteamNetworkingIdentity_SetPSNID"); }
S_API void S_CALLTYPE SteamAPI_SteamNetworkingIdentity_SetStadiaID( void * self, uint64 a1 ) { EMU_LOG("compat:SteamAPI_SteamNetworkingIdentity_SetStadiaID"); }
S_API void S_CALLTYPE SteamAPI_SteamNetworkingIdentity_SetSteamID( void * self, uint64 a1 ) { EMU_LOG("compat:SteamAPI_SteamNetworkingIdentity_SetSteamID"); }
S_API void S_CALLTYPE SteamAPI_SteamNetworkingIdentity_SetSteamID64( void * self, uint64 a1 ) { EMU_LOG("compat:SteamAPI_SteamNetworkingIdentity_SetSteamID64"); }
S_API bool S_CALLTYPE SteamAPI_SteamNetworkingIdentity_SetXboxPairwiseID( void * self, const void * a1 ) { EMU_LOG("compat:SteamAPI_SteamNetworkingIdentity_SetXboxPairwiseID"); return false; }
S_API void S_CALLTYPE SteamAPI_SteamNetworkingIdentity_ToString( void * self, void * a1, uint32 a2 ) { EMU_LOG("compat:SteamAPI_SteamNetworkingIdentity_ToString"); }
S_API void S_CALLTYPE SteamAPI_SteamNetworkingMessage_t_Release( void * self ) { EMU_LOG("compat:SteamAPI_SteamNetworkingMessage_t_Release"); }

} // extern "C"
