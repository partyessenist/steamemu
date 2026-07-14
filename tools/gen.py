#!/usr/bin/env python3
"""steamemu code generator.

Reads the reference SDK's steam_api.json (machine-readable description of every
interface, method, callback and struct) and emits, into src/generated/:

  interfaces.h   concrete emu::C<Iface> classes, one per interface, that INHERIT
                 the reference ISteam* header and implement every pure virtual.
                 Because they derive from the real header, the compiler emits the
                 vtable in the exact slot order the game was built against -- the
                 layout is never hand-built.  Method bodies are logged neutral
                 stubs unless overridden in BODIES below.

  flat_api.cpp   every SteamAPI_ISteam<Iface>_<Method> flat C export, forwarding
                 to the same vtable method.  Only CSteamID/CGameID marshal (they
                 cross the flat boundary as uint64), matching steam_api_flat.h.

  registry.cpp   per-interface singletons + emu::FindInterface(version) used by
                 the SteamInternal_* / GetISteamGenericInterface location plumbing.

The whole surface is mechanical, so we generate it: version bumps and new methods
stay consistent and cheap.  Real behavior lives in BODIES (keyed by interface +
method) so regeneration never clobbers it.  Do not edit the generated files by
hand -- edit this script and re-run it (see tools/regen.sh / the custom target).
"""
import json
import os
import re
import sys

import oldversions

SDK = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "third_party", "SteamworksSDK")
OUT = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), "..", "src", "generated")
JSON = os.path.join(SDK, "public", "steam", "steam_api.json")

# ---------------------------------------------------------------------------
# Interface -> reference header that declares it.  (Callback-sink interfaces
# the GAME implements -- the *Response interfaces -- are intentionally absent;
# we never instantiate those.)
# ---------------------------------------------------------------------------
HEADERS = {
    "ISteamClient": "isteamclient.h",
    "ISteamUser": "isteamuser.h",
    "ISteamFriends": "isteamfriends.h",
    "ISteamUtils": "isteamutils.h",
    "ISteamMatchmaking": "isteammatchmaking.h",
    "ISteamMatchmakingServers": "isteammatchmaking.h",
    "ISteamParties": "isteammatchmaking.h",
    "ISteamRemoteStorage": "isteamremotestorage.h",
    "ISteamUserStats": "isteamuserstats.h",
    "ISteamApps": "isteamapps.h",
    "ISteamNetworking": "isteamnetworking.h",
    "ISteamScreenshots": "isteamscreenshots.h",
    "ISteamMusic": "isteammusic.h",
    "ISteamHTTP": "isteamhttp.h",
    "ISteamInput": "isteaminput.h",
    "ISteamController": "isteamcontroller.h",
    "ISteamUGC": "isteamugc.h",
    "ISteamHTMLSurface": "isteamhtmlsurface.h",
    "ISteamInventory": "isteaminventory.h",
    "ISteamTimeline": "isteamtimeline.h",
    "ISteamVideo": "isteamvideo.h",
    "ISteamParentalSettings": "isteamparentalsettings.h",
    "ISteamRemotePlay": "isteamremoteplay.h",
    "ISteamNetworkingMessages": "isteamnetworkingmessages.h",
    "ISteamNetworkingSockets": "isteamnetworkingsockets.h",
    "ISteamNetworkingUtils": "isteamnetworkingutils.h",
    "ISteamGameServer": "isteamgameserver.h",
    "ISteamGameServerStats": "isteamgameserverstats.h",
    # ISteamNetworkingFakeUDPPort is intentionally omitted: its definition is
    # gated behind an SDR build macro, so in our configuration it is only a
    # forward declaration (incomplete type) and cannot be inherited. Methods
    # that would return one (CreateFakeUDPPort) fall back to the neutral
    # nullptr, which is the correct "no relay networking" behavior.
}

# Interface -> exact version string it is located by (from *_INTERFACE_VERSION).
VERSIONS = {
    "ISteamClient": "SteamClient023",
    "ISteamUser": "SteamUser023",
    "ISteamFriends": "SteamFriends018",
    "ISteamUtils": "SteamUtils010",
    "ISteamMatchmaking": "SteamMatchMaking009",
    "ISteamMatchmakingServers": "SteamMatchMakingServers002",
    "ISteamParties": "SteamParties002",
    "ISteamRemoteStorage": "STEAMREMOTESTORAGE_INTERFACE_VERSION016",
    "ISteamUserStats": "STEAMUSERSTATS_INTERFACE_VERSION013",
    "ISteamApps": "STEAMAPPS_INTERFACE_VERSION009",
    "ISteamNetworking": "SteamNetworking006",
    "ISteamScreenshots": "STEAMSCREENSHOTS_INTERFACE_VERSION003",
    "ISteamMusic": "STEAMMUSIC_INTERFACE_VERSION001",
    "ISteamHTTP": "STEAMHTTP_INTERFACE_VERSION003",
    "ISteamInput": "SteamInput006",
    "ISteamController": "SteamController008",
    "ISteamUGC": "STEAMUGC_INTERFACE_VERSION021",
    "ISteamHTMLSurface": "STEAMHTMLSURFACE_INTERFACE_VERSION_005",
    "ISteamInventory": "STEAMINVENTORY_INTERFACE_V003",
    "ISteamTimeline": "STEAMTIMELINE_INTERFACE_V004",
    "ISteamVideo": "STEAMVIDEO_INTERFACE_V007",
    "ISteamParentalSettings": "STEAMPARENTALSETTINGS_INTERFACE_VERSION001",
    "ISteamRemotePlay": "STEAMREMOTEPLAY_INTERFACE_VERSION004",
    "ISteamNetworkingMessages": "SteamNetworkingMessages002",
    "ISteamNetworkingSockets": "SteamNetworkingSockets012",
    "ISteamNetworkingUtils": "SteamNetworkingUtils004",
    "ISteamGameServer": "SteamGameServer015",
    "ISteamGameServerStats": "SteamGameServerStats001",
}

# Interfaces that exist separately on the client AND game-server side of one
# process (SteamGameServerNetworking etc.). Each gets a second, game-server
# singleton (<Accessor>GS_(), m_gs = true) returned by the game-server locators;
# state is still shared through the emu:: subsystems -- m_gs only switches the
# sending/receiving identity where that matters (legacy P2P today).
GS_SHARED = ["ISteamNetworking", "ISteamNetworkingSockets", "ISteamNetworkingMessages"]

# Interfaces the GAME implements (server-browser callback sinks). We never
# instantiate them, but the flat C API still exports thunks so C bindings can
# invoke a game-supplied object -- so we generate their flat wrappers only, no
# concrete class and no registry entry.
FLAT_ONLY = [
    "ISteamMatchmakingServerListResponse",
    "ISteamMatchmakingPingResponse",
    "ISteamMatchmakingPlayersResponse",
    "ISteamMatchmakingRulesResponse",
]

# ISteamNetworkingFakeUDPPort is only forward-declared in the public SDK (Valve
# compiles its wrappers internally), so we cannot forward through it. Provide the
# exports as neutral stubs so the symbols exist for dynamic loaders.
FAKEUDP_STUBS = """// ---- ISteamNetworkingFakeUDPPort (opaque in public SDK: neutral stubs) ----
S_API void SteamAPI_ISteamNetworkingFakeUDPPort_DestroyFakeUDPPort( ISteamNetworkingFakeUDPPort* self ) { (void)self; }
S_API EResult SteamAPI_ISteamNetworkingFakeUDPPort_SendMessageToFakeIP( ISteamNetworkingFakeUDPPort* self, const SteamNetworkingIPAddr & remoteAddress, const void * pData, uint32 cbData, int nSendFlags ) { (void)self; (void)remoteAddress; (void)pData; (void)cbData; (void)nSendFlags; return k_EResultFail; }
S_API int SteamAPI_ISteamNetworkingFakeUDPPort_ReceiveMessages( ISteamNetworkingFakeUDPPort* self, SteamNetworkingMessage_t ** ppOutMessages, int nMaxMessages ) { (void)self; (void)ppOutMessages; (void)nMaxMessages; return 0; }
S_API void SteamAPI_ISteamNetworkingFakeUDPPort_ScheduleCleanup( ISteamNetworkingFakeUDPPort* self, const SteamNetworkingIPAddr & remoteAddress ) { (void)self; (void)remoteAddress; }
"""

# Interfaces reachable through ISteamClient::GetISteam* and the version-keyed
# locator.  ISteamNetworkingFakeUDPPort is created on demand, not located, so it
# has a class but no singleton/registry entry.
NO_SINGLETON = {"ISteamNetworkingFakeUDPPort"}

