//====== steamemu ============================================================
// Local auth shimming implementation. See auth.h.
//============================================================================
#include "auth.h"
#include "dispatch.h"
#include "emu_common.h"

#include "steam/isteamuser.h"  // GetAuthSessionTicketResponse_t, ValidateAuthTicketResponse_t

#include <cstring>
#include <mutex>

namespace emu {
namespace {
std::mutex g_mutex;

// Wire format of a fabricated ticket. Small and self-describing so a peer can
// recover the issuer's identity without a backend.
#pragma pack(push, 1)
struct EmuTicket {
	uint32_t magic;   // 'STEM'
	uint32_t version;
	uint64_t steamID; // issuer
	uint32_t appID;
	uint32_t handle;
};
#pragma pack(pop)
constexpr uint32_t kMagic = 0x4d455453;  // 'STEM'
} // namespace

AuthManager& AuthManager::Get() {
	static AuthManager m;
	return m;
}

HAuthTicket AuthManager::GetAuthSessionTicket(void* pTicket, int cbMaxTicket, uint32* pcbTicket) {
	std::lock_guard<std::mutex> lk(g_mutex);
	HAuthTicket h = m_nextTicket++;
	EmuTicket t{};
	t.magic = kMagic;
	t.version = 1;
	t.steamID = LocalSteamID().ConvertToUint64();
	t.appID = AppID();
	t.handle = h;
	if (pTicket && cbMaxTicket >= static_cast<int>(sizeof(t)))
		std::memcpy(pTicket, &t, sizeof(t));
	if (pcbTicket)
		*pcbTicket = sizeof(t);

	GetAuthSessionTicketResponse_t resp{};
	resp.m_hAuthTicket = h;
	resp.m_eResult = k_EResultOK;
	QueueCallback(resp);
	return h;
}

HAuthTicket AuthManager::GetAuthTicketForWebApi(const char* pchIdentity) {
	(void)pchIdentity;
	std::lock_guard<std::mutex> lk(g_mutex);
	HAuthTicket h = m_nextTicket++;

	// The web-API ticket is delivered ONLY through GetTicketForWebApiResponse_t
	// (k_iSteamUserCallbacks + 68), which carries the ticket bytes inline -- there
	// is no output buffer on the call. Delivering GetAuthSessionTicketResponse_t
	// (+63) instead leaves a game that registered for +68 waiting forever. Carry
	// the same self-describing blob GetAuthSessionTicket uses so a peer/backend
	// that inspects it recovers our identity.
	EmuTicket t{};
	t.magic = kMagic;
	t.version = 1;
	t.steamID = LocalSteamID().ConvertToUint64();
	t.appID = AppID();
	t.handle = h;

	GetTicketForWebApiResponse_t resp{};
	resp.m_hAuthTicket = h;
	resp.m_eResult = k_EResultOK;
	resp.m_cubTicket = static_cast<int>(sizeof(t));
	std::memcpy(resp.m_rgubTicket, &t, sizeof(t));
	QueueCallback(resp);
	return h;
}

EResult AuthManager::RequestEncryptedAppTicket(const void* pDataToInclude, int cbDataToInclude) {
	// Real tickets are opaque bytes encrypted with the developer's key; with no
	// backend we fabricate a structured blob (identity + appid + gen date + the
	// caller's included data). Games that merely require "a ticket arrived"
	// proceed; only a third-party backend decrypting with the real key could
	// tell the difference (and it has no reason to accept us anyway).
	std::lock_guard<std::mutex> lk(g_mutex);
	if (cbDataToInclude < 0 || (cbDataToInclude > 0 && !pDataToInclude))
		return k_EResultInvalidParam;
	struct {
		uint32_t magic, version;
		uint64_t steamID;
		uint32_t appID, genDate, cbUserData;
	} hdr{kMagic, 2 /*encrypted app ticket*/, LocalSteamID().ConvertToUint64(),
	      AppID(), (uint32_t)UnixTime(), (uint32_t)cbDataToInclude};
	m_encTicket.assign(reinterpret_cast<uint8_t*>(&hdr),
	                   reinterpret_cast<uint8_t*>(&hdr) + sizeof(hdr));
	if (cbDataToInclude > 0)
		m_encTicket.insert(m_encTicket.end(), static_cast<const uint8_t*>(pDataToInclude),
		                   static_cast<const uint8_t*>(pDataToInclude) + cbDataToInclude);
	return k_EResultOK;
}

bool AuthManager::GetEncryptedAppTicket(void* pTicket, int cbMaxTicket, uint32* pcbTicket) {
	std::lock_guard<std::mutex> lk(g_mutex);
	if (m_encTicket.empty()) return false;  // no RequestEncryptedAppTicket yet
	if (pcbTicket) *pcbTicket = (uint32)m_encTicket.size();
	if (!pTicket || cbMaxTicket < (int)m_encTicket.size()) return false;
	std::memcpy(pTicket, m_encTicket.data(), m_encTicket.size());
	return true;
}

EBeginAuthSessionResult AuthManager::BeginAuthSession(const void* pAuthTicket, int cbAuthTicket, CSteamID steamID) {
	CSteamID owner = steamID;
	if (pAuthTicket && cbAuthTicket >= static_cast<int>(sizeof(EmuTicket))) {
		EmuTicket t{};
		std::memcpy(&t, pAuthTicket, sizeof(t));
		if (t.magic == kMagic)
			owner = CSteamID(static_cast<uint64>(t.steamID));
	}

	ValidateAuthTicketResponse_t resp{};
	resp.m_SteamID = steamID;
	resp.m_eAuthSessionResponse = k_EAuthSessionResponseOK;
	resp.m_OwnerSteamID = owner;
	QueueCallback(resp);
	return k_EBeginAuthSessionResultOK;
}

void AuthManager::EndAuthSession(CSteamID) {}
void AuthManager::CancelAuthTicket(HAuthTicket) {}

} // namespace emu
