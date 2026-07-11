//====== steamemu ============================================================
// LAN networking backend.
//
// The headline feature: two emulator instances discover each other and exchange
// game packets with no Valve backend. Isolated behind this internal interface so
// the emulated ISteam* networking/matchmaking/friends methods call in here and
// the transport can evolve independently.
//
//   discovery  -- IP multicast beacons announce {SteamID, name, AppID, dataport}.
//                 Multicast (not broadcast) so multiple instances on ONE host --
//                 the common test setup -- all receive via loopback.
//   transport  -- direct unicast UDP to a peer's announced data port; per-peer,
//                 per-channel receive queues back the legacy ISteamNetworking
//                 P2P API. A background thread pumps both sockets.
//
// Only peers announcing the SAME AppID are surfaced, mirroring Steam (you only
// see players in the same game).
//============================================================================
#pragma once

#include "steam/steamclientpublic.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct SteamNetworkingMessage_t;

namespace emu {

class NetBackend {
public:
	static NetBackend& Get();

	// Idempotent: bring up sockets + the pump thread on first use. Called lazily
	// the first time a game touches a networking/friends/matchmaking method.
	void EnsureStarted();
	void Stop();

	// --- legacy ISteamNetworking P2P -----------------------------------------
	// gs=true is the game-server side of this instance (SteamGameServerNetworking):
	// packets are SENT with the LocalGSSteamID identity, and each side receives
	// only traffic addressed to its identity -- a listen-server game runs both in
	// one process and the client's pump must not steal the server's packets (or
	// vice versa). A remote client connecting to the id from LobbyGameCreated_t
	// then sees replies from the server id it expects.
	bool SendP2PPacket(CSteamID target, const void* data, uint32_t cb, int channel, bool gs = false);
	bool IsP2PPacketAvailable(uint32_t* pcubMsgSize, int channel, bool gs = false);
	bool ReadP2PPacket(void* dest, uint32_t cbDest, uint32_t* pcbRead, CSteamID* sender, int channel, bool gs = false);
	bool CloseP2PChannel(CSteamID remote, int channel, bool gs = false);
	// ISteamNetworkingMessages: same transport, delivered as message objects.
	int ReceiveMessagesOnChannel(int channel, SteamNetworkingMessage_t** out, int maxMsgs);

	// --- discovered peers (backs ISteamFriends presence) ---------------------
	std::vector<CSteamID> Peers();       // peers in our AppID, recently seen
	std::string PeerName(CSteamID id);   // "" if unknown
	// Stable const char* for a peer name (ISteamFriends returns borrowed strings).
	// The pointer stays valid for the process lifetime.
	const char* PeerNameC(CSteamID id);
	bool IsPeer(CSteamID id);

	// --- lobbies (backs ISteamMatchmaking) -----------------------------------
	// Gossip model: every member multicasts its presence in each lobby it has
	// joined; the owner's presence also carries the authoritative lobby data.
	// State merges eventually-consistent, so there is no join handshake to fail.
	CSteamID CreateLobby(int lobbyType, int maxMembers);   // we become owner+member
	bool JoinLobby(CSteamID lobby);                        // mark joined; announce
	void LeaveLobby(CSteamID lobby);
	std::vector<CSteamID> KnownLobbies();                  // joinable, recently heard
	std::vector<CSteamID> LobbyMembers(CSteamID lobby);
	CSteamID LobbyOwner(CSteamID lobby);
	int LobbyMaxMembers(CSteamID lobby);
	bool SetLobbyData(CSteamID lobby, const char* key, const char* value);  // owner
	const char* GetLobbyDataC(CSteamID lobby, const char* key);             // borrowed
	int LobbyDataCount(CSteamID lobby);
	bool LobbyDataByIndex(CSteamID lobby, int i, char* keyOut, int keyLen, char* valOut, int valLen);
	bool SetLobbyJoinable(CSteamID lobby, bool joinable);
	// Game server registered on the lobby: stored on the owner's gossiped state;
	// every member (and the caller) gets LobbyGameCreated_t -- the "host started
	// the game, connect now" signal.
	void SetLobbyGameServer(CSteamID lobby, uint32_t ip, uint16_t port, CSteamID server);
	bool GetLobbyGameServer(CSteamID lobby, uint32_t* ip, uint16_t* port, CSteamID* server);
	// Per-member data: each member gossips its own KV; only our own is writable.
	bool SetLobbyMemberData(CSteamID lobby, const char* key, const char* value);
	const char* GetLobbyMemberDataC(CSteamID lobby, CSteamID member, const char* key);  // borrowed
	// Lobby chat: multicast to members; each gets a LobbyChatMsg_t and reads the
	// body back by chat-id.
	bool SendLobbyChatMsg(CSteamID lobby, const void* body, int cb);
	int GetLobbyChatEntry(CSteamID lobby, int iChatID, CSteamID* user, void* data, int cbData, int* chatType);
	// Ask for a not-joined lobby's data; fires LobbyDataUpdate_t and keeps it
	// updated as the owner re-gossips. Returns false if we've never heard of it.
	bool RequestLobbyData(CSteamID lobby);
	// RequestLobbyList filters (consumed by the next RequestLobbyList()).
	void AddLobbyListStringFilter(const char* key, const char* value, int cmp);
	void AddLobbyListNumFilter(const char* key, int value, int cmp);
	void AddLobbyListSlotsFilter(int slotsAvailable);
	void SetLobbyListResultCount(int maxResults);
	int RequestLobbyList();               // apply+clear filters, cache, return count
	CSteamID LobbyListByIndex(int i);     // into the last RequestLobbyList result

