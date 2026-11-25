// The Look tab: the game's own settings ("dvars") for the sun, the sky, fog, depth of field, film and the
// HUD, each shown as the kind of control its type calls for and with the limits the game gives it.
//
// Which settings Advanced Warfare has is found out as it runs: a row is shown only if the game has the
// setting. The names are those the engine family uses (IW6, S1, H1); H1MVM's theater editor (GPL-3) is
// where the selection for a theater comes from. A setting is changed through the game's console, and
// whatever was changed is put back when playback ends.
//
// A dvar (s1-mod's structs): name at +0, flags at +8, type at +0xC (0 bool, 1 float, 2-4 vectors, 5 int,
// 6 one of a list, 7 text, 8 colour in bytes, 9 colour in three floats), the current value at +0x10, the one it resets to at +0x30, its
// limits at +0x40 (floats: min, max; ints: min, max; a list: how many, and their names).
#include "ui_parts.hpp"
#include "passes.hpp"

#include <cfloat>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace
{
	struct Dvar
	{
		const unsigned char* at = nullptr;

		explicit operator bool() const { return at != nullptr; }
		const char* name() const { return *reinterpret_cast<const char* const*>(at); }
		int type() const { return static_cast<signed char>(at[0xC]); }
		const unsigned char* current() const { return at + 0x10; }
		const unsigned char* limits() const { return at + 0x40; }
	};

	std::map<std::string, const unsigned char*> g_found;
	std::map<std::string, ULONGLONG> g_missing;      // when a name the game did not have was last asked for
	std::map<std::string, std::string> g_originals;  // what a setting was before the editor changed it

	// The focus handed to ReShade (reshadelink.cpp): on, and the game's own blur left alone meanwhile; which
	// setting of which effect; how much of that setting one unit of the game's distance is; the distance when
	// nothing is pinned (it can have keyframes: "reshade_focus"); the pinned thing's distance (< 0: none).
	bool g_rs_on = false, g_rs_only = true;
	char g_rs_effect[64] = "", g_rs_name[64] = "";
	float g_rs_scale = 0.001f, g_rs_distance = 500.0f, g_rs_pinned = -1.0f;

	Dvar find(const char* name)
	{
		const auto found = g_found.find(name);
		if (found != g_found.end())
		{
			return {found->second};
		}

		const ULONGLONG now = GetTickCount64();
		auto& asked = g_missing[name];
		if (asked && now - asked < 1000)
		{
			return {};
		}

		asked = now;
		const auto* at = static_cast<const unsigned char*>(theater_dvar(name));
		if (at)
		{
			g_found[name] = at;
			g_missing.erase(name);
		}

		return {at};
	}

	// The value as the console takes it.
	std::string text_of(const Dvar dvar, const unsigned char* value)
	{
		char text[256] = {};
		const float* f = reinterpret_cast<const float*>(value);
		switch (dvar.type())
		{
		case 0: snprintf(text, sizeof(text), "%d", value[0] ? 1 : 0); break;
		case 1: snprintf(text, sizeof(text), "%g", f[0]); break;
		case 2: snprintf(text, sizeof(text), "%g %g", f[0], f[1]); break;
		case 3: snprintf(text, sizeof(text), "%g %g %g", f[0], f[1], f[2]); break;
		case 4: snprintf(text, sizeof(text), "%g %g %g %g", f[0], f[1], f[2], f[3]); break;
		case 5:
		case 6: snprintf(text, sizeof(text), "%d", *reinterpret_cast<const int*>(value)); break;
		case 7:
		{
			const char* string = *reinterpret_cast<const char* const*>(value);
			snprintf(text, sizeof(text), "\"%.200s\"", string ? string : "");
			break;
		}
		case 8: snprintf(text, sizeof(text), "%g %g %g %g", value[0] / 255.0f, value[1] / 255.0f, value[2] / 255.0f, value[3] / 255.0f); break;
		// (0.33 read these as bytes: the film tints showed, and were saved to presets as, other colours)
		case 9: snprintf(text, sizeof(text), "%g %g %g", f[0], f[1], f[2]); break;
		default: break;
		}

		return text;
	}

	void set(const Dvar dvar, const std::string& value)
	{
		const std::string name = dvar.name();
		if (!g_originals.count(name))
		{
			g_originals[name] = text_of(dvar, dvar.current());
		}

		theater_command((name + " " + value).c_str());
	}

	void set(const Dvar dvar, const char* format, ...)
	{
		char text[256];
		va_list args;
		va_start(args, format);
		vsnprintf(text, sizeof(text), format, args);
		va_end(args);
		set(dvar, std::string(text));
	}

	void restore(const std::string& name)
	{
		const auto original = g_originals.find(name);
		if (original != g_originals.end())
		{
			theater_command((name + " " + original->second).c_str());
			g_originals.erase(original);
		}
	}

	// ---- keyframes. A setting with keyframes is set every frame to what its keyframes make of the
	// recording's time (After Effects' stopwatch: the diamond at a row's right puts a keyframe at the time
	// shown, and from then on changing the value there makes or changes one).
	struct Key
	{
		std::uint32_t ms = 0;
		float v[4] = {};
	};

	struct Track
	{
		std::string id;  // a dvar's name, or sun_color, sun_brightness, sun_direction
		std::string label;
		int count = 1;
		std::vector<Key> keys;  // by time
		std::string sent;       // what the game was last given
	};

	std::vector<Track> g_tracks;
	std::uint32_t g_now_ms = 0;
	constexpr std::uint32_t SAME_KEY_MS = 40;

	Track* track_of(const char* id)
	{
		for (Track& track : g_tracks)
		{
			if (track.id == id)
			{
				return &track;
			}
		}

		return nullptr;
	}

	int key_at(const Track& track, const std::uint32_t ms)
	{
		for (size_t i = 0; i < track.keys.size(); ++i)
		{
			const std::uint32_t at = track.keys[i].ms;
			if ((at > ms ? at - ms : ms - at) <= SAME_KEY_MS)
			{
				return static_cast<int>(i);
			}
		}

		return -1;
	}

	void put_key(const char* id, const char* label, const int count, const float* values)
	{
		Track* track = track_of(id);
		if (!track)
		{
			g_tracks.push_back({});
			track = &g_tracks.back();
			track->id = id;
			track->label = label;
			track->count = count;
		}

		Key key;
		key.ms = g_now_ms;
		memcpy(key.v, values, sizeof(float) * static_cast<size_t>(count));
		const int same = key_at(*track, g_now_ms);
		if (same >= 0)
		{
			key.ms = track->keys[static_cast<size_t>(same)].ms;
			track->keys[static_cast<size_t>(same)] = key;
			return;
		}

		size_t at = 0;
		while (at < track->keys.size() && track->keys[at].ms < key.ms)
		{
			++at;
		}

		track->keys.insert(track->keys.begin() + static_cast<std::ptrdiff_t>(at), key);
	}

	// A track's value at a time: a smooth curve through its keyframes (the campath's: at a keyframe the
	// value changes as fast as its two neighbours ask, over the time between them), held before the first
	// and after the last.
	void sample(const Track& track, const double ms, float out[4])
	{
		const std::vector<Key>& k = track.keys;
		if (ms <= k.front().ms || k.size() == 1)
		{
			memcpy(out, k.front().v, sizeof(k.front().v));
			return;
		}

		if (ms >= k.back().ms)
		{
			memcpy(out, k.back().v, sizeof(k.back().v));
			return;
		}

		size_t i = 0;
		while (i + 2 < k.size() && ms >= k[i + 1].ms)
		{
			++i;
		}

		const double t0 = k[i].ms, t1 = k[i + 1].ms, h = t1 - t0, u = (ms - t0) / h;
		const double h00 = 2 * u * u * u - 3 * u * u + 1, h10 = u * u * u - 2 * u * u + u, h01 = -2 * u * u * u + 3 * u * u, h11 = u * u * u - u * u;
		for (int c = 0; c < track.count; ++c)
		{
			const double p0 = k[i].v[c], p1 = k[i + 1].v[c];
			const double straight = (p1 - p0) / h;
			const double m0 = i > 0 ? (p1 - k[i - 1].v[c]) / (t1 - k[i - 1].ms) : straight;
			const double m1 = i + 2 < k.size() ? (k[i + 2].v[c] - p0) / (k[i + 2].ms - t0) : straight;
			out[c] = static_cast<float>(h00 * p0 + h10 * h * m0 + h01 * p1 + h11 * h * m1);
		}
	}

	float key_button_width()
	{
		return ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x;
	}

	// The diamond at a row's right. Hollow: no keyframes; filled: a keyframe at the time shown; outlined
	// in the accent colour: keyframes at other times.
	void key_button(const char* id, const char* label, const int count, const float* values)
	{
		ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
		const float size = ImGui::GetFrameHeight();
		const ImVec2 corner = ImGui::GetCursorScreenPos();
		ImGui::PushID(id);
		const bool pressed = ImGui::InvisibleButton("##key", {size, size});
		const bool hovered = ImGui::IsItemHovered();
		ImGui::PopID();
		Track* track = track_of(id);
		const int here = track ? key_at(*track, g_now_ms) : -1;
		const ImVec2 middle = {corner.x + size * 0.5f, corner.y + size * 0.5f};
		const float r = size * (hovered ? 0.30f : 0.26f);
		const ImU32 accent = ImGui::GetColorU32(ImGuiCol_SliderGrabActive), dim = ImGui::GetColorU32(ImGuiCol_TextDisabled);
		ImDrawList* draw = ImGui::GetWindowDrawList();
		const ImVec2 a = {middle.x, middle.y - r}, b = {middle.x + r, middle.y}, c = {middle.x, middle.y + r}, d = {middle.x - r, middle.y};
		if (here >= 0)
		{
			draw->AddQuadFilled(a, b, c, d, accent);
		}
		else
		{
			draw->AddQuad(a, b, c, d, track ? accent : dim, track ? 2.0f : 1.5f);
		}

		if (hovered)
		{
			ImGui::SetTooltip(here >= 0 ? "Remove this keyframe" : track ? "Add a keyframe here" : "Animate: add a keyframe here.\nWith keyframes, changing the value at another time adds one there.");
		}

		if (pressed)
		{
			if (here >= 0)
			{
				track->keys.erase(track->keys.begin() + here);
				if (track->keys.empty())
				{
					g_tracks.erase(g_tracks.begin() + (track - g_tracks.data()));
				}
			}
			else
			{
				put_key(id, label, count, values);
			}
		}
	}

	// A change made by hand to a setting that has keyframes is a keyframe at the time shown.
	void key_changed(const char* id, const float* values)
	{
		if (Track* track = track_of(id))
		{
			put_key(id, track->label.c_str(), track->count, values);
		}
	}

	void remove_track(const char* id)
	{
		if (const Track* track = track_of(id))
		{
			g_tracks.erase(g_tracks.begin() + (track - g_tracks.data()));
		}
	}

	// While a control is held its own value is shown: the game takes a change a frame later.
	struct Held
	{
		ImGuiID id = 0;
		float value[4] = {};
		int integer = 0;
	} g_held;

	// A colour just chosen is shown as chosen for a moment: the picker reads its colour back every frame,
	// and the game's own copy is a frame behind and rounded.
	struct Chosen
	{
		float value[4] = {};
		ULONGLONG until = 0;
	};
	std::map<const unsigned char*, Chosen> g_chosen;

	// The control for a dvar, by its type. True if it was changed.
	bool control(const Dvar dvar, const char* label = nullptr)
	{
		ImGui::PushID(dvar.at);
		const int kind = dvar.type();
		const bool keyable = kind == 1 || kind == 2 || kind == 3 || kind == 4 || kind == 5 || kind == 8 || kind == 9;
		// (room at the row's right for the keyframe diamond and the "back to default" button)
		const float reset_width = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x;
		ImGui::SetNextItemWidth(keyable ? -(key_button_width() + reset_width) : -reset_width);
		float shown[4] = {};  // the value the row shows, for a keyframe
		bool changed = false;
		const unsigned char* value = dvar.current();
		const float* limits = reinterpret_cast<const float*>(dvar.limits());
		const int type = dvar.type();
		const ImGuiID id = ImGui::GetID("##value");
		const bool held = g_held.id == id;
		switch (type)
		{
		case 0:
		{
			bool on = value[0] != 0;
			if (ImGui::Checkbox("##value", &on))
			{
				set(dvar, "%d", on ? 1 : 0);
				changed = true;
			}

			break;
		}
		case 1:
		{
			float v = held ? g_held.value[0] : *reinterpret_cast<const float*>(value);
			const float low = limits[0], high = limits[1];
			const bool slider = low > -1.0e6f && high < 1.0e6f && high > low;
			const float span = slider ? high - low : 0.0f;
			const char* format = span > 0.0f && span <= 4.0f ? "%.3f" : span <= 100.0f ? "%.2f" : "%.1f";
			const bool edited = slider && span <= 20000.0f
				? ImGui::SliderFloat("##value", &v, low, high, format, span > 2000.0f ? ImGuiSliderFlags_Logarithmic : 0)
				: ImGui::DragFloat("##value", &v, std::fabs(v) > 100.0f ? std::fabs(v) * 0.01f : 0.05f, slider ? low : 0.0f, slider ? high : 0.0f,
				                   "%.3f");
			if (edited)
			{
				set(dvar, "%g", v);
				changed = true;
			}

			shown[0] = v;
			if (ImGui::IsItemActive())
			{
				g_held.id = id;
				g_held.value[0] = v;
			}
			else if (held)
			{
				g_held.id = 0;
			}

			break;
		}
		case 2:
		case 3:
		case 4:
		{
			float v[4] = {};
			memcpy(v, held ? reinterpret_cast<const unsigned char*>(g_held.value) : value, sizeof(float) * type);
			const float low = limits[0], high = limits[1];
			const bool bounded = low > -1.0e6f && high < 1.0e6f && high > low;
			const float speed = bounded ? (high - low) / 500.0f : 0.05f;
			if (ImGui::DragScalarN("##value", ImGuiDataType_Float, v, type, speed, bounded ? &low : nullptr, bounded ? &high : nullptr, "%.3f"))
			{
				set(dvar, type == 2 ? "%g %g" : type == 3 ? "%g %g %g" : "%g %g %g %g", v[0], v[1], v[2], v[3]);
				changed = true;
			}

			memcpy(shown, v, sizeof(shown));

			if (ImGui::IsItemActive())
			{
				g_held.id = id;
				memcpy(g_held.value, v, sizeof(v));
			}
			else if (held)
			{
				g_held.id = 0;
			}

			break;
		}
		case 5:
		{
			int v = held ? g_held.integer : *reinterpret_cast<const int*>(value);
			const int low = *reinterpret_cast<const int*>(dvar.limits()), high = *reinterpret_cast<const int*>(dvar.limits() + 4);
			const bool slider = high > low && static_cast<long long>(high) - low <= 4096;
			if (slider ? ImGui::SliderInt("##value", &v, low, high) : ImGui::DragInt("##value", &v, 1.0f, low, high))
			{
				set(dvar, "%d", v);
				changed = true;
			}

			shown[0] = static_cast<float>(v);

			if (ImGui::IsItemActive())
			{
				g_held.id = id;
				g_held.integer = v;
			}
			else if (held)
			{
				g_held.id = 0;
			}

			break;
		}
		case 6:
		{
			const int now = *reinterpret_cast<const int*>(value);
			const int count = *reinterpret_cast<const int*>(dvar.limits());
			const char* const* names = *reinterpret_cast<const char* const* const*>(dvar.limits() + 8);
			const char* current = names && now >= 0 && now < count && names[now] ? names[now] : "?";
			if (ImGui::BeginCombo("##value", current))
			{
				for (int i = 0; names && i < count && i < 64; ++i)
				{
					if (names[i] && ImGui::Selectable(names[i], i == now))
					{
						set(dvar, "\"%s\"", names[i]);
						changed = true;
					}
				}

				ImGui::EndCombo();
			}

			break;
		}
		case 7:
		{
			// while it is typed in, the box shows what was typed; otherwise what the game has
			static const unsigned char* editing = nullptr;
			static std::string typed;
			const char* string = *reinterpret_cast<const char* const*>(value);
			char text[200];
			snprintf(text, sizeof(text), "%s", editing == dvar.at ? typed.c_str() : string ? string : "");
			if (ImGui::InputText("##value", text, sizeof(text), ImGuiInputTextFlags_EnterReturnsTrue))
			{
				set(dvar, "\"%s\"", text);
				changed = true;
			}

			if (ImGui::IsItemActive())
			{
				editing = dvar.at;
				typed = text;
			}
			else if (editing == dvar.at)
			{
				editing = nullptr;
			}

			break;
		}
		case 8:
		case 9:
		{
			// 8: four bytes. 9: three floats, which may be above 1 (the film tints are: 1.1 is a tint
			// that brightens), so its numbers are shown beside the swatch and are not held to 0..1.
			float v[4] = {value[0] / 255.0f, value[1] / 255.0f, value[2] / 255.0f, value[3] / 255.0f};
			if (type == 9)
			{
				memcpy(v, value, sizeof(float) * 3);
				v[3] = 1.0f;
			}

			const ULONGLONG tick = GetTickCount64();
			const auto chosen = g_chosen.find(dvar.at);
			if (chosen != g_chosen.end())
			{
				if (tick < chosen->second.until)
				{
					memcpy(v, chosen->second.value, sizeof(v));
				}
				else
				{
					g_chosen.erase(chosen);
				}
			}

			const bool edited = type == 9
				? ImGui::ColorEdit3("##value", v, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR)
				: ImGui::ColorEdit4("##value", v, ImGuiColorEditFlags_NoInputs);
			if (edited)
			{
				if (type == 9)
				{
					set(dvar, "%g %g %g", v[0], v[1], v[2]);
				}
				else
				{
					set(dvar, "%g %g %g %g", v[0], v[1], v[2], v[3]);
				}

				Chosen& keep = g_chosen[dvar.at];
				memcpy(keep.value, v, sizeof(v));
				keep.until = tick + 250;
				changed = true;
			}

			memcpy(shown, v, sizeof(shown));
			break;
		}
		default:
			ImGui::TextDisabled("?");
			break;
		}

		if (keyable)
		{
			const int count = kind == 2 ? 2 : kind == 3 || kind == 9 ? 3 : kind == 4 || kind == 8 ? 4 : 1;
			if (changed)
			{
				key_changed(dvar.name(), shown);
			}

			key_button(dvar.name(), label ? label : dvar.name(), count, shown);
		}

		// (right click on the row: the setting as the game had it, its keyframes gone)
		ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ImGui::GetFrameHeight());
		const bool reset_pressed = ui_reset_button("##rowReset");
		if (ui_reset_menu("##row") || reset_pressed)
		{
			remove_track(dvar.name());
			restore(dvar.name());
		}

		ImGui::PopID();
		return changed;
	}

	struct Entry
	{
		const char* label;
		const char* name;
	};

	struct Section
	{
		const char* icon;
		const char* label;
		bool open;
		std::vector<Entry> entries;
	};

	// IrisStudio's Look sections, with the settings of Advanced Warfare's that were found to act (0.30
	// listed every candidate; those that did nothing in the game, or broke the picture, are gone: the
	// sun's own flare settings (r_sun_from_dvars turned the sun into a missing texture), the sky fog, the
	// light strengths and r_zfar). The Sun section's light is sun.cpp's.
	const Section SECTIONS[] = {
		{ICON_SUN, "Sun", true,
		 {{"Sun in the sky", "r_drawSun"},
		  {"Sun shadows", "sm_sunEnable"},
		  {"Sun shadow softness", "sm_sunFilterRadius"}}},
		{ICON_LIGHTBULB, "Lighting", false,
		 {{"Shadows", "sm_enable"},
		  {"Baked lighting", "r_lightMap"},
		  {"Without lighting", "r_fullbright"},
		  {"Glow", "r_veil"},
		  {"Use the glow strength below", "r_veilUseTweaks"},
		  {"Glow strength", "r_veilStrength"},
		  {"Ambient occlusion", "r_ssao"},
		  {"Use the occlusion strength below", "r_ssaoUseTweaks"},
		  {"Occlusion strength", "r_ssaoStrength"}}},
		// (the fog's own colour and thickness are the map's script's, not settings: see passes.cpp for those)
		{ICON_CLOUD, "Fog", false,
		 {{"Fog", "r_fog"},
		  {"Fog brightness", "r_fog_ev_adjust"},
		  {"Fog over the sky", "r_sky_fog_intensity"},
		  {"Sky fog from angle", "r_sky_fog_min_angle"},
		  {"Sky fog up to angle", "r_sky_fog_max_angle"}}},
		{ICON_EYE, "Depth of Field", false,
		 {{"Depth of field", "r_dof_enable"},
		  {"Use the values below", "r_dof_tweak"},
		  {"Near blur", "r_dof_nearBlur"},
		  {"Near start", "r_dof_nearStart"},
		  {"Near end", "r_dof_nearEnd"},
		  {"Far blur", "r_dof_farBlur"},
		  {"Far start", "r_dof_farStart"},
		  {"Far end", "r_dof_farEnd"},
		  {"Bias", "r_dof_bias"},
		  {"Gun start", "r_dof_viewModelStart"},
		  {"Gun end", "r_dof_viewModelEnd"},
		  {"Lens model", "r_dof_physical_enable"},
		  {"Focus distance", "r_dof_physical_focusDistance"},
		  {"Aperture (f-stop)", "r_dof_physical_fstop"},
		  {"Film size", "r_dof_physical_filmDiagonal"},
		  {"Bokeh", "r_dof_physical_bokehEnable"},
		  {"Bokeh quality", "r_dof_physical_bokehPreset"},
		  {"Bokeh shape", "r_dof_physical_bokehShape"},
		  {"Bokeh rotation", "r_dof_physical_bokehRotation"},
		  {"Bokeh sharpness", "r_dof_physical_bokehSharpness"}}},
		{ICON_FILM, "Film", false,
		 {{"Use the values below", "r_filmUseTweaks"},
		  {"Film effects", "r_filmTweakEnable"},
		  {"Brightness", "r_filmTweakBrightness"},
		  {"Contrast", "r_filmTweakContrast"},
		  {"Desaturation", "r_filmTweakDesaturation"},
		  {"Desaturation, dark", "r_filmTweakDesaturationDark"},
		  {"Light tint", "r_filmTweakLightTint"},
		  {"Medium tint", "r_filmTweakMediumTint"},
		  {"Dark tint", "r_filmTweakDarkTint"},
		  {"Invert", "r_filmTweakInvert"}}},
		// The exposure, before the picture is brought down to what a screen shows: darkening here keeps what is
		// far brighter than the scene (a muzzle flash, an explosion, a lamp) bright, where the Film section's
		// brightness, which works on the finished picture, pulls those down to grey with everything else.
		// (0.75: not yet known which of these act in this game.)
		// (0.83: these were settings set through the console, "r_tonemap...", and did nothing; now exposure.cpp
		// writes the values into the scene, and the section is drawn by draw_tone below.)
		// How finely models are drawn. The game smooths some models by subdividing them (r_subdiv,
		// r_adaptiveSubdiv) and swaps to coarser ones with distance (the rest).
		{ICON_PERSON, "Models", false,
		 {{"Detail level", "r_forceLod"},
		  {"Detail kept longer, characters", "r_lodBiasSkinned"},
		  {"Detail kept longer, objects", "r_lodBiasRigid"},
		  {"Detail falloff, characters", "r_lodScaleSkinned"},
		  // (the smoothing and surface relief settings were here to 0.57: multiplayer's models are not made
		  // for either, and they changed nothing; they are still under All settings)
		  {"Detail falloff, objects", "r_lodScaleRigid"}}},
		{ICON_WAND, "Effects", false,
		 {{"Effects", "fx_enable"},
		  {"Draw effects", "fx_draw"},
		  {"Lens flares", "fx_flare"},
		  {"Bullet marks", "fx_marks"},
		  {"Post processing", "r_postfx_enable"},
		  {"Post processing", "r_postFX"},
		  {"Distortion", "r_distortion"},
		  {"Blood", "cg_blood"},
		  {"Shell casings", "cg_brass"},
		  {"Field of view (first person)", "cg_fov"},
		  {"Field of view scale", "cg_fovScale"},
		  {"Gun forward", "cg_gun_x"},
		  {"Gun right", "cg_gun_y"},
		  {"Gun up", "cg_gun_z"}}},
	};

	const Entry HUD_SETTINGS[] = {{"2D Elements (F3)", "cg_draw2D"}, {"Gun", "cg_drawGun"}, {"Names over players", "cg_drawFriendlyNames"}};

	// The sun as a light, as IrisStudio's Sun section has it: colour, brightness, direction.
	void draw_sun_light(const bool reset)
	{
		if (reset)
		{
			remove_track("sun_color");
			remove_track("sun_brightness");
			remove_track("sun_direction");
			sun_reset();
		}

		const SunState sun = sun_state();
		if (!sun.known)
		{
			ImGui::TextDisabled("This map's sun light has not been seen yet.");
			return;
		}

		float color[3] = {sun.color[0], sun.color[1], sun.color[2]};
		ui_row_label("Color");
		ImGui::SetNextItemWidth(-key_button_width());
		if (ImGui::ColorEdit3("##sunColor", color, ImGuiColorEditFlags_NoAlpha))
		{
			sun_set_color(color);
			key_changed("sun_color", color);
		}

		key_button("sun_color", "Sun color", 3, color);
		float brightness = sun.brightness;
		ui_row_label("Brightness");
		ImGui::SetNextItemWidth(-key_button_width());
		if (ImGui::DragFloat("##sunBrightness", &brightness, brightness > 20.0f ? 0.5f : 0.05f, 0.0f, 1000.0f, "%.2f"))
		{
			sun_set_brightness(brightness);
			key_changed("sun_brightness", &brightness);
		}

		key_button("sun_brightness", "Sun brightness", 1, &brightness);
		float direction[2] = {sun.pitch, sun.yaw};
		ui_row_label("Direction");
		ImGui::SetNextItemWidth(-key_button_width());
		if (ImGui::DragFloat2("##sunDirection", direction, 0.25f, -720.0f, 720.0f, "%.1f"))
		{
			sun_set_direction(direction[0], direction[1]);
			key_changed("sun_direction", direction);
		}

		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("Up and down, then around: where the sunlight and its shadows come from");
		}

		key_button("sun_direction", "Sun direction", 2, direction);
	}

	// The sky's picture (sky.cpp): colour and turning.
	void draw_sky(const bool reset)
	{
		static const char* const TRACKS[] = {"sky_hue", "sky_saturation", "sky_brightness", "sky_turn"};
		if (reset)
		{
			for (const char* track : TRACKS)
			{
				remove_track(track);
			}

			sky_reset();
		}

		SkyState sky = sky_state();
		if (sky.failed)
		{
			ImGui::PushTextWrapPos(0.0f);
			ImGui::TextDisabled("This map's sky picture could not be found, so it cannot be changed here. The log says what was seen.");
			ImGui::PopTextWrapPos();
			return;
		}

		// the picture: this map's own sky, or one saved on another map
		static char saved[32][64];
		const int saved_count = sky_saved(saved, 32);
		ui_row_label("Picture");
		if (ImGui::BeginCombo("##skyPicture", sky.picture[0] ? sky.picture : "This map's own"))
		{
			if (ImGui::Selectable("This map's own", !sky.picture[0]))
			{
				sky_use("");
			}

			for (int i = 0; i < saved_count; ++i)
			{
				if (ImGui::Selectable(saved[i], strcmp(saved[i], sky.picture) == 0))
				{
					sky_use(saved[i]);
				}
			}

			ImGui::EndCombo();
		}

		if (ImGui::Button("Save this map's sky"))
		{
			sky_save();
		}

		ImGui::SetItemTooltip("Keeps this map's sky so it can be chosen as the Picture on any other map");
		if (sky.message[0])
		{
			ImGui::PushTextWrapPos(0.0f);
			ImGui::TextDisabled("%s", sky.message);
			ImGui::PopTextWrapPos();
		}

		bool changed = false;
		ui_row_label("Hue");
		ImGui::SetNextItemWidth(-key_button_width());
		ui::Default(0.0f);
		if (ui::SliderFloat("##skyHue", &sky.hsv[0], -180.0f, 180.0f, "%.0f deg"))
		{
			changed = true;
			key_changed("sky_hue", &sky.hsv[0]);
		}

		key_button("sky_hue", "Sky hue", 1, &sky.hsv[0]);
		ui_row_label("Saturation");
		ImGui::SetNextItemWidth(-key_button_width());
		ui::Default(1.0f);
		if (ui::SliderFloat("##skySaturation", &sky.hsv[1], 0.0f, 3.0f, "%.2f"))
		{
			changed = true;
			key_changed("sky_saturation", &sky.hsv[1]);
		}

		key_button("sky_saturation", "Sky saturation", 1, &sky.hsv[1]);
		ui_row_label("Brightness");
		ImGui::SetNextItemWidth(-key_button_width());
		ui::Default(1.0f);
		if (ui::SliderFloat("##skyValue", &sky.hsv[2], 0.0f, 4.0f, "%.2f"))
		{
			changed = true;
			key_changed("sky_brightness", &sky.hsv[2]);
		}

		key_button("sky_brightness", "Sky brightness", 1, &sky.hsv[2]);
		ui_row_label("Rotation");
		ImGui::SetNextItemWidth(-key_button_width());
		ui::Default(0.0f, 0.0f, 0.0f);
		if (ui::DragFloat3("##skyTurn", sky.turn, 0.5f, -720.0f, 720.0f, "%.1f"))
		{
			changed = true;
			key_changed("sky_turn", sky.turn);
		}

		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("Around, then the two tilts. The sun in the sky and the light on the map stay where they are.");
		}

		key_button("sky_turn", "Sky rotation", 3, sky.turn);
		if (changed)
		{
			sky_set(sky.hsv, sky.turn);
		}
	}

	// ---- the HUD's parts, from the game's own menus: a script the editor installs with the game
	// (s1x\ui_scripts\s1iris) lists the parts of the match HUD in dvars (s1iris_hud_count, s1iris_hud_0
	// and so on, each "depth name") and hides those named in s1iris_hud_hide.
	std::set<std::string> g_hud_hidden;
	bool g_hud_touched = false;

	std::string dvar_string(const char* name)
	{
		const Dvar dvar = find(name);
		if (!dvar || dvar.type() != 7)
		{
			return {};
		}

		const char* text = *reinterpret_cast<const char* const*>(dvar.current());
		return text && !IsBadReadPtr(text, 1) ? std::string(text, strnlen(text, 300)) : std::string();
	}

	void send_hud_hidden()
	{
		std::string list;
		for (const std::string& name : g_hud_hidden)
		{
			list += (list.empty() ? "" : " ") + name;
		}

		g_hud_touched = true;
		theater_command(("set s1iris_hud_hide \"" + list + "\"").c_str());
		theater_hud_wake();
	}

	void draw_hud(const bool reset)
	{
		for (const Entry& entry : HUD_SETTINGS)
		{
			if (const Dvar dvar = find(entry.name))
			{
				if (reset)
				{
					restore(entry.name);
				}

				ui_row_label(entry.label);
				control(dvar);
			}
		}

		if (reset && !g_hud_hidden.empty())
		{
			g_hud_hidden.clear();
			send_hud_hidden();
		}

		{
			// ReShade's effects kept off the HUD (hudkeep.cpp)
			bool keep = hud_keep_get();
			ui_row_label("Sharp under ReShade");
			if (ImGui::Checkbox("##hudKeep", &keep))
			{
				hud_keep_set(keep);
			}

			ImGui::SetItemTooltip("ReShade's effects (a blur, a depth of field, a colour grade) are made on the scene alone, and the game's HUD\n(the kill feed, the score popups, everything drawn flat over the picture) is put back over the result untouched.\nNow: %s", hud_keep_status());
		}

		const int count = atoi(dvar_string("s1iris_hud_count").c_str());
		if (count <= 0)
		{
			// (how the script is doing, as it says itself: shown, and logged when it changes)
			const std::string script_state = dvar_string("s1iris_lua_state"), script_error = dvar_string("s1iris_lua_error");
			static std::string said;
			if (said != script_state + "|" + script_error)
			{
				said = script_state + "|" + script_error;
				ui_log("hud script: state \"%s\", error \"%s\"", script_state.c_str(), script_error.c_str());
			}

			ImGui::PushTextWrapPos(0.0f);
			if (!script_error.empty())
			{
				ImGui::TextDisabled("The HUD's parts cannot be listed: the script that lists them has a fault (%s).", script_error.c_str());
			}
			else if (script_state.empty())
			{
				ImGui::TextDisabled("The HUD's separate parts are listed here by a small script the game loads when it starts "
				                    "(s1x\\ui_scripts\\s1iris). The game has not loaded it: restart the game.");
			}
			else
			{
				ImGui::TextDisabled("The HUD's separate parts are listed here once a match's HUD is on screen (the script: %s).", script_state.c_str());
			}

			ImGui::PopTextWrapPos();
			return;
		}

		struct Part
		{
			int depth;
			std::string name;
		};
		std::vector<Part> parts;
		for (int i = 0; i < count && i < 160; ++i)
		{
			char dvar_name[40];
			snprintf(dvar_name, sizeof(dvar_name), "s1iris_hud_%d", i);
			const std::string line = dvar_string(dvar_name);
			const size_t space = line.find(' ');
			if (space != std::string::npos)
			{
				parts.push_back({atoi(line.c_str()), line.substr(space + 1)});
			}
		}

		// (the list goes to the log once, to name the parts by)
		static int logged = 0;
		if (logged != static_cast<int>(parts.size()))
		{
			logged = static_cast<int>(parts.size());
			std::string all;
			for (const Part& part : parts)
			{
				all += (all.empty() ? "" : ", ") + part.name;
				if (all.size() > 1500)
				{
					ui_log("hud parts: %s", all.c_str());
					all.clear();
				}
			}

			ui_log("hud parts: %s", all.c_str());
		}

		auto toggle = [&](const std::string& name, bool shown) {
			if (shown)
			{
				g_hud_hidden.erase(name);
			}
			else
			{
				g_hud_hidden.insert(name);
			}

			send_hud_hidden();
		};

		// the parts that have been matched to what they are
		// (a part named in full is not always there: it is offered all the same, and hidden whenever it comes)
		static const struct { const char* label; const char* part; const char* full; } KNOWN[] = {
			{"Kill feed", "wrapper_for_obituary_rotation_id", nullptr},
			{"Score popups (+100)", "wrapper_for_pointsPopup", nullptr},
			{"Hitmarkers", "wrapper_for_damageFeedbackHud", nullptr},
			{"Weapon and streaks", "wrapper_for_baseWeaponInfoHud", nullptr},
			{"Minimap", "wrapper_for_minimapHud", nullptr},
			{"Team scores", "wrapper_for_teamScores_rotation_id", nullptr},
			{"Button hints", "wrapper_for_hintsContainerHudId", nullptr},
			{"Game messages", "wrapper_for_gameMessageHudId", nullptr},
			{"Medals and splashes", "wrapper_for_splashesHud", nullptr},
			// "ASSAULT DRONE READY! Press 4 to Activate": a part with no name of its own, put straight onto the
			// HUD's "static" layer when a scorestreak is earned (an icon and two lines of text) and taken off after
			{"Scorestreak ready popup", nullptr, "static.LUIElement"},
		};

		// at a click: only what a kill shows (the feed, the score, the hitmarker), or everything
		if (ImGui::Button("Kills only"))
		{
			g_hud_hidden.clear();
			for (const Part& part : parts)
			{
				// (the parts themselves are the third level down; what holds them stays)
				const bool kept = part.name.find("obituary") != std::string::npos || part.name.find("pointsPopup") != std::string::npos ||
				                  part.name.find("damageFeedback") != std::string::npos;
				if (std::count(part.name.begin(), part.name.end(), '.') == 2 && !kept)
				{
					g_hud_hidden.insert(part.name);
				}
			}

			// (and the parts that are only there now and then)
			for (const auto& known : KNOWN)
			{
				if (known.full)
				{
					g_hud_hidden.insert(known.full);
				}
			}

			send_hud_hidden();
		}

		ImGui::SetItemTooltip("Hides every part of the HUD but the kill feed, the score popups and the hitmarkers");
		ImGui::SameLine();
		if (ImGui::Button("Show everything"))
		{
			g_hud_hidden.clear();
			send_hud_hidden();
		}

		for (const auto& known : KNOWN)
		{
			if (known.full)
			{
				bool shown = !g_hud_hidden.count(known.full);
				ui_row_label(known.label);
				ImGui::PushID(known.full);
				if (ImGui::Checkbox("##shown", &shown))
				{
					toggle(known.full, shown);
				}

				ImGui::PopID();
				continue;
			}

			for (const Part& part : parts)
			{
				const size_t dot = part.name.rfind('.');
				if ((dot == std::string::npos ? part.name : part.name.substr(dot + 1)) == known.part)
				{
					bool shown = !g_hud_hidden.count(part.name);
					ui_row_label(known.label);
					ImGui::PushID(known.part);
					if (ImGui::Checkbox("##shown", &shown))
					{
						toggle(part.name, shown);
					}

					ImGui::PopID();
					break;
				}
			}
		}

		// every part by the game's own name, out of the way
		char label[64];
		snprintf(label, sizeof(label), "All HUD parts (%d)", static_cast<int>(parts.size()));
		if (ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_SpanAvailWidth))
		{
			int index = 0;
			for (const Part& part : parts)
			{
				bool shown = !g_hud_hidden.count(part.name);
				const size_t dot = part.name.rfind('.');
				ImGui::PushID(index++);
				// (the box first, then the name: a long name used to run under the box)
				ImGui::Indent(part.depth * ui_font_size() * 0.8f + 0.01f);
				if (ImGui::Checkbox("##shown", &shown))
				{
					toggle(part.name, shown);
				}

				ImGui::SameLine();
				ImGui::TextUnformatted(dot == std::string::npos ? part.name.c_str() : part.name.c_str() + dot + 1);
				ImGui::Unindent(part.depth * ui_font_size() * 0.8f + 0.01f);
				ImGui::PopID();
			}

			ImGui::TreePop();
		}
	}

	// ---- presets: what the Look tab was set to, kept by name in theater\presets and put back on at a click.
	// A preset holds every setting the editor has changed, the HUD's hidden parts, and the sun's light and
	// the sky where those were set.
	std::string presets_folder()
	{
		const std::string folder = std::string(theater_folder()) + "\\presets";
		CreateDirectoryA(folder.c_str(), nullptr);
		return folder;
	}

	std::vector<std::string> list_presets()
	{
		static std::vector<std::string> names;
		static ULONGLONG listed = 0;
		const ULONGLONG now = GetTickCount64();
		if (listed && now - listed < 1500)
		{
			return names;
		}

		listed = now;
		names.clear();
		WIN32_FIND_DATAA found;
		const HANDLE search = FindFirstFileA((presets_folder() + "\\*.txt").c_str(), &found);
		if (search != INVALID_HANDLE_VALUE)
		{
			do
			{
				std::string name = found.cFileName;
				names.push_back(name.substr(0, name.size() - 4));
			} while (FindNextFileA(search, &found));
			FindClose(search);
		}

		return names;
	}

	// A preset's file: in the presets folder by its name; or the file itself, when a whole path is given (the look
	// kept beside a recording).
	std::string preset_path(const std::string& name)
	{
		return name.find(':') != std::string::npos ? name : presets_folder() + "\\" + name + ".txt";
	}

	bool save_preset(const std::string& name)
	{
		FILE* file = nullptr;
		if (fopen_s(&file, preset_path(name).c_str(), "w") != 0 || !file)
		{
			return false;
		}

		for (const auto& changed : g_originals)
		{
			if (const Dvar dvar = find(changed.first.c_str()))
			{
				fprintf(file, "dvar %s %s\n", changed.first.c_str(), text_of(dvar, dvar.current()).c_str());
			}
		}

		// (the HUD's parts that are hidden: the line is there with none, so that bringing the HUD in shows them all)
		std::string hidden;
		for (const std::string& part : g_hud_hidden)
		{
			hidden += " " + part;
		}

		fprintf(file, "hud_hide%s\n", hidden.c_str());
		const SunState sun = sun_state();
		if (sun.color_set)
		{
			fprintf(file, "sun_color %g %g %g\n", sun.color[0], sun.color[1], sun.color[2]);
		}

		if (sun.brightness_set)
		{
			fprintf(file, "sun_brightness %g\n", sun.brightness);
		}

		if (sun.direction_set)
		{
			fprintf(file, "sun_direction %g %g\n", sun.pitch, sun.yaw);
		}

		if (thermal_mode() != 0)
		{
			fprintf(file, "tone_thermal %d\n", thermal_mode());
		}

		if (g_rs_effect[0] && g_rs_name[0])
		{
			fprintf(file, "reshade_focus %d %d %g %g %s %s\n", g_rs_on ? 1 : 0, g_rs_only ? 1 : 0, g_rs_scale, g_rs_distance, g_rs_effect, g_rs_name);
		}

		{
			// the exposure and the tone curve: only what was changed
			ToneState map, mine;
			std::uint32_t tone_bits = 0;
			tone_get(&map, &mine, &tone_bits);
			for (int i = 0; i < TONE_FLOATS; ++i)
			{
				if (tone_bits & (1u << i))
				{
					fprintf(file, "tone_value %d %g\n", i, mine.f[i]);
				}
			}

			if (tone_bits & TONE_SET_CURVE)
			{
				fprintf(file, "tone_curve %d\n", mine.curve);
			}

			if (tone_bits & TONE_SET_AUTO)
			{
				fprintf(file, "tone_auto %d\n", mine.automatic ? 1 : 0);
			}

			if (tone_bits & TONE_SET_LOCK)
			{
				fprintf(file, "tone_lock %d\n", mine.lock ? 1 : 0);
			}
		}

		const SkyState sky = sky_state();
		if (sky.changed)
		{
			fprintf(file, "sky %g %g %g %g %g %g\n", sky.hsv[0], sky.hsv[1], sky.hsv[2], sky.turn[0], sky.turn[1], sky.turn[2]);
			if (sky.picture[0])
			{
				fprintf(file, "sky_picture %s\n", sky.picture);
			}
		}

		fclose(file);
		return true;
	}

	// ---- what a preset holds, by the Look tab's sections: a preset can be brought in whole or a section
	// at a time (the HUD's without the sun's, and so on). A section brought in is made as the preset has
	// it, what the preset leaves alone going back to the game's own; a section not brought in is not
	// touched.
	const char* const GROUP_HUD = "HUD";
	const char* const GROUP_SUN = "Sun";
	const char* const GROUP_SKY = "Sky";
	const char* const GROUP_TONE = "Exposure";
	const char* const GROUP_OTHER = "Other settings";

	std::vector<std::string> all_groups()
	{
		std::vector<std::string> groups = {GROUP_HUD};
		for (const Section& section : SECTIONS)
		{
			groups.push_back(section.label);
			if (strcmp(section.label, GROUP_SUN) == 0)
			{
				groups.push_back(GROUP_SKY);
			}

			if (strcmp(section.label, "Film") == 0)
			{
				groups.push_back(GROUP_TONE);
			}
		}

		groups.push_back(GROUP_OTHER);
		return groups;
	}

	std::string group_of_setting(const std::string& name)
	{
		for (const Entry& entry : HUD_SETTINGS)
		{
			if (name == entry.name)
			{
				return GROUP_HUD;
			}
		}

		for (const Section& section : SECTIONS)
		{
			for (const Entry& entry : section.entries)
			{
				if (name == entry.name)
				{
					return section.label;
				}
			}
		}

		return GROUP_OTHER;
	}

	// (a preset's line: the section it belongs to, "" for a line that is not known)
	std::string group_of_line(const char* line)
	{
		if (strncmp(line, "dvar ", 5) == 0)
		{
			const char* end = strchr(line + 5, ' ');
			return end ? group_of_setting(std::string(line + 5, end)) : std::string();
		}

		if (strncmp(line, "hud_hide", 8) == 0)
		{
			return GROUP_HUD;
		}

		if (strncmp(line, "sun_", 4) == 0)
		{
			return GROUP_SUN;
		}

		if (strncmp(line, "tone_", 5) == 0)
		{
			return GROUP_TONE;
		}

		if (strncmp(line, "reshade_", 8) == 0)
		{
			return "Depth of Field";
		}

		return strncmp(line, "sky", 3) == 0 ? GROUP_SKY : std::string();
	}

	// The sections a preset has something for.
	std::set<std::string> preset_groups(const std::string& name)
	{
		std::set<std::string> groups;
		FILE* file = nullptr;
		if (fopen_s(&file, preset_path(name).c_str(), "r") != 0 || !file)
		{
			return groups;
		}

		char line[2048];
		while (fgets(line, sizeof(line), file))
		{
			line[strcspn(line, "\r\n")] = 0;
			// (a HUD line with no parts named changes nothing by itself)
			if (strcmp(line, "hud_hide") == 0)
			{
				continue;
			}

			const std::string group = group_of_line(line);
			if (!group.empty())
			{
				groups.insert(group);
			}
		}

		fclose(file);
		return groups;
	}

	// only: the sections to bring in (null: all of them).
	bool load_preset(const std::string& name, const std::set<std::string>* only)
	{
		FILE* file = nullptr;
		if (fopen_s(&file, preset_path(name).c_str(), "r") != 0 || !file)
		{
			return false;
		}

		const auto wanted = [only](const std::string& group) { return !only || only->count(group) != 0; };
		std::set<std::string> in_preset;
		if (wanted(GROUP_SUN))
		{
			sun_reset();
		}

		if (wanted(GROUP_SKY))
		{
			sky_reset();
		}

		ToneState tone_map, tone_mine;
		std::uint32_t tone_bits = 0;
		tone_get(&tone_map, &tone_mine, &tone_bits);
		if (wanted(GROUP_TONE))
		{
			tone_bits = 0;
			thermal_set_mode(0);
		}

		char line[2048];
		while (fgets(line, sizeof(line), file))
		{
			line[strcspn(line, "\r\n")] = 0;
			const std::string group = group_of_line(line);
			if (group.empty() || !wanted(group))
			{
				continue;
			}

			float a = 0.0f, b = 0.0f, c = 0.0f;
			int n = 0;
			if (strncmp(line, "dvar ", 5) == 0)
			{
				char* value = strchr(line + 5, ' ');
				if (value)
				{
					*value++ = 0;
					if (const Dvar dvar = find(line + 5))
					{
						in_preset.insert(line + 5);
						set(dvar, std::string(value));
					}
				}
			}
			else if (strncmp(line, "hud_hide", 8) == 0)
			{
				// (presets from before 0.61 have no such line: the HUD's parts are left as they are)
				g_hud_hidden.clear();
				char* rest = nullptr;
				for (char* part = strtok_s(line + 8, " ", &rest); part; part = strtok_s(nullptr, " ", &rest))
				{
					g_hud_hidden.insert(part);
				}

				send_hud_hidden();
			}
			else if (int on = 0, alone = 0; sscanf_s(line, "reshade_focus %d %d %f %f", &on, &alone, &a, &b) == 4)
			{
				// (then the effect's file and the setting's name, neither with a space in it)
				char effect[64] = "", name[64] = "";
				const char* rest = line;
				for (int skip = 0; skip < 5 && rest; ++skip)
				{
					rest = strchr(rest, ' ');
					rest = rest ? rest + 1 : nullptr;
				}

				if (rest && sscanf_s(rest, "%63s %63s", effect, static_cast<unsigned>(sizeof(effect)), name, static_cast<unsigned>(sizeof(name))) == 2)
				{
					g_rs_on = on != 0;
					g_rs_only = alone != 0;
					g_rs_scale = a;
					g_rs_distance = b;
					strncpy_s(g_rs_effect, effect, _TRUNCATE);
					strncpy_s(g_rs_name, name, _TRUNCATE);
				}
			}
			else if (sscanf_s(line, "tone_thermal %d", &n) == 1)
			{
				thermal_set_mode(n);
			}
			else if (sscanf_s(line, "tone_value %d %f", &n, &a) == 2)
			{
				if (n >= 0 && n < TONE_FLOATS)
				{
					tone_mine.f[n] = a;
					tone_bits |= 1u << n;
				}
			}
			else if (sscanf_s(line, "tone_curve %d", &n) == 1)
			{
				tone_mine.curve = n;
				tone_bits |= TONE_SET_CURVE;
			}
			else if (sscanf_s(line, "tone_auto %d", &n) == 1)
			{
				tone_mine.automatic = n != 0;
				tone_bits |= TONE_SET_AUTO;
			}
			else if (sscanf_s(line, "tone_lock %d", &n) == 1)
			{
				tone_mine.lock = n != 0;
				tone_bits |= TONE_SET_LOCK;
			}
			else if (sscanf_s(line, "sun_color %f %f %f", &a, &b, &c) == 3)
			{
				const float color[3] = {a, b, c};
				sun_set_color(color);
			}
			else if (sscanf_s(line, "sun_brightness %f", &a) == 1)
			{
				sun_set_brightness(a);
			}
			else if (sscanf_s(line, "sun_direction %f %f", &a, &b) == 2)
			{
				sun_set_direction(a, b);
			}
			else if (strncmp(line, "sky_picture ", 12) == 0)
			{
				sky_use(line + 12);
			}
			else if (float s[6] = {}; sscanf_s(line, "sky %f %f %f %f %f %f", &s[0], &s[1], &s[2], &s[3], &s[4], &s[5]) == 6)
			{
				sky_set(s, s + 3);
			}
		}

		tone_set(tone_mine, tone_bits);
		fclose(file);
		// what was changed before in a section brought in, and is not in the preset, goes back to the game's own
		const auto originals = g_originals;
		for (const auto& original : originals)
		{
			if (!in_preset.count(original.first) && wanted(group_of_setting(original.first)))
			{
				restore(original.first);
			}
		}

		return true;
	}

	void draw_presets()
	{
		static std::string selected;
		static char name[48] = "";
		static std::string message;
		const std::vector<std::string> presets = list_presets();
		const ImGuiStyle& style = ImGui::GetStyle();
		const float load_width = ImGui::CalcTextSize("Load").x + style.FramePadding.x * 2.0f;
		const float delete_width = ImGui::CalcTextSize("Delete").x + style.FramePadding.x * 2.0f;
		const float save_width = ImGui::CalcTextSize("Save").x + style.FramePadding.x * 2.0f;
		ui_row_label("Preset");
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - load_width - delete_width - style.ItemSpacing.x * 2.0f);
		if (ImGui::BeginCombo("##preset", selected.empty() ? (presets.empty() ? "none saved" : "choose") : selected.c_str()))
		{
			for (const std::string& preset : presets)
			{
				if (ImGui::Selectable(preset.c_str(), preset == selected))
				{
					selected = preset;
				}
			}

			ImGui::EndCombo();
		}

		// which of the preset's sections to bring in (all of them unless unticked)
		static std::map<std::string, bool> bring;
		static std::string listed_for;
		static std::set<std::string> has;
		static ULONGLONG listed_at = 0;
		const std::vector<std::string> groups = all_groups();
		if (!selected.empty() && (listed_for != selected || GetTickCount64() - listed_at > 2000))
		{
			listed_for = selected;
			listed_at = GetTickCount64();
			has = preset_groups(selected);
		}

		std::set<std::string> chosen;
		for (const std::string& group : groups)
		{
			if (!bring.count(group))
			{
				bring[group] = true;
			}

			if (bring[group])
			{
				chosen.insert(group);
			}
		}

		ImGui::SameLine();
		ImGui::BeginDisabled(selected.empty() || chosen.empty());
		if (ImGui::Button("Load"))
		{
			const bool whole = chosen.size() == groups.size();
			std::string parts;
			for (const std::string& group : groups)
			{
				if (chosen.count(group) && has.count(group))
				{
					parts += (parts.empty() ? "" : ", ") + group;
				}
			}

			if (!load_preset(selected, whole ? nullptr : &chosen))
			{
				message = "That preset could not be read.";
			}
			else if (whole)
			{
				message = "Loaded " + selected + ".";
			}
			else
			{
				message = parts.empty() ? "Nothing in " + selected + " for what is ticked: those went back to the game's own." : "Loaded " + parts + " from " + selected + ".";
			}
		}

		ImGui::EndDisabled();
		ImGui::BeginDisabled(selected.empty());

		ImGui::SameLine();
		if (ImGui::Button("Delete"))
		{
			DeleteFileA((presets_folder() + "\\" + selected + ".txt").c_str());
			message = "Deleted " + selected + ".";
			selected.clear();
		}

		ImGui::EndDisabled();
		if (!selected.empty())
		{
			ui_row_label("Bring in");
			const float start = ImGui::GetCursorPosX();
			const float right = start + ImGui::GetContentRegionAvail().x;
			float x = start;
			const auto place = [&](const float width) {
				if (x > start && x + width <= right)
				{
					ImGui::SameLine(0.0f, style.ItemSpacing.x);
				}
				else if (x > start)
				{
					ImGui::SetCursorPosX(start);
					x = start;
				}

				x += width + style.ItemSpacing.x;
			};
			for (const std::string& group : groups)
			{
				// (what the preset has nothing for is shown faint: ticked, it goes back to the game's own)
				const bool there = has.count(group) != 0;
				place(ImGui::GetFrameHeight() + style.ItemInnerSpacing.x + ImGui::CalcTextSize(group.c_str()).x);
				if (!there)
				{
					ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
				}

				ImGui::Checkbox((group + "##bring").c_str(), &bring[group]);
				if (!there)
				{
					ImGui::PopStyleColor();
					if (ImGui::IsItemHovered())
					{
						ImGui::SetTooltip("This preset has nothing saved for %s. Ticked, %s goes back to how the game has it; unticked, it is left as it is now.", group.c_str(),
						                  group.c_str());
					}
				}
			}

			place(ImGui::CalcTextSize("All").x + style.FramePadding.x * 2.0f);
			if (ImGui::SmallButton("All##bring"))
			{
				for (auto& one : bring)
				{
					one.second = true;
				}
			}

			place(ImGui::CalcTextSize("None").x + style.FramePadding.x * 2.0f);
			if (ImGui::SmallButton("None##bring"))
			{
				for (auto& one : bring)
				{
					one.second = false;
				}
			}
		}

		ui_row_label("Save as");
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - save_width - style.ItemSpacing.x);
		ImGui::InputTextWithHint("##presetName", "a name", name, sizeof(name));
		std::string clean;
		for (const char* c = name; *c; ++c)
		{
			if (isalnum(static_cast<unsigned char>(*c)) || *c == ' ' || *c == '-' || *c == '_')
			{
				clean += *c;
			}
		}

		ImGui::SameLine();
		ImGui::BeginDisabled(clean.empty());
		if (ImGui::Button("Save"))
		{
			message = save_preset(clean) ? "Saved " + clean + "." : "The preset could not be written.";
			selected = clean;
		}

		ImGui::EndDisabled();
		if (!message.empty())
		{
			ImGui::TextDisabled("%s", message.c_str());
		}
	}

	// ---- every setting the game has, to look through and try
	void draw_all()
	{
		static char filter[64] = "";
		static std::vector<int> matches;
		static std::string filtered_for = "\x01";
		ImGui::SetNextItemWidth(-1.0f);
		ImGui::InputTextWithHint("##filter", ICON_SEARCH "  part of a setting's name, e.g. sun", filter, sizeof(filter));
		const int count = theater_dvar_count();
		if (filtered_for != filter)
		{
			filtered_for = filter;
			matches.clear();
			std::string wanted = filter;
			for (char& c : wanted)
			{
				c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
			}

			for (int i = 0; i < count && wanted.size() >= 2; ++i)
			{
				const auto* at = static_cast<const unsigned char*>(theater_dvar_at(i));
				const char* name = at ? *reinterpret_cast<const char* const*>(at) : nullptr;
				if (!name)
				{
					continue;
				}

				std::string lower = name;
				for (char& c : lower)
				{
					c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
				}

				if (lower.find(wanted) != std::string::npos)
				{
					matches.push_back(i);
				}
			}
		}

		if (strlen(filter) < 2)
		{
			ImGui::PushTextWrapPos(0.0f);
			ImGui::TextDisabled("%d settings. Type two letters or more to list those with them in the name. What is changed here is put back "
			                    "when playback ends.",
			                    count);
			ImGui::PopTextWrapPos();
			return;
		}

		ImGui::TextDisabled("%d found", static_cast<int>(matches.size()));
		ImGui::BeginChild("##all", {0.0f, ui_font_size() * 22.0f});
		ImGuiListClipper clipper;
		clipper.Begin(static_cast<int>(matches.size()), ImGui::GetFrameHeightWithSpacing());
		while (clipper.Step())
		{
			for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
			{
				const Dvar dvar{static_cast<const unsigned char*>(theater_dvar_at(matches[row]))};
				if (!dvar)
				{
					continue;
				}

				ImGui::AlignTextToFramePadding();
				ImGui::TextUnformatted(dvar.name());
				if (ImGui::IsItemHovered())
				{
					ImGui::SetTooltip("%s", dvar.name());
				}

				ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.55f);
				control(dvar);
			}
		}

		ImGui::EndChild();
	}
}