# ---------------------------------------------------------------------------
# Real behavior.  Keyed by "IFace::Method"; value is the C++ body (the EMU_LOG
# is emitted for us).  Everything not listed here is a neutral stub.  Keep this
# the ONLY place hand-written logic lives so regeneration is safe.  Do not add
# overloaded methods here (the key can't disambiguate them).
# ---------------------------------------------------------------------------
BODIES = {
    # -- ISteamClient: hand back our interface singletons ------------------
    "ISteamClient::CreateSteamPipe": "return emu::kHSteamPipe;",
    "ISteamClient::BReleaseSteamPipe": "return true;",
    "ISteamClient::ConnectToGlobalUser": "return emu::kHSteamUser;",
    "ISteamClient::CreateLocalUser": "if (phSteamPipe) *phSteamPipe = emu::kHSteamPipe; return emu::kHSteamUser;",
    # Each GetISteam* accessor version-dispatches on pchVersion -- a game that
    # fetches an OLDER interface via the client vtable gets that version's
    # sub-object, exactly like GetISteamGenericInterface. (Written as direct
    # <Iface>_ForVersion calls; those functions exist whether or not the
    # historical extraction ran -- newest-only builds just have no legacy arms.)
    "ISteamClient::GetISteamUser": "return (ISteamUser *)emu::SteamUser_ForVersion(pchVersion);",
    "ISteamClient::GetISteamFriends": "return (ISteamFriends *)emu::SteamFriends_ForVersion(pchVersion);",
    "ISteamClient::GetISteamUtils": "return (ISteamUtils *)emu::SteamUtils_ForVersion(pchVersion);",
    "ISteamClient::GetISteamMatchmaking": "return (ISteamMatchmaking *)emu::SteamMatchmaking_ForVersion(pchVersion);",
    "ISteamClient::GetISteamMatchmakingServers": "return (ISteamMatchmakingServers *)emu::SteamMatchmakingServers_ForVersion(pchVersion);",
    "ISteamClient::GetISteamUserStats": "return (ISteamUserStats *)emu::SteamUserStats_ForVersion(pchVersion);",
    "ISteamClient::GetISteamApps": "return (ISteamApps *)emu::SteamApps_ForVersion(pchVersion);",
    # The game-server user handle gets the game-server-side instance: both paths still version-dispatch.
    "ISteamClient::GetISteamNetworking": "return (ISteamNetworking *)(hSteamUser == emu::kHSteamUserGameServer ? emu::SteamNetworking_ForVersionGS(pchVersion) : emu::SteamNetworking_ForVersion(pchVersion));",
    "ISteamClient::GetISteamRemoteStorage": "return (ISteamRemoteStorage *)emu::SteamRemoteStorage_ForVersion(pchVersion);",
    "ISteamClient::GetISteamScreenshots": "return (ISteamScreenshots *)emu::SteamScreenshots_ForVersion(pchVersion);",
    "ISteamClient::GetISteamHTTP": "return (ISteamHTTP *)emu::SteamHTTP_ForVersion(pchVersion);",
    "ISteamClient::GetISteamController": "return (ISteamController *)emu::SteamController_ForVersion(pchVersion);",
    "ISteamClient::GetISteamUGC": "return (ISteamUGC *)emu::SteamUGC_ForVersion(pchVersion);",
    "ISteamClient::GetISteamMusic": "return (ISteamMusic *)emu::SteamMusic_ForVersion(pchVersion);",
    "ISteamClient::GetISteamHTMLSurface": "return (ISteamHTMLSurface *)emu::SteamHTMLSurface_ForVersion(pchVersion);",
    "ISteamClient::GetISteamInventory": "return (ISteamInventory *)emu::SteamInventory_ForVersion(pchVersion);",
    "ISteamClient::GetISteamVideo": "return (ISteamVideo *)emu::SteamVideo_ForVersion(pchVersion);",
    "ISteamClient::GetISteamParentalSettings": "return (ISteamParentalSettings *)emu::SteamParentalSettings_ForVersion(pchVersion);",
    "ISteamClient::GetISteamInput": "return (ISteamInput *)emu::SteamInput_ForVersion(pchVersion);",
    "ISteamClient::GetISteamParties": "return (ISteamParties *)emu::SteamParties_ForVersion(pchVersion);",
    "ISteamClient::GetISteamRemotePlay": "return (ISteamRemotePlay *)emu::SteamRemotePlay_ForVersion(pchVersion);",
    "ISteamClient::GetISteamGameServer": "return (ISteamGameServer *)emu::SteamGameServer_ForVersion(pchVersion);",
    "ISteamClient::GetISteamGameServerStats": "return (ISteamGameServerStats *)emu::SteamGameServerStats_ForVersion(pchVersion);",
    "ISteamClient::GetISteamGenericInterface": "return hSteamUser == emu::kHSteamUserGameServer ? emu::FindInterfaceGS(pchVersion) : emu::FindInterface(pchVersion);",
    "ISteamClient::BShutdownIfAllPipesClosed": "return true;",
    # -- ISteamUser: identity / login / auth --------------------------------
    "ISteamUser::GetHSteamUser": "return emu::kHSteamUser;",
    "ISteamUser::BLoggedOn": "return true;",
    "ISteamUser::GetSteamID": "return emu::LocalSteamID();",
    "ISteamUser::BIsBehindNAT": "return false;",
    "ISteamUser::GetPlayerSteamLevel": "return 1;",
    "ISteamUser::UserHasLicenseForApp": "return k_EUserHasLicenseResultHasLicense;",
    "ISteamUser::GetAuthSessionTicket": "return emu::Auth().GetAuthSessionTicket(pTicket, cbMaxTicket, pcbTicket);",
    "ISteamUser::GetAuthTicketForWebApi": "return emu::Auth().GetAuthTicketForWebApi(pchIdentity);",
    "ISteamUser::BeginAuthSession": "return emu::Auth().BeginAuthSession(pAuthTicket, cbAuthTicket, steamID);",
    "ISteamUser::EndAuthSession": "emu::Auth().EndAuthSession(steamID);",
    "ISteamUser::CancelAuthTicket": "emu::Auth().CancelAuthTicket(hAuthTicket);",
    "ISteamUser::GetUserDataFolder": "if (pchBuffer && cubBuffer > 0) std::snprintf(pchBuffer, cubBuffer, \"%s\", emu::UserDataDirC()); return true;",
    # Fabricated blob (identity + appid + included data); the sync getter hands
    # out the same bytes. Many games request this at startup for third-party
    # backends and hang without the completion.
    "ISteamUser::RequestEncryptedAppTicket": "EResult er = emu::Auth().RequestEncryptedAppTicket(pDataToInclude, cbDataToInclude); EncryptedAppTicketResponse_t r{}; r.m_eResult = er; return emu::QueueCallResult(r);",
    "ISteamUser::GetEncryptedAppTicket": "return emu::Auth().GetEncryptedAppTicket(pTicket, cbMaxTicket, pcbTicket);",
    # -- ISteamUser: voice (no microphone backend) --------------------------
    # NotRecording terminates the documented "call GetVoice until it returns
    # k_EVoiceResultNotRecording" loop; NoData would keep some games polling
    # a mic that will never produce bytes.
    "ISteamUser::GetAvailableVoice": "if (pcbCompressed) *pcbCompressed = 0; if (pcbUncompressed_Deprecated) *pcbUncompressed_Deprecated = 0; return k_EVoiceResultNotRecording;",
    "ISteamUser::GetVoice": "if (nBytesWritten) *nBytesWritten = 0; if (nUncompressBytesWritten_Deprecated) *nUncompressBytesWritten_Deprecated = 0; return k_EVoiceResultNotRecording;",
    "ISteamUser::DecompressVoice": "if (nBytesWritten) *nBytesWritten = 0; return k_EVoiceResultNoData;",
    # -- ISteamUtils: appid, locale, overlay ------------------------------
    "ISteamUtils::GetAppID": "return emu::AppID();",
    "ISteamUtils::GetIPCountry": "return emu::Config().Country().c_str();",
    "ISteamUtils::GetSteamUILanguage": "return emu::Config().UiLanguage().c_str();",
    "ISteamUtils::GetCurrentBatteryPower": "return 255;",  # k_EDisplayStatePluggedIn / on AC
    # Backed by the Dear ImGui overlay (src/overlay). Available() is true only
    # when the overlay was compiled in AND a backend is live (opt-in via
    # [overlay] enabled); games gate invite/notification features on it.
    "ISteamUtils::IsOverlayEnabled": "return emu::Overlay().Available();",
    # True while the overlay is showing, so a game that only presents an extra
    # frame when the overlay needs to redraw does so while it is up.
    "ISteamUtils::BOverlayNeedsPresent": "return emu::Overlay().IsVisible();",
    "ISteamUtils::IsSteamRunningInVR": "return false;",
    "ISteamUtils::IsSteamInBigPictureMode": "return false;",
    "ISteamUtils::IsSteamRunningOnSteamDeck": "return false;",
    "ISteamUtils::GetServerRealTime": "return emu::UnixTime();",
    "ISteamUtils::GetSecondsSinceAppActive": "return 0;",
    "ISteamUtils::GetSecondsSinceComputerActive": "return 0;",
    "ISteamUtils::GetConnectedUniverse": "return k_EUniversePublic;",
    # Manual-poll games spin on these against the utils vtable (not only the
    # SteamAPI_ManualDispatch_* exports) -- route them to the real dispatcher.
    "ISteamUtils::IsAPICallCompleted": "bool failed = false; bool done = emu::Dispatch().IsCallCompleted(hSteamAPICall, &failed); if (pbFailed) *pbFailed = failed; return done;",
    "ISteamUtils::GetAPICallResult": "return emu::Dispatch().GetAPICallResult(hSteamAPICall, pCallback, cubCallback, iCallbackExpected, pbFailed);",
    "ISteamUtils::GetImageSize": "return emu::AvatarImageSize(iImage, pnWidth, pnHeight);",
    "ISteamUtils::GetImageRGBA": "return emu::AvatarImageRGBA(iImage, pubDest, nDestBufferSize);",
    # No signing backend exists, so every file the game checks is "valid" --
    # failing here can make a game refuse to run its own content.
    "ISteamUtils::CheckFileSignature": "CheckFileSignature_t r{}; r.m_eCheckFileSignature = k_ECheckFileSignatureValidSignature; return emu::QueueCallResult(r);",
    "ISteamUtils::GetAPICallFailureReason": "return k_ESteamAPICallFailureNone;",
    # -- ISteamApps: ownership / install ----------------------------------
    "ISteamApps::BIsSubscribed": "return true;",
    "ISteamApps::BIsSubscribedApp": "return true;",
    "ISteamApps::BIsAppInstalled": "return true;",
    "ISteamApps::BIsDlcInstalled": "return emu::Config().HasDLC(appID);",
    "ISteamApps::BIsSubscribedFromFreeWeekend": "return false;",
    "ISteamApps::BIsVACBanned": "return false;",
    "ISteamApps::BIsCybercafe": "return false;",
    "ISteamApps::BIsLowViolence": "return false;",
    "ISteamApps::GetAppOwner": "return emu::LocalSteamID();",
    "ISteamApps::GetCurrentGameLanguage": "return emu::Config().Language().c_str();",
    "ISteamApps::GetAvailableGameLanguages": "return emu::Config().Language().c_str();",
    # Games locate content/config with this; empty breaks them. We run from the
    # game's directory, so the cwd is the install dir.
    "ISteamApps::GetAppInstallDir": "const char* d = emu::AppInstallDirC(); if (pchFolder && cchFolderBufferSize > 0) std::snprintf(pchFolder, cchFolderBufferSize, \"%s\", d); return (uint32)std::strlen(d) + 1;",
    "ISteamApps::GetEarliestPurchaseUnixTime": "return emu::UnixTime();",
    # -- ISteamFriends: presence ------------------------------------------
    "ISteamFriends::GetPersonaName": "return emu::PersonaName();",
    "ISteamFriends::GetPersonaState": "return k_EPersonaStateOnline;",
    "ISteamFriends::GetFriendCount": "return 0;",
    "ISteamFriends::GetFriendPersonaName": 'return "";',
    "ISteamFriends::SetRichPresence": "return true;",
    "ISteamFriends::GetFriendPersonaState": "return k_EPersonaStateOnline;",
    # -- ISteamFriends: discovered LAN peers surfaced as friends -----------
    "ISteamFriends::GetFriendCount": "return (int)emu::Net().Peers().size();",
    "ISteamFriends::GetFriendByIndex": "auto v = emu::Net().Peers(); return (iFriend >= 0 && iFriend < (int)v.size()) ? v[iFriend] : CSteamID();",
    # The local user is their own "friend" in a lobby (the host looking at their own
    # member entry); PeerNameC only knows remote peers, so answer our own persona for
    # our own id or the lobby would show the host with a blank name.
    "ISteamFriends::GetFriendPersonaName": "return (steamIDFriend == emu::LocalSteamID()) ? emu::PersonaName() : emu::Net().PeerNameC(steamIDFriend);",
    "ISteamFriends::GetFriendRelationship": "return emu::Net().IsPeer(steamIDFriend) ? k_EFriendRelationshipFriend : k_EFriendRelationshipNone;",
    # Real Steam returns NULL when no nickname is set.
    "ISteamFriends::GetPlayerNickname": "return nullptr;",
    # -- ISteamFriends: overlay activation ---------------------------------
    # Raise the Dear ImGui overlay (src/overlay). The dialog/URL string is
    # advisory -- we surface it as a toast and show the panel. No-op (but the
    # game gets its GameOverlayActivated_t via the visibility pump) when the
    # overlay is compiled out or not enabled.
    "ISteamFriends::ActivateGameOverlay": "emu::Overlay().Show(pchDialog);",
    "ISteamFriends::ActivateGameOverlayToUser": "(void)steamID; emu::Overlay().Show(pchDialog);",
    "ISteamFriends::ActivateGameOverlayToWebPage": "(void)eMode; emu::Overlay().Show(pchURL);",
    "ISteamFriends::ActivateGameOverlayToStore": "(void)nAppID; (void)eFlag; emu::Overlay().Show(\"store\");",
    "ISteamFriends::ActivateGameOverlayInviteDialog": "(void)steamIDLobby; emu::Overlay().Show(\"invite\");",
    "ISteamFriends::ActivateGameOverlayRemotePlayTogetherInviteDialog": "(void)steamIDLobby; emu::Overlay().Show(\"invite\");",
    "ISteamFriends::ActivateGameOverlayInviteDialogConnectString": "emu::Overlay().Show(pchConnectString);",
    # -- ISteamFriends: placeholder avatars ---------------------------------
    # Games poll these + GetImageSize/GetImageRGBA for every lobby member; 0
    # leaves permanent spinners. Return the handle immediately -- NEVER -1 from
    # GetLargeFriendAvatar ("pending") unless AvatarImageLoaded_t will fire.
    "ISteamFriends::GetSmallFriendAvatar": "return emu::AvatarImageHandle(steamIDFriend, 0);",
    "ISteamFriends::GetMediumFriendAvatar": "return emu::AvatarImageHandle(steamIDFriend, 1);",
    "ISteamFriends::GetLargeFriendAvatar": "return emu::AvatarImageHandle(steamIDFriend, 2);",
    # Post a PersonaStateChange_t for the requested user (real Steam does this once the
    # info is available). Some game requests info for the lobby owner right
    # after creating the lobby and only writes the lobby metadata (name/gamemode/
    # prefabdbversion) from its OnPersonaStateChanged handler -- without this callback
    # that handler never fires and only the guid ends up on the lobby.
    "ISteamFriends::RequestUserInformation": "if (!emu::FirstUserInfoRequest(steamIDUser)) return false; PersonaStateChange_t ps{}; ps.m_ulSteamID = steamIDUser.ConvertToUint64(); ps.m_nChangeFlags = k_EPersonaChangeName | k_EPersonaChangeNameFirstSet | k_EPersonaChangeComeOnline; emu::QueueCallback(ps); return true;",
    # -- legacy ISteamNetworking P2P over the LAN backend ------------------
    # m_gs: the game-server-side instance sends with (and receives traffic
    # addressed to) the LocalGSSteamID identity -- see NetBackend::SendP2PPacket.
    "ISteamNetworking::SendP2PPacket": "return emu::Net().SendP2PPacket(steamIDRemote, pubData, cubData, nChannel, m_gs);",
    "ISteamNetworking::IsP2PPacketAvailable": "return emu::Net().IsP2PPacketAvailable(pcubMsgSize, nChannel, m_gs);",
    "ISteamNetworking::ReadP2PPacket": "return emu::Net().ReadP2PPacket(pubDest, cubDest, pcubMsgSize, psteamIDRemote, nChannel, m_gs);",
    "ISteamNetworking::AcceptP2PSessionWithUser": "return true;",
    "ISteamNetworking::CloseP2PSessionWithUser": "return emu::Net().CloseP2PChannel(steamIDRemote, 0, m_gs);",
    "ISteamNetworking::GetP2PSessionState": "bool up = emu::Net().IsPeer(steamIDRemote); if (pConnectionState) { *pConnectionState = P2PSessionState_t{}; pConnectionState->m_bConnectionActive = up ? 1 : 0; } return up;",
    # -- ISteamMatchmaking: LAN lobbies -----------------------------------
    # Order matters: LobbyCreated_t (the call-result) must reach the game BEFORE the
    # LobbyEnter_t callback, or the game treats itself as having joined someone else's
    # lobby (read-only) instead of as the owner and never sets the lobby's data. Queue
    # the result first so the insertion-ordered manual-dispatch pump delivers it first.
    # Delay the completion (~0.75s) instead of firing it instantly: the game prepares
    # its lobby metadata (name/gamemode/prefabdbversion) over the frames while
    # "Creating server..." is shown and only writes it when LobbyCreated_t arrives --
    # deliver it instantly and its handler runs before that data is ready, writing only
    # the guid. All three items share the delay so their order (513->504->505) holds.
    # Do NOT fire LobbyDataUpdate_t here. Real Steam only sends it in response to
    # SetLobbyData (see SetLobbyData below). Sending it on create makes the game run
    # its lobby-refresh (CoopLobby.UpdateData -> GetLobbyData) on the still-empty lobby
    # BEFORE it has written its metadata, which overwrites the game's in-memory
    # name/gamemode/prefabdbversion with empty strings -- so its subsequent write finds
    # them "unchanged" and only the guid ever reaches the lobby.
    "ISteamMatchmaking::CreateLobby": "CSteamID id = emu::Net().CreateLobby((int)eLobbyType, cMaxMembers); LobbyCreated_t c{}; c.m_eResult = k_EResultOK; c.m_ulSteamIDLobby = id.ConvertToUint64(); SteamAPICall_t h = emu::QueueCallResult(c, 750); LobbyEnter_t e{}; e.m_ulSteamIDLobby = id.ConvertToUint64(); e.m_EChatRoomEnterResponse = k_EChatRoomEnterResponseSuccess; emu::QueueCallback(e, 750); return h;",
    "ISteamMatchmaking::JoinLobby": "emu::Net().JoinLobby(steamIDLobby); LobbyEnter_t e{}; e.m_ulSteamIDLobby = steamIDLobby.ConvertToUint64(); e.m_EChatRoomEnterResponse = k_EChatRoomEnterResponseSuccess; return emu::QueueCallResult(e);",
    "ISteamMatchmaking::LeaveLobby": "emu::Net().LeaveLobby(steamIDLobby);",
    "ISteamMatchmaking::RequestLobbyList": "LobbyMatchList_t r{}; r.m_nLobbiesMatching = (uint32)emu::Net().RequestLobbyList(); return emu::QueueCallResult(r);",
    "ISteamMatchmaking::GetLobbyByIndex": "return emu::Net().LobbyListByIndex(iLobby);",
    "ISteamMatchmaking::AddRequestLobbyListStringFilter": "emu::Net().AddLobbyListStringFilter(pchKeyToMatch, pchValueToMatch, (int)eComparisonType);",
    "ISteamMatchmaking::AddRequestLobbyListNumericalFilter": "emu::Net().AddLobbyListNumFilter(pchKeyToMatch, nValueToMatch, (int)eComparisonType);",
    "ISteamMatchmaking::AddRequestLobbyListFilterSlotsAvailable": "emu::Net().AddLobbyListSlotsFilter(nSlotsAvailable);",
    "ISteamMatchmaking::AddRequestLobbyListResultCountFilter": "emu::Net().SetLobbyListResultCount(cMaxResults);",
    "ISteamMatchmaking::RequestLobbyData": "return emu::Net().RequestLobbyData(steamIDLobby);",
    "ISteamMatchmaking::SetLobbyMemberData": "emu::Net().SetLobbyMemberData(steamIDLobby, pchKey, pchValue);",
    "ISteamMatchmaking::GetLobbyMemberData": "return emu::Net().GetLobbyMemberDataC(steamIDLobby, steamIDUser, pchKey);",
    "ISteamMatchmaking::SendLobbyChatMsg": "return emu::Net().SendLobbyChatMsg(steamIDLobby, pvMsgBody, cubMsgBody);",
    "ISteamMatchmaking::GetLobbyChatEntry": "int ct = 0; int n = emu::Net().GetLobbyChatEntry(steamIDLobby, iChatID, pSteamIDUser, pvData, cubData, &ct); if (peChatEntryType) *peChatEntryType = (EChatEntryType)ct; return n;",
    "ISteamMatchmaking::GetNumLobbyMembers": "return (int)emu::Net().LobbyMembers(steamIDLobby).size();",
    "ISteamMatchmaking::GetLobbyMemberByIndex": "auto v = emu::Net().LobbyMembers(steamIDLobby); return (iMember >= 0 && iMember < (int)v.size()) ? v[iMember] : CSteamID();",
    "ISteamMatchmaking::GetLobbyData": "const char* v = emu::Net().GetLobbyDataC(steamIDLobby, pchKey); EMU_DEBUG(\"GetLobbyData %llu '%s' -> '%s'\", (unsigned long long)steamIDLobby.ConvertToUint64(), pchKey ? pchKey : \"\", v ? v : \"\"); return v;",
    # Fire LobbyDataUpdate_t AFTER the write (as real Steam does), so the game reads its
    # lobby back only once the data is actually set -- not before (see CreateLobby).
    "ISteamMatchmaking::SetLobbyData": "bool ok = emu::Net().SetLobbyData(steamIDLobby, pchKey, pchValue); LobbyDataUpdate_t u{}; u.m_ulSteamIDLobby = steamIDLobby.ConvertToUint64(); u.m_ulSteamIDMember = steamIDLobby.ConvertToUint64(); u.m_bSuccess = 1; emu::QueueCallback(u); return ok;",
    "ISteamMatchmaking::GetLobbyDataCount": "return emu::Net().LobbyDataCount(steamIDLobby);",
    "ISteamMatchmaking::GetLobbyDataByIndex": "return emu::Net().LobbyDataByIndex(steamIDLobby, iLobbyData, pchKey, cchKeyBufferSize, pchValue, cchValueBufferSize);",
    "ISteamMatchmaking::GetLobbyOwner": "CSteamID o = emu::Net().LobbyOwner(steamIDLobby); EMU_INFO(\"GetLobbyOwner %llu -> %llu (me %llu)\", (unsigned long long)steamIDLobby.ConvertToUint64(), (unsigned long long)o.ConvertToUint64(), (unsigned long long)emu::LocalSteamID().ConvertToUint64()); return o;",
    "ISteamMatchmaking::SetLobbyType": "return true;",
    "ISteamMatchmaking::SetLobbyJoinable": "return emu::Net().SetLobbyJoinable(steamIDLobby, bLobbyJoinable);",
    # "Host started the game": stores the server on the lobby (gossiped to members,
    # who each get LobbyGameCreated_t) -- lobby members wait on that callback before
    # connecting to the session.
    "ISteamMatchmaking::SetLobbyGameServer": "emu::Net().SetLobbyGameServer(steamIDLobby, unGameServerIP, unGameServerPort, steamIDGameServer);",
    "ISteamMatchmaking::GetLobbyGameServer": "return emu::Net().GetLobbyGameServer(steamIDLobby, punGameServerIP, punGameServerPort, psteamIDGameServer);",
    "ISteamMatchmaking::GetLobbyMemberLimit": "return emu::Net().LobbyMaxMembers(steamIDLobby);",
    # -- ISteamRemoteStorage: local file cloud ----------------------------
    "ISteamRemoteStorage::FileWrite": "return emu::Storage().Write(pchFile, pvData, cubData);",
    "ISteamRemoteStorage::FileRead": "return emu::Storage().Read(pchFile, pvData, cubDataToRead);",
    "ISteamRemoteStorage::FileForget": "return true;",
    "ISteamRemoteStorage::FileDelete": "return emu::Storage().Delete(pchFile);",
    "ISteamRemoteStorage::FileExists": "return emu::Storage().Exists(pchFile);",
    "ISteamRemoteStorage::FilePersisted": "return emu::Storage().Exists(pchFile);",
    "ISteamRemoteStorage::GetFileSize": "return emu::Storage().GetSize(pchFile);",
    "ISteamRemoteStorage::GetFileTimestamp": "return emu::Storage().GetTimestamp(pchFile);",
    "ISteamRemoteStorage::GetFileCount": "return emu::Storage().FileCount();",
    "ISteamRemoteStorage::GetFileNameAndSize": "return emu::Storage().FileNameByIndex(iFile, pnFileSizeInBytes);",
    "ISteamRemoteStorage::GetQuota": "if (pnTotalBytes) *pnTotalBytes = (uint64)1 << 34; if (puAvailableBytes) *puAvailableBytes = (uint64)1 << 33; return true;",
    "ISteamRemoteStorage::IsCloudEnabledForAccount": "return true;",
    "ISteamRemoteStorage::IsCloudEnabledForApp": "return true;",
    "ISteamRemoteStorage::SetCloudEnabledForApp": "(void)bEnabled;",
    "ISteamRemoteStorage::FileWriteStreamOpen": "return emu::Storage().StreamOpen(pchFile);",
    "ISteamRemoteStorage::FileWriteStreamWriteChunk": "return emu::Storage().StreamWrite(writeHandle, pvData, cubData);",
    "ISteamRemoteStorage::FileWriteStreamClose": "return emu::Storage().StreamClose(writeHandle);",
    "ISteamRemoteStorage::FileWriteStreamCancel": "return emu::Storage().StreamCancel(writeHandle);",
    "ISteamRemoteStorage::FileWriteAsync": "bool ok = emu::Storage().Write(pchFile, pvData, (int32)cubData); RemoteStorageFileWriteAsyncComplete_t r{}; r.m_eResult = ok ? k_EResultOK : k_EResultFail; return emu::QueueCallResult(r);",
    # The completion struct carries its own call handle, so allocate the handle
    # first, stash the bytes under it, then queue the result ON that handle; the
    # game copies them out from its handler via FileReadAsyncComplete.
    "ISteamRemoteStorage::FileReadAsync": "SteamAPICall_t h = emu::Dispatch().AllocCall(); uint32 got = 0; bool ok = emu::Storage().ReadAsyncStart(h, pchFile ? pchFile : \"\", nOffset, cubToRead, &got); RemoteStorageFileReadAsyncComplete_t r{}; r.m_hFileReadAsync = h; r.m_eResult = ok ? k_EResultOK : k_EResultFileNotFound; r.m_nOffset = nOffset; r.m_cubRead = got; emu::QueueCallResultOn(h, r); return h;",
    "ISteamRemoteStorage::FileReadAsyncComplete": "return emu::Storage().ReadAsyncComplete(hReadCall, pvBuffer, cubToRead);",
    "ISteamRemoteStorage::FileShare": "RemoteStorageFileShareResult_t r{}; bool ok = pchFile && emu::Storage().Exists(pchFile); r.m_eResult = ok ? k_EResultOK : k_EResultFileNotFound; r.m_hFile = ok ? emu::NextUGCHandle() : 0; if (pchFile) std::snprintf(r.m_rgchFilename, sizeof(r.m_rgchFilename), \"%s\", pchFile); return emu::QueueCallResult(r);",
    # UGC content is never actually hosted anywhere: succeed with an empty file
    # (games treat failure as "workshop broken" but handle 0-byte content).
    "ISteamRemoteStorage::UGCDownload": "RemoteStorageDownloadUGCResult_t r{}; r.m_eResult = k_EResultOK; r.m_hFile = hContent; r.m_nAppID = emu::AppID(); return emu::QueueCallResult(r);",
    "ISteamRemoteStorage::UGCDownloadToLocation": "RemoteStorageDownloadUGCResult_t r{}; r.m_eResult = k_EResultOK; r.m_hFile = hContent; r.m_nAppID = emu::AppID(); return emu::QueueCallResult(r);",
    "ISteamRemoteStorage::GetPublishedFileDetails": "RemoteStorageGetPublishedFileDetailsResult_t r{}; r.m_eResult = k_EResultOK; r.m_nPublishedFileId = unPublishedFileId; return emu::QueueCallResult(r);",
    "ISteamRemoteStorage::GetLocalFileChangeCount": "return 0;",
    # Zero is k_ERemoteStoragePlatformNone ("syncs nowhere"); our files are
    # plain local files, i.e. available everywhere.
    "ISteamRemoteStorage::GetSyncPlatforms": "return k_ERemoteStoragePlatformAll;",
    # -- ISteamUserStats: persisted stats + achievements ------------------
    "ISteamUserStats::GetStat(const char *,int32 *)": "return emu::Stats().GetInt(pchName, pData);",
    "ISteamUserStats::GetStat(const char *,float *)": "return emu::Stats().GetFloat(pchName, pData);",
    "ISteamUserStats::SetStat(const char *,int32)": "return emu::Stats().SetInt(pchName, nData);",
    "ISteamUserStats::SetStat(const char *,float)": "return emu::Stats().SetFloat(pchName, fData);",
    "ISteamUserStats::GetAchievement": "return emu::Stats().GetAchievement(pchName, pbAchieved);",
    "ISteamUserStats::SetAchievement": "return emu::Stats().SetAchievement(pchName);",
    "ISteamUserStats::ClearAchievement": "return emu::Stats().ClearAchievement(pchName);",
    "ISteamUserStats::GetAchievementAndUnlockTime": "return emu::Stats().GetAchievement(pchName, pbAchieved, punUnlockTime);",
    "ISteamUserStats::StoreStats": "bool ok = emu::Stats().Store(); UserStatsStored_t s{}; s.m_nGameID = emu::AppID(); s.m_eResult = k_EResultOK; emu::QueueCallback(s); return ok;",
    "ISteamUserStats::GetNumAchievements": "return emu::Stats().NumAchievements();",
    "ISteamUserStats::GetAchievementName": "return emu::Stats().AchievementNameByIndex(iAchievement);",
    "ISteamUserStats::ResetAllStats": "emu::Stats().ResetAll(bAchievementsToo); return true;",
    "ISteamUserStats::RequestUserStats": "UserStatsReceived_t r{}; r.m_nGameID = emu::AppID(); r.m_eResult = k_EResultOK; r.m_steamIDUser = steamIDUser; return emu::QueueCallResult(r);",
    "ISteamUserStats::GetUserStat(CSteamID,const char *,int32 *)": "return emu::Stats().GetInt(pchName, pData);",
    "ISteamUserStats::GetUserStat(CSteamID,const char *,float *)": "return emu::Stats().GetFloat(pchName, pData);",
    "ISteamUserStats::GetUserAchievement": "return emu::Stats().GetAchievement(pchName, pbAchieved);",
    # Games display the player count; 0 sometimes reads as "offline", so report 1
    # (ourselves).
    "ISteamUserStats::GetNumberOfCurrentPlayers": "NumberOfCurrentPlayers_t r{}; r.m_bSuccess = 1; r.m_cPlayers = 1; return emu::QueueCallResult(r);",
    # -- ISteamUserStats: global stats "received but empty" -----------------
    # The requests complete OK so CCallResult waiters proceed; the paired
    # getters then report zero data (true + 0) rather than "not received".
    "ISteamUserStats::RequestGlobalAchievementPercentages": "GlobalAchievementPercentagesReady_t r{}; r.m_nGameID = emu::AppID(); r.m_eResult = k_EResultOK; return emu::QueueCallResult(r);",
    "ISteamUserStats::RequestGlobalStats": "GlobalStatsReceived_t r{}; r.m_nGameID = emu::AppID(); r.m_eResult = k_EResultOK; return emu::QueueCallResult(r);",
    "ISteamUserStats::GetGlobalStat(const char *,int64 *)": "if (pData) *pData = 0; return true;",
    "ISteamUserStats::GetGlobalStat(const char *,double *)": "if (pData) *pData = 0; return true;",
    "ISteamUserStats::GetGlobalStatHistory(const char *,int64 *,uint32)": "return 0;",
    "ISteamUserStats::GetGlobalStatHistory(const char *,double *,uint32)": "return 0;",
    "ISteamUserStats::GetAchievementAchievedPercent": "if (pflPercent) *pflPercent = 0.0f; return true;",
    "ISteamUserStats::GetMostAchievedAchievementInfo": "return -1;",  # -1 = no data (iterator end)
    "ISteamUserStats::GetNextMostAchievedAchievementInfo": "return -1;",
    # -- ISteamUserStats: local persisted leaderboards --------------------
    "ISteamUserStats::FindOrCreateLeaderboard": "bool found = false; uint64 h = emu::Leaderboards().FindOrCreate(pchLeaderboardName ? pchLeaderboardName : \"\", (int)eLeaderboardSortMethod, (int)eLeaderboardDisplayType, true, &found); LeaderboardFindResult_t r{}; r.m_hSteamLeaderboard = h; r.m_bLeaderboardFound = found ? 1 : 0; return emu::QueueCallResult(r);",
    "ISteamUserStats::FindLeaderboard": "bool found = false; uint64 h = emu::Leaderboards().FindOrCreate(pchLeaderboardName ? pchLeaderboardName : \"\", 0, 0, false, &found); LeaderboardFindResult_t r{}; r.m_hSteamLeaderboard = h; r.m_bLeaderboardFound = found ? 1 : 0; return emu::QueueCallResult(r);",
    "ISteamUserStats::GetLeaderboardName": "return emu::Leaderboards().NameOf(hSteamLeaderboard);",
    "ISteamUserStats::GetLeaderboardEntryCount": "return emu::Leaderboards().EntryCount(hSteamLeaderboard);",
    "ISteamUserStats::GetLeaderboardSortMethod": "return (ELeaderboardSortMethod)emu::Leaderboards().SortMethod(hSteamLeaderboard);",
    "ISteamUserStats::GetLeaderboardDisplayType": "return (ELeaderboardDisplayType)emu::Leaderboards().DisplayType(hSteamLeaderboard);",
    "ISteamUserStats::UploadLeaderboardScore": "bool changed = false; int nr = 0, pr = 0; bool ok = emu::Leaderboards().Upload(hSteamLeaderboard, (int)eLeaderboardUploadScoreMethod, nScore, pScoreDetails, cScoreDetailsCount, &changed, &nr, &pr); LeaderboardScoreUploaded_t r{}; r.m_bSuccess = ok ? 1 : 0; r.m_hSteamLeaderboard = hSteamLeaderboard; r.m_nScore = nScore; r.m_bScoreChanged = changed ? 1 : 0; r.m_nGlobalRankNew = nr; r.m_nGlobalRankPrevious = pr; return emu::QueueCallResult(r);",
    "ISteamUserStats::DownloadLeaderboardEntries": "int cnt = 0; uint64 eh = emu::Leaderboards().Download(hSteamLeaderboard, (int)eLeaderboardDataRequest, nRangeStart, nRangeEnd, &cnt); LeaderboardScoresDownloaded_t r{}; r.m_hSteamLeaderboard = hSteamLeaderboard; r.m_hSteamLeaderboardEntries = eh; r.m_cEntryCount = cnt; return emu::QueueCallResult(r);",
    # The local board only ever holds scores this machine uploaded, so the
    # global top-N slice IS the per-user view for any user list.
    "ISteamUserStats::DownloadLeaderboardEntriesForUsers": "int cnt = 0; uint64 eh = emu::Leaderboards().Download(hSteamLeaderboard, 0 /*k_ELeaderboardDataRequestGlobal*/, 1, cUsers > 0 ? cUsers : 1, &cnt); LeaderboardScoresDownloaded_t r{}; r.m_hSteamLeaderboard = hSteamLeaderboard; r.m_hSteamLeaderboardEntries = eh; r.m_cEntryCount = cnt; return emu::QueueCallResult(r);",
    "ISteamUserStats::GetDownloadedLeaderboardEntry": "uint64_t sid = 0, ugc = 0; int32_t rank = 0, score = 0, nd = 0; bool ok = emu::Leaderboards().GetEntry(hSteamLeaderboardEntries, index, &sid, &rank, &score, &ugc, pDetails, cDetailsMax, &nd); if (ok && pLeaderboardEntry) { *pLeaderboardEntry = LeaderboardEntry_t{}; pLeaderboardEntry->m_steamIDUser = CSteamID((uint64)sid); pLeaderboardEntry->m_nGlobalRank = rank; pLeaderboardEntry->m_nScore = score; pLeaderboardEntry->m_cDetails = nd; pLeaderboardEntry->m_hUGC = ugc; } return ok;",
    # -- modern ISteamNetworkingSockets over the LAN backend --------------
    "ISteamNetworkingSockets::CreateListenSocketP2P": "return emu::Net().CreateListenSocketP2P(nLocalVirtualPort);",
    "ISteamNetworkingSockets::CloseListenSocket": "return emu::Net().CloseListenSocket(hSocket);",
    "ISteamNetworkingSockets::ConnectP2P": "return emu::Net().ConnectP2P(identityRemote.GetSteamID(), nRemoteVirtualPort);",
    "ISteamNetworkingSockets::AcceptConnection": "return (EResult)emu::Net().AcceptConnection(hConn);",
    "ISteamNetworkingSockets::CloseConnection": "return emu::Net().CloseConnection(hPeer);",
    "ISteamNetworkingSockets::SendMessageToConnection": "if (pOutMessageNumber) *pOutMessageNumber = 0; return (EResult)emu::Net().SendToConnection(hConn, pData, cbData);",
    "ISteamNetworkingSockets::FlushMessagesOnConnection": "return k_EResultOK;",
    "ISteamNetworkingSockets::ReceiveMessagesOnConnection": "return emu::Net().ReceiveOnConnection(hConn, ppOutMessages, nMaxMessages);",
    "ISteamNetworkingSockets::GetConnectionInfo": "CSteamID r; int st = 0; uint32 ls = 0; bool ok = emu::Net().GetConnectionInfo(hConn, &r, &st, &ls); if (ok && pInfo) { *pInfo = SteamNetConnectionInfo_t{}; pInfo->m_identityRemote.SetSteamID(r); pInfo->m_eState = (ESteamNetworkingConnectionState)st; pInfo->m_hListenSocket = ls; } return ok;",
    "ISteamNetworkingSockets::GetIdentity": "if (pIdentity) pIdentity->SetSteamID(emu::LocalSteamID()); return true;",
    # Unity transports (FishNet/Mirror/Netcode) poll this for RTT and treat
    # failure as a disconnect. Zeroed-but-sane stats: connected, 1ms ping.
    "ISteamNetworkingSockets::GetConnectionRealTimeStatus": "CSteamID remote; int st = 0; uint32 ls = 0; if (!emu::Net().GetConnectionInfo(hConn, &remote, &st, &ls)) return k_EResultNoConnection; if (pStatus) { *pStatus = SteamNetConnectionRealTimeStatus_t{}; pStatus->m_eState = (ESteamNetworkingConnectionState)st; pStatus->m_nPing = 1; pStatus->m_flConnectionQualityLocal = 1.0f; pStatus->m_flConnectionQualityRemote = 1.0f; pStatus->m_nSendRateBytesPerSecond = 64 * 1024 * 1024; } if (pLanes) for (int i = 0; i < nLanes; ++i) pLanes[i] = SteamNetConnectionRealTimeLaneStatus_t{}; return k_EResultOK;",
    "ISteamNetworkingSockets::CreatePollGroup": "return emu::Net().CreatePollGroup();",
    "ISteamNetworkingSockets::DestroyPollGroup": "return emu::Net().DestroyPollGroup(hPollGroup);",
    "ISteamNetworkingSockets::SetConnectionPollGroup": "return emu::Net().SetConnectionPollGroup(hConn, hPollGroup);",
    "ISteamNetworkingSockets::ReceiveMessagesOnPollGroup": "return emu::Net().ReceiveOnPollGroup(hPollGroup, ppOutMessages, nMaxMessages);",
    "ISteamNetworkingSockets::InitAuthentication": "return k_ESteamNetworkingAvailability_Current;",
    "ISteamNetworkingSockets::GetAuthenticationStatus": "return k_ESteamNetworkingAvailability_Current;",
    # Wrong-zero fixes: EResult{} is 0 = k_EResultNone ("invalid"), not a verdict.
    # Lanes are accepted-but-unused (single lane; messages still flow in order);
    # the SDR/FakeIP getters fail honestly -- those facilities don't exist here.
    "ISteamNetworkingSockets::ConfigureConnectionLanes": "return k_EResultOK;",
    "ISteamNetworkingSockets::GetHostedDedicatedServerAddress": "return k_EResultFail;",
    "ISteamNetworkingSockets::GetGameCoordinatorServerLogin": "return k_EResultFail;",
    "ISteamNetworkingSockets::GetRemoteFakeIPForConnection": "return k_EResultFail;",
    # -- ISteamApps: installed DLC comes from the config [dlc] section ------
    "ISteamApps::GetDLCCount": "return emu::Config().DLCCount();",
    "ISteamApps::BGetDLCDataByIndex": "uint32 a = 0; bool avail = false; bool ok = emu::Config().DLCByIndex(iDLC, &a, &avail, pchName, cchNameBufferSize); if (ok) { if (pAppID) *pAppID = a; if (pbAvailable) *pbAvailable = avail; } return ok;",
    # -- ISteamApps: remaining fields where 0/false is the right answer ----
    "ISteamApps::GetLaunchQueryParam": 'return "";',
    "ISteamApps::GetAppBuildId": "return 1;",
    "ISteamApps::GetLaunchCommandLine": "if (pszCommandLine && cubCommandLine > 0) pszCommandLine[0] = 0; return 0;",
    "ISteamApps::BIsTimedTrial": "if (punSecondsAllowed) *punSecondsAllowed = 0; if (punSecondsPlayed) *punSecondsPlayed = 0; return false;",
    "ISteamApps::GetCurrentBetaName": "if (pchName && cchNameBufferSize > 0) pchName[0] = 0; return false;",
    "ISteamApps::GetInstalledDepots": "return 0;",
    # -- ISteamInput / ISteamController: initialize successfully -----------
    "ISteamInput::Init": "(void)bExplicitlyCallRunFrame; return true;",
    "ISteamInput::Shutdown": "return true;",
    "ISteamInput::GetConnectedControllers": "return 0;",
    "ISteamController::Init": "return true;",
    "ISteamController::Shutdown": "return true;",
    "ISteamController::GetConnectedControllers": "return 0;",
    # -- ISteamInventory: succeed with empty results -----------------------
    "ISteamInventory::GetResultStatus": "return k_EResultOK;",
    "ISteamInventory::GetResultTimestamp": "return emu::UnixTime();",
    "ISteamInventory::CheckResultSteamID": "return true;",
    "ISteamInventory::GetAllItems": "if (pResultHandle) *pResultHandle = 1; return true;",
    # Never fake a successful purchase -- the game might grant paid items.
    "ISteamInventory::StartPurchase": "SteamInventoryStartPurchaseResult_t r{}; r.m_result = k_EResultFail; return emu::QueueCallResult(r);",
    # -- ISteamGameServer: local anonymous login --------------------------
    # The game server logs on and its OWN manual-dispatch pump (a distinct pipe from
    # the client's) waits for SteamServersConnected_t + GSPolicyResponse_t before it
    # considers the server up. Route these to the game-server pipe so a listen-server
    # game doesn't have them consumed by its client pump.
    # The game server's own manual-dispatch pump waits for SteamServersConnected_t +
    # GSPolicyResponse_t before it considers the server up.
    # A small delay avoids game re-issuing CreateServer
    # while it waits, but keep it short so hosting isn't sluggish.
    "ISteamGameServer::LogOnAnonymous": "emu::Net().EnsureStarted(); SteamServersConnected_t c{}; emu::QueueGameServerCallback(c, 200); GSPolicyResponse_t p{}; p.m_bSecure = 0; emu::QueueGameServerCallback(p, 200);",
    "ISteamGameServer::LogOn": "(void)pszToken; emu::Net().EnsureStarted(); SteamServersConnected_t c{}; emu::QueueGameServerCallback(c, 200); GSPolicyResponse_t p{}; p.m_bSecure = 0; emu::QueueGameServerCallback(p, 200);",
    "ISteamGameServer::BLoggedOn": "return true;",
    "ISteamGameServer::GetSteamID": "return emu::LocalGSSteamID();",
    "ISteamGameServer::BeginAuthSession": "return emu::Auth().BeginAuthSession(pAuthTicket, cbAuthTicket, steamID);",
    "ISteamGameServer::BUpdateUserData": "return true;",
    # Games embed this in the session/server info they gossip to peers; a zeroed
    # address breaks "join by server info" flows.
    "ISteamGameServer::GetPublicIP": "SteamIPAddress_t ip{}; ip.m_eType = k_ESteamIPTypeIPv4; ip.m_unIPv4 = emu::LanIPv4(); return ip;",
    # -- ISteamNetworkingMessages: ad-hoc messaging over the P2P layer -----
    "ISteamNetworkingMessages::SendMessageToUser": "return emu::Net().SendP2PPacket(identityRemote.GetSteamID(), pubData, cubData, nRemoteChannel) ? k_EResultOK : k_EResultNoConnection;",
    "ISteamNetworkingMessages::ReceiveMessagesOnChannel": "return emu::Net().ReceiveMessagesOnChannel(nLocalChannel, ppOutMessages, nMaxMessages);",
    "ISteamNetworkingMessages::AcceptSessionWithUser": "return true;",
    "ISteamNetworkingMessages::CloseSessionWithUser": "return emu::Net().CloseP2PChannel(identityRemote.GetSteamID(), 0);",
    "ISteamNetworkingMessages::CloseChannelWithUser": "return emu::Net().CloseP2PChannel(identityRemote.GetSteamID(), nLocalChannel);",
    "ISteamNetworkingMessages::GetSessionConnectionInfo": "bool up = emu::Net().IsPeer(identityRemote.GetSteamID()); if (pConnectionInfo) { *pConnectionInfo = SteamNetConnectionInfo_t{}; pConnectionInfo->m_identityRemote.SetSteamID(identityRemote.GetSteamID()); pConnectionInfo->m_eState = up ? k_ESteamNetworkingConnectionState_Connected : k_ESteamNetworkingConnectionState_None; } return up ? k_ESteamNetworkingConnectionState_Connected : k_ESteamNetworkingConnectionState_None;",
    # -- ISteamHTTP: real HTTP/1.1 client ---------------------------------
    "ISteamHTTP::CreateHTTPRequest": "return emu::Http().Create((int)eHTTPRequestMethod, pchAbsoluteURL ? pchAbsoluteURL : \"\");",
    "ISteamHTTP::SetHTTPRequestContextValue": "return emu::Http().SetContext(hRequest, ulContextValue);",
    "ISteamHTTP::SetHTTPRequestNetworkActivityTimeout": "(void)unTimeoutSeconds; return true;",
    "ISteamHTTP::SetHTTPRequestHeaderValue": "return emu::Http().SetHeader(hRequest, pchHeaderName ? pchHeaderName : \"\", pchHeaderValue ? pchHeaderValue : \"\");",
    "ISteamHTTP::SetHTTPRequestGetOrPostParameter": "return emu::Http().SetParam(hRequest, pchParamName ? pchParamName : \"\", pchParamValue ? pchParamValue : \"\");",
    "ISteamHTTP::SetHTTPRequestRawPostBody": "return emu::Http().SetRawBody(hRequest, pchContentType ? pchContentType : \"\", pubBody, unBodyLen);",
    "ISteamHTTP::SendHTTPRequest": "emu::Http().Send(hRequest); HTTPRequestCompleted_t r{}; r.m_hRequest = hRequest; r.m_ulContextValue = emu::Http().Context(hRequest); r.m_bRequestSuccessful = emu::Http().Status(hRequest) != 0; r.m_eStatusCode = (EHTTPStatusCode)emu::Http().Status(hRequest); uint32 bs = 0; emu::Http().GetBodySize(hRequest, &bs); r.m_unBodySize = bs; SteamAPICall_t call = emu::QueueCallResult(r); if (pCallHandle) *pCallHandle = call; return true;",
    "ISteamHTTP::SendHTTPRequestAndStreamResponse": "emu::Http().Send(hRequest); HTTPRequestCompleted_t r{}; r.m_hRequest = hRequest; r.m_ulContextValue = emu::Http().Context(hRequest); r.m_bRequestSuccessful = emu::Http().Status(hRequest) != 0; r.m_eStatusCode = (EHTTPStatusCode)emu::Http().Status(hRequest); uint32 bs = 0; emu::Http().GetBodySize(hRequest, &bs); r.m_unBodySize = bs; SteamAPICall_t call = emu::QueueCallResult(r); if (pCallHandle) *pCallHandle = call; return true;",
    "ISteamHTTP::GetHTTPResponseBodySize": "return emu::Http().GetBodySize(hRequest, unBodySize);",
    "ISteamHTTP::GetHTTPResponseBodyData": "return emu::Http().GetBodyData(hRequest, pBodyDataBuffer, unBufferSize);",
    "ISteamHTTP::GetHTTPResponseHeaderSize": "return emu::Http().GetHeaderSize(hRequest, pchHeaderName ? pchHeaderName : \"\", unResponseHeaderSize);",
    "ISteamHTTP::GetHTTPResponseHeaderValue": "return emu::Http().GetHeaderValue(hRequest, pchHeaderName ? pchHeaderName : \"\", pHeaderValueBuffer, unBufferSize);",
    "ISteamHTTP::GetHTTPRequestWasTimedOut": "return emu::Http().WasTimedOut(hRequest, pbWasTimedOut);",
    "ISteamHTTP::GetHTTPDownloadProgressPct": "if (pflPercentOut) *pflPercentOut = 100.0f; return true;",
    "ISteamHTTP::ReleaseHTTPRequest": "return emu::Http().Release(hRequest);",
    "ISteamHTTP::DeferHTTPRequest": "return true;",
    "ISteamHTTP::PrioritizeHTTPRequest": "return true;",
    "ISteamHTTP::SetHTTPRequestRequiresVerifiedCertificate": "(void)bRequireVerifiedCertificate; return true;",
    "ISteamHTTP::SetHTTPRequestUserAgentInfo": "(void)pchUserAgentInfo; return true;",
    "ISteamHTTP::SetHTTPRequestAbsoluteTimeoutMS": "(void)unMilliseconds; return true;",
    "ISteamHTTP::CreateCookieContainer": "(void)bAllowResponsesToModify; return 1;",
    "ISteamHTTP::ReleaseCookieContainer": "return true;",
    "ISteamHTTP::SetCookie": "return true;",
    "ISteamHTTP::SetHTTPRequestCookieContainer": "return true;",
    # -- ISteamMusic ------------------------------------------------------
    "ISteamMusic::BIsEnabled": "return emu::Music().enabled;",
    "ISteamMusic::BIsPlaying": "return emu::Music().playing;",
    "ISteamMusic::GetPlaybackStatus": "return (AudioPlayback_Status)emu::Music().status;",
    "ISteamMusic::Play": "emu::Music().Play();",
    "ISteamMusic::Pause": "emu::Music().Pause();",
    "ISteamMusic::PlayPrevious": "emu::Music().Prev();",
    "ISteamMusic::PlayNext": "emu::Music().Next();",
    "ISteamMusic::SetVolume": "emu::Music().volume = flVolume;",
    "ISteamMusic::GetVolume": "return emu::Music().volume;",
    # -- ISteamScreenshots ------------------------------------------------
    "ISteamScreenshots::WriteScreenshot": "uint32 hs = emu::WriteScreenshotRGB(pubRGB, cubRGB, nWidth, nHeight); ScreenshotReady_t r{}; r.m_hLocal = hs; r.m_eResult = hs ? k_EResultOK : k_EResultFail; emu::QueueCallback(r); return hs;",
    "ISteamScreenshots::AddScreenshotToLibrary": "(void)pchFilename; (void)pchThumbnailFilename; (void)nWidth; (void)nHeight; ScreenshotHandle hs = emu::NextScreenshotHandle(); ScreenshotReady_t r{}; r.m_hLocal = hs; r.m_eResult = k_EResultOK; emu::QueueCallback(r); return hs;",
    "ISteamScreenshots::IsScreenshotsHooked": "return false;",
    "ISteamScreenshots::SetLocation": "return true;",
    "ISteamScreenshots::TagUser": "return true;",
    "ISteamScreenshots::TagPublishedFile": "return true;",
    # -- ISteamParentalSettings: everything unlocked ----------------------
    "ISteamParentalSettings::BIsParentalLockEnabled": "return false;",
    "ISteamParentalSettings::BIsParentalLockLocked": "return false;",
    "ISteamParentalSettings::BIsAppBlocked": "(void)nAppID; return false;",
    "ISteamParentalSettings::BIsAppInBlockList": "(void)nAppID; return false;",
    "ISteamParentalSettings::BIsFeatureBlocked": "(void)eFeature; return false;",
    "ISteamParentalSettings::BIsFeatureInBlockList": "(void)eFeature; return false;",
    # -- ISteamVideo ------------------------------------------------------
    "ISteamVideo::IsBroadcasting": "if (pnNumViewers) *pnNumViewers = 0; return false;",
    "ISteamVideo::GetOPFStringForApp": "if (pnBufferSize) *pnBufferSize = 0; return false;",
    # Fires a plain callback (not a call-result): failure is correct -- there is
    # no 360-video content -- but the callback must still arrive or pollers hang.
    "ISteamVideo::GetOPFSettings": "GetOPFSettingsResult_t r{}; r.m_eResult = k_EResultFail; r.m_unVideoAppID = unVideoAppID; emu::QueueCallback(r);",
    # -- ISteamGameServerStats -------------------------------------------
    # Queued on the game-server pipe: a dedicated/listen server pumps only that
    # pipe, so a client-pipe completion would never reach its CCallResult.
    "ISteamGameServerStats::RequestUserStats": "GSStatsReceived_t r{}; r.m_eResult = k_EResultOK; r.m_steamIDUser = steamIDUser; return emu::QueueGameServerCallResult(r);",
    "ISteamGameServerStats::GetUserStat(CSteamID,const char *,int32 *)": "return emu::SvStats().GetInt(steamIDUser, pchName, pData);",
    "ISteamGameServerStats::GetUserStat(CSteamID,const char *,float *)": "return emu::SvStats().GetFloat(steamIDUser, pchName, pData);",
    "ISteamGameServerStats::SetUserStat(CSteamID,const char *,int32)": "return emu::SvStats().SetInt(steamIDUser, pchName, nData);",
    "ISteamGameServerStats::SetUserStat(CSteamID,const char *,float)": "return emu::SvStats().SetFloat(steamIDUser, pchName, fData);",
    "ISteamGameServerStats::GetUserAchievement": "return emu::SvStats().GetAch(steamIDUser, pchName, pbAchieved);",
    "ISteamGameServerStats::SetUserAchievement": "return emu::SvStats().SetAch(steamIDUser, pchName, true);",
    "ISteamGameServerStats::ClearUserAchievement": "return emu::SvStats().SetAch(steamIDUser, pchName, false);",
    "ISteamGameServerStats::StoreUserStats": "GSStatsStored_t r{}; r.m_eResult = k_EResultOK; r.m_steamIDUser = steamIDUser; return emu::QueueGameServerCallResult(r);",
    # -- ISteamHTMLSurface: handle bookkeeping (no renderer) --------------
    "ISteamHTMLSurface::Init": "return true;",
    "ISteamHTMLSurface::Shutdown": "return true;",
    "ISteamHTMLSurface::CreateBrowser": "(void)pchUserAgent; (void)pchUserCSS; return emu::Dispatch().AllocCall();",
    # -- ISteamTimeline: event-handle bookkeeping -------------------------
    "ISteamTimeline::AddInstantaneousTimelineEvent": "return emu::NextTimelineEvent();",
    "ISteamTimeline::AddRangeTimelineEvent": "return emu::NextTimelineEvent();",
    "ISteamTimeline::StartRangeTimelineEvent": "return emu::NextTimelineEvent();",
    # -- ISteamParties / ISteamRemotePlay: empty but valid ----------------
    "ISteamParties::GetNumActiveBeacons": "return 0;",
    "ISteamParties::GetNumAvailableBeaconLocations": "if (puNumLocations) *puNumLocations = 0; return true;",
    "ISteamParties::GetAvailableBeaconLocations": "(void)pLocationList; (void)uMaxNumLocations; return true;",
    "ISteamRemotePlay::GetSessionCount": "return 0;",
    "ISteamRemotePlay::BSessionRemotePlayTogether": "(void)unSessionID; return false;",
    # -- ISteamNetworkingUtils --------------------------------------------
    "ISteamNetworkingUtils::AllocateMessage": "return emu::AllocNetworkingMessage(cbAllocateBuffer);",
    # Real config store: the typed SetGlobalConfigValue* helpers all funnel into
    # these two virtuals, and the neutral false/0 read as "networking broken"
    # (GetConfigValue's zero isn't even a valid enum value).
    "ISteamNetworkingUtils::SetConfigValue": "return emu::NetConfigSet((int)eValue, (int)eScopeType, scopeObj, (int)eDataType, pArg);",
    "ISteamNetworkingUtils::GetConfigValue": "int dt = 0; int res = emu::NetConfigGet((int)eValue, (int)eScopeType, scopeObj, &dt, pResult, cbResult); if (pOutDataType && res > 0) *pOutDataType = (ESteamNetworkingConfigDataType)dt; return (ESteamNetworkingGetConfigValueResult)res;",
    # FakeIP / SDR features genuinely absent: fail honestly (EResult{} = 0 =
    # k_EResultNone would read as "invalid call", not "not available").
    "ISteamNetworkingUtils::GetRealIdentityForFakeIP": "return k_EResultFail;",
    "ISteamNetworkingUtils::GetLocalTimestamp": "return emu::LocalTimestamp();",
    "ISteamNetworkingUtils::GetRelayNetworkStatus": "return k_ESteamNetworkingAvailability_Current;",
    "ISteamNetworkingUtils::CheckPingDataUpToDate": "(void)flMaxAgeSeconds; return true;",
    "ISteamNetworkingUtils::GetPOPCount": "return 0;",
    "ISteamNetworkingUtils::GetIPv4FakeIPType": "(void)nIPv4; return k_ESteamNetworkingFakeIPType_NotFake;",
    "ISteamNetworkingUtils::SteamNetworkingIPAddr_ToString": "if (buf && cbBuf) { uint32 ip = addr.GetIPv4(); if (bWithPort) std::snprintf(buf, cbBuf, \"%u.%u.%u.%u:%u\", (ip>>24)&0xff, (ip>>16)&0xff, (ip>>8)&0xff, ip&0xff, addr.m_port); else std::snprintf(buf, cbBuf, \"%u.%u.%u.%u\", (ip>>24)&0xff, (ip>>16)&0xff, (ip>>8)&0xff, ip&0xff); }",
    "ISteamNetworkingUtils::SteamNetworkingIPAddr_ParseString": "(void)pAddr; (void)pszStr; return false;",
    "ISteamNetworkingUtils::SteamNetworkingIdentity_ToString": "if (buf && cbBuf) std::snprintf(buf, cbBuf, \"steamid:%llu\", (unsigned long long)identity.GetSteamID64());",
    "ISteamNetworkingUtils::SteamNetworkingIdentity_ParseString": "(void)pIdentity; (void)pszStr; return false;",
    # -- ISteamMatchmakingServers: LAN browse completes empty -------------
    "ISteamMatchmakingServers::RequestLANServerList": "if (pRequestServersResponse) pRequestServersResponse->RefreshComplete((HServerListRequest)1, eNoServersListedOnMasterServer); return (HServerListRequest)1;",
    "ISteamMatchmakingServers::RequestInternetServerList": "if (pRequestServersResponse) pRequestServersResponse->RefreshComplete((HServerListRequest)1, eNoServersListedOnMasterServer); return (HServerListRequest)1;",
    "ISteamMatchmakingServers::RequestFriendsServerList": "if (pRequestServersResponse) pRequestServersResponse->RefreshComplete((HServerListRequest)1, eNoServersListedOnMasterServer); return (HServerListRequest)1;",
    "ISteamMatchmakingServers::RequestFavoritesServerList": "if (pRequestServersResponse) pRequestServersResponse->RefreshComplete((HServerListRequest)1, eNoServersListedOnMasterServer); return (HServerListRequest)1;",
    "ISteamMatchmakingServers::RequestHistoryServerList": "if (pRequestServersResponse) pRequestServersResponse->RefreshComplete((HServerListRequest)1, eNoServersListedOnMasterServer); return (HServerListRequest)1;",
    "ISteamMatchmakingServers::RequestSpectatorServerList": "if (pRequestServersResponse) pRequestServersResponse->RefreshComplete((HServerListRequest)1, eNoServersListedOnMasterServer); return (HServerListRequest)1;",
    "ISteamMatchmakingServers::GetServerCount": "(void)hRequest; return 0;",
    "ISteamMatchmakingServers::IsRefreshing": "(void)hRequest; return false;",
    # -- ISteamUGC: workshop, no items present (queries complete empty) ---
    "ISteamUGC::SendQueryUGCRequest": "SteamUGCQueryCompleted_t r{}; r.m_handle = handle; r.m_eResult = k_EResultOK; r.m_unNumResultsReturned = 0; r.m_unTotalMatchingResults = 0; return emu::QueueCallResult(r);",
    "ISteamUGC::GetQueryUGCResult": "return false;",
    "ISteamUGC::GetNumSubscribedItems": "return 0;",
    "ISteamUGC::GetSubscribedItems": "(void)pvecPublishedFileID; (void)cMaxEntries; return 0;",
    "ISteamUGC::GetItemState": "(void)nPublishedFileID; return k_EItemStateNone;",
    "ISteamUGC::GetItemInstallInfo": "return false;",
    "ISteamUGC::GetItemDownloadInfo": "return false;",
    "ISteamUGC::GetItemUpdateProgress": "return k_EItemUpdateStatusInvalid;",
    "ISteamUGC::CreateItem": "CreateItemResult_t r{}; r.m_eResult = k_EResultOK; r.m_nPublishedFileId = 0; return emu::QueueCallResult(r);",
    "ISteamUGC::StartItemUpdate": "(void)nConsumerAppId; (void)nPublishedFileID; return 1;",
    "ISteamUGC::SubmitItemUpdate": "SubmitItemUpdateResult_t r{}; r.m_eResult = k_EResultOK; return emu::QueueCallResult(r);",
    "ISteamUGC::SubscribeItem": "RemoteStorageSubscribePublishedFileResult_t r{}; r.m_eResult = k_EResultOK; r.m_nPublishedFileId = nPublishedFileID; return emu::QueueCallResult(r);",
    "ISteamUGC::UnsubscribeItem": "RemoteStorageSubscribePublishedFileResult_t r{}; r.m_eResult = k_EResultOK; r.m_nPublishedFileId = nPublishedFileID; return emu::QueueCallResult(r);",
    "ISteamUGC::RequestUGCDetails": "SteamUGCRequestUGCDetailsResult_t r{}; return emu::QueueCallResult(r);",
    "ISteamUGC::BInitWorkshopForGameServer": "return true;",
    "ISteamUGC::CreateQueryUserUGCRequest": "return 1;",
    "ISteamUGC::CreateQueryUGCDetailsRequest": "return 1;",
}

