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
#include <cstdlib>
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

	// ---- the game's own gear: its tables say which model and which skin (a material in place of the
	// model's own) each item of gear is, and which items make a set; a model nobody in the match wears
	// is not loaded, and is asked for from the game's on-demand files (CL_LoadTransientByXModel,
	// 0x140026A20: the model's name).
	constexpr std::uintptr_t CL_LOAD_TRANSIENT_BY_XMODEL = 0x140026A20;
	constexpr int ASSET_MATERIAL = 8, MAX_MATERIALS = 65536;
	struct ModelRow
	{
		std::string model, category, lob, material;
		int primary = 0, female = -1;  // -1: either
	};
	struct IdRow
	{
		std::string category;
		int primary = 0;
		unsigned id = 0;
	};
	struct OverrideRow
	{
		std::string model, material;
		unsigned id = 0;
	};
	struct SetRow
	{
		std::string name;
		std::vector<unsigned> ids;
	};
	struct Skin
	{
		std::string base, over;  // the model's own material, and the one drawn in its place ("": none)
	};
	struct Wanted
	{
		int slot = -1;
		std::string model;
		Skin skin;
	};
	struct PendingSet
	{
		int entity = -1;
		std::string name;
		std::vector<Wanted> parts;
		ULONGLONG since = 0;
		bool asked = false;
	};
	std::vector<ModelRow> g_model_rows;
	std::vector<IdRow> g_id_rows;
	std::vector<OverrideRow> g_override_rows;
	std::vector<SetRow> g_set_rows;
	bool g_tables_read = false;
	std::unordered_map<std::string, void*> g_materials;
	std::map<int, std::map<std::string, Skin>> g_skins;  // per entity: a model put on, and its skin
	std::vector<PendingSet> g_pending;
	char g_status[200] = "";

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

	// (as take, for lists of any length)
	void take_many(void* header, void* data)
	{
		auto* found = static_cast<Found*>(data);
		if (found->count < 65536)
		{
			found->at[found->count++] = header;
		}
	}

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

	int enumerate_materials(void** out)
	{
		__try
		{
			Found found;
			found.at = out;
			found.count = 0;
			reinterpret_cast<void (*)(int, void (*)(void*, void*), const void*, bool)>(DB_ENUM_XASSETS)(ASSET_MATERIAL, take_many, &found, false);
			return found.count;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return -1;
		}
	}

	bool ask_for(const char* model)
	{
		__try
		{
			reinterpret_cast<int (*)(const char*)>(CL_LOAD_TRANSIENT_BY_XMODEL)(model);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	// (with the lock held)
	void list_materials()
	{
		static void* found[MAX_MATERIALS];
		g_materials.clear();
		const int count = enumerate_materials(found);
		for (int i = 0; i < count; ++i)
		{
			if (const char* name = name_of(found[i]))
			{
				g_materials[name] = found[i];
			}
		}
	}

	// One of the game's tables, by a part of its name: its rows of cells.
	std::vector<std::vector<std::string>> read_table(const char* part)
	{
		static void* found[4096];
		std::vector<std::vector<std::string>> rows;
		const int count = enumerate_tables(found);
		for (int i = 0; i < count && i < 4096; ++i)
		{
			const auto* table = static_cast<const unsigned char*>(found[i]);
			const char* name = name_of(table);
			if (!name)
			{
				continue;
			}

			std::string lower = name;
			std::transform(lower.begin(), lower.end(), lower.begin(), [](const unsigned char c) { return static_cast<char>(tolower(c)); });
			if (lower.find(part) == std::string::npos)
			{
				continue;
			}

			const int columns = *reinterpret_cast<const int*>(table + 8), row_count = *reinterpret_cast<const int*>(table + 12);
			const auto* cells = *reinterpret_cast<const unsigned char* const*>(table + 16);
			if (columns < 1 || columns > 200 || row_count < 1 || row_count > 20000 || IsBadReadPtr(cells, static_cast<size_t>(columns) * row_count * 16))
			{
				continue;
			}

			for (int r = 0; r < row_count; ++r)
			{
				std::vector<std::string> row;
				for (int c = 0; c < columns; ++c)
				{
					const char* text = *reinterpret_cast<const char* const*>(cells + (static_cast<size_t>(r) * columns + c) * 16);
					row.emplace_back(text && !IsBadStringPtrA(text, 512) ? text : "");
				}

				rows.push_back(std::move(row));
			}

			break;
		}

		return rows;
	}

	// (with the lock held) The gear's tables, as the game has them.
	void read_gear_tables()
	{
		if (g_tables_read)
		{
			return;
		}

		g_tables_read = true;
		for (const auto& row : read_table("costumemodeltable"))
		{
			if (row.size() >= 11 && row[0].compare(0, 3, "mp_") == 0)
			{
				ModelRow model;
				model.model = row[0];
				model.category = row[1];
				model.primary = atoi(row[3].c_str());
				model.lob = row[6];
				model.material = row[8];
				model.female = row[10] == "female" ? 1 : row[10] == "!female" ? 0 : -1;
				g_model_rows.push_back(model);
			}
		}

		for (const auto& row : read_table("costumeidtable"))
		{
			if (row.size() >= 8)
			{
				g_id_rows.push_back({row[0], atoi(row[4].c_str()), static_cast<unsigned>(strtoul(row[7].c_str(), nullptr, 16))});
			}
		}

		for (const auto& row : read_table("costumeoverridetable"))
		{
			if (row.size() >= 4 && row[0].compare(0, 3, "mp_") == 0)
			{
				g_override_rows.push_back({row[0], row[3], static_cast<unsigned>(strtoul(row[2].c_str(), nullptr, 16))});
			}
		}

		for (const auto& row : read_table("charactergearsettable"))
		{
			SetRow set;
			set.name = row.empty() ? "" : row[0];
			for (size_t c = 5; c < row.size(); ++c)
			{
				if (row[c].compare(0, 2, "0x") == 0)
				{
					set.ids.push_back(static_cast<unsigned>(strtoul(row[c].c_str(), nullptr, 16)));
				}
			}

			if (!set.name.empty() && !set.ids.empty())
			{
				g_set_rows.push_back(set);
			}
		}

		note("models: the game's gear: %zu models, %zu items, %zu skins, %zu sets", g_model_rows.size(), g_id_rows.size(), g_override_rows.size(),
		     g_set_rows.size());
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

	// (with the lock held) A list's entry for a model of the gear's: the entry's third field is the
	// material drawn in place of the model's own, its fourth the model's own (what the game's own lists
	// were seen to hold).
	void put_skin(const int entity, void* model, unsigned char* entry, const size_t stride)
	{
		const char* name = name_of(model);
		const auto skins = g_skins.find(entity);
		if (!name || stride != 0x20 || skins == g_skins.end())
		{
			return;
		}

		const auto skin = skins->second.find(name);
		if (skin == skins->second.end())
		{
			return;
		}

		const auto base = g_materials.find(skin->second.base);
		const auto over = g_materials.find(skin->second.over);
		auto* fields = reinterpret_cast<void**>(entry);
		fields[2] = over != g_materials.end() && !skin->second.over.empty() ? over->second : nullptr;
		if (base != g_materials.end())
		{
			fields[3] = base->second;
		}
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
						put_skin(handle, model, copy + stride * out, stride);
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
							put_skin(handle, loaded->second, copy + stride * out, stride);
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
	g_skins.erase(entity);
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

// The game's own sets of gear, by their names in its table.
int models_sets(char names[][32], const int max)
{
	std::lock_guard<std::mutex> lock(g_lock);
	read_gear_tables();
	int count = 0;
	for (const SetRow& set : g_set_rows)
	{
		if (count < max)
		{
			strncpy_s(names[count++], 32, set.name.c_str(), _TRUNCATE);
		}
	}

	return count;
}

void models_status(char out[200])
{
	std::lock_guard<std::mutex> lock(g_lock);
	strncpy_s(out, 200, g_status, _TRUNCATE);
}

// A whole set on the entity: its models are asked for from the game's files, and put on once they are
// there (models_game_frame). How many parts the set has for this player.
int models_apply_set(const int entity, const char* name)
{
	std::lock_guard<std::mutex> lock(g_lock);
	read_gear_tables();
	const auto seen = g_seen.find(entity);
	if (seen == g_seen.end())
	{
		return 0;
	}

	bool female = false, lob = false;
	for (const std::string& own : seen->second)
	{
		female = female || own.find("_f_") != std::string::npos;
		lob = lob || ends_with(own, "_lob");
	}

	PendingSet pending;
	pending.entity = entity;
	pending.name = name;
	pending.since = GetTickCount64();
	for (const SetRow& set : g_set_rows)
	{
		if (set.name != name)
		{
			continue;
		}

		for (const unsigned id : set.ids)
		{
			Wanted wanted;
			const ModelRow* row = nullptr;
			if ((id >> 12) & 0xFF)
			{
				// a skin: the model it is for (the player's kind), and its material
				const OverrideRow* chosen = nullptr;
				for (const OverrideRow& over : g_override_rows)
				{
					if (over.id == id && (!chosen || (over.model.find("_f_") != std::string::npos) == female))
					{
						chosen = &over;
					}
				}

				if (!chosen)
				{
					continue;
				}

				wanted.skin.over = chosen->material;
				for (const ModelRow& model : g_model_rows)
				{
					if (model.model == chosen->model)
					{
						row = &model;
					}
				}
			}
			else
			{
				for (const IdRow& item : g_id_rows)
				{
					if (item.id != id)
					{
						continue;
					}

					for (const ModelRow& model : g_model_rows)
					{
						if (model.category == item.category && model.primary == item.primary && (!row || model.female == (female ? 1 : 0)))
						{
							row = &model;
						}
					}
				}
			}

			if (!row)
			{
				continue;
			}

			wanted.model = lob && !row->lob.empty() ? row->lob : row->model;
			wanted.skin.base = row->material;
			wanted.slot = slot_of(row->model);
			if (wanted.slot >= 0 && wanted.slot != SLOT_HEAD)
			{
				pending.parts.push_back(wanted);
			}
		}
	}

	if (pending.parts.empty())
	{
		snprintf(g_status, sizeof(g_status), "The set %s has no parts the game's tables give a model for.", name);
		return 0;
	}

	snprintf(g_status, sizeof(g_status), "Loading %s: %zu parts...", name, pending.parts.size());
	const int count = static_cast<int>(pending.parts.size());
	g_pending.push_back(std::move(pending));
	return count;
}

// Each frame on the game's own thread: the sets being waited for.
void models_game_frame(const TheaterState& state)
{
	std::lock_guard<std::mutex> lock(g_lock);
	if (g_pending.empty() || !state.playing)
	{
		g_pending.clear();
		return;
	}

	PendingSet& pending = g_pending.front();
	const ULONGLONG now = GetTickCount64();
	if (!pending.asked)
	{
		pending.asked = true;
		pending.since = now;
		for (const Wanted& part : pending.parts)
		{
			ask_for(part.model.c_str());
		}

		return;
	}

	static ULONGLONG looked = 0;
	if (now - looked < 400)
	{
		return;
	}

	looked = now;
	list_loaded();
	size_t there = 0;
	for (const Wanted& part : pending.parts)
	{
		there += g_loaded.count(part.model);
	}

	if (there < pending.parts.size() && now - pending.since < 6000)
	{
		snprintf(g_status, sizeof(g_status), "Loading %s: %zu of %zu parts are there...", pending.name.c_str(), there, pending.parts.size());
		return;
	}

	// what is there is put on: in place of the part of its kind the player has, or as well
	list_materials();
	const int entity = pending.entity;
	const auto seen = g_seen.find(entity);
	++g_changed[entity];
	g_swaps.erase(entity);
	g_adds.erase(entity);
	g_skins.erase(entity);
	std::string missing;
	for (const Wanted& part : pending.parts)
	{
		if (!g_loaded.count(part.model))
		{
			missing += (missing.empty() ? "" : ", ") + part.model;
			continue;
		}

		std::string own;
		if (seen != g_seen.end())
		{
			for (const std::string& name : seen->second)
			{
				if (slot_of(name) == part.slot)
				{
					own = name;
				}
			}
		}

		if (own.empty())
		{
			g_adds[entity].push_back(part.model);
		}
		else if (own != part.model || !part.skin.over.empty())
		{
			g_swaps[entity][own] = part.model;
		}

		g_skins[entity][part.model] = part.skin;
	}

	snprintf(g_status, sizeof(g_status), missing.empty() ? "%s is on: %zu parts." : "%s: %zu parts are on; the game did not load %s", pending.name.c_str(), there,
	         missing.c_str());
	note("models: set %s on entity %d: %zu of %zu parts%s%s", pending.name.c_str(), entity, there, pending.parts.size(), missing.empty() ? "" : "; not loaded: ",
	     missing.c_str());
	g_pending.erase(g_pending.begin());
	theater_rebuild();
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
