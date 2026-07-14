//====== steamemu ============================================================
// Steam game overlay -- Dear ImGui implementation.
//
// The real Steam overlay renders in-game (shift-tab, invites, notifications)
// by hooking the game's present/swap path and drawing on top. We do the same
// with Dear ImGui: SwapchainHook.cpp patches IDXGISwapChain::Present and draws
// our panel over the game on the game's own device. If no game swapchain can be
// found (or STEAMEMU_OVERLAY_WINDOW is set for headless testing), Overlay.cpp's
// separate-window fallback stands up its own top-level window + D3D11 device on
// a background thread and renders there instead -- it never touches the game's
// present path, so a game whose overlay never appears still runs unaffected.
//
// This facade is deliberately free of any ImGui or Win32 type: those live only
// in the overlay .cpp/OverlayInternal.h TUs (behind STEAMEMU_ENABLE_OVERLAY), so
// no ImGui header ever reaches an exported signature or a generated stub TU. The
// generated ISteam* method bodies (ActivateGameOverlay*, IsOverlayEnabled, ...)
// include only THIS header. When the overlay is compiled out (headless/Linux
// build) every method is inert and Available() is false.
//
// The overlay is OPT-IN via steamemu.ini ([overlay] enabled = 1); Start() reads
// emu::Config() and does nothing unless enabled. See src/overlay/ and the
// [overlay] section documented in steamemu.ini.example.
//============================================================================
#pragma once

#include <string>

namespace emu {

struct OverlayImpl;  // defined in the overlay backend TUs (Win32/D3D11/ImGui state)

// Process-life overlay manager. Backs ISteamFriends::ActivateGameOverlay*,
// ISteamUtils::IsOverlayEnabled / BOverlayNeedsPresent, and the
// GameOverlayActivated_t signal (delivered by the lifecycle pump).
class OverlayManager {
public:
	static OverlayManager& Get();

	// Brings up the overlay (swapchain hook, or the separate-window fallback).
	// No-op if the overlay was compiled out or `[overlay] enabled` is false.
	// Idempotent: safe to call from every init path.
	void Start();
	void Stop();

	// True when built with overlay support AND a backend is live.
	bool Available() const;

	// Show/hide the overlay. Programmatic (game-initiated); the resulting
	// visibility change is reported as GameOverlayActivated_t with
	// m_bUserInitiated = false.
	void SetVisible(bool visible);
	bool IsVisible() const;

	// True only while the overlay is visible AND actually consuming input.
	bool IsExclusiveInput() const;

	// ISteamFriends::ActivateGameOverlay* entry point: raise the overlay. The
	// dialog string ("friends", "invite", a URL, ...) is advisory -- we surface
	// it as a toast and show the panel.
	void Show(const char* dialog);

	// Drain a pending visibility change (from the toggle hotkey, a window close,
	// or SetVisible/Show). Returns true and writes the new state + whether the
	// user triggered it if the visibility changed since the last call. The
	// lifecycle pump calls this each frame to deliver GameOverlayActivated_t.
	bool ConsumeVisibilityChange(bool& outVisible, bool& outUserInitiated);

	// Show a transient message in the overlay (invite/notification/action banner).
	void PushToast(const std::string& text);

	OverlayManager(const OverlayManager&) = delete;
	OverlayManager& operator=(const OverlayManager&) = delete;

private:
	OverlayManager();
	~OverlayManager();
	OverlayImpl* Impl_ = nullptr;  // null when compiled without overlay support
};

// Singleton accessor, mirroring emu::Net() / emu::Config() / emu::Dispatch().
inline OverlayManager& Overlay() { return OverlayManager::Get(); }

} // namespace emu