# Methods hidden behind STEAM_PRIVATE_API in the headers. They are NOT in
# steam_api.json (they're "private"), but they still occupy real vtable slots,
# so the concrete class must override them or it stays abstract and, worse, the
# layout would be wrong. Because we inherit the reference header, the base class
# fixes slot order -- we only need to supply an override (declared order here is
# irrelevant to the ABI). Each entry: (decl-without-trailing-semicolon, returntype).
EXTRA_METHODS = {
    "ISteamClient": [
        ("void RunFrame()", "void"),
        ("void DEPRECATED_Set_SteamAPI_CPostAPIResultInProcess( void (*)() )", "void"),
        ("void DEPRECATED_Remove_SteamAPI_CPostAPIResultInProcess( void (*)() )", "void"),
        ("void Set_SteamAPI_CCheckCallbackRegisteredInProcess( SteamAPI_CheckCallbackRegistered_t func )", "void"),
        ("void DestroyAllInterfaces()", "void"),
    ],
    "ISteamUtils": [
        ("bool GetCSERIPPort( uint32 *unIP, uint16 *usPort )", "bool"),
        ("void RunFrame()", "void"),
    ],
    "ISteamGameServer": [
        ("bool InitGameServer( uint32 unIP, uint16 usGamePort, uint16 usQueryPort, uint32 unFlags, AppId_t nGameAppId, const char *pchVersionString )", "bool"),
        ("void SetMasterServerHeartbeatInterval_DEPRECATED( int iHeartbeatInterval )", "void"),
        ("void ForceMasterServerHeartbeat_DEPRECATED()", "void"),
    ],
    # The real ToString vtable slots take size_t cbBuf; steam_api.json instead
    # describes the inline uint32 helper wrappers (skipped above). Override the
    # true pure virtuals here so the class is concrete and the slots line up.
    "ISteamNetworkingUtils": [
        ("void SteamNetworkingIPAddr_ToString( const SteamNetworkingIPAddr &addr, char *buf, size_t cbBuf, bool bWithPort )", "void"),
        ("void SteamNetworkingIdentity_ToString( const SteamNetworkingIdentity &identity, char *buf, size_t cbBuf )", "void"),
    ],
}


