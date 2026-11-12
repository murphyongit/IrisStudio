// The model swapper: any model the game puts on a player (a body part, gear, a gun) drawn as another, or
// left out.
//
// What the game draws of an entity is a list of models put together (a "DObj"): a player's is the costume's
// parts and the weapon in the hands; the first person's arms and gun are one too. Every one of them is made
// in Com_ClientDObjCreate (0x1403D2AF0: the list, how many, the animations, the entity's number). The list's
// entries begin with the model (whose first field is its name). Here each list is noted by the entity it
// is for, so the editor can show what a player is made of, and the swaps asked for are made in a copy of
// the list before the game sees it. A model is only made when its entity comes into the match, so a swap
// shows once the recording has been gone back in (the editor does that).
#include "theater.hpp"

#include <MinHook.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
	constexpr std::uintptr_t COM_CLIENT_DOBJ_CREATE = 0x1403D2AF0, DB_ENUM_XASSETS = 0x14026EC80;
	constexpr int ASSET_XMODEL = 7, MAX_IN_LIST = 32, MAX_LOADED = 32768;

	using create_t = void* (*)(unsigned char*, unsigned short, void*, int);
	create_t g_original_create = nullptr;
	std::mutex g_lock;
	std::map<int, std::vector<std::string>> g_seen;   // an entity's models, by their own names
	std::map<std::string, std::string> g_swaps;       // a model's name, and what is drawn for it ("": nothing)
	std::unordered_map<std::string, void*> g_loaded;  // the models the game has loaded
	std::vector<std::string> g_names;
	bool g_listed = false;
	size_t g_stride = 0;
	int g_noted = 0;

	void note(const char* format, ...)
	{
		char text[1024];
		va_list args;
		va_start(args, format);
		vsnprintf(text, sizeof(text), format, args);
		va_end(args);
		theater_log(text);
	}

	// A model's name, if this is a model.
	const char* name_of(const void* model)
	{
		if (!model || IsBadReadPtr(model, sizeof(void*)))
		{
			return nullptr;
		}

		const char* name = *static_cast<const char* const*>(model);
		if (!name || IsBadStringPtrA(name, 128))
		{
			return nullptr;
		}

		const size_t length = strnlen(name, 128);
		if (length < 2 || length >= 120)
		{
			return nullptr;
		}

		for (size_t i = 0; i < length; ++i)
		{
			if (name[i] < 32 || name[i] > 126)
			{
				return nullptr;
			}
		}

		return name;
	}

	struct Found
	{
		void** at;
		int count;
	};

	void take(void* header, void* data)
	{
		auto* found = static_cast<Found*>(data);
		if (found->count < MAX_LOADED)
		{
			found->at[found->count++] = header;
		}
	}

	int enumerate(void** out)
	{
		__try
		{
			Found found;
			found.at = out;
			found.count = 0;
			reinterpret_cast<void (*)(int, void (*)(void*, void*), const void*, bool)>(DB_ENUM_XASSETS)(ASSET_XMODEL, take, &found, false);
			return found.count;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return -1;
		}
	}

	// (with the lock held)
	void list_loaded()
	{
		static void* found[MAX_LOADED];
		g_listed = true;
		g_loaded.clear();
		g_names.clear();
		const int count = enumerate(found);
		for (int i = 0; i < count; ++i)
		{
			if (const char* name = name_of(found[i]))
			{
				g_loaded[name] = found[i];
			}
		}

		for (const auto& model : g_loaded)
		{
			g_names.push_back(model.first);
		}

		std::sort(g_names.begin(), g_names.end());
		note("models: the game has %d models loaded (%zu by name)", count, g_names.size());
	}

	void* hooked_create(unsigned char* models, const unsigned short count, void* tree, const int handle)
	{
		unsigned char copy[MAX_IN_LIST * 0x20];
		unsigned short out = count;
		bool changed = false;
		if (models && count >= 1 && count <= MAX_IN_LIST)
		{
			std::lock_guard<std::mutex> lock(g_lock);
			if (!g_stride && count >= 2)
			{
				// how far apart the list's entries are: the second entry is a model too
				for (const size_t stride : {size_t(0x10), size_t(0x18), size_t(0x20), size_t(8)})
				{
					if (!IsBadReadPtr(models, stride * 2) && name_of(*reinterpret_cast<void**>(models + stride)))
					{
						g_stride = stride;
						note("models: a model list's entries are %zu bytes", stride);
						break;
					}
				}
			}

			const size_t stride = g_stride;
			std::vector<std::string> names;
			if ((stride || count == 1) && !IsBadReadPtr(models, (stride ? stride : 8) * count))
			{
				for (int i = 0; i < count; ++i)
				{
					const char* name = name_of(*reinterpret_cast<void**>(models + stride * i));
					if (!name)
					{
						names.clear();
						break;
					}

					names.emplace_back(name);
				}
			}

			if (!names.empty())
			{
				if (g_noted < 40)
				{
					++g_noted;
					std::string all;
					for (const std::string& name : names)
					{
						all += (all.empty() ? "" : ", ") + name;
					}

					note("models: entity %d is made of %s", handle, all.c_str());
				}

				g_seen[handle] = names;
				const TheaterState state = theater_state();
				if (!g_swaps.empty() && stride && (state.playing || state.starting))
				{
					if (!g_listed)
					{
						list_loaded();
					}

					out = 0;
					for (int i = 0; i < count; ++i)
					{
						void* model = *reinterpret_cast<void**>(models + stride * i);
						const auto swap = g_swaps.find(names[static_cast<size_t>(i)]);
						if (swap != g_swaps.end())
						{
							if (swap->second.empty())
							{
								// left out (the first is what the rest hang on: it stays)
								if (i > 0)
								{
									changed = true;
									continue;
								}
							}
							else if (const auto loaded = g_loaded.find(swap->second); loaded != g_loaded.end() && name_of(loaded->second))
							{
								model = loaded->second;
								changed = true;
							}
						}

						memcpy(copy + stride * out, models + stride * i, stride);
						*reinterpret_cast<void**>(copy + stride * out) = model;
						++out;
					}
				}
			}
		}

		return g_original_create(changed ? copy : models, changed ? out : count, tree, handle);
	}

	void copy_name(char* out, const std::string& name)
	{
		strncpy_s(out, 64, name.c_str(), _TRUNCATE);
	}
}