void look_toggle_hud()
{
	if (const Dvar dvar = find("cg_draw2D"))
	{
		set(dvar, "%d", dvar.current()[0] ? 0 : 1);
	}
}

namespace
{
	void draw_focus();
	void draw_reshade_focus();

	// The Exposure section: the map's own values shown, each changed by dragging; what was changed is the
	// editor's from then on (exposure.cpp), until its reset button puts the map's back.
	void draw_tone(const bool reset)
	{
		struct Field
		{
			int index;
			const char* label;
			const char* tip;
			float speed, low, high;
			const char* format;
		};
		static const Field FIELDS[] = {
			{3, "Exposure shift", "Shifts the whole exposure, in stops. This is the one to darken or brighten a scene with: what is far brighter\nthan the scene (a muzzle flash, an explosion, a lamp) stays bright, as it would for a camera.", 0.02f, -12.0f, 12.0f, "%.2f"},
			{0, "Automatic: shift", "The same, added to the exposure the game finds by itself.", 0.02f, -12.0f, 12.0f, "%.2f"},
			{16, "Automatic: least shift", "How far the automatic exposure may go one way.", 0.02f, -12.0f, 12.0f, "%.2f"},
			{17, "Automatic: most shift", "How far the automatic exposure may go the other way.", 0.02f, -12.0f, 12.0f, "%.2f"},
			{4, "Automatic: how fast it follows", "How quickly the automatic exposure follows a change of light (walking out of a dark room).", 0.001f, 0.0f, 1.0f, "%.3f"},
			{2, "Exposure when not automatic", "The exposure used with \"Automatic exposure\" off.", 0.05f, -20.0f, 30.0f, "%.2f"},
			{1, "Most exposure", "The limit of the exposure.", 0.05f, -20.0f, 30.0f, "%.2f"},
			{6, "White point", "How bright a thing has to be to come out pure white. Higher: more of the brightest things keep their colour and shape.", 0.5f, 0.01f, 5000.0f, "%.2f"},
			{8, "Highlights roll off", "How gently the brightest parts run up to white (the curve's shoulder).", 0.002f, 0.0f, 4.0f, "%.3f"},
			{7, "Crossover", "Where the curve passes from its shadows' part to its highlights' part.", 0.01f, 0.0f, 100.0f, "%.2f"},
			{9, "Shadows roll off", "How gently the darkest parts run down to black (the curve's toe).", 0.002f, 0.0f, 4.0f, "%.3f"},
			{5, "Black point", "How dark a thing has to be to come out pure black.", 0.0005f, 0.0f, 1.0f, "%.4f"},
			{10, "Dark scene: level", "The automatic exposure's own curve: how dark a scene counts as dark...", 0.05f, -20.0f, 30.0f, "%.2f"},
			{13, "Dark scene: shift", "...and how much the exposure is shifted in such a scene.", 0.02f, -12.0f, 12.0f, "%.2f"},
			{11, "Middle scene: level", "The same for a scene of middling brightness.", 0.05f, -20.0f, 30.0f, "%.2f"},
			{14, "Middle scene: shift", "The same for a scene of middling brightness.", 0.02f, -12.0f, 12.0f, "%.2f"},
			{12, "Bright scene: level", "The same for a bright scene.", 0.05f, -20.0f, 30.0f, "%.2f"},
			{15, "Bright scene: shift", "The same for a bright scene.", 0.02f, -12.0f, 12.0f, "%.2f"},
		};
		ToneState map, mine;
		std::uint32_t bits = 0;
		const bool hooked = tone_get(&map, &mine, &bits);
		const std::uint32_t before = bits;
		bool changed = false;
		if (reset)
		{
			bits = 0;
		}

		if (!hooked)
		{
			ImGui::TextDisabled("Not available: the game's tone mapping could not be reached (see the log).");
			return;
		}

		if (!map.seen)
		{
			ImGui::TextDisabled("The values show once a recording is playing.");
			return;
		}

		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextDisabled("These are the map's own values. Change one and it is yours from then on (marked *); its reset button gives the map's back. They act at once, paused or playing.");
		ImGui::PopTextWrapPos();
		{
			bool automatic = (bits & TONE_SET_AUTO) ? mine.automatic : map.automatic;
			ui_row_label((bits & TONE_SET_AUTO) ? "Automatic exposure *" : "Automatic exposure");
			if (ImGui::Checkbox("##toneAuto", &automatic))
			{
				mine.automatic = automatic;
				bits = automatic == map.automatic ? bits & ~TONE_SET_AUTO : bits | TONE_SET_AUTO;
				changed = true;
			}

			ImGui::SetItemTooltip("The game finds the exposure by itself from how bright the picture is, as a camera does, and follows changes of light.\nOff: the exposure is the one below, and stays put.");
			bool lock = (bits & TONE_SET_LOCK) ? mine.lock : map.lock;
			ui_row_label((bits & TONE_SET_LOCK) ? "Automatic: held where it is *" : "Automatic: held where it is");
			if (ImGui::Checkbox("##toneLock", &lock))
			{
				mine.lock = lock;
				bits = lock == map.lock ? bits & ~TONE_SET_LOCK : bits | TONE_SET_LOCK;
				changed = true;
			}

			ImGui::SetItemTooltip("The automatic exposure stops following the light and keeps what it has now.");
		}

		for (const Field& field : FIELDS)
		{
			const std::uint32_t bit = 1u << field.index;
			float value = (bits & bit) ? mine.f[field.index] : map.f[field.index];
			char label[80];
			snprintf(label, sizeof(label), "%s%s", field.label, (bits & bit) ? " *" : "");
			ui_row_label(label);
			char id[32];
			snprintf(id, sizeof(id), "##tone%d", field.index);
			ui::Default(map.f[field.index]);
			if (ui::DragFloat(id, &value, field.speed, field.low, field.high, field.format))
			{
				if (value == map.f[field.index])
				{
					// (its reset button: the map's own again)
					bits &= ~bit;
				}
				else
				{
					mine.f[field.index] = value;
					bits |= bit;
				}

				changed = true;
			}

			ImGui::SetItemTooltip("%s", field.tip);
		}

		{
			int curve = (bits & TONE_SET_CURVE) ? mine.curve : map.curve;
			ui_row_label((bits & TONE_SET_CURVE) ? "Curve's kind *" : "Curve's kind");
			if (ImGui::SliderInt("##toneCurve", &curve, 0, 3))
			{
				if (curve == map.curve)
				{
					bits &= ~TONE_SET_CURVE;
				}
				else
				{
					mine.curve = curve;
					bits |= TONE_SET_CURVE;
				}

				changed = true;
			}

			ImGui::SetItemTooltip("Which of the game's curves brings the light down to the screen's range. The map's own is %d.", map.curve);
		}

		if (changed || bits != before)
		{
			tone_set(mine, bits);
		}
	}
}