# Methods that appear in steam_api.json but are INLINE, NON-VIRTUAL convenience
# wrappers in the header -- they occupy no vtable slot, so a concrete class must
# NOT `override` them (it inherits them from the base). Overriding would fail to
# compile ("marked override, but does not override"). The flat C wrapper still
# forwards to the inherited inline method, so callers keep working.
#
# ISteamNetworkingUtils exposes a large family of these (typed Set*/Get* config
# helpers that all funnel into the SetConfigValue/GetConfigValue virtuals, plus
# the ToString and callback-registration shims). This is the complete set the
# compiler rejects for the current SDK; revisit on a version bump.
NONVIRTUAL_HELPERS = {
    "ISteamNetworkingUtils": {
        "InitRelayNetworkAccess", "IsFakeIPv4",
        "SetGlobalConfigValueInt32", "SetGlobalConfigValueFloat",
        "SetGlobalConfigValueString", "SetGlobalConfigValuePtr",
        "SetConnectionConfigValueInt32", "SetConnectionConfigValueFloat",
        "SetConnectionConfigValueString",
        "SetGlobalCallback_SteamNetConnectionStatusChanged",
        "SetGlobalCallback_SteamNetAuthenticationStatusChanged",
        "SetGlobalCallback_SteamRelayNetworkStatusChanged",
        "SetGlobalCallback_FakeIPResult",
        "SetGlobalCallback_MessagesSessionRequest",
        "SetGlobalCallback_MessagesSessionFailed",
        "SetConfigValueStruct",
        "SteamNetworkingIPAddr_ToString", "SteamNetworkingIdentity_ToString",
    },
}