int models_of(const int entity, char names[][64], const int max)
{
	std::lock_guard<std::mutex> lock(g_lock);
	const auto seen = g_seen.find(entity);
	if (seen == g_seen.end())
	{
		return 0;
	}

	int count = 0;
	for (const std::string& name : seen->second)
	{
		if (count < max)
		{
			copy_name(names[count++], name);
		}
	}

	return count;
}

void models_swap(const char* from, const char* to)
{
	std::lock_guard<std::mutex> lock(g_lock);
	if (to)
	{
		g_swaps[from] = to;
	}
	else
	{
		g_swaps.erase(from);
	}
}

int models_swaps(char from[][64], char to[][64], const int max)
{
	std::lock_guard<std::mutex> lock(g_lock);
	int count = 0;
	for (const auto& swap : g_swaps)
	{
		if (count < max)
		{
			copy_name(from[count], swap.first);
			copy_name(to[count], swap.second);
			++count;
		}
	}

	return count;
}

// The loaded models whose names have the text in them.
int models_find(const char* text, char names[][64], const int max)
{
	std::lock_guard<std::mutex> lock(g_lock);
	if (!g_listed)
	{
		list_loaded();
	}

	int count = 0;
	for (const std::string& name : g_names)
	{
		if (count >= max)
		{
			break;
		}

		if (!text[0] || name.find(text) != std::string::npos)
		{
			copy_name(names[count++], name);
		}
	}

	return count;
}

// Each frame: between playbacks the game may load another map's models.
void models_frame(const TheaterState& state)
{
	if (!state.playing && !state.starting)
	{
		std::lock_guard<std::mutex> lock(g_lock);
		g_listed = false;
		g_seen.clear();
	}
}

void models_start()
{
	MH_STATUS status = MH_CreateHook(reinterpret_cast<LPVOID>(COM_CLIENT_DOBJ_CREATE), reinterpret_cast<LPVOID>(&hooked_create),
	                                 reinterpret_cast<LPVOID*>(&g_original_create));
	if (status == MH_OK)
	{
		status = MH_EnableHook(reinterpret_cast<LPVOID>(COM_CLIENT_DOBJ_CREATE));
	}

	theater_log(status == MH_OK ? "models: the making of models hooked (models can be swapped)" : "models: the making of models NOT hooked");
}
