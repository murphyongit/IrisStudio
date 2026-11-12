// S1iris' panel: IrisStudio's look (the Graphite palette of core/src/UI/Palettes.hpp), drawn over the game
// with Dear ImGui on the game's own Direct3D 11 swap chain.
//
// The game's Present is found from a throwaway swap chain's table (every swap chain of the process shares
// the code) and hooked; the panel is drawn there, on the thread the game draws on. The game's window gets
// a second window procedure in front of its own, which only keeps the messages for the drawing thread.
//
// F1 gives the mouse to the panel (and back).
#include "theater.hpp"

#include <MinHook.h>
#include <d3d11.h>
#include <dxgi.h>

#include <imgui.h>
#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>

#include <atomic>
#include <cfloat>
#include <cmath>
#include <string>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace
{
	// The game puts the pointer back in the middle of its window every frame and keeps it inside the
	// window (SetCursorPos, ClipCursor). While the panel or the free camera has the mouse, those calls of
	// the game's do nothing.
	using set_cursor_pos_t = BOOL(WINAPI*)(int, int);
	using clip_cursor_t = BOOL(WINAPI*)(const RECT*);
	set_cursor_pos_t g_original_set_cursor_pos = nullptr;
	clip_cursor_t g_original_clip_cursor = nullptr;
	std::atomic<bool> g_camera_mouse{false};
	std::atomic<int> g_wheel{0};

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

	BOOL WINAPI hooked_set_cursor_pos(const int x, const int y)
	{
		if (g_mouse || g_camera_mouse)
		{
			return TRUE;
		}

		return g_original_set_cursor_pos(x, y);
	}

	BOOL WINAPI hooked_clip_cursor(const RECT* rect)
	{
		if (g_mouse)
		{
			return g_original_clip_cursor(nullptr);
		}

		return g_original_clip_cursor(rect);
	}

	LRESULT CALLBACK window_proc(const HWND window, const UINT message, const WPARAM wparam, const LPARAM lparam)
	{
		if (message == WM_MOUSEWHEEL)
		{
			g_wheel += GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA;
		}

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

	// ---- IrisStudio's editor (core/src/UI): the Graphite palette, Rubik with Font Awesome's icons, the
	// game's picture at the top left under the menu bar, the side panel with its five tabs to the right
	// and the timeline under both.

	// Font Awesome 6 (solid), the icons the editor uses
	#define ICON_FILM "\xef\x80\x88"
	#define ICON_VIDEO "\xef\x80\xbd"
	#define ICON_SLIDERS "\xef\x87\x9e"
	#define ICON_PERSON "\xef\x86\x83"
	#define ICON_CIRCLE "\xef\x84\x91"
	#define ICON_CAMERA "\xef\x80\xb0"
	#define ICON_LOCK "\xef\x80\xa3"
	#define ICON_LOCK_OPEN "\xef\x8f\x81"
	#define ICON_BACKWARD_STEP "\xef\x81\x88"
	#define ICON_FORWARD_STEP "\xef\x81\x91"
	#define ICON_PLAY "\xef\x81\x8b"
	#define ICON_PAUSE "\xef\x81\x8c"
	#define ICON_FLAG "\xef\x80\xa4"
	#define ICON_TRASH "\xef\x87\xb8"
	#define ICON_PLUS "\x2b"

	struct Palette
	{
		unsigned bg = 0x1E1E1E, surface = 0x252525, raised = 0x303030, border = 0x3A3A3A, text = 0xD6D6D6, muted = 0x8F8F8F,
			accent = 0x2F80D1, warn = 0xD4AA55, bad = 0xD9534F;
	} const P{};

	ImVec4 color(const unsigned rgb, const float alpha = 1.0f)
	{
		return {((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, alpha};
	}

	unsigned mix(const unsigned a, const unsigned b, const float t)
	{
		unsigned out = 0;
		for (int shift = 16; shift >= 0; shift -= 8)
		{
			const float ca = static_cast<float>((a >> shift) & 0xFF), cb = static_cast<float>((b >> shift) & 0xFF);
			out |= static_cast<unsigned>(ca + (cb - ca) * t + 0.5f) << shift;
		}

		return out;
	}

	ImU32 u32(const unsigned rgb, const float alpha = 1.0f)
	{
		return ImGui::ColorConvertFloat4ToU32(color(rgb, alpha));
	}

	// core/src/UI/Theme.cpp's Apply and UIManager.cpp's SetImGuiStyle
	void apply_look(const float font_size)
	{
		const unsigned hover = mix(P.raised, P.text, 0.08f), selected = mix(P.raised, P.accent, 0.30f), held = mix(P.raised, P.accent, 0.40f);
		ImGuiStyle& style = ImGui::GetStyle();
		style = ImGuiStyle();
		style.FramePadding = {font_size * 0.28f, font_size * 0.28f};
		style.WindowPadding = {16, 16};
		style.WindowRounding = 0.0f;
		style.WindowBorderSize = 0.0f;
		ImVec4* c = style.Colors;
		c[ImGuiCol_Text] = color(P.text);
		c[ImGuiCol_TextDisabled] = color(P.muted);
		c[ImGuiCol_WindowBg] = color(P.bg);
		c[ImGuiCol_ChildBg] = color(P.bg, 0.0f);
		c[ImGuiCol_PopupBg] = color(P.surface, 0.98f);
		c[ImGuiCol_Border] = color(P.border);
		c[ImGuiCol_BorderShadow] = color(P.bg, 0.0f);
		c[ImGuiCol_FrameBg] = color(P.surface);
		c[ImGuiCol_FrameBgHovered] = color(mix(P.surface, P.text, 0.07f));
		c[ImGuiCol_FrameBgActive] = color(mix(P.surface, P.text, 0.12f));
		c[ImGuiCol_TitleBg] = color(P.surface);
		c[ImGuiCol_TitleBgActive] = color(P.raised);
		c[ImGuiCol_MenuBarBg] = color(P.surface);
		c[ImGuiCol_ScrollbarBg] = color(P.bg, 0.6f);
		c[ImGuiCol_ScrollbarGrab] = color(P.raised);
		c[ImGuiCol_ScrollbarGrabHovered] = color(hover);
		c[ImGuiCol_ScrollbarGrabActive] = color(held);
		c[ImGuiCol_CheckMark] = color(P.accent);
		c[ImGuiCol_SliderGrab] = color(P.accent);
		c[ImGuiCol_SliderGrabActive] = color(mix(P.accent, 0xFFFFFF, 0.12f));
		c[ImGuiCol_Button] = color(P.raised);
		c[ImGuiCol_ButtonHovered] = color(hover);
		c[ImGuiCol_ButtonActive] = color(held);
		c[ImGuiCol_Header] = color(selected);
		c[ImGuiCol_HeaderHovered] = color(hover);
		c[ImGuiCol_HeaderActive] = color(held);
		c[ImGuiCol_Separator] = color(P.border);
		c[ImGuiCol_SeparatorHovered] = color(P.muted);
		c[ImGuiCol_SeparatorActive] = color(P.accent);
		c[ImGuiCol_TextSelectedBg] = color(P.accent, 0.30f);
	}

	bool resource(const int id, void** data, int* size)
	{
		const HMODULE module = theater_module();
		const HRSRC found = module ? FindResourceA(module, MAKEINTRESOURCEA(id), MAKEINTRESOURCEA(10) /* RT_RCDATA */) : nullptr;
		const HGLOBAL loaded = found ? LoadResource(module, found) : nullptr;
		*data = loaded ? LockResource(loaded) : nullptr;
		*size = found ? static_cast<int>(SizeofResource(module, found)) : 0;
		return *data && *size > 0;
	}

	ImFont* g_bold = nullptr;
	float g_font_size = 0.0f;

	// UIManager::LoadFonts: Rubik with the icons merged in at two thirds of its size
	void load_fonts(const float size)
	{
		ImGuiIO& io = ImGui::GetIO();
		io.Fonts->Clear();
		void *regular = nullptr, *bold = nullptr, *icons = nullptr;
		int regular_size = 0, bold_size = 0, icons_size = 0;
		const bool have = resource(101, &regular, &regular_size) && resource(102, &bold, &bold_size) && resource(103, &icons, &icons_size);
		static const ImWchar ICON_RANGE[] = {0xE005, 0xF8FF, 0};
		auto add = [&](void* data, const int data_size) -> ImFont* {
			ImFontConfig config;
			config.FontDataOwnedByAtlas = false;
			ImFont* font = io.Fonts->AddFontFromMemoryTTF(data, data_size, size, &config);
			ImFontConfig icon_config;
			icon_config.FontDataOwnedByAtlas = false;
			icon_config.MergeMode = true;
			icon_config.PixelSnapH = true;
			icon_config.GlyphMinAdvanceX = size * 2.0f / 3.0f;
			io.Fonts->AddFontFromMemoryTTF(icons, icons_size, size * 2.0f / 3.0f, &icon_config, ICON_RANGE);
			return font;
		};

		if (have)
		{
			add(regular, regular_size);
			g_bold = add(bold, bold_size);
		}
		else
		{
			log("editor: its fonts are not in the DLL; Windows' own is used");
			if (!io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", size))
			{
				io.Fonts->AddFontDefault();
			}

			g_bold = nullptr;
		}

		g_font_size = size;
		apply_look(size);
	}

	void format_time(char* out, const size_t size, const std::uint32_t ms)
	{
		snprintf(out, size, "%u:%02u.%02u", ms / 60000, (ms / 1000) % 60, (ms / 10) % 100);
	}

	enum class Tab { Demo, Camera, Look, Scene, Record };
	Tab g_tab = Tab::Camera;
	std::atomic<bool> g_hidden{false};      // F1: the editor away, the game's picture alone
	std::atomic<bool> g_looking{false};     // the right mouse button held on the picture: the mouse flies the camera
	std::atomic<bool> g_view_hovered{false};
	ID3D11Texture2D* g_copy = nullptr;      // the game's picture, to draw inside the editor
	ID3D11ShaderResourceView* g_copy_view = nullptr;
	ID3D11BlendState* g_opaque = nullptr;

	const struct { CameraMode mode; const char* label; const char* title; bool built; } MODES[] = {
		{CameraMode::FirstPerson, "First person", "First Person Camera", true},
		{CameraMode::ThirdPerson, "Third person", "Third Person Camera", true},
		{CameraMode::Free, "Free", "Free Camera", true},
		{CameraMode::Orbit, "Orbit", "Orbit Camera", false},
		{CameraMode::Dolly, "Dolly", "Dolly Camera", true},
		{CameraMode::Bone, "Bone", "Bone Camera", false}};

	void opaque_callback(const ImDrawList*, const ImDrawCmd*)
	{
		const float factor[4] = {0, 0, 0, 0};
		g_context->OMSetBlendState(g_opaque, factor, 0xFFFFFFFF);
	}

	void not_built(const char* what)
	{
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextDisabled("%s is not built for Advanced Warfare yet.", what);
		ImGui::PopTextWrapPos();
	}

	void draw_menu_bar(const TheaterState& state, const float width)
	{
		if (!ImGui::BeginMenuBar())
		{
			return;
		}

		if (ImGui::BeginMenu("File"))
		{
			if (ImGui::MenuItem("Stop playback", "F10"))
			{
				theater_stop();
			}

			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu("Edit"))
		{
			if (ImGui::MenuItem("Add campath node", "K", false, camera_state().mode == CameraMode::Free))
			{
				camera_add_point();
			}

			if (ImGui::MenuItem("Go to the first campath node", nullptr, false, camera_state().points > 0))
			{
				theater_seek(camera_state().point_ms[0]);
			}

			if (ImGui::MenuItem("Clear campath"))
			{
				camera_clear_points();
			}

			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu("View"))
		{
			if (ImGui::MenuItem("Hide the editor", "F1"))
			{
				g_hidden = true;
			}

			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu("Help"))
		{
			ImGui::TextDisabled("Playback");
			ImGui::TextUnformatted("Space  play and pause\nUp / Down  faster / slower\nLeft / Right  10 s back / on\nF10  stop\nF3  the HUD on and off");
			ImGui::Separator();
			ImGui::TextDisabled("Camera");
			ImGui::TextUnformatted("F2  free camera\nHold the right mouse button on the picture to look\nW A S D  fly,  E / Q  up / down\n"
			                       "Shift / Ctrl  faster / slower\nMouse wheel  roll\nK  add a campath node\nL  dolly camera\nF1  hide the editor");
			ImGui::EndMenu();
		}

		char now[32], right[200];
		format_time(now, sizeof(now), state.position_ms);
		snprintf(right, sizeof(right), "%s  \xe2\x80\xa2  %s", state.name, now);
		char version[48];
		snprintf(version, sizeof(version), "S1iris %.4s", theater_version());
		const float version_width = ImGui::CalcTextSize(version).x, right_width = ImGui::CalcTextSize(right).x;
		ImGui::SameLine(width - version_width - right_width - g_font_size * 3.0f);
		ImGui::TextUnformatted(right);
		ImGui::SameLine(width - version_width - g_font_size);
		ImGui::TextDisabled("%s", version);
		ImGui::EndMenuBar();
	}

	// The game's picture, as large as fits, under the camera's name.
	void draw_view(const CameraState& camera, const ImVec2 size)
	{
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {g_font_size * 0.5f, g_font_size * 0.5f});
		ImGui::BeginChild("view", size, ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
		const char* title = "";
		for (const auto& m : MODES)
		{
			if (m.mode == camera.mode)
			{
				title = m.title;
			}
		}

		ImGui::SetNextItemWidth(g_font_size * 13.0f);
		if (ImGui::BeginCombo("##viewCamera", title))
		{
			for (const auto& m : MODES)
			{
				ImGui::BeginDisabled(!m.built);
				if (ImGui::Selectable(m.title, m.mode == camera.mode))
				{
					camera_set_mode(m.mode);
				}

				ImGui::EndDisabled();
			}

			ImGui::EndCombo();
		}

		const ImVec2 room = ImGui::GetContentRegionAvail();
		const ImVec2 corner = ImGui::GetCursorScreenPos();
		const ImGuiIO& io = ImGui::GetIO();
		const float aspect = io.DisplaySize.y > 0.0f ? io.DisplaySize.x / io.DisplaySize.y : 16.0f / 9.0f;
		ImVec2 picture = {room.x, room.x / aspect};
		if (picture.y > room.y)
		{
			picture = {room.y * aspect, room.y};
		}

		picture = {std::floor(picture.x), std::floor(picture.y)};
		const ImVec2 at = {std::floor(corner.x + (room.x - picture.x) * 0.5f), corner.y};
		ImDrawList* draw = ImGui::GetWindowDrawList();
		if (g_copy_view)
		{
			// as it is: the picture's own alpha is not a transparency
			draw->AddCallback(&opaque_callback, nullptr);
			draw->AddImage(reinterpret_cast<ImTextureID>(g_copy_view), at, {at.x + picture.x, at.y + picture.y});
			draw->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
		}

		ImGui::SetCursorScreenPos(at);
		ImGui::InvisibleButton("picture", picture, ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonLeft);
		const bool hovered = ImGui::IsItemHovered();
		g_view_hovered = hovered;
		if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && camera.mode == CameraMode::Free)
		{
			g_looking = true;
		}

		if (g_looking && (!ImGui::IsMouseDown(ImGuiMouseButton_Right) || camera.mode != CameraMode::Free))
		{
			g_looking = false;
		}

		if (camera.mode == CameraMode::Free && !g_looking)
		{
			const char* hint = "Hold the right mouse button to look  \xe2\x80\xa2  W A S D to fly  \xe2\x80\xa2  K adds a campath node";
			const ImVec2 hint_size = ImGui::CalcTextSize(hint);
			draw->AddText({at.x + picture.x - hint_size.x - g_font_size, at.y + picture.y - hint_size.y - g_font_size * 0.5f}, u32(P.muted), hint);
		}

		ImGui::EndChild();
		ImGui::PopStyleVar();
	}

	bool section(const char* icon, const char* label)
	{
		char text[96];
		snprintf(text, sizeof(text), "%s  %s", icon, label);
		ImGui::PushStyleColor(ImGuiCol_Header, color(P.raised));
		const bool open = ImGui::CollapsingHeader(text, ImGuiTreeNodeFlags_DefaultOpen);
		ImGui::PopStyleColor();
		return open;
	}

	// A row of the panel: the label on the left, the control from 40% across (ImGuiEx's rows).
	void row_label(const char* label)
	{
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(label);
		ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.40f);
		ImGui::SetNextItemWidth(-1.0f);
	}

	void draw_camera_tab(const TheaterState& state, const CameraState& camera)
	{
		if (section(ICON_CAMERA, "Camera"))
		{
			const float spacing = ImGui::GetStyle().ItemSpacing.x;
			const float button = std::floor((ImGui::GetContentRegionAvail().x - spacing * 2.0f) / 3.0f);
			for (int i = 0; i < 6; ++i)
			{
				const auto& m = MODES[i];
				if (i % 3)
				{
					ImGui::SameLine();
				}

				ImGui::BeginDisabled(!m.built);
				ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(m.mode == camera.mode ? ImGuiCol_Header : ImGuiCol_Button));
				if (ImGui::Button(m.label, {button, 0.0f}))
				{
					camera_set_mode(m.mode);
				}

				ImGui::PopStyleColor();
				ImGui::EndDisabled();
				if (!m.built && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				{
					ImGui::SetTooltip("Not built for Advanced Warfare yet");
				}
			}

			float fov = camera.fov;
			row_label("Field of View");
			ImGui::BeginDisabled(camera.mode != CameraMode::Free);
			if (ImGui::SliderFloat("##fov", &fov, 5.0f, 160.0f, "%.1f"))
			{
				camera_set_fov(fov);
			}

			ImGui::EndDisabled();
			if (camera.mode == CameraMode::Free)
			{
				float speed = camera.speed;
				row_label("Fly speed");
				if (ImGui::SliderFloat("##speed", &speed, 50.0f, 3000.0f, "%.0f", ImGuiSliderFlags_Logarithmic))
				{
					camera_set_speed(speed);
				}
			}
		}

		if (section(ICON_PERSON, "Player"))
		{
			bool body = camera.body;
			row_label("Show the body");
			if (ImGui::Checkbox("##body", &body))
			{
				camera_set_body(body);
			}

			ImGui::PushTextWrapPos(0.0f);
			ImGui::TextDisabled("Draws your own soldier (the game's third person) in the Third person, Free and Dolly cameras, in place of the "
			                    "arms and gun. Untested in Advanced Warfare: if the demo stops when it is ticked, say so.");
			ImGui::PopTextWrapPos();
		}

		if (section(ICON_VIDEO, "Campath"))
		{
			ImGui::BeginDisabled(camera.mode != CameraMode::Free);
			if (ImGui::Button(ICON_PLUS "  Add node here"))
			{
				camera_add_point();
			}

			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::BeginDisabled(camera.points == 0);
			if (ImGui::Button(ICON_TRASH "  Clear"))
			{
				camera_clear_points();
			}

			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::BeginDisabled(camera.points == 0);
			if (ImGui::Button(ICON_BACKWARD_STEP "  First node"))
			{
				theater_seek(camera.point_ms[0]);
			}

			ImGui::EndDisabled();
			for (int i = 0; i < camera.points && i < 64; ++i)
			{
				char time[32], label[64];
				format_time(time, sizeof(time), camera.point_ms[i]);
				snprintf(label, sizeof(label), "Node %d      %s", i + 1, time);
				if (ImGui::Selectable(label, false))
				{
					theater_seek(camera.point_ms[i]);
				}
			}

			ImGui::PushTextWrapPos(0.0f);
			if (camera.points < 2)
			{
				ImGui::TextDisabled("A campath needs two nodes: in the free camera, fly somewhere and add a node, let the demo run on, and add another. "
				                    "The dolly camera then follows them.");
			}

			ImGui::PopTextWrapPos();
		}

		(void)state;
	}

	// The HUD's parts, by the game's own settings (the names the engine family uses; they are set
	// through the console). F3 switches the first, which takes all of the HUD with it.
	struct HudPart
	{
		const char* label;
		const char* setting;
		bool shown;
	};
	HudPart g_hud[] = {{"2D Elements", "cg_draw2D", true}, {"Player HUD", "hud_drawHud", true}, {"Crosshair", "cg_drawCrosshair", true},
	                   {"Gun", "cg_drawGun", true}};
	bool g_hud_changed = false;

	void set_hud(HudPart& part, const bool shown)
	{
		part.shown = shown;
		g_hud_changed = true;
		char text[64];
		snprintf(text, sizeof(text), "%s %d", part.setting, shown ? 1 : 0);
		theater_command(text);
	}

	void draw_hud_section()
	{
		if (!section(ICON_SLIDERS, "HUD"))
		{
			return;
		}

		for (HudPart& part : g_hud)
		{
			bool shown = part.shown;
			ImGui::PushID(part.setting);
			row_label(&part == &g_hud[0] ? "2D Elements (F3)" : part.label);
			if (ImGui::Checkbox("##shown", &shown))
			{
				set_hud(part, shown);
			}

			ImGui::PopID();
		}
	}

	void draw_panel(const TheaterState& state, const CameraState& camera, const ImVec2 size)
	{
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {g_font_size * 0.5f, g_font_size * 0.5f});
		ImGui::BeginChild("panel", size, ImGuiChildFlags_AlwaysUseWindowPadding);
		static const struct { Tab tab; const char* icon; const char* label; } TABS[] = {
			{Tab::Demo, ICON_FILM, "Demo"}, {Tab::Camera, ICON_VIDEO, "Camera"}, {Tab::Look, ICON_SLIDERS, "Look"},
			{Tab::Scene, ICON_PERSON, "Scene"}, {Tab::Record, ICON_CIRCLE, "Record"}};
		const ImGuiStyle& style = ImGui::GetStyle();
		const float line = ImGui::GetTextLineHeight();
		const float tab_width = std::floor((ImGui::GetContentRegionAvail().x - style.ItemSpacing.x * 4.0f) / 5.0f);
		const float tab_height = std::floor(line * 2.0f + style.FramePadding.y * 3.0f);
		for (int i = 0; i < 5; ++i)
		{
			const auto& tab = TABS[i];
			if (i)
			{
				ImGui::SameLine();
			}

			const bool selected = g_tab == tab.tab;
			ImGui::PushID(i);
			ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(selected ? ImGuiCol_Header : ImGuiCol_Button));
			if (ImGui::Button("##tab", {tab_width, tab_height}))
			{
				g_tab = tab.tab;
			}

			ImGui::PopStyleColor();
			ImGui::PopID();
			const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
			ImDrawList* draw = ImGui::GetWindowDrawList();
			const ImU32 text = ImGui::GetColorU32(ImGui::IsItemHovered() || selected ? ImGuiCol_Text : ImGuiCol_TextDisabled);
			const ImVec2 icon_size = ImGui::CalcTextSize(tab.icon), label_size = ImGui::CalcTextSize(tab.label);
			const float top = min.y + style.FramePadding.y;
			draw->AddText({std::floor((min.x + max.x - icon_size.x) / 2), std::floor(top)}, text, tab.icon);
			draw->AddText({std::floor((min.x + max.x - label_size.x) / 2), std::floor(top + line + style.FramePadding.y * 0.5f)}, text, tab.label);
			if (selected)
			{
				draw->AddRectFilled({min.x, max.y - 2}, max, ImGui::GetColorU32(ImGuiCol_CheckMark));
			}
		}

		ImGui::Separator();
		ImGui::Spacing();
		switch (g_tab)
		{
		case Tab::Demo:
			if (section(ICON_FILM, "Demo"))
			{
				char length[32];
				format_time(length, sizeof(length), state.length_ms);
				ImGui::TextUnformatted(state.name);
				ImGui::TextDisabled("Map %s  \xe2\x80\xa2  %s", state.map, length);
				if (ImGui::Button("Stop playback"))
				{
					theater_stop();
				}

				ImGui::Spacing();
				not_built("Browsing recordings (F9 at the game's menu plays the newest)");
			}

			break;
		case Tab::Camera:
			draw_camera_tab(state, camera);
			break;
		case Tab::Look:
			draw_hud_section();
			ImGui::Spacing();
			not_built("The rest of the Look tab (sun, fog, depth of field, sky)");
			break;
		case Tab::Scene:
			not_built("The Scene tab (actors, players)");
			break;
		case Tab::Record:
			not_built("The Record tab (capture)");
			break;
		}

		ImGui::EndChild();
		ImGui::PopStyleVar();
	}

	// The timeline: the controls' row, then the ruler with the playhead over the rows of what has keys.
	void draw_timeline(const TheaterState& state, const CameraState& camera, const ImVec2 size)
	{
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {g_font_size * 0.5f, g_font_size * 0.4f});
		ImGui::BeginChild("timeline", size, ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
		const ImGuiStyle& style = ImGui::GetStyle();
		const float frame = ImGui::GetFrameHeight();

		// Freeze time: the demo's time stands still (the camera still flies)
		ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(state.paused ? ImGuiCol_Header : ImGuiCol_Button));
		if (ImGui::Button(state.paused ? ICON_LOCK " Freeze time" : ICON_LOCK_OPEN " Freeze time"))
		{
			theater_pause_toggle();
		}

		ImGui::PopStyleColor();
		ImGui::SameLine(0.0f, g_font_size);
		const ImVec2 square = {frame * 1.5f, frame};
		if (ImGui::Button(ICON_BACKWARD_STEP "##back", square))
		{
			theater_seek(state.position_ms > 1000 ? state.position_ms - 1000 : 0);
		}

		ImGui::SetItemTooltip("One second back");
		ImGui::SameLine();
		if (ImGui::Button(state.paused ? ICON_PLAY "##play" : ICON_PAUSE "##play", square))
		{
			theater_pause_toggle();
		}

		ImGui::SameLine();
		if (ImGui::Button(ICON_FORWARD_STEP "##forward", square))
		{
			theater_seek(state.position_ms + 1000);
		}

		ImGui::SetItemTooltip("One second on");
		ImGui::SameLine(0.0f, g_font_size);
		char now[32], end[32], both[80];
		format_time(now, sizeof(now), state.position_ms);
		format_time(end, sizeof(end), state.length_ms);
		snprintf(both, sizeof(both), "%s / %s", now, end);
		ImGui::PushStyleColor(ImGuiCol_Button, color(P.surface));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, color(P.surface));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, color(P.surface));
		if (g_bold)
		{
			ImGui::PushFont(g_bold);
		}

		ImGui::Button(both);
		if (g_bold)
		{
			ImGui::PopFont();
		}

		ImGui::PopStyleColor(3);
		ImGui::SameLine();
		{
			// IrisStudio's timescale slider: it rests only on the steps
			const float* steps = nullptr;
			const int count = theater_speed_steps(&steps);
			int step = 0;
			for (int i = 0; i < count; ++i)
			{
				if (fabsf(steps[i] - state.speed) < fabsf(steps[step] - state.speed))
				{
					step = i;
				}
			}

			char speed_label[32];
			snprintf(speed_label, sizeof(speed_label), "%.3f", steps[step]);
			ImGui::SetNextItemWidth(g_font_size * 10.0f);
			if (ImGui::SliderInt("##timescale", &step, 0, count - 1, speed_label, ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput))
			{
				theater_set_speed(steps[step]);
			}

			ImGui::SetItemTooltip("Timescale");
		}

		// the ruler and the rows
		ImGui::Spacing();
		const float labels = g_font_size * 15.0f;
		const ImVec2 origin = ImGui::GetCursorScreenPos();
		const float full = ImGui::GetContentRegionAvail().x;
		const float left = origin.x + labels, right = origin.x + full - g_font_size * 0.5f;
		const float length = state.length_ms > 0 ? static_cast<float>(state.length_ms) : 1.0f;
		const float row = frame + style.ItemSpacing.y * 0.5f;
		ImDrawList* draw = ImGui::GetWindowDrawList();
		auto x_of = [&](const float ms) { return left + (right - left) * (ms / length); };

		// a tick every so many seconds that the labels have room
		const float label_room = ImGui::CalcTextSize("0:00").x * 2.2f;
		static const int EVERY[] = {1, 2, 5, 10, 15, 30, 60, 120, 300, 600};
		int every = EVERY[9];
		for (const int candidate : EVERY)
		{
			if ((right - left) * (candidate * 1000.0f / length) >= label_room)
			{
				every = candidate;
				break;
			}
		}

		const float ruler_bottom = origin.y + frame;
		const float rows_bottom = origin.y + size.y;
		for (int s = 0; s * 1000.0f <= length; s += every)
		{
			const float x = std::floor(x_of(s * 1000.0f));
			char text[16];
			snprintf(text, sizeof(text), "%d:%02d", s / 60, s % 60);
			draw->AddLine({x, origin.y + frame * 0.25f}, {x, ruler_bottom}, u32(P.muted));
			draw->AddText({x + 4.0f, origin.y}, u32(P.text), text);
			draw->AddLine({x, ruler_bottom}, {x, rows_bottom}, u32(P.border, 0.5f));
			const float half = std::floor(x_of((s + every * 0.5f) * 1000.0f));
			if (half < right)
			{
				draw->AddLine({half, origin.y + frame * 0.65f}, {half, ruler_bottom}, u32(P.muted));
			}
		}

		// the ruler takes the mouse: where it is pressed or dragged to is gone to when it lets go
		static bool scrubbing = false;
		static float scrub_ms = 0.0f;
		ImGui::SetCursorScreenPos({left, origin.y});
		ImGui::InvisibleButton("ruler", {right - left, frame});
		if (ImGui::IsItemActive())
		{
			const float t = (ImGui::GetIO().MousePos.x - left) / (right - left);
			scrub_ms = (t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t) * length;
			scrubbing = true;
		}
		else if (scrubbing)
		{
			theater_seek(static_cast<std::uint32_t>(scrub_ms));
			scrubbing = false;
		}

		// the rows: Markers, Campath
		float y = ruler_bottom + style.ItemSpacing.y * 0.5f;
		draw->AddRectFilled({left, y}, {right, y + frame}, u32(P.surface));
		draw->AddText({origin.x + g_font_size * 0.5f, y + style.FramePadding.y}, u32(P.muted), ICON_FLAG "  Markers");
		y += row;
		draw->AddRectFilled({left, y}, {right, y + frame}, u32(P.surface));
		char campath[48];
		snprintf(campath, sizeof(campath), ICON_VIDEO "  Campath (%d)", camera.points);
		draw->AddText({origin.x + g_font_size * 0.5f, y + style.FramePadding.y}, u32(P.text), campath);
		ImGui::SetCursorScreenPos({left - frame * 1.2f - style.ItemSpacing.x, y});
		ImGui::BeginDisabled(camera.points == 0);
		if (ImGui::Button(ICON_TRASH "##clearCampath", {frame * 1.2f, frame}))
		{
			camera_clear_points();
		}

		ImGui::EndDisabled();
		const float diamond = frame * 0.28f;
		for (int i = 0; i < camera.points && i < 64; ++i)
		{
			const float x = std::floor(x_of(static_cast<float>(camera.point_ms[i]))), middle = y + frame * 0.5f;
			draw->AddQuadFilled({x, middle - diamond}, {x + diamond, middle}, {x, middle + diamond}, {x - diamond, middle}, u32(P.text));
		}

		// the playhead over all of it
		const float head = std::floor(x_of(scrubbing ? scrub_ms : static_cast<float>(state.position_ms)));
		draw->AddLine({head, origin.y}, {head, rows_bottom}, u32(P.text), 2.0f);
		draw->AddTriangleFilled({head - frame * 0.3f, origin.y}, {head + frame * 0.3f, origin.y}, {head, origin.y + frame * 0.5f}, u32(P.text));
		ImGui::EndChild();
		ImGui::PopStyleVar();
	}

	// core/src/UI/LayoutMath: the panel 26 ems wide, the timeline 14 ems tall, the picture what is left.
	void draw_editor()
	{
		const TheaterState state = theater_state();
		const CameraState camera = camera_state();
		const ImGuiIO& io = ImGui::GetIO();
		ImGui::SetNextWindowPos({0, 0});
		ImGui::SetNextWindowSize(io.DisplaySize);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {0, 0});
		ImGui::Begin("S1iris", nullptr,
		             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar |
		                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar |
		                 ImGuiWindowFlags_NoScrollWithMouse);
		ImGui::PopStyleVar(2);
		draw_menu_bar(state, io.DisplaySize.x);
		const ImVec2 room = ImGui::GetContentRegionAvail();
		const float panel = std::floor(g_font_size * 26.0f), timeline = std::floor(g_font_size * 14.0f);
		const ImVec2 view = {room.x - panel, room.y - timeline};
		const ImVec2 corner = ImGui::GetCursorScreenPos();
		draw_view(camera, view);
		ImGui::SetCursorScreenPos({corner.x + view.x, corner.y});
		draw_panel(state, camera, {panel, view.y});
		ImGui::SetCursorScreenPos({corner.x, corner.y + view.y});
		draw_timeline(state, camera, {room.x, timeline});
		ImDrawList* draw = ImGui::GetWindowDrawList();
		draw->AddLine({corner.x + view.x, corner.y}, {corner.x + view.x, corner.y + view.y}, u32(P.border));
		draw->AddLine({corner.x, corner.y + view.y}, {corner.x + room.x, corner.y + view.y}, u32(P.border));
		ImGui::End();
	}

	bool set_up(IDXGISwapChain* swap_chain)
	{
		if (FAILED(swap_chain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g_device))))
		{
			log("editor: the swap chain has no Direct3D 11 device");
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
		// LayoutMath::GetFontSize
		const float size = std::floor(desc.BufferDesc.Width / 106.0f);
		load_fonts(size < 8.0f ? 8.0f : size);
		ImGui_ImplWin32_Init(g_window);
		ImGui_ImplDX11_Init(g_device, g_context);
		D3D11_BLEND_DESC blend{};
		blend.RenderTarget[0].BlendEnable = FALSE;
		blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		g_device->CreateBlendState(&blend, &g_opaque);
		g_original_window_proc = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&window_proc)));
		log("editor: ready on a %ux%u picture, window %p, font %.0f px", desc.BufferDesc.Width, desc.BufferDesc.Height, g_window, g_font_size);
		return true;
	}

	void set_mouse(const bool on)
	{
		if (g_mouse == on)
		{
			return;
		}

		g_mouse = on;
		if (on)
		{
			if (g_original_clip_cursor)
			{
				g_original_clip_cursor(nullptr);
			}
		}
		else
		{
			g_looking = false;
			std::lock_guard<std::mutex> lock(g_messages_lock);
			g_messages.clear();
		}

		log("editor: %s", on ? "shown" : "away");
	}

	DXGI_FORMAT typeless_of(const DXGI_FORMAT format, DXGI_FORMAT* plain)
	{
		switch (format)
		{
		case DXGI_FORMAT_R8G8B8A8_UNORM:
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
			*plain = DXGI_FORMAT_R8G8B8A8_UNORM;
			return DXGI_FORMAT_R8G8B8A8_TYPELESS;
		case DXGI_FORMAT_B8G8R8A8_UNORM:
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
			*plain = DXGI_FORMAT_B8G8R8A8_UNORM;
			return DXGI_FORMAT_B8G8R8A8_TYPELESS;
		case DXGI_FORMAT_R10G10B10A2_UNORM:
			*plain = DXGI_FORMAT_R10G10B10A2_UNORM;
			return DXGI_FORMAT_R10G10B10A2_TYPELESS;
		default:
			*plain = format;
			return format;
		}
	}

	// The target to draw on and, for the editor, a copy of what the game drew.
	bool targets(IDXGISwapChain* swap_chain, const bool copy)
	{
		ID3D11Texture2D* back = nullptr;
		if (FAILED(swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))) || !back)
		{
			return false;
		}

		D3D11_TEXTURE2D_DESC texture{};
		back->GetDesc(&texture);
		DXGI_FORMAT plain = texture.Format;
		const DXGI_FORMAT typeless = typeless_of(texture.Format, &plain);
		if (!g_target)
		{
			// the picture may be an sRGB one; the editor's colours are meant as they are
			D3D11_RENDER_TARGET_VIEW_DESC view{};
			view.ViewDimension = texture.SampleDesc.Count > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
			view.Format = plain;
			if (FAILED(g_device->CreateRenderTargetView(back, &view, &g_target)))
			{
				g_device->CreateRenderTargetView(back, nullptr, &g_target);
			}
		}

		if (copy && !g_copy)
		{
			D3D11_TEXTURE2D_DESC wanted = texture;
			wanted.Format = typeless;
			wanted.SampleDesc.Count = 1;
			wanted.SampleDesc.Quality = 0;
			wanted.Usage = D3D11_USAGE_DEFAULT;
			wanted.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			wanted.CPUAccessFlags = 0;
			wanted.MiscFlags = 0;
			wanted.MipLevels = 1;
			wanted.ArraySize = 1;
			if (SUCCEEDED(g_device->CreateTexture2D(&wanted, nullptr, &g_copy)) && g_copy)
			{
				D3D11_SHADER_RESOURCE_VIEW_DESC view{};
				view.Format = plain;
				view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
				view.Texture2D.MipLevels = 1;
				g_device->CreateShaderResourceView(g_copy, &view, &g_copy_view);
			}

			if (!g_copy_view)
			{
				log("editor: no copy of the game's picture could be made (format %d)", static_cast<int>(texture.Format));
			}
		}

		if (copy && g_copy)
		{
			if (texture.SampleDesc.Count > 1)
			{
				g_context->ResolveSubresource(g_copy, 0, back, 0, plain);
			}
			else
			{
				g_context->CopyResource(g_copy, back);
			}
		}

		back->Release();
		return g_target != nullptr;
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
		const bool f1 = GetForegroundWindow() == g_window && (GetAsyncKeyState(VK_F1) & 0x8000) != 0 && !theater_keys_blocked();
		if (f1 && !g_f1_was_down && state.playing)
		{
			g_hidden = !g_hidden;
		}

		g_f1_was_down = f1;
		static bool f3_was_down = false;
		const bool f3 = GetForegroundWindow() == g_window && (GetAsyncKeyState(VK_F3) & 0x8000) != 0 && !theater_keys_blocked();
		if (f3 && !f3_was_down && state.playing)
		{
			set_hud(g_hud[0], !g_hud[0].shown);
		}

		f3_was_down = f3;
		if (!state.playing)
		{
			g_hidden = false;
			camera_playback_ended();
			if (g_hud_changed)
			{
				// the game's own HUD again outside playback
				g_hud_changed = false;
				for (HudPart& part : g_hud)
				{
					part.shown = true;
					char text[64];
					snprintf(text, sizeof(text), "%s 1", part.setting);
					theater_command(text);
				}
			}
		}

		const bool shown = state.playing && !g_hidden;
		set_mouse(shown);
		if (!shown || !targets(swap_chain, true))
		{
			return;
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

		ImGui::GetIO().MouseDrawCursor = !g_looking;
		ImGui_ImplDX11_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();
		draw_editor();
		ImGui::Render();
		g_context->OMSetRenderTargets(1, &g_target, nullptr);
		const ImVec4 background = color(P.bg);
		g_context->ClearRenderTargetView(g_target, &background.x);
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

		if (g_copy_view)
		{
			g_copy_view->Release();
			g_copy_view = nullptr;
		}

		if (g_copy)
		{
			g_copy->Release();
			g_copy = nullptr;
		}

		return g_original_resize(swap_chain, count, width, height, format, flags);
	}
}

