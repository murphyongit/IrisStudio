// S1iris: actors. A player-like figure the editor puts into the match: a model made of a player's outfit, with an
// animation tree of the editor's own, played by the timeline (clips one after another, blended into each other),
// and handed to the renderer each frame with a place to stand.
//
// How IrisStudio's Black Ops version does it (t5/src/Actors.cpp), with this game's own functions:
//  - The animations the game has loaded are assets of kind 5 (XAnimParts: +0 name, +0xE frames, +0x10 flags (1:
//    it loops), +0x30 frames a second).
//  - A tree's animations ("XAnim_s") are a count and 16-byte entries from +8: a word how many children (0 for an
//    animation), a word its parent's entry, then at +8 either the animation, or for a blend a word of flags and
//    a word the first child's entry (XAnimCreateAnims 0x1404E5000, XAnimBlend 0x1404E3540, XAnimCreate
//    0x1404E4FC0 show the layout; the editor fills its own). A tree is 16 bytes: the animations, and a word (+8)
//    the place of its root's state among the game's states (0x48 bytes each from 0x14B5B9E00), 0 for none.
//  - XAnimSetGoalWeight 0x1404E8420 (model, entry, weight, time to get there, rate, a notify's name and kind,
//    restart) gives an entry a weight (making its state and its parents'); an entry's time is in its state
//    (+0x28 and +0x2C the time from 0 to 1, +0x30 how many times round, a word +2 the next notify: -1), found by
//    XAnimGetInfoIndex_r 0x1404E6390 (tree, entry, the root's state). The game's own XAnimPlaySimpleTreeAnim
//    (0x1404E7350, a script model's animation) does exactly these two things.
//  - XAnimGetAbsDelta 0x1404E5EB0 (animations, entry, a rotation out (4), an offset out (3), time 0 to 1): how
//    far the animation has carried the figure by then (a walk walks, a death falls back).
//  - A model is drawn by R_AddDObjToScene (0x1405C9610: the model, its pose, the entity's number, flags, where
//    it is lit from, 0.0, 0), the pose being a client entity (0x288 bytes each from 0x141655F60: the origin at
//    +0x20, each of its three numbers kept XORed with the inverse of the low half of where the origin is kept;
//    angles +0x44; its number a word +0x144; its kind a byte +0x150). Actors take the entities 1984 and up,
//    which no match uses, as their handles and poses.
//  - Going back in the recording starts the client game again, which lets go of every model (Com_InitDObj) but
//    not of the trees' states: the actors' are freed before (actors_release), XAnimFreeInfo 0x1404E5CC0.
#include "theater.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <windows.h>

void theater_log(const char* text);

namespace
{
	constexpr std::uintptr_t DB_ENUM_XASSETS = 0x14026EC80, COM_GET_CLIENT_DOBJ = 0x1403D2BA0, COM_SAFE_CLIENT_DOBJ_FREE = 0x1403D2DF0;
	constexpr std::uintptr_t XANIM_SET_GOAL_WEIGHT = 0x1404E8420, XANIM_GET_INFO_INDEX = 0x1404E6390, XANIM_FREE_INFO = 0x1404E5CC0;
	constexpr std::uintptr_t XANIM_INFOS = 0x14B5B9E00, XANIM_GET_ABS_DELTA = 0x1404E5EB0;
	constexpr std::uintptr_t R_ADD_DOBJ_TO_SCENE = 0x1405C9610, CG_ENTITIES = 0x141655F60, SL_FIND_STRING = 0x1403F11C0;
	constexpr std::uintptr_t CG_DOBJ_GET_WORLD_TAG_MATRIX = 0x140190D00;
	constexpr size_t CENTITY_SIZE = 0x288, CENTITY_ORIGIN = 0x20, CENTITY_ANGLES = 0x44, CENTITY_NUMBER = 0x144, CENTITY_KIND = 0x150;
	constexpr size_t INFO_SIZE = 0x48;
	constexpr int ASSET_XANIM = 5, MAX_ANIMATIONS = 32768;
	// (the entities' numbers go to 2045; a match's own stay far below these)
	constexpr int FIRST_HANDLE = 1984, PREVIEW_HANDLE = 1983, PREVIEW = ACTORS_MAX;
	constexpr float TO_RAD = 3.14159265f / 180.0f, TO_DEG = 180.0f / 3.14159265f;

	// the parts of a figure the small bone view draws (see actors_preview_bones)
	const char* const BONES[ACTOR_BONES] = {
		"j_mainroot", "j_spinelower", "j_spineupper", "j_spine4", "j_neck", "j_head",
		"j_clavicle_le", "j_shoulder_le", "j_elbow_le", "j_wrist_le", "j_clavicle_ri", "j_shoulder_ri", "j_elbow_ri", "j_wrist_ri",
		"j_hip_le", "j_knee_le", "j_ankle_le", "j_ball_le", "j_hip_ri", "j_knee_ri", "j_ankle_ri", "j_ball_ri",
	};

