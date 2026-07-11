//====== steamemu ============================================================
// Async call-result completion test.
//
// Every SteamAPICall_t-returning method must COMPLETE: a game parks a
// CCallResult on the returned handle and waits forever if no result is queued
// (loading screens that never finish). Stubs auto-complete with a zeroed
// success result (see callresult_default in tools/gen.py); curated methods
// carry real data. This test parks real CCallResults, pumps
// SteamAPI_RunCallbacks(), and checks:
//   * a generator-default method (GetFollowerCount) completes with k_EResultOK,
//   * curated results carry their promised fields (GetNumberOfCurrentPlayers,
//     CheckFileSignature),
//   * the same result ALSO reaches a plain Callback<T> listener,
//   * FileReadAsync round-trips real bytes through a real file,
//   * StartPurchase does NOT succeed (games may grant paid items),
//   * the ISteamUtils vtable polling path (IsAPICallCompleted /
//     GetAPICallResult) sees completions too.
//============================================================================
#include <cstdio>
#include <cstring>

#include "steam/steam_api.h"

static int g_failures = 0;
#define CHECK(cond, msg) do { \
	if (!(cond)) { printf("FAIL: %s\n", msg); ++g_failures; } \
	else { printf("ok  : %s\n", msg); } \
} while (0)

// Generic CCallResult waiter: parks on a handle, records the payload.
template <class T>
struct Waiter {
	bool fired = false;
	bool ioFailure = false;
	T res{};
	CCallResult<Waiter<T>, T> cr;
	void Park(SteamAPICall_t h) { cr.Set(h, this, &Waiter<T>::On); }
	void On(T* p, bool bIOFailure) { fired = true; ioFailure = bIOFailure; res = *p; }
};

// Plain callback listener (games often hook Callback<T> instead of the
// CallResult; the dispatcher must deliver results to both).
struct PlayersListener {
	bool fired = false;
	NumberOfCurrentPlayers_t res{};
	STEAM_CALLBACK(PlayersListener, OnPlayers, NumberOfCurrentPlayers_t);
};
void PlayersListener::OnPlayers(NumberOfCurrentPlayers_t* p) { fired = true; res = *p; }

// GetAuthTicketForWebApi delivers its ticket ONLY via this callback (games hook
// it directly; there is no output buffer on the call).
struct WebApiTicketListener {
	bool fired = false;
	GetTicketForWebApiResponse_t res{};
	STEAM_CALLBACK(WebApiTicketListener, OnTicket, GetTicketForWebApiResponse_t);
};
void WebApiTicketListener::OnTicket(GetTicketForWebApiResponse_t* p) { fired = true; res = *p; }

