// The focus handed to ReShade: a number of the editor's (how far off the thing in focus is) written into a
// setting of one of ReShade's effects, every picture, so that any depth of field effect of ReShade's can be
// racked by the editor's focus (a pinned player or actor, or keyframes).
//
// ReShade lets another module take part as an "add-on" (its own headers, third_party/reshade, BSD 3-clause):
// the module registers itself, and is called before the effects are drawn with the "effect runtime", which
// can list the effects' settings ("uniform variables"), read them and set them. Built against the add-on API
// of ReShade 6.0 (version 10); a ReShade older than that refuses, and so may a build of ReShade without
// add-on support: reshade_link_status says which.
//
// Dear ImGui is NOT included here: ReShade's overlay header would then want its own version of it.
#include "theater.hpp"

#include <reshade.hpp>

#include <cstring>
#include <mutex>
#include <vector>

namespace
{
	std::mutex g_lock;
	bool g_registered = false, g_found = false, g_called = false;
	int g_tries = 0;
	ULONGLONG g_tried_at = 0;
	bool g_list_wanted = false, g_send = false, g_have_current = false, g_missing = false;
	std::vector<ReshadeSetting> g_list;
	char g_effect[64] = "", g_name[64] = "";
	float g_value = 0.0f, g_current = 0.0f;

	void on_begin_effects(reshade::api::effect_runtime* runtime, reshade::api::command_list*, reshade::api::resource_view, reshade::api::resource_view)
	{
		std::lock_guard<std::mutex> lock(g_lock);
		g_called = true;
		if (!runtime)
		{
			return;
		}

		if (g_list_wanted)
		{
			g_list_wanted = false;
			g_list.clear();
			runtime->enumerate_uniform_variables(nullptr, [](reshade::api::effect_runtime* from, reshade::api::effect_uniform_variable variable) {
				reshade::api::format type = reshade::api::format::unknown;
				std::uint32_t rows = 0, columns = 0, length = 0;
				from->get_uniform_variable_type(variable, &type, &rows, &columns, &length);
				char source[64] = "";
				if (type != reshade::api::format::r32_float || rows != 1 || columns != 1 || g_list.size() >= 2000 ||
				    from->get_annotation_string_from_uniform_variable(variable, "source", source))
				{
					// (not a single number, or one ReShade itself fills in: the time, the mouse)
					return;
				}

				ReshadeSetting setting = {};
				from->get_uniform_variable_name(variable, setting.name);
				from->get_uniform_variable_effect_name(variable, setting.effect);
				if (!from->get_annotation_string_from_uniform_variable(variable, "ui_label", setting.label))
				{
					strncpy_s(setting.label, setting.name, _TRUNCATE);
				}

				from->get_uniform_value_float(variable, &setting.value, 1);
				g_list.push_back(setting);
			});
		}

		if (g_effect[0] && g_name[0])
		{
			const reshade::api::effect_uniform_variable variable = runtime->find_uniform_variable(g_effect, g_name);
			g_missing = variable.handle == 0;
			if (!g_missing)
			{
				if (g_send)
				{
					runtime->set_uniform_value_float(variable, &g_value, 1);
				}
				else
				{
					runtime->get_uniform_value_float(variable, &g_current, 1);
					g_have_current = true;
				}
			}
		}
	}
}

void reshade_link_frame()
{
	if (g_registered || g_tries >= 8 || GetTickCount64() - g_tried_at < 4000)
	{
		return;
	}

	g_tried_at = GetTickCount64();
	++g_tries;
	g_found = reshade::internal::get_reshade_module_handle() != nullptr;
	// (0.85 gave no module: the header does not find the caller's own, and ReShade 6.8 refused without a word)
	HMODULE own = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&reshade_link_frame), &own);
	if (g_found && own && reshade::register_addon(own))
	{
		g_registered = true;
		reshade::register_event<reshade::addon_event::reshade_begin_effects>(&on_begin_effects);
		theater_log("reshade: registered with ReShade as an add-on (for the focus)");
	}
	else if (g_tries == 8)
	{
		theater_log(g_found ? "reshade: ReShade is there but would not take the editor as an add-on (a build without add-on support, or older than 6.0)"
		                    : "reshade: no ReShade found in the game");
	}
}

int reshade_link_status()
{
	std::lock_guard<std::mutex> lock(g_lock);
	return g_registered ? (g_called ? 3 : 2) : g_found ? 1 : 0;
}

void reshade_link_list_request()
{
	std::lock_guard<std::mutex> lock(g_lock);
	g_list_wanted = true;
}

int reshade_link_list(ReshadeSetting* out, const int max)
{
	std::lock_guard<std::mutex> lock(g_lock);
	const int count = static_cast<int>(g_list.size()) < max ? static_cast<int>(g_list.size()) : max;
	for (int i = 0; i < count; ++i)
	{
		out[i] = g_list[static_cast<size_t>(i)];
	}

	return count;
}

void reshade_link_send(const bool send, const char* effect, const char* name, const float value)
{
	std::lock_guard<std::mutex> lock(g_lock);
	if (strcmp(g_effect, effect) != 0 || strcmp(g_name, name) != 0)
	{
		strncpy_s(g_effect, effect, _TRUNCATE);
		strncpy_s(g_name, name, _TRUNCATE);
		g_have_current = false;
		g_missing = false;
	}

	g_send = send;
	g_value = value;
}

bool reshade_link_current(float* value, bool* missing)
{
	std::lock_guard<std::mutex> lock(g_lock);
	*value = g_current;
	*missing = g_missing;
	return g_have_current;
}