	std::mutex g_lock;
	Actor g_actors[ACTORS_MAX];
	ModelsOutfit g_outfits[ACTORS_MAX + 1];  // (the last: the preview's)
	unsigned g_outfit_changes[ACTORS_MAX + 1] = {};
	ActorNow g_now[ACTORS_MAX];
	int g_count = 0, g_next_number = 1;

	// the animations the game has loaded, by name
	std::map<std::string, void*> g_animations;
	bool g_listed = false;

	// the preview: one animation on a figure that is not drawn, for its bones
	char g_preview_wanted[64] = "";
	float g_preview_bones[ACTOR_BONES][3] = {};
	bool g_preview_has[ACTOR_BONES] = {};
	float g_preview_fraction = 0.0f;
	bool g_preview_ready = false;

	// what the game holds for an actor (the game's thread only)
	struct Built
	{
		void* tree = nullptr;
		void* anims = nullptr;
		std::vector<std::string> names;  // the tree's entries after its root
		std::vector<void*> parts;
		std::vector<float> weights;      // as last given
		unsigned outfit = 0;             // the outfit's changes it was made with
		bool made = false;               // its model was made (it is found again by its handle each frame)
		bool broken = false;             // a fault with it: left alone until it is changed
		ULONGLONG tried = 0;
		char said[96] = "";
	};
	// (by handle, which an actor keeps for as long as it is there: the list's places move when one is removed)
	Built g_built[ACTORS_MAX + 1];
	int g_told = 0;
	std::vector<int> g_removed;  // the handles of actors removed, to let go of on the game's thread

	int slot_of(const int handle)
	{
		return handle == PREVIEW_HANDLE ? PREVIEW : handle - FIRST_HANDLE;
	}

	void say(const char* format, ...)
	{
		if (g_told >= 300)
		{
			return;
		}

		++g_told;
		char text[500];
		va_list arguments;
		va_start(arguments, format);
		vsnprintf(text, sizeof(text), format, arguments);
		va_end(arguments);
		theater_log(text);
	}

	struct Found
	{
		void** out;
		int count;
	};

	void take(void* header, void* context)
	{
		auto* found = static_cast<Found*>(context);
		if (found->count < MAX_ANIMATIONS)
		{
			found->out[found->count++] = header;
		}
	}

	int enumerate(void** out)
	{
		Found found = {out, 0};
		__try
		{
			reinterpret_cast<void (*)(int, void (*)(void*, void*), const void*, bool)>(DB_ENUM_XASSETS)(ASSET_XANIM, take, &found, false);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return 0;
		}

		return found.count;
	}

	const char* asset_name(const void* asset)
	{
		if (!asset || IsBadReadPtr(asset, 0x38))
		{
			return nullptr;
		}

		const char* name = *static_cast<const char* const*>(asset);
		if (!name || IsBadStringPtrA(name, 100))
		{
			return nullptr;
		}

		const size_t length = strnlen(name, 100);
		return length >= 2 && length < 64 ? name : nullptr;
	}

	// (with the lock held)
	void list_animations()
	{
		static void* all[MAX_ANIMATIONS];
		const int count = enumerate(all);
		g_animations.clear();
		for (int i = 0; i < count; ++i)
		{
			if (const char* name = asset_name(all[i]))
			{
				g_animations[name] = all[i];
			}
		}

		g_listed = true;
		char text[120];
		snprintf(text, sizeof(text), "actors: %zu animations loaded", g_animations.size());
		theater_log(text);
	}

	// An animation's length in seconds, and whether it is one that goes round.
	float length_of(const void* parts, bool* loops)
	{
		__try
		{
			const auto* at = static_cast<const unsigned char*>(parts);
			const int frames = *reinterpret_cast<const std::uint16_t*>(at + 0xE);
			const float rate = *reinterpret_cast<const float*>(at + 0x30);
			if (loops)
			{
				*loops = (at[0x10] & 1) != 0;
			}

			return rate > 0.01f && frames > 0 ? frames / rate : 0.0f;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return 0.0f;
		}
	}

	unsigned char* entity_at(const int handle)
	{
		return reinterpret_cast<unsigned char*>(CG_ENTITIES + static_cast<size_t>(handle) * CENTITY_SIZE);
	}

	void* model_of(const int handle)
	{
		__try
		{
			return reinterpret_cast<void* (*)(int, int)>(COM_GET_CLIENT_DOBJ)(handle, 0);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return nullptr;
		}
	}

