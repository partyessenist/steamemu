//====== steamemu ============================================================
// Callback / call-result dispatch.
//
// Steam has two delivery mechanisms and we back both from one queue:
//
//  * Standard dispatch -- games build CCallback / CCallResult objects that call
//    SteamAPI_RegisterCallback / RegisterCallResult. Nothing fires until the
//    game calls SteamAPI_RunCallbacks(), at which point we invoke
//    CCallbackBase::Run on the registered objects on the calling thread.
//  * Manual dispatch -- the game pumps CallbackMsg_t structs itself via
//    SteamAPI_ManualDispatch_*.
//
// The emulator (and the networking backend) enqueue results with QueueCallback /
// QueueCallResult; whichever mechanism the game uses drains the queue.
//============================================================================
#pragma once

#include "steam/steam_api.h"  // transitively provides CCallbackBase, CallbackMsg_t

#include <cstdint>

namespace emu {

class Dispatcher {
public:
	static Dispatcher& Get();

	// --- registration (from the exported Register/Unregister entry points) ---
	void RegisterCallback(CCallbackBase* cb, int iCallback);
	void UnregisterCallback(CCallbackBase* cb);
	void RegisterCallResult(CCallbackBase* cb, SteamAPICall_t hCall);
	void UnregisterCallResult(CCallbackBase* cb, SteamAPICall_t hCall);

	// --- producers (emulator side) -------------------------------------------
	// Enqueue an immediate callback (broadcast to everyone registered for its id).
	// gameServer=true routes it to the game-server manual-dispatch pump/pipe only.
	// delayMs>0 holds delivery back that many ms (manual dispatch) to mimic Steam latency.
	void QueueCallback(int iCallback, const void* data, int size, bool gameServer = false, int delayMs = 0);
	// Enqueue an async result, returning the SteamAPICall_t the API method hands
	// back to the game now and completes on a later RunCallbacks / dispatch pump.
	SteamAPICall_t QueueCallResult(int iCallback, const void* data, int size, bool gameServer = false, int delayMs = 0);
	// Allocate a call handle without data yet (for methods that complete later).
	SteamAPICall_t AllocCall();
	// Queue a result on a handle previously obtained from AllocCall() -- for
	// results whose struct must carry its own call handle (e.g.
	// RemoteStorageFileReadAsyncComplete_t::m_hFileReadAsync).
	void QueueCallResultOn(SteamAPICall_t hCall, int iCallback, const void* data, int size, bool gameServer = false, int delayMs = 0);

	// --- standard dispatch ----------------------------------------------------
	void RunCallbacks(bool gameServer);

	// --- manual dispatch ------------------------------------------------------
	bool GetNextCallback(HSteamPipe pipe, CallbackMsg_t* msg);
	void FreeLastCallback(HSteamPipe pipe);
	bool GetAPICallResult(SteamAPICall_t hCall, void* pCallback, int cubCallback,
	                      int iCallbackExpected, bool* pbFailed);
	bool IsCallCompleted(SteamAPICall_t hCall, bool* pbFailed);

private:
	Dispatcher() = default;
};

// Real Steam never delivers a callback on the same frame the triggering call was made
// -- results come back a few frames later. Delivering instantly desyncs games whose
// state machines assume that gap, so every callback/result carries a small default
// latency in manual dispatch (standard RunCallbacks is unaffected). Specific calls that
// need longer pass an explicit delayMs (e.g. CreateLobby 750, game-server logon 200).
constexpr int kDefaultCallbackDelayMs = 50;

// Type-safe convenience wrappers.
template <class T>
inline void QueueCallback(const T& cb, int delayMs = kDefaultCallbackDelayMs) {
	Dispatcher::Get().QueueCallback(T::k_iCallback, &cb, sizeof(T), false, delayMs);
}
template <class T>
inline SteamAPICall_t QueueCallResult(const T& cb, int delayMs = kDefaultCallbackDelayMs) {
	return Dispatcher::Get().QueueCallResult(T::k_iCallback, &cb, sizeof(T), false, delayMs);
}
template <class T>
inline void QueueCallResultOn(SteamAPICall_t hCall, const T& cb, int delayMs = kDefaultCallbackDelayMs) {
	Dispatcher::Get().QueueCallResultOn(hCall, T::k_iCallback, &cb, sizeof(T), false, delayMs);
}
// Game-server variants: delivered only on the game-server manual-dispatch pipe, so a
// listen-server game's client and server callback loops don't consume each other's.
template <class T>
inline void QueueGameServerCallback(const T& cb, int delayMs = kDefaultCallbackDelayMs) {
	Dispatcher::Get().QueueCallback(T::k_iCallback, &cb, sizeof(T), true, delayMs);
}
template <class T>
inline SteamAPICall_t QueueGameServerCallResult(const T& cb, int delayMs = kDefaultCallbackDelayMs) {
	return Dispatcher::Get().QueueCallResult(T::k_iCallback, &cb, sizeof(T), true, delayMs);
}

inline Dispatcher& Dispatch() { return Dispatcher::Get(); }

} // namespace emu
