//====== steamemu ============================================================
// In-game overlay backend: hooks the game's IDXGISwapChain::Present and draws
// the ImGui panel over the game, on the game's own device and render thread.
// See overlay_internal.h for how this shares state with the windowed fallback
// in Overlay.cpp.
//
// Mechanism (the classic vtable-patch injection, as the real overlay does):
//   1. Stand up a throwaway swapchain to read the process-wide CDXGISwapChain
//      vtable, which every swapchain in the process shares.
//   2. Patch the Present (slot 8) and ResizeBuffers (slot 13) entries so the
//      game's calls land in our hooks. One patch covers every swapchain.
//   3. On the first real Present, adopt that swapchain's device/window, stand up
//      the ImGui DX11 backend on it, and hook its WndProc for input.
//   4. Each visible frame: build our draw data and render it onto the game's
//      backbuffer, then call the original Present (we never present ourselves).
//
// Rendering runs on the game's render thread inside the hook. This backend only
// reads OverlayImpl's thread-safe state (visibility, toasts) and pulls live data
// from emu::Net() when it draws.
//============================================================================
#include "emu_common.h"

#ifdef STEAMEMU_ENABLE_OVERLAY

#include "overlay/overlay_internal.h"

#include <chrono>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace emu {

// COM method signatures we intercept. STDMETHODCALLTYPE pins the calling
// convention so the hooks are ABI-identical to the originals.
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

// Full hook state, forward-declared in overlay_internal.h so the windowed path
// never sees DXGI hooking details.
struct HookState {
	OverlayImpl* Impl = nullptr;
	void** Vtable = nullptr;          // the patched CDXGISwapChain vtable
	PresentFn OrigPresent = nullptr;
	ResizeBuffersFn OrigResize = nullptr;

	std::atomic<bool> Shutting{false};
	bool Initialized = false;         // set once we've adopted a swapchain
	IDXGISwapChain* BoundSwapchain = nullptr;
	ID3D11Device* Device = nullptr;
	ID3D11DeviceContext* Context = nullptr;
	ID3D11RenderTargetView* Rtv = nullptr;
	HWND Hwnd = nullptr;
	WNDPROC PrevWndProc = nullptr;
	ImGuiContext* ImCtx = nullptr;

	// Last cursor position pushed to ImGui by the per-frame poll, so we only
	// emit AddMousePosEvent on actual movement.
	POINT LastMouse = {};
	bool HasLastMouse = false;
};