void look_draw_tab(const TheaterState& state)
{
	(void)state;
	bool reset = false;
	ImGui::TextDisabled("Right-click any setting to reset it to default.");
	if (ui_section(ICON_SLIDERS, "Presets"))
	{
		draw_presets();
	}

	if (ui_section(ICON_BURGER, "HUD", &reset))
	{
		draw_hud(reset);
	}

	for (const Section& section : SECTIONS)
	{
		// (the Exposure section comes after the Film's)
		if (strcmp(section.label, "Models") == 0)
		{
			reset = false;
			if (ui_section(ICON_SLIDERS, "Thermal vision", &reset, false))
			{
				static const char* const MODES[] = {"As in the match, in first person only", "Never", "Always (any camera)", "Left to the game (as before 0.88)"};
				int mode = thermal_mode();
				ui_row_label("Thermal picture");
				if (ImGui::Combo("##thermal", &mode, MODES, 4))
				{
					thermal_set_mode(mode);
				}

				ImGui::SetItemTooltip("The white-hot picture of a thermal scope. As in the match: when the recorded player looks down a thermal scope,\nand only while you watch through his eyes (not in the free camera or any other). Always: on in every camera.");
			}

			if (reset)
			{
				thermal_set_mode(0);
			}

			reset = false;
			const bool tone_open = ui_section(ICON_SLIDERS, "Exposure", &reset, false);
			if (tone_open)
			{
				draw_tone(reset);
			}
			else if (reset)
			{
				tone_set(ToneState(), 0);
			}
		}

		// (the Sky section comes after the Sun's)
		if (strcmp(section.label, "Lighting") == 0)
		{
			reset = false;
			const bool sky_open = ui_section(ICON_CLOUD, "Sky", &reset, false);
			if (sky_open)
			{
				draw_sky(reset);
			}
			else if (reset)
			{
				for (const char* track : {"sky_hue", "sky_saturation", "sky_brightness", "sky_turn"})
				{
					remove_track(track);
				}

				sky_reset();
			}
		}

		bool any = false;
		for (const Entry& entry : section.entries)
		{
			any = any || find(entry.name);
		}

		if (!any)
		{
			continue;
		}

		reset = false;
		const bool open = ui_section(section.icon, section.label, &reset, section.open);
		if (open && strstr(section.label, "epth of"))
		{
			draw_focus();
		}

		if (reset)
		{
			for (const Entry& entry : section.entries)
			{
				remove_track(entry.name);
				restore(entry.name);
			}
		}

		const bool sun = strcmp(section.label, "Sun") == 0;
		if (sun && (open || reset))
		{
			if (reset || open)
			{
				if (open)
				{
					draw_sun_light(reset);
				}
				else
				{
					remove_track("sun_color");
					remove_track("sun_brightness");
					remove_track("sun_direction");
					sun_reset();
				}
			}
		}

		if (!open)
		{
			continue;
		}

		const char* shown = nullptr;
		for (const Entry& entry : section.entries)
		{
			const Dvar dvar = find(entry.name);
			// (the same label twice is one setting under two names; the first the game has is shown)
			if (!dvar || (shown && strcmp(shown, entry.label) == 0))
			{
				continue;
			}

			shown = entry.label;
			ui_row_label(entry.label);
			control(dvar, entry.label);
			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip("%s", entry.name);
			}
		}

		// (the fog's own colour and thickness are not settings: they are the game's fog values, changed in place)
		if (strcmp(section.label, "Fog") == 0)
		{
			passes_draw_fog();
		}
	}

	if (ui_section(ICON_SLIDERS, "Surfaces", nullptr, false))
	{
		StormState scene = storm_state();
		ui_row_label("Bump strength");
		ui::Default(1.0f);
		if (ui::SliderFloat("##bump", &scene.bump, 0.0f, 4.0f, "%.2fx"))
		{
			storm_set(scene);
		}

		ImGui::SetItemTooltip("How strongly the fine relief of surfaces stands out (their normal maps): the grain of a floor, the joints of a wall,\nthe weave of cloth. 1: as the game has it. 0: smooth. Above 1: deeper, which shows most under a low sun, a light or rain.\nIt takes a second or two to take hold after a change, and uses more of the graphics card's memory while it is not 1.");
		if (scene.bump != 1.0f)
		{
			int found = 0, made = 0, megabytes = 0;
			surfaces_status(&found, &made, &megabytes);
			ImGui::TextDisabled("%d of %d surfaces' maps done (%d MB)", made, found, megabytes);
		}
	}

	if (ui_section(ICON_SEARCH, "All settings", nullptr, false))
	{
		draw_all();
	}
}