def accessor(classname):
    """Singleton accessor function name for an interface, e.g. SteamUser_."""
    short = classname[len("ISteam"):] if classname.startswith("ISteam") else classname
    return "Steam" + short + "_"


def flat_type(t):
    """Map a C++ type to how it crosses the flat boundary (steam_api_flat.h)."""
    if t == "CSteamID":
        return "uint64_steamid"
    if t == "CGameID":
        return "uint64_gameid"
    return t


def iface_default(iface, m):
    """Per-interface default body for methods not in BODIES, or None to fall back
    to the generic neutral default. ISteamUGC has ~60 configuration setters
    (Set*/Add*/Remove*/Release*) that should report success; its Get* accessors
    keep the neutral 'no data' (false/0)."""
    if iface == "ISteamUGC":
        n = m["methodname"]
        ret = m["returntype"]
        if ret == "bool" and n.startswith(("Set", "Add", "Remove", "Release", "Init")):
            return " return true; "
        # Query/update creators must return a usable (non-zero) handle.
        if ret in ("UGCQueryHandle_t", "UGCUpdateHandle_t"):
            return " return 1; "
    return None


def default_body(ret):
    if ret == "void":
        return ""
    if ret == "const char *":
        return ' return ""; '
    return " return {}; "


# struct name -> [(fieldname, fieldtype), ...] for every callback struct in the
# JSON; filled by main() and consumed by callresult_default below.
CALLRESULT_FIELDS = {}