namespace {

// IDXGISwapChain vtable indices (IUnknown 0-2, IDXGIObject 3-6,
// IDXGIDeviceSubObject 7=GetDevice, IDXGISwapChain 8=Present ... 13=ResizeBuffers).
constexpr int kPresentIndex = 8;
constexpr int kResizeBuffersIndex = 13;

// Only one overlay hook can own the shared vtable at a time. These globals let
// the free hook functions reach the active state; the originals stay here too so
// an in-flight Present can still forward while StopHook runs.
std::atomic<HookState*> g_Hook{nullptr};
PresentFn g_OrigPresent = nullptr;
ResizeBuffersFn g_OrigResize = nullptr;

bool PatchSlot(void** vtable, int index, void* hook, void** outOrig) {
	DWORD old = 0;
	if (!VirtualProtect(&vtable[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &old))
		return false;
	if (outOrig) *outOrig = vtable[index];
	vtable[index] = hook;
	VirtualProtect(&vtable[index], sizeof(void*), old, &old);
	return true;
}

void RestoreSlot(void** vtable, int index, void* orig) {
	DWORD old = 0;
	if (VirtualProtect(&vtable[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
		vtable[index] = orig;
		VirtualProtect(&vtable[index], sizeof(void*), old, &old);
	}
}

// Stand up a hidden 1x1 swapchain purely to read its vtable, then release
// everything. The vtable memory belongs to the DXGI module and stays valid after
// release, so the pointer we return is patchable later.
bool DiscoverVtable(void*** outVtable) {
	WNDCLASSEXW wc = {};
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = DefWindowProcW;
	wc.hInstance = GetModuleHandleW(nullptr);
	wc.lpszClassName = L"SteamEmuHookProbe";
	RegisterClassExW(&wc);
	HWND wnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
		0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);

	DXGI_SWAP_CHAIN_DESC sd = {};
	sd.BufferCount = 1;
	sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.OutputWindow = wnd;
	sd.SampleDesc.Count = 1;
	sd.Windowed = TRUE;
	sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

	const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
	IDXGISwapChain* sc = nullptr;
	ID3D11Device* dev = nullptr;
	ID3D11DeviceContext* ctx = nullptr;
	D3D_FEATURE_LEVEL got;
	HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
		levels, 2, D3D11_SDK_VERSION, &sd, &sc, &dev, &got, &ctx);
	if (hr == DXGI_ERROR_UNSUPPORTED)
		hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
			levels, 2, D3D11_SDK_VERSION, &sd, &sc, &dev, &got, &ctx);

	bool ok = false;
	if (SUCCEEDED(hr) && sc != nullptr) {
		*outVtable = *reinterpret_cast<void***>(sc);
		ok = true;
	}
	if (sc) sc->Release();
	if (ctx) ctx->Release();
	if (dev) dev->Release();
	if (wnd) DestroyWindow(wnd);
	UnregisterClassW(wc.lpszClassName, wc.hInstance);
	return ok;
}

void RebuildRtv(HookState* h, IDXGISwapChain* sc) {
	ID3D11Texture2D* back = nullptr;
	if (SUCCEEDED(sc->GetBuffer(0, IID_PPV_ARGS(&back))) && back != nullptr) {
		h->Device->CreateRenderTargetView(back, nullptr, &h->Rtv);
		back->Release();
	}
}

LRESULT CALLBACK HookWndProc(HWND wnd, UINT msg, WPARAM w, LPARAM l) {
	HookState* h = g_Hook.load();
	if (h != nullptr && h->Initialized && h->ImCtx != nullptr && h->Impl->WantVisible.load()) {
		ImGui::SetCurrentContext(h->ImCtx);
		const LRESULT handled = ImGui_ImplWin32_WndProcHandler(wnd, msg, w, l);
		const ImGuiIO& io = ImGui::GetIO();
		switch (msg) {
		case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
		case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
		case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
		case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: case WM_MOUSEMOVE:
			if (io.WantCaptureMouse) return 1;  // swallow: game must not see it
			break;
		case WM_INPUT:
			// Raw-input games read the mouse through WM_INPUT, bypassing the
			// WM_MOUSE* swallows above -- the game keeps steering its own UI
			// underneath the overlay. Divert to DefWindowProc (required for
			// raw-input cleanup) instead of the game while we're capturing.
			if (io.WantCaptureMouse || io.WantCaptureKeyboard)
				return DefWindowProcW(wnd, msg, w, l);
			break;
		case WM_SETCURSOR:
			// The overlay draws a software cursor (MouseDrawCursor); the ImGui
			// handler has already hidden the hardware one. Don't let the game
			// re-set it underneath ours.
			if (handled != 0 || io.WantCaptureMouse) return 1;
			break;
		case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP:
		case WM_CHAR:
			if (io.WantCaptureKeyboard) return 1;
			break;
		}
	}
	WNDPROC prev = (h != nullptr) ? h->PrevWndProc : nullptr;
	if (prev != nullptr) return CallWindowProcW(prev, wnd, msg, w, l);
	return DefWindowProcW(wnd, msg, w, l);
}

// First-Present adoption of the game's swapchain.
bool InitFromSwapchain(HookState* h, IDXGISwapChain* sc) {
	ID3D11Device* dev = nullptr;
	if (FAILED(sc->GetDevice(IID_PPV_ARGS(&dev))) || dev == nullptr)
		return false;  // not a D3D11 swapchain (D3D12/other) -- leave it alone
	ID3D11DeviceContext* ctx = nullptr;
	dev->GetImmediateContext(&ctx);

	DXGI_SWAP_CHAIN_DESC desc = {};
	sc->GetDesc(&desc);
	HWND wnd = desc.OutputWindow;
	if (wnd == nullptr || ctx == nullptr) {
		if (ctx) ctx->Release();
		dev->Release();
		return false;
	}

	h->Device = dev;
	h->Context = ctx;
	h->Hwnd = wnd;
	h->BoundSwapchain = sc;
	RebuildRtv(h, sc);
	if (h->Rtv == nullptr) {
		h->Device = nullptr; h->Context = nullptr; h->Hwnd = nullptr; h->BoundSwapchain = nullptr;
		ctx->Release();
		dev->Release();
		return false;
	}

	IMGUI_CHECKVERSION();
	h->ImCtx = ImGui::CreateContext();
	ImGui::SetCurrentContext(h->ImCtx);
	ImGuiIO& io = ImGui::GetIO();
	io.IniFilename = nullptr;  // don't litter the game's directory
	ImGui::StyleColorsDark();
	ImGui_ImplWin32_Init(wnd);
	ImGui_ImplDX11_Init(dev, ctx);

	h->PrevWndProc = reinterpret_cast<WNDPROC>(
		SetWindowLongPtrW(wnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&HookWndProc)));
	h->Initialized = true;
	EMU_INFO("overlay: captured game swapchain (hwnd %p), rendering in-game", (void*)wnd);
	return true;
}

