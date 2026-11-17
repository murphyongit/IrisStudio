// The exposure and the tone curve: how the scene's light (which goes far above white: a lamp, a muzzle flash,
// the sky) is brought down to what a screen shows.
//
// A map's own values come with its light sets (maps/createart/<map>_lightsets_hdr.csv: tonemapauto,
// tonemapexposure, tonemapwhite and so on); the client game blends them into the view it hands the renderer,
// and the renderer copies them into the scene it is about to draw in 0x1405D0590(scene, view): from the view
// (+0x300 on), or, if a setting of the game's says so, from settings of its own. Those settings have no names
// the console of this game knows them by that could be relied on (0.75 to 0.82 set "r_tonemap..." through the
// console and nothing happened), so the editor's values are written straight into the scene after that copy:
// only the ones that were changed, the rest stay the map's.
//
// In the scene: the curve's kind (int) at +0x1DB0, automatic exposure (byte) +0x1DB4, its hold (byte) +0x1DB6,
// then eighteen floats from +0x1DB8: the automatic shift, the most exposure, the exposure, its shift, how fast
// the automatic one follows, black, white, crossover, shoulder, toe, the dark, middle and light scene levels,
// their shifts, the least and the most shift.
#include "theater.hpp"

#include <MinHook.h>
#include <windows.h>

#include <cstring>
#include <mutex>

namespace
{
	constexpr std::uintptr_t R_SCENE_TONEMAP = 0x1405D0590;
	constexpr size_t SCENE_CURVE = 0x1DB0, SCENE_AUTO = 0x1DB4, SCENE_LOCK = 0x1DB6, SCENE_FLOATS = 0x1DB8;
	using scene_tonemap_t = void (*)(unsigned char*, const unsigned char*);
	scene_tonemap_t g_original = nullptr;
	std::mutex g_lock;
	ToneState g_map, g_mine;
	std::uint32_t g_set = 0;
	bool g_hooked = false;

	void apply(unsigned char* scene)
	{
		std::lock_guard<std::mutex> lock(g_lock);
		auto* floats = reinterpret_cast<float*>(scene + SCENE_FLOATS);
		g_map.seen = true;
		g_map.curve = *reinterpret_cast<const int*>(scene + SCENE_CURVE);
		g_map.automatic = scene[SCENE_AUTO] != 0;
		g_map.lock = scene[SCENE_LOCK] != 0;
		memcpy(g_map.f, floats, sizeof(g_map.f));
		if (!g_set)
		{
			return;
		}

		if (g_set & TONE_SET_CURVE)
		{
			*reinterpret_cast<int*>(scene + SCENE_CURVE) = g_mine.curve;
		}

		if (g_set & TONE_SET_AUTO)
		{
			scene[SCENE_AUTO] = g_mine.automatic ? 1 : 0;
		}

		if (g_set & TONE_SET_LOCK)
		{
			scene[SCENE_LOCK] = g_mine.lock ? 1 : 0;
		}

		for (int i = 0; i < TONE_FLOATS; ++i)
		{
			// (how fast the automatic exposure follows: the game makes it 0 or 1 itself at times, a cut for one;
			// left alone then)
			if ((g_set & (1u << i)) && !(i == 4 && (floats[i] == 0.0f || floats[i] == 1.0f)))
			{
				floats[i] = g_mine.f[i];
			}
		}
	}

	void hooked(unsigned char* scene, const unsigned char* view)
	{
		g_original(scene, view);
		if (scene)
		{
			apply(scene);
		}
	}
}

void tone_start()
{
	g_hooked = MH_CreateHook(reinterpret_cast<LPVOID>(R_SCENE_TONEMAP), reinterpret_cast<LPVOID>(&hooked), reinterpret_cast<LPVOID*>(&g_original)) == MH_OK &&
	           MH_EnableHook(reinterpret_cast<LPVOID>(R_SCENE_TONEMAP)) == MH_OK;
	theater_log(g_hooked ? "exposure: the scene's tone mapping is hooked" : "exposure: the scene's tone mapping could NOT be hooked");
}

bool tone_get(ToneState* map, ToneState* mine, std::uint32_t* set)
{
	std::lock_guard<std::mutex> lock(g_lock);
	*map = g_map;
	*mine = g_mine;
	*set = g_set;
	return g_hooked;
}

void tone_set(const ToneState& mine, const std::uint32_t set)
{
	std::lock_guard<std::mutex> lock(g_lock);
	g_mine = mine;
	g_set = set;
}
