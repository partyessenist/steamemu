//====== steamemu ============================================================
// Steam overlay implementation. See overlay.h for the design rationale and
// overlay_internal.h for the shared OverlayImpl definition.
//
// This TU owns the separate-window fallback backend and the public facade. The
// in-game swapchain-hook backend lives in SwapchainHook.cpp; both share the
// panel-drawing members and the visibility/toast state on OverlayImpl.
//
// STEAMEMU_ENABLE_OVERLAY selects between the real Win32 + D3D11 + Dear ImGui
// implementation and a headless no-op. All ImGui/Win32 usage is confined to the
// overlay TUs.
//============================================================================
#include "overlay/overlay.h"

#include "emu_common.h"

#ifdef STEAMEMU_ENABLE_OVERLAY

#include "overlay/overlay_internal.h"

#include "config.h"
#include "net.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

// Declared by imgui_impl_win32.h; forwards raw window messages into ImGui.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace emu {
namespace {

// Reads a boolean-ish env var ("1"/nonzero-first-char => true). Matches the
// getenv handling used elsewhere so the overlay's automation hooks follow the
// existing env-var convention.
bool OverlayEnvFlag(const char* name) {
#if defined(_MSC_VER)
	size_t len = 0;
	char buf[8] = {};
	if (getenv_s(&len, buf, sizeof(buf), name) != 0 || len == 0) return false;
	return buf[0] != '0';
#else
	const char* v = std::getenv(name);
	return v != nullptr && v[0] != '0';
#endif
}

// Parse a hotkey string like "shift+tab", "ctrl+shift+f7", "f12", "alt+o" into a
// Win32 virtual-key + modifier flags. Returns false if no non-modifier key was
// found (the overlay then has no toggle shortcut). Case-insensitive; separators
// are '+', '-', or whitespace.
bool ParseHotkey(const std::string& spec, int& vk, bool& shift, bool& ctrl, bool& alt) {
	vk = 0; shift = ctrl = alt = false;
	std::string tok;
	auto flush = [&]() -> bool {
		if (tok.empty()) return true;
		if (tok == "shift") { shift = true; }
		else if (tok == "ctrl" || tok == "control") { ctrl = true; }
		else if (tok == "alt") { alt = true; }
		else if (tok == "tab") { vk = VK_TAB; }
		else if (tok == "space" || tok == "spacebar") { vk = VK_SPACE; }
		else if (tok == "esc" || tok == "escape") { vk = VK_ESCAPE; }
		else if (tok == "enter" || tok == "return") { vk = VK_RETURN; }
		else if (tok == "backspace") { vk = VK_BACK; }
		else if (tok == "ins" || tok == "insert") { vk = VK_INSERT; }
		else if (tok == "del" || tok == "delete") { vk = VK_DELETE; }
		else if (tok == "home") { vk = VK_HOME; }
		else if (tok == "end") { vk = VK_END; }
		else if (tok.size() >= 2 && tok[0] == 'f' && std::isdigit((unsigned char)tok[1])) {
			int n = std::atoi(tok.c_str() + 1);
			if (n >= 1 && n <= 24) vk = VK_F1 + (n - 1);
		}
		else if (tok.size() == 1 && std::isalnum((unsigned char)tok[0])) {
			vk = std::toupper((unsigned char)tok[0]);  // 'A'-'Z' / '0'-'9' == their vk
		}
		tok.clear();
		return true;
	};
	for (char c : spec) {
		if (c == '+' || c == '-' || std::isspace((unsigned char)c)) flush();
		else tok.push_back((char)std::tolower((unsigned char)c));
	}
	flush();
	return vk != 0;
}

LRESULT WINAPI OverlayWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
	auto* self = reinterpret_cast<OverlayImpl*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
	switch (msg) {
	case WM_SIZE:
		if (self != nullptr && wParam != SIZE_MINIMIZED) {
			self->ResizeW = LOWORD(lParam);
			self->ResizeH = HIWORD(lParam);
		}
		return 0;
	case WM_CLOSE:
		// Closing the window hides the overlay rather than tearing it down; the
		// lifecycle pump learns of the change and fires GameOverlayActivated_t.
		if (self != nullptr) self->RequestHide();
		return 0;
	case WM_SYSCOMMAND:
		if ((wParam & 0xFFF0) == SC_KEYMENU) return 0;  // swallow the Alt menu
		break;
	}
	return DefWindowProcW(hWnd, msg, wParam, lParam);
}

} // namespace

// --- separate-window device management --------------------------------------