float look_key_width()
{
	return key_button_width();
}

void look_key_button(const char* id, const char* label, const int count, const float* values)
{
	key_button(id, label, count, values);
}

void look_key_changed(const char* id, const float* values)
{
	key_changed(id, values);
}

// A keyframe of the setting at the time the recording is at (the track made if there is none).
void look_key_add(const char* id, const char* label, const int count, const float* values)
{
	put_key(id, label, count, values);
}

void look_key_remove(const char* id)
{
	remove_track(id);
}

int look_track_count()
{
	return static_cast<int>(g_tracks.size());
}

bool look_track(const int index, LookTrack* out)
{
	if (index < 0 || index >= static_cast<int>(g_tracks.size()))
	{
		return false;
	}

	const Track& track = g_tracks[static_cast<size_t>(index)];
	out->label = track.label.c_str();
	out->keys = static_cast<int>(track.keys.size() > 256 ? 256 : track.keys.size());
	for (int i = 0; i < out->keys; ++i)
	{
		out->ms[i] = track.keys[static_cast<size_t>(i)].ms;
	}

	return true;
}

void look_track_clear(const int index)
{
	if (index >= 0 && index < static_cast<int>(g_tracks.size()))
	{
		g_tracks.erase(g_tracks.begin() + index);
	}
}

