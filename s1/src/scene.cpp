// S1iris: the scene. What the editor has put into a recording (its actors and props, effects, lights, the camera's
// path, the deaths chosen, the storm) taken as one block of bytes, so that it can be
//  - kept beside the recording (theater\<recording>.scene) and put back when the recording is played again,
//  - gone back to (undo) and forward again (redo): the block is taken whenever nothing is being dragged, and when
//    it differs from the last one, the last one goes on the undo list.
// The block is sections: a tag, the size of one item, how many, the items. An item read from a file made by
// another version is taken as far as both know it (new fields are only ever added at a struct's end).
#include "theater.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <windows.h>

void theater_log(const char* text);

namespace
{
	constexpr std::uint32_t MAGIC = 0x43533153;  // "S1SC"
	constexpr std::uint32_t ACTR = 0x52544341, OUTF = 0x4654554F, AMOD = 0x444F4D41, EFFX = 0x58464645, LITE = 0x4554494C, CAMP = 0x504D4143,
	                        DETH = 0x48544544, STRM = 0x4D525453;
	constexpr int LIGHTS_MAX = 32;

	std::mutex g_storm_lock;
	StormState g_storm;

	void put(std::string& out, const std::uint32_t tag, const std::uint32_t each, const std::uint32_t count, const void* bytes)
	{
		const std::uint32_t head[3] = {tag, each, count};
		out.append(reinterpret_cast<const char*>(head), sizeof(head));
		if (each && count)
		{
			out.append(static_cast<const char*>(bytes), static_cast<size_t>(each) * count);
		}
	}

	std::string pack()
	{
		std::string out;
		const std::uint32_t head[2] = {MAGIC, 1};
		out.append(reinterpret_cast<const char*>(head), sizeof(head));
		static Actor actors[ACTORS_MAX];
		static ModelsOutfit outfits[ACTORS_MAX];
		const int actor_count = actors_export(actors, outfits, ACTORS_MAX);
		put(out, ACTR, sizeof(Actor), static_cast<std::uint32_t>(actor_count), actors);
		put(out, OUTF, sizeof(ModelsOutfit), static_cast<std::uint32_t>(actor_count), outfits);
		// the editor's changes to each actor's outfit: "#handle", then its lines
		std::string changes;
		static char text[16384];
		for (int i = 0; i < actor_count; ++i)
		{
			if (models_export(actors[i].handle, text, sizeof(text)) > 0)
			{
				changes += "#" + std::to_string(actors[i].handle) + "\n" + text;
			}
		}

		put(out, AMOD, 1, static_cast<std::uint32_t>(changes.size()), changes.data());
		static PlacedEffect effects[EFFECTS_MAX];
		const int effect_count = effects_placed(effects, EFFECTS_MAX);
		put(out, EFFX, sizeof(PlacedEffect), static_cast<std::uint32_t>(effect_count), effects);
		static SceneLight lights[LIGHTS_MAX];
		const int light_count = scene_lights(lights, LIGHTS_MAX);
		put(out, LITE, sizeof(SceneLight), static_cast<std::uint32_t>(light_count), lights);
		static unsigned char path[65536];
		const int path_bytes = camera_points_export(path, sizeof(path));
		put(out, CAMP, 1, static_cast<std::uint32_t>(path_bytes), path);
		const int death_bytes = deaths_export(text, sizeof(text));
		put(out, DETH, 1, static_cast<std::uint32_t>(death_bytes), text);
		const StormState storm = storm_state();
		put(out, STRM, sizeof(StormState), 1, &storm);
		return out;
	}

	// The items of a section, each as far as this version and the block's both know it.
	template <typename T> int items(const char* bytes, const std::uint32_t each, const std::uint32_t count, T* out, const int max)
	{
		const int taken = static_cast<int>(count) < max ? static_cast<int>(count) : max;
		for (int i = 0; i < taken; ++i)
		{
			out[i] = T();
			memcpy(&out[i], bytes + static_cast<size_t>(i) * each, each < sizeof(T) ? each : sizeof(T));
		}

		return taken;
	}

