// S1iris' panel: IrisStudio's look (the Graphite palette of core/src/UI/Palettes.hpp), drawn over the game
// with Dear ImGui on the game's own Direct3D 11 swap chain.
//
// The game's Present is found from a throwaway swap chain's table (every swap chain of the process shares
// the code) and hooked; the panel is drawn there, on the thread the game draws on. The game's window gets
// a second window procedure in front of its own, which only keeps the messages for the drawing thread.
//
// F1 gives the mouse to the panel (and back).
#include "ui_parts.hpp"
#include "passes.hpp"

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
#include <cstdint>
#include <cstring>
#include <iterator>
#include <mutex>
#include <set>
#include <utility>
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

	// The placed effects are shown by kind (all the rain together, all of one explosion together): which kinds
	// are opened out in the timeline, and which effects are selected there (moved, stretched, removed as one).
	std::set<std::string> g_fx_open_kinds;
	std::set<int> g_fx_selected;

	// The placed effects' places in the list, kind by kind, in the order the kinds were first placed.
	std::vector<std::pair<std::string, std::vector<int>>> fx_kinds(const PlacedEffect* placed, const int count)
	{
		std::vector<std::pair<std::string, std::vector<int>>> kinds;
		for (int i = 0; i < count; ++i)
		{
			bool found = false;
			for (auto& kind : kinds)
			{
				if (kind.first == placed[i].name)
				{
					kind.second.push_back(i);
					found = true;
					break;
				}
			}

			if (!found)
			{
				kinds.push_back({placed[i].name, {i}});
			}
		}

		return kinds;
	}

	// How many rows the effects take in the timeline: one a kind, and one for each effect of a kind opened out.
	int fx_timeline_rows()
	{
		static PlacedEffect placed[EFFECTS_MAX];
		const int count = effects_placed(placed, EFFECTS_MAX);
		int rows = 0;
		for (const auto& kind : fx_kinds(placed, count))
		{
			rows += 1 + (g_fx_open_kinds.count(kind.first) ? static_cast<int>(kind.second.size()) : 0);
		}

		return rows;
	}

	// (the list's places change when one is removed)
	void remove_effects(std::vector<int> indices)
	{
		std::sort(indices.begin(), indices.end());
		for (auto index = indices.rbegin(); index != indices.rend(); ++index)
		{
			effects_remove(*index);
		}

		g_fx_selected.clear();
	}

	// ---- actors: which one's settings are open in the Scene tab, which of its clips is chosen, and the entity
	// the Models list is showing (an actor's handle can be put there from the Actors list)
	int g_actor_open = -1, g_actor_clip = -1;
	int g_models_entity = -1;
	// the animation library: for which actor and which of its clips (-1: a new one after its last, "Then")
	struct Library
	{
		bool open = false;
		int actor = -1, clip = -1;
		char search[48] = "";
		int group = 0;
		char chosen[64] = "";
	};
	Library g_library;

	bool is_actor_handle(const int entity, char name[32] = nullptr)
	{
		static Actor actors[ACTORS_MAX];
		const int count = actors_list(actors, ACTORS_MAX);
		for (int i = 0; i < count; ++i)
		{
			if (actors[i].handle == entity)
			{
				if (name)
				{
					strncpy_s(name, 32, actors[i].name, _TRUNCATE);
				}

				return true;
			}
		}

		return false;
	}

	// An actor put at a place, facing the camera, dressed as the player nearest the camera (or any player whose
	// model the game has made).
	int add_actor_at(const float point[3])
	{
		const BoneState players = camera_bone_state();
		float eye[3];
		camera_view_origin(eye);
		const float yaw = std::atan2(eye[1] - point[1], eye[0] - point[0]) * 57.29578f;
		const TheaterState state = theater_state();
		int index = players.nearest >= 0 ? actors_add(point, yaw, players.nearest, state.position_ms) : -1;
		for (int e = 0; index < 0 && e < 64; ++e)
		{
			if (players.entities & (1ull << e))
			{
				index = actors_add(point, yaw, e, state.position_ms);
			}
		}

		if (index < 0)
		{
			ui_log("No actor added: an actor is dressed as a player, and no player's model is in the match right now (or all %d actors are in use).", ACTORS_MAX);
		}
		else
		{
			g_actor_open = index;
			g_actor_clip = 0;
		}

		return index;
	}

	// A clip for an animation after an actor's last ("Then"), or the animation of one of its clips changed.
	void use_animation(const int actor_index, const int clip_index, const char* animation)
	{
		static Actor actors[ACTORS_MAX];
		const int count = actors_list(actors, ACTORS_MAX);
		if (actor_index < 0 || actor_index >= count)
		{
			return;
		}

		Actor& actor = actors[actor_index];
		bool loops = false;
		const float length = actors_animation_length(animation, &loops);
		// (as long as the animation; one that goes round, twice round and two seconds at the least)
		const std::uint32_t runs = static_cast<std::uint32_t>((loops ? (std::max)(2.0f, length * 2.0f) : (std::max)(0.2f, length)) * 1000.0f);
		if (clip_index >= 0 && clip_index < actor.clip_count)
		{
			strncpy_s(actor.clips[clip_index].anim, animation, _TRUNCATE);
			actor.clips[clip_index].length_ms = runs;
			actor.clips[clip_index].loop = -1;
			g_actor_clip = clip_index;
		}
		else if (actor.clip_count < ACTOR_CLIPS_MAX)
		{
			ActorClip clip;
			strncpy_s(clip.anim, animation, _TRUNCATE);
			clip.length_ms = runs;
			// where the last one ends (the recording's time now for the first)
			std::uint32_t end = theater_state().position_ms;
			for (int c = 0; c < actor.clip_count; ++c)
			{
				end = c == 0 ? actor.clips[c].at_ms + actor.clips[c].length_ms : (std::max)(end, actor.clips[c].at_ms + actor.clips[c].length_ms);
			}

			clip.at_ms = end;
			g_actor_clip = actor.clip_count;
			actor.clips[actor.clip_count++] = clip;
		}

		actors_set(actor_index, actor);
	}

	// The small bone view: the figure's bones as lines, seen a little from the side.
	void draw_bone_view(const ImVec2 size)
	{
		const ImVec2 corner = ImGui::GetCursorScreenPos();
		ImDrawList* draw = ImGui::GetWindowDrawList();
		ImGui::Dummy(size);
		draw->AddRectFilled(corner, {corner.x + size.x, corner.y + size.y}, u32(P.surface), 4.0f);
		draw->AddRect(corner, {corner.x + size.x, corner.y + size.y}, u32(P.border), 4.0f);
		float bones[ACTOR_BONES][3];
		bool has[ACTOR_BONES];
		float fraction = 0.0f;
		if (!actors_preview_bones(bones, has, &fraction))
		{
			const char* text = theater_state().playing ? "Getting it ready..." : "Play a recording to see it";
			const ImVec2 text_size = ImGui::CalcTextSize(text);
			draw->AddText({corner.x + (size.x - text_size.x) * 0.5f, corner.y + (size.y - text_size.y) * 0.5f}, u32(P.muted), text);
			return;
		}

		// (a figure is about 72 high; room for one fallen over, too)
		const float scale = (size.y - 28.0f) / 96.0f;
		const float turn = 0.6f, c = std::cos(turn), sn = std::sin(turn);
		const ImVec2 feet = {corner.x + size.x * 0.5f, corner.y + size.y - 18.0f};
		auto at = [&](const float* bone) {
			const float side = bone[0] * sn + bone[1] * c, depth = bone[0] * c - bone[1] * sn;
			return ImVec2(feet.x - side * scale, feet.y - (bone[2] + depth * 0.18f) * scale);
		};
		static const int LINKS[][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 4}, {4, 5}, {3, 6}, {6, 7}, {7, 8}, {8, 9}, {3, 10}, {10, 11}, {11, 12}, {12, 13},
		                               {0, 14}, {14, 15}, {15, 16}, {16, 17}, {0, 18}, {18, 19}, {19, 20}, {20, 21}};
		ImGui::PushClipRect(corner, {corner.x + size.x, corner.y + size.y}, true);
		draw->AddLine({corner.x + 10.0f, feet.y}, {corner.x + size.x - 10.0f, feet.y}, u32(P.border), 1.0f);
		for (const auto& link : LINKS)
		{
			if (has[link[0]] && has[link[1]])
			{
				// (the left side a little fainter, to tell the sides apart)
				const bool left_side = (link[1] >= 6 && link[1] <= 9) || (link[1] >= 14 && link[1] <= 17);
				draw->AddLine(at(bones[link[0]]), at(bones[link[1]]), u32(P.text, left_side ? 0.55f : 1.0f), 2.5f);
			}
		}

		if (has[5])
		{
			draw->AddCircleFilled(at(bones[5]), 5.0f * (std::max)(1.0f, scale), u32(P.accent));
		}

		draw->AddRectFilled({corner.x + 8.0f, corner.y + size.y - 8.0f}, {corner.x + 8.0f + (size.x - 16.0f) * fraction, corner.y + size.y - 5.0f}, u32(P.accent), 1.5f);
		ImGui::PopClipRect();
	}

	// The animation library: the animations the game has loaded, by group and by a text in their names; the one
	// under the mouse (or chosen with a click) plays in the small bone view; a double click or "Use it" takes it.
	void draw_library()
	{
		if (g_library.open)
		{
			g_library.open = false;
			ImGui::OpenPopup("Animations");
		}

		ImGui::SetNextWindowSize({g_font_size * 46.0f, g_font_size * 30.0f}, ImGuiCond_Appearing);
		bool keep = true;
		if (!ImGui::BeginPopupModal("Animations", &keep, ImGuiWindowFlags_NoSavedSettings))
		{
			return;
		}

		static char names[3000][64];
		static int count = 0, shown_group = -1;
		static char shown_search[48] = "\x01";
		static char playing[64] = "";
		ImGui::SetNextItemWidth(g_font_size * 11.0f);
		ImGui::Combo("##group", &g_library.group, "Player's body\0Deaths\0Standing\0Crouching\0Prone\0Moving\0Everything loaded\0");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(g_font_size * 14.0f);
		ImGui::InputTextWithHint("##search", ICON_SEARCH "  a text in its name", g_library.search, sizeof(g_library.search));
		if (shown_group != g_library.group || strcmp(shown_search, g_library.search) != 0 || ImGui::IsWindowAppearing())
		{
			shown_group = g_library.group;
			snprintf(shown_search, sizeof(shown_search), "%s", g_library.search);
			count = actors_animations(g_library.search, g_library.group, names, 3000);
		}

		ImGui::SameLine();
		ImGui::TextDisabled("%d", count);
		const float view_width = g_font_size * 17.0f;
		bool use = false;
		ImGui::BeginChild("##list", {ImGui::GetContentRegionAvail().x - view_width - ImGui::GetStyle().ItemSpacing.x, -ImGui::GetFrameHeightWithSpacing() * 1.2f},
		                  ImGuiChildFlags_Border);
		const char* hovered = nullptr;
		ImGuiListClipper clipper;
		clipper.Begin(count);
		while (clipper.Step())
		{
			for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
			{
				if (ImGui::Selectable(names[i], strcmp(names[i], g_library.chosen) == 0, ImGuiSelectableFlags_AllowDoubleClick))
				{
					snprintf(g_library.chosen, sizeof(g_library.chosen), "%s", names[i]);
					use = use || ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
				}

				if (ImGui::IsItemHovered())
				{
					hovered = names[i];
				}
			}
		}

		if (count == 0)
		{
			ImGui::TextDisabled(theater_state().playing ? "No animation of this group has that in its name." : "Play a recording: the animations are the ones its match has loaded.");
		}

		ImGui::EndChild();
		ImGui::SameLine();
		ImGui::BeginGroup();
		// (what plays: the one under the mouse, else the one chosen)
		const char* wanted = hovered ? hovered : g_library.chosen;
		if (strcmp(playing, wanted) != 0)
		{
			snprintf(playing, sizeof(playing), "%s", wanted);
		}

		actors_preview(playing, g_library.actor);
		draw_bone_view({view_width, g_font_size * 20.0f});
		if (playing[0])
		{
			bool loops = false;
			const float length = actors_animation_length(playing, &loops);
			ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + view_width);
			ImGui::TextUnformatted(playing);
			ImGui::TextDisabled("%.2f s%s", length, loops ? ", goes round and round" : "");
			ImGui::PopTextWrapPos();
		}
		else
		{
			ImGui::TextDisabled("Point at an animation to see it.");
		}

		ImGui::EndGroup();
		ImGui::BeginDisabled(!g_library.chosen[0]);
		use = ImGui::Button(g_library.clip < 0 ? "Then play it" : "Use it") || use;
		ImGui::EndDisabled();
		ImGui::SameLine();
		const bool cancel = ImGui::Button("Cancel");
		ImGui::SameLine();
		ImGui::TextDisabled(g_library.clip < 0 ? "It is added after the actor's last animation, and blends in from it." : "Double click: use it.");
		if (use && g_library.chosen[0])
		{
			use_animation(g_library.actor, g_library.clip, g_library.chosen);
		}

		if (use || cancel || !keep)
		{
			actors_preview("", -1);
			playing[0] = 0;
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	}

	// Where a place in the match is on the picture (false: behind the camera).
	struct ViewMap
	{
		float origin[3], axis[9], tan[2];
		ImVec2 at, size;
	};

	bool on_picture(const ViewMap& view, const float point[3], ImVec2* out)
	{
		const float d[3] = {point[0] - view.origin[0], point[1] - view.origin[1], point[2] - view.origin[2]};
		const float forward = d[0] * view.axis[0] + d[1] * view.axis[1] + d[2] * view.axis[2];
		if (forward < 4.0f)
		{
			return false;
		}

		const float left = d[0] * view.axis[3] + d[1] * view.axis[4] + d[2] * view.axis[5];
		const float up = d[0] * view.axis[6] + d[1] * view.axis[7] + d[2] * view.axis[8];
		out->x = view.at.x + view.size.x * (0.5f - 0.5f * (left / forward) / view.tan[0]);
		out->y = view.at.y + view.size.y * (0.5f - 0.5f * (up / forward) / view.tan[1]);
		return true;
	}

	float distance_to_segment(const ImVec2 p, const ImVec2 a, const ImVec2 b)
	{
		const float vx = b.x - a.x, vy = b.y - a.y, wx = p.x - a.x, wy = p.y - a.y;
		const float length = vx * vx + vy * vy;
		float t = length > 0.0001f ? (wx * vx + wy * vy) / length : 0.0f;
		t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
		const float dx = a.x + vx * t - p.x, dy = a.y + vy * t - p.y;
		return std::sqrt(dx * dx + dy * dy);
	}

	// The handles on what the editor has put into the match (its effects and actors): three arrows to slide it
	// along the match's axes (red x, green y, blue z up), or, in the turning mode, three rings to turn it about
	// them. Shown while the mouse is over the picture and the right button is not held. True while one is held.
	bool g_handles_turn = false;
	bool draw_handles(const ImVec2 at, const ImVec2 size, const bool hovered)
	{
		struct Thing
		{
			int kind;  // 0 an effect, 1 an actor
			int index;
			float origin[3];   // where its handle is
			float angles[3];   // up/down, around, roll
			const char* name;
		};
		// a drag under way
		static bool held = false;
		static int held_kind = 0, held_index = 0, held_axis = 0;
		static bool held_turn = false;
		static float held_origin[3] = {}, held_angles[3] = {};
		static float held_angle = 0.0f;
		static PlacedEffect effects[EFFECTS_MAX];
		static Actor actors[ACTORS_MAX];
		const ImGuiIO& io = ImGui::GetIO();
		if (held && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
		{
			held = false;
		}

		if ((!hovered && !held) || ImGui::IsMouseDown(ImGuiMouseButton_Right) || size.x < 8.0f || !theater_state().playing)
		{
			return held;
		}

		ViewMap view;
		camera_view(view.origin, view.axis, view.tan);
		view.at = at;
		view.size = size;
		if (view.tan[0] < 0.01f || view.tan[1] < 0.01f)
		{
			return held;
		}

		std::vector<Thing> things;
		const int effect_count = effects_placed(effects, EFFECTS_MAX);
		for (int i = 0; i < effect_count; ++i)
		{
			if (!effects[i].follow)
			{
				Thing thing = {0, i, {}, {}, effects[i].name};
				memcpy(thing.origin, effects[i].origin, sizeof(thing.origin));
				memcpy(thing.angles, effects[i].angles, sizeof(thing.angles));
				things.push_back(thing);
			}
		}

		const int actor_count = actors_list(actors, ACTORS_MAX);
		for (int i = 0; i < actor_count; ++i)
		{
			// (where it stands now: its animations may have carried it from where it was put)
			ActorNow standing;
			Thing thing = {1, i, {}, {0.0f, actors[i].yaw, 0.0f}, actors[i].name};
			memcpy(thing.origin, actors[i].origin, sizeof(thing.origin));
			if (actors_now(i, &standing) && standing.shown)
			{
				memcpy(thing.origin, standing.origin, sizeof(thing.origin));
			}

			things.push_back(thing);
		}

		ImDrawList* draw = ImGui::GetWindowDrawList();
		static const unsigned COLOURS[3] = {0xFF5050E6u, 0xFF50C850u, 0xFFE68C50u};  // (ABGR) red, green, blue
		static const float AXES[3][3] = {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
		int over_thing = -1, over_axis = -1;
		float over_distance = 9.0f;
		ImGui::PushClipRect(at, {at.x + size.x, at.y + size.y}, true);
		for (size_t t = 0; t < things.size(); ++t)
		{
			const Thing& thing = things[t];
			ImVec2 centre;
			if (!on_picture(view, thing.origin, &centre))
			{
				continue;
			}

			// (as long on the picture whatever the distance: about a tenth of its height)
			const float d[3] = {thing.origin[0] - view.origin[0], thing.origin[1] - view.origin[1], thing.origin[2] - view.origin[2]};
			const float away = d[0] * view.axis[0] + d[1] * view.axis[1] + d[2] * view.axis[2];
			const float reach = away * view.tan[1] * 0.22f;
			const bool this_held = held && held_kind == thing.kind && held_index == thing.index;
			if (held && !this_held)
			{
				continue;
			}

			const bool turning = this_held ? held_turn : g_handles_turn;
			for (int a = 0; a < 3; ++a)
			{
				// (an actor only turns about the upright)
				if (turning && thing.kind == 1 && a != 2)
				{
					continue;
				}

				const bool lit = this_held && held_axis == a;
				if (!turning)
				{
					const float tip_at[3] = {thing.origin[0] + AXES[a][0] * reach, thing.origin[1] + AXES[a][1] * reach, thing.origin[2] + AXES[a][2] * reach};
					ImVec2 tip;
					if (!on_picture(view, tip_at, &tip))
					{
						continue;
					}

					const float near_it = distance_to_segment(io.MousePos, centre, tip);
					if (!held && near_it < over_distance)
					{
						over_distance = near_it;
						over_thing = static_cast<int>(t);
						over_axis = a;
					}

					draw->AddLine(centre, tip, COLOURS[a], lit ? 4.0f : 2.5f);
					// the arrow's head
					const float vx = tip.x - centre.x, vy = tip.y - centre.y, length = std::sqrt(vx * vx + vy * vy);
					if (length > 6.0f)
					{
						const float ux = vx / length, uy = vy / length;
						draw->AddTriangleFilled(tip, {tip.x - ux * 9.0f - uy * 4.5f, tip.y - uy * 9.0f + ux * 4.5f}, {tip.x - ux * 9.0f + uy * 4.5f, tip.y - uy * 9.0f - ux * 4.5f},
						                        COLOURS[a]);
					}
				}
				else
				{
					// a ring about the axis
					const float* u = AXES[(a + 1) % 3];
					const float* v = AXES[(a + 2) % 3];
					ImVec2 ring[33];
					bool whole = true;
					for (int k = 0; k <= 32 && whole; ++k)
					{
						const float angle = k * (6.2831853f / 32.0f), cu = std::cos(angle) * reach * 0.8f, cv = std::sin(angle) * reach * 0.8f;
						const float on_ring[3] = {thing.origin[0] + u[0] * cu + v[0] * cv, thing.origin[1] + u[1] * cu + v[1] * cv, thing.origin[2] + u[2] * cu + v[2] * cv};
						whole = on_picture(view, on_ring, &ring[k]);
					}

					if (!whole)
					{
						continue;
					}

					for (int k = 0; k < 32; ++k)
					{
						const float near_it = distance_to_segment(io.MousePos, ring[k], ring[k + 1]);
						if (!held && near_it < over_distance)
						{
							over_distance = near_it;
							over_thing = static_cast<int>(t);
							over_axis = a;
						}
					}

					draw->AddPolyline(ring, 33, COLOURS[a], 0, lit ? 4.0f : 2.5f);
				}
			}

			draw->AddCircleFilled(centre, 4.0f, u32(P.text));
			if (this_held || (!held && over_thing == static_cast<int>(t)))
			{
				const char* slash = strrchr(thing.name, '/');
				draw->AddText({centre.x + 8.0f, centre.y + 6.0f}, u32(P.text), slash ? slash + 1 : thing.name);
			}
		}

		ImGui::PopClipRect();
		if (!held && over_thing >= 0)
		{
			ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
			if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
			{
				const Thing& thing = things[static_cast<size_t>(over_thing)];
				held = true;
				held_kind = thing.kind;
				held_index = thing.index;
				held_axis = over_axis;
				held_turn = g_handles_turn;
				// (an actor is slid from where it was put, not from where its animations have carried it)
				if (thing.kind == 1)
				{
					memcpy(held_origin, actors[thing.index].origin, sizeof(held_origin));
					g_actor_open = thing.index;
				}
				else
				{
					memcpy(held_origin, thing.origin, sizeof(held_origin));
				}

				memcpy(held_angles, thing.angles, sizeof(held_angles));
				ImVec2 centre;
				on_picture(view, thing.origin, &centre);
				held_angle = std::atan2(io.MousePos.y - centre.y, io.MousePos.x - centre.x);
			}
		}

		if (held)
		{
			// what is held, as it is now
			const Thing* thing = nullptr;
			for (const Thing& each : things)
			{
				if (each.kind == held_kind && each.index == held_index)
				{
					thing = &each;
				}
			}

			ImVec2 centre;
			if (!thing || !on_picture(view, thing->origin, &centre))
			{
				held = false;
				return false;
			}

			float origin[3], angles[3];
			memcpy(origin, held_origin, sizeof(origin));
			memcpy(angles, held_angles, sizeof(angles));
			if (!held_turn)
			{
				// along the axis by as much as the mouse has gone along the arrow on the picture
				const float d[3] = {thing->origin[0] - view.origin[0], thing->origin[1] - view.origin[1], thing->origin[2] - view.origin[2]};
				const float away = d[0] * view.axis[0] + d[1] * view.axis[1] + d[2] * view.axis[2];
				const float reach = away * view.tan[1] * 0.22f;
				const float tip_at[3] = {thing->origin[0] + AXES[held_axis][0] * reach, thing->origin[1] + AXES[held_axis][1] * reach,
				                         thing->origin[2] + AXES[held_axis][2] * reach};
				ImVec2 tip;
				if (on_picture(view, tip_at, &tip))
				{
					const float vx = tip.x - centre.x, vy = tip.y - centre.y, length = vx * vx + vy * vy;
					if (length > 4.0f)
					{
						// (the arrow's length changes as the thing moves: taken a frame at a time)
						const float step = ((io.MouseDelta.x) * vx + (io.MouseDelta.y) * vy) / length * reach;
						for (int k = 0; k < 3; ++k)
						{
							held_origin[k] += AXES[held_axis][k] * step;
							origin[k] = held_origin[k];
						}
					}
				}
			}
			else
			{
				// about the axis by the angle the mouse has gone round the thing on the picture
				const float angle = std::atan2(io.MousePos.y - centre.y, io.MousePos.x - centre.x);
				float by = (angle - held_angle) * 57.29578f;
				by = by > 180.0f ? by - 360.0f : by < -180.0f ? by + 360.0f : by;
				held_angle = angle;
				// (the way round as seen: the other way when the axis points away from the camera)
				const float toward = AXES[held_axis][0] * view.axis[0] + AXES[held_axis][1] * view.axis[1] + AXES[held_axis][2] * view.axis[2];
				by = toward > 0.0f ? by : -by;
				// (the game's angles: up/down about y, around about z, roll about x)
				const int which = held_axis == 2 ? 1 : held_axis == 1 ? 0 : 2;
				held_angles[which] = std::remainder(held_angles[which] + by, 360.0f);
				memcpy(angles, held_angles, sizeof(angles));
			}

			if (held_kind == 0 && held_index < effect_count)
			{
				PlacedEffect changed = effects[held_index];
				memcpy(changed.origin, origin, sizeof(changed.origin));
				memcpy(changed.angles, angles, sizeof(changed.angles));
				effects_set(held_index, changed);
			}
			else if (held_kind == 1 && held_index < actor_count)
			{
				Actor changed = actors[held_index];
				memcpy(changed.origin, origin, sizeof(changed.origin));
				changed.yaw = angles[1];
				actors_set(held_index, changed);
			}
		}

		return held;
	}

	// An effect put at a place, facing off the surface there.
	void add_effect_at(const char* name, const float point[3], const float normal[3])
	{
		const int index = effects_add(name, point, theater_state().position_ms);
		PlacedEffect placed[EFFECTS_MAX];
		const int count = effects_placed(placed, EFFECTS_MAX);
		if (index >= 0 && index < count)
		{
			placed[index].angles[0] = std::atan2(-normal[2], std::sqrt(normal[0] * normal[0] + normal[1] * normal[1])) * 57.29578f;
			placed[index].angles[1] = std::atan2(normal[1], normal[0]) * 57.29578f;
			placed[index].angles[2] = 0.0f;
			effects_set(index, placed[index]);
		}

		snprintf(g_fx_last, sizeof(g_fx_last), "%s", name);
	}

	// an effect copied, with its settings: pasted, it is the same effect somewhere else (foliage, fires...)
	PlacedEffect g_fx_clip;
	bool g_fx_copied = false;

	void paste_effect_at(const float point[3])
	{
		const int index = effects_add(g_fx_clip.name, point, g_fx_clip.at_ms);
		PlacedEffect placed[EFFECTS_MAX];
		const int count = effects_placed(placed, EFFECTS_MAX);
		if (index >= 0 && index < count)
		{
			PlacedEffect copy = g_fx_clip;
			memcpy(copy.origin, point, sizeof(copy.origin));
			copy.follow = false;
			effects_set(index, copy);
		}
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

		if (ImGui::MenuItem(ICON_PERSON "  Add an actor here"))
		{
			add_actor_at(point);
		}

		ImGui::SetItemTooltip("A figure of the editor's own, dressed as the player nearest the camera: its outfit and its animations are set in the Scene tab");

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
		if (g_fx_copied)
		{
			const char* slash = strrchr(g_fx_clip.name, '/');
			char label[128];
			snprintf(label, sizeof(label), "Paste %s here", slash ? slash + 1 : g_fx_clip.name);
			if (ImGui::MenuItem(label))
			{
				paste_effect_at(point);
			}
		}

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

		PlacedEffect placed[EFFECTS_MAX];
		const int placed_count = effects_placed(placed, EFFECTS_MAX);
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

	float g_lane_scale = 1.0f;  // how tall the timeline's rows are, against a line of text

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

		// the handles on the editor's effects and actors: slide them, or turn them
		ImGui::SameLine();
		if (ImGui::Button(g_handles_turn ? ICON_ROTATE "  Turn" : "Slide"))
		{
			g_handles_turn = !g_handles_turn;
		}

		ImGui::SetItemTooltip("What the handles on your effects and actors do when dragged: slide along an axis, or turn about one.\nThe handles show while the mouse is over the picture. R switches between the two.");
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

		const bool handle_held = draw_handles(at, picture, hovered);
		if (hovered && !handle_held && !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_R, false))
		{
			g_handles_turn = !g_handles_turn;
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
				: "Hold the right mouse button to look  \xe2\x80\xa2  right click to place a light, an effect or an actor  \xe2\x80\xa2  W A S D to fly  \xe2\x80\xa2  K adds a campath node";
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
			for (int i = 0; i < static_cast<int>(sizeof(MODES) / sizeof(MODES[0])); ++i)
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
			ImGui::SetNextItemWidth(-look_key_width());
			if (ImGui::SliderFloat("##fov", &fov, 5.0f, 160.0f, "%.1f"))
			{
				camera_set_fov(fov);
				look_key_changed("camera_fov", &fov);
			}

			// (a keyframe: with the bone camera the view can zoom as the recording plays)
			look_key_button("camera_fov", "Field of view", 1, &fov);
			ImGui::EndDisabled();
			if (camera.mode == CameraMode::Free || camera.mode == CameraMode::Bone)
			{
				// (the bone camera has a fly speed of its own: it is flown close to a player)
				float speed = camera.speed;
				row_label("Fly speed");
				if (ImGui::SliderFloat("##speed", &speed, camera.mode == CameraMode::Bone ? 2.0f : 50.0f, 3000.0f, "%.0f", ImGuiSliderFlags_Logarithmic))
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

			// (Offset and Rotation can have keyframes: the camera moves and looks around on the bone as the
			// recording plays; flown with the mouse, press the diamond to keep where it is now)
			row_label("Offset");
			ImGui::SetNextItemWidth(-look_key_width());
			if (ImGui::DragFloat3("##boneOffset", wanted.offset, 0.25f, -2000.0f, 2000.0f, "%.1f"))
			{
				changed = true;
				look_key_changed("bone_offset", wanted.offset);
			}

			ImGui::SetItemTooltip("Forward, right, up, along the bone's own directions");
			look_key_button("bone_offset", "Bone camera offset", 3, wanted.offset);
			row_label("Rotation");
			ImGui::SetNextItemWidth(-look_key_width());
			if (ImGui::DragFloat3("##boneTurn", wanted.turn, 0.5f, -720.0f, 720.0f, "%.1f"))
			{
				changed = true;
				look_key_changed("bone_turn", wanted.turn);
			}

			ImGui::SetItemTooltip("Up and down, around, roll");
			look_key_button("bone_turn", "Bone camera rotation", 3, wanted.turn);
			if (ImGui::Button("Add a camera point here"))
			{
				// the three at once: where the camera is on the bone, where it looks, and how wide
				look_key_add("bone_offset", "Bone camera offset", 3, wanted.offset);
				look_key_add("bone_turn", "Bone camera rotation", 3, wanted.turn);
				float wide = camera.fov;
				look_key_add("camera_fov", "Field of view", 1, &wide);
			}

			ImGui::SetItemTooltip("Keeps the bone camera's offset, rotation and field of view at this time of the recording.\nBetween two points the camera glides from one to the other. They show on the timeline.");
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
				static PlacedEffect placed[EFFECTS_MAX];
				static bool live[EFFECTS_MAX];
				const int placed_count = effects_placed(placed, EFFECTS_MAX);
				effects_live(live, EFFECTS_MAX);
				std::vector<int> remove_fx;
				// the placed effects by kind, a line each inside; a click on one opens its settings under it
				static int fx_open = -1, fx_order = 0;
				if (placed_count > 0)
				{
					ImGui::Separator();
					ImGui::AlignTextToFramePadding();
					ImGui::TextDisabled("%d placed", placed_count);
					ImGui::SameLine();
					ImGui::SetNextItemWidth(g_font_size * 9.0f);
					ImGui::Combo("##fxOrder", &fx_order, "As added\0By time\0");
					if (g_fx_copied)
					{
						ImGui::SameLine();
						if (ImGui::Button("Paste here"))
						{
							float here[3];
							camera_view_origin(here);
							paste_effect_at(here);
						}

						ImGui::SetItemTooltip("A copy of %s where the camera is.\nOr right click a place on the picture and choose Paste.", g_fx_clip.name);
					}

					ImGui::PushTextWrapPos(0.0f);
					ImGui::TextDisabled("An effect is in the game while the time is inside its clip on the timeline, and is taken away outside it.");
					ImGui::PopTextWrapPos();
				}

				int kind_number = 0;
				for (const auto& kind : fx_kinds(placed, placed_count))
				{
					std::vector<int> members = kind.second;
					if (fx_order == 1)
					{
						std::stable_sort(members.begin(), members.end(), [&](const int a, const int b) { return placed[a].at_ms < placed[b].at_ms; });
					}

					int on_count = 0, live_count = 0;
					for (const int i : members)
					{
						on_count += placed[i].on ? 1 : 0;
						live_count += live[i] ? 1 : 0;
					}

					const char* kind_slash = strrchr(kind.first.c_str(), '/');
					ImGui::PushID(4000 + kind_number++);
					bool all_on = on_count == static_cast<int>(members.size());
					if (ImGui::Checkbox("##allOn", &all_on))
					{
						for (const int i : members)
						{
							PlacedEffect changed = placed[i];
							changed.on = all_on;
							effects_set(i, changed);
						}
					}

					ImGui::SetItemTooltip("All of this kind on or off");
					ImGui::SameLine();
					char header[200];
					snprintf(header, sizeof(header), "%s (%d)%s###kind", kind_slash ? kind_slash + 1 : kind.first.c_str(), static_cast<int>(members.size()),
					         live_count ? "   in the game now" : "");
					// (one of a kind is shown opened out; several stay shut until asked for)
					ImGui::SetNextItemOpen(members.size() == 1, ImGuiCond_Once);
					const float trash_width = ImGui::CalcTextSize(ICON_TRASH).x + ImGui::GetStyle().FramePadding.x * 2.0f;
					const bool kind_open = ImGui::TreeNodeEx(header, ImGuiTreeNodeFlags_AllowOverlap | ImGuiTreeNodeFlags_SpanAvailWidth);
					if (ImGui::IsItemClicked())
					{
						// (the timeline shows them selected)
						g_fx_selected.clear();
						g_fx_selected.insert(members.begin(), members.end());
					}

					ImGui::SetItemTooltip("%s\n%d placed, %d switched on", kind.first.c_str(), static_cast<int>(members.size()), on_count);
					ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - trash_width);
					if (ImGui::SmallButton(ICON_TRASH "##kindRemove"))
					{
						remove_fx = members;
					}

					ImGui::SetItemTooltip(members.size() > 1 ? "Remove all %d of these" : "Remove it", static_cast<int>(members.size()));
					if (!kind_open)
					{
						ImGui::PopID();
						continue;
					}

					for (const int i : members)
					{
						PlacedEffect effect = placed[i];
						bool changed = false;
						ImGui::PushID(2000 + i);
						changed = ImGui::Checkbox("##on", &effect.on) || changed;
						ImGui::SameLine();
						if (ImGui::SmallButton(ICON_PLAY))
						{
							effects_fire(i);
						}

						ImGui::SetItemTooltip("Play it now: from its start again if it is in the game, and otherwise for a few seconds to look at");
						ImGui::SameLine();
						{
							char at[32], until[32], line[160];
							format_time(at, sizeof(at), effect.at_ms);
							format_time(until, sizeof(until), effect.at_ms + effect.length_ms);
							snprintf(line, sizeof(line), "%s to %s%s", at, until, live[i] ? "   in the game now" : "");
							const float buttons = ImGui::CalcTextSize("Copy").x + ImGui::CalcTextSize(ICON_TRASH).x + ImGui::GetStyle().FramePadding.x * 4.0f +
							                      ImGui::GetStyle().ItemSpacing.x * 2.0f;
							if (ImGui::Selectable(line, fx_open == i || g_fx_selected.count(i) != 0, 0, {ImGui::GetContentRegionAvail().x - buttons, 0.0f}))
							{
								fx_open = fx_open == i ? -1 : i;
								g_fx_selected.clear();
								g_fx_selected.insert(i);
							}

							ImGui::SetItemTooltip("%s\nClick: its settings", effect.name);
						}

						ImGui::SameLine();
						if (ImGui::SmallButton("Copy"))
						{
							g_fx_clip = effect;
							g_fx_copied = true;
						}

						ImGui::SetItemTooltip("Copy it: then right click places on the picture and choose Paste, or use Paste here");
						ImGui::SameLine();
						if (ImGui::SmallButton(ICON_TRASH))
						{
							remove_fx = {i};
						}

						if (fx_open != i)
						{
							if (changed)
							{
								effects_set(i, effect);
							}

							ImGui::PopID();
							continue;
						}

						ImGui::Indent();
						if (ImGui::SmallButton("Move to the camera"))
						{
							camera_view_origin(effect.origin);
							changed = true;
						}

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

						ImGui::SetItemTooltip("Its clip on the timeline: the effect is in the game exactly while the recording's time is inside it,\nand is taken away when the time leaves it, wherever the time came from.");
						{
							// how long the effect lasts by itself, and playing it again and again for one that ends
							const int lasts = effects_lasts(effect.name);
							if (lasts == 0)
							{
								ImGui::TextDisabled("This effect goes on by itself for as long as its clip.");
							}
							else
							{
								if (lasts > 0)
								{
									ImGui::TextDisabled("This effect lasts %.2f s by itself.", lasts / 1000.0f);
									if (effect.length_ms > static_cast<std::uint32_t>(lasts) && effect.every_ms == 0)
									{
										ImGui::SameLine();
										if (ImGui::SmallButton("Repeat it through its clip"))
										{
											effect.every_ms = static_cast<std::uint32_t>(lasts < 50 ? 50 : lasts);
											changed = true;
										}
									}
								}

								row_label("Again every");
								if (ImGui::DragFloat("##every", &again, 0.01f, 0.0f, 60.0f, again > 0.0f ? "%.2f s" : "never"))
								{
									effect.every_ms = static_cast<std::uint32_t>(again * 1000.0f);
									changed = true;
								}

								ImGui::SetItemTooltip("Played again this often while the time is in its clip (for effects that end by themselves)");
							}
						}

						row_label("Follow the camera");
						changed = ImGui::Checkbox("##follow", &effect.follow) || changed;
						ImGui::SetItemTooltip("It is put where the camera is when it begins (rain or mist around the view), not at a place of its own");
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
						ImGui::Unindent();
						if (changed)
						{
							effects_set(i, effect);
						}

						ImGui::PopID();
					}

					ImGui::TreePop();
					ImGui::PopID();
				}

				if (!remove_fx.empty())
				{
					remove_effects(remove_fx);
					fx_open = -1;
				}
			}

			if (section(ICON_PERSON, "Actors"))
			{
				// figures of the editor's own: dressed as a player, played by animations one after another
				static Actor actors[ACTORS_MAX];
				const int actor_count = actors_list(actors, ACTORS_MAX);
				const BoneState players = camera_bone_state();
				if (ImGui::Button(ICON_PLUS "  Add an actor"))
				{
					// where the camera is, on the ground under it as far as the editor can tell: at its feet
					float here[3];
					camera_view_origin(here);
					here[2] -= 60.0f;
					add_actor_at(here);
				}

				ImGui::SetItemTooltip("An actor where the camera is. Or right click a place on the picture and choose \"Add an actor here\".\nIt is dressed as the player nearest the camera; its outfit is changed like a player's, under Models.");
				ImGui::SameLine();
				ImGui::TextDisabled("%d of %d", actor_count, ACTORS_MAX);
				if (actor_count == 0)
				{
					ImGui::PushTextWrapPos(0.0f);
					ImGui::TextDisabled("An actor stands where you put it and plays the animations you give it, one after another (\"Then\"), "
					                    "blending from each into the next. Drag its arrows on the picture to slide it, its clips on the timeline to time them.");
					ImGui::PopTextWrapPos();
				}

				int remove_actor = -1;
				for (int i = 0; i < actor_count; ++i)
				{
					Actor actor = actors[i];
					ActorNow standing;
					const bool have_now = actors_now(i, &standing);
					bool changed = false;
					ImGui::PushID(6000 + i);
					changed = ImGui::Checkbox("##on", &actor.on) || changed;
					ImGui::SameLine();
					{
						char line[160];
						snprintf(line, sizeof(line), "%s   %d animation%s%s###actor", actor.name, actor.clip_count, actor.clip_count == 1 ? "" : "s",
						         have_now && standing.broken ? "   (a fault: change it to try again)" : have_now && standing.shown ? "   in the match now"
						         : have_now && standing.waiting[0] ? "   waiting" : "");
						const float buttons = ImGui::CalcTextSize(ICON_TRASH).x + ImGui::GetStyle().FramePadding.x * 2.0f + ImGui::GetStyle().ItemSpacing.x;
						if (ImGui::Selectable(line, g_actor_open == i, 0, {ImGui::GetContentRegionAvail().x - buttons, 0.0f}))
						{
							g_actor_open = g_actor_open == i ? -1 : i;
							g_actor_clip = -1;
						}
					}

					ImGui::SameLine();
					if (ImGui::SmallButton(ICON_TRASH))
					{
						remove_actor = i;
					}

					ImGui::SetItemTooltip("Remove this actor");
					if (g_actor_open != i)
					{
						if (changed)
						{
							actors_set(i, actor);
						}

						ImGui::PopID();
						continue;
					}

					ImGui::Indent();
					if (have_now && standing.waiting[0] && !standing.shown)
					{
						ImGui::PushTextWrapPos(0.0f);
						ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(u32(P.warn)), "Not in the match yet: %s is not in memory.", standing.waiting);
						ImGui::PopTextWrapPos();
					}

					row_label("Name");
					changed = ImGui::InputText("##name", actor.name, sizeof(actor.name)) || changed;
					row_label("Position");
					changed = ImGui::DragFloat3("##origin", actor.origin, 1.0f, 0.0f, 0.0f, "%.0f") || changed;
					ImGui::SetItemTooltip("Where it stands when its first animation begins. Or drag its arrows on the picture.");
					row_label("Facing");
					changed = ImGui::DragFloat("##yaw", &actor.yaw, 1.0f, -360.0f, 360.0f, "%.0f") || changed;
					row_label("Dressed as");
					if (ImGui::BeginCombo("##dressed", "A player's outfit..."))
					{
						for (int e = 0; e < 64; ++e)
						{
							if (players.entities & (1ull << e))
							{
								char name[48];
								snprintf(name, sizeof(name), e == players.nearest ? "Player %d (nearest the camera)" : "Player %d", e);
								if (ImGui::Selectable(name) && !actors_dress(i, e))
								{
									ui_log("That player's model has not been made yet in this part of the recording: go back a little and try again.");
								}
							}
						}

						ImGui::EndCombo();
					}

					ImGui::SetItemTooltip("Dress it as a player in the match is dressed now");
					ImGui::SameLine();
					if (ImGui::SmallButton("Change its outfit"))
					{
						g_models_entity = actor.handle;
					}

					ImGui::SetItemTooltip("Shows this actor under Models below: its parts, the gear sets and a random outfit work on it as on a player");
					row_label("Carried by its animations");
					changed = ImGui::Checkbox("##moves", &actor.moves) || changed;
					ImGui::SetItemTooltip("A walk walks it forward, a death throws it back: each animation carries on from where the one before left it");
					row_label("Stays after the last");
					changed = ImGui::Checkbox("##stays", &actor.stays) || changed;
					ImGui::SetItemTooltip("On: it holds its last pose to the end of the recording (a body stays down). Off: it goes when its last animation is over.");
					ImGui::SeparatorText("Animations");
					int remove_clip = -1;
					for (int c = 0; c < actor.clip_count; ++c)
					{
						ActorClip& clip = actor.clips[c];
						ImGui::PushID(c);
						char at[32], line[160];
						format_time(at, sizeof(at), clip.at_ms);
						snprintf(line, sizeof(line), "%s%s   %s, %.1f s###clip", c == 0 ? "" : "then  ", clip.anim, at, clip.length_ms / 1000.0f);
						const float buttons = ImGui::CalcTextSize(ICON_TRASH).x + ImGui::GetStyle().FramePadding.x * 2.0f + ImGui::GetStyle().ItemSpacing.x;
						if (ImGui::Selectable(line, g_actor_clip == c, 0, {ImGui::GetContentRegionAvail().x - buttons, 0.0f}))
						{
							g_actor_clip = g_actor_clip == c ? -1 : c;
						}

						if (have_now && standing.shown && standing.clip == c)
						{
							ImGui::SameLine(0.0f, 0.0f);
							ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(u32(P.accent)), "  " ICON_PLAY);
						}

						ImGui::SameLine();
						if (ImGui::SmallButton(ICON_TRASH))
						{
							remove_clip = c;
						}

						if (g_actor_clip == c)
						{
							ImGui::Indent();
							row_label("Animation");
							if (ImGui::Button("Choose another..."))
							{
								g_library.open = true;
								g_library.actor = i;
								g_library.clip = c;
								snprintf(g_library.chosen, sizeof(g_library.chosen), "%s", clip.anim);
							}

							float starts = clip.at_ms / 1000.0f, runs = clip.length_ms / 1000.0f, blend = clip.blend_ms / 1000.0f;
							row_label("Begins at");
							if (ImGui::DragFloat("##at", &starts, 0.02f, 0.0f, state.length_ms / 1000.0f, "%.2f s"))
							{
								clip.at_ms = static_cast<std::uint32_t>((std::max)(0.0f, starts) * 1000.0f);
								changed = true;
							}

							row_label("Runs for");
							if (ImGui::DragFloat("##runs", &runs, 0.02f, 0.1f, 3600.0f, "%.2f s"))
							{
								clip.length_ms = static_cast<std::uint32_t>((std::max)(0.1f, runs) * 1000.0f);
								changed = true;
							}

							{
								bool loops = false;
								const float length = actors_animation_length(clip.anim, &loops);
								ImGui::SetItemTooltip("The animation itself is %.2f s long%s. Past its end it %s; then the next one blends in.", length, loops ? " and goes round" : "",
								                      (clip.loop < 0 ? loops : clip.loop != 0) ? "goes round again" : "holds its last pose");
							}

							row_label("Speed");
							changed = ImGui::DragFloat("##speed", &clip.speed, 0.01f, 0.05f, 4.0f, "%.2fx") || changed;
							row_label("Blends in over");
							if (ImGui::DragFloat("##blend", &blend, 0.01f, 0.0f, 3.0f, "%.2f s"))
							{
								clip.blend_ms = static_cast<std::uint32_t>((std::max)(0.0f, blend) * 1000.0f);
								changed = true;
							}

							ImGui::SetItemTooltip("How long it takes to take over from the animation before it");
							row_label("Goes round");
							int round = clip.loop + 1;
							if (ImGui::Combo("##loop", &round, "As the animation says\0No: holds its last pose\0Yes: again and again\0"))
							{
								clip.loop = round - 1;
								changed = true;
							}

							ImGui::Unindent();
						}

						ImGui::PopID();
					}

					if (remove_clip >= 0)
					{
						for (int c = remove_clip; c + 1 < actor.clip_count; ++c)
						{
							actor.clips[c] = actor.clips[c + 1];
						}

						--actor.clip_count;
						g_actor_clip = -1;
						changed = true;
					}

					ImGui::BeginDisabled(actor.clip_count >= ACTOR_CLIPS_MAX);
					if (ImGui::Button(actor.clip_count ? "Then..." : "Choose an animation..."))
					{
						g_library.open = true;
						g_library.actor = i;
						g_library.clip = -1;
						g_library.chosen[0] = 0;
					}

					ImGui::EndDisabled();
					ImGui::SetItemTooltip("Another animation after the last: it begins where that one ends and blends in from it.\nSo: stand, then walk, then die.");
					ImGui::Unindent();
					if (changed)
					{
						actors_set(i, actor);
					}

					ImGui::PopID();
				}

				if (remove_actor >= 0)
				{
					actors_remove(remove_actor);
					g_actor_open = -1;
					g_actor_clip = -1;
				}

			}

			if (section(ICON_PERSON, "Models"))
			{
				// a player's outfit and weapon, part by part, each drawn as another model or left out
				static const char* const SLOT_LABELS[] = {"Top", "Headgear", "Head", "Pants", "Gloves", "Boots", "Kneepads", "Loadout gear", "Eyewear", "Exo"};
				int& entity = g_models_entity;
				static int target_slot = -1, group = 0;
				static char target[64] = "", search[48] = "", set_id[32] = "";
				static bool only_this_kind = true;
				const BoneState players = camera_bone_state();
				// (2048 is the first person's arms and gun: the game makes them like an entity's model)
				constexpr int FIRST_PERSON = 2048;
				char actor_name[32] = "";
				const bool an_actor = entity >= 64 && entity != FIRST_PERSON && is_actor_handle(entity, actor_name);
				if (entity != FIRST_PERSON && !an_actor && (entity < 0 || entity >= 64 || !(players.entities & (1ull << entity))))
				{
					entity = players.nearest;
				}

				char label[48];
				if (an_actor)
				{
					snprintf(label, sizeof(label), "%s", actor_name);
				}
				else
				{
					snprintf(label, sizeof(label), entity == FIRST_PERSON ? "First person (arms and gun)" : entity < 0 ? "None in view"
						: entity == players.nearest ? "Player %d (nearest the camera)" : "Player %d", entity);
				}

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

					{
						static Actor listed[ACTORS_MAX];
						const int listed_count = actors_list(listed, ACTORS_MAX);
						for (int a = 0; a < listed_count; ++a)
						{
							if (ImGui::Selectable(listed[a].name, listed[a].handle == entity))
							{
								entity = listed[a].handle;
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
					// (the tables' names, made readable: atlas_campaign_1 is Atlas Campaign 1)
					auto readable = [](const char* id, char out[40]) {
						bool start = true;
						size_t n = 0;
						for (; id[n] && n < 39; ++n)
						{
							out[n] = id[n] == '_' ? ' ' : start ? static_cast<char>(toupper(static_cast<unsigned char>(id[n]))) : id[n];
							start = id[n] == '_';
						}

						out[n] = 0;
						return out;
					};
					char shown_set[40] = "";
					if (ImGui::BeginCombo("##gearSet", set_id[0] ? readable(set_id, shown_set) : "Choose..."))
					{
						for (int i = 0; i < set_count; ++i)
						{
							char label[40];
							if (ImGui::Selectable(readable(sets[i], label), strcmp(sets[i], set_id) == 0))
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
					if (ImGui::Button("Random outfit"))
					{
						if (entity >= 64)
						{
							models_randomize_one(entity);
						}
						else
						{
							models_randomize(entity >= 0 ? 1ull << entity : 0);
						}
					}

					ImGui::SetItemTooltip("One of the game's sets, picked at random, for this player");
					ImGui::EndDisabled();
					ImGui::SameLine();
					if (ImGui::Button("Random outfits for everyone"))
					{
						models_randomize(players.entities);
					}

					ImGui::SetItemTooltip("A random set for every player in the match. The game keeps 18 of each kind of gear (shirts, hats, ...) in memory at most:\nonce a kind is full, players get sets made of parts that are already loaded. Gear nobody wears any more is let go of\na few seconds after a change. Click again for another mix.");
					ImGui::BeginDisabled(entity == FIRST_PERSON);
					bool loading = models_loading_allowed();
					if (ImGui::Checkbox("Load a set's missing parts from the game's files", &loading))
					{
						models_allow_loading(loading);
					}

					ImGui::SetItemTooltip("Off: a set's parts are put on as far as this match has them in memory.\nOn: the rest are added to the list of gear the game itself keeps loaded for the match, and taken off it again when nobody wears them.");
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
							// (a gun: the whole guns first, so that it stays in one piece)
							const bool gun = (strncmp(own, "npc_", 4) == 0 || strncmp(own, "vm_", 3) == 0) && (strstr(own, "_base") || strstr(own, "_loot"));
							group = gun ? 4 : entity == FIRST_PERSON ? 2 : slot >= 0 ? 0 : strncmp(own, "npc_", 4) == 0 ? 1 : strncmp(own, "mp_", 3) == 0 || !own[0] ? 0 : 2;
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
						static const char* const GROUPS[] = {"Outfit", "Weapons", "First person", "Everything", "Whole guns"};
						for (int g = 0; g < 5; ++g)
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

						if (group == 4)
						{
							ImGui::PushTextWrapPos(0.0f);
							ImGui::TextDisabled("Guns that come in one piece. Under Weapons, a \"_base\" model is only the body: its barrel and stock are separate parts.");
							ImGui::PopTextWrapPos();
						}

						const int found_count = models_find(search, group, group == 4 ? (entity == FIRST_PERSON ? 1 : 0) : group == 0 && only_this_kind ? target_slot : -1, found, 400);
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

				if (rebuilt && entity == FIRST_PERSON)
				{
					// the first person's arms and gun are made by the game as the recording is gone back in
					// (a player's model is made again where it stands: models.cpp)
					theater_rebuild();
				}
			}

			not_built("Actors and animations");
			break;
		case Tab::Record:
			passes_draw_ui(capture_wants_picture());
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
		// how far down the rows are scrolled (the wheel over the names), and how tall all of them were last frame
		static float rows_scroll = 0.0f, rows_height = 0.0f;

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
			ImGui::TextDisabled("wheel over the timeline: zoom in   Ctrl + wheel: taller or shorter rows");
		}

		// the ruler and the rows
		ImGui::Spacing();
		// (the names' column is as wide as the line between it and the rows is dragged)
		static float labels_set = 0.0f;
		const ImVec2 origin = ImGui::GetCursorScreenPos();
		const float full = ImGui::GetContentRegionAvail().x;
		const float labels = (std::max)(g_font_size * 8.0f, (std::min)(labels_set > 0.0f ? labels_set : g_font_size * 15.0f, full * 0.6f));
		const float left = origin.x + labels, right = origin.x + full - g_font_size * 0.5f;
		const float length = state.length_ms > 0 ? static_cast<float>(state.length_ms) : 1.0f;
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
				if (io.KeyCtrl)
				{
					// (the rows' height)
					g_lane_scale *= io.MouseWheel > 0.0f ? 1.15f : 1.0f / 1.15f;
					g_lane_scale = g_lane_scale < 0.6f ? 0.6f : g_lane_scale > 4.0f ? 4.0f : g_lane_scale;
				}
				else if (io.MousePos.x < left)
				{
					// (over the names: the rows go up and down)
					rows_scroll -= io.MouseWheel * frame * 2.5f;
				}
				else if (io.KeyShift)
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

		// the line between the names and the rows: dragged, the names' column is wider or narrower
		{
			const float grip = 6.0f;
			ImGui::SetCursorScreenPos({left - grip, ruler_bottom});
			ImGui::InvisibleButton("##labelsWidth", {grip, (std::max)(1.0f, rows_bottom - ruler_bottom)});
			const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
			if (ImGui::IsItemActive())
			{
				labels_set = labels + ImGui::GetIO().MouseDelta.x;
			}

			if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
			{
				labels_set = 0.0f;
			}

			if (hot)
			{
				ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
				draw->AddLine({left - grip * 0.5f, ruler_bottom}, {left - grip * 0.5f, rows_bottom}, u32(P.accent), 2.0f);
			}
		}

		// the rows: Markers, Campath (as tall as Ctrl + the wheel has made them)
		const float lane = std::floor(frame * g_lane_scale);
		const float row = lane + style.ItemSpacing.y * 0.5f;
		const float rows_top = ruler_bottom + style.ItemSpacing.y * 0.5f;
		{
			const float most = rows_height - (rows_bottom - rows_top);
			rows_scroll = rows_scroll > most ? most : rows_scroll;
			rows_scroll = rows_scroll < 0.0f ? 0.0f : rows_scroll;
		}

		float y = rows_top - rows_scroll;
		// (a row is drawn and takes the mouse only where it shows under the ruler)
		auto row_shows = [&](const float top) { return top + lane > rows_top && top < rows_bottom; };
		ImGui::PushClipRect({origin.x, rows_top - 1.0f}, {origin.x + full, rows_bottom}, true);
		draw->AddRectFilled({left, y}, {right, y + lane}, u32(P.surface));
		draw->AddText({origin.x + g_font_size * 0.5f, y + style.FramePadding.y}, u32(P.muted), ICON_FLAG "  Markers");
		y += row;
		draw->AddRectFilled({left, y}, {right, y + lane}, u32(P.surface));
		char campath[48];
		snprintf(campath, sizeof(campath), ICON_VIDEO "  Campath (%d)", camera.points);
		draw->AddText({origin.x + g_font_size * 0.5f, y + style.FramePadding.y}, u32(P.text), campath);
		ImGui::SetCursorScreenPos({left - lane * 1.2f - style.ItemSpacing.x - 6.0f, y});
		ImGui::BeginDisabled(camera.points == 0);
		if (ImGui::Button(ICON_TRASH "##clearCampath", {lane * 1.2f, lane}))
		{
			camera_clear_points();
		}

		ImGui::EndDisabled();
		const float diamond = lane * 0.28f;
		for (int i = 0; i < camera.points && i < 64; ++i)
		{
			const float x = std::floor(x_of(static_cast<float>(camera.point_ms[i]))), middle = y + lane * 0.5f;
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
			if (!row_shows(y))
			{
				continue;
			}

			draw->AddRectFilled({left, y}, {right, y + lane}, u32(P.surface));
			ImGui::PushID(t);
			ImGui::PushClipRect({origin.x, y}, {left - lane * 1.2f - style.ItemSpacing.x * 2.0f - 6.0f, y + lane}, true);
			char name[96];
			snprintf(name, sizeof(name), ICON_SLIDERS "  %s (%d)", track.label, track.keys);
			draw->AddText({origin.x + g_font_size * 0.5f, y + style.FramePadding.y}, u32(P.text), name);
			ImGui::PopClipRect();
			ImGui::SetCursorScreenPos({left - lane * 1.2f - style.ItemSpacing.x - 6.0f, y});
			if (ImGui::Button(ICON_TRASH "##clearTrack", {lane * 1.2f, lane}))
			{
				clear = t;
			}

			ImGui::SetItemTooltip("Remove these keyframes");
			for (int i = 0; i < track.keys; ++i)
			{
				const float x = std::floor(x_of(static_cast<float>(track.ms[i]))), middle = y + lane * 0.5f;
				if (!in_view(x))
				{
					continue;
				}

				ImGui::SetCursorScreenPos({x - diamond, y});
				ImGui::PushID(i);
				if (ImGui::InvisibleButton("##key", {diamond * 2.0f, lane}))
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

		// The editor's effects, by kind: a row for each kind with one bar over all of its clips (dragged, they all
		// move; its ends dragged, they all begin or end sooner or later), and, opened out, a row for each effect.
		// An effect is in the game exactly while the recording's time is inside its clip.
		{
			static PlacedEffect placed[EFFECTS_MAX];
			static bool live[EFFECTS_MAX];
			const int placed_count = effects_placed(placed, EFFECTS_MAX);
			effects_live(live, EFFECTS_MAX);
			const ImGuiIO& io = ImGui::GetIO();
			const float mouse_ms = ms_at(io.MousePos.x);
			for (auto it = g_fx_selected.begin(); it != g_fx_selected.end();)
			{
				it = *it >= placed_count ? g_fx_selected.erase(it) : std::next(it);
			}

			// a drag under way: what is dragged (1 the clips, 2 their ends, 3 their starts), from where, and the
			// clips as they were when it began
			static int drag = 0;
			static float drag_from = 0.0f;
			static bool dragged = false;
			static std::vector<std::pair<int, PlacedEffect>> drag_start;
			// (what was pressed, when it was already among several selected: let go of without a drag, it alone stays selected)
			static std::vector<int> pressed_which;
			std::vector<int> to_remove;
			bool open_menu = false;
			auto selected_all = [&](const std::vector<int>& which) {
				for (const int i : which)
				{
					if (!g_fx_selected.count(i))
					{
						return false;
					}
				}

				return !which.empty();
			};

			// One bar on a row: its two ends and its middle take the mouse. "which" is what the bar stands for
			// (one effect, or all of a kind).
			auto bar = [&](const char* id, const std::vector<int>& which, const std::uint32_t from_ms, const std::uint32_t to_ms, const float top, const char* text,
			               const bool on, const bool is_live, const bool whole_kind) {
				float x0 = std::floor(x_of(static_cast<float>(from_ms))), x1 = std::floor(x_of(static_cast<float>(to_ms)));
				if (x1 < left || x0 > right)
				{
					return;
				}

				x0 = x0 < left ? left : x0;
				x1 = x1 < x0 + 10.0f ? x0 + 10.0f : x1 > right ? right : x1;
				const float grip = x1 - x0 >= 28.0f ? 7.0f : 4.0f;
				const bool both_ends = x1 - x0 >= 28.0f;
				ImGui::PushID(id);
				int over = 0, pressed = 0;
				bool double_click = false, right_click = false;
				auto part = [&](const char* name, const float from, const float to, const int kind) {
					ImGui::SetCursorScreenPos({from, top});
					ImGui::InvisibleButton(name, {to - from < 2.0f ? 2.0f : to - from, lane});
					if (ImGui::IsItemHovered())
					{
						over = kind;
						double_click = double_click || ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
						right_click = right_click || ImGui::IsMouseClicked(ImGuiMouseButton_Right);
					}

					if (ImGui::IsItemActivated())
					{
						pressed = kind;
					}
				};
				part("##end", x1 - grip, x1 + grip, 2);
				if (both_ends)
				{
					part("##start", x0 - grip, x0 + grip, 3);
				}

				part("##clip", both_ends ? x0 + grip : x0, x1 - grip, 1);
				if (pressed || right_click)
				{
					// what is pressed becomes what is selected (with Ctrl: is added to it), unless it already is
					if (io.KeyCtrl && pressed)
					{
						g_fx_selected.insert(which.begin(), which.end());
					}
					else if (!selected_all(which))
					{
						g_fx_selected.clear();
						g_fx_selected.insert(which.begin(), which.end());
					}
				}

				if (pressed)
				{
					pressed_which = io.KeyCtrl ? std::vector<int>() : which;
					drag = pressed;
					drag_from = mouse_ms;
					dragged = false;
					drag_start.clear();
					for (const int i : g_fx_selected)
					{
						drag_start.push_back({i, placed[i]});
					}
				}

				if (double_click)
				{
					theater_seek(from_ms);
				}

				if (right_click)
				{
					open_menu = true;
				}

				if (over == 2 || over == 3)
				{
					ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
				}

				if (over && !drag)
				{
					char at[32], until[32];
					format_time(at, sizeof(at), from_ms);
					format_time(until, sizeof(until), to_ms);
					ImGui::SetTooltip("%s\n%s to %s%s\nDrag: earlier or later.  Drag an end: begins or ends sooner or later.\nCtrl + click: select more.  "
					                  "Double click: go to its start.  Right click: more.",
					                  text, at, until, is_live ? "  (in the game now)" : "");
				}

				ImGui::PopID();
				const bool chosen = selected_all(which);
				const unsigned fill = on ? P.warn : P.muted;
				const float inset = whole_kind ? 2.0f : 3.0f;
				draw->AddRectFilled({x0, top + inset}, {x1, top + lane - inset}, u32(fill, is_live ? 0.60f : over ? 0.42f : 0.28f), 3.0f);
				draw->AddRect({x0, top + inset}, {x1, top + lane - inset}, u32(chosen ? P.text : fill), 3.0f, 0, chosen ? 2.0f : 1.0f);
				draw->AddRectFilled({x1 - 3.0f, top + inset}, {x1, top + lane - inset}, u32(chosen ? P.text : fill));
				if (both_ends)
				{
					draw->AddRectFilled({x0, top + inset}, {x0 + 3.0f, top + lane - inset}, u32(chosen ? P.text : fill));
				}

				ImGui::PushClipRect({x0 + 5.0f, top}, {x1 - 5.0f, top + lane}, true);
				draw->AddText({x0 + 7.0f, top + style.FramePadding.y}, u32(P.text), text);
				ImGui::PopClipRect();
			};

			const auto kinds = fx_kinds(placed, placed_count);
			int kind_number = 0;
			for (const auto& kind : kinds)
			{
				const std::vector<int>& members = kind.second;
				const bool opened = g_fx_open_kinds.count(kind.first) != 0;
				const char* slash = strrchr(kind.first.c_str(), '/');
				const char* shown_name = slash ? slash + 1 : kind.first.c_str();
				y += row;
				if (row_shows(y))
				{
					std::uint32_t from = UINT32_MAX, to = 0;
					bool any_on = false, any_live = false;
					for (const int i : members)
					{
						from = (std::min)(from, placed[i].at_ms);
						to = (std::max)(to, placed[i].at_ms + placed[i].length_ms);
						any_on = any_on || placed[i].on;
						any_live = any_live || live[i];
					}

					draw->AddRectFilled({left, y}, {right, y + lane}, u32(P.surface));
					ImGui::PushID(9000 + kind_number);
					// the name: a click opens the kind out or shuts it
					const float name_right = left - lane * 1.2f - style.ItemSpacing.x * 2.0f - 6.0f;
					ImGui::SetCursorScreenPos({origin.x, y});
					if (ImGui::InvisibleButton("##kind", {(std::max)(4.0f, name_right - origin.x), lane}))
					{
						if (opened)
						{
							g_fx_open_kinds.erase(kind.first);
						}
						else
						{
							g_fx_open_kinds.insert(kind.first);
						}
					}

					ImGui::SetItemTooltip("%s\n%d placed. Click: show %s.", kind.first.c_str(), static_cast<int>(members.size()),
					                      opened ? "them as one row again" : "a row for each");
					ImGui::PushClipRect({origin.x, y}, {name_right, y + lane}, true);
					char name[160];
					snprintf(name, sizeof(name), "%s  " ICON_WAND "  %s (%d)", opened ? ICON_CHEVRON_DOWN : ICON_CHEVRON_RIGHT, shown_name, static_cast<int>(members.size()));
					draw->AddText({origin.x + g_font_size * 0.5f, y + style.FramePadding.y}, u32(any_on ? P.text : P.muted), name);
					ImGui::PopClipRect();
					ImGui::SetCursorScreenPos({left - lane * 1.2f - style.ItemSpacing.x - 6.0f, y});
					if (ImGui::Button(ICON_TRASH "##removeKind", {lane * 1.2f, lane}))
					{
						to_remove = members;
					}

					ImGui::SetItemTooltip(members.size() > 1 ? "Remove all %d of these" : "Remove this effect", static_cast<int>(members.size()));
					// where each of its clips is, faintly, under the one bar that stands for them all
					if (members.size() > 1)
					{
						for (const int i : members)
						{
							const float c0 = (std::max)(left, std::floor(x_of(static_cast<float>(placed[i].at_ms))));
							const float c1 = (std::min)(right, std::floor(x_of(static_cast<float>(placed[i].at_ms + placed[i].length_ms))));
							if (c1 > c0)
							{
								draw->AddRectFilled({c0, y + lane - 4.0f}, {c1, y + lane - 2.0f}, u32(placed[i].on ? P.warn : P.muted, 0.8f));
							}
						}
					}

					char text[160];
					snprintf(text, sizeof(text), members.size() > 1 ? "%s  x%d" : "%s", shown_name, static_cast<int>(members.size()));
					bar("##all", members, from, to, y, text, any_on, any_live, true);
					ImGui::PopID();
				}

				++kind_number;
				if (!opened)
				{
					continue;
				}

				for (const int i : members)
				{
					y += row;
					if (!row_shows(y))
					{
						continue;
					}

					const PlacedEffect& effect = placed[i];
					draw->AddRectFilled({left, y}, {right, y + lane}, u32(P.surface, 0.6f));
					ImGui::PushID(7000 + i);
					ImGui::PushClipRect({origin.x, y}, {left - lane * 1.2f - style.ItemSpacing.x * 2.0f - 6.0f, y + lane}, true);
					char name[128], at[32];
					format_time(at, sizeof(at), effect.at_ms);
					snprintf(name, sizeof(name), "%s   %s", shown_name, at);
					draw->AddText({origin.x + g_font_size * 2.2f, y + style.FramePadding.y}, u32(effect.on ? (live[i] ? P.text : P.muted) : P.muted, effect.on ? 1.0f : 0.6f), name);
					ImGui::PopClipRect();
					ImGui::SetCursorScreenPos({left - lane * 1.2f - style.ItemSpacing.x - 6.0f, y});
					if (ImGui::Button(ICON_TRASH "##removeEffect", {lane * 1.2f, lane}))
					{
						to_remove = {i};
					}

					ImGui::SetItemTooltip("Remove this effect");
					bar("##one", {i}, effect.at_ms, effect.at_ms + effect.length_ms, y, shown_name, effect.on, live[i], false);
					ImGui::PopID();
				}
			}

			// the drag: every selected clip by as much as the mouse has gone since it was pressed
			if (drag && ImGui::IsMouseDown(ImGuiMouseButton_Left))
			{
				float by = mouse_ms - drag_from;
				dragged = dragged || ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f);
				if (dragged)
				{
					if (drag == 1)
					{
						// (none of them before the recording's start)
						for (const auto& start : drag_start)
						{
							by = (std::max)(by, -static_cast<float>(start.second.at_ms));
						}
					}

					for (const auto& start : drag_start)
					{
						if (start.first >= placed_count)
						{
							continue;
						}

						const PlacedEffect& was = start.second;
						// (what the Scene tab changed meanwhile is kept: only the times are the drag's)
						PlacedEffect current = placed[start.first];
						const float was_at = static_cast<float>(was.at_ms), was_long = static_cast<float>(was.length_ms);
						if (drag == 1)
						{
							current.at_ms = static_cast<std::uint32_t>((std::min)(length, (std::max)(0.0f, was_at + by)));
						}
						else if (drag == 2)
						{
							current.length_ms = static_cast<std::uint32_t>((std::max)(100.0f, was_long + by));
						}
						else
						{
							const float at = (std::min)(was_at + was_long - 100.0f, (std::max)(0.0f, was_at + by));
							current.at_ms = static_cast<std::uint32_t>(at);
							current.length_ms = static_cast<std::uint32_t>(was_at + was_long - at);
						}

						if (current.at_ms != placed[start.first].at_ms || current.length_ms != placed[start.first].length_ms)
						{
							effects_set(start.first, current);
						}
					}

					ImGui::SetMouseCursor(drag == 1 ? ImGuiMouseCursor_Hand : ImGuiMouseCursor_ResizeEW);
				}
			}
			else
			{
				if (drag && !dragged && !pressed_which.empty())
				{
					g_fx_selected.clear();
					g_fx_selected.insert(pressed_which.begin(), pressed_which.end());
				}

				drag = 0;
			}

			if (open_menu)
			{
				ImGui::OpenPopup("##fxMenu");
			}

			if (ImGui::BeginPopup("##fxMenu"))
			{
				const int count = static_cast<int>(g_fx_selected.size());
				ImGui::TextDisabled(count == 1 ? "1 effect" : "%d effects", count);
				ImGui::Separator();
				if (ImGui::MenuItem("Go to where the first begins"))
				{
					std::uint32_t first = UINT32_MAX;
					for (const int i : g_fx_selected)
					{
						first = (std::min)(first, placed[i].at_ms);
					}

					if (first != UINT32_MAX)
					{
						theater_seek(first);
					}
				}

				const bool on = ImGui::MenuItem("Switch on"), off = ImGui::MenuItem("Switch off");
				if (on || off)
				{
					for (const int i : g_fx_selected)
					{
						PlacedEffect changed = placed[i];
						changed.on = on;
						effects_set(i, changed);
					}
				}

				if (ImGui::MenuItem("Select every one of this kind"))
				{
					const std::set<int> chosen = g_fx_selected;
					for (int i = 0; i < placed_count; ++i)
					{
						for (const int c : chosen)
						{
							if (strcmp(placed[i].name, placed[c].name) == 0)
							{
								g_fx_selected.insert(i);
								break;
							}
						}
					}
				}

				ImGui::Separator();
				if (ImGui::MenuItem(ICON_TRASH "  Remove", "Del"))
				{
					to_remove.assign(g_fx_selected.begin(), g_fx_selected.end());
				}

				ImGui::EndPopup();
			}

			if (!io.WantTextInput && !g_fx_selected.empty() && ImGui::IsKeyPressed(ImGuiKey_Delete, false) &&
			    ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
			{
				to_remove.assign(g_fx_selected.begin(), g_fx_selected.end());
			}

			if (!to_remove.empty())
			{
				drag = 0;
				remove_effects(to_remove);
			}
		}

		// The editor's actors: a row each, with its animations as bars one after another. Dragged, a bar begins
		// earlier or later; its ends dragged, it begins or stops sooner or later. A right click: "Then...".
		{
			static Actor actors[ACTORS_MAX];
			const int actor_count = actors_list(actors, ACTORS_MAX);
			const ImGuiIO& io = ImGui::GetIO();
			const float mouse_ms = ms_at(io.MousePos.x);
			static int drag = 0, drag_actor = -1, drag_clip = -1, menu_actor = -1, menu_clip = -1;
			static float drag_from = 0.0f;
			static ActorClip drag_start;
			static bool dragged = false;
			int remove_actor = -1;
			bool open_menu = false;
			for (int i = 0; i < actor_count; ++i)
			{
				y += row;
				if (!row_shows(y))
				{
					continue;
				}

				const Actor& actor = actors[i];
				ActorNow standing;
				const bool in_match = actors_now(i, &standing) && standing.shown;
				draw->AddRectFilled({left, y}, {right, y + lane}, u32(P.surface));
				ImGui::PushID(11000 + i);
				const float name_right = left - lane * 1.2f - style.ItemSpacing.x * 2.0f - 6.0f;
				ImGui::SetCursorScreenPos({origin.x, y});
				if (ImGui::InvisibleButton("##actor", {(std::max)(4.0f, name_right - origin.x), lane}))
				{
					g_actor_open = i;
					g_actor_clip = -1;
				}

				ImGui::SetItemTooltip("%s: its settings are under Actors in the Scene tab", actor.name);
				ImGui::PushClipRect({origin.x, y}, {name_right, y + lane}, true);
				char name[64];
				snprintf(name, sizeof(name), ICON_PERSON "  %s", actor.name);
				draw->AddText({origin.x + g_font_size * 0.5f, y + style.FramePadding.y}, u32(actor.on ? (in_match ? P.text : P.muted) : P.muted, actor.on ? 1.0f : 0.6f), name);
				ImGui::PopClipRect();
				ImGui::SetCursorScreenPos({left - lane * 1.2f - style.ItemSpacing.x - 6.0f, y});
				if (ImGui::Button(ICON_TRASH "##removeActor", {lane * 1.2f, lane}))
				{
					remove_actor = i;
				}

				ImGui::SetItemTooltip("Remove this actor");
				for (int c = 0; c < actor.clip_count; ++c)
				{
					const ActorClip& clip = actor.clips[c];
					float x0 = std::floor(x_of(static_cast<float>(clip.at_ms))), x1 = std::floor(x_of(static_cast<float>(clip.at_ms + clip.length_ms)));
					if (x1 < left || x0 > right)
					{
						continue;
					}

					x0 = x0 < left ? left : x0;
					x1 = x1 < x0 + 10.0f ? x0 + 10.0f : x1 > right ? right : x1;
					const bool both_ends = x1 - x0 >= 28.0f;
					const float grip = both_ends ? 7.0f : 4.0f;
					ImGui::PushID(c);
					int over = 0, pressed = 0;
					bool double_click = false, right_click = false;
					auto part = [&](const char* id, const float from, const float to, const int kind) {
						ImGui::SetCursorScreenPos({from, y});
						ImGui::InvisibleButton(id, {to - from < 2.0f ? 2.0f : to - from, lane});
						if (ImGui::IsItemHovered())
						{
							over = kind;
							double_click = double_click || ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
							right_click = right_click || ImGui::IsMouseClicked(ImGuiMouseButton_Right);
						}

						if (ImGui::IsItemActivated())
						{
							pressed = kind;
						}
					};
					part("##end", x1 - grip, x1 + grip, 2);
					if (both_ends)
					{
						part("##start", x0 - grip, x0 + grip, 3);
					}

					part("##clip", both_ends ? x0 + grip : x0, x1 - grip, 1);
					ImGui::PopID();
					if (pressed)
					{
						drag = pressed;
						drag_actor = i;
						drag_clip = c;
						drag_from = mouse_ms;
						drag_start = clip;
						dragged = false;
						g_actor_open = i;
						g_actor_clip = c;
					}

					if (double_click)
					{
						theater_seek(clip.at_ms);
					}

					if (right_click)
					{
						open_menu = true;
						menu_actor = i;
						menu_clip = c;
						g_actor_open = i;
						g_actor_clip = c;
					}

					if (over == 2 || over == 3)
					{
						ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
					}

					if (over && !drag)
					{
						char at[32];
						format_time(at, sizeof(at), clip.at_ms);
						ImGui::SetTooltip("%s\n%s, for %.2f s\nDrag: earlier or later.  Drag an end: begins or stops sooner or later.\nDouble click: go to its start.  "
						                  "Right click: \"Then...\" and more.",
						                  clip.anim, at, clip.length_ms / 1000.0f);
					}

					const bool chosen = g_actor_open == i && g_actor_clip == c;
					const bool playing = in_match && standing.clip == c;
					const unsigned fill = actor.on ? P.accent : P.muted;
					draw->AddRectFilled({x0, y + 3.0f}, {x1, y + lane - 3.0f}, u32(fill, playing ? 0.65f : over ? 0.45f : 0.30f), 3.0f);
					draw->AddRect({x0, y + 3.0f}, {x1, y + lane - 3.0f}, u32(chosen ? P.text : fill), 3.0f, 0, chosen ? 2.0f : 1.0f);
					ImGui::PushClipRect({x0 + 5.0f, y}, {x1 - 5.0f, y + lane}, true);
					// (a player's body animations all begin "pb_")
					draw->AddText({x0 + 7.0f, y + style.FramePadding.y}, u32(P.text), strncmp(clip.anim, "pb_", 3) == 0 ? clip.anim + 3 : clip.anim);
					ImGui::PopClipRect();
				}

				ImGui::PopID();
			}

			if (drag && ImGui::IsMouseDown(ImGuiMouseButton_Left) && drag_actor < actor_count && drag_clip < actors[drag_actor].clip_count)
			{
				const float by = mouse_ms - drag_from;
				dragged = dragged || ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f);
				if (dragged)
				{
					Actor changed = actors[drag_actor];
					ActorClip& clip = changed.clips[drag_clip];
					const float was_at = static_cast<float>(drag_start.at_ms), was_long = static_cast<float>(drag_start.length_ms);
					if (drag == 1)
					{
						clip.at_ms = static_cast<std::uint32_t>((std::min)(length, (std::max)(0.0f, was_at + by)));
					}
					else if (drag == 2)
					{
						clip.length_ms = static_cast<std::uint32_t>((std::max)(100.0f, was_long + by));
					}
					else
					{
						const float at = (std::min)(was_at + was_long - 100.0f, (std::max)(0.0f, was_at + by));
						clip.at_ms = static_cast<std::uint32_t>(at);
						clip.length_ms = static_cast<std::uint32_t>(was_at + was_long - at);
					}

					actors_set(drag_actor, changed);
					ImGui::SetMouseCursor(drag == 1 ? ImGuiMouseCursor_Hand : ImGuiMouseCursor_ResizeEW);
				}
			}
			else
			{
				drag = 0;
			}

			if (open_menu)
			{
				ImGui::OpenPopup("##actorClipMenu");
			}

			if (ImGui::BeginPopup("##actorClipMenu"))
			{
				const bool valid = menu_actor >= 0 && menu_actor < actor_count && menu_clip >= 0 && menu_clip < actors[menu_actor].clip_count;
				if (valid)
				{
					ImGui::TextDisabled("%s: %s", actors[menu_actor].name, actors[menu_actor].clips[menu_clip].anim);
					ImGui::Separator();
					if (ImGui::MenuItem("Then...", nullptr, false, actors[menu_actor].clip_count < ACTOR_CLIPS_MAX))
					{
						g_library.open = true;
						g_library.actor = menu_actor;
						g_library.clip = -1;
						g_library.chosen[0] = 0;
					}

					if (ImGui::MenuItem("Choose another animation..."))
					{
						g_library.open = true;
						g_library.actor = menu_actor;
						g_library.clip = menu_clip;
						snprintf(g_library.chosen, sizeof(g_library.chosen), "%s", actors[menu_actor].clips[menu_clip].anim);
					}

					ImGui::Separator();
					if (ImGui::MenuItem(ICON_TRASH "  Remove this animation"))
					{
						Actor changed = actors[menu_actor];
						for (int c = menu_clip; c + 1 < changed.clip_count; ++c)
						{
							changed.clips[c] = changed.clips[c + 1];
						}

						--changed.clip_count;
						actors_set(menu_actor, changed);
						g_actor_clip = -1;
					}
				}

				ImGui::EndPopup();
			}

			if (remove_actor >= 0)
			{
				drag = 0;
				actors_remove(remove_actor);
				g_actor_open = -1;
				g_actor_clip = -1;
			}
		}

		ImGui::PopClipRect();
		rows_height = (y + row + rows_scroll) - rows_top;
		// (the animation library opens over the editor from the Scene tab or from an actor's row here)
		draw_library();
		// (more rows than there is room for: a bar at the right edge shows where the rows shown are among them)
		if (rows_height > rows_bottom - rows_top + 1.0f)
		{
			const float room = rows_bottom - rows_top, track_x = origin.x + full - 4.0f;
			const float thumb = (std::max)(12.0f, room * room / rows_height), travel = room - thumb;
			const float most = rows_height - room;
			const float thumb_y = rows_top + (most > 0.0f ? rows_scroll / most : 0.0f) * travel;
			ImGui::SetCursorScreenPos({track_x - 4.0f, rows_top});
			ImGui::InvisibleButton("##rowsScroll", {8.0f, room});
			if (ImGui::IsItemActive() && travel > 0.0f)
			{
				rows_scroll = (ImGui::GetIO().MousePos.y - rows_top - thumb * 0.5f) / travel * most;
			}

			draw->AddRectFilled({track_x, rows_top}, {track_x + 4.0f, rows_bottom}, u32(P.border, 0.5f), 2.0f);
			draw->AddRectFilled({track_x, thumb_y}, {track_x + 4.0f, thumb_y + thumb}, u32(ImGui::IsItemHovered() || ImGui::IsItemActive() ? P.text : P.muted), 2.0f);
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
		static Actor row_actors[ACTORS_MAX];
		const int timeline_rows = look_track_count() + fx_timeline_rows() + actors_list(row_actors, ACTORS_MAX);
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
		if (shown || recording)
		{
			// (a pass that shows the depth draws it over the game's picture, before the picture is copied)
			passes_before_copy(swap_chain, g_device, g_context, state.playing);
		}

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
		passes_release();
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

// A row's right-click menu, asked for after the row's controls are drawn: true when "Reset to default"
// was chosen.
bool ui_reset_menu(const char* id)
{
	const ImVec2 low = ImGui::GetItemRectMin(), high = ImGui::GetItemRectMax();
	const float left = ImGui::GetWindowPos().x, right = left + ImGui::GetWindowWidth();
	ImGui::PushID(id);
	if (ImGui::IsWindowHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right) && ImGui::IsMouseHoveringRect({left, low.y}, {right, high.y}, false))
	{
		ImGui::OpenPopup("##resetMenu");
	}

	bool chosen = false;
	if (ImGui::BeginPopup("##resetMenu"))
	{
		chosen = ImGui::MenuItem("Reset to default");
		ImGui::EndPopup();
	}

	ImGui::PopID();
	return chosen;
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
