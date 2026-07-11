//====== steamemu ============================================================
// Local auth shimming. There is no Valve backend, so we fabricate session
// tickets and validate peers among emulator instances rather than against
// Steam. Tickets carry the issuer's CSteamID so a peer's BeginAuthSession can
// recover who connected. Everything succeeds locally.
//============================================================================
#pragma once

#include "steam/steam_api.h"
#include "steam/steamclientpublic.h"

#include <cstdint>
#include <vector>

namespace emu {

class AuthManager {
public:
	static AuthManager& Get();

	// Fill pTicket with a fabricated ticket, queue GetAuthSessionTicketResponse_t
	// (success), and return the ticket handle.
	HAuthTicket GetAuthSessionTicket(void* pTicket, int cbMaxTicket, uint32* pcbTicket);
	HAuthTicket GetAuthTicketForWebApi(const char* pchIdentity);

	// Encrypted app ticket: fabricate a blob carrying our identity, the AppID
	// and the caller's included data (real tickets are opaque encrypted bytes,
	// so a plausible blob is the best no-backend answer). The caller queues the
	// EncryptedAppTicketResponse_t; the sync getter hands out the same bytes.
	EResult RequestEncryptedAppTicket(const void* pDataToInclude, int cbDataToInclude);
	bool GetEncryptedAppTicket(void* pTicket, int cbMaxTicket, uint32* pcbTicket);

	// Validate a peer ticket; queues ValidateAuthTicketResponse_t (OK).
	EBeginAuthSessionResult BeginAuthSession(const void* pAuthTicket, int cbAuthTicket, CSteamID steamID);
	void EndAuthSession(CSteamID steamID);
	void CancelAuthTicket(HAuthTicket hAuthTicket);

private:
	AuthManager() = default;
	uint32_t m_nextTicket = 1;
	std::vector<uint8_t> m_encTicket;  // last fabricated encrypted app ticket
};

inline AuthManager& Auth() { return AuthManager::Get(); }

} // namespace emu