	void unpack(const std::string& block)
	{
		if (block.size() < 8 || memcmp(block.data(), &MAGIC, 4) != 0)
		{
			return;
		}

		static Actor actors[ACTORS_MAX];
		static ModelsOutfit outfits[ACTORS_MAX];
		static PlacedEffect effects[EFFECTS_MAX];
		static SceneLight lights[LIGHTS_MAX];
		int actor_count = 0, outfit_count = 0, effect_count = 0, light_count = 0;
		std::string changes, deaths;
		bool have_deaths = false;
		for (size_t at = 8; at + 12 <= block.size();)
		{
			std::uint32_t head[3];
			memcpy(head, block.data() + at, sizeof(head));
			at += 12;
			const size_t bytes = static_cast<size_t>(head[1]) * head[2];
			if (bytes > block.size() - at)
			{
				break;
			}

			const char* data = block.data() + at;
			at += bytes;
			switch (head[0])
			{
			case ACTR: actor_count = items(data, head[1], head[2], actors, ACTORS_MAX); break;
			case OUTF: outfit_count = items(data, head[1], head[2], outfits, ACTORS_MAX); break;
			case AMOD: changes.assign(data, bytes); break;
			case EFFX: effect_count = items(data, head[1], head[2], effects, EFFECTS_MAX); break;
			case LITE: light_count = items(data, head[1], head[2], lights, LIGHTS_MAX); break;
			case CAMP: camera_points_import(reinterpret_cast<const unsigned char*>(data), static_cast<int>(bytes)); break;
			case DETH:
				deaths.assign(data, bytes);
				have_deaths = true;
				break;
			case STRM:
			{
				StormState storm;
				if (items(data, head[1], head[2], &storm, 1) == 1)
				{
					storm_set(storm);
				}

				break;
			}
			default: break;
			}
		}

		// the actors, then the editor's changes to their outfits (an actor whose changes differ is made again)
		actor_count = actor_count < outfit_count ? actor_count : outfit_count;
		actors_import(actors, outfits, actor_count);
		static char text[16384];
		for (int i = 0; i < actor_count; ++i)
		{
			const std::string mark = "#" + std::to_string(actors[i].handle) + "\n";
			std::string wanted;
			const size_t found = changes.find(mark);
			if (found != std::string::npos && (found == 0 || changes[found - 1] == '\n'))
			{
				const size_t begin = found + mark.size(), end = changes.find("\n#", begin - 1);
				wanted = changes.substr(begin, end == std::string::npos ? std::string::npos : end + 1 - begin);
			}

			models_export(actors[i].handle, text, sizeof(text));
			if (wanted != text)
			{
				models_import(actors[i].handle, wanted.c_str());
				actors_touch(actors[i].handle);
			}
		}

		// the effects: those there changed to these, the rest added or removed
		static PlacedEffect now[EFFECTS_MAX];
		int have = effects_placed(now, EFFECTS_MAX);
		while (have > effect_count)
		{
			effects_remove(--have);
		}

		for (int i = 0; i < effect_count; ++i)
		{
			if (i >= have && effects_add(effects[i].name, effects[i].origin, effects[i].at_ms) < 0)
			{
				break;
			}

			if (i >= have || memcmp(&now[i], &effects[i], sizeof(PlacedEffect)) != 0)
			{
				effects_set(i, effects[i]);
			}
		}

		static SceneLight lit[LIGHTS_MAX];
		int lit_count = scene_lights(lit, LIGHTS_MAX);
		while (lit_count > light_count)
		{
			scene_remove_light(--lit_count);
		}

		for (int i = 0; i < light_count; ++i)
		{
			if (i >= lit_count && scene_add_light() < 0)
			{
				break;
			}

			scene_set_light(i, lights[i]);
		}

		if (have_deaths)
		{
			deaths_export(text, sizeof(text));
			if (deaths != text)
			{
				deaths_import(deaths.c_str());
				// (a body takes its death when it is made: the recording is given again up to here)
				theater_rebuild();
			}
		}
	}

	std::string g_base;
	std::vector<std::string> g_undo, g_redo;
	char g_name[96] = "";
	ULONGLONG g_dirty_at = 0;
	bool g_ask_undo = false, g_ask_redo = false;

