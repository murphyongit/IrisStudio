// The thermal picture of a thermal scope (and of a scorestreak's thermal view).
//
// The client game decides it each frame in 0x1401D4520(client): off if 0x14013C2E0(the player's state) says so;
// on if the player's state has flag 8 at +0x5C (a script turned thermal vision on: a scorestreak), unless
// another flag; else on if 0x1401DE0C0(client, the player's state) says the thermal scope is up to the eye. The
// answer goes to the byte at 0x141643E96, to the renderer (0x1405D0560) and, when it changes, the "thermal"
// vision and "clut_thermal" colours are put on or taken off.
//
// The game decides by the PLAYER, not by the camera: with the editor's free, dolly, orbit or bone camera, or in
// third person, the picture went thermal whenever the recorded player looked down a thermal scope. So: in any
// camera but first person the answer is made "off"; and a setting of the Look tab makes it always on or off.
#include "theater.hpp"

#include <MinHook.h>
#include <windows.h>

#include <atomic>

namespace
{
	constexpr std::uintptr_t CG_UPDATE_THERMAL = 0x1401D4520, CG_THERMAL_SCOPE_UP = 0x1401DE0C0, PLAYER_FLAGS = 0x1414C175C;
	using update_t = void (*)(int);
	using scope_t = bool (*)(int, void*);
	update_t g_original_update = nullptr;
	scope_t g_original_scope = nullptr;
	std::atomic<int> g_mode{0};
	// for the call under way: 0 the game's own answer, 1 off, 2 on
	int g_now = 0;

	// (0.93) "Is a scope up to the eye" (0x14018D150: the player's state, and one more; thirteen callers): with
	// the camera away from the eyes it is not, whatever the recorded player does. The game dropped the player's
	// body from the picture while he looked down a scope (it only draws the scope's picture then).
	constexpr std::uintptr_t SCOPE_UP = 0x14018D150;
	using scope_up_t = std::uint64_t (*)(void*, std::uint64_t);
	scope_up_t g_original_scope_up = nullptr;

	std::uint64_t hooked_scope_up(void* player, const std::uint64_t more)
	{
		const std::uint64_t is = g_original_scope_up(player, more);
		if ((is & 0xFF) && theater_state().playing && camera_state().mode != CameraMode::FirstPerson)
		{
			return is & ~0xFFull;
		}

		return is;
	}

	bool hooked_scope(const int client, void* player)
	{
		return g_now == 1 ? false : g_now == 2 ? true : g_original_scope(client, player);
	}

	void hooked_update(const int client)
	{
		const TheaterState state = theater_state();
		g_now = 0;
		const int mode = g_mode;
		if (mode == 3 || !state.playing || client != 0)
		{
			// (3: all of it left to the game, nothing of the player's state touched: as before 0.88)
			g_original_update(client);
			return;
		}

		g_now = mode == 1 ? 1 : mode == 2 ? 2 : camera_state().mode == CameraMode::FirstPerson ? 0 : 1;
		if (g_now == 0)
		{
			g_original_update(client);
			return;
		}

		auto* flags = reinterpret_cast<unsigned*>(PLAYER_FLAGS);
		const unsigned kept = *flags;
		if (g_now == 1)
		{
			*flags &= ~8u;
		}

		g_original_update(client);
		*flags = (*flags & ~8u) | (kept & 8u);
		g_now = 0;
	}
}

void thermal_start()
{
	const bool ok = MH_CreateHook(reinterpret_cast<LPVOID>(CG_UPDATE_THERMAL), reinterpret_cast<LPVOID>(&hooked_update), reinterpret_cast<LPVOID*>(&g_original_update)) == MH_OK &&
	                MH_CreateHook(reinterpret_cast<LPVOID>(CG_THERMAL_SCOPE_UP), reinterpret_cast<LPVOID>(&hooked_scope), reinterpret_cast<LPVOID*>(&g_original_scope)) == MH_OK &&
	                MH_EnableHook(reinterpret_cast<LPVOID>(CG_UPDATE_THERMAL)) == MH_OK && MH_EnableHook(reinterpret_cast<LPVOID>(CG_THERMAL_SCOPE_UP)) == MH_OK;
	const bool scope = MH_CreateHook(reinterpret_cast<LPVOID>(SCOPE_UP), reinterpret_cast<LPVOID>(&hooked_scope_up), reinterpret_cast<LPVOID*>(&g_original_scope_up)) == MH_OK &&
	                   MH_EnableHook(reinterpret_cast<LPVOID>(SCOPE_UP)) == MH_OK;
	theater_log(scope ? "scope: the game's \"scope up to the eye\" is hooked (not so outside first person)" : "scope: \"scope up to the eye\" could NOT be hooked");
	theater_log(ok ? "thermal: the game's thermal picture is hooked (first person only; the Look tab can turn it on or off)" : "thermal: the game's thermal picture could NOT be hooked");
}

int thermal_mode() { return g_mode; }

void thermal_set_mode(const int mode) { g_mode = mode < 0 || mode > 3 ? 0 : mode; }
