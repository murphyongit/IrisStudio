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
		ImGui::SetNextItemWidth(keyable ? -key_button_width() : -1.0f);
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
		{ICON_CLOUD, "Fog", false, {{"Fog", "r_fog"}}},
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
		// How finely models are drawn. The game smooths some models by subdividing them (r_subdiv,
		// r_adaptiveSubdiv) and swaps to coarser ones with distance (the rest).
		{ICON_PERSON, "Models", false,
		 {{"Detail level", "r_forceLod"},
		  {"Detail kept longer, characters", "r_lodBiasSkinned"},
		  {"Detail kept longer, objects", "r_lodBiasRigid"},
		  {"Detail falloff, characters", "r_lodScaleSkinned"},
		  {"Detail falloff, objects", "r_lodScaleRigid"},
		  {"Smoothing (subdivision)", "r_subdiv"},
		  {"Smoothing, which models", "r_adaptiveSubdiv"},
		  {"Smoothing steps", "r_subdivLimit"},
		  {"Smoothing patches", "r_subdivPatchCount"},
		  {"Smoothing by distance", "r_adaptiveSubdivBaseFactor"},
		  {"Surface relief", "r_tessellation"},
		  {"Surface relief amount", "r_tessellationFactor"},
		  {"Surface relief distance", "r_tessellationCutoffDistance"}}},
		{ICON_WAND, "Effects", false,
		 {{"Effects", "fx_enable"},
		  {"Draw effects", "fx_draw"},
		  {"Lens flares", "fx_flare"},
		  {"Bullet marks", "fx_marks"},
		  {"Post processing", "r_postfx_enable"},
		  {"Post processing", "r_postFX"},
		  {"Distortion", "r_distortion"},
		  {"Motion blur", "r_mbEnable"},
		  {"Motion blur", "r_mbFastEnable"},
		  {"Motion blur quality", "r_mbFastPreset"},
		  {"Motion blur, the world", "r_mbDebugStaticVelocityScalar"},
		  {"Motion blur, models", "r_mbDebugModelVelocityScalar"},
		  {"Motion blur, the gun", "r_mbDebugViewModelVelocityScalar"},
		  {"Blood", "cg_blood"},
		  {"Shell casings", "cg_brass"},
		  {"Field of view (first person)", "cg_fov"},
		  {"Field of view scale", "cg_fovScale"},
		  {"Gun forward", "cg_gun_x"},
		  {"Gun right", "cg_gun_y"},
		  {"Gun up", "cg_gun_z"}}},
	};

	const Entry HUD_SETTINGS[] = {{"2D Elements (F3)", "cg_draw2D"}, {"Gun", "cg_drawGun"}, {"Names over players", "cg_drawFriendlyNames"},
	                              {"Killfeed", "ui_hud_obituaries"}};

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

		bool changed = false;
		ui_row_label("Hue");
		ImGui::SetNextItemWidth(-key_button_width());
		if (ImGui::SliderFloat("##skyHue", &sky.hsv[0], -180.0f, 180.0f, "%.0f deg"))
		{
			changed = true;
			key_changed("sky_hue", &sky.hsv[0]);
		}

		key_button("sky_hue", "Sky hue", 1, &sky.hsv[0]);
		ui_row_label("Saturation");
		ImGui::SetNextItemWidth(-key_button_width());
		if (ImGui::SliderFloat("##skySaturation", &sky.hsv[1], 0.0f, 3.0f, "%.2f"))
		{
			changed = true;
			key_changed("sky_saturation", &sky.hsv[1]);
		}

		key_button("sky_saturation", "Sky saturation", 1, &sky.hsv[1]);
		ui_row_label("Brightness");
		ImGui::SetNextItemWidth(-key_button_width());
		if (ImGui::SliderFloat("##skyValue", &sky.hsv[2], 0.0f, 4.0f, "%.2f"))
		{
			changed = true;
			key_changed("sky_brightness", &sky.hsv[2]);
		}

		key_button("sky_brightness", "Sky brightness", 1, &sky.hsv[2]);
		ui_row_label("Rotation");
		ImGui::SetNextItemWidth(-key_button_width());
		if (ImGui::DragFloat3("##skyTurn", sky.turn, 0.5f, -720.0f, 720.0f, "%.1f"))
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

		const int count = atoi(dvar_string("s1iris_hud_count").c_str());
		if (count <= 0)
		{
			ImGui::PushTextWrapPos(0.0f);
			ImGui::TextDisabled("The HUD's separate parts are listed here by a small script that loads when the game starts "
			                    "(s1x\\ui_scripts\\s1iris). None are listed yet: restart the game if this was just installed.");
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
		static const struct { const char* label; const char* part; } KNOWN[] = {{"Minimap", "wrapper_for_minimapHud"}};
		for (const auto& known : KNOWN)
		{
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
				ImGui::Indent(part.depth * ui_font_size() * 0.8f + 0.01f);
				ui_row_label(dot == std::string::npos ? part.name.c_str() : part.name.c_str() + dot + 1);
				if (ImGui::Checkbox("##shown", &shown))
				{
					toggle(part.name, shown);
				}

				ImGui::Unindent(part.depth * ui_font_size() * 0.8f + 0.01f);
				ImGui::PopID();
			}

			ImGui::TreePop();
		}
	}

	// ---- presets: what the Look tab was set to, kept by name in theater\presets and put back on at a click.
	// A preset holds every setting the editor has changed and the sun's light where that was set.
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

	bool save_preset(const std::string& name)
	{
		FILE* file = nullptr;
		if (fopen_s(&file, (presets_folder() + "\\" + name + ".txt").c_str(), "w") != 0 || !file)
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

		const SkyState sky = sky_state();
		if (sky.changed)
		{
			fprintf(file, "sky %g %g %g %g %g %g\n", sky.hsv[0], sky.hsv[1], sky.hsv[2], sky.turn[0], sky.turn[1], sky.turn[2]);
		}

		fclose(file);
		return true;
	}

	bool load_preset(const std::string& name)
	{
		FILE* file = nullptr;
		if (fopen_s(&file, (presets_folder() + "\\" + name + ".txt").c_str(), "r") != 0 || !file)
		{
			return false;
		}

		std::set<std::string> in_preset;
		sun_reset();
		sky_reset();
		char line[512];
		while (fgets(line, sizeof(line), file))
		{
			line[strcspn(line, "\r\n")] = 0;
			float a = 0.0f, b = 0.0f, c = 0.0f;
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
			else if (float s[6] = {}; sscanf_s(line, "sky %f %f %f %f %f %f", &s[0], &s[1], &s[2], &s[3], &s[4], &s[5]) == 6)
			{
				sky_set(s, s + 3);
			}
		}

		fclose(file);
		// what was changed before and is not in the preset goes back to the game's own
		const auto originals = g_originals;
		for (const auto& original : originals)
		{
			if (!in_preset.count(original.first))
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

		ImGui::SameLine();
		ImGui::BeginDisabled(selected.empty());
		if (ImGui::Button("Load"))
		{
			message = load_preset(selected) ? "Loaded " + selected + "." : "That preset could not be read.";
		}

		ImGui::SameLine();
		if (ImGui::Button("Delete"))
		{
			DeleteFileA((presets_folder() + "\\" + selected + ".txt").c_str());
			message = "Deleted " + selected + ".";
			selected.clear();
		}

		ImGui::EndDisabled();
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

void look_draw_tab(const TheaterState& state)
{
	(void)state;
	bool reset = false;
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

void look_frame(const TheaterState& state)
{
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
			else if (track.id == "sun_brightness")
			{
				sun_set_brightness(v[0] < 0.0f ? 0.0f : v[0]);
			}
			else if (track.id == "sun_direction")
			{
				sun_set_direction(v[0], v[1]);
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
}