void look_track_remove_key(const int index, const int key)
{
	if (index >= 0 && index < static_cast<int>(g_tracks.size()))
	{
		Track& track = g_tracks[static_cast<size_t>(index)];
		if (key >= 0 && key < static_cast<int>(track.keys.size()))
		{
			track.keys.erase(track.keys.begin() + key);
			if (track.keys.empty())
			{
				g_tracks.erase(g_tracks.begin() + index);
			}
		}
	}
}

namespace
{
	// The depth of field's sharp distance kept on a player or an actor: 0 off, 1 a player (its entity), 2 an actor
	// or prop (its handle). How much around it stays sharp: a part of the distance.
	int g_focus_kind = 0, g_focus_id = 0;
	float g_focus_range = 0.35f, g_focus_sent = -1.0f, g_focus_range_sent = -1.0f;

	void focus_frame(const TheaterState& state)
	{
		g_rs_pinned = -1.0f;
		if (!state.playing || g_focus_kind == 0)
		{
			g_focus_sent = -1.0f;
			return;
		}

		float place[3] = {}, eye[3] = {};
		if (!(g_focus_kind == 2 ? actors_origin_of(g_focus_id, place) : actors_entity_origin(g_focus_id, place)))
		{
			return;
		}

		place[2] += 48.0f;
		camera_view_origin(eye);
		const float far_off = std::sqrt((place[0] - eye[0]) * (place[0] - eye[0]) + (place[1] - eye[1]) * (place[1] - eye[1]) + (place[2] - eye[2]) * (place[2] - eye[2]));
		g_rs_pinned = far_off;
		{
			// (0.89, asked for: the game's own depth of field is kept off for good, see look_frame; a pinned
			// focus only gives its distance, for ReShade's effect)
			return;
		}

		// (only when it has changed enough to see: each setting is a command to the game)
		if (g_focus_sent > 0.0f && std::fabs(far_off - g_focus_sent) < g_focus_sent * 0.01f + 0.5f && g_focus_range == g_focus_range_sent)
		{
			return;
		}

		g_focus_sent = far_off;
		g_focus_range_sent = g_focus_range;
		const float near_end = far_off * (1.0f - g_focus_range), far_start = far_off * (1.0f + g_focus_range);
		const struct
		{
			const char* name;
			float value;
		} wanted[] = {{"r_dof_enable", 1.0f}, {"r_dof_tweak", 1.0f}, {"r_dof_nearStart", near_end * 0.4f}, {"r_dof_nearEnd", near_end},
		              {"r_dof_farStart", far_start}, {"r_dof_farEnd", far_start * 2.5f + 100.0f}};
		for (const auto& one : wanted)
		{
			if (const Dvar dvar = find(one.name))
			{
				set(dvar, "%g", one.value);
			}
		}
	}