	// The tree's memory, as the game lays it out (see the top).
	void make_tree(Built& built)
	{
		const size_t count = built.parts.size();
		auto* anims = static_cast<unsigned char*>(calloc(1, 8 + 16 * (count + 1)));
		auto* tree = static_cast<unsigned char*>(calloc(1, 16));
		if (!anims || !tree)
		{
			free(anims);
			free(tree);
			return;
		}

		*reinterpret_cast<std::uint32_t*>(anims) = static_cast<std::uint32_t>(count + 1);
		unsigned char* root = anims + 8;
		*reinterpret_cast<std::uint16_t*>(root) = static_cast<std::uint16_t>(count);
		*reinterpret_cast<std::uint16_t*>(root + 0xA) = 1;
		for (size_t i = 0; i < count; ++i)
		{
			unsigned char* entry = anims + 8 + 16 * (i + 1);
			*reinterpret_cast<void**>(entry + 8) = built.parts[i];
		}

		*reinterpret_cast<void**>(tree) = anims;
		built.anims = anims;
		built.tree = tree;
	}

	// The model let go of (if it is still the one made with this tree), then the tree's states, then its memory.
	// game_holds: the game still has what was made (false after the client game began again: only forgotten).
	bool let_go(const int handle, void* tree, const bool game_holds)
	{
		__try
		{
			if (game_holds)
			{
				void* model = reinterpret_cast<void* (*)(int, int)>(COM_GET_CLIENT_DOBJ)(handle, 0);
				if (model && *static_cast<void**>(model) == tree)
				{
					reinterpret_cast<void (*)(int)>(COM_SAFE_CLIENT_DOBJ_FREE)(handle);
				}

				const std::uint16_t root = *reinterpret_cast<const std::uint16_t*>(static_cast<unsigned char*>(tree) + 8);
				if (root)
				{
					reinterpret_cast<void (*)(void*, unsigned, void*)>(XANIM_FREE_INFO)(tree, root, nullptr);
				}
			}

			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	void unbuild(const int slot, const int handle, const bool game_holds)
	{
		Built& built = g_built[slot];
		if (built.tree)
		{
			if (!let_go(handle, built.tree, game_holds))
			{
				say("actors: a fault letting go of entity %d's model", handle);
				// (its memory is left: something of the game's may still point at it)
			}
			else
			{
				free(built.tree);
				free(built.anims);
			}
		}

		const bool broken = built.broken;
		built = Built();
		built.broken = broken;
	}

	bool weigh(void* model, const unsigned entry, const float weight)
	{
		__try
		{
			reinterpret_cast<void* (*)(void*, unsigned, float, float, float, unsigned, unsigned, int)>(XANIM_SET_GOAL_WEIGHT)(
				model, entry, weight, 0.0f, 0.0f, 0, 0, 0);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	bool set_time(void* tree, const unsigned entry, const float fraction)
	{
		__try
		{
			const std::uint16_t root = *reinterpret_cast<const std::uint16_t*>(static_cast<unsigned char*>(tree) + 8);
			if (!root)
			{
				return true;
			}

			const unsigned info = reinterpret_cast<unsigned (*)(void*, unsigned, unsigned)>(XANIM_GET_INFO_INDEX)(tree, entry, root);
			if (info && info < 65536)
			{
				unsigned char* at = reinterpret_cast<unsigned char*>(XANIM_INFOS) + info * INFO_SIZE;
				*reinterpret_cast<std::int32_t*>(at + 0x30) = 0;
				*reinterpret_cast<float*>(at + 0x28) = fraction;
				*reinterpret_cast<float*>(at + 0x2C) = fraction;
				*reinterpret_cast<std::uint16_t*>(at + 2) = 0xFFFF;
			}

			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	// How far an animation has carried the figure by a time (0 to 1), and how far it has turned it (degrees).
	bool carried(void* anims, const unsigned entry, const float fraction, float offset[3], float* turned)
	{
		float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f}, moved[4] = {};
		__try
		{
			reinterpret_cast<void (*)(void*, unsigned, float*, float*, float)>(XANIM_GET_ABS_DELTA)(anims, entry, rotation, moved, fraction);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}

		for (int i = 0; i < 3; ++i)
		{
			offset[i] = std::isfinite(moved[i]) && std::fabs(moved[i]) < 100000.0f ? moved[i] : 0.0f;
		}

		const float x = rotation[0], y = rotation[1], z = rotation[2], w = rotation[3];
		const float yaw = std::atan2(2.0f * (w * z + x * y), 1.0f - 2.0f * (y * y + z * z)) * TO_DEG;
		*turned = std::isfinite(yaw) ? yaw : 0.0f;
		return true;
	}

	// The pose: an entity of the client game's that no match uses, told where the figure stands.
	bool pose(const int handle, const float origin[3], const float yaw)
	{
		__try
		{
			unsigned char* entity = entity_at(handle);
			auto* kept = reinterpret_cast<std::uint32_t*>(entity + CENTITY_ORIGIN);
			const std::uint32_t key = ~static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(kept));
			for (int i = 0; i < 3; ++i)
			{
				std::uint32_t bits;
				memcpy(&bits, &origin[i], sizeof(bits));
				kept[i] = bits ^ key;
			}

			auto* angles = reinterpret_cast<float*>(entity + CENTITY_ANGLES);
			angles[0] = 0.0f;
			angles[1] = yaw;
			angles[2] = 0.0f;
			*reinterpret_cast<std::int16_t*>(entity + CENTITY_NUMBER) = static_cast<std::int16_t>(handle);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	bool draw(void* model, const int handle, const float origin[3])
	{
		// (lit as from its middle)
		float lit[4] = {origin[0], origin[1], origin[2] + 36.0f, 0.0f};
		__try
		{
			reinterpret_cast<void (*)(void*, void*, int, unsigned, const float*, float, int)>(R_ADD_DOBJ_TO_SCENE)(
				model, entity_at(handle), handle, 0, lit, 0.0f, 0);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	bool bone(const int handle, void* model, const char* name, float origin[3])
	{
		float axis[3][3];
		__try
		{
			const unsigned tag = reinterpret_cast<unsigned (*)(const char*)>(SL_FIND_STRING)(name);
			return tag && reinterpret_cast<int (*)(const void*, const void*, unsigned, float (*)[3], float*)>(CG_DOBJ_GET_WORLD_TAG_MATRIX)(
				              entity_at(handle), model, tag, axis, origin) != 0;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	// An entity the editor may take: nothing of the game's is there.
	bool entity_free(const int handle, void* tree)
	{
		__try
		{
			const unsigned char* entity = entity_at(handle);
			const int number = *reinterpret_cast<const std::int16_t*>(entity + CENTITY_NUMBER);
			if ((number != 0 && number != handle) || entity[CENTITY_KIND] != 0)
			{
				return false;
			}

			void* model = reinterpret_cast<void* (*)(int, int)>(COM_GET_CLIENT_DOBJ)(handle, 0);
			return !model || *static_cast<void**>(model) == tree;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	float smooth(const float t)
	{
		const float c = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
		return c * c * (3.0f - 2.0f * c);
	}

	// What a figure shows at a time: for each of its clips a weight and a time (0 to 1), where it stands and
	// which way it faces.
	struct Shown
	{
		bool any = false;
		int clip = -1;
		float weights[ACTOR_CLIPS_MAX] = {};
		float times[ACTOR_CLIPS_MAX] = {};
		float origin[3] = {};
		float yaw = 0.0f;
	};

	// How far into a clip's animation the time is: whole times round, and the part of one.
	void clip_time(const ActorClip& clip, const float length, const bool loops, const double now, int* rounds, float* fraction)
	{
		*rounds = 0;
		*fraction = 0.0f;
		if (length <= 0.0f)
		{
			return;
		}

		const double end = static_cast<double>(clip.at_ms) + clip.length_ms;
		const double played = ((now < end ? now : end) - clip.at_ms) / 1000.0 * (clip.speed > 0.0f ? clip.speed : 1.0f);
		if (played <= 0.0)
		{
			return;
		}

		const double turns = played / length;
		const bool round = clip.loop < 0 ? loops : clip.loop != 0;
		if (round)
		{
			*rounds = static_cast<int>(turns);
			*fraction = static_cast<float>(turns - *rounds);
		}
		else
		{
			*fraction = turns >= 1.0 ? 1.0f : static_cast<float>(turns);
		}
	}

	Shown shown_at(const Actor& actor, const Built& built, const double now)
	{
		Shown shown;
		memcpy(shown.origin, actor.origin, sizeof(shown.origin));
		shown.yaw = actor.yaw;
		const int count = actor.clip_count < static_cast<int>(built.parts.size()) ? actor.clip_count : static_cast<int>(built.parts.size());
		// the clip the time is in: the last that has begun
		int playing = -1;
		for (int i = 0; i < count; ++i)
		{
			if (now >= actor.clips[i].at_ms && (playing < 0 || actor.clips[i].at_ms >= actor.clips[playing].at_ms))
			{
				playing = i;
			}
		}

		if (playing < 0)
		{
			return shown;
		}

		const ActorClip& clip = actor.clips[playing];
		if (!actor.stays && now >= static_cast<double>(clip.at_ms) + clip.length_ms)
		{
			// (after its last clip, for one that does not stay)
			bool later = false;
			for (int i = 0; i < count; ++i)
			{
				later = later || actor.clips[i].at_ms > clip.at_ms;
			}

			if (!later)
			{
				return shown;
			}
		}

		shown.any = true;
		shown.clip = playing;
		// the one before it, while this one blends in over it
		int before = -1;
		for (int i = 0; i < count; ++i)
		{
			if (i != playing && actor.clips[i].at_ms <= clip.at_ms && (before < 0 || actor.clips[i].at_ms > actor.clips[before].at_ms))
			{
				before = i;
			}
		}

		float in = 1.0f;
		if (before >= 0 && clip.blend_ms > 0)
		{
			in = smooth(static_cast<float>((now - clip.at_ms) / clip.blend_ms));
		}

		shown.weights[playing] = in;
		if (before >= 0 && in < 1.0f)
		{
			shown.weights[before] = 1.0f - in;
		}

		// each clip that has begun, in the order they begin: its time, and how far it has carried the figure
		int order[ACTOR_CLIPS_MAX];
		int ordered = 0;
		for (int i = 0; i < count; ++i)
		{
			if (now >= actor.clips[i].at_ms)
			{
				order[ordered++] = i;
			}
		}

		std::sort(order, order + ordered, [&](const int a, const int b) { return actor.clips[a].at_ms < actor.clips[b].at_ms; });
		for (int o = 0; o < ordered; ++o)
		{
			const int i = order[o];
			bool loops = false;
			const float length = length_of(built.parts[static_cast<size_t>(i)], &loops);
			int rounds = 0;
			float fraction = 0.0f;
			// (a clip stops where the next begins)
			ActorClip until = actor.clips[i];
			if (o + 1 < ordered)
			{
				const std::uint32_t next = actor.clips[order[o + 1]].at_ms;
				// (the one blending out runs on under the blend)
				const std::uint32_t over = i == before ? clip.blend_ms : 0;
				if (next + over < until.at_ms + until.length_ms)
				{
					until.length_ms = next + over - until.at_ms;
				}
			}

			clip_time(until, length, loops, now, &rounds, &fraction);
			shown.times[i] = fraction;
			if (!actor.moves || !built.anims)
			{
				continue;
			}

			float offset[3] = {}, whole[3] = {}, turned = 0.0f, whole_turn = 0.0f;
			if (!carried(built.anims, static_cast<unsigned>(i + 1), fraction, offset, &turned))
			{
				continue;
			}

			if (rounds > 0 && carried(built.anims, static_cast<unsigned>(i + 1), 1.0f, whole, &whole_turn))
			{
				for (int k = 0; k < 3; ++k)
				{
					offset[k] += whole[k] * rounds;
				}

				turned += whole_turn * rounds;
			}

			const float c = std::cos(shown.yaw * TO_RAD), s = std::sin(shown.yaw * TO_RAD);
			shown.origin[0] += offset[0] * c - offset[1] * s;
			shown.origin[1] += offset[0] * s + offset[1] * c;
			shown.origin[2] += offset[2];
			shown.yaw += turned;
		}

		return shown;
	}

	// (the game's thread) The model and tree for a figure, made if they are not there or no longer fit.
	void* ensure(const int slot, const int handle, const std::vector<std::string>& names, const ModelsOutfit& outfit, const unsigned outfit_changes)
	{
		Built& built = g_built[slot];
		if (built.names != names || built.outfit != outfit_changes)
		{
			// (changed: made again, and tried again after a fault)
			if (built.tree)
			{
				unbuild(slot, handle, !built.broken);
			}

			built.broken = false;
			built.names = names;
			built.outfit = outfit_changes;
		}

		if (built.broken)
		{
			return nullptr;
		}

		if (built.tree && built.made)
		{
			void* model = model_of(handle);
			if (model && *static_cast<void**>(model) == built.tree)
			{
				return model;
			}

			// (the game let go of it: the client game began again without the editor's doing)
			say("actors: entity %d's model was gone; made again", handle);
			unbuild(slot, handle, false);
		}

		const ULONGLONG tick = GetTickCount64();
		if (tick - built.tried < 500)
		{
			return nullptr;
		}

		built.tried = tick;
		if (names.empty() || outfit.count == 0)
		{
			return nullptr;
		}

		if (!built.tree)
		{
			std::vector<void*> parts;
			{
				std::lock_guard<std::mutex> lock(g_lock);
				if (!g_listed)
				{
					list_animations();
				}

				for (const std::string& name : names)
				{
					const auto found = g_animations.find(name);
					if (found == g_animations.end())
					{
						if (strcmp(built.said, name.c_str()) != 0)
						{
							strncpy_s(built.said, name.c_str(), _TRUNCATE);
							say("actors: the animation %s is not loaded in this match", name.c_str());
						}

						return nullptr;
					}

					parts.push_back(found->second);
				}
			}

			if (!entity_free(handle, nullptr))
			{
				say("actors: entity %d is in use by the match; the actor is not made", handle);
				built.broken = true;
				return nullptr;
			}

			built.names = names;
			built.outfit = outfit_changes;
			built.parts = parts;
			make_tree(built);
			if (!built.tree)
			{
				return nullptr;
			}
		}

		char missing[64] = "";
		void* model = models_make_actor(handle, outfit, built.tree, missing);
		if (!model)
		{
			if (strcmp(built.said, missing) != 0)
			{
				strncpy_s(built.said, missing, _TRUNCATE);
				say("actors: entity %d's model could not be made: %s is not in memory", handle, missing[0] ? missing : "its outfit");
			}

			return nullptr;
		}

		built.made = true;
		built.weights.assign(names.size(), 0.0f);
		built.said[0] = 0;
		say("actors: entity %d made: %d parts, %zu animations", handle, outfit.count, names.size());
		return model;
	}

	// The weights and times given to the tree; false at a fault.
	bool animate(Built& built, void* model, const float* weights, const float* times)
	{
		const size_t count = built.parts.size();
		built.weights.resize(count, 0.0f);
		for (size_t i = 0; i < count; ++i)
		{
			// (the game takes a weight under a thousandth for none)
			const float weight = weights[i] < 0.002f ? 0.0f : weights[i];
			if (weight == 0.0f && built.weights[i] == 0.0f)
			{
				continue;
			}

			if (!weigh(model, static_cast<unsigned>(i + 1), weight))
			{
				return false;
			}

			if (weight > 0.0f && !set_time(built.tree, static_cast<unsigned>(i + 1), times[i]))
			{
				return false;
			}

			built.weights[i] = weight;
		}

		return true;
	}
}

int actors_list(Actor* out, const int max)
{
	std::lock_guard<std::mutex> lock(g_lock);
	const int count = g_count < max ? g_count : max;
	for (int i = 0; i < count; ++i)
	{
		out[i] = g_actors[i];
	}

	return count;
}

bool actors_now(const int index, ActorNow* out)
{
	std::lock_guard<std::mutex> lock(g_lock);
	if (index < 0 || index >= g_count)
	{
		return false;
	}

	*out = g_now[index];
	return true;
}

// The entities the actors' models are made under.
int actors_handles(int* out, const int max)
{
	std::lock_guard<std::mutex> lock(g_lock);
	const int count = g_count < max ? g_count : max;
	for (int i = 0; i < count; ++i)
	{
		out[i] = g_actors[i].handle;
	}

	return count;
}

// A new actor at a place, facing a way, dressed as a player is (an entity whose model the game has made), with
// one clip beginning at a time of the recording. -1: no room, or no such player.
int actors_add(const float origin[3], const float yaw, const int dressed_as, const std::uint32_t ms)
{
	ModelsOutfit outfit;
	if (!models_outfit_of(dressed_as, &outfit))
	{
		return -1;
	}

	std::lock_guard<std::mutex> lock(g_lock);
	if (g_count >= ACTORS_MAX)
	{
		return -1;
	}

	// a handle none of the others has
	int handle = FIRST_HANDLE;
	for (bool taken = true; taken; )
	{
		taken = false;
		for (int i = 0; i < g_count; ++i)
		{
			if (g_actors[i].handle == handle)
			{
				taken = true;
				++handle;
			}
		}
	}

	if (!g_listed)
	{
		list_animations();
	}

	Actor actor;
	actor.handle = handle;
	snprintf(actor.name, sizeof(actor.name), "actor%d", g_next_number++);
	memcpy(actor.origin, origin, sizeof(actor.origin));
	actor.yaw = yaw;
	// standing, to begin with
	for (const char* name : {"pb_stand_alert", "pb_stand_alert_pistol", "pb_stand_alert_rpg"})
	{
		if (g_animations.count(name))
		{
			ActorClip& clip = actor.clips[0];
			strncpy_s(clip.anim, name, _TRUNCATE);
			clip.at_ms = ms;
			clip.length_ms = 5000;
			actor.clip_count = 1;
			break;
		}
	}

	g_actors[g_count] = actor;
	g_outfits[g_count] = outfit;
	g_outfit_changes[g_count] = 1;
	g_now[g_count] = ActorNow();
	return g_count++;
}

void actors_set(const int index, const Actor& actor)
{
	std::lock_guard<std::mutex> lock(g_lock);
	if (index >= 0 && index < g_count)
	{
		const int handle = g_actors[index].handle;
		g_actors[index] = actor;
		g_actors[index].handle = handle;
		g_actors[index].clip_count = actor.clip_count < 0 ? 0 : actor.clip_count > ACTOR_CLIPS_MAX ? ACTOR_CLIPS_MAX : actor.clip_count;
	}
}

// Dressed as a player is now (the editor's own changes for the actor stay on top of it).
bool actors_dress(const int index, const int dressed_as)
{
	ModelsOutfit outfit;
	if (!models_outfit_of(dressed_as, &outfit))
	{
		return false;
	}

	std::lock_guard<std::mutex> lock(g_lock);
	if (index < 0 || index >= g_count)
	{
		return false;
	}

	g_outfits[index] = outfit;
	++g_outfit_changes[index];
	return true;
}

void actors_remove(const int index)
{
	int handle = -1;
	{
		std::lock_guard<std::mutex> lock(g_lock);
		if (index < 0 || index >= g_count)
		{
			return;
		}

		handle = g_actors[index].handle;
		// (what the game holds is let go of on the game's thread: see actors_scene_frame)
		g_removed.push_back(handle);
		for (int i = index; i + 1 < g_count; ++i)
		{
			g_actors[i] = g_actors[i + 1];
			g_outfits[i] = g_outfits[i + 1];
			g_outfit_changes[i] = g_outfit_changes[i + 1];
			g_now[i] = g_now[i + 1];
		}

		--g_count;
	}

	models_drop(handle);
}

// The animations the game has loaded whose names have a text in them, of a group: 0 a player's body, 1 deaths,
// 2 standing, 3 crouching, 4 prone, 5 moving, 6 every animation there is.
int actors_animations(const char* text, const int group, char names[][64], const int max)
{
	std::lock_guard<std::mutex> lock(g_lock);
	if (!g_listed)
	{
		list_animations();
	}

	std::string wanted = text ? text : "";
	std::transform(wanted.begin(), wanted.end(), wanted.begin(), [](const unsigned char c) { return static_cast<char>(tolower(c)); });
	int count = 0;
	for (const auto& animation : g_animations)
	{
		const std::string& name = animation.first;
		const bool body = name.rfind("pb_", 0) == 0;
		const bool death = name.find("death") != std::string::npos;
		bool fits = true;
		switch (group)
		{
		case 0: fits = body; break;
		case 1: fits = death && (body || name.rfind("mp_", 0) == 0 || name.rfind("exo", 0) == 0); break;
		case 2: fits = body && !death && name.find("stand") != std::string::npos; break;
		case 3: fits = body && !death && name.find("crouch") != std::string::npos; break;
		case 4: fits = body && !death && name.find("prone") != std::string::npos; break;
		case 5:
			fits = body && !death && (name.find("walk") != std::string::npos || name.find("run") != std::string::npos ||
			                          name.find("sprint") != std::string::npos || name.find("jump") != std::string::npos);
			break;
		default: break;
		}

		if (fits && (wanted.empty() || name.find(wanted) != std::string::npos) && count < max)
		{
			strncpy_s(names[count++], 64, name.c_str(), _TRUNCATE);
		}
	}

	return count;
}

// An animation's length in seconds (0: not loaded), and whether it is one that goes round.
float actors_animation_length(const char* name, bool* loops)
{
	std::lock_guard<std::mutex> lock(g_lock);
	if (!g_listed)
	{
		list_animations();
	}

	const auto found = g_animations.find(name);
	if (loops)
	{
		*loops = false;
	}

	return found == g_animations.end() ? 0.0f : length_of(found->second, loops);
}

// The small bone view: an animation ("" for none) played on a figure that is not drawn, dressed as the actor in
// a place of the list.
void actors_preview(const char* animation, const int dressed_as_actor)
{
	std::lock_guard<std::mutex> lock(g_lock);
	strncpy_s(g_preview_wanted, animation ? animation : "", _TRUNCATE);
	if (dressed_as_actor >= 0 && dressed_as_actor < g_count && g_outfits[PREVIEW].count == 0)
	{
		g_outfits[PREVIEW] = g_outfits[dressed_as_actor];
		++g_outfit_changes[PREVIEW];
	}

	if (!g_preview_wanted[0])
	{
		g_preview_ready = false;
	}
}

// Where the preview's bones are (from where it stands), and which of them its model has; how far through the
// animation it is. False: nothing to show yet.
bool actors_preview_bones(float out[ACTOR_BONES][3], bool has[ACTOR_BONES], float* fraction)
{
	std::lock_guard<std::mutex> lock(g_lock);
	if (!g_preview_ready)
	{
		return false;
	}

	memcpy(out, g_preview_bones, sizeof(g_preview_bones));
	memcpy(has, g_preview_has, sizeof(g_preview_has));
	*fraction = g_preview_fraction;
	return true;
}

// (the game's thread) Before the recording is gone back in: the client game begins again, and what was made in
// it must not be kept.
void actors_release()
{
	for (int slot = 0; slot <= ACTORS_MAX; ++slot)
	{
		if (g_built[slot].tree)
		{
			unbuild(slot, slot == PREVIEW ? PREVIEW_HANDLE : FIRST_HANDLE + slot, true);
		}
	}
}

// (the game's thread, as the scene is about to be drawn) Each actor made if it is not, given its animations'
// weights and times for the recording's time, and put into the scene where it stands.
void actors_scene_frame(const TheaterState& state)
{
	// what was removed since the last frame
	std::vector<int> removed;
	Actor actors[ACTORS_MAX];
	int count = 0;
	char preview[64];
	{
		std::lock_guard<std::mutex> lock(g_lock);
		removed.swap(g_removed);
		count = g_count;
		for (int i = 0; i < count; ++i)
		{
			actors[i] = g_actors[i];
		}

		strncpy_s(preview, g_preview_wanted, _TRUNCATE);
	}

	for (const int handle : removed)
	{
		const int slot = slot_of(handle);
		if (slot >= 0 && slot < ACTORS_MAX)
		{
			if (g_built[slot].tree)
			{
				unbuild(slot, handle, state.playing);
			}

			g_built[slot] = Built();
		}
	}

	if (!state.playing)
	{
		// (the match is over: its models went with it; the trees' states are the game's to clear)
		for (int slot = 0; slot <= ACTORS_MAX; ++slot)
		{
			if (g_built[slot].tree)
			{
				free(g_built[slot].tree);
				free(g_built[slot].anims);
			}

			g_built[slot] = Built();
		}

		std::lock_guard<std::mutex> lock(g_lock);
		g_listed = false;
		g_animations.clear();
		g_preview_ready = false;
		g_told = 0;
		for (ActorNow& now : g_now)
		{
			now = ActorNow();
		}

		return;
	}

	// (not while the recording is being run up to a place: the client game is not whole then)
	if (!state.settled)
	{
		return;
	}

	const double now = state.position_exact;
	for (int i = 0; i < count; ++i)
	{
		const Actor& actor = actors[i];
		ActorNow status;
		std::vector<std::string> names;
		for (int c = 0; c < actor.clip_count; ++c)
		{
			names.emplace_back(actor.clips[c].anim);
		}

		const int slot = slot_of(actor.handle);
		if (slot < 0 || slot >= ACTORS_MAX)
		{
			continue;
		}

		const bool wanted = actor.on && actor.clip_count > 0;
		Shown shown;
		void* model = nullptr;
		if (wanted)
		{
			ModelsOutfit outfit;
			unsigned changes = 0;
			{
				std::lock_guard<std::mutex> lock(g_lock);
				outfit = g_outfits[i];
				changes = g_outfit_changes[i];
			}

			model = ensure(slot, actor.handle, names, outfit, changes);
			status.made = model != nullptr;
			if (model)
			{
				shown = shown_at(actor, g_built[slot], now);
			}
		}

		Built& built = g_built[slot];
		if (model && shown.any)
		{
			if (!animate(built, model, shown.weights, shown.times) || !pose(actor.handle, shown.origin, shown.yaw) || !draw(model, actor.handle, shown.origin))
			{
				say("actors: a fault drawing %s; it is left out until it is changed", actor.name);
				built.broken = true;
			}

			status.shown = true;
			status.clip = shown.clip;
			memcpy(status.origin, shown.origin, sizeof(status.origin));
			status.yaw = shown.yaw;
		}
		else
		{
			memcpy(status.origin, actor.origin, sizeof(status.origin));
			status.yaw = actor.yaw;
		}

		status.broken = built.broken;
		strncpy_s(status.waiting, built.said, _TRUNCATE);
		std::lock_guard<std::mutex> lock(g_lock);
		if (i < g_count && g_actors[i].handle == actor.handle)
		{
			g_now[i] = status;
		}
	}

	// the preview
	Built& built = g_built[PREVIEW];
	if (!preview[0])
	{
		if (built.tree)
		{
			unbuild(PREVIEW, PREVIEW_HANDLE, true);
		}

		return;
	}

	ModelsOutfit outfit;
	unsigned changes = 0;
	{
		std::lock_guard<std::mutex> lock(g_lock);
		outfit = g_outfits[PREVIEW];
		changes = g_outfit_changes[PREVIEW];
	}

	void* model = ensure(PREVIEW, PREVIEW_HANDLE, {preview}, outfit, changes);
	if (!model)
	{
		return;
	}

	bool loops = false;
	const float length = length_of(built.parts[0], &loops);
	// (played round and round by the clock, with a pause at the end of one that does not go round)
	float fraction = 0.0f;
	if (length > 0.0f)
	{
		const double seconds = GetTickCount64() / 1000.0;
		const double span = loops ? length : length + 0.6;
		const double into = std::fmod(seconds, span);
		fraction = into >= length ? 1.0f : static_cast<float>(into / length);
	}

	const float weights[1] = {1.0f}, times[1] = {fraction};
	const float nowhere[3] = {0.0f, 0.0f, 0.0f};
	if (!animate(built, model, weights, times) || !pose(PREVIEW_HANDLE, nowhere, 0.0f))
	{
		built.broken = true;
		return;
	}

	float bones[ACTOR_BONES][3] = {};
	bool has[ACTOR_BONES] = {};
	float offset[3] = {}, turned = 0.0f;
	carried(built.anims, 1, fraction, offset, &turned);
	for (int b = 0; b < ACTOR_BONES; ++b)
	{
		has[b] = bone(PREVIEW_HANDLE, model, BONES[b], bones[b]);
		for (int k = 0; k < 3; ++k)
		{
			bones[b][k] += offset[k];
		}
	}

	std::lock_guard<std::mutex> lock(g_lock);
	memcpy(g_preview_bones, bones, sizeof(bones));
	memcpy(g_preview_has, has, sizeof(has));
	g_preview_fraction = fraction;
	g_preview_ready = true;
}