HWND ui_window()
{
	return g_window;
}

bool ui_mouse_is_panels()
{
	// with the editor shown the mouse is its own, but for the right button held on the picture; with it
	// away the mouse flies the camera
	return g_mouse && !g_looking;
}

void ui_camera_has_mouse(const bool on)
{
	g_camera_mouse = on;
}

void ui_take_mouse_motion(int* dx, int* dy)
{
	*dx = *dy = 0;
	RECT rect{};
	POINT now{};
	if (!g_window || !g_original_set_cursor_pos || !GetClientRect(g_window, &rect) || !GetCursorPos(&now))
	{
		return;
	}

	POINT middle{(rect.right - rect.left) / 2, (rect.bottom - rect.top) / 2};
	ClientToScreen(g_window, &middle);
	*dx = now.x - middle.x;
	*dy = now.y - middle.y;
	if (*dx || *dy)
	{
		g_original_set_cursor_pos(middle.x, middle.y);
	}
}

int ui_take_mouse_wheel()
{
	// the wheel over the game's picture is the camera's; elsewhere it scrolls the editor
	const int wheel = g_wheel.exchange(0);
	return !g_mouse || g_looking || g_view_hovered ? wheel : 0;
}

void ui_start()
{
	if (HMODULE user32 = GetModuleHandleA("user32.dll"))
	{
		void* set_pos = reinterpret_cast<void*>(GetProcAddress(user32, "SetCursorPos"));
		void* clip = reinterpret_cast<void*>(GetProcAddress(user32, "ClipCursor"));
		const bool hooked = set_pos && clip &&
			MH_CreateHook(set_pos, reinterpret_cast<LPVOID>(&hooked_set_cursor_pos), reinterpret_cast<LPVOID*>(&g_original_set_cursor_pos)) == MH_OK &&
			MH_CreateHook(clip, reinterpret_cast<LPVOID>(&hooked_clip_cursor), reinterpret_cast<LPVOID*>(&g_original_clip_cursor)) == MH_OK &&
			MH_EnableHook(set_pos) == MH_OK && MH_EnableHook(clip) == MH_OK;
		log("panel: the game's hold on the pointer %s", hooked ? "can be let go" : "can NOT be let go");
	}

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

		log(status == MH_OK ? "editor: drawing hooked (%p); the editor shows while a recording plays, F1 puts it away"
		                    : "editor: drawing NOT hooked (%p)",
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