	void draw_reshade_focus()
	{
		ImGui::Spacing();
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextDisabled("The game's own depth of field is kept switched off (r_dof_enable 0). The focus above only gives its distance to ReShade's effect, below.");
		ImGui::PopTextWrapPos();
		ImGui::SeparatorText("Focus in ReShade");
		const int status = reshade_link_status();
		if (status < 2)
		{
			ImGui::PushTextWrapPos(0.0f);
			ImGui::TextDisabled(status == 0 ? "No ReShade found in the game."
			                                : "ReShade is there, but it would not let the editor in (its reason, if it gave one, is in ReShade.log in the game's folder).");
			ImGui::PopTextWrapPos();
			return;
		}

		static ReshadeSetting settings[2000];
		static int count = 0;
		static char filter[48] = "focus";
		static bool asked = false;
		if (!asked)
		{
			asked = true;
			reshade_link_list_request();
		}

		count = reshade_link_list(settings, 2000);
		char shown[160];
		snprintf(shown, sizeof(shown), "%s", g_rs_name[0] ? g_rs_name : "Choose...");
		ui_row_label("ReShade setting");
		if (ImGui::BeginCombo("##rsSetting", shown, ImGuiComboFlags_HeightLarge))
		{
			ImGui::SetNextItemWidth(-90.0f);
			ImGui::InputTextWithHint("##rsFilter", "part of its name (empty: all)", filter, sizeof(filter));
			ImGui::SameLine();
			if (ImGui::Button("Read again"))
			{
				reshade_link_list_request();
			}

			for (int i = 0; i < count; ++i)
			{
				const ReshadeSetting& one = settings[i];
				char text[220];
				snprintf(text, sizeof(text), "%s  (%s, %s) = %g", one.label, one.effect, one.name, one.value);
				bool fits = !filter[0];
				for (const char* at = text; !fits && *at; ++at)
				{
					fits = _strnicmp(at, filter, strlen(filter)) == 0;
				}

				if (fits && ImGui::Selectable(text, strcmp(one.effect, g_rs_effect) == 0 && strcmp(one.name, g_rs_name) == 0))
				{
					strncpy_s(g_rs_effect, one.effect, _TRUNCATE);
					strncpy_s(g_rs_name, one.name, _TRUNCATE);
				}
			}

			if (count == 0)
			{
				ImGui::TextDisabled("No settings read yet: turn an effect on in ReShade, then \"Read again\".");
			}

			ImGui::EndCombo();
		}

		ImGui::SetItemTooltip("The setting of your depth of field effect that says how far off the sharp plane is: its manual focus\n(\"Manual focus\", \"Focus plane\", \"Focus depth\"...). Turn the effect's autofocus off in ReShade so that it uses it.\nEvery number setting of every effect that is on is listed; type part of a name to narrow the list.");
		if (!g_rs_name[0])
		{
			return;
		}

		float current = 0.0f;
		bool missing = false;
		const bool have = reshade_link_current(&current, &missing);
		if (missing)
		{
			ImGui::TextDisabled("ReShade has no such setting now (is the effect still on?).");
		}

		const float distance = g_rs_pinned > 0.0f ? g_rs_pinned : g_rs_distance;
		ui_row_label("Send the focus to it");
		ImGui::Checkbox("##rsOn", &g_rs_on);
		ImGui::SetItemTooltip("Every picture the editor writes the focus distance into that setting: the pinned player's or actor's above\nif one is pinned, else the distance below (which can have keyframes: a rack focus).");
		ui_row_label("Leave the game's own blur alone");
		ImGui::Checkbox("##rsOnly", &g_rs_only);
		ImGui::SetItemTooltip("On: a pinned focus only drives ReShade. Off: it drives the game's depth of field as well.");
		if (g_rs_pinned <= 0.0f)
		{
			ui_row_label("Focus distance");
			ui::Default(500.0f);
			if (ui::DragFloat("##rsDistance", &g_rs_distance, 2.0f, 1.0f, 100000.0f, "%.0f units"))
			{
				key_changed("reshade_focus", &g_rs_distance);
			}

			ImGui::SetItemTooltip("How far from the camera the sharp plane is, in the game's units (a player is about 70 tall).\nThe diamond puts a keyframe: two keyframes with different distances are a rack focus.");
			key_button("reshade_focus", "ReShade focus distance", 1, &g_rs_distance);
		}
		else
		{
			ImGui::TextDisabled("The pinned one is %.0f units away.", g_rs_pinned);
		}

		float per_hundred = g_rs_scale * 100.0f;
		ui_row_label("ReShade's number per 100 units");
		if (ImGui::DragFloat("##rsScale", &per_hundred, per_hundred * 0.01f + 0.00001f, 0.000001f, 100000.0f, "%.5f"))
		{
			g_rs_scale = per_hundred / 100.0f;
		}

		ImGui::SetItemTooltip("How the game's distance becomes the effect's own number (each effect counts distance its own way).\n\"Match\" below works it out for you.");
		ImGui::BeginDisabled(g_rs_on || !have || distance <= 0.0f);
		if (ImGui::Button("Match: ReShade is sharp at this distance now"))
		{
			g_rs_scale = current / distance;
		}

		ImGui::EndDisabled();
		ImGui::SetItemTooltip("To set it up, once per effect:\n1. Leave \"Send the focus to it\" off.\n2. Pin a player or actor above (or set the distance to something you can see).\n3. In ReShade, move the effect's own focus until that player or thing is sharp.\n4. Press this. From then on the editor's distance drives the effect.");
		ImGui::TextDisabled(g_rs_on ? "Sending %.5f (%.0f units)." : "ReShade has %.5f there now; the editor would send %.5f.", g_rs_on ? distance * g_rs_scale : current,
		                    g_rs_on ? distance : distance * g_rs_scale);
	}

