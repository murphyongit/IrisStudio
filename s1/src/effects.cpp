// Effects put into the scene: any effect the game has loaded (an explosion, sparks, smoke) played at a place
// of the editor's choosing, at a time of the recording.
//
// The game's effects are assets (type 42, their names first in them); FX_PlayOrientedEffect (0x1402AC700:
// the client, the effect, the client game's time, where, and its axis) plays one, as the game's own code does
// for a landing's dust. Only what the map and the match have loaded can be played.
#include "theater.hpp"

#include <MinHook.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <windows.h>

namespace
{
	constexpr std::uintptr_t DB_ENUM_XASSETS = 0x14026EC80, FX_PLAY_ORIENTED_EFFECT = 0x1402AC700, CG_TIME = 0x141557580;
	// FX_KillEffectDefNewerThan (the client, the effect, a time of the client game's): every effect of that kind
	// begun after the time is taken away at the game's next step (the request only goes on a list of 64 a frame).
	// The game's own use: explosions of the last moments taken back (0x1401C43BA), and all of a kind before a
	// killcam plays its effects again at their old times (0x1401A73F5, with -1).
	constexpr std::uintptr_t FX_KILL_EFFECT_DEF_NEWER_THAN = 0x1402B1890;
	constexpr int ASSET_FX = 42, MAX_LOADED = 16384, MAX_PLACED = EFFECTS_MAX;
	// an effect begun when the time is already well into its clip is begun as of this long ago at the most
	constexpr int CATCH_UP_MS = 20000, MOST_KILLS_A_FRAME = 16;
	constexpr float TO_RAD = 3.14159265358979f / 180.0f;

	// Effects of another map's. A map's effects are only in that map's own files; a small file of the
	// game's kind built from them (zone\s1iris_rain.ff: Ascend's rain, mist and lightning, made with x64
	// ZoneTool) is loaded on top of whatever map is being played (DB_LoadXAssets 0x140270F30: a name, the
	// kind of file it is kept as, 1 being the game's common ones, which stay for the session).
	// Called from the drawing of a frame (0.47) it never came back: the game's loader and its drawing
	// wait on each other there. S1x's own console command "loadzone" makes the same call from where the
	// game runs its commands, between frames, so the load is asked for through that, and only once the
	// game's loader has nothing in hand (Sys_IsDatabaseReady 0x1403E1850).
	constexpr std::uintptr_t SYS_IS_DATABASE_READY = 0x1403E1850;
	bool g_extra_wanted = false, g_extra_loaded = false;
	ULONGLONG g_extra_at = 0, g_extra_asked = 0;

	// A file built by ZoneTool is packed with zlib, the game's own with LZ4, and the game takes one kind
	// only in a session ("Mixed compression types are not supported", 0x140242DF0: the first file's kind is
	// kept and every later one compared with it). s1-mod's fastfiles.cpp lifts that the same three ways:
	// the comparison's jump taken out (0x140242DF7), the kind set back to LZ4 (4, at 0x141E0B080) whenever
	// the game reads its streamed files (DB_ReadStreamFile 0x14027AA70), and unsigned files let through
	// (0x1402427A5).
	constexpr std::uintptr_t MIXED_JUMP = 0x140242DF7, UNSIGNED_JUMP = 0x1402427A5;
	constexpr std::uintptr_t DB_READ_STREAM_FILE = 0x14027AA70, G_COMPRESSOR = 0x141E0B080;
	int (*g_original_read_stream_file)(int, int) = nullptr;
	bool g_mixed_allowed = false;

	int hooked_read_stream_file(const int allow_abort, const int finish)
	{
		*reinterpret_cast<int*>(G_COMPRESSOR) = 4;
		return g_original_read_stream_file(allow_abort, finish);
	}

