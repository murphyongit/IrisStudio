// Players' animation in slow motion.
//
// The editor's clock keeps the part of a millisecond, and gives it to the game for where things ARE (the
// frame's interpolation, theater.cpp), so a player glides evenly at any speed. But the game moves a player's
// ANIMATION on by whole milliseconds (the player animation, 0x140130C40 and 0x14000AE70: the entity, the
// player's record, the frame's milliseconds). When a frame is about a millisecond of the recording or less
// (0.1x and slower), the pose holds, then steps, while the body glides: the player jitters, and the editor's
// own actors (which run on the exact clock) do not.
//
// So after the game has moved a player's animation on, each animation in that player's tree is put forward
// by the part of a millisecond as well, at the pace it had at the game's last whole step (its time now less
// its time before, over the step's milliseconds); and before the game next touches the tree, the game's own
// times are put back, so that its whole-millisecond bookkeeping never sees the editor's.
//
// A tree's records ("infos", 0x48 bytes each in the array at 0x14B5B9E00, index 0 none): the animation's
// number at +0x12, the first child at +0xE, the next sibling at +0xC, the time (0..1) at +0x28, the time
// before at +0x2C, the times around at +0x30 and before at +0x32 (shorts). The tree's root info: tree +8.
#include "theater.hpp"