bool OverlayImpl::CreateDevice() {
	DXGI_SWAP_CHAIN_DESC sd = {};
	sd.BufferCount = 2;
	sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.BufferDesc.RefreshRate.Numerator = 60;
	sd.BufferDesc.RefreshRate.Denominator = 1;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.OutputWindow = Hwnd;
	sd.SampleDesc.Count = 1;
	sd.Windowed = TRUE;
	sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

	const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
	D3D_FEATURE_LEVEL got;
	HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
		levels, 2, D3D11_SDK_VERSION, &sd, &Swapchain, &Device, &got, &Context);
	if (hr == DXGI_ERROR_UNSUPPORTED) {
		hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
			levels, 2, D3D11_SDK_VERSION, &sd, &Swapchain, &Device, &got, &Context);
	}
	if (FAILED(hr)) return false;
	CreateRtv();
	return true;
}

void OverlayImpl::CreateRtv() {
	ID3D11Texture2D* back = nullptr;
	if (SUCCEEDED(Swapchain->GetBuffer(0, IID_PPV_ARGS(&back))) && back != nullptr) {
		Device->CreateRenderTargetView(back, nullptr, &Rtv);
		back->Release();
	}
}

void OverlayImpl::CleanupRtv() {
	if (Rtv) { Rtv->Release(); Rtv = nullptr; }
}

void OverlayImpl::CleanupDevice() {
	CleanupRtv();
	if (Swapchain) { Swapchain->Release(); Swapchain = nullptr; }
	if (Context) { Context->Release(); Context = nullptr; }
	if (Device) { Device->Release(); Device = nullptr; }
}

// --- shared helpers ---------------------------------------------------------

void OverlayImpl::PollHotkey() {
	const int vkKey = Vk.load();
	if (vkKey == 0) { HotkeyWasDown = false; return; }
	auto down = [](int v) { return (GetAsyncKeyState(v) & 0x8000) != 0; };
	bool ok = down(vkKey);
	if (ModShift.load()) ok = ok && down(VK_SHIFT);
	if (ModCtrl.load()) ok = ok && down(VK_CONTROL);
	if (ModAlt.load()) ok = ok && down(VK_MENU);
	if (ok && !HotkeyWasDown) RequestToggle();  // rising edge only
	HotkeyWasDown = ok;
}

// --- shared panel drawing ---------------------------------------------------

