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

	// ---- camouflage. A weapon's painted camouflages (the game's camo table, from "reptile" on) are whole
	// materials, one per weapon and camouflage (mp/camoMaterialTable: the weapon, then its sixteen), and the
	// game draws one by putting it in the weapon's model's entry of a list as the material drawn in place of
	// the model's own, with the byte at +0xD set (seen: [vm_dear_base_loot 00002A26 00000100 m/dear_jackpot]
	// for the AE4 with Jackpot, and zeros for a weapon with none).
	struct CamoRow
	{
		std::string weapon, prefix;          // iw5_dlcgun1, and what its models are named with: dear
		std::vector<std::string> materials;  // m/dear_reptile ...
	};
	std::vector<CamoRow> g_camo_rows;
	std::map<int, std::map<std::string, std::string>> g_camos;  // per entity: a model, and the material over it
	ULONGLONG g_materials_at = 0;
	std::vector<PendingSet> g_pending;
	char g_status[200] = "";
	// Asking the game for a model it has not loaded makes room for it by letting go of another, and that
	// can be one a player in the match is drawn with: 0.44 crashed that way (a bone looked up in a model
	// that was no longer there). So it is only done when asked for; without it a set's parts are put on
	// as far as the match has them loaded.
	bool g_allow_loading = false;

	// The proper way to have gear loaded: the match's server tells the client which gear models to keep
	// loaded, a list for each kind of gear (0x140022290 takes them: lists of 16 bytes each, a count and
	// the numbers, each a model's number within its kind), and the game loads and lets go of models by
	// those lists. A set asked for is added to the lists as they come (the recording gives them again
	// when it is gone back in), so the game holds the set's models itself, with everything else the match
	// needs. A model's number within its kind is found by asking the game for each number's name
	// (0x14000C100, with the kind from 0x1403CC830).
	constexpr std::uintptr_t CL_STREAM_SYNC = 0x140022290, BG_CUSTOMIZATION_KIND = 0x1403CC830, BG_CUSTOMIZATION_MODEL_NAME = 0x14000C100;
	constexpr int SYNC_KINDS = 10;
	constexpr size_t SYNC_SIZE = 0x4A0;
	using stream_sync_t = void (*)(int, unsigned char*, int);
	stream_sync_t g_original_stream_sync = nullptr;
	std::unordered_map<std::string, std::pair<int, unsigned>> g_numbers;  // a gear model's kind and number
	bool g_numbers_read = false;
	std::vector<std::pair<int, unsigned>> g_extra;  // what is asked for on top of the server's lists
	unsigned g_syncs = 0;

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

	const char* number_name(const int kind, const unsigned number)
	{
		__try
		{
			const int of_kind = reinterpret_cast<int (*)(int)>(BG_CUSTOMIZATION_KIND)(kind);
			return reinterpret_cast<const char* (*)(int, unsigned)>(BG_CUSTOMIZATION_MODEL_NAME)(of_kind, number);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return nullptr;
		}
	}

	// (with the lock held)
	void read_numbers()
	{
		if (g_numbers_read)
		{
			return;
		}

		g_numbers_read = true;
		for (int kind = 0; kind < SYNC_KINDS; ++kind)
		{
			std::string first;
			for (unsigned number = 0; number < 1500; ++number)
			{
				const char* name = number_name(kind, number);
				if (!name || IsBadStringPtrA(name, 96) || name[0] < 33 || strnlen(name, 96) >= 90)
				{
					break;
				}

				// (past the last of a kind the game may give its first again)
				if (number > 0 && first == name)
				{
					break;
				}

				if (number == 0)
				{
					first = name;
				}

				g_numbers.emplace(name, std::make_pair(kind, number));
			}
		}

		note("models: %zu gear models have a number the game's lists can ask for", g_numbers.size());
	}

	// The server's lists, with what the editor asks for added.
	void hooked_stream_sync(const int client, unsigned char* lists, const int flag)
	{
		static unsigned char copy[SYNC_SIZE];
		static std::vector<std::uint32_t> numbers[SYNC_KINDS];
		bool changed = false;
		if (client == 0 && lists && !IsBadReadPtr(lists, SYNC_SIZE))
		{
			std::lock_guard<std::mutex> lock(g_lock);
			++g_syncs;
			const TheaterState state = theater_state();
			if (!g_extra.empty() && (state.playing || state.starting))
			{
				memcpy(copy, lists, SYNC_SIZE);
				for (int kind = 0; kind < SYNC_KINDS; ++kind)
				{
					unsigned char* list = copy + kind * 16;
					const unsigned count = *reinterpret_cast<const std::uint16_t*>(list);
					const auto* values = *reinterpret_cast<const std::uint32_t* const*>(list + 8);
					if (count > 2000 || (count && IsBadReadPtr(values, count * 4)))
					{
						continue;
					}

					numbers[kind].assign(values, values + count);
					bool added = false;
					for (const auto& extra : g_extra)
					{
						if (extra.first != kind)
						{
							continue;
						}

						bool there = false;
						for (const std::uint32_t value : numbers[kind])
						{
							there = there || (value & 0xFFFFFF) == extra.second;
						}

						if (!there)
						{
							numbers[kind].push_back(extra.second);
							added = true;
						}
					}

					if (added)
					{
						*reinterpret_cast<std::uint16_t*>(list) = static_cast<std::uint16_t>(numbers[kind].size());
						*reinterpret_cast<const std::uint32_t**>(list + 8) = numbers[kind].data();
						changed = true;
					}
				}

				if (changed && g_noted < 200)
				{
					++g_noted;
					note("models: %zu gear models added to the lists the game loads by", g_extra.size());
				}
			}
		}

		g_original_stream_sync(client, changed ? copy : lists, flag);
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
		g_materials_at = GetTickCount64();
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

		for (const auto& row : read_table("camomaterialtable"))
		{
			CamoRow camo;
			camo.weapon = row.empty() ? "" : row[0];
			for (size_t c = 1; c < row.size(); ++c)
			{
				if (!row[c].empty())
				{
					camo.materials.push_back(row[c]);
				}
			}

			// what the weapon's materials are named with: m/dear_reptile is dear's
			if (!camo.materials.empty())
			{
				const std::string& first = camo.materials[0];
				const size_t slash = first.find('/'), last = first.rfind('_');
				if (slash != std::string::npos && last != std::string::npos && last > slash + 1)
				{
					camo.prefix = first.substr(slash + 1, last - slash - 1);
				}
			}

			if (!camo.prefix.empty())
			{
				g_camo_rows.push_back(camo);
			}
		}

		note("models: the game's gear: %zu models, %zu items, %zu skins, %zu sets, %zu weapons with camouflage", g_model_rows.size(), g_id_rows.size(),
		     g_override_rows.size(), g_set_rows.size(), g_camo_rows.size());
	}

	// (with the lock held) The camouflage row of the weapon a model is of: the one whose name is in the
	// model's (vm_dear_base_loot is dear's), the longest if several are.
	const CamoRow* camo_row(const std::string& model)
	{
		const CamoRow* best = nullptr;
		for (const CamoRow& row : g_camo_rows)
		{
			if (model.find("_" + row.prefix + "_") != std::string::npos && (!best || row.prefix.size() > best->prefix.size()))
			{
				best = &row;
			}
		}

		return best;
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

	// ---- what the game does with a list besides making the model. The making itself (DObjCreate) reads
	// only each entry's model and where it hangs; the materials in an entry are put over the model's own
	// afterwards, by a second call with the same list (0x1404DE730: the model made, the list, its length;
	// an entry with both materials and the byte at +0xD set has the one at +0x18 drawn as the one at
	// +0x10). 0.38 to 0.51 changed the list given to the making only, so no skin or camouflage of the
	// editor's was ever drawn. The game's code calls the two together (0x1401A2BF0: the list, its length,
	// the animation tree, the entity, the client, the entity again).
	constexpr std::uintptr_t DOBJ_SET_OVERRIDES = 0x1404DE730, CG_MAKE_DOBJ = 0x1401A2BF0;
	constexpr std::uintptr_t COM_GET_CLIENT_DOBJ = 0x1403D2BA0, COM_SAFE_CLIENT_DOBJ_FREE = 0x1403D2DF0;
	constexpr std::uintptr_t XMODEL_MATERIAL_COUNT = 0x1404F8070, XMODEL_MATERIALS = 0x1404F8050;
	void (*g_original_overrides)(void*, unsigned char*, int) = nullptr;
	thread_local void* t_made = nullptr;
	thread_local unsigned char t_list[MAX_IN_LIST * 0x20];
	thread_local int t_count = 0;

	// the list the game last made each entity's model from, as the game gave it: the model is made again
	// from it when the editor's choices change (going back in the recording did not always make the game
	// do so: a player whose gear is the same before and after is not made again)
	struct LastList
	{
		std::vector<unsigned char> bytes;
		unsigned short count = 0;
	};
	std::map<int, LastList> g_last;

	void hooked_overrides(void* made, unsigned char* list, const int count)
	{
		if (made && made == t_made && t_count > 0)
		{
			t_made = nullptr;
			g_original_overrides(made, t_list, t_count);
			return;
		}

		g_original_overrides(made, list, count);
	}

	// A model's materials (a model made of several models: all of theirs), as the game's own code reads
	// them (0x1404F8070 how many, 0x1404F8050 the list; the parts at +0x298, how many at +0x202, when the
	// word at +0x204 has 0x400).
	int model_materials(void* model, void** out, const int max)
	{
		__try
		{
			const auto count_of = reinterpret_cast<int (*)(void*, int)>(XMODEL_MATERIAL_COUNT);
			const auto list_of = reinterpret_cast<void** (*)(void*, int)>(XMODEL_MATERIALS);
			auto* bytes = static_cast<unsigned char*>(model);
			const bool several = (*reinterpret_cast<std::uint16_t*>(bytes + 0x204) & 0x400) != 0;
			void** parts = several ? *reinterpret_cast<void***>(bytes + 0x298) : nullptr;
			const int part_count = several ? bytes[0x202] : 1;
			int found = 0;
			for (int p = 0; p < part_count; ++p)
			{
				void* part = several ? parts[p] : model;
				if (!part)
				{
					continue;
				}

				const int count = count_of(part, 0);
				void** list = list_of(part, 0);
				for (int i = 0; list && i < count && found < max; ++i)
				{
					out[found++] = list[i];
				}
			}

			return found;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return 0;
		}
	}

	// (with the lock held) The material of a weapon's model a camouflage is drawn over: the one named
	// after the weapon, its body before any other.
	void* camo_base(void* model, const std::string& prefix, const char* model_name)
	{
		void* found[64];
		const int count = model_materials(model, found, 64);
		void* best = nullptr;
		int best_score = -1;
		size_t best_length = 0;
		std::string all, chosen;
		for (int i = 0; i < count; ++i)
		{
			const char* name = name_of(found[i]);
			if (!name)
			{
				continue;
			}

			const std::string material = name;
			all += " " + material;
			int score = 0;
			score += material.find(prefix) != std::string::npos ? 4 : 0;
			score += material.find("base") != std::string::npos || material.find("body") != std::string::npos || material.find("main") != std::string::npos ? 2 : 0;
			if (score > best_score || (score == best_score && material.size() < best_length))
			{
				best = found[i];
				best_score = score;
				best_length = material.size();
				chosen = material;
			}
		}

		static std::map<std::string, bool> told;
		if (!told[model_name] && told.size() < 40)
		{
			told[model_name] = true;
			note("models: %s's materials:%s; a camouflage goes over %s", model_name, all.empty() ? " (none read)" : all.c_str(), chosen.empty() ? "none" : chosen.c_str());
		}

		return best;
	}

	// The entity's model made again from the list the game last gave, with the game's own calls: what
	// there is let go of, and the list made and its materials put on as the game's code does.
	bool remake(const int handle, const unsigned char* bytes, const unsigned short count)
	{
		__try
		{
			void* made = reinterpret_cast<void* (*)(int, int)>(COM_GET_CLIENT_DOBJ)(handle, 0);
			if (!made)
			{
				return false;
			}

			void* tree = *static_cast<void**>(made);
			reinterpret_cast<void (*)(int)>(COM_SAFE_CLIENT_DOBJ_FREE)(handle);
			reinterpret_cast<void* (*)(const unsigned char*, unsigned short, void*, int, int, int)>(CG_MAKE_DOBJ)(bytes, count, tree, handle, 0, handle);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	// (on the game's thread) The players whose choices changed since their models were made: made again.
	void remake_changed(const TheaterState& state)
	{
		if (!state.playing)
		{
			return;
		}

		struct Job
		{
			int handle;
			LastList list;
		};
		std::vector<Job> jobs;
		{
			std::lock_guard<std::mutex> lock(g_lock);
			for (const auto& changed : g_changed)
			{
				const auto built = g_built.find(changed.first);
				const auto last = g_last.find(changed.first);
				if (changed.first < 2048 && last != g_last.end() && last->second.count && (built == g_built.end() || built->second != changed.second))
				{
					jobs.push_back({changed.first, last->second});
				}
			}
		}

		for (const Job& job : jobs)
		{
			static int told = 0;
			const bool done = remake(job.handle, job.list.bytes.data(), job.list.count);
			if (told < 40 && done)
			{
				++told;
				note("models: entity %d made again (%d parts in the game's list)", job.handle, job.list.count);
			}
		}
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

		// (the byte that has the one drawn in place of the other)
		if (fields[2] && fields[3])
		{
			entry[0xD] = 1;
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
				if (stride == 0x20 && handle < 2048)
				{
					LastList& last = g_last[handle];
					last.bytes.assign(models, models + stride * count);
					last.count = count;
				}

				const TheaterState state = theater_state();
				const auto swaps = g_swaps.find(handle);
				const auto adds = g_adds.find(handle);
				const auto camos = g_camos.find(handle);
				const bool painted = camos != g_camos.end() && !camos->second.empty() && stride == 0x20;
				if (painted && GetTickCount64() - g_materials_at > 1000)
				{
					// (the materials as they are now: one let go of since is not drawn with)
					list_materials();
				}

				const bool any = (swaps != g_swaps.end() && !swaps->second.empty()) || (adds != g_adds.end() && !adds->second.empty()) || painted;
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
						if (painted)
						{
							const char* drawn = name_of(model);
							const auto camo = drawn ? camos->second.find(drawn) : camos->second.end();
							const auto material = camo != camos->second.end() ? g_materials.find(camo->second) : g_materials.end();
							if (material != g_materials.end())
							{
								unsigned char* entry = copy + stride * out;
								void** fields = reinterpret_cast<void**>(entry);
								fields[2] = material->second;
								if (!fields[3])
								{
									// (the material it goes over: the game's own entry has it when the game's
									// own camouflage is on)
									const CamoRow* row = camo_row(drawn);
									fields[3] = row ? camo_base(model, row->prefix, drawn) : nullptr;
								}

								entry[0xD] = 1;
								changed = true;
								made += std::string(" ") + drawn + "~" + camo->second + (fields[3] ? "" : " (nothing to put it over)");
							}
						}

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

		void* result = g_original_create(changed ? copy : models, changed ? out : count, tree, handle);
		// (the call that puts the list's materials on is given the same list: see hooked_overrides)
		t_made = nullptr;
		if (changed && g_stride == 0x20 && out > 0)
		{
			memcpy(t_list, copy, static_cast<size_t>(out) * 0x20);
			t_count = out;
			t_made = result;
		}

		return result;
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

// The camouflages there are for the weapon a model is of: their materials, those the game has loaded
// (0: the model is not a weapon with any; -1: it is, and none is loaded).
int models_camos(const char* model, char materials[][64], const int max)
{
	std::lock_guard<std::mutex> lock(g_lock);
	read_gear_tables();
	const CamoRow* row = camo_row(model);
	if (!row)
	{
		return 0;
	}

	if (GetTickCount64() - g_materials_at > 3000)
	{
		list_materials();
		static size_t told = static_cast<size_t>(-1);
		size_t loaded = 0;
		for (const CamoRow& each : g_camo_rows)
		{
			for (const std::string& material : each.materials)
			{
				loaded += g_materials.count(material);
			}
		}

		if (loaded != told)
		{
			told = loaded;
			note("models: %zu of the camouflage materials are loaded (%zu of %s's %zu)", loaded,
			     static_cast<size_t>(std::count_if(row->materials.begin(), row->materials.end(), [](const std::string& m) { return g_materials.count(m) != 0; })),
			     row->weapon.c_str(), row->materials.size());
		}
	}

	int count = 0;
	for (const std::string& material : row->materials)
	{
		if (count < max && g_materials.count(material))
		{
			copy_name(materials[count++], material);
		}
	}

	// (a weapon that has camouflages, none of them in memory: -1)
	return count ? count : -1;
}

// A camouflage put on a model of the entity's (its material; none or "": as the recording has it).
void models_set_camo(const int entity, const char* model, const char* material)
{
	std::lock_guard<std::mutex> lock(g_lock);
	++g_changed[entity];
	if (material && material[0])
	{
		g_camos[entity][model] = material;
	}
	else
	{
		g_camos[entity].erase(model);
	}
}

bool models_camo(const int entity, const char* model, char material[64])
{
	std::lock_guard<std::mutex> lock(g_lock);
	const auto camos = g_camos.find(entity);
	if (camos == g_camos.end())
	{
		return false;
	}

	const auto camo = camos->second.find(model);
	if (camo == camos->second.end())
	{
		return false;
	}

	copy_name(material, camo->second);
	return true;
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
	g_camos.erase(entity);
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

namespace
{
	ULONGLONG& looked_at()
	{
		static ULONGLONG at = 0;
		return at;
	}

	void looked_reset()
	{
		looked_at() = 0;
	}
}

void models_allow_loading(const bool on)
{
	std::lock_guard<std::mutex> lock(g_lock);
	g_allow_loading = on;
}

bool models_loading_allowed()
{
	std::lock_guard<std::mutex> lock(g_lock);
	return g_allow_loading;
}

// Each frame on the game's own thread: the sets being waited for.
void models_game_frame(const TheaterState& state)
{
	remake_changed(state);
	std::lock_guard<std::mutex> lock(g_lock);
	if (!state.playing && !state.starting)
	{
		g_pending.clear();
		g_extra.clear();
		return;
	}

	if (g_pending.empty())
	{
		return;
	}

	PendingSet& pending = g_pending.front();
	const ULONGLONG now = GetTickCount64();
	if (!pending.asked)
	{
		pending.asked = true;
		pending.since = now;
		if (g_allow_loading)
		{
			// what is not loaded goes onto the game's own lists: they are given again, with it, when the
			// recording is gone back in
			read_numbers();
			list_loaded();
			size_t asked = 0;
			for (const Wanted& part : pending.parts)
			{
				const auto number = g_numbers.find(part.model);
				if (!g_loaded.count(part.model) && number != g_numbers.end())
				{
					g_extra.push_back(number->second);
					++asked;
				}
			}

			if (asked)
			{
				pending.since = now + 4000;  // (the going back, then the loading)
				theater_rebuild();
				return;
			}
		}

		// (nothing is asked for: what is there now is what there is)
		pending.since = now > 10000 ? now - 10000 : 0;
		looked_reset();
	}

	static ULONGLONG& looked = looked_at();
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

	if (there < pending.parts.size() && (now < pending.since || now - pending.since < 8000))
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

	snprintf(g_status, sizeof(g_status),
	         missing.empty() ? "%s is on: %zu parts." : g_allow_loading ? "%s: %zu parts are on; the game did not load %s"
	                                                                    : "%s: %zu parts are on; not in this match's memory: %s",
	         pending.name.c_str(), there, missing.c_str());
	note("models: set %s on entity %d: %zu of %zu parts%s%s", pending.name.c_str(), entity, there, pending.parts.size(), missing.empty() ? "" : "; not loaded: ",
	     missing.c_str());
	// (the player's model is made again from here on its own: see remake_changed)
	g_pending.erase(g_pending.begin());
}

// Each frame: between playbacks the game may load another map's models.
void models_frame(const TheaterState& state)
{
	if (!state.playing && !state.starting)
	{
		std::lock_guard<std::mutex> lock(g_lock);
		g_listed = false;
		g_seen.clear();
		g_last.clear();
		g_built.clear();
		g_extra.clear();
		g_pending.clear();
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
	if (MH_CreateHook(reinterpret_cast<LPVOID>(DOBJ_SET_OVERRIDES), reinterpret_cast<LPVOID>(&hooked_overrides),
	                  reinterpret_cast<LPVOID*>(&g_original_overrides)) != MH_OK ||
	    MH_EnableHook(reinterpret_cast<LPVOID>(DOBJ_SET_OVERRIDES)) != MH_OK)
	{
		theater_log("models: the putting on of materials NOT hooked (no skins or camouflage)");
	}

	if (MH_CreateHook(reinterpret_cast<LPVOID>(CL_STREAM_SYNC), reinterpret_cast<LPVOID>(&hooked_stream_sync),
	                  reinterpret_cast<LPVOID*>(&g_original_stream_sync)) != MH_OK ||
	    MH_EnableHook(reinterpret_cast<LPVOID>(CL_STREAM_SYNC)) != MH_OK)
	{
		theater_log("models: the lists the game loads gear by NOT hooked");
	}
}