	std::string file_of(const char* recording)
	{
		return std::string(theater_folder()) + "\\" + recording + ".scene";
	}

	void save(const char* recording, const std::string& block)
	{
		FILE* file = nullptr;
		if (fopen_s(&file, file_of(recording).c_str(), "wb") == 0 && file)
		{
			fwrite(block.data(), 1, block.size(), file);
			fclose(file);
		}
	}

	bool load(const char* recording, std::string& block)
	{
		FILE* file = nullptr;
		if (fopen_s(&file, file_of(recording).c_str(), "rb") != 0 || !file)
		{
			return false;
		}

		block.clear();
		char part[65536];
		for (size_t got; (got = fread(part, 1, sizeof(part), file)) > 0;)
		{
			block.append(part, got);
		}

		fclose(file);
		return block.size() >= 8;
	}

	std::string empty_block()
	{
		std::string out;
		const std::uint32_t head[2] = {MAGIC, 1};
		out.append(reinterpret_cast<const char*>(head), sizeof(head));
		put(out, ACTR, sizeof(Actor), 0, nullptr);
		put(out, OUTF, sizeof(ModelsOutfit), 0, nullptr);
		put(out, EFFX, sizeof(PlacedEffect), 0, nullptr);
		put(out, LITE, sizeof(SceneLight), 0, nullptr);
		const std::uint32_t no_points[1] = {0};
		put(out, CAMP, 1, 0, no_points);
		const StormState storm;
		put(out, STRM, sizeof(StormState), 1, &storm);
		return out;
	}
}

namespace
{
	// The effects in a saved scene's block.
	int effects_in(const std::string& block, PlacedEffect* out, const int max)
	{
		if (block.size() < 8 || memcmp(block.data(), &MAGIC, 4) != 0)
		{
			return 0;
		}

		for (size_t at = 8; at + 12 <= block.size();)
		{
			std::uint32_t head[3];
			memcpy(head, block.data() + at, sizeof(head));
			at += 12;
			const size_t bytes = static_cast<size_t>(head[1]) * head[2];
			if (bytes > block.size() - at)
			{
				break;
			}

			if (head[0] == EFFX)
			{
				return out ? items(block.data() + at, head[1], head[2], out, max) : static_cast<int>(head[2]);
			}

			at += bytes;
		}

		return 0;
	}
}

int scene_recordings(char names[][96], int counts[], const int max)
{
	int count = 0;
	WIN32_FIND_DATAA found;
	const HANDLE find = FindFirstFileA((std::string(theater_folder()) + "\\*.scene").c_str(), &found);
	if (find == INVALID_HANDLE_VALUE)
	{
		return 0;
	}

	do
	{
		std::string recording = found.cFileName;
		recording.resize(recording.size() - 6);
		std::string block;
		if (count < max && recording != g_name && recording.size() < 96 && load(recording.c_str(), block))
		{
			const int effects = effects_in(block, nullptr, 0);
			if (effects > 0)
			{
				strncpy_s(names[count], 96, recording.c_str(), _TRUNCATE);
				counts[count++] = effects;
			}
		}
	} while (FindNextFileA(find, &found));

	FindClose(find);
	return count;
}

int scene_take_effects(const char* recording)
{
	std::string block;
	static PlacedEffect effects[EFFECTS_MAX];
	if (!load(recording, block))
	{
		return 0;
	}

	const int count = effects_in(block, effects, EFFECTS_MAX);
	int taken = 0;
	for (int i = 0; i < count; ++i)
	{
		const int index = effects_add(effects[i].name, effects[i].origin, effects[i].at_ms);
		if (index < 0)
		{
			break;
		}

		// (one that went with an actor of that recording's stands where it was put instead)
		effects[i].on_actor = 0;
		effects_set(index, effects[i]);
		++taken;
	}

	return taken;
}

bool scene_can_undo() { return !g_undo.empty(); }
bool scene_can_redo() { return !g_redo.empty(); }
void scene_undo() { g_ask_undo = true; }
void scene_redo() { g_ask_redo = true; }

