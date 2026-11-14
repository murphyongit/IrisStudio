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
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <mutex>
#include <random>
#include <set>
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
	// is not loaded, and is asked for from the game's on-demand files by way of the game's own lists (see
	// "the game's on-demand files" below).
	constexpr int ASSET_MATERIAL = 8, MAX_MATERIALS = 65536;
	struct ModelRow
	{
		std::string model, category, lob, material;
		int primary = 0;
		std::vector<std::string> groups;  // what must be worn with it ("female", a face's name), or not ("!kneepads")
	};
	struct IdRow
	{
		std::string category;
		int primary = 0;
		unsigned id = 0;
		std::vector<std::string> tags;    // what wearing it counts as ("fullmask", "kneepads", a face's name and skin)
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
	// Whether a set's parts that are not in memory are asked for from the game's files; without it a set's
	// parts are put on as far as the match has them loaded. (0.44's crash, a bone looked up in a model that
	// was no longer there, came of calling what unloads a model in the belief that it loaded one.)
	bool g_allow_loading = true;
	ULONGLONG g_extra_dirty = 0;  // when the editor's choices last changed
	ULONGLONG g_relist_at = 0;    // when what the game has in memory is to be read afresh (after gear is let go of)
	// the gear asked for on top of a match's own and still held: what no player wears any more is taken off
	// the lists and let go of (release_unworn)
	std::set<std::string> g_asked_models;

	// The proper way to have gear loaded: the match's server tells the client which gear models to keep
	// loaded, a list for each kind of gear (0x140022290 takes them: lists of 16 bytes each, a count and
	// the numbers, each a model's number within its kind), and the game loads and lets go of models by
	// those lists. A set asked for is added to the lists as they come (the recording gives them again
	// when it is gone back in), so the game holds the set's models itself, with everything else the match
	// needs. A model's number within its kind is found by asking the game for each number's name
	// (0x14000C100, with the kind from 0x1403CC830).
	constexpr std::uintptr_t CL_STREAM_SYNC = 0x140022290, BG_CUSTOMIZATION_KIND = 0x1403CC830, BG_CUSTOMIZATION_MODEL_NAME = 0x14000C100;
	constexpr int SYNC_KINDS = 10, KIND_MOST = 18;  // (a list of a kind holds 18 numbers, as the kind's pool has 18 places)
	constexpr size_t SYNC_SIZE = 0x4A0;
	constexpr int SYNC_LISTS = static_cast<int>(SYNC_SIZE / 16);
	constexpr std::uintptr_t COM_GET_CLIENT_DOBJ = 0x1403D2BA0, COM_SAFE_CLIENT_DOBJ_FREE = 0x1403D2DF0;
	constexpr std::uintptr_t CUSTOM_IN_USE = 0x140B5F0E8, CUSTOM_KINDS = 0x140B33B48, CUSTOM_ENTRIES = 0x140B33B90;

	// the lists as the game last had them from the server, kept whole (their numbers copied): the game's
	// own handling of them is run again with what the editor asks for added, so the asking does not wait
	// for the recording to give the lists again (it may never)
	struct SavedSync
	{
		bool have = false;
		int flag = 0;
		unsigned char bytes[SYNC_SIZE];
		std::vector<std::uint32_t> numbers[SYNC_LISTS];
		bool owned[SYNC_LISTS];
	};
	SavedSync g_saved_sync;
	std::atomic<bool> g_replay_wanted{false};
	using stream_sync_t = void (*)(int, unsigned char*, int);
	stream_sync_t g_original_stream_sync = nullptr;
	std::unordered_map<std::string, std::pair<int, unsigned>> g_numbers;  // a gear model's kind and number
	bool g_numbers_read = false;
	std::vector<std::pair<int, unsigned>> g_extra;  // what is asked for on top of the server's lists
	// ---- a re-dressed player's gear, by name (0.63). The game keeps each character's models as NAMES: a
	// record of 0xCC0 bytes a character (42 players' from 0x141620568; 8 more from 0x141649350 and the
	// bodies' from 0x14164F950), its handle first, whether its models are all loaded next (0x1401B06E0
	// asks for each name), the first model's name at +0x24 and up to 20 more names of 0x40 bytes from
	// +0x64; a character's model is made from the names. So gear a set replaces is written over in the
	// player's record with the set's (the game then makes the player with it by itself, and the bodies it
	// leaves take the record as it is), and the gear the player came with, named by no record any more,
	// can be taken off the lists and out of memory: that is what lets every player wear something else.
	struct Renamed
	{
		std::string original, written;
	};
	std::map<std::pair<std::uintptr_t, int>, Renamed> g_renamed;     // by record and place in it
	std::map<int, std::map<std::string, std::string>> g_unrenamed;  // per handle: a name written, and what was there
	std::set<std::string> g_record_names;                           // every name in the records now
	std::set<std::pair<int, unsigned>> g_dropped;                   // the match's own gear kept off the lists
	bool g_names_off = false;
	// (the list the game last made each entity's model from: see hooked_create)
	struct LastList
	{
		std::vector<unsigned char> bytes;
		unsigned short count = 0;
	};
	std::map<int, LastList> g_last;
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

		// (shinguards are worn where kneepads are)
		if (name.compare(0, 13, "mp_shinguard_") == 0)
		{
			return 6;
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

	// The name the game itself gives a number of a kind (as its loading of the lists reads it, 0x1400237F0:
	// 0x14000C100 with the kind from 0x1403CC830).
	const char* number_name(const int kind, const unsigned number)
	{
		__try
		{
			const int of_kind = reinterpret_cast<int (*)(int)>(BG_CUSTOMIZATION_KIND)(kind);
			const char* name = reinterpret_cast<const char* (*)(int, unsigned)>(BG_CUSTOMIZATION_MODEL_NAME)(of_kind, number);
			return name && !IsBadStringPtrA(name, 96) ? name : nullptr;
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

		// The game's own table of gear models, as its code reads it (0x14000C100, 0x14000C3D0): entries of
		// 0x58 bytes from 0x140B33B90 (the model's name, its lobby model's name, the name it is loaded by),
		// and for each kind of gear where its entries begin and how many there are (two words each, from
		// 0x140B33B48). The lists the game loads gear by hold, for each kind, an entry's place among that
		// kind's, from 1 (0x1400237F0 names a list's number with 0x14000C100(kind, number)).
		// (0.46 to 0.53 asked the game for the name of number 0 of each kind, which is none: nothing was
		// ever found, and nothing was ever asked for. 0.54 put the place in the whole table in the lists:
		// the game found no name for it and crashed at 0x140025470.)
		const int in_use = IsBadReadPtr(reinterpret_cast<const void*>(CUSTOM_IN_USE), 4) ? -1 : *reinterpret_cast<const int*>(CUSTOM_IN_USE);
		std::string layout;
		for (int t = 0; t < 12 && !IsBadReadPtr(reinterpret_cast<const void*>(CUSTOM_KINDS), 48); ++t)
		{
			const unsigned first = *reinterpret_cast<const std::uint16_t*>(CUSTOM_KINDS + t * 4);
			const unsigned count = *reinterpret_cast<const std::uint16_t*>(CUSTOM_KINDS + t * 4 + 2);
			layout += " " + std::to_string(first) + "+" + std::to_string(count);
			if (!count || count > 1500 || first + count > 4000)
			{
				continue;
			}

			for (unsigned i = 0; i < count; ++i)
			{
				const std::uintptr_t entry = CUSTOM_ENTRIES + static_cast<std::uintptr_t>(first + i) * 0x58;
				if (IsBadReadPtr(reinterpret_cast<const void*>(entry), 0x18))
				{
					break;
				}

				for (const int offset : {0, 8})
				{
					const char* name = *reinterpret_cast<const char* const*>(entry + offset);
					if (name && !IsBadStringPtrA(name, 96) && name[0] >= 33 && strnlen(name, 96) < 90)
					{
						if (t >= 1 && t <= SYNC_KINDS)
						{
							g_numbers.emplace(name, std::make_pair(t - 1, i + 1));
						}
					}
				}
			}
		}

		g_numbers_read = !g_numbers.empty();
		note("models: %zu gear models have a place in the game's table (in use %d; kinds' first+count:%s)", g_numbers.size(), in_use, layout.c_str());
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
			// kept, as the server gave them
			g_saved_sync.have = true;
			g_saved_sync.flag = flag;
			memcpy(g_saved_sync.bytes, lists, SYNC_SIZE);
			size_t held = 0;
			for (int i = 0; i < SYNC_LISTS; ++i)
			{
				const unsigned char* list = lists + i * 16;
				const unsigned count = *reinterpret_cast<const std::uint16_t*>(list);
				const auto* values = *reinterpret_cast<const std::uint32_t* const*>(list + 8);
				g_saved_sync.numbers[i].clear();
				g_saved_sync.owned[i] = count == 0 && i < SYNC_KINDS;
				if (count && count <= 4096 && values && !IsBadReadPtr(values, count * 4))
				{
					g_saved_sync.numbers[i].assign(values, values + count);
					g_saved_sync.owned[i] = true;
					held += i < SYNC_KINDS ? count : 0;
				}
			}

			if (g_syncs <= 6)
			{
				note("models: the game was given its lists of gear to load (%u so far; %zu numbers in the ten kinds; flag %d)", g_syncs, held, flag);
			}

			if ((!g_extra.empty() || !g_dropped.empty()) && (state.playing || state.starting))
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
					// (the match's own gear nobody is made with any more stays out)
					for (auto value = numbers[kind].begin(); value != numbers[kind].end();)
					{
						if (g_dropped.count({kind, *value & 0xFFFFFF}))
						{
							value = numbers[kind].erase(value);
							added = true;
						}
						else
						{
							++value;
						}
					}

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

						if (!there && numbers[kind].size() < static_cast<size_t>(KIND_MOST))
						{
							numbers[kind].push_back(extra.second);
							added = true;
						}
					}

					if (added)
					{
						*reinterpret_cast<std::uint16_t*>(list) = static_cast<std::uint16_t>(numbers[kind].size());
						*reinterpret_cast<const std::uint32_t**>(list + 8) = numbers[kind].empty() ? nullptr : numbers[kind].data();
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

	bool call_sync(unsigned char* lists, const int flag)
	{
		__try
		{
			g_original_stream_sync(0, lists, flag);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	// ---- the game's on-demand files (0.61: read right at last).
	// A gear model's file is held by a count: 0x1400259A0 (the model's name) adds one and has the file
	// loaded; 0x140026A20 takes one off, and at none the file is unloaded a couple of frames later and its
	// place handed back. (To 0.60 the two were taken for each other: 0.59's "letting go" added to the
	// counts, and 0.44's "loading" took models away from under the players.) A name's files: 0x140025470
	// (name, 7, places out) gives places in the table at 0x140BDA050 (12 bytes each: the file's number at
	// +6, the count at +8; 0x1400268D0 takes one off a place); a file is 0x30 bytes at 0x140BB8F10 (its
	// pool at +8, its state at +0x2C: 0 not loaded, 1 to 3 on its way in, 4 loaded, 6 and 5 on its way out).
	// The game's handling of the lists (0x140022290) does all of this itself when it is told the lists
	// are CHANGED (its last argument not 0): what is no longer on a list is counted off and the models made
	// with it are let go of (0x1401EECE0), what is new is loaded. Told the lists BEGIN (0, as the server's
	// first ones come), it forgets what it held without counting any of it off and counts everything on:
	// that is what the editor's own handling of the lists did to 0.60, so nothing ever left memory.
	// The room: each kind of gear has a pool of its own (0x78 bytes each at 0x140BB79D0, 13 of them, the
	// name first: mp_char_shirt and so on) with as many places as a match has players at most (18, and the
	// game's own lists hold 18 of a kind): a file takes a free place when it is loaded (the free ones are a
	// chain from the word at +0x68 of the pool through the word at +0xA of each 16-byte place at
	// 0x140BB7FF0) and nothing checks that one is left: that was the "out of memory". So what fits is
	// counted here, kind by kind, from the game's own books.
	constexpr std::uintptr_t TR_FIND = 0x140025470, TR_UNLOAD_ONE = 0x1400268D0, TR_ASSETS = 0x140BDA050, TR_FILES = 0x140BB8F10;
	constexpr std::uintptr_t TR_POOLS = 0x140BB79D0, TR_PLACES = 0x140BB7FF0, TR_PLACE_COUNT = 0x140BB7FEC, CG_MODEL_UNLOADED = 0x1401EECE0;
	constexpr std::uintptr_t STREAM_STORED = 0x140BB1C10, STREAM_ACTIVE = STREAM_STORED + 0x5DB0;
	constexpr int TR_POOL_COUNT = 13, TR_FILE_COUNT = 2700;
	void list_loaded();
	struct TrFile
	{
		std::uintptr_t at;
		int pool, state, count;
	};

	// A name's files, as the game's books have them now.
	int tr_files(const char* name, TrFile* out, const int most)
	{
		__try
		{
			std::uint16_t places[256] = {};
			const int count = reinterpret_cast<int (*)(const char*, int, std::uint16_t*)>(TR_FIND)(name, 7, places);
			int found = 0;
			for (int i = 0; i < count && i < 256 && found < most; ++i)
			{
				const auto* asset = reinterpret_cast<const unsigned char*>(TR_ASSETS + static_cast<std::uintptr_t>(places[i]) * 12);
				const unsigned number = *reinterpret_cast<const std::uint16_t*>(asset + 6);
				if (number >= static_cast<unsigned>(TR_FILE_COUNT))
				{
					continue;
				}

				const auto* file = reinterpret_cast<const unsigned char*>(TR_FILES + static_cast<std::uintptr_t>(number) * 0x30);
				out[found].at = reinterpret_cast<std::uintptr_t>(file);
				out[found].pool = file[8];
				out[found].state = file[0x2C];
				out[found].count = file[0x2D];
				++found;
			}

			return found;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return 0;
		}
	}

	// (on the game's thread) Everything the game counts on a name taken off, and the models made with it
	// let go of the way the game does: how many counts were taken off (-1: a fault).
	int tr_let_go(const char* name)
	{
		__try
		{
			std::uint16_t places[256] = {};
			const int count = reinterpret_cast<int (*)(const char*, int, std::uint16_t*)>(TR_FIND)(name, 7, places);
			int taken = 0;
			for (int i = 0; i < count && i < 256; ++i)
			{
				const auto* asset = reinterpret_cast<const unsigned char*>(TR_ASSETS + static_cast<std::uintptr_t>(places[i]) * 12);
				for (int guard = 0; asset[8] != 0 && guard < 255; ++guard)
				{
					reinterpret_cast<int (*)(std::uint16_t)>(TR_UNLOAD_ONE)(places[i]);
					++taken;
				}
			}

			// (as the game does when a count comes to none: the model is still in memory for a couple of frames)
			if (taken > 0)
			{
				reinterpret_cast<void (*)(const char*, int)>(CG_MODEL_UNLOADED)(name, 0);
			}

			return taken;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return -1;
		}
	}

	struct Room
	{
		int left[TR_POOL_COUNT];  // the free places of each pool, less the files on their way into it
		char names[TR_POOL_COUNT][24];
	};

	bool read_room_raw(Room* room)
	{
		__try
		{
			int coming[TR_POOL_COUNT] = {};
			for (int i = 0; i < TR_FILE_COUNT; ++i)
			{
				const auto* file = reinterpret_cast<const unsigned char*>(TR_FILES + static_cast<std::uintptr_t>(i) * 0x30);
				if (file[0x2C] >= 1 && file[0x2C] <= 3 && file[8] < TR_POOL_COUNT)
				{
					++coming[file[8]];
				}
			}

			const unsigned places = *reinterpret_cast<const std::uint32_t*>(TR_PLACE_COUNT);
			for (int pool = 0; pool < TR_POOL_COUNT; ++pool)
			{
				const auto* at = reinterpret_cast<const unsigned char*>(TR_POOLS + static_cast<std::uintptr_t>(pool) * 0x78);
				const char* name = reinterpret_cast<const char*>(at);
				const char* shown = strncmp(name, "mp_char_", 8) == 0 ? name + 8 : name;
				size_t length = 0;
				for (; length < 23 && shown[length]; ++length)
				{
					room->names[pool][length] = shown[length];
				}

				room->names[pool][length] = 0;
				if (!name[0] || *reinterpret_cast<const std::uint64_t*>(at + 0x70))
				{
					// (no pool here, or one whose files each have a place of their own)
					room->left[pool] = 1000000;
					continue;
				}

				int free_places = 0;
				for (unsigned next = *reinterpret_cast<const std::uint16_t*>(at + 0x68); next != 0xFFFF && next < places && free_places < 512;
				     next = *reinterpret_cast<const std::uint16_t*>(TR_PLACES + static_cast<std::uintptr_t>(next) * 16 + 0xA))
				{
					++free_places;
				}

				room->left[pool] = free_places - coming[pool];
			}

			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	Room read_room()
	{
		Room room = {};
		if (!read_room_raw(&room))
		{
			// (the books could not be read: nothing more is asked for)
			for (int& left : room.left)
			{
				left = 0;
			}
		}

		return room;
	}

	std::string room_text(const Room& room)
	{
		std::string text;
		for (int pool = 0; pool < TR_POOL_COUNT; ++pool)
		{
			if (room.names[pool][0] && room.left[pool] < 1000000)
			{
				text += " " + std::string(room.names[pool]) + " " + std::to_string(room.left[pool]);
			}
		}

		return text;
	}

	bool stream_active()
	{
		__try
		{
			return *reinterpret_cast<const int*>(STREAM_ACTIVE) != 0;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	// (the game's own kept lists of weapons and of first-person models, put into the lists handed to it:
	// the same before and after, so nothing of theirs is loaded or let go of)
	bool copy_stored(unsigned char* lists, const int list)
	{
		__try
		{
			memcpy(lists + list * 16, reinterpret_cast<const void*>(STREAM_STORED + static_cast<std::uintptr_t>(list) * 16), 16);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	// (on the game's thread) The game's handling of its lists run again, with the editor's asked-for gear
	// in them and what the editor lets go of out of them, as a CHANGE of the lists: false if the game has
	// not been given any lists in this session.
	bool replay_sync()
	{
		static unsigned char buffer[SYNC_SIZE];
		static std::vector<std::uint32_t> merged[SYNC_LISTS];
		int flag = 0;
		size_t added = 0, refused = 0;
		{
			std::lock_guard<std::mutex> lock(g_lock);
			if (!g_saved_sync.have)
			{
				return false;
			}

			memcpy(buffer, g_saved_sync.bytes, SYNC_SIZE);
			for (int i = 0; i < SYNC_LISTS; ++i)
			{
				if (!g_saved_sync.owned[i])
				{
					continue;
				}

				merged[i] = g_saved_sync.numbers[i];
				if (i < SYNC_KINDS)
				{
					for (auto value = merged[i].begin(); value != merged[i].end();)
					{
						value = g_dropped.count({i, *value & 0xFFFFFF}) ? merged[i].erase(value) : std::next(value);
					}
				}

				for (const auto& extra : g_extra)
				{
					bool there = extra.first != i;
					for (const std::uint32_t value : merged[i])
					{
						there = there || (value & 0xFFFFFF) == extra.second;
					}

					if (!there && i < SYNC_KINDS && merged[i].size() >= static_cast<size_t>(KIND_MOST))
					{
						// (the game's own list of a kind holds 18, as its pool does)
						++refused;
					}
					else if (!there)
					{
						merged[i].push_back(extra.second);
						++added;
					}
				}

				*reinterpret_cast<std::uint16_t*>(buffer + i * 16) = static_cast<std::uint16_t>(merged[i].size());
				*reinterpret_cast<const std::uint32_t**>(buffer + i * 16 + 8) = merged[i].empty() ? nullptr : merged[i].data();
			}
		}

		// (a change if the game holds lists now; if it holds none it is given them as the server gives them)
		if (stream_active() && copy_stored(buffer, 10) && copy_stored(buffer, 12))
		{
			flag = 1;
		}
		else
		{
			std::lock_guard<std::mutex> lock(g_lock);
			flag = g_saved_sync.flag;
		}

		const bool done = call_sync(buffer, flag);
		note(done ? "models: the game's lists handled again with %zu more gear models in them (as %s; %zu had no place on a list)"
		          : "models: a fault handling the game's lists again (%zu more; as %s; %zu refused)",
		     added, flag ? "a change" : "new lists", refused);
		return done;
	}

	// ---- letting go of gear: what the editor asked for and no player wears any more is taken off the
	// lists (the game counts it off and unloads it), and then whatever the game still counts on it (it is
	// counted on again each time the recording is gone back in) is taken off as well.
	struct Release
	{
		std::vector<std::pair<std::string, std::string>> names;  // the model, and the name the game loads it by
		ULONGLONG due = 0;
	};
	Release g_release;
	std::map<std::string, ULONGLONG> g_let_go;  // models let go of, and when: not listed as loaded while they go

	// (with the lock held) Whether the lists the match's server gave hold a gear model (its kind and number).
	bool server_lists(const std::pair<int, unsigned>& number)
	{
		if (number.first < 0 || number.first >= SYNC_KINDS)
		{
			return false;
		}

		for (const std::uint32_t value : g_saved_sync.numbers[number.first])
		{
			if ((value & 0xFFFFFF) == number.second)
			{
				return true;
			}
		}

		return false;
	}

	bool has_dobj(const int handle)
	{
		__try
		{
			return reinterpret_cast<void* (*)(int, int)>(COM_GET_CLIENT_DOBJ)(handle, 0) != nullptr;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	constexpr std::uintptr_t RECORDS_PLAYERS = 0x141620568, RECORDS_AGENTS = 0x141649350, RECORDS_BODIES = 0x14164F950;
	constexpr int RECORD_SIZE = 0xCC0, NAME_PLACES = 21, NAME_SIZE = 0x40;
	struct RecordCopy
	{
		int handle, valid;
		char names[NAME_PLACES][NAME_SIZE];
	};

	char* name_place(const std::uintptr_t record, const int place)
	{
		return reinterpret_cast<char*>(record + (place == 0 ? 0x24 : 0x64 + static_cast<std::uintptr_t>(place - 1) * NAME_SIZE));
	}

	bool read_record(const std::uintptr_t record, RecordCopy* out)
	{
		__try
		{
			out->handle = *reinterpret_cast<const int*>(record);
			out->valid = *reinterpret_cast<const int*>(record + 4);
			for (int place = 0; place < NAME_PLACES; ++place)
			{
				memcpy(out->names[place], name_place(record, place), NAME_SIZE);
				out->names[place][NAME_SIZE - 1] = 0;
			}

			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	bool write_name(const std::uintptr_t record, const int place, const char* name)
	{
		__try
		{
			char* at = name_place(record, place);
			memset(at, 0, NAME_SIZE);
			strncpy_s(at, NAME_SIZE, name, _TRUNCATE);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	// (a record the game marked as not having its models loaded, because the gear it named had been let go
	// of when the game looked: marked as having them again once every name in it is in memory)
	bool mark_loaded(const std::uintptr_t record)
	{
		__try
		{
			*reinterpret_cast<int*>(record + 4) = 1;
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	bool free_dobj(const int handle)
	{
		__try
		{
			reinterpret_cast<void (*)(int)>(COM_SAFE_CLIENT_DOBJ_FREE)(handle);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	// (on the game's thread, the lock not held) The players' records brought to what the editor's choices
	// say: gear a set replaces written over with the set's, gear no longer replaced written back. A player
	// whose record changed is let go of, and the game makes it again from the record.
	void rename_gear(const TheaterState& state)
	{
		if (!state.playing)
		{
			return;
		}

		static ULONGLONG window = 0;
		static int writes = 0;
		static int told = 0;
		std::vector<int> changed;
		bool back = false;
		{
			std::lock_guard<std::mutex> lock(g_lock);
			if (!g_listed)
			{
				list_loaded();
			}

			g_record_names.clear();
			g_unrenamed.clear();
			const struct
			{
				std::uintptr_t base;
				int count;
				bool players;
			} arrays[] = {{RECORDS_PLAYERS, 42, true}, {RECORDS_AGENTS, 8, false}, {RECORDS_BODIES, 8, false}};
			static RecordCopy copy;
			for (const auto& array : arrays)
			{
				for (int index = 0; index < array.count; ++index)
				{
					const std::uintptr_t record = array.base + static_cast<std::uintptr_t>(index) * RECORD_SIZE;
					if (!read_record(record, &copy) || !copy.names[0][0])
					{
						continue;
					}

					const auto swaps = array.players && !g_names_off ? g_swaps.find(copy.handle) : g_swaps.end();
					bool ours = false, all_loaded = true;
					for (int place = 0; place < NAME_PLACES; ++place)
					{
						std::string now = copy.names[place];
						const std::pair<std::uintptr_t, int> key{record, place};
						auto renamed = g_renamed.find(key);
						// (0.63 found the game writes the records anew a few times a second, from the match's
						// data: a name gone back to the player's own is written over again, without the
						// player being made again; only another name altogether is the game's own change)
						if (renamed != g_renamed.end() && now != renamed->second.written && now != renamed->second.original)
						{
							g_renamed.erase(renamed);
							renamed = g_renamed.end();
						}

						if (now.empty())
						{
							continue;
						}

						const std::string original = renamed != g_renamed.end() ? renamed->second.original : now;
						std::string wanted = original;
						if (swaps != g_swaps.end())
						{
							const auto swap = swaps->second.find(original);
							if (swap != swaps->second.end() && !swap->second.empty() && swap->second != original && swap->second.size() < NAME_SIZE &&
							    g_numbers.count(swap->second) && g_numbers.count(original) && g_loaded.count(swap->second))
							{
								wanted = swap->second;
							}
						}

						bool remake_wanted = false;
						if (wanted == original)
						{
							if (renamed != g_renamed.end())
							{
								if (g_loaded.count(original))
								{
									// (back to the player's own gear, which is in memory)
									g_renamed.erase(renamed);
									renamed = g_renamed.end();
									remake_wanted = true;
								}
								else
								{
									// (it was let go of: asked for again first, and what is worn stays till then)
									const auto number = g_numbers.find(original);
									if (number != g_numbers.end() && g_dropped.erase(number->second))
									{
										back = true;
									}

									wanted = renamed->second.written;
								}
							}
						}
						else
						{
							remake_wanted = renamed == g_renamed.end() || renamed->second.written != wanted;
							g_renamed[key] = {original, wanted};
						}

						if (wanted != now && write_name(record, place, wanted.c_str()))
						{
							if (told < 60 && remake_wanted)
							{
								++told;
								note("models: entity %d's record: %s written over with %s", copy.handle, now.c_str(), wanted.c_str());
							}

							now = wanted;
						}

						if (remake_wanted)
						{
							changed.push_back(copy.handle);
							++writes;
						}

						g_record_names.insert(now);
						all_loaded = all_loaded && g_loaded.count(now) != 0;
						if (array.players && now != original)
						{
							g_unrenamed[copy.handle][now] = original;
							ours = true;
						}
					}

					if (ours && !copy.valid && all_loaded && mark_loaded(record))
					{
						changed.push_back(copy.handle);
						if (told < 60)
						{
							++told;
							note("models: entity %d's record was marked as lacking its models: marked as having them (every name in it is in memory)", copy.handle);
						}
					}
				}
			}

			// (if the game keeps writing the records back, this is not the way: the players would flicker)
			const ULONGLONG now = GetTickCount64();
			if (now - window > 3000)
			{
				window = now;
				writes = 0;
			}
			else if (writes > 300 && !g_names_off)
			{
				// (players being made again over and over: not the way; the players' own gear is asked for back)
				g_names_off = true;
				back = back || !g_dropped.empty();
				g_dropped.clear();
				note("models: the players are being made again over and over (%d times in 3 s): gear is no longer changed by name", writes);
			}
		}

		for (const int handle : changed)
		{
			free_dobj(handle);
		}

		if (back)
		{
			g_replay_wanted = true;
		}
	}

	// (with the lock held) Whether a model that is in memory is in the list a character that is there now was made from.
	bool in_a_made_model(const std::string& name)
	{
		const auto loaded = g_loaded.find(name);
		if (loaded == g_loaded.end())
		{
			return false;
		}

		for (const auto& last : g_last)
		{
			const unsigned char* bytes = last.second.bytes.data();
			for (unsigned short i = 0; i < last.second.count && (i + 1u) * 0x20 <= last.second.bytes.size(); ++i)
			{
				if (*reinterpret_cast<void* const*>(bytes + i * 0x20) == loaded->second && has_dobj(last.first))
				{
					return true;
				}
			}
		}

		return false;
	}

	// (on the game's thread, the lock not held) Two steps, a little apart: the gear no longer worn is taken
	// off the lists, then everything still counted on it is taken off.
	void release_unworn(const TheaterState& state)
	{
		if (!state.playing)
		{
			return;
		}

		const ULONGLONG now = GetTickCount64();
		std::vector<std::pair<std::string, std::string>> names;
		{
			std::lock_guard<std::mutex> lock(g_lock);
			if (g_release.due && now >= g_release.due)
			{
				names.swap(g_release.names);
				g_release.due = 0;
			}
			else if (!g_release.due && g_extra_dirty && now - g_extra_dirty > 2000 && g_pending.empty())
			{
				// (not while a player whose choices changed still has the model made before them)
				for (const auto& changed : g_changed)
				{
					const auto built = g_built.find(changed.first);
					if (changed.first < 2048 && (built == g_built.end() || built->second != changed.second) && has_dobj(changed.first))
					{
						return;
					}
				}

				g_extra_dirty = 0;
				std::set<std::string> worn;
				for (const auto& entity : g_swaps)
				{
					for (const auto& swap : entity.second)
					{
						worn.insert(swap.second);
					}
				}

				for (const auto& entity : g_adds)
				{
					worn.insert(entity.second.begin(), entity.second.end());
				}

				// (what a record names is what the game makes its character with)
				worn.insert(g_record_names.begin(), g_record_names.end());
				// the match's own gear that the editor wrote over in every record that named it: nothing is
				// made with it any more, and its place can go to something else
				for (const auto& renamed : g_renamed)
				{
					const std::string& own = renamed.second.original;
					const auto number = g_numbers.find(own);
					const char* named = number != g_numbers.end() ? number_name(number->second.first, number->second.second) : nullptr;
					if (!named || worn.count(own) || !g_loaded.count(own) || g_dropped.count(number->second) || !server_lists(number->second) ||
					    in_a_made_model(own))
					{
						continue;
					}

					g_dropped.insert(number->second);
					g_release.names.emplace_back(own, named);
				}

				for (auto asked = g_asked_models.begin(); asked != g_asked_models.end();)
				{
					const auto number = g_numbers.find(*asked);
					const char* named = number != g_numbers.end() ? number_name(number->second.first, number->second.second) : nullptr;
					if (worn.count(*asked) || !named)
					{
						++asked;
						continue;
					}

					// (what the match's own lists have come to hold as well is the game's to keep)
					if (!server_lists(number->second) || g_dropped.count(number->second))
					{
						g_release.names.emplace_back(*asked, named);
					}

					g_extra.erase(std::remove(g_extra.begin(), g_extra.end(), number->second), g_extra.end());
					asked = g_asked_models.erase(asked);
				}

				if (!g_release.names.empty())
				{
					g_release.due = now + 500;
					g_replay_wanted = true;
				}
			}
		}

		if (names.empty())
		{
			return;
		}

		// (anything asked for again in the meantime stays)
		std::string told;
		int counts = 0, faults = 0;
		size_t done = 0;
		for (const auto& name : names)
		{
			{
				std::lock_guard<std::mutex> lock(g_lock);
				const auto number = g_numbers.find(name.first);
				if (g_asked_models.count(name.first) || number == g_numbers.end() || (server_lists(number->second) && !g_dropped.count(number->second)) ||
				    std::find(g_extra.begin(), g_extra.end(), number->second) != g_extra.end())
				{
					continue;
				}
			}

			const int taken = tr_let_go(name.second.c_str());
			faults += taken < 0 ? 1 : 0;
			counts += taken > 0 ? taken : 0;
			++done;
			if (told.size() < 300)
			{
				told += " " + name.first;
			}

			std::lock_guard<std::mutex> lock(g_lock);
			g_let_go[name.first] = now;
			g_loaded.erase(name.first);
		}

		const Room room = read_room();
		std::lock_guard<std::mutex> lock(g_lock);
		g_listed = false;
		g_relist_at = now + 2500;
		note("models: %zu gear models no longer worn let go of (%d counts the game still had on them taken off, %d faults):%s", done, counts, faults, told.c_str());
		note("models: free places by kind now:%s", room_text(room).c_str());
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
				for (size_t c = 10; c < row.size() && c < 18; ++c)
				{
					if (!row[c].empty())
					{
						model.groups.push_back(row[c]);
					}
				}

				g_model_rows.push_back(model);
			}
		}

		for (const auto& row : read_table("costumeidtable"))
		{
			if (row.size() >= 8)
			{
				IdRow item;
				item.category = row[0];
				item.primary = atoi(row[4].c_str());
				item.id = static_cast<unsigned>(strtoul(row[7].c_str(), nullptr, 16));
				for (size_t c = 9; c < row.size() && c < 13; ++c)
				{
					// (words: the column before them is a number)
					if (!row[c].empty() && !isdigit(static_cast<unsigned char>(row[c][0])))
					{
						item.tags.push_back(row[c]);
					}
				}

				g_id_rows.push_back(item);
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
		const ULONGLONG now = GetTickCount64();
		for (int i = 0; i < count; ++i)
		{
			if (const char* name = name_of(found[i]))
			{
				// (a model let go of is in memory a little longer: it is not one to draw with)
				const auto gone = g_let_go.find(name);
				if (gone != g_let_go.end() && now - gone->second < 3000)
				{
					continue;
				}

				g_loaded[name] = found[i];
			}
		}

		for (auto gone = g_let_go.begin(); gone != g_let_go.end();)
		{
			gone = now - gone->second >= 3000 ? g_let_go.erase(gone) : std::next(gone);
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
	constexpr std::uintptr_t XMODEL_MATERIAL_COUNT = 0x1404F8070, XMODEL_MATERIALS = 0x1404F8050;
	void (*g_original_overrides)(void*, unsigned char*, int) = nullptr;
	thread_local void* t_made = nullptr;
	thread_local unsigned char t_list[MAX_IN_LIST * 0x20];
	thread_local int t_count = 0;

	// the list the game last made each entity's model from, as the game gave it: the model is made again
	// from it when the editor's choices change (going back in the recording did not always make the game
	// do so: a player whose gear is the same before and after is not made again)

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

			// (gear the editor wrote into the player's record goes by the name that was there: the editor's
			// choices are kept by the player's own gear)
			if (const auto renamed = g_unrenamed.find(handle); renamed != g_unrenamed.end())
			{
				for (std::string& name : names)
				{
					const auto was = renamed->second.find(name);
					if (was != renamed->second.end())
					{
						name = was->second;
					}
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
	g_extra_dirty = GetTickCount64();
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
	g_extra_dirty = GetTickCount64();
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
	g_extra_dirty = GetTickCount64();
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
		bool whole = false;
		if (group == 4 && name.compare(0, slot == 1 ? 3 : 4, slot == 1 ? "vm_" : "npc_") == 0)
		{
			// a gun that comes whole: a model made of several (its barrel, stock and the rest), as the loot
			// variants are; a bare "_base" is the receiver alone, the game adding the rest part by part
			const auto loaded = g_loaded.find(name);
			const auto* bytes = loaded != g_loaded.end() ? static_cast<const unsigned char*>(loaded->second) : nullptr;
			const bool several = bytes && !IsBadReadPtr(bytes + 0x200, 8) && (*reinterpret_cast<const std::uint16_t*>(bytes + 0x204) & 0x400) != 0 && bytes[0x202] > 1;
			whole = several || name.find("_loot") != std::string::npos;
		}

		const bool in_group = group == 0 ? part >= 0 && (slot < 0 || part == slot) && !ends_with(name, "_lob")
			: group == 1 ? name.compare(0, 4, "npc_") == 0
			: group == 2 ? first_person
			: group == 4 ? whole
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
namespace
{
// (with the lock held) The parts a set is for an entity, by the game's own matching: false if none.
bool build_set(const int entity, const char* name, PendingSet& pending, const bool quiet)
{
	read_gear_tables();
	const auto seen = g_seen.find(entity);
	if (seen == g_seen.end())
	{
		return false;
	}

	if (!g_listed)
	{
		list_loaded();
	}

	// The game's tables give an item of gear several models, and say which goes with what else is worn: a
	// hat has one for each face, trousers one for kneepads, one for shinguards and one for neither, most
	// one for men and one for women. Each item has tags (a face's name and skin, "female", "kneepads",
	// "fullmask"...), and each model the tags it needs or must not have ("!female"). So: the tags of
	// what the player will be wearing, then for each item the model whose needs they meet.
	const SetRow* set = nullptr;
	for (const SetRow& each : g_set_rows)
	{
		if (each.name == name)
		{
			set = &each;
		}
	}

	if (!set)
	{
		return false;
	}

	auto item_of = [](const std::string& category, const int primary) -> const IdRow* {
		for (const IdRow& item : g_id_rows)
		{
			if (item.category == category && item.primary == primary)
			{
				return &item;
			}
		}

		return nullptr;
	};
	auto row_of = [](const std::string& model) -> const ModelRow* {
		for (const ModelRow& row : g_model_rows)
		{
			if (row.model == model || (!row.lob.empty() && row.lob == model))
			{
				return &row;
			}
		}

		return nullptr;
	};

	// the set's items (a skin's number taken off: bits 12 to 19)
	struct Item
	{
		unsigned id;
		const IdRow* row;
	};
	std::vector<Item> items;
	std::set<std::string> in_set;
	for (const unsigned id : set->ids)
	{
		for (const IdRow& item : g_id_rows)
		{
			if (item.id == (id & 0xFFF00FFFu))
			{
				items.push_back({id, &item});
				in_set.insert(item.category);
			}
		}
	}

	// what the player wears now: its tags stay for the kinds of gear the set has nothing of
	std::set<std::string> tags;
	bool female = false, lob = false;
	const ModelRow* head_now = nullptr;
	std::string head_name;
	for (const std::string& own : seen->second)
	{
		female = female || own.find("_f_") != std::string::npos;
		lob = lob || ends_with(own, "_lob");
		const ModelRow* row = row_of(own);
		if (!row)
		{
			continue;
		}

		if (row->category == "head")
		{
			head_now = row;
			head_name = own;
		}

		if (row->category == "head" || !in_set.count(row->category))
		{
			if (const IdRow* item = item_of(row->category, row->primary))
			{
				tags.insert(item->tags.begin(), item->tags.end());
			}
		}
	}

	female = female || tags.count("female") != 0;
	if (female)
	{
		tags.insert("female");
	}

	bool hat_in_set = false, full_mask = false;
	for (const Item& item : items)
	{
		tags.insert(item.row->tags.begin(), item.row->tags.end());
		if (item.row->category == "hat")
		{
			hat_in_set = true;
			full_mask = std::find(item.row->tags.begin(), item.row->tags.end(), "fullmask") != item.row->tags.end();
		}
	}

	pending.entity = entity;
	pending.name = name;
	pending.since = GetTickCount64();

	// the face: a hat that is not a whole mask sits on one, and a player in a whole mask has none
	const ModelRow* head_added = nullptr;
	if (!head_now && hat_in_set && !full_mask)
	{
		// (a face of the player's kind the game has in memory, if there is one)
		const IdRow* chosen = nullptr;
		for (const IdRow& item : g_id_rows)
		{
			if (item.category != "head" || (std::find(item.tags.begin(), item.tags.end(), "female") != item.tags.end()) != female)
			{
				continue;
			}

			for (const ModelRow& row : g_model_rows)
			{
				const bool better = !chosen || ((!head_added || !g_loaded.count(head_added->model)) && g_loaded.count(row.model) != 0);
				if (row.category == "head" && row.primary == item.primary && better)
				{
					chosen = &item;
					head_added = &row;
				}
			}
		}

		if (chosen && head_added)
		{
			tags.insert(chosen->tags.begin(), chosen->tags.end());
			Wanted face;
			face.slot = SLOT_HEAD;
			face.model = lob && !head_added->lob.empty() ? head_added->lob : head_added->model;
			pending.parts.push_back(face);
		}
	}
	else if (head_now && full_mask)
	{
		// (left out: the mask is the whole head)
		Wanted face;
		face.slot = SLOT_HEAD;
		pending.parts.push_back(face);
	}

	auto fits = [&tags](const ModelRow& row) {
		for (const std::string& group : row.groups)
		{
			const bool not_this = group[0] == '!';
			if ((tags.count(not_this ? group.substr(1) : group) != 0) == not_this)
			{
				return false;
			}
		}

		return true;
	};

	std::string all_tags;
	for (const std::string& tag : tags)
	{
		all_tags += " " + tag;
	}

	std::string chosen_models;
	for (const Item& item : items)
	{
		const ModelRow* row = nullptr;
		const ModelRow* any = nullptr;
		for (const ModelRow& model : g_model_rows)
		{
			if (model.category != item.row->category || model.primary != item.row->primary)
			{
				continue;
			}

			any = any ? any : &model;
			if (!row && fits(model))
			{
				row = &model;
			}
		}

		// (none fits: the first there is, better than nothing)
		row = row ? row : any;
		if (!row)
		{
			continue;
		}

		Wanted wanted;
		if ((item.id >> 12) & 0xFF)
		{
			// a skin: a material over this model's own
			for (const OverrideRow& over : g_override_rows)
			{
				if (over.id == item.id && (wanted.skin.over.empty() || over.model == row->model))
				{
					wanted.skin.over = over.material;
				}
			}
		}

		wanted.model = lob && !row->lob.empty() ? row->lob : row->model;
		wanted.skin.base = row->material;
		wanted.slot = slot_of(row->model);
		chosen_models += " " + wanted.model;
		if (wanted.slot >= 0 && wanted.slot != SLOT_HEAD)
		{
			pending.parts.push_back(wanted);
		}
	}

	if (!quiet)
	{
		note("models: set %s for entity %d (tags:%s):%s", name, entity, all_tags.c_str(), chosen_models.c_str());
	}

	return !pending.parts.empty();
}

// (with the lock held) What loading a model would take, added to `need` (a place in a pool for each of its
// files that is not in memory or on its way; `files` are those counted already): false if the game has
// no file to load it from.
bool add_need(const std::string& model, std::set<std::uintptr_t>& files, int* need)
{
	const auto number = g_numbers.find(model);
	const char* named = number != g_numbers.end() ? number_name(number->second.first, number->second.second) : nullptr;
	TrFile found[16];
	const int count = named ? tr_files(named, found, 16) : 0;
	for (int i = 0; i < count; ++i)
	{
		// (on its way in, or loaded: it has, or is counted as having, its place)
		const bool placed = found[i].state >= 1 && found[i].state <= 4;
		if (!placed && found[i].pool < TR_POOL_COUNT && files.insert(found[i].at).second)
		{
			++need[found[i].pool];
		}
	}

	return count > 0;
}

// (with the lock held) The same for a set's parts that are not in memory: false if a part cannot be loaded.
bool set_need(const PendingSet& pending, std::set<std::uintptr_t>& files, int* need)
{
	bool loadable = true;
	for (const Wanted& part : pending.parts)
	{
		if (!part.model.empty() && !g_loaded.count(part.model))
		{
			loadable = add_need(part.model, files, need) && loadable;
		}
	}

	return loadable;
}

// (with the lock held) What the sets being waited for will take.
void planned_need(std::set<std::uintptr_t>& files, int* need)
{
	for (const PendingSet& pending : g_pending)
	{
		set_need(pending, files, need);
	}
}

// The kinds of gear there is no free place left for ("": it fits).
std::string kinds_full(const Room& room, const int* need, const int* before)
{
	std::string full;
	for (int pool = 0; pool < TR_POOL_COUNT; ++pool)
	{
		if (need[pool] > before[pool] && need[pool] > room.left[pool])
		{
			full += (full.empty() ? "" : ", ") + std::string(room.names[pool]);
		}
	}

	return full;
}
}

int models_apply_set(const int entity, const char* name)
{
	std::lock_guard<std::mutex> lock(g_lock);
	PendingSet pending;
	if (!build_set(entity, name, pending, false))
	{
		snprintf(g_status, sizeof(g_status), "The set %s has no parts the game's tables give a model for.", name);
		return 0;
	}

	if (!g_listed)
	{
		list_loaded();
	}

	read_numbers();
	list_loaded();
	std::set<std::uintptr_t> files;
	int need[TR_POOL_COUNT] = {}, before[TR_POOL_COUNT] = {};
	planned_need(files, need);
	memcpy(before, need, sizeof(before));
	set_need(pending, files, need);
	const Room room = read_room();
	const std::string full = kinds_full(room, need, before);
	if (!full.empty() && g_allow_loading)
	{
		note("models: no room for set %s (%s); free places by kind:%s", name, full.c_str(), room_text(room).c_str());
		snprintf(g_status, sizeof(g_status),
		         "No room for %s yet: the game holds %d of each kind of gear at most, and no place is free for its %s. Unworn gear is let go of a few seconds after a change: try again then.", name,
		         KIND_MOST, full.c_str());
		return 0;
	}

	snprintf(g_status, sizeof(g_status), "Loading %s: %zu parts...", name, pending.parts.size());
	const int count = static_cast<int>(pending.parts.size());
	g_pending.push_back(std::move(pending));
	return count;
}

// A set chosen at random for each of the entities (a bit each), within what the game's memory has room
// for: once that is used up, the rest get sets made of parts that are already there.
namespace
{
	int randomize(const std::uint64_t entities, const int one);
}

int models_randomize(const std::uint64_t entities)
{
	return randomize(entities, -1);
}

// The same for one entity of any number (an actor's).
int models_randomize_one(const int entity)
{
	return randomize(0, entity);
}

namespace
{
int randomize(const std::uint64_t entities, const int one)
{
	std::lock_guard<std::mutex> lock(g_lock);
	read_gear_tables();
	if (!g_listed)
	{
		list_loaded();
	}

	read_numbers();
	static std::mt19937 random(static_cast<unsigned>(GetTickCount64()));
	list_loaded();
	std::set<std::uintptr_t> files;
	int need[TR_POOL_COUNT] = {};
	planned_need(files, need);
	const Room room = read_room();
	std::vector<int> order;
	for (int entity = 0; entity < 64; ++entity)
	{
		if ((entities & (1ull << entity)) && g_seen.count(entity))
		{
			order.push_back(entity);
		}
	}

	if (one >= 0 && g_seen.count(one))
	{
		order.push_back(one);
	}

	std::shuffle(order.begin(), order.end(), random);
	int queued = 0, fresh = 0;
	for (const int entity : order)
	{
		std::vector<PendingSet> can;
		std::vector<int> costs;
		for (const SetRow& set : g_set_rows)
		{
			PendingSet pending;
			if (!build_set(entity, set.name.c_str(), pending, true) || pending.parts.size() < 4)
			{
				continue;
			}

			// (what this set would take on top of those chosen so far)
			std::set<std::uintptr_t> with = files;
			int then[TR_POOL_COUNT];
			memcpy(then, need, sizeof(then));
			const bool loadable = set_need(pending, with, then);
			int cost = 0;
			for (int pool = 0; pool < TR_POOL_COUNT; ++pool)
			{
				cost += then[pool] - need[pool];
			}

			if (loadable && (cost == 0 || g_allow_loading) && kinds_full(room, then, need).empty())
			{
				can.push_back(std::move(pending));
				costs.push_back(cost);
			}
		}

		if (can.empty())
		{
			continue;
		}

		const size_t pick = random() % can.size();
		set_need(can[pick], files, need);
		fresh += costs[pick];
		note("models: at random, %s for entity %d (%d parts to load)", can[pick].name.c_str(), entity, costs[pick]);
		can[pick].since = GetTickCount64();
		g_pending.push_back(std::move(can[pick]));
		++queued;
	}

	note("models: %d random outfits, %d parts to load; free places by kind before:%s", queued, fresh, room_text(room).c_str());
	snprintf(g_status, sizeof(g_status), queued ? "%d random outfits on their way (%d parts to load)..." : "No set fits these players from what is loaded.", queued, fresh);
	return queued;
}
}

namespace
{
	void* make_guarded(const unsigned char* bytes, const unsigned short count, void* tree, const int handle)
	{
		__try
		{
			return reinterpret_cast<void* (*)(const unsigned char*, unsigned short, void*, int, int, int)>(CG_MAKE_DOBJ)(bytes, count, tree, handle, 0, handle);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return nullptr;
		}
	}
}

// ---- actors (actors.cpp): a model made by the editor, of an outfit taken from a player's.
// What the game last made an entity's model from, kept by name: the models and materials are found again
// by their names when the outfit is used (gear nobody wears is let go of, and a pointer kept would go bad).
bool models_outfit_of(const int entity, ModelsOutfit* out)
{
	std::lock_guard<std::mutex> lock(g_lock);
	const auto last = g_last.find(entity);
	if (last == g_last.end() || last->second.count == 0 || last->second.count > 32 || last->second.bytes.size() < last->second.count * size_t(0x20))
	{
		return false;
	}

	*out = ModelsOutfit();
	for (unsigned short i = 0; i < last->second.count; ++i)
	{
		const unsigned char* entry = last->second.bytes.data() + i * size_t(0x20);
		const void* const* fields = reinterpret_cast<const void* const*>(entry);
		const char* name = name_of(fields[0]);
		if (!name)
		{
			if (i == 0)
			{
				return false;
			}

			continue;
		}

		const int at = out->count++;
		memcpy(out->entries[at], entry, 0x20);
		strncpy_s(out->names[at], name, _TRUNCATE);
		const char* over = name_of(fields[2]);
		const char* base = name_of(fields[3]);
		strncpy_s(out->over[at], over ? over : "", _TRUNCATE);
		strncpy_s(out->base[at], base ? base : "", _TRUNCATE);
	}

	return out->count > 0;
}

// (on the game's thread) An actor's model made from an outfit, with an animation tree of the editor's, under a
// handle of its own. The game's own maker is used, so the editor's changes for that handle (other parts, sets,
// camouflage) go on it like on a player's. Null: a part of it is not in memory (what is missing is named).
void* models_make_actor(const int handle, const ModelsOutfit& outfit, void* tree, char missing[64])
{
	unsigned char bytes[32 * 0x20];
	unsigned short count = 0;
	missing[0] = 0;
	{
		std::lock_guard<std::mutex> lock(g_lock);
		list_loaded();
		if (GetTickCount64() - g_materials_at > 3000)
		{
			list_materials();
		}

		for (int i = 0; i < outfit.count && i < 32; ++i)
		{
			const auto loaded = g_loaded.find(outfit.names[i]);
			if (loaded == g_loaded.end() || !name_of(loaded->second))
			{
				if (!missing[0])
				{
					strncpy_s(missing, 64, outfit.names[i], _TRUNCATE);
				}

				if (i == 0)
				{
					// (the body: the rest hang on it)
					return nullptr;
				}

				continue;
			}

			unsigned char* entry = bytes + count * size_t(0x20);
			memcpy(entry, outfit.entries[i], 0x20);
			void** fields = reinterpret_cast<void**>(entry);
			fields[0] = loaded->second;
			const auto over = outfit.over[i][0] ? g_materials.find(outfit.over[i]) : g_materials.end();
			const auto base = outfit.base[i][0] ? g_materials.find(outfit.base[i]) : g_materials.end();
			fields[2] = over != g_materials.end() ? over->second : nullptr;
			fields[3] = base != g_materials.end() ? base->second : nullptr;
			if (!fields[2] || !fields[3])
			{
				fields[2] = nullptr;
				entry[0xD] = 0;
			}

			++count;
		}
	}

	if (count == 0)
	{
		return nullptr;
	}

	void* made = make_guarded(bytes, count, tree, handle);
	if (!made)
	{
		strncpy_s(missing, 64, "(a fault making it)", _TRUNCATE);
	}

	return made;
}

// An actor is gone: what the editor kept for its handle goes with it.
void models_drop(const int entity)
{
	std::lock_guard<std::mutex> lock(g_lock);
	g_extra_dirty = GetTickCount64();
	g_swaps.erase(entity);
	g_adds.erase(entity);
	g_skins.erase(entity);
	g_camos.erase(entity);
	g_seen.erase(entity);
	g_last.erase(entity);
	g_built.erase(entity);
	g_changed.erase(entity);
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
	if (g_replay_wanted.exchange(false) && !replay_sync())
	{
		// (no lists yet in this session: they are given when the recording is gone back in)
		theater_log("models: the game has not been given its lists yet; going back in the recording for them");
		theater_rebuild();
	}

	rename_gear(state);
	remake_changed(state);
	release_unworn(state);
	std::lock_guard<std::mutex> lock(g_lock);
	if (g_relist_at && GetTickCount64() > g_relist_at)
	{
		g_relist_at = 0;
		list_loaded();
		note("models: after letting go, free places by kind:%s", room_text(read_room()).c_str());
	}

	if (!state.playing && !state.starting)
	{
		// (the match is left: what was asked for goes with its map)
		g_pending.clear();
		g_extra.clear();
		g_asked_models.clear();
		g_release.names.clear();
		g_release.due = 0;
		g_dropped.clear();
		g_renamed.clear();
		g_unrenamed.clear();
		g_record_names.clear();
		g_names_off = false;
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
				if (number != g_numbers.end() && g_loaded.count(part.model) && g_asked_models.count(part.model))
				{
					// (asked for before and still in memory: it stays on the lists while it is worn)
					if (std::find(g_extra.begin(), g_extra.end(), number->second) == g_extra.end())
					{
						g_extra.push_back(number->second);
						++asked;
					}
				}
				else if (!g_loaded.count(part.model) && number != g_numbers.end())
				{
					// (only what the game itself names the same way: a number it has no name for stops it)
					const char* named = number_name(number->second.first, number->second.second);
					const std::string base = ends_with(part.model, "_lob") ? part.model.substr(0, part.model.size() - 4) : part.model;
					if (named && (part.model == named || base == named || base + "_lob" == named))
					{
						if (std::find(g_extra.begin(), g_extra.end(), number->second) == g_extra.end())
						{
							g_extra.push_back(number->second);
						}

						g_asked_models.insert(part.model);
						++asked;
					}
					else
					{
						note("models: %s is number %u of kind %d, which the game names %s: not asked for", part.model.c_str(), number->second.second,
						     number->second.first, named ? named : "nothing");
					}
				}
			}

			note("models: set %s: %zu of its parts not in memory asked for from the game's files", pending.name.c_str(), asked);
			if (asked)
			{
				// (the lists are handled again on the next frame: see models_game_frame)
				pending.since = now + 1500;
				g_replay_wanted = true;
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
		there += part.model.empty() ? 1 : g_loaded.count(part.model);
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
	std::string missing, no_skin;
	for (const Wanted& part : pending.parts)
	{
		if (!part.model.empty() && !g_loaded.count(part.model))
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

		if (part.model.empty())
		{
			// (a part left out: the face under a whole mask)
			if (!own.empty())
			{
				g_swaps[entity][own] = "";
			}

			continue;
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
		if (!part.skin.over.empty() && !g_materials.count(part.skin.over))
		{
			// (the part is there in its plain colours: its skin's material is not in memory)
			no_skin += (no_skin.empty() ? "" : ", ") + part.skin.over;
		}
	}

	if (!no_skin.empty())
	{
		note("models: set %s: skins not in memory: %s", pending.name.c_str(), no_skin.c_str());
	}

	snprintf(g_status, sizeof(g_status),
	         missing.empty() ? "%s is on: %zu parts." : g_allow_loading ? "%s: %zu parts are on; the game did not load %s"
	                                                                    : "%s: %zu parts are on; not in this match's memory: %s",
	         pending.name.c_str(), there, missing.c_str());
	note("models: set %s on entity %d: %zu of %zu parts%s%s", pending.name.c_str(), entity, there, pending.parts.size(), missing.empty() ? "" : "; not loaded: ",
	     missing.c_str());
	// (the player's model is made again from here on its own: see remake_changed)
	g_extra_dirty = GetTickCount64();
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