void RenderOverlay(HookState* h, IDXGISwapChain* sc) {
	ImGui::SetCurrentContext(h->ImCtx);
	OverlayImpl* impl = h->Impl;
	impl->PollHotkey();  // rising-edge toggle, on the render thread

	const bool visible = impl->WantVisible.load();
	ImGuiIO& io = ImGui::GetIO();
	// Games hide the hardware cursor and draw their own; render an ImGui software
	// cursor while the overlay is up so the user has something to point with
	// regardless of what the game did to the OS cursor.
	io.MouseDrawCursor = visible;
	// Exclusive input only while visible AND ImGui actually wants the device --
	// outside that the game keeps full input.
	impl->Exclusive.store(visible && (io.WantCaptureMouse || io.WantCaptureKeyboard));
	if (!visible) {
		h->HasLastMouse = false;
		return;
	}

	// Raw-input games often never generate WM_MOUSEMOVE, so the WndProc hook alone
	// can leave ImGui's mouse position stale. Poll the OS cursor each frame; the
	// game can't hide *position*, only the visible cursor.
	POINT cur = {};
	if (GetCursorPos(&cur) && ScreenToClient(h->Hwnd, &cur)) {
		if (!h->HasLastMouse || cur.x != h->LastMouse.x || cur.y != h->LastMouse.y) {
			io.AddMousePosEvent(static_cast<float>(cur.x), static_cast<float>(cur.y));
			h->LastMouse = cur;
			h->HasLastMouse = true;
		}
	}
	// Release any cursor confinement the game holds, every frame -- games re-assert
	// their clip. On hide we stop interfering and the game's next re-assert
	// restores its own clip.
	ClipCursor(nullptr);

	if (h->Rtv == nullptr) RebuildRtv(h, sc);  // lost after a resize
	if (h->Rtv == nullptr) return;

	ImGui_ImplDX11_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();
	impl->DrawFriendsPanel(true);
	impl->DrawToasts();
	ImGui::Render();

	// Draw onto the game's backbuffer without clearing -- we compose over the
	// already-rendered frame, then let the game present it.
	h->Context->OMSetRenderTargets(1, &h->Rtv, nullptr);
	ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* sc, UINT syncInterval, UINT flags) {
	HookState* h = g_Hook.load();
	PresentFn orig = (h != nullptr) ? h->OrigPresent : g_OrigPresent;
	// DXGI_PRESENT_TEST presents nothing; never render into it.
	if (h != nullptr && !h->Shutting.load() && (flags & DXGI_PRESENT_TEST) == 0) {
		if (!h->Initialized) InitFromSwapchain(h, sc);
		if (h->Initialized && sc == h->BoundSwapchain) RenderOverlay(h, sc);
	}
	if (orig == nullptr) return S_OK;  // fully torn down
	return orig(sc, syncInterval, flags);
}