// (the editor's thread, each frame) busy: something is being dragged or typed: the scene is not looked at until
// it is let go of, so a drag is one step back, not hundreds.
void scene_ui_frame(const TheaterState& state, const bool busy, const bool touched)
{
	const ULONGLONG tick = GetTickCount64();
	static ULONGLONG touched_at = 0;
	if (touched)
	{
		touched_at = tick;
	}

	static bool active = false;
	static ULONGLONG marked_at = 0;
	if (marked_at && (!state.playing || (state.settled && tick - marked_at > 20000)))
	{
		// (shown for a while, or stopped by hand: the saved scene is good)
		marked_at = 0;
		DeleteFileA((file_of(g_name) + ".loading").c_str());
	}

	if (!state.playing)
	{
		if (active && g_dirty_at)
		{
			save(g_name, g_base);
		}

		active = false;
		g_dirty_at = 0;
		g_ask_undo = g_ask_redo = false;
		g_undo.clear();
		g_redo.clear();
		return;
	}

	if (!active)
	{
		// a recording begun: its own scene if it has one saved; nothing of another recording's is left in it
		active = true;
		const bool same = strcmp(g_name, state.name) == 0;
		strncpy_s(g_name, state.name, _TRUNCATE);
		std::string block;
		char line[300];
		// (a mark is left while a saved scene is first shown: if the game went down then, the scene is set aside the
		// next time instead of taking the game down again)
		const std::string mark = file_of(g_name) + ".loading";
		if (GetFileAttributesA(mark.c_str()) != INVALID_FILE_ATTRIBUTES)
		{
			DeleteFileA(mark.c_str());
			MoveFileExA(file_of(g_name).c_str(), (file_of(g_name) + ".bad").c_str(), MOVEFILE_REPLACE_EXISTING);
			snprintf(line, sizeof(line), "scene: %s.scene was being shown when the game last went down: set aside as .scene.bad, not read", g_name);
			theater_log(line);
			unpack(empty_block());
		}
		else if (load(g_name, block))
		{
			FILE* marked = nullptr;
			if (fopen_s(&marked, mark.c_str(), "w") == 0 && marked)
			{
				fclose(marked);
				marked_at = tick;
			}

			unpack(block);
			snprintf(line, sizeof(line), "scene: %s.scene read (%zu bytes)", g_name, block.size());
			theater_log(line);
			// (its rain, mist or lightning are in the editor's own file: loaded without being asked for)
			static PlacedEffect saved[EFFECTS_MAX];
			const int saved_count = effects_in(block, saved, EFFECTS_MAX);
			for (int i = 0; i < saved_count; ++i)
			{
				if (effects_lasts(saved[i].name) < 0 && effects_extra_state() == 1)
				{
					effects_load_extra();
					theater_log("scene: its effects need the rain file: loading it");
					break;
				}
			}
		}
		else if (!same)
		{
			unpack(empty_block());
		}

		g_base = pack();
		g_undo.clear();
		g_redo.clear();
		g_dirty_at = 0;
		return;
	}

	if (g_ask_undo || g_ask_redo)
	{
		std::vector<std::string>& from = g_ask_undo ? g_undo : g_redo;
		std::vector<std::string>& to = g_ask_undo ? g_redo : g_undo;
		g_ask_undo = g_ask_redo = false;
		if (!from.empty() && !busy)
		{
			to.push_back(g_base);
			unpack(from.back());
			from.pop_back();
			g_base = pack();
			g_dirty_at = tick;
		}

		return;
	}

	if (!busy)
	{
		std::string now = pack();
		if (now != g_base)
		{
			// (a change with no hand in it, a keyframed light's colour going on as the recording plays, is not a step)
			if (tick - touched_at < 1500)
			{
				g_undo.push_back(std::move(g_base));
				if (g_undo.size() > 200)
				{
					g_undo.erase(g_undo.begin());
				}

				g_redo.clear();
				g_dirty_at = tick;
			}

			g_base = std::move(now);
		}
	}

	if (g_dirty_at && tick - g_dirty_at > 1500)
	{
		g_dirty_at = 0;
		save(g_name, g_base);
	}
}

