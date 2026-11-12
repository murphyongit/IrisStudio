// S1iris' panel: IrisStudio's look (the Graphite palette of core/src/UI/Palettes.hpp), drawn over the game
// with Dear ImGui on the game's own Direct3D 11 swap chain.
//
// The game's Present is found from a throwaway swap chain's table (every swap chain of the process shares
// the code) and hooked; the panel is drawn there, on the thread the game draws on. The game's window gets
// a second window procedure in front of its own, which only keeps the messages for the drawing thread.
//
// F1 gives the mouse to the panel (and back): the game is told a menu has the keys (its "key catchers"),
// which makes it let go of the mouse.
#include "theater.hpp"

#include <MinHook.h>
#include <d3d11.h>
#include <dxgi.h>

#include <imgui.h>
#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>

#include <atomic>
#include <cfloat>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace
{
	constexpr uintptr_t KEY_CATCHERS = 0x1417E168C;
	constexpr int KEYCATCH_UI = 0x10;

	using present_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
	using resize_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
	present_t g_original_present = nullptr;
	resize_t g_original_resize = nullptr;

	ID3D11Device* g_device = nullptr;
	ID3D11DeviceContext* g_context = nullptr;
	ID3D11RenderTargetView* g_target = nullptr;
	IDXGISwapChain* g_swap_chain = nullptr;
	HWND g_window = nullptr;
	WNDPROC g_original_window_proc = nullptr;
	bool g_ready = false, g_failed = false;
	std::atomic<bool> g_mouse{false};
	bool g_f1_was_down = false;

	struct Message
	{
		HWND window;
		UINT message;
		WPARAM wparam;
		LPARAM lparam;
	};
	std::mutex g_messages_lock;
	std::vector<Message> g_messages;

	void log(const char* format, ...)
	{
		char text[512];
		va_list args;
		va_start(args, format);
		vsnprintf(text, sizeof(text), format, args);
		va_end(args);
		theater_log(text);
	}

	LRESULT CALLBACK window_proc(const HWND window, const UINT message, const WPARAM wparam, const LPARAM lparam)
	{
		if (g_mouse)
		{
			std::lock_guard<std::mutex> lock(g_messages_lock);
			if (g_messages.size() < 4096)
			{
				g_messages.push_back({window, message, wparam, lparam});
			}
		}

		return CallWindowProcA(g_original_window_proc, window, message, wparam, lparam);
	}

	ImVec4 color(const unsigned rgb, const float alpha = 1.0f)
	{
		return {((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, alpha};
	}

	void apply_look(const float scale)
	{
		// Graphite: bg, surface, raised, border, text, muted, accent
		const unsigned bg = 0x1E1E1E, surface = 0x252525, raised = 0x303030, border = 0x3A3A3A, text = 0xD6D6D6, muted = 0x8F8F8F,
			accent = 0x2F80D1, hover = 0x414141, held = 0x30506F;
		ImGuiStyle& style = ImGui::GetStyle();
		style = ImGuiStyle();
		style.WindowRounding = 6.0f;
		style.FrameRounding = 4.0f;
		style.GrabRounding = 4.0f;
		style.WindowBorderSize = 1.0f;
		style.WindowPadding = {12.0f, 10.0f};
		style.FramePadding = {10.0f, 5.0f};
		style.ItemSpacing = {8.0f, 6.0f};
		style.GrabMinSize = 12.0f;
		style.ScaleAllSizes(scale);
		ImVec4* c = style.Colors;
		c[ImGuiCol_Text] = color(text);
		c[ImGuiCol_TextDisabled] = color(muted);
		c[ImGuiCol_WindowBg] = color(bg, 0.94f);
		c[ImGuiCol_PopupBg] = color(surface, 0.98f);
		c[ImGuiCol_Border] = color(border);
		c[ImGuiCol_FrameBg] = color(surface);
		c[ImGuiCol_FrameBgHovered] = color(0x323232);
		c[ImGuiCol_FrameBgActive] = color(0x3A3A3A);
		c[ImGuiCol_SliderGrab] = color(accent);
		c[ImGuiCol_SliderGrabActive] = color(0x4890D7);
		c[ImGuiCol_Button] = color(raised);
		c[ImGuiCol_ButtonHovered] = color(hover);
		c[ImGuiCol_ButtonActive] = color(held);
		c[ImGuiCol_Separator] = color(border);
		c[ImGuiCol_CheckMark] = color(accent);
	}

	void format_time(char* out, const size_t size, const std::uint32_t ms)
	{
		snprintf(out, size, "%u:%02u.%u", ms / 60000, (ms / 1000) % 60, (ms / 100) % 10);
	}

	// The bar along the bottom while a recording plays: where it is, and the same controls as the keys.
	void draw_bar()
	{
		const TheaterState state = theater_state();
		if (!state.playing)
		{
			return;
		}

		const ImGuiIO& io = ImGui::GetIO();
		const float width = io.DisplaySize.x * 0.62f;
		ImGui::SetNextWindowPos({io.DisplaySize.x * 0.5f, io.DisplaySize.y - 24.0f}, ImGuiCond_Always, {0.5f, 1.0f});
		ImGui::SetNextWindowSize({width, 0.0f});
		ImGui::Begin("S1iris", nullptr,
		             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
		                 ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);

		static float dragged = 0.0f;
		static bool dragging = false;
		const float length = state.length_ms > 0 ? state.length_ms / 1000.0f : 1.0f;
		float position = dragging ? dragged : state.position_ms / 1000.0f;
		char now[32], end[32], label[80];
		format_time(now, sizeof(now), static_cast<std::uint32_t>(position * 1000.0f));
		format_time(end, sizeof(end), state.length_ms);
		snprintf(label, sizeof(label), "%s / %s", now, end);
		ImGui::SetNextItemWidth(-1.0f);
		if (ImGui::SliderFloat("##position", &position, 0.0f, length, label, ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput))
		{
			dragged = position;
			dragging = true;
		}

		// going back in a recording starts it over underneath, so only once the mouse lets go
		if (dragging && ImGui::IsItemDeactivated())
		{
			theater_seek(static_cast<std::uint32_t>(dragged * 1000.0f));
			dragging = false;
		}

		if (ImGui::Button("Start"))
		{
			theater_seek(0);
		}

		ImGui::SameLine();
		if (ImGui::Button("-10 s"))
		{
			theater_seek(state.position_ms > 10000 ? state.position_ms - 10000 : 0);
		}

		ImGui::SameLine();
		if (ImGui::Button(state.paused ? "  Play  " : " Pause "))
		{
			theater_pause_toggle();
		}

		ImGui::SameLine();
		if (ImGui::Button("+10 s"))
		{
			theater_seek(state.position_ms + 10000);
		}

		ImGui::SameLine(0.0f, 24.0f);
		if (ImGui::Button("Slower"))
		{
			theater_slower();
		}

		ImGui::SameLine();
		ImGui::AlignTextToFramePadding();
		ImGui::Text("%gx", state.speed);
		ImGui::SameLine();
		if (ImGui::Button("Faster"))
		{
			theater_faster();
		}

		ImGui::SameLine(0.0f, 24.0f);
		if (ImGui::Button("Stop"))
		{
			theater_stop();
		}

		ImGui::SameLine(0.0f, 24.0f);
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled(g_mouse ? "F1: give the mouse back" : "F1: use the mouse here");
		ImGui::End();
	}

	bool set_up(IDXGISwapChain* swap_chain)
	{
		if (FAILED(swap_chain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g_device))))
		{
			log("panel: the swap chain has no Direct3D 11 device");
			return false;
		}

		DXGI_SWAP_CHAIN_DESC desc{};
		swap_chain->GetDesc(&desc);
		g_window = desc.OutputWindow;
		g_device->GetImmediateContext(&g_context);
		g_swap_chain = swap_chain;

		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
		io.IniFilename = nullptr;
		io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
		const float scale = desc.BufferDesc.Height > 0 ? desc.BufferDesc.Height / 1080.0f : 1.0f;
		if (!io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 18.0f * scale))
		{
			io.Fonts->AddFontDefault();
		}

		apply_look(scale);
		ImGui_ImplWin32_Init(g_window);
		ImGui_ImplDX11_Init(g_device, g_context);
		g_original_window_proc = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&window_proc)));
		log("panel: ready on a %ux%u picture, window %p", desc.BufferDesc.Width, desc.BufferDesc.Height, g_window);
		return true;
	}

	void set_mouse(const bool on)
	{
		g_mouse = on;
		int* catchers = reinterpret_cast<int*>(KEY_CATCHERS);
		if (on)
		{
			*catchers |= KEYCATCH_UI;
		}
		else
		{
			*catchers &= ~KEYCATCH_UI;
			std::lock_guard<std::mutex> lock(g_messages_lock);
			g_messages.clear();
		}

		log("panel: the mouse is %s", on ? "the panel's" : "the game's");
	}

	void draw(IDXGISwapChain* swap_chain)
	{
		if (g_failed)
		{
			return;
		}

		if (!g_ready)
		{
			g_ready = set_up(swap_chain);
			g_failed = !g_ready;
			if (g_failed)
			{
				return;
			}
		}

		if (swap_chain != g_swap_chain)
		{
			return;
		}

		const TheaterState state = theater_state();
		const bool f1 = GetForegroundWindow() == g_window && (GetAsyncKeyState(VK_F1) & 0x8000) != 0;
		if (f1 && !g_f1_was_down && state.playing)
		{
			set_mouse(!g_mouse);
		}

		g_f1_was_down = f1;
		if (g_mouse && !state.playing)
		{
			set_mouse(false);
		}

		if (g_mouse)
		{
			// the game clears it when its own menus close
			*reinterpret_cast<int*>(KEY_CATCHERS) |= KEYCATCH_UI;
		}

		if (!state.playing)
		{
			return;
		}

		if (!g_target)
		{
			ID3D11Texture2D* back = nullptr;
			if (FAILED(swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))) || !back)
			{
				return;
			}

			// the picture may be an sRGB one; the panel's colours are meant as they are
			D3D11_TEXTURE2D_DESC texture{};
			back->GetDesc(&texture);
			D3D11_RENDER_TARGET_VIEW_DESC view{};
			view.ViewDimension = texture.SampleDesc.Count > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
			view.Format = texture.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ? DXGI_FORMAT_R8G8B8A8_UNORM
				: texture.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ? DXGI_FORMAT_B8G8R8A8_UNORM : texture.Format;
			if (FAILED(g_device->CreateRenderTargetView(back, &view, &g_target)))
			{
				g_device->CreateRenderTargetView(back, nullptr, &g_target);
			}

			back->Release();
			if (!g_target)
			{
				return;
			}
		}

		{
			std::vector<Message> messages;
			{
				std::lock_guard<std::mutex> lock(g_messages_lock);
				messages.swap(g_messages);
			}

			for (const Message& m : messages)
			{
				ImGui_ImplWin32_WndProcHandler(m.window, m.message, m.wparam, m.lparam);
			}
		}

		ImGui::GetIO().MouseDrawCursor = g_mouse;
		ImGui_ImplDX11_NewFrame();
		ImGui_ImplWin32_NewFrame();
		if (!g_mouse)
		{
			// the game has the mouse: nothing under it is the panel's
			ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
		}

		ImGui::NewFrame();
		draw_bar();
		ImGui::Render();
		g_context->OMSetRenderTargets(1, &g_target, nullptr);
		ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
	}

	HRESULT STDMETHODCALLTYPE hooked_present(IDXGISwapChain* swap_chain, const UINT sync, const UINT flags)
	{
		if (!(flags & DXGI_PRESENT_TEST))
		{
			draw(swap_chain);
		}

		return g_original_present(swap_chain, sync, flags);
	}

	HRESULT STDMETHODCALLTYPE hooked_resize(IDXGISwapChain* swap_chain, const UINT count, const UINT width, const UINT height,
	                                        const DXGI_FORMAT format, const UINT flags)
	{
		if (g_target)
		{
			g_target->Release();
			g_target = nullptr;
		}

		return g_original_resize(swap_chain, count, width, height, format, flags);
	}
}