	// --- modern ISteamNetworkingSockets (connection-oriented P2P) ------------
	// Connection/socket handles are opaque uint32s. Status transitions are
	// reported to the game as SteamNetConnectionStatusChangedCallback_t.
	uint32_t CreateListenSocketP2P(int virtualPort);
	bool CloseListenSocket(uint32_t sock);
	uint32_t ConnectP2P(CSteamID remote, int virtualPort);
	int AcceptConnection(uint32_t conn);   // returns EResult as int
	bool CloseConnection(uint32_t conn);
	int SendToConnection(uint32_t conn, const void* data, uint32_t cb);  // EResult as int
	int ReceiveOnConnection(uint32_t conn, SteamNetworkingMessage_t** out, int maxMsgs);
	bool GetConnectionInfo(uint32_t conn, CSteamID* remoteOut, int* stateOut, uint32_t* listenOut);
	CSteamID ConnectionRemote(uint32_t conn);
	uint32_t CreatePollGroup();
	bool DestroyPollGroup(uint32_t pg);
	bool SetConnectionPollGroup(uint32_t conn, uint32_t pg);
	int ReceiveOnPollGroup(uint32_t pg, SteamNetworkingMessage_t** out, int maxMsgs);

private:
	NetBackend() = default;
	~NetBackend();
	struct Impl;
	std::shared_ptr<Impl> m_impl;   // shared with the detached pump thread
	std::mutex m_lifecycleMtx;      // guards EnsureStarted / Stop so they can't race
};

inline NetBackend& Net() { return NetBackend::Get(); }

// ISteamNetworkingUtils helpers (independent of a NetBackend instance).
// Allocate a SteamNetworkingMessage_t whose Release() frees it (and its buffer).
SteamNetworkingMessage_t* AllocNetworkingMessage(int cbBuffer);
// Monotonic microsecond timestamp for GetLocalTimestamp().
int64_t LocalTimestamp();
// This host's LAN IPv4 in HOST byte order (ISteamGameServer::GetPublicIP: games
// embed it in session/server info they gossip). 127.0.0.1 if undeterminable.
uint32_t LanIPv4();
// ISteamNetworkingUtils config store: remember tuning values so SetConfigValue
// reports success (the typed SetGlobalConfigValue* helpers all funnel into it;
// false reads as "networking broken") and GetConfigValue reads them back.
// Types/results are the ESteamNetworkingConfig* enums passed as int to keep
// steamnetworkingtypes.h out of this header.
bool NetConfigSet(int eValue, int scopeType, intptr_t scopeObj, int dataType, const void* pArg);
int NetConfigGet(int eValue, int scopeType, intptr_t scopeObj, int* outDataType,
                 void* pResult, size_t* cbResult);  // ESteamNetworkingGetConfigValueResult

} // namespace emu