int main() {
	if (!SteamAPI_Init()) { printf("FAIL: init\n"); return 1; }

	// --- generator-default completion: zeroed success ------------------------
	{
		Waiter<FriendsGetFollowerCount_t> w;
		SteamAPICall_t h = SteamFriends()->GetFollowerCount(SteamUser()->GetSteamID());
		CHECK(h != k_uAPICallInvalid, "GetFollowerCount returns a call handle");
		w.Park(h);
		CHECK(!w.fired, "result NOT delivered before RunCallbacks");
		SteamAPI_RunCallbacks();
		CHECK(w.fired, "stubbed call-result completes (GetFollowerCount)");
		CHECK(!w.ioFailure, "completion is not an IO failure");
		CHECK(w.res.m_eResult == k_EResultOK, "zeroed-success result carries k_EResultOK");
		CHECK(w.res.m_nCount == 0, "zeroed-success result carries empty data");
	}

	// --- curated: GetNumberOfCurrentPlayers, seen by CallResult AND Callback --
	{
		Waiter<NumberOfCurrentPlayers_t> w;
		PlayersListener plain;
		SteamAPICall_t h = SteamUserStats()->GetNumberOfCurrentPlayers();
		CHECK(h != k_uAPICallInvalid, "GetNumberOfCurrentPlayers returns a call handle");
		w.Park(h);
		SteamAPI_RunCallbacks();
		CHECK(w.fired, "GetNumberOfCurrentPlayers completes");
		CHECK(w.res.m_bSuccess == 1 && w.res.m_cPlayers == 1,
		      "player count reports success with 1 player");
		CHECK(plain.fired && plain.res.m_cPlayers == 1,
		      "plain Callback<T> listener saw the same result");
	}

	// --- curated: CheckFileSignature -----------------------------------------
	{
		Waiter<CheckFileSignature_t> w;
		w.Park(SteamUtils()->CheckFileSignature("game.exe"));
		SteamAPI_RunCallbacks();
		CHECK(w.fired && w.res.m_eCheckFileSignature == k_ECheckFileSignatureValidSignature,
		      "CheckFileSignature reports a valid signature");
	}

	// --- FileReadAsync round-trips real bytes --------------------------------
	{
		ISteamRemoteStorage* rs = SteamRemoteStorage();
		static const char kPayload[] = "async-read-payload";
		CHECK(rs->FileWrite("async.dat", kPayload, sizeof(kPayload)), "FileWrite for async read");

		Waiter<RemoteStorageFileReadAsyncComplete_t> w;
		SteamAPICall_t h = rs->FileReadAsync("async.dat", 6, sizeof(kPayload) - 6);
		CHECK(h != k_uAPICallInvalid, "FileReadAsync returns a call handle");
		w.Park(h);
		SteamAPI_RunCallbacks();
		CHECK(w.fired, "FileReadAsync completes");
		CHECK(w.res.m_eResult == k_EResultOK, "FileReadAsync succeeded");
		CHECK(w.res.m_hFileReadAsync == h, "completion carries its own call handle");
		CHECK(w.res.m_nOffset == 6 && w.res.m_cubRead == sizeof(kPayload) - 6,
		      "completion reports the requested offset/size");
		char buf[64] = {};
		CHECK(rs->FileReadAsyncComplete(w.res.m_hFileReadAsync, buf, w.res.m_cubRead),
		      "FileReadAsyncComplete hands out the bytes");
		CHECK(std::memcmp(buf, kPayload + 6, sizeof(kPayload) - 6) == 0,
		      "bytes match the file contents at the offset");

		// A missing file must complete too -- with failure, not silence.
		Waiter<RemoteStorageFileReadAsyncComplete_t> miss;
		miss.Park(rs->FileReadAsync("no-such-file.dat", 0, 16));
		SteamAPI_RunCallbacks();
		CHECK(miss.fired && miss.res.m_eResult != k_EResultOK,
		      "FileReadAsync on a missing file completes with failure");
	}

	// --- GetAuthTicketForWebApi delivers GetTicketForWebApiResponse_t ---------
	// Regression: it previously queued GetAuthSessionTicketResponse_t (+63), so a
	// game hooking GetTicketForWebApiResponse_t (+68).
	{
		WebApiTicketListener wl;
		HAuthTicket h = SteamUser()->GetAuthTicketForWebApi("test-identity");
		CHECK(h != k_HAuthTicketInvalid, "GetAuthTicketForWebApi returns a handle");
		CHECK(!wl.fired, "web-api ticket NOT delivered before RunCallbacks");
		SteamAPI_RunCallbacks();
		CHECK(wl.fired, "GetTicketForWebApiResponse_t delivered (correct callback id)");
		CHECK(wl.res.m_hAuthTicket == h, "response carries the issued handle");
		CHECK(wl.res.m_eResult == k_EResultOK, "response reports success");
		CHECK(wl.res.m_cubTicket > 0, "response carries a non-empty ticket");
	}

	// --- RequestEncryptedAppTicket + sync getter round-trip -------------------
	{
		ISteamUser* user = SteamUser();
		uint8 before[16];
		uint32 cb = 0;
		CHECK(!user->GetEncryptedAppTicket(before, sizeof(before), &cb),
		      "GetEncryptedAppTicket fails before any request");

		static const char kInclude[] = "backend-nonce";
		Waiter<EncryptedAppTicketResponse_t> w;
		w.Park(user->RequestEncryptedAppTicket((void*)kInclude, sizeof(kInclude)));
		SteamAPI_RunCallbacks();
		CHECK(w.fired && w.res.m_eResult == k_EResultOK, "RequestEncryptedAppTicket completes OK");

		uint8 ticket[512];
		cb = 0;
		CHECK(user->GetEncryptedAppTicket(ticket, sizeof(ticket), &cb) && cb > sizeof(kInclude),
		      "GetEncryptedAppTicket hands out the ticket bytes");
		// The included data rides at the tail of the fabricated blob.
		CHECK(cb >= sizeof(kInclude) &&
		      std::memcmp(ticket + cb - sizeof(kInclude), kInclude, sizeof(kInclude)) == 0,
		      "ticket carries the data the game asked to include");
		uint32 need = 0;
		CHECK(!user->GetEncryptedAppTicket(ticket, 4, &need) && need == cb,
		      "too-small buffer fails and reports the required size");
	}

	// --- StartPurchase must NOT succeed --------------------------------------
	{
		Waiter<SteamInventoryStartPurchaseResult_t> w;
		SteamItemDef_t item = 1; uint32 qty = 1;
		w.Park(SteamInventory()->StartPurchase(&item, &qty, 1));
		SteamAPI_RunCallbacks();
		CHECK(w.fired, "StartPurchase completes");
		CHECK(w.res.m_result != k_EResultOK, "StartPurchase does not fake a purchase");
	}

	// --- ISteamUtils vtable polling path -------------------------------------
	{
		ISteamUtils* utils = SteamUtils();
		SteamAPICall_t h = SteamUserStats()->GetNumberOfCurrentPlayers();
		bool failed = true;
		CHECK(!utils->IsAPICallCompleted(h, &failed), "pending call reports not-completed");
		SteamAPI_RunCallbacks();
		CHECK(utils->IsAPICallCompleted(h, &failed) && !failed,
		      "completed call reports completed via ISteamUtils");
		NumberOfCurrentPlayers_t res{};
		CHECK(utils->GetAPICallResult(h, &res, sizeof(res), res.k_iCallback, &failed),
		      "ISteamUtils::GetAPICallResult fetches the payload");
		CHECK(res.m_bSuccess == 1 && res.m_cPlayers == 1, "fetched payload carries the data");
	}

	SteamAPI_Shutdown();
	if (g_failures == 0) { printf("PASS: async_test all checks passed\n"); return 0; }
	printf("FAILED: %d check(s)\n", g_failures);
	return 1;
}