# BODIES keys gen_class actually consumed; main() fails the regen on leftovers
# (a leftover means a typo'd/renamed key silently reverting a method to a stub).
USED_BODY_KEYS = set()

# Call-results produced by the game-server interfaces must be queued on the
# game-server pipe: a dedicated/listen server pumps only that pipe, so a
# client-pipe completion would never reach its CCallResult.
GS_CALLRESULT_IFACES = {"ISteamGameServer", "ISteamGameServerStats"}


def callresult_default(iface, m):
    """Default body for a stubbed method annotated with a `callresult` struct.

    A game parks a CCallResult on the returned SteamAPICall_t; the old neutral
    stub returned handle 0 and never queued anything, so the game waited forever
    (the single most common "runs but hangs" failure). Instead, complete every
    promised async result with a zeroed instance of its struct marked successful
    (m_eResult/m_result = k_EResultOK, m_bSuccess = 1) -- "succeeded with empty
    data", the async twin of the neutral-stub rule. Methods where zeroed-success
    is wrong (needs real data, or must NOT succeed, e.g. StartPurchase) override
    this in BODIES."""
    cr = m["callresult"]
    sets = []
    for fn, ft in CALLRESULT_FIELDS.get(cr, []):
        if fn in ("m_eResult", "m_result"):
            sets.append("r.%s = k_EResultOK;" % fn)
        elif fn == "m_bSuccess":
            sets.append("r.%s = %s;" % (fn, "true" if ft == "bool" else "1"))
    queue = "QueueGameServerCallResult" if iface in GS_CALLRESULT_IFACES else "QueueCallResult"
    return " %s r{}; %sreturn emu::%s(r); " % (
        cr, "".join(s + " " for s in sets), queue)