#include <MinHook.h>
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace
{
	constexpr std::uintptr_t PLAYER_ANIMATION = 0x140130C40, PLAYER_ANIMATION_OTHER = 0x14000AE70;
	constexpr std::uintptr_t XANIM_INFOS = 0x14B5B9E00, CG_ENTITIES = 0x141655F60;
	constexpr size_t INFO_SIZE = 0x48, CENTITY_SIZE = 0x288;
	using anim_t = std::uint64_t (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
	anim_t g_original = nullptr, g_original_other = nullptr;
	std::atomic<bool> g_on{true};
	void* g_trees[2048] = {};
	float g_pace[65536] = {};
	std::uint16_t g_pace_of[65536] = {};
	struct Moved
	{
		std::uint16_t info;
		float real, wrote;
	};
	struct Slot
	{
		int entity = -1, count = 0;
		Moved moved[64];
	};
	Slot g_slots[64];
	unsigned g_told = 0, g_calls = 0, g_moved = 0;

	unsigned char* info_at(const unsigned index) { return reinterpret_cast<unsigned char*>(XANIM_INFOS) + index * INFO_SIZE; }

	Slot* slot_of(const int entity, const bool make)
	{
		for (Slot& slot : g_slots)
		{
			if (slot.entity == entity)
			{
				return &slot;
			}
		}

		if (make)
		{
			for (Slot& slot : g_slots)
			{
				if (slot.entity < 0)
				{
					slot.entity = entity;
					slot.count = 0;
					return &slot;
				}
			}
		}

		return nullptr;
	}

	void put_back(Slot* slot)
	{
		__try
		{
			for (int i = 0; i < slot->count; ++i)
			{
				// (only if it is still what the editor wrote: the record may be another animation's by now)
				float* time = reinterpret_cast<float*>(info_at(slot->moved[i].info) + 0x28);
				if (memcmp(time, &slot->moved[i].wrote, sizeof(float)) == 0)
				{
					*time = slot->moved[i].real;
				}
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}

		slot->count = 0;
	}

	void walk(const unsigned index, const int step_ms, const float part, Slot* slot, const int depth, int* left)
	{
		if (!index || index >= 65536 || depth > 12 || --*left < 0)
		{
			return;
		}

		unsigned char* at = info_at(index);
		float* time = reinterpret_cast<float*>(at + 0x28);
		const std::uint16_t anim = *reinterpret_cast<const std::uint16_t*>(at + 0x12);
		if (step_ms > 0)
		{
			const float went = *time - *reinterpret_cast<const float*>(at + 0x2C) +
			                   static_cast<float>(*reinterpret_cast<const std::int16_t*>(at + 0x30) - *reinterpret_cast<const std::int16_t*>(at + 0x32));
			g_pace[index] = went > 0.0f && went < 0.5f ? went / static_cast<float>(step_ms) : 0.0f;
			g_pace_of[index] = anim;
		}

		const float on = g_pace_of[index] == anim ? g_pace[index] * part : 0.0f;
		if (on > 0.0f && slot->count < 64)
		{
			float put = *time + on;
			if (put >= 1.0f)
			{
				put = 0.99999f;
			}

			if (put > *time)
			{
				slot->moved[slot->count++] = {static_cast<std::uint16_t>(index), *time, put};
				*time = put;
				++g_moved;
			}
		}

		for (unsigned child = *reinterpret_cast<const std::uint16_t*>(at + 0xE); child; child = *reinterpret_cast<const std::uint16_t*>(info_at(child) + 0xC))
		{
			walk(child, step_ms, part, slot, depth + 1, left);
		}
	}

	void put_forward(void* tree, const int step_ms, const float part, Slot* slot)
	{
		__try
		{
			int left = 160;
			walk(*reinterpret_cast<const std::uint16_t*>(static_cast<unsigned char*>(tree) + 8), step_ms, part, slot, 0, &left);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
	}

	std::uint64_t run(const anim_t original, const std::uint64_t a, const std::uint64_t b, const std::uint64_t c, const std::uint64_t d)
	{
		const int entity = a >= CG_ENTITIES && a < CG_ENTITIES + 2048 * CENTITY_SIZE ? static_cast<int>((a - CG_ENTITIES) / CENTITY_SIZE) : -1;
		if (Slot* before = entity >= 0 ? slot_of(entity, false) : nullptr)
		{
			if (before->count)
			{
				put_back(before);
			}
		}

		const std::uint64_t result = original(a, b, c, d);
		++g_calls;
		const TheaterState state = theater_state();
		void* tree = entity >= 0 ? g_trees[entity] : nullptr;
		if (g_told < 4 && state.playing)
		{
			++g_told;
			char text[200];
			snprintf(text, sizeof(text), "animation: a player's animation moved on: given %p and %p, %d ms; entity %d, its tree %p", reinterpret_cast<void*>(a),
			         reinterpret_cast<void*>(b), static_cast<int>(static_cast<std::int32_t>(c)), entity, tree);
			theater_log(text);
		}

		if (g_on && tree && state.playing && state.caught_up && !state.scanning)
		{
			if (Slot* slot = slot_of(entity, true))
			{
				const double exact = state.position_exact;
				const float part = static_cast<float>(exact - std::floor(exact));
				const int step_ms = static_cast<int>(static_cast<std::int32_t>(c));
				put_forward(tree, step_ms < 0 || step_ms > 500 ? 0 : step_ms, part, slot);
			}
		}

		return result;
	}

	std::uint64_t hooked(const std::uint64_t a, const std::uint64_t b, const std::uint64_t c, const std::uint64_t d) { return run(g_original, a, b, c, d); }

	std::uint64_t hooked_other(const std::uint64_t a, const std::uint64_t b, const std::uint64_t c, const std::uint64_t d)
	{
		return run(g_original_other, a, b, c, d);
	}
}

void anim_smooth_start()
{
	const bool ok = MH_CreateHook(reinterpret_cast<LPVOID>(PLAYER_ANIMATION), reinterpret_cast<LPVOID>(&hooked), reinterpret_cast<LPVOID*>(&g_original)) == MH_OK &&
	                MH_CreateHook(reinterpret_cast<LPVOID>(PLAYER_ANIMATION_OTHER), reinterpret_cast<LPVOID>(&hooked_other), reinterpret_cast<LPVOID*>(&g_original_other)) == MH_OK &&
	                MH_EnableHook(reinterpret_cast<LPVOID>(PLAYER_ANIMATION)) == MH_OK && MH_EnableHook(reinterpret_cast<LPVOID>(PLAYER_ANIMATION_OTHER)) == MH_OK;
	theater_log(ok ? "animation: the players' animation is hooked (the part of a millisecond is given to it in slow motion)" : "animation: the players' animation could NOT be hooked");
}

void anim_smooth_note_tree(const int handle, void* tree)
{
	if (handle >= 0 && handle < 2048)
	{
		g_trees[handle] = tree;
	}
}

bool anim_smooth_on() { return g_on; }

void anim_smooth_set(const bool on) { g_on = on; }

void anim_smooth_status(unsigned* calls, unsigned* moved)
{
	*calls = g_calls;
	*moved = g_moved;
}
