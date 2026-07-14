//====== steamemu ============================================================
// Internal, ImGui/Win32-aware definition of OverlayImpl. Shared by the two
// backend translation units that render the overlay:
//
//   * Overlay.cpp       -- the separate-window fallback (its own Win32 window +
//                          D3D11 device on a background thread).
//   * SwapchainHook.cpp -- the in-game backend that hooks the game's
//                          IDXGISwapChain::Present and draws on the game's device.
//
// Only these two TUs (both gated on STEAMEMU_ENABLE_OVERLAY) include this header,
// so no ImGui or Win32 type ever reaches an exported signature or a generated
// stub TU -- the same rule the overlay.h facade documents. The two backends are
// mutually exclusive at runtime: Start() installs the hook and, only if that
// fails, falls back to the window.
//
// The shared state (visibility, toggle hotkey, toasts) lives on OverlayImpl and
// is the single source of truth both backends read. The panel-drawing members
// operate on whatever ImGui context is current, so each backend can own its own
// context on its own device, and pull live data straight from emu::Net().
//============================================================================
#pragma once

#include "overlay/overlay.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace emu {

// In-game swapchain-hook backend state. Defined and driven entirely by
// SwapchainHook.cpp; OverlayImpl only owns the object and asks it to install /
// uninstall. Kept out of OverlayImpl's own header footprint so the windowed
// path never touches DXGI hooking.
struct HookState;

struct OverlayImpl {
	OverlayImpl() = default;

	// --- shared visibility state (read by both backends) -------------------
	std::atomic<bool> WantVisible{false};  // authoritative desired visibility
	std::atomic<bool> Exclusive{false};    // visible AND consuming input

	// Set WantVisible and record who asked, for GameOverlayActivated_t's
	// m_bUserInitiated. The lifecycle pump edge-detects WantVisible via
	// ConsumeVisibilityChange (which reads LastUserInitiated).
	std::atomic<bool> LastUserInitiated{false};
	bool LastReported = false;  // only touched by the pump/game thread
	void SetWantVisible(bool v, bool userInitiated) {
		LastUserInitiated.store(userInitiated);
		WantVisible.store(v);
	}

	// Global toggle shortcut (decoded Win32 vk + modifiers). Vk 0 disables it.
	std::atomic<int> Vk{0};
	std::atomic<bool> ModShift{false};
	std::atomic<bool> ModCtrl{false};
	std::atomic<bool> ModAlt{false};
	bool HotkeyWasDown = false;

	struct Toast { std::string Text; std::chrono::steady_clock::time_point Expiry; };
	std::mutex ToastMutex;
	std::vector<Toast> Toasts;

	// --- separate-window backend (Overlay.cpp) -----------------------------
	std::thread Thread;
	std::atomic<bool> Running{false};
	ID3D11Device* Device = nullptr;
	ID3D11DeviceContext* Context = nullptr;
	IDXGISwapChain* Swapchain = nullptr;
	ID3D11RenderTargetView* Rtv = nullptr;
	HWND Hwnd = nullptr;
	UINT ResizeW = 0, ResizeH = 0;

	// --- in-game hook backend (SwapchainHook.cpp) --------------------------
	std::atomic<bool> HookActive{false};  // vtable patched and armed
	HookState* Hook = nullptr;

	// --- shared helpers ----------------------------------------------------
	void RequestToggle() { SetWantVisible(!WantVisible.load(), true); }
	void RequestHide() {
		SetWantVisible(false, true);
		if (Hwnd) ShowWindow(Hwnd, SW_HIDE);
	}
	void PushToast(const std::string& text) {
		std::lock_guard<std::mutex> lock(ToastMutex);
		Toasts.push_back({text, std::chrono::steady_clock::now() + std::chrono::seconds(4)});
	}

	// Rising-edge toggle-hotkey poll. Called by whichever backend has a frame
	// loop (the window thread, or the hooked Present). Safe on any thread.
	void PollHotkey();

	// --- shared panel drawing (operate on the current ImGui context) -------
	// InGame=false: the separate window fills its client area with an opaque
	// panel. InGame=true: draw a translucent, movable panel over the game.
	void DrawFriendsPanel(bool inGame);
	void DrawToasts();

	// --- separate-window backend entry points (Overlay.cpp) ----------------
	void ThreadMain();
	bool CreateDevice();
	void CleanupDevice();
	void CreateRtv();
	void CleanupRtv();
	void RenderFrame();

	// --- in-game hook backend entry points (SwapchainHook.cpp) -------------
	// Patches the shared IDXGISwapChain vtable so the game's Present is
	// intercepted. Returns false (leaving nothing patched) if a swapchain
	// vtable can't be discovered or another overlay already owns the hook.
	bool TryStartHook();
	// Restores the vtable, tears down the ImGui backend and WndProc hook.
	void StopHook();
};

} // namespace emu