void ui_start()
{
	// a swap chain of our own, only to read where the code every swap chain shares is
	WNDCLASSEXA window_class{sizeof(WNDCLASSEXA)};
	window_class.lpfnWndProc = DefWindowProcA;
	window_class.hInstance = GetModuleHandleA(nullptr);
	window_class.lpszClassName = "S1irisProbe";
	RegisterClassExA(&window_class);
	const HWND window = CreateWindowExA(0, window_class.lpszClassName, "", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr,
	                                    window_class.hInstance, nullptr);
	DXGI_SWAP_CHAIN_DESC desc{};
	desc.BufferCount = 1;
	desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	desc.OutputWindow = window;
	desc.SampleDesc.Count = 1;
	desc.Windowed = TRUE;
	IDXGISwapChain* swap_chain = nullptr;
	ID3D11Device* device = nullptr;
	ID3D11DeviceContext* context = nullptr;
	const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
	const HRESULT made = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &desc,
	                                                   &swap_chain, &device, nullptr, &context);
	if (FAILED(made) || !swap_chain)
	{
		log("panel: no swap chain to read (0x%08lX); no panel", static_cast<unsigned long>(made));
	}
	else
	{
		void** table = *reinterpret_cast<void***>(swap_chain);
		void* present = table[8];
		void* resize = table[13];
		MH_STATUS status = MH_CreateHook(present, reinterpret_cast<LPVOID>(&hooked_present), reinterpret_cast<LPVOID*>(&g_original_present));
		if (status == MH_OK)
		{
			status = MH_CreateHook(resize, reinterpret_cast<LPVOID>(&hooked_resize), reinterpret_cast<LPVOID*>(&g_original_resize));
		}

		if (status == MH_OK)
		{
			status = MH_EnableHook(resize);
		}

		if (status == MH_OK)
		{
			status = MH_EnableHook(present);
		}

		log(status == MH_OK ? "panel: drawing hooked (%p); the bar shows while a recording plays, F1 gives it the mouse"
		                    : "panel: drawing NOT hooked (%p)",
		    present);
	}

	if (context)
	{
		context->Release();
	}

	if (swap_chain)
	{
		swap_chain->Release();
	}

	if (device)
	{
		device->Release();
	}

	if (window)
	{
		DestroyWindow(window);
	}

	UnregisterClassA(window_class.lpszClassName, window_class.hInstance);
}