def render_params(params):
    """C++ parameter list as declared (for the override signature)."""
    return ", ".join("%s %s" % (p["paramtype"], p["paramname"]) for p in params)


def method_decl(m):
    return "%s %s(%s)" % (m["returntype"], m["methodname"], render_params(m["params"]))


def gen_class(iface, vers=None):
    name = iface["classname"]
    cls = "C" + name[len("ISteam"):] if name.startswith("ISteam") else "C" + name
    skip = NONVIRTUAL_HELPERS.get(name, set())
    # Multiply-inherit every historical version of this interface (renamed
    # classes in versions.h) so each version keeps its own correct sub-vtable;
    # dispatch (registry.cpp) hands a game the sub-object for the version it asks
    # for. The reference header `name` is the newest version = the primary base.
    # Several version strings can share one class (vtable-identical versions are
    # deduplicated; alias versions reuse a neighbor's class; a version can even
    # dedupe onto the newest interface itself) -- inherit each class once.
    bases = [name]
    if vers:
        for _, vc in vers["version_classes"].get(name, []):
            if vc not in bases:
                bases.append(vc)
    lines = ["class %s final : public %s {" % (cls, ", public ".join(bases)), "public:",
             "\t// true on the game-server-side instance (registry <Iface>GS_()): the"
             " same class serves both paths, but e.g. P2P must send/receive with the"
             " game-server identity there.",
             "\tbool m_gs = false;"]
    for m in iface["methods"]:
        # Inline, non-virtual JSON helpers hold no vtable slot and are inherited
        # from the base; overriding them would fail to compile.
        if m["methodname"] in skip:
            continue
        key = "%s::%s" % (name, m["methodname"])
        # Overloaded methods (same name, different params) collide on the plain
        # key, so a signature-qualified key "Iface::Method(t1,t2)" wins if present.
        sigkey = "%s(%s)" % (key, ",".join(p["paramtype"] for p in m["params"]))
        body = BODIES.get(sigkey)
        if body is not None:
            USED_BODY_KEYS.add(sigkey)
        else:
            body = BODIES.get(key)
            if body is not None:
                USED_BODY_KEYS.add(key)
        if body is None:
            log = 'EMU_LOG("%s::%s");' % (name, m["methodname"])
            # A promised call-result MUST complete (see callresult_default);
            # only then do per-interface / generic neutral defaults apply.
            if m.get("callresult"):
                special = callresult_default(name, m)
            else:
                special = iface_default(name, m)
            body = log + (special if special is not None else default_body(m["returntype"]))
        else:
            body = ('EMU_LOG("%s::%s"); ' % (name, m["methodname"])) + body
        lines.append("\t%s override { %s }" % (method_decl(m), body))
    for decl, ret in EXTRA_METHODS.get(name, []):
        logname = decl.split("(")[0].split()[-1].lstrip("*")
        # EXTRA_METHODS slots can carry real behavior too (the ToString pair).
        key = "%s::%s" % (name, logname)
        body = BODIES.get(key)
        if body is not None:
            USED_BODY_KEYS.add(key)
            body = ('EMU_LOG("%s::%s"); ' % (name, logname)) + body
        else:
            body = ('EMU_LOG("%s::%s");' % (name, logname)) + default_body(ret)
        lines.append("\t%s override { %s }" % (decl, body))
    # Overrides for methods that only ever existed in OLD versions (removed or
    # retyped since) so the class is concrete for every inherited base, plus the
    # renamed "_compat" forwarders for return-type-changed methods.
    if vers:
        for ov in vers["extras"].get(name, []):
            lines.append("\t" + ov)
    lines.append("};")
    return cls, "\n".join(lines)


def gen_flat_wrapper(iface):
    name = iface["classname"]
    out = []
    for m in iface["methods"]:
        ret = m["returntype"]
        fret = m.get("returntype_flat", flat_type(ret))
        # parameter list in flat form: self first, then marshaled params.
        decl_params = ["%s* self" % name]
        call_args = []
        for p in m["params"]:
            pt = p["paramtype"]
            fpt = p.get("paramtype_flat", flat_type(pt))
            decl_params.append("%s %s" % (fpt, p["paramname"]))
            if pt == "CSteamID":
                call_args.append("CSteamID(%s)" % p["paramname"])
            elif pt == "CGameID":
                call_args.append("CGameID(%s)" % p["paramname"])
            else:
                call_args.append(p["paramname"])
        sig = "S_API %s %s( %s )" % (fret, m["methodname_flat"], ", ".join(decl_params))
        call = "self->%s(%s)" % (m["methodname"], ", ".join(call_args))
        if ret == "void":
            body = "\t%s;" % call
        elif ret == "CSteamID":
            body = "\treturn %s.ConvertToUint64();" % call
        else:
            body = "\treturn %s;" % call
        # `self` may be a LEGACY sub-object (from a version-exact locator) whose
        # vtable is the OLD layout; these wrappers were compiled against the
        # NEWEST, so re-anchor to the instance's primary interface pointer.
        norm = "\tself = static_cast<%s*>(emu::FlatNormalize(self));\n" % name
        out.append("%s {\n\tif (!self) return%s;\n%s%s\n}" % (
            sig, "" if ret == "void" else (' ""' if ret == "const char *" else " {}"), norm, body))
    return "\n".join(out)


