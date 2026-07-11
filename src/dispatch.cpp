//====== steamemu ============================================================
// Callback / call-result dispatch implementation. See dispatch.h.
//============================================================================
#include "dispatch.h"
#include "emu_common.h"

#include "steam/isteamutils.h"  // SteamAPICallCompleted_t

#include <chrono>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

namespace emu {
namespace {

// A monotonic sequence stamped on every queued callback AND result so manual
// dispatch can deliver them in insertion order across both queues -- real Steam
// interleaves them, and e.g. CreateLobby's LobbyCreated_t must reach the game
// before its LobbyEnter_t or the game runs its "joined a lobby" path instead of
// its "created a lobby" path (and never populates the lobby's data).
uint64_t g_seq = 0;

int64_t NowMs() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Some async results must not complete instantly: real Steam take
// a beat to return, and games rely on it.
// readyAt gates manual-dispatch delivery until this wall-clock time is reached.
struct QueuedCallback {
	int iCallback;
	std::vector<uint8_t> data;
	bool gameServer = false;   // manual dispatch: deliver only on the matching pipe
	uint64_t seq = 0;
	int64_t readyAt = 0;
};

struct QueuedResult {
	SteamAPICall_t hCall;
	int iCallback;
	std::vector<uint8_t> data;
	bool ioFailure = false;
	bool gameServer = false;
	uint64_t seq = 0;
	int64_t readyAt = 0;
};

std::mutex g_mutex;

std::vector<std::pair<CCallbackBase*, int>> g_callbacks;         // registered CCallback
std::vector<std::pair<CCallbackBase*, SteamAPICall_t>> g_results; // registered CCallResult

// CCallbackBase::m_nCallbackFlags is the byte immediately after the vtable
// pointer (the layout is frozen ABI). Valve's CCallbackMgr toggles
// k_ECallbackFlagsRegistered there; the CCallback/CCallResult destructors only
// unregister when it is set -- so if we don't maintain it, a destroyed callback
// stays in our list and RunCallbacks later calls into freed memory. We MUST
// set it on register and clear it on unregister.
constexpr uint8_t kFlagRegistered = 0x01;  // k_ECallbackFlagsRegistered
constexpr uint8_t kFlagGameServer = 0x02;  // k_ECallbackFlagsGameServer
uint8_t& CallbackFlags(CCallbackBase* cb) {
	return *(reinterpret_cast<uint8_t*>(cb) + sizeof(void*));
}

std::deque<QueuedCallback> g_pendingCallbacks;
std::deque<QueuedResult> g_pendingResults;

// Results awaiting a GetAPICallResult fetch (manual dispatch). Kept until fetched.
std::vector<QueuedResult> g_completed;

// The last CallbackMsg_t handed to the game via manual dispatch, whose buffer we
// own until FreeLastCallback.
std::vector<uint8_t> g_manualCurrent;
bool g_manualHeld = false;

SteamAPICall_t g_nextCall = 1;

bool IsGameServerCallback(CCallbackBase* cb) {
	return (CallbackFlags(cb) & kFlagGameServer) != 0;
}

} // namespace

Dispatcher& Dispatcher::Get() {
	static Dispatcher d;
	return d;
}

void Dispatcher::RegisterCallback(CCallbackBase* cb, int iCallback) {
	if (!cb) return;
	std::lock_guard<std::mutex> lk(g_mutex);
	CallbackFlags(cb) |= kFlagRegistered;   // so ~CCallback unregisters itself
	g_callbacks.emplace_back(cb, iCallback);
}

void Dispatcher::UnregisterCallback(CCallbackBase* cb) {
	if (!cb) return;
	std::lock_guard<std::mutex> lk(g_mutex);
	CallbackFlags(cb) &= ~kFlagRegistered;
	for (size_t i = 0; i < g_callbacks.size();) {
		if (g_callbacks[i].first == cb)
			g_callbacks.erase(g_callbacks.begin() + i);
		else
			++i;
	}
}

void Dispatcher::RegisterCallResult(CCallbackBase* cb, SteamAPICall_t hCall) {
	if (!cb) return;
	std::lock_guard<std::mutex> lk(g_mutex);
	CallbackFlags(cb) |= kFlagRegistered;
	g_results.emplace_back(cb, hCall);
}

void Dispatcher::UnregisterCallResult(CCallbackBase* cb, SteamAPICall_t hCall) {
	if (!cb) return;
	std::lock_guard<std::mutex> lk(g_mutex);
	CallbackFlags(cb) &= ~kFlagRegistered;
	for (size_t i = 0; i < g_results.size();) {
		if (g_results[i].first == cb && g_results[i].second == hCall)
			g_results.erase(g_results.begin() + i);
		else
			++i;
	}
}

void Dispatcher::QueueCallback(int iCallback, const void* data, int size, bool gameServer, int delayMs) {
	std::lock_guard<std::mutex> lk(g_mutex);
	QueuedCallback q;
	q.iCallback = iCallback;
	q.data.assign(static_cast<const uint8_t*>(data),
	              static_cast<const uint8_t*>(data) + size);
	q.gameServer = gameServer;
	q.seq = g_seq++;
	q.readyAt = NowMs() + (delayMs > 0 ? delayMs : 0);
	g_pendingCallbacks.push_back(std::move(q));
}

SteamAPICall_t Dispatcher::AllocCall() {
	std::lock_guard<std::mutex> lk(g_mutex);
	return g_nextCall++;
}

void Dispatcher::QueueCallResultOn(SteamAPICall_t hCall, int iCallback, const void* data, int size, bool gameServer, int delayMs) {
	std::lock_guard<std::mutex> lk(g_mutex);
	QueuedResult q;
	q.hCall = hCall;
	q.iCallback = iCallback;
	q.data.assign(static_cast<const uint8_t*>(data),
	              static_cast<const uint8_t*>(data) + size);
	q.gameServer = gameServer;
	q.seq = g_seq++;
	q.readyAt = NowMs() + (delayMs > 0 ? delayMs : 0);
	g_pendingResults.push_back(std::move(q));
}

SteamAPICall_t Dispatcher::QueueCallResult(int iCallback, const void* data, int size, bool gameServer, int delayMs) {
	std::lock_guard<std::mutex> lk(g_mutex);
	QueuedResult q;
	q.hCall = g_nextCall++;
	q.iCallback = iCallback;
	q.data.assign(static_cast<const uint8_t*>(data),
	              static_cast<const uint8_t*>(data) + size);
	q.gameServer = gameServer;
	q.seq = g_seq++;
	q.readyAt = NowMs() + (delayMs > 0 ? delayMs : 0);
	g_pendingResults.push_back(q);
	return q.hCall;
}

void Dispatcher::RunCallbacks(bool gameServer) {
	(void)gameServer;
	// Snapshot-and-drain under the lock, then invoke Run() outside it: a handler
	// may re-enter (register/unregister, queue more) without deadlocking.
	std::deque<QueuedCallback> callbacks;
	std::deque<QueuedResult> results;
	std::vector<std::pair<CCallbackBase*, int>> cbReg;
	std::vector<std::pair<CCallbackBase*, SteamAPICall_t>> resReg;
	{
		std::lock_guard<std::mutex> lk(g_mutex);
		callbacks.swap(g_pendingCallbacks);
		results.swap(g_pendingResults);
		cbReg = g_callbacks;
		resReg = g_results;
	}

	// Interleave the two queues by insertion order (seq). Delivering all plain
	// callbacks first would hand a game CreateLobby's LobbyEnter_t before its
	// LobbyCreated_t -- running its "joined someone's lobby" path (a data READ)
	// before its "I created it" path (the data WRITE), leaving the lobby empty.
	size_t ci = 0, ri = 0;
	while (ci < callbacks.size() || ri < results.size()) {
		const bool useCb = ci < callbacks.size() &&
			(ri >= results.size() || callbacks[ci].seq < results[ri].seq);
		if (useCb) {
			const auto& q = callbacks[ci++];
			for (const auto& reg : cbReg) {
				// Deliver a callback to listeners whose kind matches this pump:
				// client callbacks on SteamAPI_RunCallbacks, gameserver ones on
				// SteamGameServer_RunCallbacks.
				if (reg.second == q.iCallback && IsGameServerCallback(reg.first) == gameServer)
					reg.first->Run(const_cast<uint8_t*>(q.data.data()));
			}
			continue;
		}
		auto& r = results[ri++];
		for (const auto& reg : resReg) {
			if (reg.second == r.hCall)
				reg.first->Run(r.data.data(), r.ioFailure, r.hCall);
		}
		// A call-result can also be observed as a plain callback of its own id.
		for (const auto& reg : cbReg) {
			if (reg.second == r.iCallback)
				reg.first->Run(r.data.data());
		}
		// Retain for a possible manual GetAPICallResult fetch.
		std::lock_guard<std::mutex> lk(g_mutex);
		g_completed.push_back(std::move(r));
	}
}

bool Dispatcher::GetNextCallback(HSteamPipe pipe, CallbackMsg_t* msg) {
	if (!msg) return false;
	std::lock_guard<std::mutex> lk(g_mutex);
	if (g_manualHeld) return false;  // must FreeLastCallback first

	// A listen-server game pumps the client and game-server pipes separately and
	// expects each to yield only its own callbacks. Deliver the first queued item
	// whose kind matches this pipe, preserving per-pipe FIFO order.
	const bool wantGS = (pipe == kHSteamPipeGameServer);
	const HSteamUser user = wantGS ? kHSteamUserGameServer : kHSteamUser;

	// First matching, DUE item in each queue (deques are already in insertion order;
	// an item whose readyAt is in the future is held back to mimic Steam latency).
	const int64_t now = NowMs();
	auto cbIt = g_pendingCallbacks.end();
	for (auto it = g_pendingCallbacks.begin(); it != g_pendingCallbacks.end(); ++it)
		if (it->gameServer == wantGS && it->readyAt <= now) { cbIt = it; break; }
	auto resIt = g_pendingResults.end();
	for (auto it = g_pendingResults.begin(); it != g_pendingResults.end(); ++it)
		if (it->gameServer == wantGS && it->readyAt <= now) { resIt = it; break; }

	const bool haveCb = cbIt != g_pendingCallbacks.end();
	const bool haveRes = resIt != g_pendingResults.end();
	if (!haveCb && !haveRes) return false;
	// Deliver whichever was queued first, so callbacks and call-results interleave
	// in the order the game expects (LobbyCreated_t before LobbyEnter_t, etc.).
	const bool useCb = haveCb && (!haveRes || cbIt->seq < resIt->seq);

	if (useCb) {
		g_manualCurrent = std::move(cbIt->data);
		int cb = cbIt->iCallback;
		g_pendingCallbacks.erase(cbIt);
		g_manualHeld = true;
		msg->m_hSteamUser = user;
		msg->m_iCallback = cb;
		msg->m_pubParam = g_manualCurrent.data();
		msg->m_cubParam = static_cast<int>(g_manualCurrent.size());
		EMU_DEBUG("manual dispatch pipe %d: deliver callback %d (%d bytes)",
		          (int)pipe, cb, msg->m_cubParam);
		return true;
	}
	// Present the result as a SteamAPICallCompleted_t; the game then fetches the
	// payload with GetAPICallResult.
	QueuedResult r = std::move(*resIt);
	g_pendingResults.erase(resIt);
	// Real Steam also posts these results to plain-callback listeners of the same
	// id (isteammatchmaking.h: "results will be returned by LobbyCreated_t
	// callback AND call result") -- standard dispatch already broadcasts them in
	// RunCallbacks, so mirror it here. Some games write their lobby
	// metadata from a Callback<LobbyCreated_t>, not the CallResult; without this
	// shadow that handler never runs and the lobby stays empty. push_front with
	// the result's own seq so it is the very next message on this pipe, ahead of
	// anything queued after the result (e.g. CreateLobby's LobbyEnter_t).
	QueuedCallback shadow;
	shadow.iCallback = r.iCallback;
	shadow.data = r.data;  // copy: r's buffer is retained for GetAPICallResult
	shadow.gameServer = r.gameServer;
	shadow.seq = r.seq;
	g_pendingCallbacks.push_front(std::move(shadow));
	SteamAPICallCompleted_t done{};
	done.m_hAsyncCall = r.hCall;
	done.m_iCallback = r.iCallback;
	done.m_cubParam = static_cast<uint32>(r.data.size());
	g_completed.push_back(std::move(r));
	g_manualCurrent.assign(reinterpret_cast<uint8_t*>(&done),
	                       reinterpret_cast<uint8_t*>(&done) + sizeof(done));
	g_manualHeld = true;
	msg->m_hSteamUser = user;
	msg->m_iCallback = SteamAPICallCompleted_t::k_iCallback;
	msg->m_pubParam = g_manualCurrent.data();
	msg->m_cubParam = static_cast<int>(g_manualCurrent.size());
	EMU_DEBUG("manual dispatch pipe %d: deliver completion 703 (result %d, call %llu)",
	          (int)pipe, done.m_iCallback, (unsigned long long)done.m_hAsyncCall);
	return true;
}

void Dispatcher::FreeLastCallback(HSteamPipe) {
	std::lock_guard<std::mutex> lk(g_mutex);
	g_manualHeld = false;
	g_manualCurrent.clear();
}

bool Dispatcher::IsCallCompleted(SteamAPICall_t hCall, bool* pbFailed) {
	std::lock_guard<std::mutex> lk(g_mutex);
	for (const auto& r : g_completed)
		if (r.hCall == hCall) { if (pbFailed) *pbFailed = r.ioFailure; return true; }
	for (const auto& r : g_pendingResults)
		if (r.hCall == hCall) { if (pbFailed) *pbFailed = r.ioFailure; return false; }
	// Unknown handle: report completed-but-failed so callers stop polling.
	if (pbFailed) *pbFailed = true;
	return true;
}

bool Dispatcher::GetAPICallResult(SteamAPICall_t hCall, void* pCallback, int cubCallback,
                                  int iCallbackExpected, bool* pbFailed) {
	std::lock_guard<std::mutex> lk(g_mutex);
	for (const auto& r : g_completed) {
		if (r.hCall != hCall) continue;
		if (iCallbackExpected && r.iCallback != iCallbackExpected) return false;
		if (pbFailed) *pbFailed = r.ioFailure;
		if (pCallback && cubCallback > 0) {
			int n = static_cast<int>(r.data.size());
			if (n > cubCallback) n = cubCallback;
			std::memcpy(pCallback, r.data.data(), n);
		}
		return true;
	}
	return false;
}

} // namespace emu
