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
#include <cctype>
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
	constexpr int ASSET_XMODEL = 7, ASSET_STRINGTABLE = 51, MAX_IN_LIST = 32, MAX_LOADED = 32768;

	using create_t = void* (*)(unsigned char*, unsigned short, void*, int);
	create_t g_original_create = nullptr;
	std::mutex g_lock;
	std::map<int, std::vector<std::string>> g_seen;                // an entity's models, by their own names
	std::map<int, std::map<std::string, std::string>> g_swaps;     // per entity: a model's name, and what is drawn for it ("": nothing)
	std::map<int, std::vector<std::string>> g_adds;                // per entity: models put on as well
	std::map<int, unsigned> g_built;                               // per entity: the changes its model was last made with
	std::unordered_map<std::string, void*> g_loaded;               // the models the game has loaded
	std::vector<std::string> g_names;
	bool g_listed = false;
	size_t g_stride = 0;
	int g_noted = 0, g_noted_entries = 0;
	std::map<int, unsigned> g_changed;                             // per entity: how many changes were asked for

	// The parts of a multiplayer outfit, by how their models' names begin (headgear before head).
	const char* const SLOTS[] = {"mp_top", "mp_headgear", "mp_head", "mp_pants", "mp_glove", "mp_boot", "mp_kneepad", "mp_loadouts", "mp_eyewear", "mp_exo"};
	constexpr int SLOT_COUNT = 10, SLOT_HEAD = 2;

	bool ends_with(const std::string& text, const char* end)
	{
		const size_t length = strlen(end);
		return text.size() >= length && text.compare(text.size() - length, length, end) == 0;
	}

	int slot_of(const std::string& name)
	{
		if (name.compare(0, 8, "mp_view_") == 0)
		{
			return -1;
		}

		for (int i = 0; i < SLOT_COUNT; ++i)
		{
			const size_t length = strlen(SLOTS[i]);
			if (name.compare(0, length, SLOTS[i]) == 0 && (name.size() == length || name[length] == '_'))
			{
				return i;
			}
		}

		return -1;
	}

	std::vector<std::string> tokens_of(const std::string& name)
	{
		std::vector<std::string> tokens;
		size_t at = 0;
		while (at <= name.size())
		{
			const size_t next = name.find('_', at);
			tokens.push_back(name.substr(at, next == std::string::npos ? std::string::npos : next - at));
			if (next == std::string::npos)
			{
				break;
			}

			at = next + 1;
		}

		return tokens;
	}

	// The gear a part belongs to, as its name has it: two digits and a letter (mp_top_m_a_01a: 01a).
	std::string set_of(const std::string& name)
	{
		for (const std::string& token : tokens_of(name))
		{
			if (token.size() == 3 && isdigit(static_cast<unsigned char>(token[0])) && isdigit(static_cast<unsigned char>(token[1])) &&
			    isalpha(static_cast<unsigned char>(token[2])))
			{
				return token;
			}
		}

		return {};
	}

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

	int enumerate_tables(void** out)
	{
		__try
		{
			Found found;
			found.at = out;
			found.count = 0;
			reinterpret_cast<void (*)(int, void (*)(void*, void*), const void*, bool)>(DB_ENUM_XASSETS)(ASSET_STRINGTABLE, take, &found, false);
			return found.count;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return -1;
		}
	}

	// The game's own tables of outfits, gear and loot (its "string tables": a name, columns, rows, cells
	// of 16 bytes that begin with the text), written to theater\tables once, to build the editor's lists
	// of gear from what the game's menu itself goes by.
	void dump_tables()
	{
		static bool done = false;
		if (done)
		{
			return;
		}

		done = true;
		static void* found[4096];
		const int count = enumerate_tables(found);
		const std::string folder = std::string(theater_folder()) + "\\tables";
		CreateDirectoryA(folder.c_str(), nullptr);
		std::string all;
		int written = 0;
		for (int i = 0; i < count && i < 4096; ++i)
		{
			const auto* table = static_cast<const unsigned char*>(found[i]);
			const char* name = name_of(table);
			if (!name)
			{
				continue;
			}

			all += std::string(name) + " ";
			std::string lower = name;
			std::transform(lower.begin(), lower.end(), lower.begin(), [](const unsigned char c) { return static_cast<char>(tolower(c)); });
			const bool wanted = lower.find("costume") != std::string::npos || lower.find("gear") != std::string::npos ||
			                    lower.find("loot") != std::string::npos || lower.find("camo") != std::string::npos ||
			                    lower.find("statstable") != std::string::npos || lower.find("attach") != std::string::npos ||
			                    lower.find("character") != std::string::npos || lower.find("set") != std::string::npos;
			const int columns = *reinterpret_cast<const int*>(table + 8), rows = *reinterpret_cast<const int*>(table + 12);
			const auto* cells = *reinterpret_cast<const unsigned char* const*>(table + 16);
			if (!wanted || columns < 1 || columns > 200 || rows < 1 || rows > 20000 || IsBadReadPtr(cells, static_cast<size_t>(columns) * rows * 16))
			{
				continue;
			}

			std::string file_name = name;
			std::replace(file_name.begin(), file_name.end(), '/', '_');
			std::replace(file_name.begin(), file_name.end(), '\\', '_');
			FILE* file = nullptr;
			if (fopen_s(&file, (folder + "\\" + file_name).c_str(), "w") != 0 || !file)
			{
				continue;
			}

			for (int r = 0; r < rows; ++r)
			{
				for (int c = 0; c < columns; ++c)
				{
					const char* text = *reinterpret_cast<const char* const*>(cells + (static_cast<size_t>(r) * columns + c) * 16);
					fprintf(file, c ? ",%s" : "%s", text && !IsBadStringPtrA(text, 512) ? text : "");
				}

				fputc('\n', file);
			}

			fclose(file);
			++written;
		}

		note("models: the game's tables: %d, %d of them written to theater\\tables", count, written);
		for (size_t at = 0; at < all.size(); at += 900)
		{
			note("models: tables: %s", all.substr(at, 900).c_str());
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

		int outfit = 0, weapons = 0;
		for (const auto& model : g_loaded)
		{
			g_names.push_back(model.first);
			outfit += slot_of(model.first) >= 0 ? 1 : 0;
			weapons += model.first.compare(0, 4, "npc_") == 0 ? 1 : 0;
		}

		std::sort(g_names.begin(), g_names.end());
		note("models: the game has %d models loaded: %d outfit parts, %d world weapons and their parts", count, outfit, weapons);
		dump_tables();
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
				for (const size_t stride : {size_t(0x20), size_t(0x10), size_t(0x18), size_t(8)})
				{
					if (!IsBadReadPtr(models, stride * 2) && name_of(*reinterpret_cast<void**>(models + stride)))
					{
						g_stride = stride;
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
				// (what else a list's entry holds, for the weapons' camouflage: noted for the first person's
				// and the players' lists a few times)
				if ((handle == 2048 || handle < 18) && stride == 0x20 && g_noted_entries < 8)
				{
					++g_noted_entries;
					std::string all;
					for (int i = 0; i < count; ++i)
					{
						char text[160];
						const unsigned char* e = models + stride * i;
						const std::uint64_t* q = reinterpret_cast<const std::uint64_t*>(e);
						snprintf(text, sizeof(text), " [%s %08X %08X %llX %llX]", names[static_cast<size_t>(i)].c_str(), *reinterpret_cast<const unsigned*>(e + 8),
						         *reinterpret_cast<const unsigned*>(e + 12), static_cast<unsigned long long>(q[2]), static_cast<unsigned long long>(q[3]));
						all += text;
						// (a pointer there may be to something with a name: a material)
						for (int k = 2; k < 4; ++k)
						{
							if (const char* inner = name_of(reinterpret_cast<const void*>(q[k])))
							{
								all += std::string("=") + inner;
							}
						}
					}

					note("models: entity %d's list:%s", handle, all.c_str());
				}

				g_seen[handle] = names;
				g_built[handle] = g_changed[handle];
				const TheaterState state = theater_state();
				const auto swaps = g_swaps.find(handle);
				const auto adds = g_adds.find(handle);
				const bool any = (swaps != g_swaps.end() && !swaps->second.empty()) || (adds != g_adds.end() && !adds->second.empty());
				if (any && stride && stride <= 0x20 && (state.playing || state.starting))
				{
					if (!g_listed)
					{
						list_loaded();
					}

					std::string made;
					out = 0;
					for (int i = 0; i < count; ++i)
					{
						void* model = *reinterpret_cast<void**>(models + stride * i);
						if (swaps != g_swaps.end())
						{
							const auto swap = swaps->second.find(names[static_cast<size_t>(i)]);
							if (swap != swaps->second.end())
							{
								if (swap->second.empty())
								{
									// left out (the first is what the rest hang on: it stays)
									if (i > 0)
									{
										changed = true;
										made += " -" + names[static_cast<size_t>(i)];
										continue;
									}
								}
								else if (const auto loaded = g_loaded.find(swap->second); loaded != g_loaded.end() && name_of(loaded->second))
								{
									model = loaded->second;
									changed = true;
									made += " " + names[static_cast<size_t>(i)] + ">" + swap->second;
								}
							}
						}

						memcpy(copy + stride * out, models + stride * i, stride);
						*reinterpret_cast<void**>(copy + stride * out) = model;
						++out;
					}

					if (adds != g_adds.end())
					{
						// a part put on as well: an entry like the outfit's second part's (it hangs on the
						// same bones), with the model
						for (const std::string& name : adds->second)
						{
							const auto loaded = g_loaded.find(name);
							if (out >= MAX_IN_LIST || loaded == g_loaded.end() || !name_of(loaded->second))
							{
								continue;
							}

							const int like = count > 1 && slot_of(names[1]) >= 0 ? 1 : 0;
							memcpy(copy + stride * out, models + stride * like, stride);
							*reinterpret_cast<void**>(copy + stride * out) = loaded->second;
							++out;
							changed = true;
							made += " +" + name;
						}
					}

					if (changed && g_noted < 200)
					{
						++g_noted;
						note("models: entity %d made with:%s", handle, made.c_str());
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

void models_swap(const int entity, const char* from, const char* to)
{
	std::lock_guard<std::mutex> lock(g_lock);
	++g_changed[entity];
	if (to)
	{
		g_swaps[entity][from] = to;
	}
	else
	{
		g_swaps[entity].erase(from);
	}
}

// What a model of the entity's is drawn as: false if as it is.
bool models_swapped(const int entity, const char* from, char to[64])
{
	std::lock_guard<std::mutex> lock(g_lock);
	const auto swaps = g_swaps.find(entity);
	if (swaps == g_swaps.end())
	{
		return false;
	}

	const auto swap = swaps->second.find(from);
	if (swap == swaps->second.end())
	{
		return false;
	}

	copy_name(to, swap->second);
	return true;
}

void models_add(const int entity, const char* name, const bool on)
{
	std::lock_guard<std::mutex> lock(g_lock);
	++g_changed[entity];
	std::vector<std::string>& adds = g_adds[entity];
	adds.erase(std::remove(adds.begin(), adds.end(), std::string(name)), adds.end());
	if (on)
	{
		adds.push_back(name);
	}
}

int models_added(const int entity, char names[][64], const int max)
{
	std::lock_guard<std::mutex> lock(g_lock);
	int count = 0;
	const auto adds = g_adds.find(entity);
	if (adds != g_adds.end())
	{
		for (const std::string& name : adds->second)
		{
			if (count < max)
			{
				copy_name(names[count++], name);
			}
		}
	}

	return count;
}

void models_clear(const int entity)
{
	std::lock_guard<std::mutex> lock(g_lock);
	++g_changed[entity];
	g_swaps.erase(entity);
	g_adds.erase(entity);
}

// The entity has changes its model has not been made with yet.
bool models_pending(const int entity)
{
	std::lock_guard<std::mutex> lock(g_lock);
	const auto built = g_built.find(entity);
	return built != g_built.end() && built->second != g_changed[entity];
}

// The outfit part a model is (an index into models_slot_names), or -1.
int models_slot(const char* name)
{
	return slot_of(name);
}

// The loaded models whose names have the text in them. group: 0 outfit parts (slot: which, or -1 for
// all), 1 weapons and their parts as others see them, 2 the first person's, 3 every model.
int models_find(const char* text, const int group, const int slot, char names[][64], const int max)
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

		const bool first_person = name.compare(0, 3, "vm_") == 0 || name.compare(0, 10, "viewmodel_") == 0 || name.compare(0, 8, "mp_view_") == 0;
		const int part = slot_of(name);
		const bool in_group = group == 0 ? part >= 0 && (slot < 0 || part == slot) && !ends_with(name, "_lob")
			: group == 1 ? name.compare(0, 4, "npc_") == 0
			: group == 2 ? first_person
			: true;
		if (in_group && (!text[0] || name.find(text) != std::string::npos))
		{
			copy_name(names[count++], name);
		}
	}

	return count;
}

// The gear the loaded outfit parts come in (their names' "01a", "05a"...): those with parts for three
// kinds of part or more.
int models_sets(char ids[][16], const int max)
{
	std::lock_guard<std::mutex> lock(g_lock);
	if (!g_listed)
	{
		list_loaded();
	}

	std::map<std::string, unsigned> sets;
	for (const std::string& name : g_names)
	{
		const int slot = slot_of(name);
		const std::string set = set_of(name);
		if (slot >= 0 && slot != SLOT_HEAD && !set.empty() && !ends_with(name, "_lob"))
		{
			sets[set] |= 1u << slot;
		}
	}

	int count = 0;
	for (const auto& set : sets)
	{
		unsigned kinds = 0;
		for (unsigned bits = set.second; bits; bits &= bits - 1)
		{
			++kinds;
		}

		if (kinds >= 3 && count < max)
		{
			strncpy_s(ids[count++], 16, set.first.c_str(), _TRUNCATE);
		}
	}

	return count;
}

// Every part of the entity's outfit that the gear has, at once (the face stays): how many parts.
int models_apply_set(const int entity, const char* id)
{
	std::lock_guard<std::mutex> lock(g_lock);
	if (!g_listed)
	{
		list_loaded();
	}

	const auto seen = g_seen.find(entity);
	if (seen == g_seen.end())
	{
		return 0;
	}

	++g_changed[entity];
	g_swaps.erase(entity);
	g_adds.erase(entity);
	int applied = 0;
	const bool lob = !seen->second.empty() && ends_with(seen->second[0], "_lob");
	for (int slot = 0; slot < SLOT_COUNT; ++slot)
	{
		if (slot == SLOT_HEAD)
		{
			continue;
		}

		std::string own;
		for (const std::string& name : seen->second)
		{
			if (slot_of(name) == slot)
			{
				own = name;
			}
		}

		// of the gear's parts of this kind, the one whose name has the most in common with the part worn
		const std::vector<std::string> own_tokens = tokens_of(own);
		std::string best;
		int best_score = -1;
		for (const std::string& name : g_names)
		{
			if (slot_of(name) != slot || set_of(name) != id || ends_with(name, "_lob") != lob)
			{
				continue;
			}

			int score = 0;
			for (const std::string& token : tokens_of(name))
			{
				score += std::find(own_tokens.begin(), own_tokens.end(), token) != own_tokens.end() ? 1 : 0;
			}

			if (score > best_score)
			{
				best_score = score;
				best = name;
			}
		}

		if (best.empty() || best == own)
		{
			continue;
		}

		if (own.empty())
		{
			g_adds[entity].push_back(best);
		}
		else
		{
			g_swaps[entity][own] = best;
		}

		++applied;
	}

	return applied;
}

// Each frame: between playbacks the game may load another map's models.
void models_frame(const TheaterState& state)
{
	if (!state.playing && !state.starting)
	{
		std::lock_guard<std::mutex> lock(g_lock);
		g_listed = false;
		g_seen.clear();
		g_built.clear();
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
