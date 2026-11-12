// The sun as a light: its colour, its brightness and where it shines from, as IrisStudio's Sun section has
// them. This is the light on the map and its shadows, not the picture of the sun in the sky.
//
// The engine has overrides for exactly this, the ones a map's script uses (setsunlight, setsundirection),
// and the renderer applies them to the frame's copy of the sun light each frame in R_UpdateFrameSun
// (0x1405D3AF0: client, the frame's lights of 0x98 bytes each, the stage whose byte +0x16 is the sun's
// index; a light's colour is at +8 and its direction at +0x14). The overrides are a block at 0x14D859530:
//   +1  the colour is overridden          +8   the colour (3 floats)
//   +2  the brightness is overridden      +0x14 the brightness: the colour is normalized and scaled by it
//   +3  the direction is overridden       +0x18 the direction (3 floats)
//   +4  the direction is being turned     +5   the direction's change is taken up by the shadows
// (R_SetSunLightOverride 0x140571810, R_SetSunIntensityOverride 0x140571800, R_SetSunDirectionOverride
// 0x1405717C0 write them.) While playback has the sun changed the block is written each frame; what the
// game had in it is put back afterwards.
#include "theater.hpp"

#include <MinHook.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

namespace
{
	constexpr uintptr_t R_UPDATE_FRAME_SUN = 0x1405D3AF0, SUN_OVERRIDES = 0x14D859530;
	constexpr size_t OVERRIDES_SIZE = 0x40, LIGHT_SIZE = 0x98, LIGHT_COLOR = 8, LIGHT_DIRECTION = 0x14;
	constexpr float PI = 3.14159265358979f;

	using update_frame_sun_t = void (*)(int, unsigned char*, unsigned char*);
	update_frame_sun_t g_original_update_frame_sun = nullptr;

	std::mutex g_lock;
	// the map's own sun, as the frame's light has it before any override
	bool g_known = false;
	float g_map_color[3] = {1, 1, 1}, g_map_direction[3] = {0, 0, 1};
	// what the editor wants
	bool g_color_on = false, g_brightness_on = false, g_direction_on = false;
	float g_color[3] = {1, 1, 1}, g_brightness = 1.0f, g_direction[3] = {0, 0, 1};
	// what the game's overrides were before
	bool g_applied = false;
	unsigned char g_saved[OVERRIDES_SIZE] = {};

	float largest(const float* color)
	{
		const float most = color[0] > color[1] ? (color[0] > color[2] ? color[0] : color[2]) : (color[1] > color[2] ? color[1] : color[2]);
		return most > 0.0001f ? most : 0.0001f;
	}

	void hooked_update_frame_sun(const int client, unsigned char* lights, unsigned char* stage)
	{
		const TheaterState state = theater_state();
		auto* overrides = reinterpret_cast<unsigned char*>(SUN_OVERRIDES);
		{
			std::lock_guard<std::mutex> lock(g_lock);
			const int index = stage ? stage[0x16] : 0;
			unsigned char* light = index && lights ? lights + index * LIGHT_SIZE : nullptr;
			const bool wanted = state.playing && (g_color_on || g_brightness_on || g_direction_on);
			if (light && !g_applied)
			{
				memcpy(g_map_color, light + LIGHT_COLOR, sizeof(g_map_color));
				memcpy(g_map_direction, light + LIGHT_DIRECTION, sizeof(g_map_direction));
				g_known = true;
			}

			if (wanted)
			{
				if (!g_applied)
				{
					memcpy(g_saved, overrides, OVERRIDES_SIZE);
					g_applied = true;
				}

				if (g_color_on || g_brightness_on)
				{
					// both together: the engine normalizes the colour and scales it by the brightness
					const float* color = g_color_on ? g_color : g_map_color;
					const float most = largest(color);
					const float normal[3] = {color[0] / most, color[1] / most, color[2] / most};
					const float brightness = g_brightness_on ? g_brightness : largest(g_map_color);
					overrides[1] = 1;
					memcpy(overrides + 8, normal, sizeof(normal));
					overrides[2] = 1;
					memcpy(overrides + 0x14, &brightness, sizeof(brightness));
				}

				if (g_direction_on)
				{
					overrides[3] = 1;
					overrides[4] = 0;
					overrides[5] = 1;
					memcpy(overrides + 0x18, g_direction, sizeof(g_direction));
				}
			}
			else if (g_applied)
			{
				memcpy(overrides, g_saved, OVERRIDES_SIZE);
				g_applied = false;
				if (light)
				{
					// (should the lights not be a fresh copy each frame, the map's own again)
					memcpy(light + LIGHT_COLOR, g_map_color, sizeof(g_map_color));
					memcpy(light + LIGHT_DIRECTION, g_map_direction, sizeof(g_map_direction));
				}

				if (!state.playing)
				{
					g_color_on = g_brightness_on = g_direction_on = false;
				}
			}
		}

		g_original_update_frame_sun(client, lights, stage);
	}

	// AnglesToForward, as the game has it, and back
	void direction_of(const float pitch, const float yaw, float* out)
	{
		const float p = pitch * PI / 180.0f, y = yaw * PI / 180.0f;
		out[0] = std::cos(p) * std::cos(y);
		out[1] = std::cos(p) * std::sin(y);
		out[2] = -std::sin(p);
	}
}

SunState sun_state()
{
	std::lock_guard<std::mutex> lock(g_lock);
	SunState state;
	state.known = g_known;
	const float* color = g_color_on ? g_color : g_map_color;
	const float most = largest(color);
	for (int i = 0; i < 3; ++i)
	{
		state.color[i] = color[i] / most;
	}

	state.brightness = g_brightness_on ? g_brightness : largest(g_map_color);
	const float* direction = g_direction_on ? g_direction : g_map_direction;
	state.yaw = std::atan2(direction[1], direction[0]) * 180.0f / PI;
	state.pitch = std::atan2(-direction[2], std::sqrt(direction[0] * direction[0] + direction[1] * direction[1])) * 180.0f / PI;
	state.changed = g_color_on || g_brightness_on || g_direction_on;
	return state;
}

void sun_set_color(const float color[3])
{
	std::lock_guard<std::mutex> lock(g_lock);
	memcpy(g_color, color, sizeof(g_color));
	g_color_on = true;
}

void sun_set_brightness(const float brightness)
{
	std::lock_guard<std::mutex> lock(g_lock);
	g_brightness = brightness < 0.0f ? 0.0f : brightness;
	g_brightness_on = true;
}

void sun_set_direction(const float pitch, const float yaw)
{
	std::lock_guard<std::mutex> lock(g_lock);
	direction_of(pitch, yaw, g_direction);
	g_direction_on = true;
}

void sun_reset()
{
	std::lock_guard<std::mutex> lock(g_lock);
	g_color_on = g_brightness_on = g_direction_on = false;
}

void sun_start()
{
	MH_STATUS status = MH_CreateHook(reinterpret_cast<LPVOID>(R_UPDATE_FRAME_SUN), reinterpret_cast<LPVOID>(&hooked_update_frame_sun),
	                                 reinterpret_cast<LPVOID*>(&g_original_update_frame_sun));
	if (status == MH_OK)
	{
		status = MH_EnableHook(reinterpret_cast<LPVOID>(R_UPDATE_FRAME_SUN));
	}

	theater_log(status == MH_OK ? "sun: the frame's sun light hooked (colour, brightness and direction can be set)" : "sun: the frame's sun light NOT hooked");
}
