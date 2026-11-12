// The Look tab: the game's own settings ("dvars") for the sun, the sky, fog, depth of field, film and the
// HUD, each shown as the kind of control its type calls for and with the limits the game gives it.
//
// Which settings Advanced Warfare has is found out as it runs: a row is shown only if the game has the
// setting. The names are those the engine family uses (IW6, S1, H1); H1MVM's theater editor (GPL-3) is
// where the selection for a theater comes from. A setting is changed through the game's console, and
// whatever was changed is put back when playback ends.
//
// A dvar (s1-mod's structs): name at +0, flags at +8, type at +0xC (0 bool, 1 float, 2-4 vectors, 5 int,
// 6 one of a list, 7 text, 8 colour), the current value at +0x10, the one it resets to at +0x30, its
// limits at +0x40 (floats: min, max; ints: min, max; a list: how many, and their names).
#include "ui_parts.hpp"

#include <cfloat>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
		case 8:
		case 9: snprintf(text, sizeof(text), "%g %g %g %g", value[0] / 255.0f, value[1] / 255.0f, value[2] / 255.0f, value[3] / 255.0f); break;
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

	// While a control is held its own value is shown: the game takes a change a frame later.
	struct Held
	{
		ImGuiID id = 0;
		float value[4] = {};
		int integer = 0;
	} g_held;

	// The control for a dvar, by its type. True if it was changed.
	bool control(const Dvar dvar)
	{
		ImGui::PushID(dvar.at);
		ImGui::SetNextItemWidth(-1.0f);
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
			float v[4] = {value[0] / 255.0f, value[1] / 255.0f, value[2] / 255.0f, value[3] / 255.0f};
			if (held)
			{
				memcpy(v, g_held.value, sizeof(v));
			}

			if (ImGui::ColorEdit4("##value", v, ImGuiColorEditFlags_NoInputs | (type == 9 ? ImGuiColorEditFlags_NoAlpha : 0)))
			{
				set(dvar, "%g %g %g %g", v[0], v[1], v[2], v[3]);
				changed = true;
			}

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
		default:
			ImGui::TextDisabled("?");
			break;
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

	// IrisStudio's Look sections, with the settings Advanced Warfare's engine family has for each.
	const Section SECTIONS[] = {
		{ICON_SUN, "Sun", true,
		 {{"Sun", "r_drawSun"},
		  {"Sunlight", "r_lightTweakSunLight"},
		  {"Sun color", "r_lightTweakSunColor"},
		  {"Sun direction", "r_lightTweakSunDirection"},
		  {"Use the sun's settings below", "r_sun_from_dvars"},
		  {"Sun size", "r_sunsprite_size"},
		  {"Sun position", "r_sun_fx_position"},
		  {"Flare brightness", "r_sunflare_max_alpha"},
		  {"Flare smallest", "r_sunflare_min_size"},
		  {"Flare largest", "r_sunflare_max_size"},
		  {"Glare", "r_sunglare_max_lighten"},
		  {"Blinding", "r_sunblind_max_darken"},
		  {"Sun shadows", "sm_sunEnable"},
		  {"Sun shadow softness", "sm_sunFilterRadius"},
		  {"Sun shadow scale", "sm_sunShadowScale"}}},
		{ICON_CLOUD, "Sky", true,
		 {{"Sky brightness", "r_hdrSkyIntensity"},
		  {"Sky tint", "r_hdrSkyColorTint"},
		  {"Use the sky fog below", "r_skyFogUseTweaks"},
		  {"Sky fog", "r_sky_fog_intensity"},
		  {"Sky fog from angle", "r_sky_fog_min_angle"},
		  {"Sky fog to angle", "r_sky_fog_max_angle"}}},
		{ICON_LIGHTBULB, "Lighting", false,
		 {{"Shadows", "sm_enable"},
		  {"Spotlight shadows", "sm_spotEnable"},
		  {"Use the light strengths below", "r_primaryLightUseTweaks"},
		  {"Light diffuse", "r_primaryLightTweakDiffuseStrength"},
		  {"Light specular", "r_primaryLightTweakSpecularStrength"},
		  {"Use the light grid below", "r_lightGridEnableTweaks"},
		  {"Light grid brightness", "r_lightGridIntensity"},
		  {"Light grid contrast", "r_lightGridContrast"},
		  {"Diffuse scale", "r_diffuseColorScale"},
		  {"Specular scale", "r_specularColorScale"},
		  {"Reflection sun", "r_envMapSunIntensity"},
		  {"Without lighting", "r_fullbright"},
		  {"Glow", "r_veil"},
		  {"Use the glow strength below", "r_veilUseTweaks"},
		  {"Glow strength", "r_veilStrength"},
		  {"Ambient occlusion", "r_ssao"},
		  {"Use the occlusion strength below", "r_ssaoUseTweaks"},
		  {"Occlusion strength", "r_ssaoStrength"}}},
		{ICON_CLOUD, "Fog", false,
		 {{"Fog", "r_fog"},
		  {"Fog exposure", "r_fog_ev_adjust"},
		  {"Fog on the gun", "r_fog_depthhack_scale"},
		  {"Far distance", "r_zfar"}}},
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
		  {"Invert", "r_filmTweakInvert"},
		  {"Exposure: use the values below", "r_tonemapUseTweaks"},
		  {"Automatic exposure", "r_tonemapAuto"},
		  {"Exposure", "r_tonemapExposure"},
		  {"Exposure adjust", "r_tonemapExposureAdjust"},
		  {"Brightness (screen)", "r_brightness"},
		  {"Contrast (screen)", "r_contrast"},
		  {"Desaturation (screen)", "r_desaturation"},
		  {"Blur (screen)", "r_blur"}}},
		{ICON_WAND, "Effects", false,
		 {{"Effects", "fx_enable"},
		  {"Draw effects", "fx_draw"},
		  {"Lens flares", "fx_flare"},
		  {"Bullet marks", "fx_marks"},
		  {"Post processing", "r_postfx_enable"},
		  {"Post processing", "r_postFX"},
		  {"Distortion", "r_distortion"},
		  {"Motion blur", "r_mbEnable"},
		  {"Blood", "cg_blood"},
		  {"Shell casings", "cg_brass"},
		  {"Tracers", "cg_tracerLength"},
		  {"Field of view (first person)", "cg_fov"},
		  {"Field of view scale", "cg_fovScale"},
		  {"Gun forward", "cg_gun_x"},
		  {"Gun right", "cg_gun_y"},
		  {"Gun up", "cg_gun_z"}}},
	};

	const Entry HUD_SETTINGS[] = {{"2D Elements (F3)", "cg_draw2D"},          {"Gun", "cg_drawGun"},
	                              {"Crosshair", "cg_drawCrosshair"},          {"Names over players", "cg_drawFriendlyNames"},
	                              {"Names under the crosshair", "cg_drawCrosshairNames"},
	                              {"Killfeed", "ui_hud_obituaries"},          {"Hardcore HUD", "ui_hud_hardcore"},
	                              {"HUD sways with the view", "lui_hud_motion_enabled"},
	                              {"FPS counter", "cg_drawFPS"}};

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

		ImGui::Spacing();
		ImGui::TextDisabled("Parts of the HUD (the game's own names)");
		for (int i = 0; i < count && i < 160; ++i)
		{
			char dvar_name[40];
			snprintf(dvar_name, sizeof(dvar_name), "s1iris_hud_%d", i);
			const std::string line = dvar_string(dvar_name);
			const size_t space = line.find(' ');
			if (space == std::string::npos)
			{
				continue;
			}

			const int depth = atoi(line.c_str());
			const std::string name = line.substr(space + 1);
			bool shown = !g_hud_hidden.count(name);
			ImGui::PushID(i);
			ImGui::Indent(depth * ui_font_size() * 0.8f + 0.01f);
			ui_row_label(name.c_str());
			if (ImGui::Checkbox("##shown", &shown))
			{
				if (shown)
				{
					g_hud_hidden.erase(name);
				}
				else
				{
					g_hud_hidden.insert(name);
				}

				send_hud_hidden();
			}

			ImGui::Unindent(depth * ui_font_size() * 0.8f + 0.01f);
			ImGui::PopID();
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
	if (ui_section(ICON_BURGER, "HUD", &reset))
	{
		draw_hud(reset);
	}

	for (const Section& section : SECTIONS)
	{
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
				restore(entry.name);
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
			control(dvar);
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

void look_frame(const TheaterState& state)
{
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

		if (g_hud_touched)
		{
			g_hud_touched = false;
			g_hud_hidden.clear();
			theater_command("set s1iris_hud_hide \"\"");
		}
	}
}