// ---- the storm: lightning at times worked out from the recording's own time (the same strikes every time it is
// played or recorded): the sun's light flares for a moment, and a bolt of the rain file's is played far off.
StormState storm_state()
{
	std::lock_guard<std::mutex> lock(g_storm_lock);
	return g_storm;
}

void storm_set(const StormState& storm)
{
	std::lock_guard<std::mutex> lock(g_storm_lock);
	g_storm = storm;
}

namespace
{
	std::uint32_t hash(std::uint32_t x)
	{
		x ^= x >> 16;
		x *= 0x7FEB352Du;
		x ^= x >> 15;
		x *= 0x846CA68Bu;
		x ^= x >> 16;
		return x;
	}

	bool g_flaring = false;
	float g_sun_before = 1.0f;
	double g_storm_before = 0.0;
}

// (the game's thread, each frame)
void scene_game_frame(const TheaterState& state)
{
	const StormState storm = storm_state();
	const SunState sun = sun_state();
	if (!storm.on || !state.playing || !state.settled)
	{
		if (g_flaring)
		{
			g_flaring = false;
			sun_set_brightness(g_sun_before);
		}

		return;
	}

	const double now = state.position_exact;
	const double every = (storm.every_s < 1.0f ? 1.0 : storm.every_s) * 1000.0;
	// the last strike at or before now: one in each stretch of "every", somewhere in its first four fifths
	double strike = -1.0;
	std::uint32_t which = 0;
	for (int back = 0; back < 2 && strike < 0.0; ++back)
	{
		const double stretch = std::floor(now / every) - back;
		if (stretch < 0.0)
		{
			break;
		}

		const std::uint32_t number = static_cast<std::uint32_t>(stretch);
		const double at = stretch * every + (hash(number * 2654435761u + 17u) % 1000u) / 1000.0 * every * 0.8;
		if (at <= now)
		{
			strike = at;
			which = number;
		}
	}

	// the flare: up at once, flickering down over half a second
	const double age = strike < 0.0 ? 1.0e9 : now - strike;
	float flare = 0.0f;
	if (age < 700.0)
	{
		flare = static_cast<float>(std::exp(-age / 80.0));
		if (age > 130.0)
		{
			flare += 0.7f * static_cast<float>(std::exp(-(age - 130.0) / 60.0));
		}

		if (age > 290.0)
		{
			flare += 0.35f * static_cast<float>(std::exp(-(age - 290.0) / 110.0));
		}
	}

	if (sun.known)
	{
		if (flare > 0.01f)
		{
			if (!g_flaring)
			{
				g_flaring = true;
				g_sun_before = sun.brightness;
			}

			sun_set_brightness(g_sun_before * (1.0f + storm.flash * flare) + (g_sun_before < 0.05f ? storm.flash * flare * 0.5f : 0.0f));
		}
		else if (g_flaring)
		{
			g_flaring = false;
			sun_set_brightness(g_sun_before);
		}
	}

	// the bolt, as the time goes past the strike
	if (storm.bolts && strike >= 0.0 && g_storm_before < strike && now - g_storm_before < 400.0 && now >= strike)
	{
		static const char* const BOLTS[] = {"vfx/lightning/lightning_bolt_a", "vfx/lightning/lightning_cloud_a", "vfx/map/mp_refraction/lightning_mp_refraction"};
		float eye[3];
		camera_view_origin(eye);
		const float angle = (hash(which + 99u) % 6283u) / 1000.0f;
		const float origin[3] = {eye[0] + std::cos(angle) * storm.distance, eye[1] + std::sin(angle) * storm.distance, eye[2]};
		// (an effect goes along its forward: up)
		const float axis[3][3] = {{0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}};
		const char* bolt = BOLTS[hash(which + 7u) % 3u];
		if (!effects_play_once(bolt, origin, axis))
		{
			static bool said = false;
			if (!said)
			{
				said = true;
				theater_log("storm: the lightning effects are not loaded (load the rain effects first)");
			}
		}
	}

	g_storm_before = now;
}
