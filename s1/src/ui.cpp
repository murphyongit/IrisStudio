// S1iris' panel: IrisStudio's look (the Graphite palette of core/src/UI/Palettes.hpp), drawn over the game
// with Dear ImGui on the game's own Direct3D 11 swap chain.
//
// The game's Present is found from a throwaway swap chain's table (every swap chain of the process shares
// the code) and hooked; the panel is drawn there, on the thread the game draws on. The game's window gets
// a second window procedure in front of its own, which only keeps the messages for the drawing thread.
//
// F1 gives the mouse to the panel (and back).
#include "ui_parts.hpp"

#include <MinHook.h>
#include <d3d11.h>
#include <dxgi.h>

#include <imgui.h>
#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <string>
#include <cstdarg>
#include <cstdio>
#include <cctype>
#include <cstring>
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
	std::atomic<bool> g_menu_ui{false}, g_over_menu_ui{false};  // the recordings list at the game's menus
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

		if (g_mouse || g_menu_ui)
		{
			std::lock_guard<std::mutex> lock(g_messages_lock);
			if (g_messages.size() < 4096)
			{
				g_messages.push_back({window, message, wparam, lparam});
			}
		}

		// at the game's menus, a click on the recordings list is not also the game's
		if (g_menu_ui && g_over_menu_ui && message >= WM_LBUTTONDOWN && message <= WM_MOUSEWHEEL)
		{
			return 0;
		}

		return CallWindowProcA(g_original_window_proc, window, message, wparam, lparam);
	}

	// ---- IrisStudio's editor (core/src/UI): the Graphite palette, Rubik with Font Awesome's icons, the
	// game's picture at the top left under the menu bar, the side panel with its five tabs to the right
	// and the timeline under both.

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
	DXGI_FORMAT g_copy_format = DXGI_FORMAT_UNKNOWN;  // the copy's pixels, as a format that says what they are
	std::atomic<bool> g_typing{false};

	const struct { CameraMode mode; const char* label; const char* title; bool built; } MODES[] = {
		{CameraMode::FirstPerson, "First person", "First Person Camera", true},
		{CameraMode::ThirdPerson, "Third person", "Third Person Camera", true},
		{CameraMode::Free, "Free", "Free Camera", true},
		{CameraMode::Orbit, "Orbit", "Orbit Camera", false},
		{CameraMode::Dolly, "Dolly", "Dolly Camera", true},
		{CameraMode::Bone, "Bone", "Bone Camera", true}};

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
			if (ImGui::MenuItem("Recordings..."))
			{
				g_tab = Tab::Demo;
			}

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
			                       "Shift / Ctrl  faster / slower\nMouse wheel  roll,  Shift + wheel  field of view\nK  add a campath node\nL  dolly camera\nF1  hide the editor");
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

	// the effects searched for, and the one last played: the picture's right-click menu offers them too
	char g_fx_search[64] = "", g_fx_found[400][96], g_fx_last[96] = "";

	// An effect put at a place, facing off the surface there.
	void add_effect_at(const char* name, const float point[3], const float normal[3])
	{
		const int index = effects_add(name, point, theater_state().position_ms);
		PlacedEffect placed[32];
		const int count = effects_placed(placed, 32);
		if (index >= 0 && index < count)
		{
			placed[index].angles[0] = std::atan2(-normal[2], std::sqrt(normal[0] * normal[0] + normal[1] * normal[1])) * 57.29578f;
			placed[index].angles[1] = std::atan2(normal[1], normal[0]) * 57.29578f;
			placed[index].angles[2] = 0.0f;
			effects_set(index, placed[index]);
		}

		snprintf(g_fx_last, sizeof(g_fx_last), "%s", name);
	}

	// The menu a right click on the picture opens: things put at the place clicked.
	void draw_place_menu()
	{
		static int found = 0;
		static float point[3] = {}, normal[3] = {0.0f, 0.0f, 1.0f};
		if (ImGui::IsWindowAppearing())
		{
			found = 0;
		}

		if (found == 0)
		{
			found = camera_picked(point, normal);
		}

		if (found == 0)
		{
			ImGui::TextDisabled("Finding the place...");
			return;
		}

		if (found < 0)
		{
			ImGui::TextDisabled("Nothing there (the sky, or the recording is not playing).");
			return;
		}

		ImGui::TextDisabled("%.0f   %.0f   %.0f", point[0], point[1], point[2]);
		ImGui::Separator();
		// (a light a little off the surface, so it lights it)
		const float off[3] = {point[0] + normal[0] * 24.0f, point[1] + normal[1] * 24.0f, point[2] + normal[2] * 24.0f};
		if (ImGui::MenuItem("Add a light here"))
		{
			scene_add_light_at(off);
		}

		SceneLight lights[16];
		const int light_count = scene_lights(lights, 16);
		if (light_count > 0 && ImGui::BeginMenu("Move a light here"))
		{
			for (int i = 0; i < light_count; ++i)
			{
				char label[32];
				snprintf(label, sizeof(label), "Light %d", i + 1);
				if (ImGui::MenuItem(label))
				{
					memcpy(lights[i].origin, off, sizeof(off));
					scene_set_light(i, lights[i]);
				}
			}

			ImGui::EndMenu();
		}

		ImGui::Separator();
		if (g_fx_last[0])
		{
			char label[128];
			snprintf(label, sizeof(label), "Play %s here", g_fx_last);
			if (ImGui::MenuItem(label))
			{
				add_effect_at(g_fx_last, point, normal);
			}
		}

		if (ImGui::BeginMenu("Play an effect here"))
		{
			ImGui::SetNextItemWidth(g_font_size * 22.0f);
			ImGui::InputTextWithHint("##placeSearch", "search (explosion, spark, rain...)", g_fx_search, sizeof(g_fx_search));
			const int count = effects_find(g_fx_search, g_fx_found, 400);
			ImGui::BeginChild("placeResults", {g_font_size * 22.0f, g_font_size * 12.0f});
			ImGuiListClipper clipper;
			clipper.Begin(count);
			bool chosen = false;
			while (clipper.Step())
			{
				for (int f = clipper.DisplayStart; f < clipper.DisplayEnd; ++f)
				{
					if (ImGui::Selectable(g_fx_found[f]))
					{
						add_effect_at(g_fx_found[f], point, normal);
						chosen = true;
					}
				}
			}

			ImGui::EndChild();
			ImGui::EndMenu();
			if (chosen)
			{
				ImGui::CloseCurrentPopup();
			}
		}

		PlacedEffect placed[32];
		const int placed_count = effects_placed(placed, 32);
		if (placed_count > 0 && ImGui::BeginMenu("Move an effect here"))
		{
			for (int i = 0; i < placed_count; ++i)
			{
				ImGui::PushID(i);
				if (ImGui::MenuItem(placed[i].name))
				{
					memcpy(placed[i].origin, point, sizeof(placed[i].origin));
					placed[i].follow = false;
					effects_set(i, placed[i]);
				}

				ImGui::PopID();
			}

			ImGui::EndMenu();
		}
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
		const bool flown = camera.mode == CameraMode::Free || camera.mode == CameraMode::Bone;
		if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && flown)
		{
			g_looking = true;
		}

		if (g_looking && (!ImGui::IsMouseDown(ImGuiMouseButton_Right) || !flown))
		{
			g_looking = false;
		}

		// a right click that is not a drag: the place under it, and what can be put there
		static ImVec2 pressed_at = {};
		static double pressed_when = 0.0;
		static bool pressed = false;
		if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
		{
			pressed_at = io.MousePos;
			pressed_when = ImGui::GetTime();
			pressed = true;
		}

		if (pressed && ImGui::IsMouseReleased(ImGuiMouseButton_Right))
		{
			pressed = false;
			// (told from a look by how short it was: while looking the game has the cursor, and it moves it)
			if (ImGui::GetTime() - pressed_when < 0.25 && picture.x > 0.0f && picture.y > 0.0f)
			{
				camera_pick((pressed_at.x - at.x) / picture.x, (pressed_at.y - at.y) / picture.y);
				ImGui::OpenPopup("place");
			}
		}

		if (ImGui::BeginPopup("place"))
		{
			draw_place_menu();
			ImGui::EndPopup();
		}

		if (flown && !g_looking)
		{
			const char* hint = camera.mode == CameraMode::Bone
				? "Hold the right mouse button to turn  \xe2\x80\xa2  W A S D, Q, E move the camera on the bone"
				: "Hold the right mouse button to look  \xe2\x80\xa2  right click to place a light or an effect  \xe2\x80\xa2  W A S D to fly  \xe2\x80\xa2  K adds a campath node";
			const ImVec2 hint_size = ImGui::CalcTextSize(hint);
			draw->AddText({at.x + picture.x - hint_size.x - g_font_size, at.y + picture.y - hint_size.y - g_font_size * 0.5f}, u32(P.muted), hint);
		}

		ImGui::EndChild();
		ImGui::PopStyleVar();
	}

	bool section(const char* icon, const char* label, bool* reset = nullptr, const bool open_by_default = true)
	{
		char text[96];
		snprintf(text, sizeof(text), "%s  %s", icon, label);
		ImGui::PushStyleColor(ImGuiCol_Header, color(P.raised));
		const bool open = ImGui::CollapsingHeader(text, (open_by_default ? ImGuiTreeNodeFlags_DefaultOpen : 0) | ImGuiTreeNodeFlags_AllowOverlap);
		ImGui::PopStyleColor();
		if (reset)
		{
			// "Reset" at the header's right, as IrisStudio's sections have it
			const ImGuiStyle& style = ImGui::GetStyle();
			const char* reset_label = ICON_ROTATE " Reset";
			const float width = ImGui::CalcTextSize(reset_label).x + style.FramePadding.x * 2.0f;
			ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - width - style.FramePadding.x);
			ImGui::PushID(label);
			*reset = ImGui::SmallButton(reset_label);
			ImGui::PopID();
		}

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
			ImGui::BeginDisabled(camera.mode != CameraMode::Free && camera.mode != CameraMode::Bone);
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

		if (camera.mode == CameraMode::Bone && section(ICON_VIDEO, "Bone camera"))
		{
			// the camera rides on a part of a player's model
			static const char* const BONES[] = {"j_head", "tag_eye", "j_helmet", "j_neck", "j_spineupper", "j_spinelower", "j_mainroot",
			                                    "tag_weapon_right", "tag_weapon_left", "tag_flash", "j_wrist_ri", "j_wrist_le",
			                                    "j_shoulder_ri", "j_shoulder_le", "j_ankle_ri", "j_ankle_le", "tag_origin"};
			BoneState bone = camera_bone_state();
			BoneCamera wanted = bone.camera;
			bool changed = false;
			char label[48];
			auto name_of = [&](const int entity, char* out, const size_t size) {
				snprintf(out, size, entity == bone.nearest ? "Player %d (nearest the camera)" : "Player %d", entity);
			};
			if (wanted.entity >= 0)
			{
				name_of(wanted.entity, label, sizeof(label));
			}
			else
			{
				snprintf(label, sizeof(label), "None in view");
			}

			row_label("Player");
			if (ImGui::BeginCombo("##boneEntity", label))
			{
				for (int entity = 0; entity < 64; ++entity)
				{
					if (bone.entities & (1ull << entity))
					{
						char name[48];
						name_of(entity, name, sizeof(name));
						if (ImGui::Selectable(name, entity == wanted.entity))
						{
							wanted.entity = entity;
							changed = true;
						}
					}
				}

				ImGui::EndCombo();
			}

			row_label("Bone");
			if (ImGui::BeginCombo("##boneName", wanted.bone))
			{
				for (const char* name : BONES)
				{
					if (ImGui::Selectable(name, strcmp(name, wanted.bone) == 0))
					{
						snprintf(wanted.bone, sizeof(wanted.bone), "%s", name);
						changed = true;
					}
				}

				ImGui::EndCombo();
			}

			row_label("Other bone");
			static char typed[48] = "";
			if (ImGui::InputTextWithHint("##boneTyped", "a bone's name, then Enter", typed, sizeof(typed), ImGuiInputTextFlags_EnterReturnsTrue) && typed[0])
			{
				for (char* c = typed; *c; ++c)
				{
					*c = static_cast<char>(tolower(static_cast<unsigned char>(*c)));
				}

				snprintf(wanted.bone, sizeof(wanted.bone), "%s", typed);
				changed = true;
			}

			row_label("Offset");
			changed = ImGui::DragFloat3("##boneOffset", wanted.offset, 0.25f, -2000.0f, 2000.0f, "%.1f") || changed;
			ImGui::SetItemTooltip("Forward, right, up, along the bone's own directions");
			row_label("Rotation");
			changed = ImGui::DragFloat3("##boneTurn", wanted.turn, 0.5f, -720.0f, 720.0f, "%.1f") || changed;
			ImGui::SetItemTooltip("Up and down, around, roll");
			row_label("Turn with the bone");
			changed = ImGui::Checkbox("##boneRotate", &wanted.rotate) || changed;
			if (changed)
			{
				camera_set_bone(wanted);
			}

			if (!bone.found)
			{
				ImGui::PushTextWrapPos(0.0f);
				ImGui::TextDisabled(bone.entities ? "This player's model does not have that bone, or the player is not in the match right now."
				                                  : "No player's model is in the match right now.");
				ImGui::PopTextWrapPos();
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
			ImGui::TextDisabled("Your own soldier in place of the arms and gun, in the Third person, Free and Dolly cameras.");
			ImGui::PopTextWrapPos();
			bool clean = camera.clean;
			row_label("Clean outside view");
			if (ImGui::Checkbox("##clean", &clean))
			{
				camera_set_clean(clean);
			}

			ImGui::SetItemTooltip("In the Free, Dolly and Bone cameras: no HUD (a scorestreak's included) and no red flash or markers from being hit.\nFirst person keeps them.");
			bool aim = camera.player_aim;
			row_label("Player's own aim");
			if (ImGui::Checkbox("##aim", &aim))
			{
				camera_set_player_aim(aim);
			}

			ImGui::SetItemTooltip("Experimental. A scorestreak's laser or marker stays where the player aimed it, not where this camera looks.\nModels far from the player may be drawn with less detail while it is on.");
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

	// The recordings in the theater folder, newest first; a click plays one.
	void draw_recordings(const TheaterState& state, const float height)
	{
		static RecordingInfo list[200];
		static int count = 0;
		static ULONGLONG listed = 0;
		const ULONGLONG now = GetTickCount64();
		if (!listed || now - listed > 2500)
		{
			listed = now;
			count = theater_recordings(list, 200);
		}

		if (count == 0)
		{
			ImGui::PushTextWrapPos(0.0f);
			ImGui::TextDisabled("No recordings yet. Every private match is recorded by itself; it shows up here when the match ends.");
			ImGui::PopTextWrapPos();
			return;
		}

		if (!ImGui::BeginTable("recordings", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp, {0.0f, height}))
		{
			return;
		}

		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("Recorded", ImGuiTableColumnFlags_None, 2.2f);
		ImGui::TableSetupColumn("Map", ImGuiTableColumnFlags_None, 1.6f);
		ImGui::TableSetupColumn("Mode", ImGuiTableColumnFlags_None, 0.8f);
		ImGui::TableSetupColumn("Length", ImGuiTableColumnFlags_None, 0.9f);
		ImGui::TableHeadersRow();
		for (int i = 0; i < count; ++i)
		{
			const RecordingInfo& info = list[i];
			// rec_YYYYMMDD_HHMMSS.awdm
			char when[40];
			const char* f = info.file;
			if (strlen(f) >= 19 && strncmp(f, "rec_", 4) == 0)
			{
				snprintf(when, sizeof(when), "%.4s-%.2s-%.2s  %.2s:%.2s:%.2s", f + 4, f + 8, f + 10, f + 13, f + 15, f + 17);
			}
			else
			{
				snprintf(when, sizeof(when), "%.39s", f);
			}

			const bool current = state.playing && strcmp(state.name, info.file) == 0;
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::PushID(i);
			if (ImGui::Selectable(when, current, ImGuiSelectableFlags_SpanAllColumns) && !current)
			{
				theater_play(info.file);
			}

			ImGui::PopID();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(info.map);
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(info.gametype);
			ImGui::TableNextColumn();
			ImGui::Text("%u:%02u", info.length_ms / 60000, (info.length_ms / 1000) % 60);
		}

		ImGui::EndTable();
	}

	// At the game's menus: a small button, and the recordings behind it.
	void draw_menu_browser(const TheaterState& state)
	{
		static bool open = false;
		const ImGuiIO& io = ImGui::GetIO();
		ImGui::SetNextWindowPos({g_font_size * 0.6f, g_font_size * 0.6f});
		ImGui::Begin("##s1irisButton", nullptr,
		             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
		                 ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBackground);
		ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(open ? ImGuiCol_Header : ImGuiCol_Button));
		if (ImGui::Button(ICON_FILM "  S1iris recordings"))
		{
			open = !open;
		}

		ImGui::PopStyleColor();
		ImGui::End();
		if (open)
		{
			ImGui::SetNextWindowPos({g_font_size * 0.6f, g_font_size * 3.0f}, ImGuiCond_Appearing);
			ImGui::SetNextWindowSize({g_font_size * 34.0f, g_font_size * 24.0f}, ImGuiCond_Appearing);
			ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
			if (ImGui::Begin("Recordings", &open, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoNav))
			{
				ImGui::TextDisabled("Click a recording to play it.");
				draw_recordings(state, 0.0f);
			}

			ImGui::End();
			ImGui::PopStyleVar();
		}

		g_over_menu_ui = io.WantCaptureMouse;
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

			}

			if (section(ICON_FOLDER_OPEN, "Recordings"))
			{
				draw_recordings(state, (std::max)(g_font_size * 8.0f, ImGui::GetContentRegionAvail().y - ImGui::GetStyle().WindowPadding.y));
			}

			break;
		case Tab::Camera:
			draw_camera_tab(state, camera);
			break;
		case Tab::Look:
			look_draw_tab(state);
			break;
		case Tab::Scene:
			if (section(ICON_LIGHTBULB, "Lights"))
			{
				// lights of the editor's own: each a point that shines all round
				SceneLight lights[16];
				const int count = scene_lights(lights, 16);
				ImGui::BeginDisabled(count >= 16);
				if (ImGui::Button(ICON_PLUS "  Add light here"))
				{
					scene_add_light();
				}

				ImGui::EndDisabled();
				ImGui::SetItemTooltip("A new light where the camera is now");
				int remove = -1;
				for (int i = 0; i < count; ++i)
				{
					SceneLight light = lights[i];
					bool changed = false;
					ImGui::PushID(i);
					ImGui::Separator();
					changed = ImGui::Checkbox("##on", &light.on) || changed;
					ImGui::SameLine();
					ImGui::AlignTextToFramePadding();
					ImGui::Text("Light %d", i + 1);
					ImGui::SameLine();
					if (ImGui::SmallButton("Move here"))
					{
						scene_move_light_here(i);
						SceneLight moved[16];
						if (scene_lights(moved, 16) > i)
						{
							char id[32];
							snprintf(id, sizeof(id), "light%d_position", i);
							look_key_changed(id, moved[i].origin);
						}

						ImGui::PopID();
						continue;
					}

					ImGui::SetItemTooltip("Move this light to where the camera is now");
					ImGui::SameLine();
					if (ImGui::SmallButton(ICON_TRASH))
					{
						remove = i;
					}

					row_label("Kind");
					if (ImGui::RadioButton("Point", !light.spot))
					{
						light.spot = false;
						changed = true;
					}

					ImGui::SameLine();
					if (ImGui::RadioButton("Spot", light.spot))
					{
						light.spot = true;
						changed = true;
					}

					ImGui::SetItemTooltip("A cone that shines one way and can cast shadows. The game draws two spot lights at most.");
					if (light.spot)
					{
						row_label("Direction");
						changed = ImGui::DragFloat3("##angles", light.angles, 0.5f, -360.0f, 360.0f, "%.0f") || changed;
						ImGui::SetItemTooltip("Up and down, around, roll. \"Move here\" also points it the way the camera looks.");
						row_label("Cone");
						changed = ImGui::SliderFloat("##cone", &light.cone, 5.0f, 150.0f, "%.0f deg") || changed;
						row_label("Shadows");
						changed = ImGui::Checkbox("##shadows", &light.shadows) || changed;
					}

					// (each row can have keyframes: light<N>_...)
					char id[32], name[48];
					const float key = look_key_width();
					snprintf(id, sizeof(id), "light%d_color", i);
					snprintf(name, sizeof(name), "Light %d color", i + 1);
					row_label("Color");
					ImGui::SetNextItemWidth(-key);
					if (ImGui::ColorEdit3("##color", light.color))
					{
						changed = true;
						look_key_changed(id, light.color);
					}

					look_key_button(id, name, 3, light.color);
					// (the slider is in doublings: the game's own lights' colours are in the hundreds)
					float stops = light.intensity > 1.0f ? std::log2(light.intensity) : 0.0f;
					snprintf(id, sizeof(id), "light%d_brightness", i);
					snprintf(name, sizeof(name), "Light %d brightness", i + 1);
					row_label("Brightness");
					ImGui::SetNextItemWidth(-key);
					if (ImGui::SliderFloat("##intensity", &stops, 0.0f, 30.0f, "%.1f"))
					{
						light.intensity = std::pow(2.0f, stops);
						changed = true;
						look_key_changed(id, &stops);
					}

					ImGui::SetItemTooltip("Each step up is twice as bright");
					look_key_button(id, name, 1, &stops);
					snprintf(id, sizeof(id), "light%d_reach", i);
					snprintf(name, sizeof(name), "Light %d reach", i + 1);
					row_label("Reach");
					ImGui::SetNextItemWidth(-key);
					if (ImGui::SliderFloat("##radius", &light.radius, 10.0f, 5000.0f, "%.0f", ImGuiSliderFlags_Logarithmic))
					{
						changed = true;
						look_key_changed(id, &light.radius);
					}

					look_key_button(id, name, 1, &light.radius);
					snprintf(id, sizeof(id), "light%d_position", i);
					snprintf(name, sizeof(name), "Light %d position", i + 1);
					row_label("Position");
					ImGui::SetNextItemWidth(-key);
					if (ImGui::DragFloat3("##origin", light.origin, 1.0f, 0.0f, 0.0f, "%.0f"))
					{
						changed = true;
						look_key_changed(id, light.origin);
					}

					look_key_button(id, name, 3, light.origin);
					if (changed)
					{
						scene_set_light(i, light);
					}

					ImGui::PopID();
				}

				if (remove >= 0)
				{
					// (keyframes are by a light's number: this light's and the later ones' go with it)
					for (int i = remove; i < count; ++i)
					{
						for (const char* what : {"color", "brightness", "reach", "position"})
						{
							char id[32];
							snprintf(id, sizeof(id), "light%d_%s", i, what);
							look_key_remove(id);
						}
					}

					scene_remove_light(remove);
				}
			}

			if (section(ICON_WAND, "Effects", nullptr, false))
			{
				// any effect the game has loaded, played where the camera is, at a time of the recording
				char* fx_search = g_fx_search;
				auto& fx_found = g_fx_found;
				const int extra = effects_extra_state();
				if (extra == 1)
				{
					if (ImGui::Button("Load Ascend's rain, mist and lightning"))
					{
						effects_load_extra();
					}

					ImGui::SetItemTooltip("The effects built into zone\\s1iris_rain.ff, for use on any map. Loaded once; they stay until the game is closed.");
				}
				else if (extra == 2)
				{
					ImGui::TextDisabled("Ascend's rain, mist and lightning are loaded: search rain, mist or lightning.");
				}

				ImGui::SetNextItemWidth(-1.0f);
				ImGui::InputTextWithHint("##fxSearch", ICON_SEARCH "  search the effects by name (explosion, spark, smoke...)", fx_search, sizeof(g_fx_search));
				const int fx_count = effects_find(fx_search, fx_found, 400);
				ImGui::BeginChild("fxResults", {0.0f, g_font_size * 10.0f}, ImGuiChildFlags_Border);
				ImGuiListClipper fx_clipper;
				fx_clipper.Begin(fx_count);
				while (fx_clipper.Step())
				{
					for (int f = fx_clipper.DisplayStart; f < fx_clipper.DisplayEnd; ++f)
					{
						if (ImGui::Selectable(fx_found[f]))
						{
							float here[3];
							camera_view_origin(here);
							effects_add(fx_found[f], here, state.position_ms);
							snprintf(g_fx_last, sizeof(g_fx_last), "%s", fx_found[f]);
						}
					}
				}

				ImGui::EndChild();
				ImGui::TextDisabled(fx_count == 400 ? "More than 400: type more of the name. A click plays one where the camera is." : "A click plays one where the camera is and keeps it below.");
				PlacedEffect placed[32];
				const int placed_count = effects_placed(placed, 32);
				int remove_fx = -1;
				for (int i = 0; i < placed_count; ++i)
				{
					PlacedEffect effect = placed[i];
					bool changed = false;
					ImGui::PushID(2000 + i);
					ImGui::Separator();
					changed = ImGui::Checkbox("##on", &effect.on) || changed;
					ImGui::SameLine();
					if (ImGui::SmallButton(ICON_PLAY))
					{
						effects_fire(i);
					}

					ImGui::SetItemTooltip("Play it now");
					ImGui::SameLine();
					if (ImGui::SmallButton("Move here"))
					{
						camera_view_origin(effect.origin);
						changed = true;
					}

					ImGui::SameLine();
					if (ImGui::SmallButton(ICON_TRASH))
					{
						remove_fx = i;
					}

					ImGui::PushTextWrapPos(0.0f);
					ImGui::TextUnformatted(effect.name);
					ImGui::PopTextWrapPos();
					float seconds = effect.at_ms / 1000.0f, again = effect.every_ms / 1000.0f;
					row_label("Plays at");
					ImGui::SetNextItemWidth(-ImGui::CalcTextSize("Now").x - ImGui::GetStyle().FramePadding.x * 2.0f - ImGui::GetStyle().ItemSpacing.x);
					if (ImGui::DragFloat("##at", &seconds, 0.01f, 0.0f, state.length_ms / 1000.0f, "%.2f s"))
					{
						effect.at_ms = static_cast<std::uint32_t>(seconds * 1000.0f);
						changed = true;
					}

					ImGui::SameLine();
					if (ImGui::Button("Now"))
					{
						effect.at_ms = state.position_ms;
						changed = true;
					}

					float lasts = effect.length_ms / 1000.0f;
					row_label("Length");
					if (ImGui::DragFloat("##length", &lasts, 0.01f, 0.1f, 600.0f, "%.2f s"))
					{
						effect.length_ms = static_cast<std::uint32_t>(lasts * 1000.0f);
						changed = true;
					}

					ImGui::SetItemTooltip("Its clip on the timeline. With \"Again every\" set, it is played again and again only for this long.");
					row_label("Again every");
					if (ImGui::DragFloat("##every", &again, 0.01f, 0.0f, 60.0f, again > 0.0f ? "%.2f s" : "never"))
					{
						effect.every_ms = static_cast<std::uint32_t>(again * 1000.0f);
						changed = true;
					}

					row_label("Follow the camera");
					changed = ImGui::Checkbox("##follow", &effect.follow) || changed;
					ImGui::SetItemTooltip("Each time it plays, it plays where the camera is then (rain or mist around the view). Use it with \"Again every\".");
					if (effect.follow)
					{
						row_label("Above the camera");
						changed = ImGui::DragFloat("##above", &effect.above, 1.0f, -2000.0f, 2000.0f, "%.0f") || changed;
					}
					else
					{
						row_label("Position");
						changed = ImGui::DragFloat3("##origin", effect.origin, 1.0f, 0.0f, 0.0f, "%.0f") || changed;
					}

					row_label("Direction");
					changed = ImGui::DragFloat3("##angles", effect.angles, 1.0f, -360.0f, 360.0f, "%.0f") || changed;
					ImGui::SetItemTooltip("Up and down, around, roll: which way the effect goes (it starts going up)");
					if (changed)
					{
						effects_set(i, effect);
					}

					ImGui::PopID();
				}

				if (remove_fx >= 0)
				{
					effects_remove(remove_fx);
				}
			}

			if (section(ICON_PERSON, "Models"))
			{
				// a player's outfit and weapon, part by part, each drawn as another model or left out
				static const char* const SLOT_LABELS[] = {"Top", "Headgear", "Head", "Pants", "Gloves", "Boots", "Kneepads", "Loadout gear", "Eyewear", "Exo"};
				static int entity = -1, target_slot = -1, group = 0;
				static char target[64] = "", search[48] = "", set_id[32] = "";
				static bool only_this_kind = true;
				const BoneState players = camera_bone_state();
				// (2048 is the first person's arms and gun: the game makes them like an entity's model)
				constexpr int FIRST_PERSON = 2048;
				if (entity != FIRST_PERSON && (entity < 0 || !(players.entities & (1ull << entity))))
				{
					entity = players.nearest;
				}

				char label[48];
				snprintf(label, sizeof(label), entity == FIRST_PERSON ? "First person (arms and gun)" : entity < 0 ? "None in view"
					: entity == players.nearest ? "Player %d (nearest the camera)" : "Player %d", entity);
				row_label("Player");
				if (ImGui::BeginCombo("##modelEntity", label))
				{
					if (ImGui::Selectable("First person (arms and gun)", entity == FIRST_PERSON))
					{
						entity = FIRST_PERSON;
						target[0] = 0;
						target_slot = -1;
					}

					for (int e = 0; e < 64; ++e)
					{
						if (players.entities & (1ull << e))
						{
							char name[48];
							snprintf(name, sizeof(name), e == players.nearest ? "Player %d (nearest the camera)" : "Player %d", e);
							if (ImGui::Selectable(name, e == entity))
							{
								entity = e;
								target[0] = 0;
								target_slot = -1;
							}
						}
					}

					ImGui::EndCombo();
				}

				static char parts[32][64], added[16][64], found[400][64], sets[160][32];
				const int part_count = entity >= 0 ? models_of(entity, parts, 32) : 0;
				const int added_count = entity >= 0 ? models_added(entity, added, 16) : 0;
				bool rebuilt = false;
				if (part_count == 0)
				{
					ImGui::PushTextWrapPos(0.0f);
					ImGui::TextDisabled(players.entities ? "This player's parts are listed once the game has made its model: go back a little in the recording."
					                                     : "No player's model is in the match right now.");
					ImGui::PopTextWrapPos();
				}
				else
				{
					// a whole set of gear at once, as the game's own outfit menu does
					const int set_count = entity == FIRST_PERSON ? 0 : models_sets(sets, 160);
					ImGui::BeginDisabled(entity == FIRST_PERSON);
					row_label("Gear set");
					ImGui::SetNextItemWidth(-ImGui::CalcTextSize("Apply").x - ImGui::GetStyle().FramePadding.x * 2.0f - ImGui::GetStyle().ItemSpacing.x);
					if (ImGui::BeginCombo("##gearSet", set_id[0] ? set_id : "Choose..."))
					{
						for (int i = 0; i < set_count; ++i)
						{
							if (ImGui::Selectable(sets[i], strcmp(sets[i], set_id) == 0))
							{
								snprintf(set_id, sizeof(set_id), "%s", sets[i]);
							}
						}

						ImGui::EndCombo();
					}

					ImGui::SameLine();
					ImGui::BeginDisabled(!set_id[0]);
					if (ImGui::Button("Apply"))
					{
						// (the set's models are asked for from the game's files; it is put on when they are there)
						models_apply_set(entity, set_id);
					}

					ImGui::EndDisabled();
					ImGui::SetItemTooltip("The game's own sets of gear, loot included: every part at once (the face stays)");
					bool loading = models_loading_allowed();
					if (ImGui::Checkbox("Load gear nobody in the match wears (experimental)", &loading))
					{
						models_allow_loading(loading);
					}

					ImGui::SetItemTooltip("Off: a set's parts are put on as far as this match has them in memory.\nOn: the rest are added to the list of gear the game itself keeps loaded for the match; the recording goes back twice while it loads.");
					char gear_status[200];
					models_status(gear_status);
					if (gear_status[0])
					{
						ImGui::PushTextWrapPos(0.0f);
						ImGui::TextDisabled("%s", gear_status);
						ImGui::PopTextWrapPos();
					}

					ImGui::EndDisabled();
					if (ImGui::Button("Reset this player"))
					{
						models_clear(entity);
						target[0] = 0;
						target_slot = -1;
						rebuilt = true;
					}

					// a weapon's camouflage: any of that weapon's the game has in memory
					for (int i = 0; i < part_count; ++i)
					{
						static char camos[16][64];
						const int camo_count = models_camos(parts[i], camos, 16);
						if (camo_count == 0)
						{
							continue;
						}

						char current[64] = "";
						const bool painted = models_camo(entity, parts[i], current);
						auto pretty = [](const char* material) {
							const char* cut = strrchr(material, '_');
							return cut ? cut + 1 : material;
						};
						ImGui::PushID(3000 + i);
						row_label("Camo");
						ImGui::SetNextItemWidth(-1.0f);
						if (camo_count < 0)
						{
							ImGui::TextDisabled("none of this gun's are loaded in this match");
						}
						else if (ImGui::BeginCombo("##camo", painted ? pretty(current) : "As recorded"))
						{
							if (ImGui::Selectable("As recorded", !painted))
							{
								models_set_camo(entity, parts[i], nullptr);
								rebuilt = true;
							}

							for (int c = 0; c < camo_count; ++c)
							{
								if (ImGui::Selectable(pretty(camos[c]), painted && strcmp(current, camos[c]) == 0))
								{
									models_set_camo(entity, parts[i], camos[c]);
									rebuilt = true;
								}
							}

							ImGui::EndCombo();
						}

						ImGui::SetItemTooltip("%s", parts[i]);
						ImGui::PopID();
					}

					// the outfit's parts, then the rest (the weapon and what is on it)
					auto row = [&](const char* kind, const char* own, const int slot) {
						char swapped[64] = "";
						const bool is_swapped = own[0] && models_swapped(entity, own, swapped);
						const char* extra = nullptr;
						if (!own[0])
						{
							for (int a = 0; a < added_count; ++a)
							{
								if (models_slot(added[a]) == slot)
								{
									extra = added[a];
								}
							}
						}

						const bool chosen = own[0] ? strcmp(target, own) == 0 : target_slot == slot && !target[0];
						const char* shown = is_swapped ? (swapped[0] ? swapped : "(left out)") : own[0] ? own : extra ? extra : "(none)";
						ImGui::PushID(kind);
						ImGui::PushID(own);
						ImGui::AlignTextToFramePadding();
						ImGui::TextDisabled("%s", kind);
						ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.34f);
						if (is_swapped || extra)
						{
							ImGui::PushStyleColor(ImGuiCol_Text, color(P.accent));
						}

						if (ImGui::Selectable(shown, chosen))
						{
							snprintf(target, sizeof(target), "%s", own);
							target_slot = slot;
							group = entity == FIRST_PERSON ? 2 : slot >= 0 ? 0 : strncmp(own, "npc_", 4) == 0 ? 1 : strncmp(own, "mp_", 3) == 0 || !own[0] ? 0 : 2;
							search[0] = 0;
						}

						if (is_swapped || extra)
						{
							ImGui::PopStyleColor();
						}

						ImGui::PopID();
						ImGui::PopID();
					};
					ImGui::Separator();
					for (int slot = 0; slot < 10 && entity != FIRST_PERSON; ++slot)
					{
						const char* own = "";
						for (int i = 0; i < part_count; ++i)
						{
							if (models_slot(parts[i]) == slot)
							{
								own = parts[i];
							}
						}

						row(SLOT_LABELS[slot], own, slot);
					}

					for (int i = 0; i < part_count; ++i)
					{
						if (models_slot(parts[i]) < 0)
						{
							row(strncmp(parts[i], "npc_", 4) == 0 || strncmp(parts[i], "vm_", 3) == 0 ? "Weapon part" : "Part", parts[i], -1);
						}
					}

					// the picker for the row chosen
					if (target[0] || target_slot >= 0)
					{
						ImGui::Separator();
						ImGui::TextDisabled("Draw %s as", target[0] ? target : SLOT_LABELS[target_slot]);
						if (ImGui::SmallButton(target[0] ? "As it is" : "None"))
						{
							if (target[0])
							{
								models_swap(entity, target, nullptr);
							}
							else
							{
								for (int a = 0; a < added_count; ++a)
								{
									if (models_slot(added[a]) == target_slot)
									{
										models_add(entity, added[a], false);
									}
								}
							}

							rebuilt = true;
						}

						if (target[0] && strcmp(target, parts[0]) != 0)
						{
							ImGui::SameLine();
							if (ImGui::SmallButton("Leave out"))
							{
								models_swap(entity, target, "");
								rebuilt = true;
							}
						}

						ImGui::SetNextItemWidth(-1.0f);
						ImGui::InputTextWithHint("##modelSearch", ICON_SEARCH "  search the models by name", search, sizeof(search));
						static const char* const GROUPS[] = {"Outfit", "Weapons", "First person", "Everything"};
						for (int g = 0; g < 4; ++g)
						{
							if (g)
							{
								ImGui::SameLine();
							}

							if (ImGui::RadioButton(GROUPS[g], group == g))
							{
								group = g;
							}
						}

						if (group == 0 && target_slot >= 0)
						{
							ImGui::Checkbox("Only this kind of part", &only_this_kind);
						}

						const int found_count = models_find(search, group, group == 0 && only_this_kind ? target_slot : -1, found, 400);
						ImGui::BeginChild("modelResults", {0.0f, g_font_size * 14.0f}, ImGuiChildFlags_Border);
						ImGuiListClipper clipper;
						clipper.Begin(found_count);
						while (clipper.Step())
						{
							for (int f = clipper.DisplayStart; f < clipper.DisplayEnd; ++f)
							{
								if (ImGui::Selectable(found[f]))
								{
									if (target[0])
									{
										models_swap(entity, target, found[f]);
									}
									else
									{
										for (int a = 0; a < added_count; ++a)
										{
											if (models_slot(added[a]) == target_slot)
											{
												models_add(entity, added[a], false);
											}
										}

										models_add(entity, found[f], true);
									}

									rebuilt = true;
								}
							}
						}

						ImGui::EndChild();
						if (found_count == 400)
						{
							ImGui::TextDisabled("More than 400: type more of the name.");
						}
					}

					if (models_pending(entity))
					{
						ImGui::PushTextWrapPos(0.0f);
						ImGui::TextColored(color(P.warn), "Waiting for the game to rebuild this player. If nothing changes, go back a few seconds in the recording.");
						ImGui::PopTextWrapPos();
					}
				}

				if (rebuilt)
				{
					// the game makes a model when its entity comes into the match: from the start again
					theater_rebuild();
				}
			}

			not_built("Actors and animations");
			break;
		case Tab::Record:
			capture_draw_tab(state);
			break;
		}

		ImGui::EndChild();
		ImGui::PopStyleVar();
	}

	// The timeline: the controls' row, then the ruler with the playhead over the rows of what has keys.
	void draw_timeline(const TheaterState& state, const CameraState& camera, const ImVec2 size)
	{
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {g_font_size * 0.5f, g_font_size * 0.4f});
		ImGui::BeginChild("timeline", size, ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
		const ImGuiStyle& style = ImGui::GetStyle();
		const float frame = ImGui::GetFrameHeight();
		// how far in the timeline is zoomed (1: all of the recording), and the time at its left end
		static float zoom = 1.0f, view_start = 0.0f;

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

		ImGui::SameLine(0.0f, g_font_size);
		if (zoom > 1.001f)
		{
			if (ImGui::Button("Fit"))
			{
				zoom = 1.0f;
				view_start = 0.0f;
			}

			ImGui::SetItemTooltip("Show the whole recording again");
			ImGui::SameLine();
			ImGui::TextDisabled("%.1fx   wheel: zoom   Shift + wheel: move along", zoom);
		}
		else
		{
			ImGui::TextDisabled("wheel over the timeline: zoom in");
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
		{
			// the wheel over the timeline zooms about the mouse; with Shift it moves along
			const ImGuiIO& io = ImGui::GetIO();
			const bool over_timeline = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
			const float most = length / 2000.0f > 1.0f ? length / 2000.0f : 1.0f; // (two seconds across at the most)
			if (over_timeline && io.MouseWheel != 0.0f && right > left)
			{
				const float t = (io.MousePos.x - left) / (right - left);
				const float before = length / zoom;
				if (io.KeyShift)
				{
					view_start -= io.MouseWheel * before * 0.15f;
				}
				else
				{
					const float under = view_start + (t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t) * before;
					zoom *= io.MouseWheel > 0.0f ? 1.25f : 0.8f;
					zoom = zoom < 1.0f ? 1.0f : zoom > most ? most : zoom;
					view_start = under - (t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t) * (length / zoom);
				}
			}

			zoom = zoom < 1.0f ? 1.0f : zoom > most ? most : zoom;
		}

		const float shown = length / zoom;
		if (zoom > 1.001f && !state.paused && state.playing)
		{
			// playing: the timeline follows the playhead
			const float now_ms = static_cast<float>(state.position_ms);
			if (now_ms < view_start || now_ms > view_start + shown)
			{
				view_start = now_ms - shown * 0.1f;
			}
		}

		view_start = view_start > length - shown ? length - shown : view_start;
		view_start = view_start < 0.0f ? 0.0f : view_start;
		auto x_of = [&](const float ms) { return left + (right - left) * ((ms - view_start) / shown); };
		auto ms_at = [&](const float x) { return view_start + (x - left) / (right - left) * shown; };
		auto in_view = [&](const float x) { return x >= left - 1.0f && x <= right + 1.0f; };

		// a tick every so many seconds that the labels have room
		const float label_room = ImGui::CalcTextSize("0:00").x * 2.2f;
		static const int EVERY[] = {1, 2, 5, 10, 15, 30, 60, 120, 300, 600};
		int every = EVERY[9];
		for (const int candidate : EVERY)
		{
			if ((right - left) * (candidate * 1000.0f / shown) >= label_room)
			{
				every = candidate;
				break;
			}
		}

		const float ruler_bottom = origin.y + frame;
		const float rows_bottom = origin.y + size.y;
		for (int s = (static_cast<int>(view_start / 1000.0f) / every) * every; s * 1000.0f <= view_start + shown; s += every)
		{
			const float x = std::floor(x_of(s * 1000.0f));
			if (!in_view(x))
			{
				continue;
			}

			char text[16];
			snprintf(text, sizeof(text), "%d:%02d", s / 60, s % 60);
			draw->AddLine({x, origin.y + frame * 0.25f}, {x, ruler_bottom}, u32(P.muted));
			draw->AddText({x + 4.0f, origin.y}, u32(P.text), text);
			draw->AddLine({x, ruler_bottom}, {x, rows_bottom}, u32(P.border, 0.5f));
			const float half = std::floor(x_of((s + every * 0.5f) * 1000.0f));
			if (half < right && half > left)
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
			scrub_ms = ms_at(left + (t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t) * (right - left));
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
			if (!in_view(x))
			{
				continue;
			}

			draw->AddQuadFilled({x, middle - diamond}, {x + diamond, middle}, {x, middle + diamond}, {x - diamond, middle}, u32(P.text));
		}

		// a row for each setting that has keyframes: a click on a keyframe goes to it, a right click removes it
		static LookTrack track;
		int clear = -1, remove_track = -1, remove_key = -1;
		for (int t = 0; look_track(t, &track); ++t)
		{
			y += row;
			if (y + frame > rows_bottom)
			{
				break;
			}

			draw->AddRectFilled({left, y}, {right, y + frame}, u32(P.surface));
			ImGui::PushID(t);
			ImGui::PushClipRect({origin.x, y}, {left - frame * 1.2f - style.ItemSpacing.x * 2.0f, y + frame}, true);
			char name[96];
			snprintf(name, sizeof(name), ICON_SLIDERS "  %s (%d)", track.label, track.keys);
			draw->AddText({origin.x + g_font_size * 0.5f, y + style.FramePadding.y}, u32(P.text), name);
			ImGui::PopClipRect();
			ImGui::SetCursorScreenPos({left - frame * 1.2f - style.ItemSpacing.x, y});
			if (ImGui::Button(ICON_TRASH "##clearTrack", {frame * 1.2f, frame}))
			{
				clear = t;
			}

			ImGui::SetItemTooltip("Remove these keyframes");
			for (int i = 0; i < track.keys; ++i)
			{
				const float x = std::floor(x_of(static_cast<float>(track.ms[i]))), middle = y + frame * 0.5f;
				if (!in_view(x))
				{
					continue;
				}

				ImGui::SetCursorScreenPos({x - diamond, y});
				ImGui::PushID(i);
				if (ImGui::InvisibleButton("##key", {diamond * 2.0f, frame}))
				{
					theater_seek(track.ms[i]);
				}

				const bool over = ImGui::IsItemHovered();
				if (over && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
				{
					remove_track = t;
					remove_key = i;
				}

				if (over)
				{
					char at[32];
					format_time(at, sizeof(at), track.ms[i]);
					ImGui::SetTooltip("%s\nClick: go here.  Right click: remove.", at);
				}

				ImGui::PopID();
				const float r = over ? diamond * 1.2f : diamond;
				draw->AddQuadFilled({x, middle - r}, {x + r, middle}, {x, middle + r}, {x - r, middle}, u32(over ? P.text : P.accent));
			}

			ImGui::PopID();
		}

		// the editor's effects: a clip each, from when it plays for as long as it is given; dragged, it
		// plays earlier or later, and its right end makes it longer or shorter
		{
			PlacedEffect placed[32];
			const int placed_count = effects_placed(placed, 32);
			static float grab_ms = 0.0f;
			for (int i = 0; i < placed_count; ++i)
			{
				y += row;
				if (y + frame > rows_bottom)
				{
					break;
				}

				PlacedEffect& effect = placed[i];
				const char* slash = strrchr(effect.name, '/');
				const char* shown = slash ? slash + 1 : effect.name;
				draw->AddRectFilled({left, y}, {right, y + frame}, u32(P.surface));
				ImGui::PushClipRect({origin.x, y}, {left - style.ItemSpacing.x, y + frame}, true);
				char name[128];
				snprintf(name, sizeof(name), ICON_WAND "  %s", shown);
				draw->AddText({origin.x + g_font_size * 0.5f, y + style.FramePadding.y}, u32(effect.on ? P.text : P.muted), name);
				ImGui::PopClipRect();
				float x0 = std::floor(x_of(static_cast<float>(effect.at_ms)));
				float x1 = std::floor(x_of(static_cast<float>(effect.at_ms + effect.length_ms)));
				if (x1 < left || x0 > right)
				{
					// (not in the part of the recording shown)
					continue;
				}

				x0 = x0 < left ? left : x0;
				x1 = x1 < x0 + 10.0f ? x0 + 10.0f : x1 > right ? right : x1;
				const float mouse_ms = ms_at(ImGui::GetIO().MousePos.x);
				ImGui::PushID(5000 + i);
				// the right end, then the clip itself
				const float handle = 8.0f;
				ImGui::SetCursorScreenPos({x1 - handle, y});
				ImGui::InvisibleButton("##end", {handle * 2.0f, frame});
				const bool end_over = ImGui::IsItemHovered(), end_held = ImGui::IsItemActive();
				if (end_held)
				{
					const float longer = mouse_ms - static_cast<float>(effect.at_ms);
					effect.length_ms = static_cast<std::uint32_t>(longer < 100.0f ? 100.0f : longer);
					effects_set(i, effect);
				}

				ImGui::SetCursorScreenPos({x0, y});
				ImGui::InvisibleButton("##clip", {(x1 - handle - x0) < 4.0f ? 4.0f : x1 - handle - x0, frame});
				const bool over = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
				if (ImGui::IsItemActivated())
				{
					grab_ms = mouse_ms - static_cast<float>(effect.at_ms);
				}

				if (held && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f))
				{
					const float at_ms = mouse_ms - grab_ms;
					effect.at_ms = static_cast<std::uint32_t>(at_ms < 0.0f ? 0.0f : at_ms > length ? length : at_ms);
					effects_set(i, effect);
				}
				else if (ImGui::IsItemDeactivated() && !ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f) && over)
				{
					theater_seek(effect.at_ms);
				}

				if (end_over || end_held)
				{
					ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
				}

				if (over || held || end_over || end_held)
				{
					char at[32], until[32];
					format_time(at, sizeof(at), effect.at_ms);
					format_time(until, sizeof(until), effect.at_ms + effect.length_ms);
					ImGui::SetTooltip("%s\n%s to %s\nDrag: earlier or later.  Drag its right end: longer or shorter.  Click: go here.", effect.name, at, until);
				}

				ImGui::PopID();
				const unsigned fill = effect.on ? P.warn : P.muted;
				draw->AddRectFilled({x0, y + 2.0f}, {x1, y + frame - 2.0f}, u32(fill, over || held ? 0.55f : 0.35f), 3.0f);
				draw->AddRect({x0, y + 2.0f}, {x1, y + frame - 2.0f}, u32(fill), 3.0f);
				draw->AddRectFilled({x1 - 3.0f, y + 2.0f}, {x1, y + frame - 2.0f}, u32(fill));
				ImGui::PushClipRect({x0 + 4.0f, y}, {x1 - 4.0f, y + frame}, true);
				draw->AddText({x0 + 6.0f, y + style.FramePadding.y}, u32(P.text), shown);
				ImGui::PopClipRect();
			}
		}

		if (clear >= 0)
		{
			look_track_clear(clear);
		}
		else if (remove_key >= 0)
		{
			look_track_remove_key(remove_track, remove_key);
		}

		// the playhead over all of it
		const float head = std::floor(x_of(scrubbing ? scrub_ms : static_cast<float>(state.position_ms)));
		if (in_view(head))
		{
			draw->AddLine({head, origin.y}, {head, rows_bottom}, u32(P.text), 2.0f);
			draw->AddTriangleFilled({head - frame * 0.3f, origin.y}, {head + frame * 0.3f, origin.y}, {head, origin.y + frame * 0.5f}, u32(P.text));
		}
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
		// (the timeline grows by a row for each setting with keyframes past the four it has room for)
		PlacedEffect all_effects[32];
		const int timeline_rows = look_track_count() + effects_placed(all_effects, 32);
		const int more_rows = timeline_rows > 4 ? timeline_rows - 4 : 0;
		const float grown = std::floor(g_font_size * 14.0f) + static_cast<float>(more_rows) * (ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y * 0.5f);
		// the panel's width and the timeline's height are the user's once the lines between are dragged
		// (0: not yet), with a gap between the three for the mouse to take hold of
		static float panel_set = 0.0f, timeline_set = 0.0f;
		const float gap = 6.0f;
		float panel = panel_set > 0.0f ? panel_set : std::floor(g_font_size * 26.0f);
		float timeline = timeline_set > 0.0f ? timeline_set : (std::min)(grown, std::floor(room.y * 0.5f));
		panel = std::floor((std::max)(g_font_size * 16.0f, (std::min)(panel, room.x * 0.7f)));
		timeline = std::floor((std::max)(g_font_size * 7.0f, (std::min)(timeline, room.y * 0.75f)));
		const ImVec2 view = {room.x - panel - gap, room.y - timeline - gap};
		const ImVec2 corner = ImGui::GetCursorScreenPos();
		draw_view(camera, view);
		ImGui::SetCursorScreenPos({corner.x + view.x + gap, corner.y});
		draw_panel(state, camera, {panel, view.y});
		ImGui::SetCursorScreenPos({corner.x, corner.y + view.y + gap});
		draw_timeline(state, camera, {room.x, timeline});
		ImDrawList* draw = ImGui::GetWindowDrawList();
		ImGui::SetCursorScreenPos({corner.x + view.x, corner.y});
		ImGui::InvisibleButton("##splitPanel", {gap, view.y});
		const bool panel_hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
		if (ImGui::IsItemActive())
		{
			panel_set = panel - io.MouseDelta.x;
		}

		if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
		{
			panel_set = 0.0f;
		}

		ImGui::SetCursorScreenPos({corner.x, corner.y + view.y});
		ImGui::InvisibleButton("##splitTimeline", {room.x, gap});
		const bool timeline_hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
		if (ImGui::IsItemActive())
		{
			timeline_set = timeline - io.MouseDelta.y;
		}

		if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
		{
			timeline_set = 0.0f;
		}

		if (panel_hot || timeline_hot)
		{
			ImGui::SetMouseCursor(panel_hot ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
		}

		const float line_x = std::floor(corner.x + view.x + gap * 0.5f), line_y = std::floor(corner.y + view.y + gap * 0.5f);
		draw->AddLine({line_x, corner.y}, {line_x, corner.y + view.y}, u32(panel_hot ? P.accent : P.border), panel_hot ? 2.0f : 1.0f);
		draw->AddLine({corner.x, line_y}, {corner.x + room.x, line_y}, u32(timeline_hot ? P.accent : P.border), timeline_hot ? 2.0f : 1.0f);
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
			g_copy_format = plain;
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
			look_toggle_hud();
		}

		f3_was_down = f3;
		look_frame(state);
		sky_frame(g_device, g_context, state);
		models_frame(state);
		if (!state.playing)
		{
			g_hidden = false;
			camera_playback_ended();
		}

		const bool shown = state.playing && !g_hidden;
		set_mouse(shown);
		const bool recording = capture_wants_picture();
		const bool menu = !shown && !recording && !state.starting && theater_idle();
		g_menu_ui = menu;
		if (menu)
		{
			// at the game's menus: the recordings, drawn over the game's own picture
			g_typing = false;
			if (!targets(swap_chain, false))
			{
				return;
			}

			std::vector<Message> messages;
			{
				std::lock_guard<std::mutex> lock(g_messages_lock);
				messages.swap(g_messages);
			}

			for (const Message& m : messages)
			{
				ImGui_ImplWin32_WndProcHandler(m.window, m.message, m.wparam, m.lparam);
			}

			// the game's own pointer is the pointer here
			ImGui::GetIO().MouseDrawCursor = false;
			ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
			ImGui_ImplDX11_NewFrame();
			ImGui_ImplWin32_NewFrame();
			ImGui::NewFrame();
			draw_menu_browser(state);
			ImGui::Render();
			g_context->OMSetRenderTargets(1, &g_target, nullptr);
			ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
			return;
		}

		g_over_menu_ui = false;
		if ((!shown && !recording) || !targets(swap_chain, true))
		{
			g_typing = false;
			return;
		}

		// the game's picture as it was drawn, for the video
		capture_frame(state, g_device, g_context, g_copy, g_copy_format);
		if (!shown)
		{
			g_typing = false;
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

		ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
		ImGui::GetIO().MouseDrawCursor = !g_looking;
		ImGui_ImplDX11_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();
		draw_editor();
		g_typing = ImGui::GetIO().WantTextInput;
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

bool ui_section(const char* icon, const char* label, bool* reset, const bool open_by_default)
{
	return section(icon, label, reset, open_by_default);
}

void ui_row_label(const char* label)
{
	row_label(label);
}

float ui_font_size()
{
	return g_font_size;
}

ImFont* ui_bold_font()
{
	return g_bold;
}

void ui_not_built(const char* what)
{
	not_built(what);
}

void ui_log(const char* format, ...)
{
	char text[2100];
	va_list args;
	va_start(args, format);
	vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	theater_log(text);
}

bool ui_typing()
{
	return g_typing;
}

namespace
{
	bool in_windows_dxgi(const void* address)
	{
		HMODULE module = nullptr;
		char path[MAX_PATH] = {}, system[MAX_PATH] = {};
		if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		                        static_cast<LPCSTR>(address), &module) ||
		    !module || !GetModuleFileNameA(module, path, MAX_PATH) || !GetSystemDirectoryA(system, MAX_PATH))
		{
			return false;
		}

		strcat_s(system, "\\dxgi.dll");
		return _stricmp(path, system) == 0;
	}

	// The table of a swap chain's functions that are Windows' own. ReShade (and tools like it) hand the
	// game, and us, a swap chain of their own that does its effects and then calls the real one it keeps
	// inside. Hooked on ReShade's, the editor was drawn before the effects and they ran over it (the
	// depth of field blurring the panels, the depth at the picture's full size beside the smaller
	// picture). The real one is looked for inside it: a pointer to something whose table is in Windows'
	// dxgi.dll. Hooked there, the editor is drawn over the finished picture.
	void** real_swap_chain_table(IDXGISwapChain* swap_chain)
	{
		void** table = *reinterpret_cast<void***>(swap_chain);
		if (in_windows_dxgi(table))
		{
			log("editor: the swap chain is Windows' own");
			return table;
		}

		auto** members = reinterpret_cast<void**>(swap_chain);
		for (int i = 1; i < 48; ++i)
		{
			if (IsBadReadPtr(members + i, sizeof(void*)))
			{
				break;
			}

			void* inner = members[i];
			if (!inner || IsBadReadPtr(inner, sizeof(void*)))
			{
				continue;
			}

			void** inner_table = *reinterpret_cast<void***>(inner);
			if (inner_table && !IsBadReadPtr(inner_table, sizeof(void*) * 14) && in_windows_dxgi(inner_table) && in_windows_dxgi(inner_table[8]))
			{
				log("editor: the swap chain is another tool's (ReShade or the like); Windows' own was found inside it at +0x%X", i * 8);
				return inner_table;
			}
		}

		log("editor: the swap chain is another tool's and Windows' own was not found inside it; its effects will run over the editor");
		return table;
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
		void** table = real_swap_chain_table(swap_chain);
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