def main():
    data = json.load(open(JSON))
    ifaces = [i for i in data["interfaces"] if i["classname"] in HEADERS]
    os.makedirs(OUT, exist_ok=True)

    for s in data["callback_structs"]:
        CALLRESULT_FIELDS[s["struct"]] = [
            (f["fieldname"], f["fieldtype"]) for f in s.get("fields", [])]

    # Reconstruct every historical interface version from the SDK git history
    # (plus tools/inter_versions/) so a game that asks for an older version gets
    # that version's exact vtable. Failure is FATAL: regenerating against e.g. a
    # tarball SDK would otherwise silently drop the whole feature from the
    # checked-in sources. STEAMEMU_NEWEST_ONLY=1 opts out explicitly (the
    # generated library then serves the newest vtable for every version string).
    if os.environ.get("STEAMEMU_NEWEST_ONLY") == "1":
        sys.stderr.write("gen.py: STEAMEMU_NEWEST_ONLY=1 -- skipping historical "
                         "version extraction; every version string gets the "
                         "newest vtable.\n")
        vers = None
    else:
        vers = oldversions.build(SDK, expect=VERSIONS)

    used_headers = sorted(set(HEADERS[i["classname"]] for i in ifaces)) + [
        "isteamgameserver.h", "isteamgameserverstats.h"]
    includes = "\n".join('#include "steam/%s"' % h for h in sorted(set(used_headers)))

    # -- versions.h : all renamed historical interface classes --------------
    with open(os.path.join(OUT, "versions.h"), "w") as f:
        f.write("// GENERATED by tools/gen.py -- do not edit.\n#pragma once\n")
        if vers:
            f.write(vers["prelude"])
            f.write("\n\n")
            f.write(vers["class_defs"])
            f.write("\n")
            f.write(vers["prelude_undef"])
        else:
            f.write('#include "steam/steam_api.h"\n')

    # -- interfaces.h --------------------------------------------------------
    classes = []
    class_names = {}
    for i in ifaces:
        cls, txt = gen_class(i, vers)
        class_names[i["classname"]] = cls
        classes.append(txt)
    unused = set(BODIES) - USED_BODY_KEYS
    if unused:
        sys.exit("gen.py: BODIES keys matched no method (typo or renamed in the "
                 "SDK?):\n  " + "\n  ".join(sorted(unused)))
    with open(os.path.join(OUT, "interfaces.h"), "w") as f:
        f.write("// GENERATED by tools/gen.py -- do not edit.\n#pragma once\n")
        f.write('#include "emu_common.h"\n#include "registry.h"\n')
        f.write('#include "auth.h"\n#include "config.h"\n#include "dispatch.h"\n#include "net.h"\n#include "storage.h"\n#include "services.h"\n')
        # ActivateGameOverlay* / IsOverlayEnabled bodies call the ImGui-free
        # overlay facade. overlay.h has no ImGui/Win32 type, so this stays out of
        # the "no ImGui in generated TUs" rule.
        f.write('#include "overlay/overlay.h"\n')
        f.write('#include <cstdio>\n#include <cstring>\n')
        f.write(includes + "\n")
        f.write('#include "versions.h"\n')
        f.write("\nnamespace emu {\n\n")
        f.write("\n\n".join(classes))
        f.write("\n\n} // namespace emu\n")

    # -- registry.h : singleton accessor declarations -----------------------
    with open(os.path.join(OUT, "registry.h"), "w") as f:
        f.write("// GENERATED by tools/gen.py -- do not edit.\n#pragma once\n")
        f.write(includes + "\n\nnamespace emu {\n")
        for i in ifaces:
            if i["classname"] in NO_SINGLETON:
                continue
            f.write("%s* %s();\n" % (i["classname"], accessor(i["classname"])))
        f.write("// Game-server-side instances of the interfaces both sides own.\n")
        for cn in GS_SHARED:
            f.write("%s* %sGS_();\n" % (cn, accessor(cn)[:-1]))
        f.write("// Return the interface sub-object matching an EXACT requested version\n")
        f.write("// string (its own correct sub-vtable), or the newest if unrecognized.\n")
        for i in ifaces:
            cn = i["classname"]
            if cn in NO_SINGLETON or cn not in VERSIONS:
                continue
            f.write("void* %sForVersion(const char* version);\n" % accessor(cn))
        for cn in GS_SHARED:
            f.write("void* %sForVersionGS(const char* version);\n" % accessor(cn))
        f.write("// Locate an interface by its exact version string (SteamInternal_*).\n")
        f.write("void* FindInterface(const char* version);\n")
        f.write("// Same, for the game-server locators: GS_SHARED interfaces resolve to\n")
        f.write("// their game-server instance, everything else falls through.\n")
        f.write("void* FindInterfaceGS(const char* version);\n")
        f.write("// Map a pointer INTO one of our singletons (e.g. a legacy version\n")
        f.write("// sub-object handed out by the locators above) back to that instance's\n")
        f.write("// primary (newest-layout) interface pointer; anything else passes through.\n")
        f.write("void* FlatNormalize(void* self);\n")
        f.write("} // namespace emu\n")

    # -- registry.cpp : singleton definitions + version dispatch ------------
    with open(os.path.join(OUT, "registry.cpp"), "w") as f:
        f.write("// GENERATED by tools/gen.py -- do not edit.\n")
        f.write('#include "interfaces.h"\n#include <cstdint>\n#include <cstring>\n\n')
        # A couple of interfaces declare a protected destructor but leave it for
        # the (real) library to define ("Silence some warnings"). Our derived
        # classes reference it, so supply an empty out-of-line definition.
        for cn in ("ISteamNetworkingUtils", "ISteamNetworkingSockets"):
            f.write("%s::~%s() {}\n" % (cn, cn))
        f.write('\nnamespace emu {\n\n')
        for i in ifaces:
            if i["classname"] in NO_SINGLETON:
                continue
            cls = class_names[i["classname"]]
            f.write("%s* %s() { static %s s; return &s; }\n" % (i["classname"], accessor(i["classname"]), cls))
        f.write("// Game-server-side instances (m_gs = true): same class + shared state,\n")
        f.write("// but methods that care (legacy P2P) use the game-server identity.\n")
        for cn in GS_SHARED:
            cls = class_names[cn]
            f.write("%s* %sGS_() { static %s* s = ([]{ auto* p = new %s; p->m_gs = true; return p; })(); return s; }\n"
                    % (cn, accessor(cn)[:-1], cls, cls))
        # A game is compiled against ONE version of each interface and calls its
        # methods by vtable slot. Because the concrete class multiply-inherits
        # every historical version (versions.h), casting its pointer to a given
        # version's base yields THAT version's correct sub-vtable. The
        # (void*)(<ver>*) middle cast stops the compiler re-adjusting the pointer
        # back. Each <Iface>_ForVersion() returns the sub-object for the EXACT
        # requested version, or the newest one for an unrecognized/newer string.
        def for_version(f, cn, singleton, gs=False):
            cls = class_names[cn]
            base = VERSIONS[cn]
            fname = accessor(cn) + ("ForVersionGS" if gs else "ForVersion")
            f.write("void* %s(const char* v) {\n" % fname)
            f.write("\t%s* p = static_cast<%s*>(%s());\n" % (cls, cls, singleton))
            # The real client treats a NULL version string as "the default";
            # ISteamClient::GetISteam* pass pchVersion straight through, so a
            # game calling e.g. GetISteamUtils(pipe, NULL) must not crash.
            f.write("\tif (!v) return (void*)(%s*)p;\n" % cn)
            olds = (vers["version_classes"].get(cn, []) if vers else [])
            for ver, vclass in olds:
                f.write('\tif (std::strcmp(v, "%s") == 0) { EMU_DEBUG("%s: serving legacy interface %%s", v); return (void*)(%s*)p; }\n'
                        % (ver, cn, vclass))
            # Versions the REAL steamclient returns NULL for:
            # the games built against them handle NULL; a newest vtable would be
            # wrong slots.
            for ver in (vers["null_versions"].get(cn, []) if vers else []):
                f.write('\tif (std::strcmp(v, "%s") == 0) { EMU_WARN("%s: %%s is not served (the real client returns NULL for it)", v); return nullptr; }\n'
                        % (ver, cn))
            # Unknown / newer-than-us version strings fall back to the newest
            # vtable so init cannot fail on a known interface -- but LOUDLY: for
            # a native C++ game an unrecognized OLD version means wrong slots.
            f.write('\tif (std::strcmp(v, "%s") != 0) EMU_WARN("%s: unknown version %%s -> serving newest (%s); an older-than-%s game would get wrong vtable slots", v);\n'
                    % (base, cn, base, base))
            f.write('\treturn (void*)(%s*)p;\t// %s (newest) or unrecognized\n' % (cn, base))
            f.write("}\n")

        f.write("\n")
        for i in ifaces:
            cn = i["classname"]
            if cn in NO_SINGLETON or cn not in VERSIONS:
                continue
            for_version(f, cn, accessor(cn))
        for cn in GS_SHARED:
            for_version(f, cn, accessor(cn)[:-1] + "GS_", gs=True)

        # Interface version strings minus their trailing digits ("family stem"):
        # this locates the INTERFACE a request belongs to; the per-interface
        # ForVersion above then picks the exact version. Matching by equal stem
        # length + prefix keeps e.g. SteamMatchMakingServers distinct from
        # SteamMatchMaking. An interface can carry MORE than one stem across its
        # history -- SDK 1.24-1.34's controller version string is the digitless
        # "STEAMCONTROLLER_INTERFACE_VERSION" (its own 33-char stem) -- so every
        # distinct stem among an interface's known versions gets an arm.
        # Returning NULL for a known interface would make a game's wrapper treat
        # SteamAPI_Init as failed, so an unknown version of a known interface
        # still resolves (to our newest) rather than NULL.
        f.write("\nstatic size_t IfaceStemLen(const char* s) {\n"
                "\tsize_t n = std::strlen(s);\n"
                "\twhile (n > 0 && s[n-1] >= '0' && s[n-1] <= '9') --n;\n"
                "\treturn n;\n}\n")

        def stem(s):
            n = len(s)
            while n > 0 and s[n - 1].isdigit():
                n -= 1
            return s[:n]

        def stems_of(cn):
            vs = {VERSIONS[cn]}
            if vers:
                vs.update(v for v, _ in vers["version_classes"].get(cn, []))
                vs.update(vers["null_versions"].get(cn, []))
            return sorted({stem(x) for x in vs})

        def emit_find(f, fname, cns, fv_suffix, fallback):
            entries = []
            for cn in cns:
                for st in stems_of(cn):
                    entries.append((st, accessor(cn) + fv_suffix))
            entries.sort(key=lambda e: (-len(e[0]), e[0]))
            f.write("\nvoid* %s(const char* v) {\n\tif (!v) return nullptr;\n" % fname)
            f.write("\tsize_t vn = IfaceStemLen(v);\n")
            for st, fv in entries:
                f.write('\tif (vn == %d && std::strncmp(v, "%s", %d) == 0) return %s(v);\n'
                        % (len(st), st, len(st), fv))
            f.write("\treturn %s;\n}\n" % fallback)

        cns = [i["classname"] for i in ifaces
               if i["classname"] not in NO_SINGLETON and i["classname"] in VERSIONS]
        emit_find(f, "FindInterface", cns, "ForVersion", "nullptr")
        # Game-server locator: the interfaces both sides own resolve to their
        # game-server sub-object (still version-correct); everything else falls
        # through to the shared client dispatch.
        emit_find(f, "FindInterfaceGS", GS_SHARED, "ForVersionGS", "FindInterface(v)")

        # The flat exports dispatch on `self` with the NEWEST vtable layout, but
        # the locators above may have handed the game a LEGACY sub-object (its
        # vtable is the OLD layout, so a newest-slot call through it would hit
        # the wrong virtual). Every sub-object lives inside its concrete
        # singleton, so an address-range check maps any such pointer back to the
        # instance's primary (newest) interface pointer. Identity for foreign
        # pointers, and instance-preserving for the game-server-side objects.
        f.write("\nvoid* FlatNormalize(void* self) {\n")
        f.write("\tif (!self) return self;\n")
        f.write("\tstruct Range { uintptr_t base; size_t size; void* primary; };\n")
        f.write("\tstatic const Range k[] = {\n")
        for i in ifaces:
            cn = i["classname"]
            if cn in NO_SINGLETON:
                continue
            cls = class_names[cn]
            acc = accessor(cn)
            f.write("\t\t{ (uintptr_t)static_cast<%s*>(%s()), sizeof(%s), (void*)%s() },\n"
                    % (cls, acc, cls, acc))
        for cn in GS_SHARED:
            cls = class_names[cn]
            acc = accessor(cn)[:-1] + "GS_"
            f.write("\t\t{ (uintptr_t)static_cast<%s*>(%s()), sizeof(%s), (void*)%s() },\n"
                    % (cls, acc, cls, acc))
        f.write("\t};\n")
        f.write("\tconst uintptr_t s = (uintptr_t)self;\n")
        f.write("\tfor (const auto& e : k)\n")
        f.write("\t\tif (s >= e.base && s - e.base < e.size) return e.primary;\n")
        f.write("\treturn self;\n}\n\n} // namespace emu\n")

    # -- flat_api.cpp --------------------------------------------------------
    with open(os.path.join(OUT, "flat_api.cpp"), "w") as f:
        f.write("// GENERATED by tools/gen.py -- do not edit.\n")
        # steam_api_flat.h supplies the uint64_steamid/uint64_gameid typedefs AND
        # the reference prototype of every flat export, so the compiler verifies
        # each definition below matches Valve's ABI exactly. S_API already implies
        # extern "C" + default visibility. registry.h (not interfaces.h) is
        # enough: the wrappers dispatch through interface pointers and
        # FlatNormalize, never a concrete class -- and it keeps the ~20k-line
        # versions.h out of the largest generated translation unit.
        f.write('#include "registry.h"\n#include "steam/steam_api_flat.h"\n\n')
        by_name = {i["classname"]: i for i in data["interfaces"]}
        for i in ifaces + [by_name[n] for n in FLAT_ONLY]:
            f.write("// ---- %s ----\n" % i["classname"])
            f.write(gen_flat_wrapper(i))
            f.write("\n\n")
        f.write(FAKEUDP_STUBS)

        # Versioned global accessors, e.g. SteamAPI_SteamUser_v023(). These are
        # S_API exports in steam_api_flat.h that games/loaders call directly to
        # obtain an interface. Each returns the corresponding singleton; the
        # SteamGameServer* variants of the GS_SHARED interfaces return the
        # game-server-side instance (other GameServer* variants share the client
        # singleton -- there is only one local identity/state).
        flat_hdr = open(os.path.join(SDK, "public", "steam", "steam_api_flat.h"),
                        encoding="latin-1").read()
        f.write("\n// ---- versioned global accessors ----\n")
        for rettype, fname in re.findall(
                r'S_API\s+(ISteam\w+)\s*\*\s*(SteamAPI_Steam\w+_v\d+)\s*\(\s*\)', flat_hdr):
            acc = accessor(rettype)
            if rettype in GS_SHARED and fname.startswith("SteamAPI_SteamGameServer"):
                acc = acc[:-1] + "GS_"
            f.write("S_API %s* %s() { return emu::%s(); }\n" % (rettype, fname, acc))

    total = sum(len(i["methods"]) for i in ifaces)
    print("generated %d interfaces, %d methods -> %s" % (len(ifaces), total, OUT))


if __name__ == "__main__":
    main()
