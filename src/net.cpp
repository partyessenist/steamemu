//====== steamemu ============================================================
// LAN networking backend implementation. See net.h.
//
// POSIX sockets (this host is Linux-only). Windows would swap in Winsock2; the
// socket calls are kept thin and guarded so that port is mechanical.
//============================================================================
#include "net.h"
#include "emu_common.h"
#include "config.h"
#include "dispatch.h"

#include "steam/isteamnetworking.h"         // P2PSessionRequest_t
#include "steam/isteamnetworkingsockets.h"  // SteamNetConnectionStatusChangedCallback_t
#include "steam/steamnetworkingtypes.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <tuple>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>   // GetAdaptersAddresses (enumerate local interfaces)
using socklen_t = int;
#define EMU_CLOSESOCKET closesocket
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>    // getifaddrs (enumerate local interfaces)
#include <net/if.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#define EMU_CLOSESOCKET ::close
using SOCKET = int;
static constexpr SOCKET INVALID_SOCKET = -1;
#endif

namespace emu {
namespace {

// Multicast group + port for discovery (site-local scope, admin range). The
// defaults live in config.cpp; a steamemu.ini may override them so isolated
// LAN segments don't cross-talk. Resolved once per process.
inline const char* DiscoveryGroup() {
	static std::string g = Config().DiscoveryAddress();
	return g.c_str();
}
inline uint16_t DiscoveryPort() { static uint16_t p = Config().DiscoveryPort(); return p; }
constexpr uint32_t kBeaconMagic = 0x53544d42;  // 'STMB'
constexpr uint32_t kDataMagic = 0x53544d44;    // 'STMD'
constexpr uint32_t kLobbyMagic = 0x53544d4c;   // 'STML'
constexpr uint32_t kChatMagic = 0x53544d43;    // 'STMC' lobby chat

// Enumerate this host's up, non-loopback IPv4 interface addresses (as in_addr
// s_addr, network byte order). Discovery multicast must be sent out EACH of them
// and the group joined on EACH of them: a host picks a single default multicast
// interface otherwise, and on Windows that default is frequently a virtual
// adapter (Hyper-V/VMware/WSL/VPN) rather than the LAN NIC, so the beacon never
// reaches other machines even though same-host (loopback) discovery works. This
// is why cross-machine discovery silently failed. Empty on failure (callers then
// fall back to the default-interface send, i.e. the previous behavior).
// A per-process epoch, forming the high 32 bits of outgoing chat message ids so
// that a peer which restarts (same SteamID) never reuses an id still held in a
// receiver's dedup window. Nonzero.
uint32_t ProcessEpoch() {
	static uint32_t e = [] {
		auto t = (uint32_t)std::chrono::steady_clock::now().time_since_epoch().count();
		return t ? t : 1u;
	}();
	return e;
}

// A local interface: its unicast address and its subnet's directed-broadcast
// address (both network byte order, s_addr). Multicast is sent out `addr` (via
// IP_MULTICAST_IF); a directed broadcast to `bcast` reaches every host on that
// subnet and is routed out the owning interface automatically -- belt-and-braces
// with multicast, since some LANs pass broadcast more reliably (IGMP snooping).
struct IfaceAddr { uint32_t addr; uint32_t bcast; };

// Directed broadcast from an address + prefix length (host-order math).
uint32_t BroadcastForPrefix(uint32_t addrNet, unsigned prefix) {
	if (prefix == 0 || prefix > 32) return INADDR_BROADCAST;
	uint32_t host = ntohl(addrNet);
	uint32_t mask = (prefix == 32) ? 0xFFFFFFFFu : (0xFFFFFFFFu << (32 - prefix));
	return htonl((host & mask) | ~mask);
}

std::vector<IfaceAddr> LocalIPv4Interfaces() {
	std::vector<IfaceAddr> out;
#if defined(_WIN32)
	ULONG sz = 15000;
	std::vector<uint8_t> buf(sz);
	ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
	ULONG ret = GetAdaptersAddresses(AF_INET, flags, nullptr,
	                                 reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &sz);
	if (ret == ERROR_BUFFER_OVERFLOW) {
		buf.resize(sz);
		ret = GetAdaptersAddresses(AF_INET, flags, nullptr,
		                           reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &sz);
	}
	if (ret == NO_ERROR) {
		for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
			if (a->OperStatus != IfOperStatusUp) continue;
			if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
			for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
				if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
				uint32_t s = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr)->sin_addr.s_addr;
				if (s && s != htonl(INADDR_LOOPBACK))
					out.push_back({s, BroadcastForPrefix(s, u->OnLinkPrefixLength)});
			}
		}
	}
#else
	struct ifaddrs* ifap = nullptr;
	if (getifaddrs(&ifap) == 0) {
		for (auto* ifa = ifap; ifa; ifa = ifa->ifa_next) {
			if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
			if (!(ifa->ifa_flags & IFF_UP)) continue;
			if (ifa->ifa_flags & IFF_LOOPBACK) continue;
			uint32_t s = reinterpret_cast<sockaddr_in*>(ifa->ifa_addr)->sin_addr.s_addr;
			if (!s || s == htonl(INADDR_LOOPBACK)) continue;
			uint32_t mask = (ifa->ifa_netmask && ifa->ifa_netmask->sa_family == AF_INET)
			              ? reinterpret_cast<sockaddr_in*>(ifa->ifa_netmask)->sin_addr.s_addr : 0;
			uint32_t bcast = mask ? ((s & mask) | ~mask) : INADDR_BROADCAST;
			out.push_back({s, bcast});
		}
		freeifaddrs(ifap);
	}
#endif
	return out;
}
// ISteamNetworkingSockets control/data messages (over the unicast data socket).
constexpr uint32_t kConnReq = 0x53435271;   // connect request
constexpr uint32_t kConnAcc = 0x53434163;   // connect accepted
constexpr uint32_t kConnMsg = 0x53434d67;   // data message
constexpr uint32_t kConnFin = 0x53434669;   // close
constexpr int kBeaconIntervalMs = 1000;
constexpr int kPeerTimeoutMs = 5000;
constexpr int kLobbyMemberTimeoutMs = 6000;

// Discovery beacon (fixed layout; both ends run the same build).
#pragma pack(push, 1)
struct Beacon {
	uint32_t magic;
	uint64_t steamID;
	uint32_t appID;
	uint16_t dataPort;
	char name[64];
};
// Header prefixed to every P2P data datagram. senderID is the identity that
// sent it (the GameServer-typed id when sent from the server side); destID is
// the identity it was addressed to, so the receiving process can route it to
// its client or game-server inbox (a listen-server runs both).
struct DataHdr {
	uint32_t magic;
	uint64_t senderID;
	uint64_t destID;
	int32_t channel;
	// payload follows
};
// ISteamNetworkingSockets control header (unicast). For kConnMsg the payload
// follows the header.
struct ConnHdr {
	uint32_t magic;
	uint64_t senderID;
	uint32_t srcHandle;   // sender's local connection handle
	uint32_t dstHandle;   // receiver's local connection handle (0 when unknown)
	int32_t virtualPort;  // for kConnReq
};
// Lobby presence, multicast by each member. Two KV blobs follow the header:
// first the owner's authoritative lobby data (dataLen bytes, owner msgs only),
// then the sender's own per-member data (memberDataLen bytes).
struct LobbyMsg {
	uint32_t magic;
	uint32_t appID;
	uint64_t lobbyID;
	uint64_t ownerID;
	uint64_t senderID;   // the member announcing presence
	int32_t maxMembers;
	int32_t lobbyType;
	uint8_t joinable;
	uint8_t isOwner;
	uint16_t _pad;
	// Lobby game server (SetLobbyGameServer), carried by owner msgs. gsUpdate is
	// a bump count (0 = never set); members fire LobbyGameCreated_t on change.
	uint32_t gsIP;
	uint16_t gsPort;
	uint16_t _pad2;
	uint64_t gsID;
	uint32_t gsUpdate;
	uint32_t dataLen;        // owner lobby-data KV blob length
	uint32_t memberDataLen;  // sender per-member KV blob length
};
// Lobby chat message, multicast to the lobby. Body follows the header.
struct ChatMsg {
	uint32_t magic;
	uint32_t appID;
	uint64_t lobbyID;
	uint64_t senderID;
	// Unique per logical message from this sender (high 32 bits: a per-process
	// epoch so a restarted peer's ids never collide with retained ones; low 32:
	// a counter). The receiver dedups by (senderID, seq): DiscoverySend fans one
	// chat message out every interface, and IP_MULTICAST_LOOP delivers each copy
	// locally, so without this a single chat line would appear N times. Beacons
	// and lobby gossip need no such id -- both are idempotent and drop self-echoes.
	uint64_t seq;
	uint8_t chatType;
	uint8_t _pad[3];
	uint32_t bodyLen;
};
#pragma pack(pop)

// Serialize a string map to a KV blob: [u32 count]{[u16 klen]key[u16 vlen]val}...
static void PackKV(const std::map<std::string, std::string>& kv, std::vector<uint8_t>& out) {
	uint32_t n = (uint32_t)kv.size();
	out.insert(out.end(), (uint8_t*)&n, (uint8_t*)&n + 4);
	for (auto& p : kv) {
		uint16_t kl = (uint16_t)p.first.size(), vl = (uint16_t)p.second.size();
		out.insert(out.end(), (uint8_t*)&kl, (uint8_t*)&kl + 2);
		out.insert(out.end(), p.first.begin(), p.first.end());
		out.insert(out.end(), (uint8_t*)&vl, (uint8_t*)&vl + 2);
		out.insert(out.end(), p.second.begin(), p.second.end());
	}
}

