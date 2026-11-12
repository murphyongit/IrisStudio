// Effects put into the scene: any effect the game has loaded (an explosion, sparks, smoke) played at a place
// of the editor's choosing, at a time of the recording.
//
// The game's effects are assets (type 42, their names first in them); FX_PlayOrientedEffect (0x1402AC700:
// the client, the effect, the client game's time, where, and its axis) plays one, as the game's own code does
// for a landing's dust. Only what the map and the match have loaded can be played.
#include "theater.hpp"

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
	constexpr int ASSET_FX = 42, MAX_LOADED = 16384, MAX_PLACED = 32;
	constexpr float TO_RAD = 3.14159265358979f / 180.0f;

	std::mutex g_lock;
	std::unordered_map<std::string, void*> g_loaded;
	std::vector<std::string> g_names;
	bool g_listed = false;
	PlacedEffect g_placed[MAX_PLACED];
	bool g_fire[MAX_PLACED] = {};
	int g_count = 0;
	double g_last_ms = -1.0;

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

	bool play(void* effect, const float* origin, const float (*axis)[3])
	{
		__try
		{
			reinterpret_cast<void (*)(int, const void*, int, const float*, const float (*)[3])>(FX_PLAY_ORIENTED_EFFECT)(
				0, effect, *reinterpret_cast<const int*>(CG_TIME), origin, axis);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
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

	void fire(const PlacedEffect& placed)
	{
		const auto loaded = g_loaded.find(placed.name);
		if (loaded == g_loaded.end())
		{
			return;
		}

		// the game's own: forward, left, up from pitch, yaw, roll (an effect goes along its forward)
		const float sp = std::sin(placed.angles[0] * TO_RAD), cp = std::cos(placed.angles[0] * TO_RAD);
		const float sy = std::sin(placed.angles[1] * TO_RAD), cy = std::cos(placed.angles[1] * TO_RAD);
		const float sr = std::sin(placed.angles[2] * TO_RAD), cr = std::cos(placed.angles[2] * TO_RAD);
		const float axis[3][3] = {{cp * cy, cp * sy, -sp},
		                          {sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, sr * cp},
		                          {cr * sp * cy + sr * sy, cr * sp * sy - sr * cy, cr * cp}};
		if (!play(loaded->second, placed.origin, axis))
		{
			theater_log("effects: a fault playing an effect");
		}
	}
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

// A new one where the camera is, at the recording's time now; it plays at once too. -1: no room.
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
	g_placed[g_count] = placed;
	g_fire[g_count] = true;
	return g_count++;
}

void effects_set(const int index, const PlacedEffect& placed)
{
	std::lock_guard<std::mutex> lock(g_lock);
	if (index >= 0 && index < g_count)
	{
		g_placed[index] = placed;
	}
}

void effects_fire(const int index)
{
	std::lock_guard<std::mutex> lock(g_lock);
	if (index >= 0 && index < g_count)
	{
		g_fire[index] = true;
	}
}

void effects_remove(const int index)
{
	std::lock_guard<std::mutex> lock(g_lock);
	if (index >= 0 && index < g_count)
	{
		for (int i = index; i + 1 < g_count; ++i)
		{
			g_placed[i] = g_placed[i + 1];
			g_fire[i] = g_fire[i + 1];
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
		g_listed = false;
		g_last_ms = -1.0;
		return;
	}

	if (g_count && !g_listed)
	{
		list_loaded();
	}

	const double now = state.position_exact, last = g_last_ms;
	g_last_ms = now;
	for (int i = 0; i < g_count; ++i)
	{
		const PlacedEffect& placed = g_placed[i];
		bool due = g_fire[i];
		g_fire[i] = false;
		// the recording's time passing the effect's (not a jump: only time that ran on)
		if (placed.on && last >= 0.0 && now > last && now - last < 1000.0)
		{
			if (last < placed.at_ms && now >= placed.at_ms)
			{
				due = true;
			}
			else if (placed.every_ms >= 50 && now > placed.at_ms && now <= static_cast<double>(placed.at_ms) + placed.length_ms)
			{
				const double since_last = (last - placed.at_ms) / placed.every_ms, since_now = (now - placed.at_ms) / placed.every_ms;
				due = due || (last >= placed.at_ms && std::floor(since_now) > std::floor(since_last));
			}
		}

		if (due)
		{
			fire(placed);
		}
	}
}
