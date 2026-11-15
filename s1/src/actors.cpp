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
//    +0x20, each of its three numbers kept XORed with the 32 bits at 0x150D160B9; angles +0x44; its number a
//    word +0x144; its kind a byte +0x150). Actors take the entities 1984 and up,
//    which no match uses, as their handles and poses.
//  - Going back in the recording starts the client game again, which lets go of every model (Com_InitDObj) but
//    not of the trees' states: the actors' are freed before (actors_release), XAnimFreeInfo 0x1404E5CC0.
#include "theater.hpp"

#include <algorithm>
#include <atomic>
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
	// The number a pose's origin is kept XORed with (each of its three numbers, as 32 bits): the game reads and
	// writes a pose's origin only through a routine that does this (0x150C1D673, as CG_General calls it with the
	// entity's +0x20 and 12 bytes: byte i of what it copies is XORed with byte (i & 3) of this).
	constexpr std::uintptr_t ORIGIN_KEY = 0x150D160B9;
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
		bool prop = false;               // one model, no animations
		bool broken = false;             // a fault with it: left alone until it is changed
		ULONGLONG tried = 0;
		char said[96] = "";
	};
	// (by handle, which an actor keeps for as long as it is there: the list's places move when one is removed)
	Built g_built[ACTORS_MAX + 1];
	unsigned g_change_counter = 100;
	double g_before[ACTORS_MAX] = {};          // (the game's thread) the recording's time at the frame before, for what happens at a time
	Actor g_scanned[ACTORS_MAX];               // (the game's thread) the actor its path's end was worked out for
	std::uint32_t g_path_end[ACTORS_MAX] = {};
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

	void free_model(const int handle)
	{
		__try
		{
			reinterpret_cast<void (*)(int)>(COM_SAFE_CLIENT_DOBJ_FREE)(handle);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
	}

	void unbuild(const int slot, const int handle, const bool game_holds)
	{
		Built& built = g_built[slot];
		if (!built.tree && built.prop && built.made && game_holds)
		{
			free_model(handle);
		}

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
	bool pose(const int handle, const float origin[3], const float yaw, const float pitch = 0.0f, const float roll = 0.0f)
	{
		__try
		{
			unsigned char* entity = entity_at(handle);
			auto* kept = reinterpret_cast<std::uint32_t*>(entity + CENTITY_ORIGIN);
			const std::uint32_t key = *reinterpret_cast<const std::uint32_t*>(ORIGIN_KEY);
			for (int i = 0; i < 3; ++i)
			{
				std::uint32_t bits;
				memcpy(&bits, &origin[i], sizeof(bits));
				kept[i] = bits ^ key;
			}

			auto* angles = reinterpret_cast<float*>(entity + CENTITY_ANGLES);
			angles[0] = pitch;
			angles[1] = yaw;
			angles[2] = roll;
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

	bool bone(const int handle, void* model, const char* name, float origin[3], float (*axis_out)[3] = nullptr)
	{
		float own[3][3];
		float (*axis)[3] = axis_out ? axis_out : own;
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
		float gone = 0.0f, path_length = 0.0f;  // how far along its path it is, and the path's length
	};

	float turn_to(const float from, const float to, const float t)
	{
		return from + std::remainder(to - from, 360.0f) * t;
	}

	// Along an actor's path (from where it stands through its points) by a distance: where that is, and which way
	// the path heads there (turning over a short way at each corner). flat: distances along the ground only (an
	// actor's steps); else through the air too (a prop). Returns the path's whole length.
	float along(const Actor& actor, const float distance, const bool flat, float out[3], float* heading)
	{
		const int count = actor.path_count < 0 ? 0 : actor.path_count > ACTOR_PATH_MAX ? ACTOR_PATH_MAX : actor.path_count;
		memcpy(out, actor.origin, 3 * sizeof(float));
		*heading = actor.yaw;
		if (count == 0)
		{
			return 0.0f;
		}

		float lengths[ACTOR_PATH_MAX], headings[ACTOR_PATH_MAX], total = 0.0f;
		const float* from = actor.origin;
		for (int i = 0; i < count; ++i)
		{
			const float* to = actor.path[i];
			const float dx = to[0] - from[0], dy = to[1] - from[1], dz = flat ? 0.0f : to[2] - from[2];
			lengths[i] = std::sqrt(dx * dx + dy * dy + dz * dz);
			headings[i] = dx * dx + dy * dy > 0.01f ? std::atan2(dy, dx) * TO_DEG : i ? headings[i - 1] : actor.yaw;
			total += lengths[i];
			from = to;
		}

		const float CORNER = 28.0f;
		float left = distance < 0.0f ? 0.0f : distance;
		from = actor.origin;
		for (int i = 0; i < count; ++i)
		{
			const float* to = actor.path[i];
			if (left <= lengths[i] || i == count - 1)
			{
				const float t = lengths[i] > 0.001f && left < lengths[i] ? left / lengths[i] : 1.0f;
				for (int k = 0; k < 3; ++k)
				{
					out[k] = from[k] + (to[k] - from[k]) * t;
				}

				float h = headings[i];
				const float to_end = lengths[i] - left;
				if (i + 1 < count && to_end < CORNER)
				{
					h = turn_to(h, headings[i + 1], 0.5f * (1.0f - (to_end < 0.0f ? 0.0f : to_end) / CORNER));
				}
				else if (i > 0 && left < CORNER)
				{
					h = turn_to(headings[i - 1], h, 0.5f + 0.5f * left / CORNER);
				}

				*heading = h;
				return total;
			}

			left -= lengths[i];
			from = to;
		}

		return total;
	}

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
		if (actor.prop)
		{
			// (a prop: there for its clip's time, going along its path at its speed)
			const ActorClip& span = actor.clips[0];
			if (actor.clip_count < 1 || now < span.at_ms || (!actor.stays && now >= static_cast<double>(span.at_ms) + span.length_ms))
			{
				return shown;
			}

			const double end = static_cast<double>(span.at_ms) + span.length_ms;
			shown.any = true;
			shown.clip = 0;
			shown.gone = static_cast<float>(((now < end ? now : end) - span.at_ms) / 1000.0) * actor.speed;
			float heading = actor.yaw;
			shown.path_length = along(actor, shown.gone, false, shown.origin, &heading);
			shown.yaw = actor.path_count > 0 ? heading + actor.yaw : actor.yaw;
			return shown;
		}

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
		// on a path: what its moving animations have carried it forward goes along the path; the rest (a death's
		// fall, a turn on the spot) is added to where the path has it
		const bool path = actor.path_count > 0 && actor.moves;
		float gone = 0.0f, beside[3] = {}, beside_yaw = 0.0f;
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

			if (path)
			{
				if (offset[0] > 1.0f && offset[0] > std::fabs(offset[1]))
				{
					gone += std::sqrt(offset[0] * offset[0] + offset[1] * offset[1]);
					beside[2] += offset[2];
				}
				else
				{
					float at[3], heading = 0.0f;
					along(actor, gone, true, at, &heading);
					const float facing = heading + beside_yaw;
					const float c = std::cos(facing * TO_RAD), s = std::sin(facing * TO_RAD);
					beside[0] += offset[0] * c - offset[1] * s;
					beside[1] += offset[0] * s + offset[1] * c;
					beside[2] += offset[2];
					beside_yaw += turned;
				}

				continue;
			}

			const float c = std::cos(shown.yaw * TO_RAD), s = std::sin(shown.yaw * TO_RAD);
			shown.origin[0] += offset[0] * c - offset[1] * s;
			shown.origin[1] += offset[0] * s + offset[1] * c;
			shown.origin[2] += offset[2];
			shown.yaw += turned;
		}

		if (path)
		{
			float heading = 0.0f;
			shown.path_length = along(actor, gone, true, shown.origin, &heading);
			shown.gone = gone < shown.path_length ? gone : shown.path_length;
			for (int k = 0; k < 3; ++k)
			{
				shown.origin[k] += beside[k];
			}

			shown.yaw = heading + beside_yaw;
		}

		return shown;
	}

	// (the game's thread) The model and tree for a figure, made if they are not there or no longer fit.
	void* ensure(const int slot, const int handle, const std::vector<std::string>& names, const ModelsOutfit& outfit, const unsigned outfit_changes,
	             const bool prop = false)
	{
		Built& built = g_built[slot];
		if (built.names != names || built.outfit != outfit_changes || built.prop != prop)
		{
			// (changed: made again, and tried again after a fault)
			if (built.tree || built.prop)
			{
				unbuild(slot, handle, !built.broken);
			}

			built.broken = false;
			built.names = names;
			built.outfit = outfit_changes;
			built.prop = prop;
		}

		if (built.broken)
		{
			return nullptr;
		}

		if (prop)
		{
			if (built.made)
			{
				if (void* model = model_of(handle))
				{
					return model;
				}

				built.made = false;
			}

			const ULONGLONG tick = GetTickCount64();
			if (tick - built.tried < 500 || outfit.count == 0)
			{
				return nullptr;
			}

			built.tried = tick;
			if (!entity_free(handle, nullptr))
			{
				say("actors: entity %d is in use by the match; the prop is not made", handle);
				built.broken = true;
				return nullptr;
			}

			char missing[64] = "";
			void* model = models_make_actor(handle, outfit, nullptr, missing);
			if (!model)
			{
				if (strcmp(built.said, missing) != 0)
				{
					strncpy_s(built.said, missing, _TRUNCATE);
					say("actors: the prop for entity %d could not be made: %s is not in memory", handle, missing[0] ? missing : "its model");
				}

				return nullptr;
			}

			built.made = true;
			built.said[0] = 0;
			say("actors: entity %d made as a prop: %s", handle, outfit.names[0]);
			return model;
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

namespace
{
	void facing_axis(const float yaw, float axis[3][3])
	{
		const float c = std::cos(yaw * TO_RAD), s = std::sin(yaw * TO_RAD);
		const float made[3][3] = {{c, s, 0.0f}, {-s, c, 0.0f}, {0.0f, 0.0f, 1.0f}};
		memcpy(axis, made, sizeof(made));
	}

	// (the game's thread) What an actor does at a time, as the recording's time goes past it.
	void happen(const Actor& actor, const ActorEvent& event, void* model, const Shown& shown, const double before, const double now)
	{
		if (!event.effect[0])
		{
			return;
		}

		float origin[3] = {}, axis[3][3] = {};
		if (event.kind == 0)
		{
			const double every = event.every_ms < 20.0f ? 20.0 : event.every_ms;
			const double end = static_cast<double>(event.at_ms) + event.length_ms;
			const double shots_now = now < event.at_ms ? -1.0 : std::floor(((now < end ? now : end) - event.at_ms) / every);
			const double shots_before = before < event.at_ms ? -1.0 : std::floor(((before < end ? before : end) - event.at_ms) / every);
			if (shots_now <= shots_before)
			{
				return;
			}

			if (!bone(actor.handle, model, "tag_flash", origin, axis))
			{
				// (no gun, or one with no such place: from the right hand, the way it faces)
				if (!bone(actor.handle, model, "j_wrist_ri", origin))
				{
					return;
				}

				facing_axis(shown.yaw, axis);
			}
		}
		else
		{
			if (!(before < event.at_ms && now >= event.at_ms))
			{
				return;
			}

			if (!bone(actor.handle, model, event.bone[0] ? event.bone : "j_spineupper", origin))
			{
				memcpy(origin, shown.origin, sizeof(origin));
				origin[2] += 44.0f;
			}

			// (out of its back)
			facing_axis(shown.yaw + 180.0f, axis);
		}

		if (!effects_play_once(event.effect, origin, axis))
		{
			say("actors: %s's effect %s is not loaded in this match", actor.name, event.effect);
		}
	}
}

// ---- death animations for the match's own players.
// A body plays its death by the number the server gave it, which is an entry of the players' animations (one set,
// shared by every player's and body's tree). The body's tree is given a copy of that set in which every death is
// the animation chosen for that player, at the moment the game makes the body's model, before the death is begun:
// whichever death the server picked, the chosen one plays. (IrisStudio's Black Ops version does the same.)
namespace
{
	std::string g_death_all;               // for everybody without a choice of their own
	std::map<int, std::string> g_death_of; // by player
	std::atomic<bool> g_death_any{false};

	struct DeathCopy
	{
		void* original;   // the players' animations
		std::string name; // the death every death is in the copy
		unsigned char* copy;
	};
	std::vector<DeathCopy> g_death_copies;  // (the game's thread; kept for good: trees point at them)

	// The players' animations? (many entries, some of them a player's body's deaths)
	bool players_set(const unsigned char* anims, unsigned* size)
	{
		__try
		{
			*size = *reinterpret_cast<const std::uint32_t*>(anims);
			if (*size < 64 || *size > 20000)
			{
				return false;
			}

			int deaths = 0;
			for (unsigned i = 1; i < *size && deaths < 3; ++i)
			{
				const unsigned char* entry = anims + 8 + 16 * static_cast<size_t>(i);
				if (*reinterpret_cast<const std::uint16_t*>(entry) != 0)
				{
					continue;
				}

				const char* name = asset_name(*reinterpret_cast<void* const*>(entry + 8));
				deaths += name && strncmp(name, "pb_", 3) == 0 && strstr(name, "death") ? 1 : 0;
			}

			return deaths >= 3;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	int fill_copy(unsigned char* copy, const unsigned char* anims, const unsigned size, void* parts)
	{
		__try
		{
			memcpy(copy, anims, 8 + 16 * static_cast<size_t>(size));
			int changed = 0;
			for (unsigned i = 1; i < size; ++i)
			{
				unsigned char* entry = copy + 8 + 16 * static_cast<size_t>(i);
				if (*reinterpret_cast<const std::uint16_t*>(entry) != 0)
				{
					continue;
				}

				const char* name = asset_name(*reinterpret_cast<void**>(entry + 8));
				if (name && strncmp(name, "pb_", 3) == 0 && strstr(name, "death"))
				{
					*reinterpret_cast<void**>(entry + 8) = parts;
					++changed;
				}
			}

			return changed;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return -1;
		}
	}

	bool tree_anims(void* tree, void** anims)
	{
		__try
		{
			*anims = *static_cast<void**>(tree);
			return *anims != nullptr;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	bool set_tree_anims(void* tree, void* anims)
	{
		__try
		{
			*static_cast<void**>(tree) = anims;
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}
}

// Is any death animation chosen?
bool deaths_any()
{
	return g_death_any;
}

// The death animation for a player (-1: for everybody who has none of their own); "" for the one recorded.
void deaths_set(const int entity, const char* animation)
{
	std::lock_guard<std::mutex> lock(g_lock);
	const std::string name = animation ? animation : "";
	if (entity < 0)
	{
		g_death_all = name;
	}
	else if (name.empty())
	{
		g_death_of.erase(entity);
	}
	else
	{
		g_death_of[entity] = name;
	}

	g_death_any = !g_death_all.empty() || !g_death_of.empty();
}

bool deaths_get(const int entity, char out[64])
{
	std::lock_guard<std::mutex> lock(g_lock);
	const std::string* name = &g_death_all;
	if (entity >= 0)
	{
		const auto own = g_death_of.find(entity);
		if (own == g_death_of.end())
		{
			out[0] = 0;
			return false;
		}

		name = &own->second;
	}

	strncpy_s(out, 64, name->c_str(), _TRUNCATE);
	return !name->empty();
}

// Where an entity of the client game's is (its pose's origin, kept XORed: see ORIGIN_KEY).
bool actors_entity_origin(const int handle, float out[3])
{
	if (handle < 0 || handle >= 2048)
	{
		return false;
	}

	__try
	{
		const auto* kept = reinterpret_cast<const std::uint32_t*>(entity_at(handle) + CENTITY_ORIGIN);
		const std::uint32_t key = *reinterpret_cast<const std::uint32_t*>(ORIGIN_KEY);
		bool any = false;
		for (int i = 0; i < 3; ++i)
		{
			const std::uint32_t bits = kept[i] ^ key;
			memcpy(&out[i], &bits, sizeof(bits));
			any = any || kept[i] != 0;
		}

		return any && std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}

// (the game's thread, as the game is about to make a model with an animation tree) whose: the player the model
// is, or is the body of (-1: not known). The tree is given the players' animations with that player's chosen
// death in place of every death, or the players' own again if none is chosen.
void deaths_on_create(void* tree, const int whose, const int handle)
{
	void* anims = nullptr;
	if (!tree || !tree_anims(tree, &anims))
	{
		return;
	}

	// (a tree the editor changed before, used again by the game for another body)
	void* original = anims;
	for (const DeathCopy& copy : g_death_copies)
	{
		if (copy.copy == anims)
		{
			original = copy.original;
		}
	}

	if (!g_death_any && original == anims)
	{
		return;
	}

	std::string wanted;
	void* parts = nullptr;
	{
		std::lock_guard<std::mutex> lock(g_lock);
		const auto own = g_death_of.find(whose);
		wanted = whose >= 0 && own != g_death_of.end() ? own->second : g_death_all;
		if (!wanted.empty())
		{
			if (!g_listed)
			{
				list_animations();
			}

			const auto found = g_animations.find(wanted);
			parts = found == g_animations.end() ? nullptr : found->second;
		}
	}

	if (wanted.empty() || !parts)
	{
		if (original != anims)
		{
			set_tree_anims(tree, original);
		}

		return;
	}

	unsigned size = 0;
	if (!players_set(static_cast<const unsigned char*>(original), &size))
	{
		return;
	}

	DeathCopy* mine = nullptr;
	for (DeathCopy& copy : g_death_copies)
	{
		if (copy.original == original && copy.name == wanted)
		{
			mine = &copy;
		}
	}

	const bool fresh = !mine;
	if (!mine)
	{
		auto* memory = static_cast<unsigned char*>(calloc(1, 8 + 16 * static_cast<size_t>(size) + 64));
		if (!memory)
		{
			return;
		}

		g_death_copies.push_back({original, wanted, memory});
		mine = &g_death_copies.back();
	}

	if (fresh)
	{
		const int changed = fill_copy(mine->copy, static_cast<const unsigned char*>(original), size, parts);
		say("deaths: %s in place of %d deaths among the players' %u animations", wanted.c_str(), changed, size);
		if (changed <= 0)
		{
			// (nothing to play it in place of: the copy is the players' own set, harmless)
			return;
		}
	}

	if (set_tree_anims(tree, mine->copy) && handle >= 42)
	{
		say("deaths: entity %d, the body of player %d, is given %s", handle, whose, wanted.c_str());
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
	g_outfit_changes[g_count] = ++g_change_counter;
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
	g_outfit_changes[index] = ++g_change_counter;
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
void actors_preview(const char* animation, const int dressed_as_actor, const int dressed_as_entity)
{
	// (a player's outfit is asked for before the lock is taken: the Models list has a lock of its own)
	ModelsOutfit players;
	bool have_players = false;
	{
		std::lock_guard<std::mutex> lock(g_lock);
		have_players = g_outfits[PREVIEW].count == 0 && !(dressed_as_actor >= 0 && dressed_as_actor < g_count);
	}

	if (have_players && animation && animation[0])
	{
		have_players = false;
		if (dressed_as_entity >= 0)
		{
			have_players = models_outfit_of(dressed_as_entity, &players);
		}

		for (int entity = 0; !have_players && entity < 42; ++entity)
		{
			have_players = models_outfit_of(entity, &players);
		}
	}
	else
	{
		have_players = false;
	}

	std::lock_guard<std::mutex> lock(g_lock);
	strncpy_s(g_preview_wanted, animation ? animation : "", _TRUNCATE);
	if (g_outfits[PREVIEW].count == 0)
	{
		if (dressed_as_actor >= 0 && dressed_as_actor < g_count)
		{
			g_outfits[PREVIEW] = g_outfits[dressed_as_actor];
			++g_outfit_changes[PREVIEW];
		}
		else if (have_players)
		{
			g_outfits[PREVIEW] = players;
			++g_outfit_changes[PREVIEW];
		}
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
		if (g_built[slot].tree || g_built[slot].prop)
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
			if (g_built[slot].tree || g_built[slot].prop)
			{
				unbuild(slot, handle, state.playing);
			}

			g_built[slot] = Built();
			g_scanned[slot] = Actor();
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
		for (int c = 0; c < actor.clip_count && !actor.prop; ++c)
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

			model = ensure(slot, actor.handle, names, outfit, changes, actor.prop);
			status.made = model != nullptr;
			if (model)
			{
				shown = shown_at(actor, g_built[slot], now);
			}
		}

		Built& built = g_built[slot];
		if (model && shown.any)
		{
			if ((!actor.prop && !animate(built, model, shown.weights, shown.times)) ||
			    !pose(actor.handle, shown.origin, shown.yaw, actor.prop ? actor.pitch : 0.0f, actor.prop ? actor.roll : 0.0f) ||
			    !draw(model, actor.handle, shown.origin))
			{
				say("actors: a fault drawing %s; it is left out until it is changed", actor.name);
				built.broken = true;
			}

			status.shown = true;
			status.clip = shown.clip;
			memcpy(status.origin, shown.origin, sizeof(status.origin));
			status.yaw = shown.yaw;
			status.path_gone = shown.gone;
			status.path_length = shown.path_length;
			// what it does at a time (its shots, its blood), as the time goes past it
			const double before = g_before[slot];
			if (!built.broken && now > before && now - before < 400.0)
			{
				for (int e = 0; e < actor.event_count && e < ACTOR_EVENTS_MAX; ++e)
				{
					happen(actor, actor.events[e], model, shown, before, now);
				}
			}
		}
		else
		{
			memcpy(status.origin, actor.origin, sizeof(status.origin));
			status.yaw = actor.yaw;
		}

		g_before[slot] = now;
		// when it gets to its path's end (worked out anew when it has been changed)
		if (actor.path_count > 0 && model && !built.broken)
		{
			if (memcmp(&g_scanned[slot], &actor, sizeof(Actor)) != 0)
			{
				g_scanned[slot] = actor;
				g_path_end[slot] = 0;
				std::uint32_t first = UINT32_MAX, last = 0;
				for (int c = 0; c < actor.clip_count; ++c)
				{
					first = (std::min)(first, actor.clips[c].at_ms);
					last = (std::max)(last, actor.clips[c].at_ms + actor.clips[c].length_ms);
				}

				for (std::uint32_t t = first, steps = 0; t <= last && steps < 6000 && first != UINT32_MAX; t += 50, ++steps)
				{
					const Shown then = shown_at(actor, built, t);
					if (then.path_length > 0.0f && then.gone >= then.path_length - 0.5f)
					{
						g_path_end[slot] = t;
						break;
					}
				}
			}

			status.path_end_ms = g_path_end[slot];
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

	// (kept in the middle of the view however far the animation carries it: its hips over the middle, and brought
	// back up or down when it climbs or drops out of sight)
	if (has[0])
	{
		const float x = bones[0][0], y = bones[0][1];
		const float z = bones[0][2] > 62.0f ? bones[0][2] - 62.0f : bones[0][2] < -4.0f ? bones[0][2] + 4.0f : 0.0f;
		for (int b = 0; b < ACTOR_BONES; ++b)
		{
			bones[b][0] -= x;
			bones[b][1] -= y;
			bones[b][2] -= z;
		}
	}

	std::lock_guard<std::mutex> lock(g_lock);
	memcpy(g_preview_bones, bones, sizeof(bones));
	memcpy(g_preview_has, has, sizeof(has));
	g_preview_fraction = fraction;
	g_preview_ready = true;
}

// ---- props, guns, and the whole of it taken and put back (the saved scene, undo)
namespace
{
	// (with the lock held) A handle none of the actors has.
	int free_handle()
	{
		int handle = FIRST_HANDLE;
		for (bool taken = true; taken;)
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

		return handle;
	}

	// (a gun or a part of one, as others see it; the launcher on an exo's arm is "npc_" too and is not)
	bool is_gun(const char* name)
	{
		return strncmp(name, "npc_", 4) == 0 && !strstr(name, "exo_arm");
	}
}

int actors_add_prop(const char* model, const float origin[3], const float yaw, const std::uint32_t ms)
{
	ModelsOutfit outfit;
	models_outfit_named(model, &outfit);
	std::lock_guard<std::mutex> lock(g_lock);
	if (g_count >= ACTORS_MAX || !model || !model[0])
	{
		return -1;
	}

	Actor actor;
	actor.handle = free_handle();
	snprintf(actor.name, sizeof(actor.name), "%.31s", model);
	memcpy(actor.origin, origin, sizeof(actor.origin));
	actor.yaw = yaw;
	actor.prop = true;
	actor.moves = false;
	strncpy_s(actor.model, model, _TRUNCATE);
	actor.clips[0].at_ms = ms;
	actor.clips[0].length_ms = 10000;
	actor.clips[0].anim[0] = 0;
	actor.clip_count = 1;
	g_actors[g_count] = actor;
	g_outfits[g_count] = outfit;
	g_outfit_changes[g_count] = ++g_change_counter;
	g_now[g_count] = ActorNow();
	return g_count++;
}

bool actors_gun(const int index, char out[64])
{
	std::lock_guard<std::mutex> lock(g_lock);
	out[0] = 0;
	if (index < 0 || index >= g_count)
	{
		return false;
	}

	const ModelsOutfit& outfit = g_outfits[index];
	for (int i = 0; i < outfit.count; ++i)
	{
		if (is_gun(outfit.names[i]))
		{
			strncpy_s(out, 64, outfit.names[i], _TRUNCATE);
			return true;
		}
	}

	return false;
}

// The gun in its hands is another model ("": none). The place in the hands is the one a player's gun has: if the
// actor's outfit came without a gun, a player's of the match is gone by.
bool actors_give_gun(const int index, const char* model)
{
	// (a player's gun's entry, asked for before the lock is taken)
	ModelsOutfit donor;
	int donor_entry = -1;
	for (int entity = 0; entity < 42 && donor_entry < 0; ++entity)
	{
		if (models_outfit_of(entity, &donor))
		{
			for (int i = 0; i < donor.count && donor_entry < 0; ++i)
			{
				donor_entry = is_gun(donor.names[i]) ? i : -1;
			}
		}
	}

	std::lock_guard<std::mutex> lock(g_lock);
	if (index < 0 || index >= g_count || g_actors[index].prop)
	{
		return false;
	}

	ModelsOutfit& outfit = g_outfits[index];
	// the first of its gun's parts is kept as the place; the others (sights, grips) go
	int first = -1, kept = 0;
	for (int i = 0; i < outfit.count; ++i)
	{
		const bool gun = is_gun(outfit.names[i]);
		if (gun && first >= 0)
		{
			continue;
		}

		if (gun && (!model || !model[0]))
		{
			continue;
		}

		if (kept != i)
		{
			memcpy(outfit.names[kept], outfit.names[i], sizeof(outfit.names[0]));
			memcpy(outfit.over[kept], outfit.over[i], sizeof(outfit.over[0]));
			memcpy(outfit.base[kept], outfit.base[i], sizeof(outfit.base[0]));
			memcpy(outfit.entries[kept], outfit.entries[i], sizeof(outfit.entries[0]));
		}

		first = gun ? kept : first;
		++kept;
	}

	outfit.count = kept;
	if (model && model[0])
	{
		if (first < 0)
		{
			if (donor_entry < 0 || outfit.count >= 32)
			{
				return false;
			}

			first = outfit.count++;
			memcpy(outfit.entries[first], donor.entries[donor_entry], sizeof(outfit.entries[0]));
		}

		strncpy_s(outfit.names[first], model, _TRUNCATE);
		outfit.over[first][0] = 0;
		outfit.base[first][0] = 0;
		outfit.entries[first][0xD] = 0;
	}

	g_outfit_changes[index] = ++g_change_counter;
	return true;
}

int actors_export(Actor* actors, ModelsOutfit* outfits, const int max)
{
	std::lock_guard<std::mutex> lock(g_lock);
	const int count = g_count < max ? g_count : max;
	for (int i = 0; i < count; ++i)
	{
		actors[i] = g_actors[i];
		outfits[i] = g_outfits[i];
	}

	return count;
}

void actors_import(const Actor* actors, const ModelsOutfit* outfits, const int count)
{
	std::vector<int> gone;
	{
		std::lock_guard<std::mutex> lock(g_lock);
		// what each handle had, to tell what has changed
		std::map<int, int> had;
		for (int i = 0; i < g_count; ++i)
		{
			had[g_actors[i].handle] = i;
		}

		static ModelsOutfit old_outfits[ACTORS_MAX];
		unsigned old_changes[ACTORS_MAX];
		ActorNow old_now[ACTORS_MAX];
		for (int i = 0; i < g_count; ++i)
		{
			old_outfits[i] = g_outfits[i];
			old_changes[i] = g_outfit_changes[i];
			old_now[i] = g_now[i];
		}

		const int taken = count < ACTORS_MAX ? count : ACTORS_MAX;
		int highest = 0;
		for (int i = 0; i < taken; ++i)
		{
			g_actors[i] = actors[i];
			g_outfits[i] = outfits[i];
			const auto was = had.find(actors[i].handle);
			if (was != had.end() && memcmp(&old_outfits[was->second], &outfits[i], sizeof(ModelsOutfit)) == 0)
			{
				g_outfit_changes[i] = old_changes[was->second];
				g_now[i] = old_now[was->second];
			}
			else
			{
				g_outfit_changes[i] = ++g_change_counter;
				g_now[i] = ActorNow();
			}

			if (was != had.end())
			{
				had.erase(was);
			}

			int number = 0;
			if (sscanf_s(actors[i].name, "actor%d", &number) == 1 && number > highest)
			{
				highest = number;
			}
		}

		for (const auto& left : had)
		{
			gone.push_back(left.first);
			g_removed.push_back(left.first);
		}

		g_count = taken;
		g_next_number = (std::max)(g_next_number, highest + 1);
	}

	for (const int handle : gone)
	{
		models_drop(handle);
	}
}

void actors_touch(const int handle)
{
	std::lock_guard<std::mutex> lock(g_lock);
	for (int i = 0; i < g_count; ++i)
	{
		if (g_actors[i].handle == handle)
		{
			g_outfit_changes[i] = ++g_change_counter;
		}
	}
}

// Where an actor or prop is now (by its handle); false: there is none such, or it is not shown.
bool actors_origin_of(const int handle, float out[3])
{
	std::lock_guard<std::mutex> lock(g_lock);
	for (int i = 0; i < g_count; ++i)
	{
		if (g_actors[i].handle == handle && g_now[i].shown)
		{
			memcpy(out, g_now[i].origin, 3 * sizeof(float));
			return true;
		}
	}

	return false;
}

// The death animations chosen, as lines of "player<tab>animation" (-1: everybody).
int deaths_export(char* out, const int size)
{
	std::lock_guard<std::mutex> lock(g_lock);
	std::string text;
	if (!g_death_all.empty())
	{
		text += "-1\t" + g_death_all + "\n";
	}

	for (const auto& one : g_death_of)
	{
		text += std::to_string(one.first) + "\t" + one.second + "\n";
	}

	strncpy_s(out, static_cast<size_t>(size), text.c_str(), _TRUNCATE);
	return static_cast<int>(strlen(out));
}

void deaths_import(const char* text)
{
	std::lock_guard<std::mutex> lock(g_lock);
	g_death_all.clear();
	g_death_of.clear();
	for (const char* at = text; at && *at;)
	{
		const char* end = strchr(at, '\n');
		const std::string line(at, end ? static_cast<size_t>(end - at) : strlen(at));
		const size_t tab = line.find('\t');
		if (tab != std::string::npos && tab + 1 < line.size())
		{
			const int entity = atoi(line.c_str());
			if (entity < 0)
			{
				g_death_all = line.substr(tab + 1);
			}
			else
			{
				g_death_of[entity] = line.substr(tab + 1);
			}
		}

		at = end ? end + 1 : nullptr;
	}

	g_death_any = !g_death_all.empty() || !g_death_of.empty();
}