	void draw_focus()
	{
		static Actor actors[ACTORS_MAX];
		const int actor_count = actors_list(actors, ACTORS_MAX);
		const BoneState players = camera_bone_state();
		char label[64] = "Nothing (set by hand below)";
		if (g_focus_kind == 1)
		{
			snprintf(label, sizeof(label), "Player %d", g_focus_id);
		}

		for (int a = 0; a < actor_count && g_focus_kind == 2; ++a)
		{
			if (actors[a].handle == g_focus_id)
			{
				snprintf(label, sizeof(label), "%s", actors[a].name);
			}
		}

		ui_row_label("Keep in focus");
		if (ImGui::BeginCombo("##focusOn", label))
		{
			if (ImGui::Selectable("Nothing (set by hand below)", g_focus_kind == 0))
			{
				g_focus_kind = 0;
			}

			for (int entity = 0; entity < 64; ++entity)
			{
				if (players.entities & (1ull << entity))
				{
					snprintf(label, sizeof(label), entity == players.nearest ? "Player %d (nearest the camera)" : "Player %d", entity);
					if (ImGui::Selectable(label, g_focus_kind == 1 && g_focus_id == entity))
					{
						g_focus_kind = 1;
						g_focus_id = entity;
					}
				}
			}

			for (int a = 0; a < actor_count; ++a)
			{
				ImGui::PushID(7000 + a);
				if (ImGui::Selectable(actors[a].name, g_focus_kind == 2 && g_focus_id == actors[a].handle))
				{
					g_focus_kind = 2;
					g_focus_id = actors[a].handle;
				}

				ImGui::PopID();
			}

			ImGui::EndCombo();
		}

		ImGui::SetItemTooltip("The depth of field's sharp distance follows this player or actor as it and the camera move: a focus pull that\nmakes itself. It turns the depth of field on and sets the near and far distances below as it goes.");
		if (g_focus_kind != 0)
		{
			float range = g_focus_range * 100.0f;
			ui_row_label("Sharp around it");
			ui::Default(35.0f);
			if (ui::SliderFloat("##focusRange", &range, 3.0f, 90.0f, "%.0f%% of the distance"))
			{
				g_focus_range = range / 100.0f;
			}

			ImGui::SetItemTooltip("How deep the sharp zone is around it. Small: only it is sharp, like a long lens wide open.");
		}

		draw_reshade_focus();
	}