static bool UnpackKV(const uint8_t* p, int len, std::map<std::string, std::string>& kv) {
	int off = 0;
	if (len < 4) return false;
	uint32_t n; std::memcpy(&n, p, 4); off += 4;
	for (uint32_t i = 0; i < n; ++i) {
		if (off + 2 > len) return false;
		uint16_t kl; std::memcpy(&kl, p + off, 2); off += 2;
		if (off + kl > len) return false;
		std::string k((const char*)p + off, kl); off += kl;
		if (off + 2 > len) return false;
		uint16_t vl; std::memcpy(&vl, p + off, 2); off += 2;
		if (off + vl > len) return false;
		std::string v((const char*)p + off, vl); off += vl;
		kv[k] = v;
	}
	return true;
}

int64_t NowMs() {
	// Monotonic-ish millisecond clock. std::chrono steady_clock is fine here
	// (the workflow-script Date restriction does not apply to the C++ runtime).
	return static_cast<int64_t>(
		std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());
}

} // namespace

struct NetBackend::Impl {
	SOCKET discoSock = INVALID_SOCKET;
	SOCKET dataSock = INVALID_SOCKET;
	uint16_t dataPort = 0;

	// Local interface unicast addresses (for IP_MULTICAST_IF + group joins) and
	// their subnet directed-broadcast addresses, cached at Start() (see
	// LocalIPv4Interfaces). discoSendMtx serializes the set-IP_MULTICAST_IF +
	// sendto sequence in DiscoverySend, since beacons, lobby gossip and chat can
	// be sent from different threads on discoSock.
	std::vector<uint32_t> mcastIfaces;
	std::vector<uint32_t> bcastAddrs;
	std::mutex discoSendMtx;
	// Send a discovery-group datagram out every local interface (falling back to
	// the default interface if enumeration turned up nothing). Used for beacons,
	// lobby gossip and lobby chat -- everything addressed to the multicast group.
	void DiscoverySend(const void* data, int len);

	// Outgoing chat id counter (low 32 bits of ChatMsg::seq) and the receiver's
	// bounded dedup window of (senderID, seq) keys -- drops the N fan-out/loopback
	// copies of one chat line (see ChatMsg::seq). Guarded by mtx on the read side.
	std::atomic<uint32_t> chatCounter{0};
	std::set<std::pair<uint64_t, uint64_t>> seenChat;
	std::deque<std::pair<uint64_t, uint64_t>> seenChatOrder;
	bool ChatDuplicate(uint64_t sender, uint64_t seq) {  // call under mtx
		auto key = std::make_pair(sender, seq);
		if (!seenChat.insert(key).second) return true;   // already seen
		seenChatOrder.push_back(key);
		if (seenChatOrder.size() > 1024) {
			seenChat.erase(seenChatOrder.front());
			seenChatOrder.pop_front();
		}
		return false;
	}

	// The pump thread is DETACHED and shares ownership of this Impl (via a
	// shared_ptr the thread captures), so it is never join()ed -- joining from a
	// DLL's static destructor at process exit deadlocks on the Windows loader
	// lock. running=false asks it to stop; pumpExited reports when it has.
	std::atomic<bool> running{false};
	std::atomic<bool> pumpExited{false};

	~Impl() {
		if (discoSock != INVALID_SOCKET) EMU_CLOSESOCKET(discoSock);
		if (dataSock != INVALID_SOCKET) EMU_CLOSESOCKET(dataSock);
	}

	std::mutex mtx;
	struct Peer {
		CSteamID id;
		std::string name;
		sockaddr_in dataAddr{};
		int64_t lastSeen = 0;
	};
	std::map<uint64_t, Peer> peers;  // keyed by SteamID

	// Resolve a send/connect target to a discovered peer (call under mtx). Falls
	// back to matching by account id: a host's game-server identity
	// (ISteamGameServer::GetSteamID, k_EAccountTypeGameServer) shares the account
	// id of the client identity its beacons announce, and games connect to the
	// server id they got from LobbyGameCreated_t.
	Peer* FindPeer(uint64_t id) {
		auto it = peers.find(id);
		if (it != peers.end()) return &it->second;
		uint32_t account = CSteamID(static_cast<uint64>(id)).GetAccountID();
		for (auto& kv : peers)
			if (kv.second.id.GetAccountID() == account) return &kv.second;
		return nullptr;
	}

	struct Packet {
		CSteamID sender;
		std::vector<uint8_t> data;
	};
	// Per-channel receive queues, split by which local identity the datagram was
	// addressed to: [0] client, [1] game server (see DataHdr::destID).
	std::map<int, std::deque<Packet>> inbox[2];
	std::map<uint64_t, bool> knownSessions[2];  // peers we've delivered a request for

	struct ChatEntry {
		uint64_t sender;
		int chatType;
		std::vector<uint8_t> data;
	};
	struct Lobby {
		CSteamID id;
		CSteamID owner;
		int maxMembers = 0;
		int type = 0;
		bool joinable = true;
		bool joined = false;                     // are WE a member?
		bool watched = false;                    // RequestLobbyData on a lobby we're not in
		std::map<std::string, std::string> data; // authoritative iff we own it
		std::map<uint64_t, int64_t> members;     // memberID -> lastSeen
		std::map<uint64_t, std::map<std::string, std::string>> memberData;  // per member KV
		std::vector<ChatEntry> chat;             // received chat, indexed by chat-id
		// Game server registered on the lobby via SetLobbyGameServer (owner
		// gossips it; gsUpdate counts sets so members can detect changes).
		uint32_t gsIP = 0;
		uint16_t gsPort = 0;
		uint64_t gsID = 0;
		uint32_t gsUpdate = 0;
		int64_t lastHeard = 0;
	};
	std::map<uint64_t, Lobby> lobbies;

	// RequestLobbyList filters (consumed by the next RequestLobbyList).
	struct LobbyFilter {
		std::string key;
		bool numeric = false;
		std::string sval;
		int nval = 0;
		int cmp = 0;   // ELobbyComparison
	};
	std::vector<LobbyFilter> lobbyFilters;
	int filterSlots = -1;        // required open slots (-1 = no filter)
	int filterResultCount = -1;  // cap on results (-1 = no cap)
	std::vector<CSteamID> lastLobbyList;  // snapshot backing GetLobbyByIndex
	bool lobbyListRequested = false;      // has RequestLobbyList ever run?

	// --- ISteamNetworkingSockets connection state ---------------------------
	struct Conn {
		uint32_t handle = 0;
		CSteamID remote;
		sockaddr_in remoteAddr{};
		uint32_t remoteHandle = 0;   // peer's handle for this connection
		int state = 0;               // ESteamNetworkingConnectionState
		int virtualPort = 0;
		uint32_t listen = 0;         // owning listen socket (0 if outbound)
		uint32_t pollGroup = 0;
		std::deque<std::vector<uint8_t>> inbox;
	};
	std::map<uint32_t, Conn> conns;
	std::map<uint32_t, int> listenSockets;  // handle -> virtualPort
	std::vector<uint32_t> pollGroups;
	uint32_t nextHandle = 1;

	int64_t lastBeacon = 0;

	bool Start();
	void Pump();
	void SendBeacon();
	void SendLobbyPresence();
	void HandleChatMsg(const uint8_t* buf, int len);
	void HandleBeacon(const Beacon& b, const sockaddr_in& from);
	void HandleData(const uint8_t* buf, int len, const sockaddr_in& from);
	void HandleLobbyMsg(const uint8_t* buf, int len);
	void HandleConn(const uint8_t* buf, int len, const sockaddr_in& from);
	// Emit SteamNetConnectionStatusChangedCallback_t (caller must NOT hold mtx).
	void FireConnStatus(uint32_t handle, CSteamID remote, int oldState, int newState, uint32_t listen);
};

// --- socket setup -----------------------------------------------------------
static void SetNonBlocking(SOCKET s) {
#if defined(_WIN32)
	u_long mode = 1; ioctlsocket(s, FIONBIO, &mode);
#else
	int fl = fcntl(s, F_GETFL, 0); fcntl(s, F_SETFL, fl | O_NONBLOCK);
#endif
}

// Initialize Winsock once per process. We deliberately never WSACleanup: calling
// it during DLL detach is hazardous, and the OS reclaims Winsock at process exit.
static void EnsureWinsock() {
#if defined(_WIN32)
	static bool once = [] { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); return true; }();
	(void)once;
#endif
}