void OverlayImpl::DrawFriendsPanel(bool inGame) {
	const ImGuiIO& io = ImGui::GetIO();
	if (inGame) {
		// Draw a translucent, movable panel over the game rather than covering
		// the whole frame.
		ImGui::SetNextWindowPos(ImVec2(40, 40), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSize(ImVec2(420, 520), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowBgAlpha(0.92f);
		ImGui::Begin("steamemu Overlay", nullptr, ImGuiWindowFlags_NoCollapse);
	} else {
		ImGui::SetNextWindowPos(ImVec2(0, 0));
		ImGui::SetNextWindowSize(io.DisplaySize);
		ImGui::Begin("steamemu Overlay", nullptr,
			ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBringToFrontOnFocus);
	}

	ImGui::TextUnformatted("steamemu - Steam Overlay");
	ImGui::Separator();
	const CSteamID me = emu::LocalSteamID();
	ImGui::Text("You: %s", emu::PersonaName());
	ImGui::SameLine();
	ImGui::TextDisabled("[%llu]", (unsigned long long)me.ConvertToUint64());
	ImGui::Spacing();

	// Discovered LAN peers, surfaced as the friends/players-in-game list.
	ImGui::TextUnformatted("Players in-game (LAN):");
	const std::vector<CSteamID> peers = emu::Net().Peers();
	if (peers.empty()) {
		ImGui::TextDisabled("   (no one discovered yet)");
	}
	for (const CSteamID p : peers) {
		const char* name = emu::Net().PeerNameC(p);
		ImGui::BulletText("%s", (name && *name) ? name : "(unknown)");
		ImGui::SameLine();
		ImGui::TextDisabled("[%llu]", (unsigned long long)p.ConvertToUint64());
	}

	// Known lobbies (create/join/list backing ISteamMatchmaking).
	ImGui::Spacing();
	ImGui::Separator();
	ImGui::TextUnformatted("Lobbies:");
	const std::vector<CSteamID> lobbies = emu::Net().KnownLobbies();
	if (lobbies.empty()) {
		ImGui::TextDisabled("   (none listed)");
	}
	for (const CSteamID l : lobbies) {
		const std::vector<CSteamID> members = emu::Net().LobbyMembers(l);
		const int maxMembers = emu::Net().LobbyMaxMembers(l);
		const char* nm = emu::Net().GetLobbyDataC(l, "name");
		ImGui::BulletText("%s  (%d/%d)", (nm && *nm) ? nm : "lobby",
			(int)members.size(), maxMembers > 0 ? maxMembers : (int)members.size());
	}

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::TextDisabled("Toggle with the configured hotkey.");

	ImGui::End();
}

void OverlayImpl::DrawToasts() {
	std::vector<std::string> active;
	{
		const auto now = std::chrono::steady_clock::now();
		std::lock_guard<std::mutex> lock(ToastMutex);
		Toasts.erase(std::remove_if(Toasts.begin(), Toasts.end(),
			[&](const Toast& t) { return t.Expiry <= now; }), Toasts.end());
		for (const Toast& t : Toasts) active.push_back(t.Text);
	}
	if (active.empty()) return;

	const ImGuiIO& io = ImGui::GetIO();
	ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 12.0f, io.DisplaySize.y - 12.0f),
		ImGuiCond_Always, ImVec2(1.0f, 1.0f));
	ImGui::SetNextWindowBgAlpha(0.85f);
	ImGui::Begin("##toasts", nullptr,
		ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize |
		ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
	for (const std::string& t : active) ImGui::TextUnformatted(t.c_str());
	ImGui::End();
}

// --- separate-window render loop --------------------------------------------

void OverlayImpl::RenderFrame() {
	ImGui_ImplDX11_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();

	DrawFriendsPanel(false);
	DrawToasts();

	ImGui::Render();
	const float clear[4] = { 0.06f, 0.07f, 0.09f, 1.0f };
	Context->OMSetRenderTargets(1, &Rtv, nullptr);
	Context->ClearRenderTargetView(Rtv, clear);
	ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
	Swapchain->Present(1, 0);  // vsync -> ~60fps while visible
}

void OverlayImpl::ThreadMain() {
	const wchar_t* className = L"SteamEmuOverlayWindow";
	WNDCLASSEXW wc = {};
	wc.cbSize = sizeof(wc);
	wc.style = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc = OverlayWndProc;
	wc.hInstance = GetModuleHandleW(nullptr);
	wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
	wc.lpszClassName = className;
	RegisterClassExW(&wc);

	Hwnd = CreateWindowExW(0, className, L"steamemu Overlay", WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT, CW_USEDEFAULT, 460, 580, nullptr, nullptr, wc.hInstance, nullptr);
	if (Hwnd == nullptr) {
		EMU_ERROR("overlay: CreateWindow failed");
		UnregisterClassW(className, wc.hInstance);
		Running.store(false);
		return;
	}
	SetWindowLongPtrW(Hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

	if (!CreateDevice()) {
		EMU_ERROR("overlay: D3D11 device creation failed; overlay disabled");
		CleanupDevice();
		DestroyWindow(Hwnd);
		Hwnd = nullptr;
		UnregisterClassW(className, wc.hInstance);
		Running.store(false);
		return;
	}

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.IniFilename = nullptr;   // don't litter the game's directory with imgui.ini
	io.MouseDrawCursor = true;  // same software cursor as the in-game backend
	ImGui::StyleColorsDark();
	ImGui_ImplWin32_Init(Hwnd);
	ImGui_ImplDX11_Init(Device, Context);

	ShowWindow(Hwnd, SW_HIDE);
	bool shown = false;

	while (Running.load()) {
		MSG msg;
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}

		PollHotkey();

		const bool visible = WantVisible.load();
		if (visible != shown) {
			ShowWindow(Hwnd, visible ? SW_SHOW : SW_HIDE);
			if (visible) SetForegroundWindow(Hwnd);
			shown = visible;
		}
		Exclusive.store(visible && GetForegroundWindow() == Hwnd);

		if (!visible) {
			std::this_thread::sleep_for(std::chrono::milliseconds(30));
			continue;
		}

		if (ResizeW != 0 && ResizeH != 0) {
			CleanupRtv();
			Swapchain->ResizeBuffers(0, ResizeW, ResizeH, DXGI_FORMAT_UNKNOWN, 0);
			ResizeW = ResizeH = 0;
			CreateRtv();
		}

		RenderFrame();
	}

	ImGui_ImplDX11_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
	CleanupDevice();
	if (Hwnd) { DestroyWindow(Hwnd); Hwnd = nullptr; }
	UnregisterClassW(className, wc.hInstance);
}

// --- facade -----------------------------------------------------------------

OverlayManager& OverlayManager::Get() {
	static OverlayManager s;
	return s;
}

OverlayManager::OverlayManager() {
	Impl_ = new OverlayImpl();
}

OverlayManager::~OverlayManager() {
	Stop();
	delete Impl_;
	Impl_ = nullptr;
}

void OverlayManager::Start() {
	if (Impl_ == nullptr) return;
	if (!Config().OverlayEnabled()) return;                         // opt-in
	if (Impl_->HookActive.load() || Impl_->Running.load()) return;  // already up
	// A prior windowed run whose init failed cleared Running itself but left the
	// thread joinable; reap it before assigning a new one.
	if (Impl_->Thread.joinable()) Impl_->Thread.join();

	// Configure the toggle hotkey from steamemu.ini ([overlay] hotkey).
	int vk = 0; bool shift = false, ctrl = false, alt = false;
	if (ParseHotkey(Config().OverlayHotkey(), vk, shift, ctrl, alt)) {
		Impl_->Vk.store(vk);
		Impl_->ModShift.store(shift);
		Impl_->ModCtrl.store(ctrl);
		Impl_->ModAlt.store(alt);
	} else {
		EMU_WARN("overlay: unparsable hotkey '%s'; no toggle shortcut set",
		         Config().OverlayHotkey().c_str());
	}

	// Prefer the in-game swapchain hook so the overlay draws over the game.
	// STEAMEMU_OVERLAY_WINDOW forces the separate-window fallback, handy for
	// headless testing where no game swapchain exists to draw into.
	if (!OverlayEnvFlag("STEAMEMU_OVERLAY_WINDOW") && Impl_->TryStartHook()) {
		EMU_INFO("overlay: in-game swapchain hook installed (toggle '%s')",
		         Config().OverlayHotkey().c_str());
		return;
	}

	Impl_->Running.store(true);
	Impl_->Thread = std::thread([this] { Impl_->ThreadMain(); });
	EMU_INFO("overlay: separate-window overlay started (toggle '%s')",
	         Config().OverlayHotkey().c_str());
}

void OverlayManager::Stop() {
	if (Impl_ == nullptr) return;
	// Unhook first so the game's Present stops entering our code before we free
	// the ImGui backend it renders through.
	if (Impl_->HookActive.load()) Impl_->StopHook();
	Impl_->Running.store(false);
	// Join unconditionally: if ThreadMain failed its window/device init it
	// cleared Running itself, and deleting a still-joinable std::thread calls
	// std::terminate.
	if (Impl_->Thread.joinable()) Impl_->Thread.join();
}

bool OverlayManager::Available() const {
	return Impl_ != nullptr && (Impl_->HookActive.load() || Impl_->Running.load());
}

void OverlayManager::SetVisible(bool visible) {
	// Only honor visibility while a backend is actually live and drawing. Firing
	// GameOverlayActivated_t (via the pump) for an overlay that cannot render --
	// disabled, compiled out, or a failed backend -- would pause a game that
	// never gets a matching "overlay closed" to resume it. See Available().
	if (Impl_ && Available()) Impl_->SetWantVisible(visible, false);  // programmatic
}

bool OverlayManager::IsVisible() const {
	return Impl_ != nullptr && Impl_->WantVisible.load();
}

bool OverlayManager::IsExclusiveInput() const {
	return Impl_ != nullptr && Impl_->Exclusive.load();
}

void OverlayManager::Show(const char* dialog) {
	// ActivateGameOverlay* is a no-op unless the overlay is actually up (same
	// soft-lock guard as SetVisible).
	if (Impl_ == nullptr || !Available()) return;
	if (dialog != nullptr && dialog[0] != '\0')
		Impl_->PushToast(std::string("Overlay: ") + dialog);
	Impl_->SetWantVisible(true, false);  // game asked to raise the overlay
}

bool OverlayManager::ConsumeVisibilityChange(bool& outVisible, bool& outUserInitiated) {
	if (Impl_ == nullptr) return false;
	const bool want = Impl_->WantVisible.load();
	if (want == Impl_->LastReported) return false;
	Impl_->LastReported = want;
	outVisible = want;
	outUserInitiated = Impl_->LastUserInitiated.load();
	return true;
}

void OverlayManager::PushToast(const std::string& text) {
	if (Impl_) Impl_->PushToast(text);
}

} // namespace emu

#else // !STEAMEMU_ENABLE_OVERLAY -- headless build: every method is inert.

namespace emu {

struct OverlayImpl {};

OverlayManager& OverlayManager::Get() { static OverlayManager s; return s; }
OverlayManager::OverlayManager() {}
OverlayManager::~OverlayManager() {}
void OverlayManager::Start() {}
void OverlayManager::Stop() {}
bool OverlayManager::Available() const { return false; }
void OverlayManager::SetVisible(bool) {}
bool OverlayManager::IsVisible() const { return false; }
bool OverlayManager::IsExclusiveInput() const { return false; }
void OverlayManager::Show(const char*) {}
bool OverlayManager::ConsumeVisibilityChange(bool&, bool&) { return false; }
void OverlayManager::PushToast(const std::string&) {}

} // namespace emu

#endif