	// While a recording is paused the game's sound is turned down to nothing (sounds that were playing went on,
	// over and over), and put back when it plays again or stops. What it was is also kept in a file until it is
	// put back, so that a game closed while paused does not start silent the next time.
	void sound_follows_pause(const TheaterState& state)
	{
		static bool looked = false, silent = false;
		static std::string was;
		const std::string keep = std::string(theater_folder()) + "\\volume_to_put_back.txt";
		const Dvar volume = find("snd_volume");
		if (!volume)
		{
			return;
		}

		if (!looked)
		{
			looked = true;
			FILE* file = nullptr;
			char text[64] = "";
			if (fopen_s(&file, keep.c_str(), "r") == 0 && file)
			{
				if (fgets(text, sizeof(text), file) && atof(text) > 0.0)
				{
					theater_command((std::string("snd_volume ") + text).c_str());
				}

				fclose(file);
				DeleteFileA(keep.c_str());
			}
		}

		const bool quiet = state.playing && state.paused;
		if (quiet && !silent)
		{
			was = text_of(volume, volume.current());
			if (atof(was.c_str()) <= 0.0)
			{
				return;
			}

			FILE* file = nullptr;
			if (fopen_s(&file, keep.c_str(), "w") == 0 && file)
			{
				fputs(was.c_str(), file);
				fclose(file);
			}

			theater_command("snd_volume 0");
			silent = true;
		}
		else if (!quiet && silent)
		{
			theater_command(("snd_volume " + was).c_str());
			DeleteFileA(keep.c_str());
			silent = false;
		}
	}
}

// The look kept beside a recording (scene.cpp): written, and read back.
bool look_save_file(const char* path)
{
	return save_preset(path);
}

bool look_load_file(const char* path)
{
	return load_preset(path, nullptr);
}

void look_frame(const TheaterState& state)
{
	focus_frame(state);
	sound_follows_pause(state);
	reshade_link_frame();
	{
		// (0.89, asked for) The game's own depth of field stays off: r_dof_enable is put back to 0 whenever
		// anything has turned it on (looked at twice a second).
		static ULONGLONG looked_at = 0;
		if (GetTickCount64() - looked_at > 500)
		{
			looked_at = GetTickCount64();
			if (const Dvar dof = find("r_dof_enable"))
			{
				if (atof(text_of(dof, dof.current()).c_str()) != 0.0)
				{
					theater_command("r_dof_enable 0");
				}
			}
		}
	}
	// the settings with keyframes: what the keyframes make of this frame's time
	g_now_ms = state.position_ms;
	if (state.playing)
	{
		for (Track& track : g_tracks)
		{
			float v[4] = {};
			sample(track, state.position_exact, v);
			if (track.id == "sun_color")
			{
				for (float& part : v)
				{
					part = part < 0.0f ? 0.0f : part;
				}

				sun_set_color(v);
			}
			else if (track.id == "reshade_focus")
			{
				g_rs_distance = v[0] < 1.0f ? 1.0f : v[0];
			}
			else if (track.id == "sun_brightness")
			{
				sun_set_brightness(v[0] < 0.0f ? 0.0f : v[0]);
			}
			else if (track.id == "sun_direction")
			{
				sun_set_direction(v[0], v[1]);
			}
			else if (track.id == "bone_offset" || track.id == "bone_turn")
			{
				// the bone camera's place and turn on its bone: only as the recording's time moves, so that
				// with time standing still the camera can be flown to where the next point is wanted
				static double applied[2] = {-1.0, -1.0};
				const int which = track.id == "bone_offset" ? 0 : 1;
				if (applied[which] != static_cast<double>(state.position_exact))
				{
					applied[which] = static_cast<double>(state.position_exact);
					BoneCamera bone = camera_bone_state().camera;
					memcpy(which == 0 ? bone.offset : bone.turn, v, sizeof(bone.offset));
					camera_set_bone(bone);
				}
			}
			else if (track.id == "camera_fov")
			{
				static double applied = -1.0;
				if (applied != static_cast<double>(state.position_exact))
				{
					applied = static_cast<double>(state.position_exact);
					camera_set_fov(v[0] < 5.0f ? 5.0f : v[0] > 160.0f ? 160.0f : v[0]);
				}
			}
			else if (track.id.compare(0, 5, "light") == 0)
			{
				// light<N>_color, _brightness (in doublings), _reach, _position
				SceneLight lights[16];
				const int count = scene_lights(lights, 16);
				const int index = atoi(track.id.c_str() + 5);
				const size_t under = track.id.find('_');
				if (index >= 0 && index < count && under != std::string::npos)
				{
					SceneLight light = lights[index];
					const std::string what = track.id.substr(under + 1);
					if (what == "color")
					{
						for (int i = 0; i < 3; ++i)
						{
							light.color[i] = v[i] < 0.0f ? 0.0f : v[i];
						}
					}
					else if (what == "brightness")
					{
						light.intensity = std::pow(2.0f, v[0]);
					}
					else if (what == "reach")
					{
						light.radius = v[0] < 1.0f ? 1.0f : v[0];
					}
					else
					{
						memcpy(light.origin, v, sizeof(light.origin));
					}

					scene_set_light(index, light);
				}
			}
			else if (track.id.compare(0, 4, "sky_") == 0)
			{
				SkyState sky = sky_state();
				if (track.id == "sky_hue")
				{
					sky.hsv[0] = v[0];
				}
				else if (track.id == "sky_saturation")
				{
					sky.hsv[1] = v[0] < 0.0f ? 0.0f : v[0];
				}
				else if (track.id == "sky_brightness")
				{
					sky.hsv[2] = v[0] < 0.0f ? 0.0f : v[0];
				}
				else
				{
					memcpy(sky.turn, v, sizeof(sky.turn));
				}

				sky_set(sky.hsv, sky.turn);
			}
			else if (const Dvar dvar = find(track.id.c_str()))
			{
				char text[128] = {};
				switch (dvar.type())
				{
				case 1: snprintf(text, sizeof(text), "%g", v[0]); break;
				case 2: snprintf(text, sizeof(text), "%g %g", v[0], v[1]); break;
				case 3:
				case 9: snprintf(text, sizeof(text), "%g %g %g", v[0], v[1], v[2]); break;
				case 4:
				case 8: snprintf(text, sizeof(text), "%g %g %g %g", v[0], v[1], v[2], v[3]); break;
				case 5: snprintf(text, sizeof(text), "%d", static_cast<int>(std::lround(v[0]))); break;
				default: break;
				}

				if (text[0] && track.sent != text)
				{
					track.sent = text;
					set(dvar, std::string(text));
				}
			}
		}
	}
	else
	{
		for (Track& track : g_tracks)
		{
			track.sent.clear();
		}
	}

	// playback over: the game's own settings again
	if (!state.playing && !state.starting)
	{
		if (!g_originals.empty())
		{
			const auto originals = g_originals;
			for (const auto& original : originals)
			{
				restore(original.first);
			}
		}

		sun_reset();
		sky_reset();
		if (g_hud_touched)
		{
			g_hud_touched = false;
			g_hud_hidden.clear();
			theater_command("set s1iris_hud_hide \"\"");
		}
	}

	// the focus for ReShade: the pinned one's distance, else the one set (or keyframed)
	reshade_link_send(g_rs_on && state.playing, g_rs_effect, g_rs_name, (g_rs_pinned > 0.0f ? g_rs_pinned : g_rs_distance) * g_rs_scale);
}