	// A two-byte jump-if-not-equal (0x75) made into nothing; 1 done, 2 already so, 0 not what was expected.
	int remove_jump(const std::uintptr_t address)
	{
		__try
		{
			auto* code = reinterpret_cast<std::uint8_t*>(address);
			if (code[0] == 0x90 && code[1] == 0x90)
			{
				return 2;
			}

			if (code[0] != 0x75)
			{
				return 0;
			}

			DWORD old = 0;
			if (!VirtualProtect(code, 2, PAGE_EXECUTE_READWRITE, &old))
			{
				return 0;
			}

			code[0] = 0x90;
			code[1] = 0x90;
			VirtualProtect(code, 2, old, &old);
			FlushInstructionCache(GetCurrentProcess(), code, 2);
			return 1;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return 0;
		}
	}

	bool loader_idle()
	{
		__try
		{
			return reinterpret_cast<int (*)()>(SYS_IS_DATABASE_READY)() != 0;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	std::string extra_path()
	{
		// (the theater folder is in the game's)
		std::string folder = theater_folder();
		const size_t slash = folder.find_last_of("\\/");
		return (slash == std::string::npos ? std::string(".") : folder.substr(0, slash)) + "\\zone\\s1iris_rain.ff";
	}

	std::mutex g_lock;
	std::unordered_map<std::string, void*> g_loaded;
	std::vector<std::string> g_names;
	bool g_listed = false;
	PlacedEffect g_placed[MAX_PLACED];
	int g_count = 0;

	// What is in the game for a placed effect. The game gives no handle to an effect begun (it is only asked
	// for, and made at the game's next step), and takes effects away by kind and time only; so what is kept is
	// the kind and the times: when ours began, to take away "this kind, begun since then".
	struct Live
	{
		bool alive = false;       // begun, and not taken away since
		void* def = nullptr;      // as what
		int rep = 0;              // which playing of it ("Again every")
		int first_begin = 0;      // the client game's time the first of ours began at, and the last
		int last_begin = 0;
		int hold = 0;             // frames to wait before beginning it again (after its kind was taken away)
		bool dirty = false;       // changed by the editor since it began: begun again once the changing stops
		ULONGLONG dirty_at = 0;
		bool preview = false;     // played by hand outside its clip: taken away after a while
		int preview_begin = 0;
		ULONGLONG preview_until = 0;
		bool play_now = false;    // "Play it now" asked for
	};
	Live g_live[MAX_PLACED];

	struct Kill
	{
		void* def;
		int from;  // (effects of the kind begun at this time or later)
	};
	std::vector<Kill> g_kills;
	std::uint32_t g_rewinds = 0;
	int g_settled_frames = 0, g_told = 0;
	std::unordered_map<std::string, bool> g_described;

	void say(const char* format, ...)
	{
		if (g_told >= 400)
		{
			return;
		}

		++g_told;
		char text[400];
		va_list arguments;
		va_start(arguments, format);
		vsnprintf(text, sizeof(text), format, arguments);
		va_end(arguments);
		theater_log(text);
	}

	const char* short_name(const char* name)
	{
		const char* slash = strrchr(name, '/');
		return slash ? slash + 1 : name;
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
			reinterpret_cast<void (*)(int, void (*)(void*, void*), const void*, bool)>(DB_ENUM_XASSETS)(ASSET_FX, take, &found, false);
			return found.count;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return -1;
		}
	}

	// (begin: the client game's time the effect is to have begun at: now, or earlier for one already under way)
	bool play(void* effect, const int begin, const float* origin, const float (*axis)[3])
	{
		__try
		{
			reinterpret_cast<void (*)(int, const void*, int, const float*, const float (*)[3])>(FX_PLAY_ORIENTED_EFFECT)(0, effect, begin, origin, axis);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	bool kill_newer(void* effect, const int than)
	{
		__try
		{
			reinterpret_cast<void (*)(int, const void*, int)>(FX_KILL_EFFECT_DEF_NEWER_THAN)(0, effect, than);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	int game_time()
	{
		__try
		{
			return *reinterpret_cast<const int*>(CG_TIME);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return 0;
		}
	}

	// What an effect is made of. FxEffectDef: +0x14 looping parts, +0x18 one-shot, +0x1C emitted, +0x38 the parts
	// (0x140 each: +0x08 for a looping part how often and how many times, +0x2C its delay and +0x34 its life, each
	// a least and a spread, +0xB4 kind, +0xB6 how many visuals, +0xD0 the visual or the list of them: for the
	// kinds up to 10, sprites to clouds, a material, whose name is first in it).
	struct Made
	{
		int looping = 0, once = 0;
		int lasts_ms = -1;  // by itself: 0 for ever, -1 not known
		char parts[700] = "";
	};

	bool read_made(void* def, Made* made)
	{
		__try
		{
			const auto* at = static_cast<const unsigned char*>(def);
			made->looping = *reinterpret_cast<const int*>(at + 0x14);
			made->once = *reinterpret_cast<const int*>(at + 0x18);
			const int emitted = *reinterpret_cast<const int*>(at + 0x1C);
			const auto* parts = *reinterpret_cast<const unsigned char* const*>(at + 0x38);
			const int count = made->looping + made->once + emitted;
			if (!parts || count < 0 || count > 256)
			{
				return false;
			}

			bool forever = false;
			long long longest = 0;
			size_t used = 0;
			for (int i = 0; i < count; ++i)
			{
				const unsigned char* part = parts + static_cast<size_t>(i) * 0x140;
				const int* delay = reinterpret_cast<const int*>(part + 0x2C);
				const int* life = reinterpret_cast<const int*>(part + 0x34);
				long long ends = static_cast<long long>(delay[0]) + delay[1] + life[0] + life[1];
				if (i < made->looping)
				{
					const int every = *reinterpret_cast<const int*>(part + 0x08), times = *reinterpret_cast<const int*>(part + 0x0C);
					if (times <= 0 || times >= 0x7FFFFFF0)
					{
						forever = true;
					}
					else
					{
						ends += static_cast<long long>(every) * times;
					}
				}

				if (i < made->looping + made->once)
				{
					longest = ends > longest ? ends : longest;
				}

				const int kind = part[0xB4], visuals = part[0xB6];
				const char* material = nullptr;
				if (kind <= 10 && visuals > 0)
				{
					const void* visual = *reinterpret_cast<const void* const*>(part + 0xD0);
					if (visuals > 1 && visual)
					{
						visual = *static_cast<const void* const*>(visual);
					}

					material = visual ? *static_cast<const char* const*>(visual) : nullptr;
				}

				if (used + 120 < sizeof(made->parts))
				{
					used += static_cast<size_t>(snprintf(made->parts + used, sizeof(made->parts) - used, " [%d: kind %d%s%.80s]", i, kind, material ? ", " : "", material ? material : ""));
				}
			}

			made->lasts_ms = forever ? 0 : longest > 3600000 ? 0 : static_cast<int>(longest);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	std::unordered_map<std::string, int> g_lasts;

	// (with the lock held; once a kind, for the log)
	void describe(const char* name, void* def)
	{
		if (g_described.count(name))
		{
			return;
		}

		g_described[name] = true;
		Made made;
		if (!read_made(def, &made))
		{
			say("effects: %s could not be read", name);
			return;
		}

		g_lasts[name] = made.lasts_ms;
		if (made.lasts_ms == 0)
		{
			say("effects: %s goes on for ever; %d looping and %d one-shot parts:%s", name, made.looping, made.once, made.parts);
		}
		else
		{
			say("effects: %s lasts %.2f s; %d looping and %d one-shot parts:%s", name, made.lasts_ms / 1000.0, made.looping, made.once, made.parts);
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
			if (!found[i] || IsBadReadPtr(found[i], sizeof(void*)))
			{
				continue;
			}

			const char* name = *static_cast<const char* const*>(found[i]);
			if (name && !IsBadStringPtrA(name, 160) && name[0] > 32 && strnlen(name, 160) < 150)
			{
				g_loaded[name] = found[i];
			}
		}

		for (const auto& effect : g_loaded)
		{
			g_names.push_back(effect.first);
		}

		std::sort(g_names.begin(), g_names.end());
		char text[96];
		snprintf(text, sizeof(text), "effects: the game has %d effects loaded", count);
		theater_log(text);
		// (the weather's, by name, to know what a map's rain is made of)
		std::string weather;
		for (const std::string& name : g_names)
		{
			for (const char* word : {"rain", "mist", "fog", "lightning", "thunder", "storm", "puddle", "drip", "splash", "weather", "cloud", "wind"})
			{
				if (name.find(word) != std::string::npos)
				{
					weather += name + " ";
					break;
				}
			}

			if (weather.size() > 900)
			{
				theater_log(("effects: weather: " + weather).c_str());
				weather.clear();
			}
		}

		if (!weather.empty())
		{
			theater_log(("effects: weather: " + weather).c_str());
		}
	}

	void fire(const PlacedEffect& placed, void* def, const int begin)
	{
		// the game's own: forward, left, up from pitch, yaw, roll (an effect goes along its forward)
		const float sp = std::sin(placed.angles[0] * TO_RAD), cp = std::cos(placed.angles[0] * TO_RAD);
		const float sy = std::sin(placed.angles[1] * TO_RAD), cy = std::cos(placed.angles[1] * TO_RAD);
		const float sr = std::sin(placed.angles[2] * TO_RAD), cr = std::cos(placed.angles[2] * TO_RAD);
		const float axis[3][3] = {{cp * cy, cp * sy, -sp},
		                          {sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, sr * cp},
		                          {cr * sp * cy + sr * sy, cr * sp * sy - sr * cy, cr * cp}};
		if (!play(def, begin, placed.origin, axis))
		{
			say("effects: a fault playing %s", placed.name);
		}
	}

	// (with the lock held) Effects of a kind begun at a time or later are to go.
	void kill_from(void* def, const int from)
	{
		if (!def)
		{
			return;
		}

		for (Kill& kill : g_kills)
		{
			if (kill.def == def)
			{
				kill.from = (std::min)(kill.from, from);
				return;
			}
		}

		g_kills.push_back({def, from});
	}

	// What makes an effect a different one in the game (not when it is switched on or how long its clip is:
	// those only decide whether it is there).
	bool same_in_game(const PlacedEffect& a, const PlacedEffect& b)
	{
		return strcmp(a.name, b.name) == 0 && memcmp(a.origin, b.origin, sizeof(a.origin)) == 0 && memcmp(a.angles, b.angles, sizeof(a.angles)) == 0 &&
		       a.at_ms == b.at_ms && a.every_ms == b.every_ms && a.follow == b.follow && a.above == b.above;
	}
}

// At the start: the game made to take files packed the other way (see above).
void effects_start()
{
	const bool hooked = MH_CreateHook(reinterpret_cast<LPVOID>(DB_READ_STREAM_FILE), reinterpret_cast<LPVOID>(&hooked_read_stream_file),
	                        reinterpret_cast<LPVOID*>(&g_original_read_stream_file)) == MH_OK &&
	                    MH_EnableHook(reinterpret_cast<LPVOID>(DB_READ_STREAM_FILE)) == MH_OK;

	// (without the hook the game's own streamed files would be read as the wrong kind: the rest is left)
	int mixed = -1, unsigned_files = -1;
	if (hooked)
	{
		mixed = remove_jump(MIXED_JUMP);
		unsigned_files = remove_jump(UNSIGNED_JUMP);
		g_mixed_allowed = mixed != 0;
	}

	char text[200];
	snprintf(text, sizeof(text), "effects: files packed with zlib %s (streamed files hook %d, mixed kinds %d, unsigned %d)",
	         g_mixed_allowed ? "can be loaded" : "can NOT be loaded", hooked ? 1 : 0, mixed, unsigned_files);
	theater_log(text);
}

// The extra effects' file: 0 it is not there, 1 it is and is not loaded, 2 it is loaded (or being loaded).
int effects_extra_state()
{
	std::lock_guard<std::mutex> lock(g_lock);
	if (g_extra_loaded || g_extra_wanted)
	{
		return 2;
	}

	return g_mixed_allowed && GetFileAttributesA(extra_path().c_str()) != INVALID_FILE_ATTRIBUTES ? 1 : 0;
}

void effects_load_extra()
{
	std::lock_guard<std::mutex> lock(g_lock);
	g_extra_wanted = true;
}

// The loaded effects whose names have the text in them.
int effects_find(const char* text, char names[][96], const int max)
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
			strncpy_s(names[count++], 96, name.c_str(), _TRUNCATE);
		}
	}

	return count;
}

int effects_placed(PlacedEffect* out, const int max)
{
	std::lock_guard<std::mutex> lock(g_lock);
	const int count = g_count < max ? g_count : max;
	for (int i = 0; i < count; ++i)
	{
		out[i] = g_placed[i];
	}

	return count;
}

// How long an effect of this name lasts by itself, in milliseconds: 0 for ever (rain), -1 not known.
int effects_lasts(const char* name)
{
	std::lock_guard<std::mutex> lock(g_lock);
	const auto known = g_lasts.find(name);
	if (known != g_lasts.end())
	{
		return known->second;
	}

	const auto loaded = g_loaded.find(name);
	if (loaded == g_loaded.end())
	{
		return -1;
	}

	Made made;
	const int lasts = read_made(loaded->second, &made) ? made.lasts_ms : -1;
	g_lasts[name] = lasts;
	return lasts;
}

// Which of the placed effects are in the game now (begun and not taken away).
int effects_live(bool* out, const int max)
{
	std::lock_guard<std::mutex> lock(g_lock);
	const int count = g_count < max ? g_count : max;
	for (int i = 0; i < count; ++i)
	{
		out[i] = g_live[i].alive;
	}

	return count;
}

// A new one at a place, its clip beginning at a time of the recording; it is in the game while the recording's
// time is in its clip. -1: no room.
int effects_add(const char* name, const float origin[3], const std::uint32_t ms)
{
	std::lock_guard<std::mutex> lock(g_lock);
	if (g_count >= MAX_PLACED)
	{
		return -1;
	}

	PlacedEffect placed;
	strncpy_s(placed.name, name, _TRUNCATE);
	memcpy(placed.origin, origin, sizeof(placed.origin));
	placed.at_ms = ms;
	{
		// its clip as long as the effect itself (ten seconds for one that never ends)
		const auto loaded = g_loaded.find(name);
		Made made;
		if (loaded != g_loaded.end() && read_made(loaded->second, &made) && made.lasts_ms >= 0)
		{
			placed.length_ms = made.lasts_ms == 0 ? 10000 : static_cast<std::uint32_t>(made.lasts_ms < 500 ? 500 : made.lasts_ms);
		}
	}

	g_placed[g_count] = placed;
	g_live[g_count] = Live();
	return g_count++;
}

void effects_set(const int index, const PlacedEffect& placed)
{
	std::lock_guard<std::mutex> lock(g_lock);
	if (index >= 0 && index < g_count)
	{
		if (!same_in_game(g_placed[index], placed))
		{
			g_live[index].dirty = true;
			g_live[index].dirty_at = GetTickCount64();
		}

		g_placed[index] = placed;
	}
}

// "Play it now": begun again from its start if it is in the game, and otherwise shown for a few seconds.
void effects_fire(const int index)
{
	std::lock_guard<std::mutex> lock(g_lock);
	if (index >= 0 && index < g_count)
	{
		g_live[index].play_now = true;
	}
}

void effects_remove(const int index)
{
	std::lock_guard<std::mutex> lock(g_lock);
	if (index >= 0 && index < g_count)
	{
		const Live& live = g_live[index];
		if (live.alive)
		{
			kill_from(live.def, live.first_begin);
		}

		if (live.preview)
		{
			kill_from(live.def, live.preview_begin);
		}

		for (int i = index; i + 1 < g_count; ++i)
		{
			g_placed[i] = g_placed[i + 1];
			g_live[i] = g_live[i + 1];
		}

		--g_count;
	}
}

// Each frame, on the game's own thread where it works its view out: what is due is played.
void effects_game_frame(const TheaterState& state)
{
	std::lock_guard<std::mutex> lock(g_lock);
	if (!state.playing)
	{
		// (the match is over: its effects went with it)
		g_listed = false;
		g_kills.clear();
		g_settled_frames = 0;
		g_told = 0;
		for (Live& live : g_live)
		{
			live = Live();
		}

		return;
	}

	// the extra effects, asked for: loaded once the match is under way, and listed a moment later
	if (g_extra_wanted && !g_extra_loaded && state.settled)
	{
		if (!g_extra_asked)
		{
			g_extra_asked = GetTickCount64();
		}

		if (loader_idle())
		{
			g_extra_loaded = true;
			g_extra_at = GetTickCount64();
			theater_log("effects: asking the game for s1iris_rain (loadzone)");
			theater_command("loadzone s1iris_rain");
		}
		else if (GetTickCount64() - g_extra_asked > 10000)
		{
			// (the button comes back)
			g_extra_wanted = false;
			g_extra_asked = 0;
			theater_log("effects: the game's loader was busy for 10 s; s1iris_rain not asked for");
		}
	}

	if (g_extra_at && GetTickCount64() - g_extra_at > 3000)
	{
		g_extra_at = 0;
		g_listed = false;
		list_loaded();
	}

	if (g_count && !g_listed)
	{
		list_loaded();
	}

	// Gone back in the recording: the game has begun its match again from the start, and nothing begun before
	// is in it any more. (What was ours is asked away all the same, in case any of it was kept.)
	if (state.rewinds != g_rewinds)
	{
		g_rewinds = state.rewinds;
		for (int i = 0; i < g_count; ++i)
		{
			Live& live = g_live[i];
			if (live.alive)
			{
				kill_from(live.def, live.first_begin);
			}

			if (live.preview)
			{
				kill_from(live.def, live.preview_begin);
			}

			live.alive = live.preview = false;
			live.hold = 0;
		}
	}

	// Nothing is begun or taken away while the recording is being run up to a place, nor in the first frames
	// after: the game still sets its clock then, and takes back effects "begun in the future" as it does.
	if (!state.settled)
	{
		g_settled_frames = 0;
		return;
	}

	if (++g_settled_frames < 3)
	{
		return;
	}

	const double now = state.position_exact;
	const int game_now = game_time();
	const ULONGLONG tick = GetTickCount64();
	// (the client game's time at the recording's start: the two run together)
	const int base = game_now - static_cast<int>(now);
	bool wanted[MAX_PLACED] = {};
	void* defs[MAX_PLACED] = {};

	// 1. what is in the game and should not be: outside its clip, switched off, or changed
	for (int i = 0; i < g_count; ++i)
	{
		const PlacedEffect& placed = g_placed[i];
		Live& live = g_live[i];
		const auto loaded = g_loaded.find(placed.name);
		defs[i] = loaded == g_loaded.end() ? nullptr : loaded->second;
		const double clip_end = static_cast<double>(placed.at_ms) + (placed.length_ms > 100 ? placed.length_ms : 100);
		wanted[i] = placed.on && defs[i] && now >= placed.at_ms && now < clip_end;
		if (live.dirty && tick - live.dirty_at >= 200)
		{
			live.dirty = false;
			if (live.alive)
			{
				kill_from(live.def, live.first_begin);
			}
		}

		if (live.play_now)
		{
			live.play_now = false;
			if (live.alive)
			{
				kill_from(live.def, live.first_begin);
			}
			else if (defs[i] && !wanted[i])
			{
				if (live.preview)
				{
					kill_from(live.def, live.preview_begin);
				}

				PlacedEffect here = placed;
				if (placed.follow)
				{
					camera_view_origin(here.origin);
					here.origin[2] += placed.above;
				}

				describe(placed.name, defs[i]);
				fire(here, defs[i], game_now);
				live.preview = true;
				live.def = defs[i];
				live.preview_begin = game_now;
				const std::uint32_t shown = placed.length_ms < 2000 ? 2000 : placed.length_ms > 10000 ? 10000 : placed.length_ms;
				live.preview_until = tick + shown;
				say("effects: %s shown by hand for %.1f s", short_name(placed.name), shown / 1000.0);
			}
		}

		if (live.preview && (tick >= live.preview_until || wanted[i]))
		{
			live.preview = false;
			kill_from(live.def, live.preview_begin);
		}

		if (live.alive && (!wanted[i] || live.def != defs[i]))
		{
			kill_from(live.def, live.first_begin);
		}
	}

	// 2. taken away: by kind, from the earliest time that has to go. Whatever else of ours is of that kind
	// and began no earlier goes with it, and is begun again below (as of when it began, so it carries on).
	int asked = 0;
	while (!g_kills.empty() && asked < MOST_KILLS_A_FRAME)
	{
		const Kill kill = g_kills.back();
		g_kills.pop_back();
		++asked;
		if (!kill_newer(kill.def, kill.from - 1))
		{
			say("effects: a fault taking an effect away");
			continue;
		}

		int gone = 0;
		const char* name = "?";
		for (int i = 0; i < g_count; ++i)
		{
			Live& live = g_live[i];
			if (live.def != kill.def)
			{
				continue;
			}

			name = g_placed[i].name;
			if (live.alive && live.last_begin >= kill.from)
			{
				live.alive = false;
				++gone;
			}

			if (live.preview && live.preview_begin >= kill.from)
			{
				live.preview = false;
			}

			// (the taking away and a new beginning are not asked for in one frame)
			live.hold = 2;
		}

		say("effects: %s taken away at %.2f s (%d of the editor's)", short_name(name), now / 1000.0, gone);
	}

	// 3. what should be in the game and is not
	for (int i = 0; i < g_count; ++i)
	{
		const PlacedEffect& placed = g_placed[i];
		Live& live = g_live[i];
		if (live.hold > 0)
		{
			--live.hold;
			continue;
		}

		if (!wanted[i] || live.dirty)
		{
			continue;
		}

		// which playing of it the time is in ("Again every": each one a new one, the earlier left to run out;
		// not for an effect that never ends, which would only pile up)
		describe(placed.name, defs[i]);
		const auto lasts = g_lasts.find(placed.name);
		const int every = lasts != g_lasts.end() && lasts->second == 0 ? 0 : static_cast<int>(placed.every_ms);
		const int rep = every >= 50 ? static_cast<int>((now - placed.at_ms) / every) : 0;
		if (live.alive && live.rep == rep)
		{
			continue;
		}

		// begun as of when its clip (or this playing of it) began: an explosion gone into half way is half over,
		// rain is already falling
		const int began = base + static_cast<int>(placed.at_ms) + rep * every;
		const int begin = (std::min)(game_now, (std::max)(began, game_now - CATCH_UP_MS));
		PlacedEffect here = placed;
		if (placed.follow)
		{
			// where the camera is now, and above it
			camera_view_origin(here.origin);
			here.origin[2] += placed.above;
		}

		fire(here, defs[i], begin);
		if (!live.alive)
		{
			live.first_begin = begin;
			say("effects: %s begun at %.2f s (as of %.2f s ago)", short_name(placed.name), now / 1000.0, (game_now - begin) / 1000.0);
		}

		live.alive = true;
		live.def = defs[i];
		live.rep = rep;
		live.last_begin = begin;
	}
}