HRESULT STDMETHODCALLTYPE HookedResizeBuffers(IDXGISwapChain* sc, UINT bufferCount,
	UINT width, UINT height, DXGI_FORMAT newFormat, UINT swapChainFlags) {
	HookState* h = g_Hook.load();
	if (h != nullptr && h->Initialized && sc == h->BoundSwapchain && h->Rtv != nullptr) {
		h->Rtv->Release();  // release our view of the old backbuffer first
		h->Rtv = nullptr;   // RenderOverlay rebuilds it lazily next frame
	}
	ResizeBuffersFn orig = (h != nullptr) ? h->OrigResize : g_OrigResize;
	if (orig == nullptr) return S_OK;
	return orig(sc, bufferCount, width, height, newFormat, swapChainFlags);
}

} // namespace

bool OverlayImpl::TryStartHook() {
	if (g_Hook.load() != nullptr) {
		EMU_WARN("overlay: swapchain hook already owned by another instance");
		return false;
	}
	void** vtable = nullptr;
	if (!DiscoverVtable(&vtable) || vtable == nullptr) {
		EMU_WARN("overlay: could not discover swapchain vtable; using windowed fallback");
		return false;
	}

	HookState* h = new HookState();
	h->Impl = this;
	h->Vtable = vtable;
	g_OrigPresent = reinterpret_cast<PresentFn>(vtable[kPresentIndex]);
	g_OrigResize = reinterpret_cast<ResizeBuffersFn>(vtable[kResizeBuffersIndex]);
	h->OrigPresent = g_OrigPresent;
	h->OrigResize = g_OrigResize;

	if (!PatchSlot(vtable, kPresentIndex, reinterpret_cast<void*>(&HookedPresent), nullptr)) {
		EMU_WARN("overlay: failed to patch Present slot; using windowed fallback");
		g_OrigPresent = nullptr;
		g_OrigResize = nullptr;
		delete h;
		return false;
	}
	PatchSlot(vtable, kResizeBuffersIndex, reinterpret_cast<void*>(&HookedResizeBuffers), nullptr);

	Hook = h;
	g_Hook.store(h);
	HookActive.store(true);
	return true;
}

void OverlayImpl::StopHook() {
	HookState* h = Hook;
	if (h == nullptr) { HookActive.store(false); return; }

	// Stop rendering, then restore the vtable so new Present calls bypass us.
	h->Shutting.store(true);
	g_Hook.store(nullptr);
	if (h->Vtable != nullptr) {
		RestoreSlot(h->Vtable, kPresentIndex, reinterpret_cast<void*>(g_OrigPresent));
		RestoreSlot(h->Vtable, kResizeBuffersIndex, reinterpret_cast<void*>(g_OrigResize));
	}
	// Let any Present already inside our hook on the render thread unwind before
	// we free the ImGui/D3D resources it may still be touching.
	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	if (h->Initialized) {
		if (h->Hwnd != nullptr && h->PrevWndProc != nullptr)
			SetWindowLongPtrW(h->Hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(h->PrevWndProc));
		ImGui::SetCurrentContext(h->ImCtx);
		ImGui_ImplDX11_Shutdown();
		ImGui_ImplWin32_Shutdown();
		ImGui::DestroyContext(h->ImCtx);
		if (h->Rtv) h->Rtv->Release();
		if (h->Context) h->Context->Release();
		if (h->Device) h->Device->Release();
	}

	HookActive.store(false);
	g_OrigPresent = nullptr;
	g_OrigResize = nullptr;
	delete h;
	Hook = nullptr;
	EMU_INFO("overlay: swapchain hook removed");
}

} // namespace emu

#endif // STEAMEMU_ENABLE_OVERLAY