bool NetBackend::Impl::Start() {
	EnsureWinsock();
	// Discovery socket: joins the multicast group, reused across instances.
	discoSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (discoSock == INVALID_SOCKET) return false;
	int yes = 1;
	setsockopt(discoSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
#ifdef SO_REUSEPORT
	setsockopt(discoSock, SOL_SOCKET, SO_REUSEPORT, (const char*)&yes, sizeof(yes));
#endif
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(DiscoveryPort());
	if (bind(discoSock, (sockaddr*)&addr, sizeof(addr)) != 0) {
		EMU_CLOSESOCKET(discoSock); discoSock = INVALID_SOCKET; return false;
	}
	// Join the discovery group. The INADDR_ANY join uses the host's default
	// multicast interface; additionally join on EACH real interface so a
	// multi-homed host (physical NIC + virtual adapters) receives on the LAN NIC
	// too, not just whichever the OS picked as default.
	const uint32_t group = inet_addr(DiscoveryGroup());
	ip_mreq mreq{};
	mreq.imr_multiaddr.s_addr = group;
	mreq.imr_interface.s_addr = htonl(INADDR_ANY);
	setsockopt(discoSock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&mreq, sizeof(mreq));
	for (const IfaceAddr& ia : LocalIPv4Interfaces()) {
		mcastIfaces.push_back(ia.addr);
		if (ia.bcast && ia.bcast != INADDR_BROADCAST) bcastAddrs.push_back(ia.bcast);
		ip_mreq m{};
		m.imr_multiaddr.s_addr = group;
		m.imr_interface.s_addr = ia.addr;
		setsockopt(discoSock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&m, sizeof(m));  // EADDRINUSE if dup: harmless
	}
	EMU_INFO("LAN backend: %d local interface(s) for discovery", (int)mcastIfaces.size());
	// Receive our own multicast (needed for multiple instances on one host).
	unsigned char loop = 1;
	setsockopt(discoSock, IPPROTO_IP, IP_MULTICAST_LOOP, (const char*)&loop, sizeof(loop));
	// TTL 1 stays on the local subnet, which is all LAN play needs; bump slightly
	// so a beacon still crosses a single small switch/bridge hop if present.
	unsigned char ttl = 4;
	setsockopt(discoSock, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&ttl, sizeof(ttl));
	// Also allow directed/limited broadcast: some LANs pass broadcast more
	// reliably than multicast, so DiscoverySend fans out over both.
	setsockopt(discoSock, SOL_SOCKET, SO_BROADCAST, (const char*)&yes, sizeof(yes));
	SetNonBlocking(discoSock);

	// Data socket: ephemeral unicast port, announced in our beacon.
	dataSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (dataSock == INVALID_SOCKET) return false;
	sockaddr_in daddr{};
	daddr.sin_family = AF_INET;
	daddr.sin_addr.s_addr = htonl(INADDR_ANY);
	// 0 => ephemeral (the default). A fixed listen_port is honored for setups
	// that need a predictable data port (one instance per host).
	daddr.sin_port = htons(Config().ListenPort());
	if (bind(dataSock, (sockaddr*)&daddr, sizeof(daddr)) != 0) return false;
	socklen_t slen = sizeof(daddr);
	getsockname(dataSock, (sockaddr*)&daddr, &slen);
	dataPort = ntohs(daddr.sin_port);
	SetNonBlocking(dataSock);
	return true;
}

void NetBackend::Impl::DiscoverySend(const void* data, int len) {
	sockaddr_in to{};
	to.sin_family = AF_INET;
	to.sin_port = htons(DiscoveryPort());
	std::lock_guard<std::mutex> lk(discoSendMtx);
	if (mcastIfaces.empty()) {
		// No interfaces enumerated: use the OS default (previous behavior).
		to.sin_addr.s_addr = inet_addr(DiscoveryGroup());
		sendto(discoSock, (const char*)data, len, 0, (sockaddr*)&to, sizeof(to));
		return;
	}
	// (1) Multicast out every interface. Set IP_MULTICAST_IF per send so the
	// packet egresses the LAN NIC(s), not just the OS default. A same-host second
	// instance still receives via IP_MULTICAST_LOOP.
	const uint32_t group = inet_addr(DiscoveryGroup());
	for (uint32_t ifAddr : mcastIfaces) {
		in_addr ia{};
		ia.s_addr = ifAddr;
		setsockopt(discoSock, IPPROTO_IP, IP_MULTICAST_IF, (const char*)&ia, sizeof(ia));
		to.sin_addr.s_addr = group;
		sendto(discoSock, (const char*)data, len, 0, (sockaddr*)&to, sizeof(to));
	}
	// (2) Directed broadcast to each subnet (routed out the owning interface),
	// plus the limited broadcast, for LANs that pass broadcast but filter
	// multicast. Duplicate arrivals are harmless: beacons/lobby gossip are
	// idempotent and drop self-echoes; chat is de-duplicated by (sender, seq).
	for (uint32_t bcast : bcastAddrs) {
		to.sin_addr.s_addr = bcast;
		sendto(discoSock, (const char*)data, len, 0, (sockaddr*)&to, sizeof(to));
	}
	to.sin_addr.s_addr = INADDR_BROADCAST;
	sendto(discoSock, (const char*)data, len, 0, (sockaddr*)&to, sizeof(to));
}

void NetBackend::Impl::SendBeacon() {
	Beacon b{};
	b.magic = kBeaconMagic;
	b.steamID = LocalSteamID().ConvertToUint64();
	b.appID = AppID();
	b.dataPort = dataPort;
	std::snprintf(b.name, sizeof(b.name), "%s", PersonaName());
	DiscoverySend(&b, sizeof(b));
}

void NetBackend::Impl::HandleBeacon(const Beacon& b, const sockaddr_in& from) {
	if (b.magic != kBeaconMagic) return;
	if (b.appID != AppID()) return;                       // only same-game peers
	if (b.steamID == LocalSteamID().ConvertToUint64()) return;  // ignore ourselves
	std::lock_guard<std::mutex> lk(mtx);
	Peer& p = peers[b.steamID];
	p.id = CSteamID(static_cast<uint64>(b.steamID));
	p.name.assign(b.name, strnlen(b.name, sizeof(b.name)));
	p.dataAddr = from;
	p.dataAddr.sin_port = htons(b.dataPort);
	p.lastSeen = NowMs();
}

void NetBackend::Impl::HandleData(const uint8_t* buf, int len, const sockaddr_in& from) {
	if (len < (int)sizeof(DataHdr)) return;
	DataHdr hdr;
	std::memcpy(&hdr, buf, sizeof(hdr));
	if (hdr.magic != kDataMagic) return;
	Packet pkt;
	pkt.sender = CSteamID(static_cast<uint64>(hdr.senderID));
	pkt.data.assign(buf + sizeof(hdr), buf + len);
	// Traffic addressed to our game-server identity goes to the server-side
	// inbox; everything else (including destID 0) to the client's.
	const int side = CSteamID(static_cast<uint64>(hdr.destID)).BGameServerAccount() ? 1 : 0;
	bool newSession = false;
	{
		std::lock_guard<std::mutex> lk(mtx);
		// Learn the peer's address from the datagram itself, so we can reply even
		// before (or without) receiving their multicast beacon.
		Peer& p = peers[hdr.senderID];
		p.id = pkt.sender;
		p.dataAddr = from;
		p.lastSeen = NowMs();
		inbox[side][hdr.channel].push_back(std::move(pkt));
		if (!knownSessions[side][hdr.senderID]) {
			knownSessions[side][hdr.senderID] = true;
			newSession = true;
		}
	}
	if (newSession) {
		// First contact from this peer -> deliver P2PSessionRequest_t so the game
		// can AcceptP2PSessionWithUser (we accept implicitly by queueing anyway).
		// Server-side sessions announce on the game-server pipe.
		P2PSessionRequest_t req{};
		req.m_steamIDRemote = CSteamID(static_cast<uint64>(hdr.senderID));
		if (side)
			QueueGameServerCallback(req);
		else
			QueueCallback(req);
	}
}

void NetBackend::Impl::SendLobbyPresence() {
	// Announce presence in each lobby we've joined. Snapshot under lock, send
	// outside it.
	struct Out { LobbyMsg hdr; std::vector<uint8_t> blob; };
	std::vector<Out> msgs;
	uint64_t meID = LocalSteamID().ConvertToUint64();
	{
		std::lock_guard<std::mutex> lk(mtx);
		for (auto& kv : lobbies) {
			Lobby& lb = kv.second;
			if (!lb.joined) continue;
			bool owner = lb.owner.ConvertToUint64() == meID;
			Out o;
			o.hdr = LobbyMsg{};
			o.hdr.magic = kLobbyMagic;
			o.hdr.appID = AppID();
			o.hdr.lobbyID = lb.id.ConvertToUint64();
			o.hdr.ownerID = lb.owner.ConvertToUint64();
			o.hdr.senderID = meID;
			o.hdr.maxMembers = lb.maxMembers;
			o.hdr.lobbyType = lb.type;
			o.hdr.joinable = lb.joinable ? 1 : 0;
			o.hdr.isOwner = owner ? 1 : 0;
			if (owner) {
				o.hdr.gsIP = lb.gsIP;
				o.hdr.gsPort = lb.gsPort;
				o.hdr.gsID = lb.gsID;
				o.hdr.gsUpdate = lb.gsUpdate;
			}
			if (owner) PackKV(lb.data, o.blob);
			o.hdr.dataLen = (uint32_t)o.blob.size();
			// Append our own per-member data blob after the owner blob.
			auto md = lb.memberData.find(meID);
			if (md != lb.memberData.end()) PackKV(md->second, o.blob);
			o.hdr.memberDataLen = (uint32_t)o.blob.size() - o.hdr.dataLen;
			msgs.push_back(std::move(o));
		}
	}
	for (auto& o : msgs) {
		std::vector<uint8_t> buf(sizeof(LobbyMsg) + o.blob.size());
		std::memcpy(buf.data(), &o.hdr, sizeof(LobbyMsg));
		if (!o.blob.empty()) std::memcpy(buf.data() + sizeof(LobbyMsg), o.blob.data(), o.blob.size());
		DiscoverySend(buf.data(), (int)buf.size());
	}
}

void NetBackend::Impl::HandleLobbyMsg(const uint8_t* buf, int len) {
	if (len < (int)sizeof(LobbyMsg)) return;
	LobbyMsg m;
	std::memcpy(&m, buf, sizeof(m));
	if (m.magic != kLobbyMagic || m.appID != AppID()) return;
	uint64_t meID = LocalSteamID().ConvertToUint64();
	if (m.senderID == meID) return;  // our own multicast echo

	bool memberJoined = false, dataChanged = false, memberDataChanged = false;
	bool gameServerChanged = false;
	bool weAreIn = false, watched = false;
	{
		std::lock_guard<std::mutex> lk(mtx);
		Lobby& lb = lobbies[m.lobbyID];
		if (!lb.id.IsValid()) lb.id = CSteamID(static_cast<uint64>(m.lobbyID));
		lb.owner = CSteamID(static_cast<uint64>(m.ownerID));
		lb.lastHeard = NowMs();
		int off = (int)sizeof(LobbyMsg);
		if (m.isOwner) {
			lb.maxMembers = m.maxMembers;
			lb.type = m.lobbyType;
			lb.joinable = m.joinable != 0;
			// A gsUpdate we haven't seen yet means the owner (re)registered the
			// game server -- including the case where we joined after it was set.
			if (m.gsUpdate != lb.gsUpdate) {
				lb.gsIP = m.gsIP;
				lb.gsPort = m.gsPort;
				lb.gsID = m.gsID;
				lb.gsUpdate = m.gsUpdate;
				gameServerChanged = m.gsUpdate != 0;
			}
			std::map<std::string, std::string> incoming;
			if (m.dataLen && len >= off + (int)m.dataLen)
				UnpackKV(buf + off, (int)m.dataLen, incoming);
			if (incoming != lb.data) { lb.data.swap(incoming); dataChanged = true; }
		}
		off += (int)m.dataLen;
		// Sender's own per-member data blob.
		if (m.memberDataLen && len >= off + (int)m.memberDataLen) {
			std::map<std::string, std::string> incoming;
			UnpackKV(buf + off, (int)m.memberDataLen, incoming);
			auto& stored = lb.memberData[m.senderID];
			if (incoming != stored) { stored.swap(incoming); memberDataChanged = true; }
		}
		if (lb.members.find(m.senderID) == lb.members.end()) memberJoined = true;
		lb.members[m.senderID] = NowMs();
		weAreIn = lb.joined;
		watched = lb.watched;
	}
	// Notify the game about member churn only for lobbies we're in; data updates
	// also fire for lobbies we've asked to watch via RequestLobbyData.
	if (weAreIn && memberJoined) {
		LobbyChatUpdate_t u{};
		u.m_ulSteamIDLobby = m.lobbyID;
		u.m_ulSteamIDUserChanged = m.senderID;
		u.m_ulSteamIDMakingChange = m.senderID;
		u.m_rgfChatMemberStateChange = k_EChatMemberStateChangeEntered;
		QueueCallback(u);
	}
	if ((weAreIn || watched) && dataChanged) {
		LobbyDataUpdate_t u{};
		u.m_ulSteamIDLobby = m.lobbyID;
		u.m_ulSteamIDMember = m.lobbyID;   // lobby-level data
		u.m_bSuccess = 1;
		QueueCallback(u);
	}
	if (weAreIn && memberDataChanged) {
		LobbyDataUpdate_t u{};
		u.m_ulSteamIDLobby = m.lobbyID;
		u.m_ulSteamIDMember = m.senderID;  // member-level data
		u.m_bSuccess = 1;
		QueueCallback(u);
	}
	if (weAreIn && gameServerChanged) {
		// The owner registered the game server (SetLobbyGameServer). This is the
		// "host started the game" signal lobby members wait on before connecting.
		LobbyGameCreated_t g{};
		g.m_ulSteamIDLobby = m.lobbyID;
		g.m_ulSteamIDGameServer = m.gsID;
		g.m_unIP = m.gsIP;
		g.m_usPort = m.gsPort;
		EMU_INFO("lobby %llu: game server registered by owner (id %llu, %u:%u) -> LobbyGameCreated_t",
		         (unsigned long long)m.lobbyID, (unsigned long long)m.gsID, m.gsIP, (unsigned)m.gsPort);
		QueueCallback(g);
	}
}

void NetBackend::Impl::HandleChatMsg(const uint8_t* buf, int len) {
	if (len < (int)sizeof(ChatMsg)) return;
	ChatMsg m;
	std::memcpy(&m, buf, sizeof(m));
	if (m.magic != kChatMagic || m.appID != AppID()) return;
	if (len < (int)(sizeof(ChatMsg) + m.bodyLen)) return;
	uint32_t chatID = 0;
	bool weAreIn = false;
	{
		std::lock_guard<std::mutex> lk(mtx);
		auto it = lobbies.find(m.lobbyID);
		if (it == lobbies.end() || !it->second.joined) return;  // not our lobby
		// Drop the extra fan-out / loopback copies of this chat line (see
		// ChatMsg::seq); only the first (senderID, seq) is appended and reported.
		if (ChatDuplicate(m.senderID, m.seq)) {
			EMU_DEBUG("net: dropped duplicate chat copy (sender %llu seq %llu)",
			          (unsigned long long)m.senderID, (unsigned long long)m.seq);
			return;
		}
		weAreIn = true;
		ChatEntry e;
		e.sender = m.senderID;
		e.chatType = m.chatType;
		e.data.assign(buf + sizeof(ChatMsg), buf + sizeof(ChatMsg) + m.bodyLen);
		chatID = (uint32_t)it->second.chat.size();
		it->second.chat.push_back(std::move(e));
	}
	if (weAreIn) {
		LobbyChatMsg_t u{};
		u.m_ulSteamIDLobby = m.lobbyID;
		u.m_ulSteamIDUser = m.senderID;
		u.m_eChatEntryType = m.chatType;
		u.m_iChatID = chatID;
		QueueCallback(u);
	}
}

void NetBackend::Impl::FireConnStatus(uint32_t handle, CSteamID remote, int oldState,
                                      int newState, uint32_t listen) {
	SteamNetConnectionStatusChangedCallback_t cb{};
	cb.m_hConn = handle;
	cb.m_eOldState = (ESteamNetworkingConnectionState)oldState;
	cb.m_info.m_identityRemote.SetSteamID(remote);
	cb.m_info.m_eState = (ESteamNetworkingConnectionState)newState;
	cb.m_info.m_hListenSocket = listen;
	QueueCallback(cb);
}

void NetBackend::Impl::HandleConn(const uint8_t* buf, int len, const sockaddr_in& from) {
	if (len < (int)sizeof(ConnHdr)) return;
	ConnHdr h;
	std::memcpy(&h, buf, sizeof(h));
	uint64_t meID = LocalSteamID().ConvertToUint64();
	if (h.senderID == meID) return;

	if (h.magic == kConnReq) {
		// Inbound connection: only accept if we have a listen socket on the port.
		uint32_t newHandle = 0, listen = 0;
		CSteamID remote(static_cast<uint64>(h.senderID));
		{
			std::lock_guard<std::mutex> lk(mtx);
			for (auto& ls : listenSockets)
				if (ls.second == h.virtualPort || h.virtualPort < 0) { listen = ls.first; break; }
			if (!listen) return;  // nobody listening there
			newHandle = nextHandle++;
			Conn& c = conns[newHandle];
			c.handle = newHandle;
			c.remote = remote;
			c.remoteAddr = from;
			c.remoteHandle = h.srcHandle;
			c.state = k_ESteamNetworkingConnectionState_Connecting;
			c.virtualPort = h.virtualPort;
			c.listen = listen;
		}
		// Tell the game a connection is incoming; it calls AcceptConnection.
		FireConnStatus(newHandle, remote, k_ESteamNetworkingConnectionState_None,
		               k_ESteamNetworkingConnectionState_Connecting, listen);
	} else if (h.magic == kConnAcc) {
		CSteamID remote(static_cast<uint64>(h.senderID));
		bool ok = false;
		{
			std::lock_guard<std::mutex> lk(mtx);
			auto it = conns.find(h.dstHandle);
			if (it != conns.end()) {
				it->second.remoteHandle = h.srcHandle;
				it->second.state = k_ESteamNetworkingConnectionState_Connected;
				ok = true;
			}
		}
		if (ok)
			FireConnStatus(h.dstHandle, remote, k_ESteamNetworkingConnectionState_Connecting,
			               k_ESteamNetworkingConnectionState_Connected, 0);
	} else if (h.magic == kConnMsg) {
		std::lock_guard<std::mutex> lk(mtx);
		auto it = conns.find(h.dstHandle);
		if (it != conns.end())
			it->second.inbox.emplace_back(buf + sizeof(ConnHdr), buf + len);
	} else if (h.magic == kConnFin) {
		CSteamID remote(static_cast<uint64>(h.senderID));
		bool ok = false;
		{
			std::lock_guard<std::mutex> lk(mtx);
			auto it = conns.find(h.dstHandle);
			if (it != conns.end()) { it->second.state = k_ESteamNetworkingConnectionState_ClosedByPeer; ok = true; }
		}
		if (ok)
			FireConnStatus(h.dstHandle, remote, k_ESteamNetworkingConnectionState_Connected,
			               k_ESteamNetworkingConnectionState_ClosedByPeer, 0);
	}
}

void NetBackend::Impl::Pump() {
	while (running.load()) {
		if (NowMs() - lastBeacon >= kBeaconIntervalMs) {
			SendBeacon();
			SendLobbyPresence();
			lastBeacon = NowMs();
		}
		fd_set rf;
		FD_ZERO(&rf);
		FD_SET(discoSock, &rf);
		FD_SET(dataSock, &rf);
		SOCKET maxfd = discoSock > dataSock ? discoSock : dataSock;
		timeval tv{0, 200 * 1000};  // 200ms
		int n = select((int)maxfd + 1, &rf, nullptr, nullptr, &tv);
		if (n < 0) {
			// select() errored -- typically the sockets were torn down under us
			// during shutdown. Never busy-spin; nap and re-check running.
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			continue;
		}
		if (n > 0) {
			uint8_t buf[2048];
			if (FD_ISSET(discoSock, &rf)) {
				sockaddr_in from{}; socklen_t fl = sizeof(from);
				int r = recvfrom(discoSock, (char*)buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
				if (r >= 4) {
					uint32_t magic; std::memcpy(&magic, buf, 4);
					if (magic == kBeaconMagic && r >= (int)sizeof(Beacon)) {
						Beacon b; std::memcpy(&b, buf, sizeof(b));
						HandleBeacon(b, from);
					} else if (magic == kLobbyMagic) {
						HandleLobbyMsg(buf, r);
					} else if (magic == kChatMagic) {
						HandleChatMsg(buf, r);
					}
				}
			}
			if (FD_ISSET(dataSock, &rf)) {
				sockaddr_in dfrom{}; socklen_t dfl = sizeof(dfrom);
				int r = recvfrom(dataSock, (char*)buf, sizeof(buf), 0, (sockaddr*)&dfrom, &dfl);
				if (r >= 4) {
					uint32_t magic; std::memcpy(&magic, buf, 4);
					if (magic == kDataMagic)
						HandleData(buf, r, dfrom);
					else if (magic == kConnReq || magic == kConnAcc || magic == kConnMsg || magic == kConnFin)
						HandleConn(buf, r, dfrom);
				}
			}
		}
		// Expire stale peers and lobby members.
		std::lock_guard<std::mutex> lk(mtx);
		int64_t now = NowMs();
		for (auto it = peers.begin(); it != peers.end();) {
			if (now - it->second.lastSeen > kPeerTimeoutMs) it = peers.erase(it);
			else ++it;
		}
		for (auto it = lobbies.begin(); it != lobbies.end();) {
			Lobby& lb = it->second;
			uint64_t meID = LocalSteamID().ConvertToUint64();
			for (auto m = lb.members.begin(); m != lb.members.end();) {
				if (m->first != meID && now - m->second > kLobbyMemberTimeoutMs) {
					lb.memberData.erase(m->first);
					m = lb.members.erase(m);
				} else ++m;
			}
			// Drop a lobby we no longer belong to once its owner goes silent.
			if (!lb.joined && now - lb.lastHeard > kLobbyMemberTimeoutMs)
				it = lobbies.erase(it);
			else ++it;
		}
	}
}

// --- NetBackend public API --------------------------------------------------
NetBackend& NetBackend::Get() {
	static NetBackend n;
	return n;
}

NetBackend::~NetBackend() { Stop(); }

void NetBackend::EnsureStarted() {
	// Restartable (not call_once): the game may Shutdown -> Stop then Init again,
	// which must be able to bring the backend back up.
	std::lock_guard<std::mutex> lk(m_lifecycleMtx);
	if (m_impl) return;
	auto impl = std::make_shared<Impl>();
	if (!impl->Start()) {
		EMU_ERROR("LAN backend failed to start (sockets unavailable)");
		return;   // impl freed here; m_impl stays null so a later call retries
	}
	impl->running.store(true);
	// Detach the pump thread and hand it a shared_ptr, so it owns Impl for as
	// long as it runs. We never join it (see Impl), so process exit can't hang.
	std::thread([impl] { impl->Pump(); impl->pumpExited.store(true); }).detach();
	m_impl = impl;
	EMU_INFO("LAN backend started (discovery %s:%u)", DiscoveryGroup(), DiscoveryPort());
}

void NetBackend::Stop() {
	std::shared_ptr<Impl> impl;
	{
		std::lock_guard<std::mutex> lk(m_lifecycleMtx);
		impl.swap(m_impl);
	}
	if (!impl) return;
	impl->running.store(false);   // ask the detached pump to wind down
	// Wait a bounded time for it to actually stop (its select has a 200ms
	// timeout) so an explicit Shutdown is orderly -- but POLL, never join(): a
	// join() from a DLL static destructor deadlocks on the Windows loader lock.
	for (int i = 0; i < 40 && !impl->pumpExited.load(); ++i)
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	// Our reference drops here. If the pump is somehow still alive it holds the
	// last reference and frees Impl (closing its sockets) when it finally exits.
}

bool NetBackend::SendP2PPacket(CSteamID target, const void* data, uint32_t cb, int channel, bool gs) {
	EnsureStarted();
	if (!m_impl) return false;
	sockaddr_in to{};
	{
		std::lock_guard<std::mutex> lk(m_impl->mtx);
		Impl::Peer* p = m_impl->FindPeer(target.ConvertToUint64());
		if (!p) return false;  // peer not (yet) discovered
		to = p->dataAddr;
	}
	std::vector<uint8_t> buf(sizeof(DataHdr) + cb);
	DataHdr hdr{};
	hdr.magic = kDataMagic;
	hdr.senderID = (gs ? LocalGSSteamID() : LocalSteamID()).ConvertToUint64();
	hdr.destID = target.ConvertToUint64();
	hdr.channel = channel;
	std::memcpy(buf.data(), &hdr, sizeof(hdr));
	if (cb) std::memcpy(buf.data() + sizeof(hdr), data, cb);
	int r = sendto(m_impl->dataSock, (const char*)buf.data(), (int)buf.size(), 0,
	               (sockaddr*)&to, sizeof(to));
	return r == (int)buf.size();
}

bool NetBackend::IsP2PPacketAvailable(uint32_t* pcubMsgSize, int channel, bool gs) {
	EnsureStarted();
	if (!m_impl) return false;
	std::lock_guard<std::mutex> lk(m_impl->mtx);
	auto& box = m_impl->inbox[gs ? 1 : 0];
	auto it = box.find(channel);
	if (it == box.end() || it->second.empty()) return false;
	if (pcubMsgSize) *pcubMsgSize = (uint32_t)it->second.front().data.size();
	return true;
}

bool NetBackend::ReadP2PPacket(void* dest, uint32_t cbDest, uint32_t* pcbRead,
                               CSteamID* sender, int channel, bool gs) {
	EnsureStarted();
	if (!m_impl) return false;
	std::lock_guard<std::mutex> lk(m_impl->mtx);
	auto& box = m_impl->inbox[gs ? 1 : 0];
	auto it = box.find(channel);
	if (it == box.end() || it->second.empty()) return false;
	Impl::Packet& pkt = it->second.front();
	uint32_t n = (uint32_t)pkt.data.size();
	if (n > cbDest) n = cbDest;
	if (dest && n) std::memcpy(dest, pkt.data.data(), n);
	if (pcbRead) *pcbRead = n;
	if (sender) *sender = pkt.sender;
	it->second.pop_front();
	return true;
}

bool NetBackend::CloseP2PChannel(CSteamID remote, int channel, bool gs) {
	if (!m_impl) return true;
	std::lock_guard<std::mutex> lk(m_impl->mtx);
	m_impl->knownSessions[gs ? 1 : 0].erase(remote.ConvertToUint64());
	auto& box = m_impl->inbox[gs ? 1 : 0];
	auto it = box.find(channel);
	if (it != box.end()) {
		for (auto p = it->second.begin(); p != it->second.end();) {
			if (p->sender == remote) p = it->second.erase(p);
			else ++p;
		}
	}
	return true;
}

std::vector<CSteamID> NetBackend::Peers() {
	EnsureStarted();
	std::vector<CSteamID> out;
	if (!m_impl) return out;
	std::lock_guard<std::mutex> lk(m_impl->mtx);
	for (auto& kv : m_impl->peers) {
		// Peers() backs the friends list; a remote instance's game-server
		// identity (learned from its server-side datagrams) is not a "player".
		if (kv.second.id.BGameServerAccount()) continue;
		out.push_back(kv.second.id);
	}
	return out;
}

std::string NetBackend::PeerName(CSteamID id) {
	if (!m_impl) return "";
	std::lock_guard<std::mutex> lk(m_impl->mtx);
	auto it = m_impl->peers.find(id.ConvertToUint64());
	return it == m_impl->peers.end() ? std::string() : it->second.name;
}

const char* NetBackend::PeerNameC(CSteamID id) {
	// Cache in a map whose node storage is stable, so the returned pointer stays
	// valid even as the peer table churns.
	static std::mutex cacheMtx;
	static std::map<uint64_t, std::string> cache;
	std::string name = PeerName(id);
	std::lock_guard<std::mutex> lk(cacheMtx);
	std::string& slot = cache[id.ConvertToUint64()];
	if (!name.empty()) slot = name;
	return slot.c_str();
}

bool NetBackend::IsPeer(CSteamID id) {
	if (!m_impl) return false;
	std::lock_guard<std::mutex> lk(m_impl->mtx);
	return m_impl->peers.count(id.ConvertToUint64()) != 0;
}

// --- lobbies ----------------------------------------------------------------
CSteamID NetBackend::CreateLobby(int lobbyType, int maxMembers) {
	EnsureStarted();
	uint64_t me = LocalSteamID().ConvertToUint64();
	// Derive a lobby id: a Chat-type CSteamID unique to this owner+counter.
	static std::atomic<uint32_t> counter{1};
	// Unique per (owner, creation): the low bits of our SteamID plus a counter.
	uint32_t acct = (uint32_t)(me & 0xfffff) | (counter.fetch_add(1) << 20);
	CSteamID lobbyID(acct, k_EUniversePublic, k_EAccountTypeChat);
	if (!m_impl) return lobbyID;
	std::lock_guard<std::mutex> lk(m_impl->mtx);
	Impl::Lobby& lb = m_impl->lobbies[lobbyID.ConvertToUint64()];
	lb.id = lobbyID;
	lb.owner = LocalSteamID();
	lb.maxMembers = maxMembers > 0 ? maxMembers : 250;
	lb.type = lobbyType;
	lb.joinable = true;
	lb.joined = true;
	lb.members[me] = 0;  // never expire ourselves
	lb.lastHeard = 0;
	return lobbyID;
}

bool NetBackend::JoinLobby(CSteamID lobby) {
	EnsureStarted();
	if (!m_impl) return false;
	std::lock_guard<std::mutex> lk(m_impl->mtx);
	Impl::Lobby& lb = m_impl->lobbies[lobby.ConvertToUint64()];
	if (!lb.id.IsValid()) lb.id = lobby;
	lb.joined = true;
	lb.members[LocalSteamID().ConvertToUint64()] = 0;
	return true;
}

void NetBackend::LeaveLobby(CSteamID lobby) {
	if (!m_impl) return;
	std::lock_guard<std::mutex> lk(m_impl->mtx);
	auto it = m_impl->lobbies.find(lobby.ConvertToUint64());
	if (it == m_impl->lobbies.end()) return;
	it->second.joined = false;
	it->second.members.erase(LocalSteamID().ConvertToUint64());
}

std::vector<CSteamID> NetBackend::KnownLobbies() {
	EnsureStarted();
	std::vector<CSteamID> out;
	if (!m_impl) return out;
	std::lock_guard<std::mutex> lk(m_impl->mtx);
	for (auto& kv : m_impl->lobbies)
		if (kv.second.joinable && kv.second.id.IsValid())
			out.push_back(kv.second.id);
	return out;
}

std::vector<CSteamID> NetBackend::LobbyMembers(CSteamID lobby) {
	std::vector<CSteamID> out;
	if (!m_impl) return out;
	std::lock_guard<std::mutex> lk(m_impl->mtx);
	auto it = m_impl->lobbies.find(lobby.ConvertToUint64());
	if (it == m_impl->lobbies.end()) return out;
	for (auto& m : it->second.members)
		out.push_back(CSteamID(static_cast<uint64>(m.first)));
	return out;
}

CSteamID NetBackend::LobbyOwner(CSteamID lobby) {
	if (!m_impl) return CSteamID();
	std::lock_guard<std::mutex> lk(m_impl->mtx);
	auto it = m_impl->lobbies.find(lobby.ConvertToUint64());
	return it == m_impl->lobbies.end() ? CSteamID() : it->second.owner;
}

int NetBackend::LobbyMaxMembers(CSteamID lobby) {
	if (!m_impl) return 0;
	std::lock_guard<std::mutex> lk(m_impl->mtx);
	auto it = m_impl->lobbies.find(lobby.ConvertToUint64());
	return it == m_impl->lobbies.end() ? 0 : it->second.maxMembers;
}

bool NetBackend::SetLobbyData(CSteamID lobby, const char* key, const char* value) {
	if (!m_impl || !key) return false;
	std::lock_guard<std::mutex> lk(m_impl->mtx);
	auto it = m_impl->lobbies.find(lobby.ConvertToUint64());
	if (it == m_impl->lobbies.end()) {
		// The data is dropped if the lobby isn't ours/known -- log it so a lobby-id
		// or timing mismatch (data set before the lobby is registered) is visible.
		EMU_INFO("SetLobbyData DROPPED (unknown lobby %llu): '%s'='%s'",
		         (unsigned long long)lobby.ConvertToUint64(), key, value ? value : "");
		return false;
	}
	EMU_INFO("SetLobbyData lobby %llu: '%s'='%s'",
	         (unsigned long long)lobby.ConvertToUint64(), key, value ? value : "");
	// Only the owner's data is authoritative, but we let any member set locally.
	it->second.data[key] = value ? value : "";
	return true;
}

const char* NetBackend::GetLobbyDataC(CSteamID lobby, const char* key) {
	static std::mutex cacheMtx;
	static std::map<std::string, std::string> cache;
	std::string result;
	if (m_impl && key) {
		std::lock_guard<std::mutex> lk(m_impl->mtx);
		auto it = m_impl->lobbies.find(lobby.ConvertToUint64());
		if (it != m_impl->lobbies.end()) {
			auto d = it->second.data.find(key);
			if (d != it->second.data.end()) result = d->second;
		}
	}
	// Return a stable pointer keyed by lobby+key.
	char k[128];
	std::snprintf(k, sizeof(k), "%llu/%s", (unsigned long long)lobby.ConvertToUint64(), key ? key : "");
	std::lock_guard<std::mutex> lk(cacheMtx);
	std::string& slot = cache[k];
	slot = result;
	return slot.c_str();
}

int NetBackend::LobbyDataCount(CSteamID lobby) {
	if (!m_impl) return 0;
	std::lock_guard<std::mutex> lk(m_impl->mtx);
	auto it = m_impl->lobbies.find(lobby.ConvertToUint64());
	return it == m_impl->lobbies.end() ? 0 : (int)it->second.data.size();
}

bool NetBackend::LobbyDataByIndex(CSteamID lobby, int i, char* keyOut, int keyLen, char* valOut, int valLen) {
	if (!m_impl) return false;
	std::lock_guard<std::mutex> lk(m_impl->mtx);
	auto it = m_impl->lobbies.find(lobby.ConvertToUint64());
	if (it == m_impl->lobbies.end() || i < 0 || i >= (int)it->second.data.size()) return false;
	auto d = it->second.data.begin();
	std::advance(d, i);
	if (keyOut && keyLen > 0) std::snprintf(keyOut, keyLen, "%s", d->first.c_str());
	if (valOut && valLen > 0) std::snprintf(valOut, valLen, "%s", d->second.c_str());
	return true;
}

bool NetBackend::SetLobbyJoinable(CSteamID lobby, bool joinable) {
	if (!m_impl) return false;
	std::lock_guard<std::mutex> lk(m_impl->mtx);
	auto it = m_impl->lobbies.find(lobby.ConvertToUint64());
	if (it == m_impl->lobbies.end()) return false;
	it->second.joinable = joinable;
	return true;
}

void NetBackend::SetLobbyGameServer(CSteamID lobby, uint32_t ip, uint16_t port, CSteamID server) {
	if (!m_impl) return;
	{
		std::lock_guard<std::mutex> lk(m_impl->mtx);
		auto it = m_impl->lobbies.find(lobby.ConvertToUint64());
		if (it == m_impl->lobbies.end()) return;
		it->second.gsIP = ip;
		it->second.gsPort = port;
		it->second.gsID = server.ConvertToUint64();
		it->second.gsUpdate++;  // members detect the bump in our gossip
	}
	EMU_INFO("SetLobbyGameServer lobby %llu: server id %llu, %u:%u",
	         (unsigned long long)lobby.ConvertToUint64(),
	         (unsigned long long)server.ConvertToUint64(), ip, (unsigned)port);
	// Real Steam also deliver LobbyGameCreated_t + a LobbyDataUpdate_t
	// to the caller itself.
	LobbyGameCreated_t g{};
	g.m_ulSteamIDLobby = lobby.ConvertToUint64();
	g.m_ulSteamIDGameServer = server.ConvertToUint64();
	g.m_unIP = ip;
	g.m_usPort = port;
	QueueCallback(g);
	LobbyDataUpdate_t u{};
	u.m_ulSteamIDLobby = lobby.ConvertToUint64();
	u.m_ulSteamIDMember = lobby.ConvertToUint64();
	u.m_bSuccess = 1;
	QueueCallback(u);
}

bool NetBackend::GetLobbyGameServer(CSteamID lobby, uint32* ip, uint16* port, CSteamID* server) {
	if (!m_impl) return false;
	std::lock_guard<std::mutex> lk(m_impl->mtx);
	auto it = m_impl->lobbies.find(lobby.ConvertToUint64());
	if (it == m_impl->lobbies.end()) return false;
	const Impl::Lobby& lb = it->second;
	if (!CSteamID(static_cast<uint64>(lb.gsID)).IsValid() && lb.gsPort == 0) return false;
	if (ip) *ip = lb.gsIP;
	if (port) *port = lb.gsPort;
	if (server) *server = CSteamID(static_cast<uint64>(lb.gsID));
	return true;
}

bool NetBackend::SetLobbyMemberData(CSteamID lobby, const char* key, const char* value) {
	if (!m_impl || !key) return false;
	std::lock_guard<std::mutex> lk(m_impl->mtx);
	auto it = m_impl->lobbies.find(lobby.ConvertToUint64());
	if (it == m_impl->lobbies.end()) return false;
	uint64_t me = LocalSteamID().ConvertToUint64();
	it->second.memberData[me][key] = value ? value : "";
	return true;
}

const char* NetBackend::GetLobbyMemberDataC(CSteamID lobby, CSteamID member, const char* key) {
	static std::mutex cacheMtx;
	static std::map<std::string, std::string> cache;
	std::string result;
	if (m_impl && key) {
		std::lock_guard<std::mutex> lk(m_impl->mtx);
		auto it = m_impl->lobbies.find(lobby.ConvertToUint64());
		if (it != m_impl->lobbies.end()) {
			auto md = it->second.memberData.find(member.ConvertToUint64());
			if (md != it->second.memberData.end()) {
				auto d = md->second.find(key);
				if (d != md->second.end()) result = d->second;
			}
		}
	}
	// Stable pointer keyed by lobby+member+key (borrowed by ISteamMatchmaking).
	char k[192];
	std::snprintf(k, sizeof(k), "%llu/%llu/%s",
	              (unsigned long long)lobby.ConvertToUint64(),
	              (unsigned long long)member.ConvertToUint64(), key ? key : "");
	std::lock_guard<std::mutex> lk(cacheMtx);
	std::string& slot = cache[k];
	slot = result;
	return slot.c_str();
}

bool NetBackend::SendLobbyChatMsg(CSteamID lobby, const void* body, int cb) {
	EnsureStarted();
	if (!m_impl || cb < 0) return false;
	{
		std::lock_guard<std::mutex> lk(m_impl->mtx);
		auto it = m_impl->lobbies.find(lobby.ConvertToUint64());
		if (it == m_impl->lobbies.end() || !it->second.joined) return false;  // members only
	}
	ChatMsg h{};
	h.magic = kChatMagic;
	h.appID = AppID();
	h.lobbyID = lobby.ConvertToUint64();
	h.senderID = LocalSteamID().ConvertToUint64();
	h.seq = ((uint64_t)ProcessEpoch() << 32) | (uint64_t)(++m_impl->chatCounter);
	h.chatType = (uint8_t)k_EChatEntryTypeChatMsg;
	h.bodyLen = (uint32_t)cb;
	std::vector<uint8_t> buf(sizeof(ChatMsg) + (size_t)cb);
	std::memcpy(buf.data(), &h, sizeof(ChatMsg));
	if (cb) std::memcpy(buf.data() + sizeof(ChatMsg), body, (size_t)cb);
	// Multicast out every interface; our own loopback copy delivers the message
	// to us too, so the local user gets its LobbyChatMsg_t like every other member.
	m_impl->DiscoverySend(buf.data(), (int)buf.size());
	return true;
}

int NetBackend::GetLobbyChatEntry(CSteamID lobby, int iChatID, CSteamID* user,
                                  void* data, int cbData, int* chatType) {
	if (!m_impl) return 0;
	std::lock_guard<std::mutex> lk(m_impl->mtx);
	auto it = m_impl->lobbies.find(lobby.ConvertToUint64());
	if (it == m_impl->lobbies.end() || iChatID < 0 || iChatID >= (int)it->second.chat.size())
		return 0;
	const Impl::ChatEntry& e = it->second.chat[iChatID];
	if (user) *user = CSteamID(static_cast<uint64>(e.sender));
	if (chatType) *chatType = e.chatType;
	int n = (int)e.data.size();
	int copy = n < cbData ? n : cbData;
	if (data && copy > 0) std::memcpy(data, e.data.data(), (size_t)copy);
	return n;
}

bool NetBackend::RequestLobbyData(CSteamID lobby) {
	EnsureStarted();
	if (!m_impl) return false;
	bool known = false;
	{
		std::lock_guard<std::mutex> lk(m_impl->mtx);
		auto it = m_impl->lobbies.find(lobby.ConvertToUint64());
		if (it != m_impl->lobbies.end()) { it->second.watched = true; known = true; }
	}
	// Deliver a LobbyDataUpdate_t now with whatever we already gossiped; future
	// owner updates for a watched lobby keep firing it (see HandleLobbyMsg).
	LobbyDataUpdate_t u{};
	u.m_ulSteamIDLobby = lobby.ConvertToUint64();
	u.m_ulSteamIDMember = lobby.ConvertToUint64();
	u.m_bSuccess = known ? 1 : 0;
	QueueCallback(u);
	return true;
}

void NetBackend::AddLobbyListStringFilter(const char* key, const char* value, int cmp) {
	EnsureStarted();
	if (!m_impl || !key) return;
	std::lock_guard<std::mutex> lk(m_impl->mtx);
	EMU_INFO("lobby filter (string): '%s' cmp=%d '%s'", key, cmp, value ? value : "");
	Impl::LobbyFilter f;
	f.key = key; f.numeric = false; f.sval = value ? value : ""; f.cmp = cmp;
	m_impl->lobbyFilters.push_back(std::move(f));
}
void NetBackend::AddLobbyListNumFilter(const char* key, int value, int cmp) {
	EnsureStarted();
	if (!m_impl || !key) return;
	std::lock_guard<std::mutex> lk(m_impl->mtx);
	EMU_INFO("lobby filter (num): '%s' cmp=%d %d", key, cmp, value);
	Impl::LobbyFilter f;
	f.key = key; f.numeric = true; f.nval = value; f.cmp = cmp;
	m_impl->lobbyFilters.push_back(std::move(f));
}
void NetBackend::AddLobbyListSlotsFilter(int slotsAvailable) {
	EnsureStarted();
	if (!m_impl) return;
	std::lock_guard<std::mutex> lk(m_impl->mtx);
	m_impl->filterSlots = slotsAvailable;
}
void NetBackend::SetLobbyListResultCount(int maxResults) {
	EnsureStarted();
	if (!m_impl) return;
	std::lock_guard<std::mutex> lk(m_impl->mtx);
	m_impl->filterResultCount = maxResults;
}

namespace {
// Compare two ints under an ELobbyComparison. For string filters we feed the
// sign of a strcmp as `a` and 0 as `b`, so ordering comparisons work too.
bool CmpLobby(int a, int cmp, int b) {
	switch (cmp) {
		case -2: return a <= b;   // EqualToOrLessThan
		case -1: return a < b;    // LessThan
		case  0: return a == b;   // Equal
		case  1: return a > b;    // GreaterThan
		case  2: return a >= b;   // EqualToOrGreaterThan
		case  3: return a != b;   // NotEqual
	}
	return true;
}
} // namespace

int NetBackend::RequestLobbyList() {
	EnsureStarted();
	if (!m_impl) return 0;
	std::lock_guard<std::mutex> lk(m_impl->mtx);
	std::vector<CSteamID> out;
	for (auto& kv : m_impl->lobbies) {
		Impl::Lobby& lb = kv.second;
		if (!lb.joinable || !lb.id.IsValid()) continue;
		bool match = true;
		for (auto& f : m_impl->lobbyFilters) {
			auto d = lb.data.find(f.key);
			bool has = d != lb.data.end();
			if (f.numeric) {
				if (!has || !CmpLobby(std::atoi(d->second.c_str()), f.cmp, f.nval)) { match = false; break; }
			} else if (f.cmp == 3) {           // NotEqual: absent or differing passes
				if (has && d->second == f.sval) { match = false; break; }
			} else if (f.cmp == 0) {           // Equal
				if (!has || d->second != f.sval) { match = false; break; }
			} else {                           // ordering: sign of strcmp
				if (!has || !CmpLobby(d->second.compare(f.sval) < 0 ? -1 : (d->second.compare(f.sval) > 0 ? 1 : 0), f.cmp, 0)) { match = false; break; }
			}
		}
		if (match && m_impl->filterSlots >= 0) {
			int open = lb.maxMembers - (int)lb.members.size();
			if (open < m_impl->filterSlots) match = false;
		}
		if (match) out.push_back(lb.id);
	}
	if (m_impl->filterResultCount >= 0 && (int)out.size() > m_impl->filterResultCount)
		out.resize(m_impl->filterResultCount);
	m_impl->lastLobbyList = out;
	m_impl->lobbyListRequested = true;
	// Filters are one-shot per Steam semantics: clear after applying.
	m_impl->lobbyFilters.clear();
	m_impl->filterSlots = -1;
	m_impl->filterResultCount = -1;
	// Diagnostic: distinguish "no lobby discovered on the LAN yet" from "discovered
	// but filtered out" -- the two failure modes for an empty server browser. On a
	// mismatch, dump each known lobby's data so the offending filter key is obvious.
	EMU_INFO("RequestLobbyList: %zu lobbies known, %zu match filters",
	         m_impl->lobbies.size(), out.size());
	if (out.empty() && !m_impl->lobbies.empty()) {
		for (auto& kv : m_impl->lobbies) {
			Impl::Lobby& lb = kv.second;
			EMU_INFO("  known lobby %llu joinable=%d members=%zu data:",
			         (unsigned long long)lb.id.ConvertToUint64(), lb.joinable, lb.members.size());
			for (auto& d : lb.data)
				EMU_INFO("    '%s' = '%s'", d.first.c_str(), d.second.c_str());
		}
	}
	return (int)out.size();
}

CSteamID NetBackend::LobbyListByIndex(int i) {
	if (!m_impl || i < 0) return CSteamID();
	std::lock_guard<std::mutex> lk(m_impl->mtx);
	// Until a RequestLobbyList has run, fall back to a live joinable scan so
	// callers that never request a list (e.g. an owner reading back its own
	// just-created lobby) still work; afterwards, index the cached snapshot.
	if (!m_impl->lobbyListRequested) {
		int idx = 0;
		for (auto& kv : m_impl->lobbies) {
			if (kv.second.joinable && kv.second.id.IsValid()) {
				if (idx == i) return kv.second.id;
				++idx;
			}
		}
		return CSteamID();
	}
	if (i >= (int)m_impl->lastLobbyList.size()) return CSteamID();
	return m_impl->lastLobbyList[i];
}

// --- modern ISteamNetworkingSockets -----------------------------------------
namespace {
void FreeNetMsg(SteamNetworkingMessage_t* m) {
	if (m->m_pData) std::free(m->m_pData);
	std::free(m);
}
SteamNetworkingMessage_t* AllocNetMsg(const std::vector<uint8_t>& data, uint32_t conn, CSteamID from) {
	auto* m = (SteamNetworkingMessage_t*)std::calloc(1, sizeof(SteamNetworkingMessage_t));
	m->m_pData = std::malloc(data.size() ? data.size() : 1);
	std::memcpy(m->m_pData, data.data(), data.size());
	m->m_cbSize = (int)data.size();
	m->m_conn = conn;
	m->m_identityPeer.SetSteamID(from);
	m->m_pfnFreeData = FreeNetMsg;
	m->m_pfnRelease = FreeNetMsg;  // Release() frees message + payload
	return m;
}
void SendConn(int sock, const sockaddr_in& to, uint32_t magic, uint32_t src, uint32_t dst,
              int32_t vport, const void* payload, uint32_t cb) {
	ConnHdr h{};
	h.magic = magic;
	h.senderID = LocalSteamID().ConvertToUint64();
	h.srcHandle = src;
	h.dstHandle = dst;
	h.virtualPort = vport;
	std::vector<uint8_t> buf(sizeof(h) + cb);
	std::memcpy(buf.data(), &h, sizeof(h));
	if (cb) std::memcpy(buf.data() + sizeof(h), payload, cb);
	sendto(sock, (const char*)buf.data(), (int)buf.size(), 0, (const sockaddr*)&to, sizeof(to));
}
} // namespace

int NetBackend::ReceiveMessagesOnChannel(int channel, SteamNetworkingMessage_t** out, int maxMsgs) {
	EnsureStarted();
	if (!m_impl || !out || maxMsgs <= 0) return 0;
	std::lock_guard<std::mutex> lk(m_impl->mtx);
	auto& box = m_impl->inbox[0];  // NetworkingMessages: client identity only
	auto it = box.find(channel);
	if (it == box.end()) return 0;
	int n = 0;
	while (n < maxMsgs && !it->second.empty()) {
		Impl::Packet& p = it->second.front();
		out[n] = AllocNetMsg(p.data, 0, p.sender);
		out[n]->m_nChannel = channel;
		++n;
		it->second.pop_front();
	}
	return n;
}

SteamNetworkingMessage_t* AllocNetworkingMessage(int cbBuffer) {
	if (cbBuffer < 0) cbBuffer = 0;
	std::vector<uint8_t> empty(cbBuffer, 0);
	return AllocNetMsg(empty, 0, CSteamID());
}

int64_t LocalTimestamp() {
	return (int64_t)std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

namespace {
struct NetConfigEntry {
	int dataType = 0;
	std::vector<uint8_t> bytes;
};
std::mutex g_netCfgMtx;
// (value, scopeType, scopeObj) -> stored bytes
std::map<std::tuple<int, int, intptr_t>, NetConfigEntry> g_netCfg;

int NetConfigSize(int dataType, const void* pArg) {
	switch (dataType) {
		case 1: return 4;                                    // Int32
		case 2: return 8;                                    // Int64
		case 3: return 4;                                    // Float
		case 4: return pArg ? (int)std::strlen((const char*)pArg) + 1 : 0;  // String
		case 5: return (int)sizeof(void*);                   // Ptr
		default: return -1;
	}
}
} // namespace

bool NetConfigSet(int eValue, int scopeType, intptr_t scopeObj, int dataType, const void* pArg) {
	int n = NetConfigSize(dataType, pArg);
	if (n < 0) return false;
	std::lock_guard<std::mutex> lk(g_netCfgMtx);
	auto key = std::make_tuple(eValue, scopeType, scopeObj);
	if (!pArg) { g_netCfg.erase(key); return true; }  // NULL = reset to default
	NetConfigEntry e;
	e.dataType = dataType;
	e.bytes.assign((const uint8_t*)pArg, (const uint8_t*)pArg + n);
	g_netCfg[key] = std::move(e);
	return true;
}

int NetConfigGet(int eValue, int scopeType, intptr_t scopeObj, int* outDataType,
                 void* pResult, size_t* cbResult) {
	std::lock_guard<std::mutex> lk(g_netCfgMtx);
	auto it = g_netCfg.find(std::make_tuple(eValue, scopeType, scopeObj));
	if (it == g_netCfg.end())
		return -1;  // k_ESteamNetworkingGetConfigValue_BadValue (nothing stored)
	if (outDataType) *outDataType = it->second.dataType;
	size_t need = it->second.bytes.size();
	size_t have = cbResult ? *cbResult : 0;
	if (cbResult) *cbResult = need;
	if (!pResult || have < need)
		return -3;  // k_ESteamNetworkingGetConfigValue_BufferTooSmall
	std::memcpy(pResult, it->second.bytes.data(), need);
	return 1;       // k_ESteamNetworkingGetConfigValue_OK
}

uint32_t LanIPv4() {
	static uint32_t ip = [] {
		// Route-lookup trick: connect() on a UDP socket sends nothing but makes
		// the kernel pick the outbound interface; getsockname reveals its address.
		uint32_t found = 0x7f000001;  // 127.0.0.1 fallback
		SOCKET s = ::socket(AF_INET, SOCK_DGRAM, 0);
		if (s != INVALID_SOCKET) {
			sockaddr_in to{};
			to.sin_family = AF_INET;
			to.sin_port = htons(53);
			to.sin_addr.s_addr = htonl(0x08080808);  // any routable address works
			if (::connect(s, (sockaddr*)&to, sizeof(to)) == 0) {
				sockaddr_in self{};
				socklen_t sl = sizeof(self);
				if (::getsockname(s, (sockaddr*)&self, &sl) == 0 && self.sin_addr.s_addr)
					found = ntohl(self.sin_addr.s_addr);
			}
			EMU_CLOSESOCKET(s);
		}
		return found;
	}();
	return ip;
}

uint32_t NetBackend::CreateListenSocketP2P(int virtualPort) {
	EnsureStarted();
	if (!m_impl) return 0;
	std::lock_guard<std::mutex> lk(m_impl->mtx);
	uint32_t h = m_impl->nextHandle++;
	m_impl->listenSockets[h] = virtualPort;
	return h;
}

bool NetBackend::CloseListenSocket(uint32_t sock) {
	if (!m_impl) return false;
	std::lock_guard<std::mutex> lk(m_impl->mtx);
	return m_impl->listenSockets.erase(sock) != 0;
}

uint32_t NetBackend::ConnectP2P(CSteamID remote, int virtualPort) {
	EnsureStarted();
	if (!m_impl) return 0;
	sockaddr_in addr{};
	bool have = false;
	uint32_t h;
	{
		std::lock_guard<std::mutex> lk(m_impl->mtx);
		if (Impl::Peer* p = m_impl->FindPeer(remote.ConvertToUint64())) { addr = p->dataAddr; have = true; }
		h = m_impl->nextHandle++;
		Impl::Conn& c = m_impl->conns[h];
		c.handle = h;
		c.remote = remote;
		c.state = k_ESteamNetworkingConnectionState_Connecting;
		c.virtualPort = virtualPort;
		if (have) c.remoteAddr = addr;
	}
	m_impl->FireConnStatus(h, remote, k_ESteamNetworkingConnectionState_None,
	                       k_ESteamNetworkingConnectionState_Connecting, 0);
	if (have)
		SendConn(m_impl->dataSock, addr, kConnReq, h, 0, virtualPort, nullptr, 0);
	return h;
}

int NetBackend::AcceptConnection(uint32_t conn) {
	if (!m_impl) return k_EResultNoConnection;
	CSteamID remote; sockaddr_in addr{}; uint32_t remoteHandle = 0, listen = 0;
	bool ok = false;
	{
		std::lock_guard<std::mutex> lk(m_impl->mtx);
		auto it = m_impl->conns.find(conn);
		if (it != m_impl->conns.end()) {
			it->second.state = k_ESteamNetworkingConnectionState_Connected;
			remote = it->second.remote;
			addr = it->second.remoteAddr;
			remoteHandle = it->second.remoteHandle;
			listen = it->second.listen;
			ok = true;
		}
	}
	if (!ok) return k_EResultInvalidParam;
	SendConn(m_impl->dataSock, addr, kConnAcc, conn, remoteHandle, 0, nullptr, 0);
	m_impl->FireConnStatus(conn, remote, k_ESteamNetworkingConnectionState_Connecting,
	                       k_ESteamNetworkingConnectionState_Connected, listen);
	return k_EResultOK;
}

bool NetBackend::CloseConnection(uint32_t conn) {
	if (!m_impl) return true;
	sockaddr_in addr{}; uint32_t remoteHandle = 0; bool have = false;
	{
		std::lock_guard<std::mutex> lk(m_impl->mtx);
		auto it = m_impl->conns.find(conn);
		if (it != m_impl->conns.end()) {
			addr = it->second.remoteAddr; remoteHandle = it->second.remoteHandle; have = true;
			m_impl->conns.erase(it);
		}
	}
	if (have) SendConn(m_impl->dataSock, addr, kConnFin, conn, remoteHandle, 0, nullptr, 0);
	return true;
}

int NetBackend::SendToConnection(uint32_t conn, const void* data, uint32_t cb) {
	if (!m_impl) return k_EResultNoConnection;
	sockaddr_in addr{}; uint32_t remoteHandle = 0; bool ok = false;
	{
		std::lock_guard<std::mutex> lk(m_impl->mtx);
		auto it = m_impl->conns.find(conn);
		if (it != m_impl->conns.end() && it->second.state == k_ESteamNetworkingConnectionState_Connected) {
			addr = it->second.remoteAddr; remoteHandle = it->second.remoteHandle; ok = true;
		}
	}
	if (!ok) return k_EResultNoConnection;
	SendConn(m_impl->dataSock, addr, kConnMsg, conn, remoteHandle, 0, data, cb);
	return k_EResultOK;
}

int NetBackend::ReceiveOnConnection(uint32_t conn, SteamNetworkingMessage_t** out, int maxMsgs) {
	if (!m_impl || !out || maxMsgs <= 0) return 0;
	std::lock_guard<std::mutex> lk(m_impl->mtx);
	auto it = m_impl->conns.find(conn);
	if (it == m_impl->conns.end()) return 0;
	int n = 0;
	while (n < maxMsgs && !it->second.inbox.empty()) {
		out[n++] = AllocNetMsg(it->second.inbox.front(), conn, it->second.remote);
		it->second.inbox.pop_front();
	}
	return n;
}

bool NetBackend::GetConnectionInfo(uint32_t conn, CSteamID* remoteOut, int* stateOut, uint32_t* listenOut) {
	if (!m_impl) return false;
	std::lock_guard<std::mutex> lk(m_impl->mtx);
	auto it = m_impl->conns.find(conn);
	if (it == m_impl->conns.end()) return false;
	if (remoteOut) *remoteOut = it->second.remote;
	if (stateOut) *stateOut = it->second.state;
	if (listenOut) *listenOut = it->second.listen;
	return true;
}

CSteamID NetBackend::ConnectionRemote(uint32_t conn) {
	if (!m_impl) return CSteamID();
	std::lock_guard<std::mutex> lk(m_impl->mtx);
	auto it = m_impl->conns.find(conn);
	return it == m_impl->conns.end() ? CSteamID() : it->second.remote;
}

uint32_t NetBackend::CreatePollGroup() {
	EnsureStarted();
	if (!m_impl) return 0;
	std::lock_guard<std::mutex> lk(m_impl->mtx);
	uint32_t h = m_impl->nextHandle++;
	m_impl->pollGroups.push_back(h);
	return h;
}

bool NetBackend::DestroyPollGroup(uint32_t pg) {
	if (!m_impl) return false;
	std::lock_guard<std::mutex> lk(m_impl->mtx);
	auto& v = m_impl->pollGroups;
	v.erase(std::remove(v.begin(), v.end(), pg), v.end());
	for (auto& kv : m_impl->conns)
		if (kv.second.pollGroup == pg) kv.second.pollGroup = 0;
	return true;
}

bool NetBackend::SetConnectionPollGroup(uint32_t conn, uint32_t pg) {
	if (!m_impl) return false;
	std::lock_guard<std::mutex> lk(m_impl->mtx);
	auto it = m_impl->conns.find(conn);
	if (it == m_impl->conns.end()) return false;
	it->second.pollGroup = pg;
	return true;
}

int NetBackend::ReceiveOnPollGroup(uint32_t pg, SteamNetworkingMessage_t** out, int maxMsgs) {
	if (!m_impl || !out || maxMsgs <= 0) return 0;
	std::lock_guard<std::mutex> lk(m_impl->mtx);
	int n = 0;
	for (auto& kv : m_impl->conns) {
		if (kv.second.pollGroup != pg) continue;
		while (n < maxMsgs && !kv.second.inbox.empty()) {
			out[n++] = AllocNetMsg(kv.second.inbox.front(), kv.first, kv.second.remote);
			kv.second.inbox.pop_front();
		}
		if (n >= maxMsgs) break;
	}
	return n;
}

} // namespace emu
