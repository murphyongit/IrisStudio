// S1iris: passes (what of the picture is drawn: everything, the world, the players, the gun; and the
// picture's depth), the gun's depth, and the fog's colour and thickness.
//
// What is drawn. The game's scene is put together from separate lists, and three of the steps that fill
// them are held back as a pass asks:
//   0x1405CB8D0  queues the map's own surfaces (five ranges of them, from the map at [0x14D85F640]:
//                +0x71C..+0x748) for the camera: held back, the map and its sky are not drawn.
//   0x1405C9AE0  queues the map's static models (the command 0x1B, with the models' count +0x710 and
//                their visibility +0x758): held back, the map's props are not drawn.
//   0x1405C8BB0  puts one animated model's surfaces in the camera's lists (its third argument is the
//                scene's entry: the model at +0x68, flags at +0x94; it returns at once for an entry with
//                no model): held back for the players' models (the 42 characters at 0x141620568, each
//                0xCC0 bytes with its handle first and whether it is in use next), or for all but them,
//                or for all but the first-person ones (handles from 2048).
// Where the map is not drawn the picture is cleared to a colour (r_clear 3, r_clearColor).
//
// The depth. The game's depth buffer is found by its being cleared (ID3D11DeviceContext's
// ClearDepthStencilView, on the immediate and on the deferred contexts) with the picture's own size; it
// is copied when the frame is done (or before one of its clears) and drawn over the picture as greys,
// before the editor takes its copy of the picture: so the editor and a video both get it.
//
// The fog. The game keeps seven sets of fog values a player (0x9C bytes each, from 0x14D858C34; the set
// the server last gave is 1, the one drawn from is 2, the one being gone to is the number at
// 0x14D8594BC): the "atmosfog"/fog command's handler (0x1401BD580) fills one and has it gone to. In a
// set: +0x3C whether it is the atmosphere kind; colours are 4 bytes, blue first (+0x7C and +0x84 for the
// plain kind, +0x94 and +0x98 for the atmosphere kind); the plain kind's start is +0x00 and its
// half-way distance +0x80. The atmosphere kind's numbers are +0x40..+0x60, +0x70, +0x8C, +0x90: which
// is which is not known yet, so they are offered by number.
#include "passes.hpp"

#include "theater.hpp"
#include "ui_parts.hpp"

#include <MinHook.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <d3dcompiler.h>
#include <mutex>
#include <string>

namespace
{
	// (0.63) In the game's usual way of drawing, the map's surfaces and static models are put in the camera's
	// lists by worker commands of their own (the dispatcher 0x1405E5B80 has the table): 0x10 -> 0x140569EB0
	// and 0x12 -> 0x14056A2F0 the map's surfaces, 0x1D -> 0x14056FFD0 and 0x13 -> 0x14056D940 its static
	// models. 0x1405CB8D0 queues only the glowing surfaces then, and 0x1405C9AE0 (command 0x1B) is the
	// effects', not the models': 0.62 held those two back and the map was drawn all the same.
	constexpr std::uintptr_t BSP_CAMERA = 0x1405CB8D0, DOBJ_CAMERA = 0x1405C8BB0;
	// (0.64) With those four held back the map's walls and sky were gone but many of its props were not: the
	// command 0x11 -> 0x14056B110 (it too works from the map at [0x14D8F2640]) is held back as well.
	constexpr std::uintptr_t WORLD_COMMANDS[5] = {0x140569EB0, 0x14056A2F0, 0x14056FFD0, 0x14056D940, 0x14056B110};
	// The game's playing of an effect fixed to a model's bone: 0x1402AC420(client, effect, time, the model's
	// handle, bone, ...), which gives nothing back when the bone is not found. Held back for the players'
	// models while they are not drawn (their jets and muzzle flashes would hang in the air).
	constexpr std::uintptr_t FX_PLAY_BOLTED = 0x1402AC420;
	// (0.65) What was still drawn on the colour: the loose models (crates, turrets, panels: 0x1405CA7A0, called
	// for each in the same command as the animated models, 0x140579B90) and the moving pieces of the map
	// (0x1405C7FD0), which are held back with the map; and effects, which all begin in 0x1402AC050 (its
	// thirteenth argument the handle of the model the effect is fixed to, 0xFFF for none): held back unless
	// they are on what the pass shows.
	constexpr std::uintptr_t LOOSE_MODEL_CAMERA = 0x1405CA7A0, BRUSH_CAMERA = 0x1405C7FD0, FX_SPAWN = 0x1402AC050;
	// The game's own clearing of what it draws to: 0x1405D9510(holder of the drawing context, which (4 the
	// picture, 1 and 2 the depth and stencil), colour, depth, stencil): it asks the context what is set and
	// clears that. The depth buffer is found here: the contexts the game draws with did not go through the
	// methods hooked in 0.62 to 0.64 (nothing was ever seen there).
	constexpr std::uintptr_t GAME_CLEAR = 0x1405D9510;
	constexpr std::uintptr_t COM_GET_CLIENT_DOBJ = 0x1403D2BA0, CHARACTERS = 0x141620568;
	// (0.66: the records go on in two more runs, 8 from 0x141649350 and the bodies' from 0x14164F950: bodies on
	// the floor were still drawn in "only the world" because only the first 42 were looked at)
	constexpr std::uintptr_t CHARACTERS_MORE = 0x141649350, CHARACTERS_BODIES = 0x14164F950;
	constexpr int PLAYER_RECORDS = 42, MORE_RECORDS = 8, BODY_RECORDS = 24;
	constexpr int CHARACTER_COUNT = PLAYER_RECORDS + MORE_RECORDS + BODY_RECORDS, CHARACTER_SIZE = 0xCC0, FIRST_PERSON_HANDLE = 2048, VIEW_HANDLES = 4;

	enum Elements
	{
		Everything,
		NoGun,
		OnlyWorld,
		OnlyPlayers,
		OnlyGun,
		ELEMENT_COUNT
	};
	const char* const ELEMENT_NAMES[ELEMENT_COUNT] = {"Everything", "World and players, no gun", "Only the world", "Only the players, on a colour",
	                                                  "Only the gun, on a colour"};

	struct Pass
	{
		const char* label;
		int elements;
		bool depth;
	};
	// the passes a recording can be made of, one after another
	const Pass PASSES[] = {
		{"The picture", Everything, false},
		{"Depth", Everything, true},
		{"World and players, no gun", NoGun, false},
		{"Only the world", OnlyWorld, false},
		{"Only the players, on a colour", OnlyPlayers, false},
		{"Only the gun, on a colour", OnlyGun, false},
		{"Depth without the gun", NoGun, true},
	};
	constexpr int PASS_COUNT = static_cast<int>(sizeof(PASSES) / sizeof(PASSES[0]));

	std::atomic<int> g_elements{Everything};
	std::atomic<bool> g_depth{false}, g_gun_true_depth{false};
	std::atomic<bool> g_hide_world{false}, g_hide_characters{false}, g_only_characters{false}, g_only_view{false};
	std::atomic<bool> g_playing{false};
	bool g_ticked[PASS_COUNT] = {};
	float g_colour[3] = {0.0f, 1.0f, 0.0f};
	// (on a colour: effects are not drawn at all unless this is on, when those on what is shown are kept)
	std::atomic<bool> g_keep_effects{false};
	int g_applied_effects = -1;
	char g_tag[32] = "";

	// the models of the players (and their bodies) and of the first person, as of this frame
	std::atomic<void*> g_characters[CHARACTER_COUNT];
	std::atomic<void*> g_views[VIEW_HANDLES];
	std::atomic<int> g_held_back{0};
	std::atomic<unsigned> g_view_flags{0};

	using bsp_t = void (*)(void*, void*, void*);
	using command_t = void (*)(void*);
	using dobj_t = void (*)(void*, void*, unsigned char*, int, int, void*, void*, int);
	bsp_t g_original_bsp = nullptr;
	command_t g_original_world[5] = {};
	using bolted_t = void* (*)(int, void*, int, int, int, int, int, int);
	bolted_t g_original_bolted = nullptr;
	using word_t = std::uintptr_t;
	using loose_t = void (*)(word_t, word_t, word_t, word_t, word_t, word_t, word_t, word_t, word_t, word_t);
	using brush_t = void (*)(word_t, word_t, word_t, word_t, word_t, word_t, word_t, word_t);
	using spawn_t = void* (*)(word_t, word_t, word_t, word_t, word_t, word_t, word_t, word_t, word_t, word_t, word_t, word_t, word_t, word_t, word_t, word_t, word_t,
	                          word_t, word_t, word_t, word_t, word_t, word_t, word_t, word_t, word_t);
	using game_clear_t = void (*)(void**, unsigned char, const float*, float, unsigned char);
	loose_t g_original_loose = nullptr;
	brush_t g_original_brush = nullptr;
	spawn_t g_original_spawn = nullptr;
	game_clear_t g_original_game_clear = nullptr;
	thread_local int t_bolted_to = -1;  // (the handle an effect being played is fixed to, while the game plays it)
	std::atomic<int> g_effects_held{0};
	dobj_t g_original_dobj = nullptr;
	std::atomic<int> g_world_held{0};

	void hooked_bsp(void* a, void* b, void* c)
	{
		if (!g_hide_world)
		{
			g_original_bsp(a, b, c);
		}
	}

	template <int N>
	void hooked_world(void* a)
	{
		if (g_hide_world)
		{
			g_world_held.fetch_add(1, std::memory_order_relaxed);
			return;
		}

		g_original_world[N](a);
	}

	// Whether a scene entry's model is not to be drawn in the pass that is on.
	bool model_held(const unsigned char* entry)
	{
		void* model = entry ? *reinterpret_cast<void* const*>(entry + 0x98) : nullptr;
		if (!model || !g_playing || !(g_only_view || g_only_characters || g_hide_characters))
		{
			return false;
		}

		bool view = false, character = false;
		for (const auto& one : g_views)
		{
			view = view || one.load(std::memory_order_relaxed) == model;
		}

		if (!view)
		{
			for (const auto& one : g_characters)
			{
				character = character || one.load(std::memory_order_relaxed) == model;
			}
		}

		return (g_only_view && !view) || (g_only_characters && !character) || (g_hide_characters && character);
	}

	// (0.66) The other way a model's surfaces are listed: 0x1405C8840(entry, n, where to write, the end, ...),
	// which gives back where it stopped writing (the start, for an entry with no model). A player shot in the
	// gun-only pass was drawn for a moment by it, and the shadows of players not drawn come from it.
	using dobj_other_t = void* (*)(unsigned char*, int, void*, void*, void*);
	dobj_other_t g_original_dobj_other = nullptr;

	void* hooked_dobj_other(unsigned char* entry, const int n, void* out, void* end, void* e)
	{
		if (model_held(entry))
		{
			g_held_back.fetch_add(1, std::memory_order_relaxed);
			return out;
		}

		return g_original_dobj_other(entry, n, out, end, e);
	}

	void hooked_dobj(void* a, void* b, unsigned char* entry, const int d, const int e, void* f, void* g, const int h)
	{
		// (0.62 took the pointer at +0x68 for the model: it is something else that has to be there; the model
		// is at +0x98, which is what the game asks for the model's parts)
		void* model = entry ? *reinterpret_cast<void**>(entry + 0x98) : nullptr;
		if (!model || !g_playing)
		{
			g_original_dobj(a, b, entry, d, e, f, g, h);
			return;
		}

		bool view = false, character = false;
		for (const auto& one : g_views)
		{
			view = view || one.load(std::memory_order_relaxed) == model;
		}

		if (!view)
		{
			for (const auto& one : g_characters)
			{
				character = character || one.load(std::memory_order_relaxed) == model;
			}
		}

		if ((g_only_view && !view) || (g_only_characters && !character) || (g_hide_characters && character))
		{
			g_held_back.fetch_add(1, std::memory_order_relaxed);
			return;
		}

		if (view)
		{
			// (noted for the log: what marks the gun's models)
			g_view_flags = *reinterpret_cast<unsigned*>(entry + 0x94);
		}

		g_original_dobj(a, b, entry, d, e, f, g, h);
	}

	void* client_dobj(const int handle);

	void* hooked_bolted(const int client, void* effect, const int time, const int handle, const int bone, const int a6, const int a7, const int a8)
	{
		const int before = t_bolted_to;
		t_bolted_to = handle;
		void* made = g_original_bolted(client, effect, time, handle, bone, a6, a7, a8);
		t_bolted_to = before;
		return made;
	}

	void hooked_loose(word_t a, word_t b, word_t c, word_t d, word_t e, word_t f, word_t g, word_t h, word_t i, word_t j)
	{
		if (g_hide_world)
		{
			g_held_back.fetch_add(1, std::memory_order_relaxed);
			return;
		}

		g_original_loose(a, b, c, d, e, f, g, h, i, j);
	}

	void hooked_brush(word_t a, word_t b, word_t c, word_t d, word_t e, word_t f, word_t g, word_t h)
	{
		if (g_hide_world)
		{
			g_held_back.fetch_add(1, std::memory_order_relaxed);
			return;
		}

		g_original_brush(a, b, c, d, e, f, g, h);
	}

	// An effect begun: not while what it is on is not shown (and, on a colour, not unless it is on what is shown).
	void* hooked_spawn(word_t a, word_t b, word_t c, word_t d, word_t e, word_t f, word_t g, word_t h, word_t i, word_t j, word_t k, word_t l, word_t handle, word_t n,
	                   word_t o, word_t p, word_t q, word_t r, word_t s2, word_t t, word_t u, word_t v, word_t w, word_t x, word_t y, word_t z)
	{
		if (g_playing && (g_hide_characters || g_only_view || g_only_characters))
		{
			const int given = static_cast<int>(handle & 0xFFFF);
			const int on = t_bolted_to >= 0 ? t_bolted_to : given != 0xFFF ? given : -1;
			const bool view = on >= FIRST_PERSON_HANDLE && on < FIRST_PERSON_HANDLE + VIEW_HANDLES;
			bool character = false;
			if (on >= 0 && on < FIRST_PERSON_HANDLE)
			{
				void* model = client_dobj(on);
				for (const auto& one : g_characters)
				{
					character = character || (model && one.load(std::memory_order_relaxed) == model);
				}
			}

			if ((g_only_view && !view) || (g_only_characters && !character) || (g_hide_characters && character))
			{
				g_effects_held.fetch_add(1, std::memory_order_relaxed);
				return nullptr;
			}
		}

		return g_original_spawn(a, b, c, d, e, f, g, h, i, j, k, l, handle, n, o, p, q, r, s2, t, u, v, w, x, y, z);
	}

	void* client_dobj(const int handle)
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

	bool character_handle(const int index, int* handle)
	{
		__try
		{
			const std::uintptr_t record = index < PLAYER_RECORDS                  ? CHARACTERS + static_cast<std::uintptr_t>(index) * CHARACTER_SIZE
			                              : index < PLAYER_RECORDS + MORE_RECORDS ? CHARACTERS_MORE + static_cast<std::uintptr_t>(index - PLAYER_RECORDS) * CHARACTER_SIZE
			                                                                      : CHARACTERS_BODIES +
			                                                                            static_cast<std::uintptr_t>(index - PLAYER_RECORDS - MORE_RECORDS) * CHARACTER_SIZE;
			const auto* at = reinterpret_cast<const int*>(record);
			*handle = at[0];
			// (a record in use has its models loaded, or at least names a model)
			return (at[1] != 0 || *reinterpret_cast<const char*>(record + 0x24) != 0) && at[0] >= 0 && at[0] < FIRST_PERSON_HANDLE;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	bool hook(const std::uintptr_t address, void* detour, void* original)
	{
		return MH_CreateHook(reinterpret_cast<LPVOID>(address), detour, static_cast<LPVOID*>(original)) == MH_OK &&
		       MH_EnableHook(reinterpret_cast<LPVOID>(address)) == MH_OK;
	}

	// ---- the game's settings a pass has changed (put back when it is left)
	int g_applied_elements = Everything;
	float g_applied_colour[3] = {-1.0f, -1.0f, -1.0f};

	void apply_settings(const int elements)
	{
		const bool colour = elements == OnlyPlayers || elements == OnlyGun;
		const bool gun = elements == Everything || elements == OnlyGun;
		const bool colour_changed = memcmp(g_applied_colour, g_colour, sizeof(g_colour)) != 0;
		// (rain, smoke, glows and water have no place on the colour: the game's drawing of effects and of water
		// is turned off there, unless effects on what is shown are asked to be kept)
		const int effects = colour && !g_keep_effects ? 0 : 1;
		if (effects != g_applied_effects && (g_applied_effects >= 0 || effects == 0))
		{
			theater_command(effects ? "fx_draw 1" : "fx_draw 0");
			theater_command(effects ? "fx_flare 1" : "fx_flare 0");
			theater_command(colour ? "r_drawWater 0" : "r_drawWater 1");
			g_applied_effects = effects;
		}

		if (elements == g_applied_elements && !(colour && colour_changed))
		{
			return;
		}

		const bool was_colour = g_applied_elements == OnlyPlayers || g_applied_elements == OnlyGun;
		const bool was_gun = g_applied_elements == Everything || g_applied_elements == OnlyGun;
		if (gun != was_gun)
		{
			theater_command(gun ? "cg_drawGun 1" : "cg_drawGun 0");
		}

		if (colour)
		{
			char text[96];
			snprintf(text, sizeof(text), "r_clearColor %g %g %g 1", g_colour[0], g_colour[1], g_colour[2]);
			theater_command("r_clear 3");
			theater_command(text);
			snprintf(text, sizeof(text), "r_clearColor2 %g %g %g 1", g_colour[0], g_colour[1], g_colour[2]);
			theater_command(text);
			// (what is drawn onto the map, or far behind it, would hang in the colour)
			theater_command("fx_marks 0");
			theater_command("r_drawSun 0");
			theater_command("fx_drawClouds 0");
			memcpy(g_applied_colour, g_colour, sizeof(g_colour));
		}
		else if (was_colour)
		{
			theater_command("r_clear 1");
			theater_command("r_clearColor 0 0 0 0");
			theater_command("r_clearColor2 0 0 0 0");
			theater_command("fx_marks 1");
			theater_command("r_drawSun 1");
			theater_command("fx_drawClouds 1");
			g_applied_colour[0] = -1.0f;
		}

		g_applied_elements = elements;
	}

	// ---- the fog
	constexpr std::uintptr_t FOG_SETS = 0x14D858C34, FOG_GOING_TO = 0x14D8594BC;
	constexpr int FOG_SET_COUNT = 7, FOG_SET_SIZE = 0x9C, FOG_DRAWN = 2;
	constexpr int FOG_NUMBER_COUNT = 12;
	const int FOG_ATMOS_NUMBERS[FOG_NUMBER_COUNT] = {0x40, 0x44, 0x48, 0x4C, 0x50, 0x54, 0x58, 0x5C, 0x60, 0x70, 0x8C, 0x90};

	constexpr float FOG_LN2 = 0.69314718f;
	struct FogWish
	{
		// (made: the map has no fog of the game's own, and one is made of the plain kind: start, half-way
		// distance, colour, how thick it gets at most (+0x04) and how bright (+0x24))
		bool made = false;
		float most = 1.0f, bright = 1.0f;
		bool colour_on = false, colour2_on = false, start_on = false, half_on = false;
		float colour[3] = {0.6f, 0.65f, 0.7f}, colour2[3] = {1.0f, 0.9f, 0.7f};
		float start = 0.0f, half = 2000.0f;
		bool number_on[FOG_NUMBER_COUNT] = {};
		float number[FOG_NUMBER_COUNT] = {};
	};

	struct FogShown
	{
		bool have = false, atmos = false, none = false;
		float start = 0.0f, half = 0.0f;
		float colour[3] = {}, colour2[3] = {};
		float number[FOG_NUMBER_COUNT] = {};
	};

	std::mutex g_fog_lock;
	FogWish g_fog_wish;
	FogShown g_fog_shown;
	unsigned char g_fog_original[FOG_SET_COUNT][FOG_SET_SIZE], g_fog_written[FOG_SET_COUNT][FOG_SET_SIZE];
	bool g_fog_have[FOG_SET_COUNT] = {};

	void put_colour(unsigned char* at, const float* colour)
	{
		for (int i = 0; i < 3; ++i)
		{
			const float value = colour[i] < 0.0f ? 0.0f : colour[i] > 1.0f ? 1.0f : colour[i];
			at[2 - i] = static_cast<unsigned char>(value * 255.0f + 0.5f);
		}
	}

	void get_colour(const unsigned char* at, float* colour)
	{
		for (int i = 0; i < 3; ++i)
		{
			colour[i] = at[2 - i] / 255.0f;
		}
	}

	void fog_apply(unsigned char* set, const FogWish& wish)
	{
		float own_half = 0.0f;
		memcpy(&own_half, set + 0x80, 4);
		if (wish.made && !set[0x3C] && own_half == 0.0f)
		{
			// (0.66: what is kept at +0x80 is not the half-way distance but the fog's density, ln 2 over that
			// distance: the drawing (0x1405FB553) hands the shader -density and density x start, and
			// 1 - (+0x04) as the least that shows through. 0.64 and 0.65 wrote the distance itself there:
			// a density thousands of times too great, which is what turned the fog inside out.)
			const float one = 1.0f, half = FOG_LN2 / (wish.half > 1.0f ? wish.half : 1.0f);
			memcpy(set + 0x00, &wish.start, 4);
			memcpy(set + 0x80, &half, 4);
			memcpy(set + 0x04, &wish.most, 4);
			memcpy(set + 0x24, &wish.bright, 4);
			memcpy(set + 0x14, &one, 4);
			put_colour(set + 0x7C, wish.colour);
			set[0x7C + 3] = 255;
			return;
		}

		const bool atmos = set[0x3C] != 0;
		if (wish.colour_on)
		{
			put_colour(set + (atmos ? 0x94 : 0x7C), wish.colour);
		}

		if (wish.colour2_on)
		{
			put_colour(set + (atmos ? 0x98 : 0x84), wish.colour2);
		}

		if (atmos)
		{
			for (int i = 0; i < FOG_NUMBER_COUNT; ++i)
			{
				if (wish.number_on[i])
				{
					memcpy(set + FOG_ATMOS_NUMBERS[i], &wish.number[i], 4);
				}
			}
		}
		else
		{
			if (wish.start_on)
			{
				memcpy(set + 0x00, &wish.start, 4);
			}

			if (wish.half_on)
			{
				const float density = FOG_LN2 / (wish.half > 1.0f ? wish.half : 1.0f);
				memcpy(set + 0x80, &density, 4);
			}
		}
	}

	// (on the game's thread) false: a fault reading or writing the game's fog. Every one of the seven sets is
	// brought to what is wished: 0.62 wrote the set the server gave and the one drawn from, but each frame
	// the game writes the one drawn from anew out of a copy it keeps of where the fog is going (set 4, with
	// the times of the change at +0x74 and +0x78 of it; 0x1405D347A), so nothing of it showed.
	constexpr int FOG_TO = 4;
	bool fog_step(const FogWish& wish, FogShown* shown, int* going_to)
	{
		__try
		{
			*going_to = *reinterpret_cast<const int*>(FOG_GOING_TO);
			for (int set = 0; set < FOG_SET_COUNT; ++set)
			{
				auto* at = reinterpret_cast<unsigned char*>(FOG_SETS + static_cast<std::uintptr_t>(set) * FOG_SET_SIZE);
				if (!g_fog_have[set] || memcmp(at, g_fog_written[set], FOG_SET_SIZE) != 0)
				{
					// (the game has set it anew)
					memcpy(g_fog_original[set], at, FOG_SET_SIZE);
					g_fog_have[set] = true;
				}

				unsigned char wanted[FOG_SET_SIZE];
				memcpy(wanted, g_fog_original[set], FOG_SET_SIZE);
				fog_apply(wanted, wish);
				if (memcmp(at, wanted, FOG_SET_SIZE) != 0)
				{
					memcpy(at, wanted, FOG_SET_SIZE);
				}

				memcpy(g_fog_written[set], wanted, FOG_SET_SIZE);
			}

			// what is shown as the game's own: where the fog is going, or the set the server gave if that is empty
			const unsigned char* own = g_fog_original[FOG_TO];
			float half = 0.0f;
			memcpy(&half, own + 0x80, 4);
			if (!own[0x3C] && half == 0.0f)
			{
				own = g_fog_original[*going_to >= 0 && *going_to < FOG_SET_COUNT ? *going_to : 1];
			}

			shown->have = true;
			shown->atmos = own[0x3C] != 0;
			memcpy(&shown->start, own + 0x00, 4);
			memcpy(&shown->half, own + 0x80, 4);
			shown->none = !shown->atmos && shown->half == 0.0f;
			// (shown as the half-way distance it stands for)
			shown->half = shown->half > 0.0f ? FOG_LN2 / shown->half : 0.0f;
			get_colour(own + (shown->atmos ? 0x94 : 0x7C), shown->colour);
			get_colour(own + (shown->atmos ? 0x98 : 0x84), shown->colour2);
			for (int n = 0; n < FOG_NUMBER_COUNT; ++n)
			{
				memcpy(&shown->number[n], own + FOG_ATMOS_NUMBERS[n], 4);
			}

			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	// ---- the depth, drawn over the picture
	struct Candidate
	{
		ID3D11Texture2D* texture;
		DXGI_FORMAT format;
		unsigned samples, clears, binds, width, height;
		float cleared_to;
	};
	ID3D11Texture2D* g_depth_chosen = nullptr;  // (only compared) the candidate that is the depth buffer, as of the last frame
	constexpr int CANDIDATE_MOST = 16;
	std::mutex g_depth_lock;
	Candidate g_candidates[CANDIDATE_MOST] = {}, g_last_candidates[CANDIDATE_MOST] = {};
	int g_candidate_count = 0, g_last_candidate_count = 0;
	ID3D11Texture2D* g_depth_source = nullptr;  // (a count of ours kept on it) the game's depth buffer as last seen cleared
	ID3D11Texture2D* g_depth_copy = nullptr;
	ID3D11ShaderResourceView* g_depth_view = nullptr;
	D3D11_TEXTURE2D_DESC g_depth_copy_desc{};
	bool g_depth_copied = false, g_depth_reversed = false;
	std::atomic<unsigned> g_picture_width{0}, g_picture_height{0};
	int g_depth_buffer = 0;  // which of the candidates (0: the first)
	int g_depth_taken = 0;   // 0: when the frame is done; n: before the buffer's n-th clear in a frame
	float g_depth_near = 4.0f, g_depth_range = 3000.0f, g_depth_curve = 1.0f, g_depth_far = 0.0f;
	bool g_depth_near_white = true, g_depth_raw = false;
	int g_depth_reversed_choice = 0;  // 0: as the game clears it; 1: near is 0; 2: near is 1
	char g_depth_status[200] = "";
	bool g_clear_hooked = false;

	using clear_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8);
	clear_t g_original_clear = nullptr, g_original_deferred_clear = nullptr;

	DXGI_FORMAT typeless_of(const DXGI_FORMAT format, DXGI_FORMAT* view)
	{
		switch (format)
		{
		case DXGI_FORMAT_R24G8_TYPELESS:
		case DXGI_FORMAT_D24_UNORM_S8_UINT:
			*view = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
			return DXGI_FORMAT_R24G8_TYPELESS;
		case DXGI_FORMAT_R32_TYPELESS:
		case DXGI_FORMAT_D32_FLOAT:
			*view = DXGI_FORMAT_R32_FLOAT;
			return DXGI_FORMAT_R32_TYPELESS;
		case DXGI_FORMAT_R32G8X24_TYPELESS:
		case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
			*view = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
			return DXGI_FORMAT_R32G8X24_TYPELESS;
		case DXGI_FORMAT_R16_TYPELESS:
		case DXGI_FORMAT_D16_UNORM:
			*view = DXGI_FORMAT_R16_UNORM;
			return DXGI_FORMAT_R16_TYPELESS;
		default:
			*view = DXGI_FORMAT_UNKNOWN;
			return DXGI_FORMAT_UNKNOWN;
		}
	}

	// (with the lock held) Our own copy of the depth buffer, made to match it.
	bool ready_copy(ID3D11Texture2D* source)
	{
		D3D11_TEXTURE2D_DESC desc{};
		source->GetDesc(&desc);
		DXGI_FORMAT view_format = DXGI_FORMAT_UNKNOWN;
		const DXGI_FORMAT typeless = typeless_of(desc.Format, &view_format);
		if (typeless == DXGI_FORMAT_UNKNOWN)
		{
			return false;
		}

		if (g_depth_copy && (g_depth_copy_desc.Width != desc.Width || g_depth_copy_desc.Height != desc.Height || g_depth_copy_desc.Format != typeless ||
		                     g_depth_copy_desc.SampleDesc.Count != desc.SampleDesc.Count))
		{
			if (g_depth_view)
			{
				g_depth_view->Release();
				g_depth_view = nullptr;
			}

			g_depth_copy->Release();
			g_depth_copy = nullptr;
		}

		if (!g_depth_copy)
		{
			ID3D11Device* device = nullptr;
			source->GetDevice(&device);
			if (!device)
			{
				return false;
			}

			D3D11_TEXTURE2D_DESC wanted = desc;
			wanted.Format = typeless;
			wanted.Usage = D3D11_USAGE_DEFAULT;
			wanted.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			wanted.CPUAccessFlags = 0;
			wanted.MiscFlags = 0;
			wanted.MipLevels = 1;
			wanted.ArraySize = 1;
			if (SUCCEEDED(device->CreateTexture2D(&wanted, nullptr, &g_depth_copy)) && g_depth_copy)
			{
				D3D11_SHADER_RESOURCE_VIEW_DESC view{};
				view.Format = view_format;
				view.ViewDimension = desc.SampleDesc.Count > 1 ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D;
				view.Texture2D.MipLevels = 1;
				device->CreateShaderResourceView(g_depth_copy, &view, &g_depth_view);
				g_depth_copy_desc = wanted;
			}

			device->Release();
		}

		return g_depth_copy && g_depth_view;
	}

	// A depth buffer the game has cleared (cleared: true) or set to be drawn to. 0.62 looked only at clears
	// of buffers of the picture's size and found none.
	void seen_depth(ID3D11DeviceContext* context, ID3D11DepthStencilView* view, const bool cleared, const FLOAT depth)
	{
		ID3D11Resource* resource = nullptr;
		view->GetResource(&resource);
		if (!resource)
		{
			return;
		}

		ID3D11Texture2D* texture = nullptr;
		if (SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture))) && texture)
		{
			std::lock_guard<std::mutex> lock(g_depth_lock);
			int index = -1;
			for (int i = 0; i < g_candidate_count; ++i)
			{
				index = g_candidates[i].texture == texture ? i : index;
			}

			if (index < 0 && g_candidate_count < CANDIDATE_MOST)
			{
				D3D11_TEXTURE2D_DESC desc{};
				texture->GetDesc(&desc);
				index = g_candidate_count++;
				g_candidates[index] = {texture, desc.Format, desc.SampleDesc.Count, 0, 0, desc.Width, desc.Height, -1.0f};
			}

			if (index >= 0)
			{
				Candidate& candidate = g_candidates[index];
				candidate.clears += cleared ? 1 : 0;
				candidate.binds += cleared ? 0 : 1;
				if (cleared)
				{
					candidate.cleared_to = depth;
				}

				if (texture == g_depth_chosen)
				{
					if (g_depth_source != texture)
					{
						if (g_depth_source)
						{
							g_depth_source->Release();
						}

						g_depth_source = texture;
						texture->AddRef();
					}

					if (cleared)
					{
						g_depth_reversed = depth < 0.5f;
						// (what the buffer holds before this clear, if that is what is asked for)
						if (g_depth_taken > 0 && static_cast<int>(candidate.clears) == g_depth_taken && ready_copy(texture))
						{
							context->CopyResource(g_depth_copy, texture);
							g_depth_copied = true;
						}
					}
				}
			}

			texture->Release();
		}

		resource->Release();
	}

	thread_local bool t_own_drawing = false;  // (the depth being drawn over the picture: not the game's doing)
	using targets_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
	targets_t g_original_targets = nullptr, g_original_deferred_targets = nullptr;

	void STDMETHODCALLTYPE hooked_clear(ID3D11DeviceContext* context, ID3D11DepthStencilView* view, const UINT flags, const FLOAT depth, const UINT8 stencil)
	{
		if (g_depth && view && (flags & D3D11_CLEAR_DEPTH) && !t_own_drawing)
		{
			seen_depth(context, view, true, depth);
		}

		g_original_clear(context, view, flags, depth, stencil);
	}

	void STDMETHODCALLTYPE hooked_deferred_clear(ID3D11DeviceContext* context, ID3D11DepthStencilView* view, const UINT flags, const FLOAT depth,
	                                             const UINT8 stencil)
	{
		if (g_depth && view && (flags & D3D11_CLEAR_DEPTH) && !t_own_drawing)
		{
			seen_depth(context, view, true, depth);
		}

		g_original_deferred_clear(context, view, flags, depth, stencil);
	}

	void STDMETHODCALLTYPE hooked_targets(ID3D11DeviceContext* context, const UINT count, ID3D11RenderTargetView* const* targets, ID3D11DepthStencilView* view)
	{
		if (g_depth && view && !t_own_drawing)
		{
			seen_depth(context, view, false, 0.0f);
		}

		g_original_targets(context, count, targets, view);
	}

	void STDMETHODCALLTYPE hooked_deferred_targets(ID3D11DeviceContext* context, const UINT count, ID3D11RenderTargetView* const* targets, ID3D11DepthStencilView* view)
	{
		if (g_depth && view && !t_own_drawing)
		{
			seen_depth(context, view, false, 0.0f);
		}

		g_original_deferred_targets(context, count, targets, view);
	}

	// (slot 34: what is drawn to, set together with the outputs of compute shaders)
	using targets_more_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*, UINT, UINT,
	                                                ID3D11UnorderedAccessView* const*, const UINT*);
	targets_more_t g_original_targets_more = nullptr, g_original_deferred_targets_more = nullptr;

	void STDMETHODCALLTYPE hooked_targets_more(ID3D11DeviceContext* context, const UINT count, ID3D11RenderTargetView* const* targets, ID3D11DepthStencilView* view,
	                                           const UINT first, const UINT outputs, ID3D11UnorderedAccessView* const* views, const UINT* counts)
	{
		// (a count of 0xFFFFFFFF keeps what is drawn to as it is: the depth buffer given then is not looked at)
		if (g_depth && view && count != 0xFFFFFFFF && !t_own_drawing)
		{
			seen_depth(context, view, false, 0.0f);
		}

		g_original_targets_more(context, count, targets, view, first, outputs, views, counts);
	}

	void STDMETHODCALLTYPE hooked_deferred_targets_more(ID3D11DeviceContext* context, const UINT count, ID3D11RenderTargetView* const* targets,
	                                                    ID3D11DepthStencilView* view, const UINT first, const UINT outputs, ID3D11UnorderedAccessView* const* views,
	                                                    const UINT* counts)
	{
		if (g_depth && view && count != 0xFFFFFFFF && !t_own_drawing)
		{
			seen_depth(context, view, false, 0.0f);
		}

		g_original_deferred_targets_more(context, count, targets, view, first, outputs, views, counts);
	}

	void hooked_game_clear(void** holder, const unsigned char which, const float* colour, const float depth, const unsigned char stencil)
	{
		if (g_depth && (which & 1) && holder && *holder && !t_own_drawing)
		{
			auto* context = static_cast<ID3D11DeviceContext*>(*holder);
			ID3D11RenderTargetView* target = nullptr;
			ID3D11DepthStencilView* view = nullptr;
			context->OMGetRenderTargets(1, &target, &view);
			if (view)
			{
				seen_depth(context, view, true, depth);
				view->Release();
			}

			if (target)
			{
				target->Release();
			}
		}

		g_original_game_clear(holder, which, colour, depth, stencil);
	}

	bool hook_method(void* method, void* detour, void* original)
	{
		return MH_CreateHook(method, detour, static_cast<LPVOID*>(original)) == MH_OK && MH_EnableHook(method) == MH_OK;
	}

	// (the methods are slots 53, the clearing of a depth buffer, and 33, the setting of what is drawn to, of
	// the drawing context: the game's own and the kind it draws ahead with have each their own)
	void hook_clears(ID3D11Device* device, ID3D11DeviceContext* context)
	{
		g_clear_hooked = true;
		void** own = *reinterpret_cast<void***>(context);
		const bool first = hook_method(own[53], reinterpret_cast<void*>(&hooked_clear), &g_original_clear);
		const bool first_set = hook_method(own[33], reinterpret_cast<void*>(&hooked_targets), &g_original_targets);
		const bool first_more = hook_method(own[34], reinterpret_cast<void*>(&hooked_targets_more), &g_original_targets_more);
		bool second_more = false;
		bool second = false, second_set = false;
		ID3D11DeviceContext* deferred = nullptr;
		if (SUCCEEDED(device->CreateDeferredContext(0, &deferred)) && deferred)
		{
			void** other = *reinterpret_cast<void***>(deferred);
			second = other[53] != own[53] && hook_method(other[53], reinterpret_cast<void*>(&hooked_deferred_clear), &g_original_deferred_clear);
			second_set = other[33] != own[33] && hook_method(other[33], reinterpret_cast<void*>(&hooked_deferred_targets), &g_original_deferred_targets);
			second_more = other[34] != own[34] && hook_method(other[34], reinterpret_cast<void*>(&hooked_deferred_targets_more), &g_original_deferred_targets_more);
			deferred->Release();
		}

		ui_log("passes: the game's depth buffers are watched: cleared %s/%s, set to be drawn to %s/%s and %s/%s (its own drawing / its drawing ahead)", first ? "yes" : "NO",
		       second ? "yes" : "no", first_set ? "yes" : "NO", second_set ? "yes" : "no", first_more ? "yes" : "NO", second_more ? "yes" : "no");
	}

	const char* const DEPTH_VS = "float4 main(uint id : SV_VertexID) : SV_POSITION { return float4((id == 2) ? 3.0 : -1.0, (id == 1) ? 3.0 : -1.0, 0.0, 1.0); }";
	const char* const DEPTH_PS_HEAD = "cbuffer P : register(b0) { float4 a; float4 b; float4 c; };\n";
	const char* const DEPTH_PS_ONE = "Texture2D<float> t : register(t0);\nfloat take(float2 p) { return t.Load(int3(p, 0)); }\n";
	const char* const DEPTH_PS_MANY = "Texture2DMS<float> t : register(t0);\nfloat take(float2 p) { return t.Load(int2(p), 0); }\n";
	const char* const DEPTH_PS_BODY =
		"float4 main(float4 pos : SV_POSITION) : SV_TARGET {\n"
		"  float d = take(pos.xy * c.xy);\n"
		"  if (a.z > 0.5) d = 1.0 - d;\n"
		"  float v = d;\n"
		"  if (a.w < 0.5) {\n"
		"    float z = b.z > 0.0 ? a.x * b.z / max(b.z - d * (b.z - a.x), 0.000001) : a.x / max(1.0 - d, 0.0000001);\n"
		"    v = saturate(z / a.y);\n"
		"  }\n"
		"  v = pow(saturate(v), b.x);\n"
		"  if (b.y > 0.5) v = 1.0 - v;\n"
		"  return float4(v, v, v, 1.0);\n"
		"}\n";
	ID3D11VertexShader* g_depth_vs = nullptr;
	ID3D11PixelShader* g_depth_ps[2] = {};
	ID3D11Buffer* g_depth_constants = nullptr;
	ID3D11RasterizerState* g_depth_raster = nullptr;
	ID3D11DepthStencilState* g_depth_stencil = nullptr;
	ID3D11BlendState* g_depth_blend = nullptr;
	bool g_depth_shaders_tried = false;

	bool ready_shaders(ID3D11Device* device)
	{
		if (g_depth_shaders_tried)
		{
			return g_depth_vs && g_depth_ps[0] && g_depth_constants;
		}

		g_depth_shaders_tried = true;
		ID3DBlob* code = nullptr;
		if (SUCCEEDED(D3DCompile(DEPTH_VS, strlen(DEPTH_VS), nullptr, nullptr, nullptr, "main", "vs_4_0", 0, 0, &code, nullptr)) && code)
		{
			device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_depth_vs);
			code->Release();
		}

		for (int i = 0; i < 2; ++i)
		{
			const std::string source = std::string(DEPTH_PS_HEAD) + (i ? DEPTH_PS_MANY : DEPTH_PS_ONE) + DEPTH_PS_BODY;
			code = nullptr;
			if (SUCCEEDED(D3DCompile(source.c_str(), source.size(), nullptr, nullptr, nullptr, "main", "ps_4_0", 0, 0, &code, nullptr)) && code)
			{
				device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_depth_ps[i]);
				code->Release();
			}
		}

		D3D11_BUFFER_DESC buffer{};
		buffer.ByteWidth = 48;
		buffer.Usage = D3D11_USAGE_DEFAULT;
		buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		device->CreateBuffer(&buffer, nullptr, &g_depth_constants);
		D3D11_RASTERIZER_DESC raster{};
		raster.FillMode = D3D11_FILL_SOLID;
		raster.CullMode = D3D11_CULL_NONE;
		raster.DepthClipEnable = FALSE;
		device->CreateRasterizerState(&raster, &g_depth_raster);
		D3D11_DEPTH_STENCIL_DESC stencil{};
		stencil.DepthEnable = FALSE;
		stencil.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
		stencil.DepthFunc = D3D11_COMPARISON_ALWAYS;
		device->CreateDepthStencilState(&stencil, &g_depth_stencil);
		D3D11_BLEND_DESC blend{};
		blend.RenderTarget[0].BlendEnable = FALSE;
		blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		device->CreateBlendState(&blend, &g_depth_blend);
		ui_log("passes: the depth's shaders: vertex %s, pixel %s, pixel for a many-sampled buffer %s", g_depth_vs ? "made" : "NOT made", g_depth_ps[0] ? "made" : "NOT made",
		       g_depth_ps[1] ? "made" : "not made");
		return g_depth_vs && g_depth_ps[0] && g_depth_constants;
	}

	// The depth drawn over the whole picture, the game's own drawing state kept and put back.
	void draw_depth(ID3D11DeviceContext* context, ID3D11RenderTargetView* target, const unsigned width, const unsigned height, const bool many, const bool reversed)
	{
		struct Kept
		{
			ID3D11RenderTargetView* targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
			ID3D11DepthStencilView* depth;
			UINT viewport_count;
			D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
			ID3D11RasterizerState* raster;
			ID3D11BlendState* blend;
			FLOAT blend_factor[4];
			UINT sample_mask, stencil_ref;
			ID3D11DepthStencilState* stencil;
			ID3D11ShaderResourceView* resource;
			ID3D11PixelShader* ps;
			ID3D11VertexShader* vs;
			ID3D11GeometryShader* gs;
			ID3D11HullShader* hs;
			ID3D11DomainShader* ds;
			ID3D11Buffer* constants;
			ID3D11InputLayout* layout;
			D3D11_PRIMITIVE_TOPOLOGY topology;
		};
		Kept kept{};
		context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, kept.targets, &kept.depth);
		kept.viewport_count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
		context->RSGetViewports(&kept.viewport_count, kept.viewports);
		context->RSGetState(&kept.raster);
		context->OMGetBlendState(&kept.blend, kept.blend_factor, &kept.sample_mask);
		context->OMGetDepthStencilState(&kept.stencil, &kept.stencil_ref);
		context->PSGetShaderResources(0, 1, &kept.resource);
		context->PSGetShader(&kept.ps, nullptr, nullptr);
		context->VSGetShader(&kept.vs, nullptr, nullptr);
		context->GSGetShader(&kept.gs, nullptr, nullptr);
		context->HSGetShader(&kept.hs, nullptr, nullptr);
		context->DSGetShader(&kept.ds, nullptr, nullptr);
		context->PSGetConstantBuffers(0, 1, &kept.constants);
		context->IAGetInputLayout(&kept.layout);
		context->IAGetPrimitiveTopology(&kept.topology);

		const float reverse = g_depth_reversed_choice == 1 ? 0.0f : g_depth_reversed_choice == 2 ? 1.0f : reversed ? 1.0f : 0.0f;
		const float values[12] = {g_depth_near, g_depth_range > 1.0f ? g_depth_range : 1.0f, reverse, g_depth_raw ? 1.0f : 0.0f, g_depth_curve > 0.05f ? g_depth_curve : 0.05f,
		                          g_depth_near_white ? 1.0f : 0.0f, g_depth_far, 0.0f, width ? static_cast<float>(g_depth_copy_desc.Width) / static_cast<float>(width) : 1.0f,
		                          height ? static_cast<float>(g_depth_copy_desc.Height) / static_cast<float>(height) : 1.0f, 0.0f, 0.0f};
		context->UpdateSubresource(g_depth_constants, 0, nullptr, values, 0, 0);
		D3D11_VIEWPORT viewport{};
		viewport.Width = static_cast<float>(width);
		viewport.Height = static_cast<float>(height);
		viewport.MaxDepth = 1.0f;
		const FLOAT factor[4] = {};
		context->OMSetRenderTargets(1, &target, nullptr);
		context->RSSetViewports(1, &viewport);
		context->RSSetState(g_depth_raster);
		context->OMSetBlendState(g_depth_blend, factor, 0xFFFFFFFF);
		context->OMSetDepthStencilState(g_depth_stencil, 0);
		context->IASetInputLayout(nullptr);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		context->VSSetShader(g_depth_vs, nullptr, 0);
		context->GSSetShader(nullptr, nullptr, 0);
		context->HSSetShader(nullptr, nullptr, 0);
		context->DSSetShader(nullptr, nullptr, 0);
		context->PSSetShader(many && g_depth_ps[1] ? g_depth_ps[1] : g_depth_ps[0], nullptr, 0);
		context->PSSetConstantBuffers(0, 1, &g_depth_constants);
		context->PSSetShaderResources(0, 1, &g_depth_view);
		context->Draw(3, 0);

		context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, kept.targets, kept.depth);
		context->RSSetViewports(kept.viewport_count, kept.viewports);
		context->RSSetState(kept.raster);
		context->OMSetBlendState(kept.blend, kept.blend_factor, kept.sample_mask);
		context->OMSetDepthStencilState(kept.stencil, kept.stencil_ref);
		context->PSSetShaderResources(0, 1, &kept.resource);
		context->PSSetShader(kept.ps, nullptr, 0);
		context->VSSetShader(kept.vs, nullptr, 0);
		context->GSSetShader(kept.gs, nullptr, 0);
		context->HSSetShader(kept.hs, nullptr, 0);
		context->DSSetShader(kept.ds, nullptr, 0);
		context->PSSetConstantBuffers(0, 1, &kept.constants);
		context->IASetInputLayout(kept.layout);
		context->IASetPrimitiveTopology(kept.topology);
		for (ID3D11RenderTargetView* one : kept.targets)
		{
			if (one)
			{
				one->Release();
			}
		}

		IUnknown* const held[] = {kept.depth, kept.raster, kept.blend, kept.stencil, kept.resource, kept.ps, kept.vs, kept.gs, kept.hs, kept.ds, kept.constants, kept.layout};
		for (IUnknown* one : held)
		{
			if (one)
			{
				one->Release();
			}
		}
	}
}

void passes_start()
{
	const bool glow = hook(BSP_CAMERA, &hooked_bsp, &g_original_bsp);
	const bool w0 = hook(WORLD_COMMANDS[0], reinterpret_cast<void*>(static_cast<command_t>(&hooked_world<0>)), &g_original_world[0]);
	const bool w1 = hook(WORLD_COMMANDS[1], reinterpret_cast<void*>(static_cast<command_t>(&hooked_world<1>)), &g_original_world[1]);
	const bool w2 = hook(WORLD_COMMANDS[2], reinterpret_cast<void*>(static_cast<command_t>(&hooked_world<2>)), &g_original_world[2]);
	const bool w3 = hook(WORLD_COMMANDS[3], reinterpret_cast<void*>(static_cast<command_t>(&hooked_world<3>)), &g_original_world[3]);
	const bool w4 = hook(WORLD_COMMANDS[4], reinterpret_cast<void*>(static_cast<command_t>(&hooked_world<4>)), &g_original_world[4]);
	const bool bolted = hook(FX_PLAY_BOLTED, reinterpret_cast<void*>(&hooked_bolted), &g_original_bolted);
	const bool loose = hook(LOOSE_MODEL_CAMERA, reinterpret_cast<void*>(&hooked_loose), &g_original_loose);
	const bool brush = hook(BRUSH_CAMERA, reinterpret_cast<void*>(&hooked_brush), &g_original_brush);
	const bool spawn = hook(FX_SPAWN, reinterpret_cast<void*>(&hooked_spawn), &g_original_spawn);
	const bool clear = hook(GAME_CLEAR, reinterpret_cast<void*>(&hooked_game_clear), &g_original_game_clear);
	const bool other = hook(0x1405C8840, reinterpret_cast<void*>(&hooked_dobj_other), &g_original_dobj_other);
	theater_log(other ? "hook: passes: the models' other drawing held" : "hook: passes: the models' other drawing NOT held");
	char more[160];
	snprintf(more, sizeof(more), "hook: passes: loose models %s, moving map pieces %s, effects begun %s, the game's clearing %s", loose ? "held" : "NOT held",
	         brush ? "held" : "NOT held", spawn ? "held" : "NOT held", clear ? "held" : "NOT held");
	theater_log(more);
	const bool models = hook(DOBJ_CAMERA, &hooked_dobj, &g_original_dobj);
	char text[200];
	snprintf(text, sizeof(text), "hook: passes: the map's surfaces %s/%s/%s, its static models %s/%s/%s, the models' drawing %s, effects on models %s", glow ? "held" : "NOT",
	         w0 ? "held" : "NOT", w1 ? "held" : "NOT", w2 ? "held" : "NOT", w3 ? "held" : "NOT", w4 ? "held" : "NOT", models ? "held" : "NOT held", bolted ? "held" : "NOT held");
	theater_log(text);
}

// (on the game's thread, before the scene is put together)
void passes_game_frame(const TheaterState& state)
{
	static int told = 0;
	static ULONGLONG told_at = 0;
	g_playing = state.playing;
	const int elements = state.playing ? g_elements.load() : Everything;
	g_hide_world = elements == OnlyPlayers || elements == OnlyGun;
	g_hide_characters = elements == OnlyWorld;
	g_only_characters = elements == OnlyPlayers;
	g_only_view = elements == OnlyGun;
	if (state.playing)
	{
		for (int i = 0; i < CHARACTER_COUNT; ++i)
		{
			int handle = -1;
			g_characters[i] = character_handle(i, &handle) ? client_dobj(handle) : nullptr;
		}

		for (int i = 0; i < VIEW_HANDLES; ++i)
		{
			g_views[i] = client_dobj(FIRST_PERSON_HANDLE + i);
		}
	}

	apply_settings(elements);
	const ULONGLONG now = GetTickCount64();
	if (state.playing && (elements != Everything || g_gun_true_depth) && told < 40 && now - told_at > 3000)
	{
		int characters = 0, views = 0;
		for (const auto& one : g_characters)
		{
			characters += one.load() ? 1 : 0;
		}

		for (const auto& one : g_views)
		{
			views += one.load() ? 1 : 0;
		}

		++told;
		told_at = now;
		char text[200];
		snprintf(text, sizeof(text), "passes: showing \"%s\": %d characters' models, %d first-person models (flags %X), %d models, %d map steps, %d effects held back",
		         ELEMENT_NAMES[elements], characters, views, g_view_flags.load(), g_held_back.exchange(0), g_world_held.exchange(0), g_effects_held.exchange(0));
		theater_log(text);
	}

	FogWish wish;
	{
		std::lock_guard<std::mutex> lock(g_fog_lock);
		wish = g_fog_wish;
	}

	if (!state.playing)
	{
		memset(g_fog_have, 0, sizeof(g_fog_have));
		std::lock_guard<std::mutex> lock(g_fog_lock);
		g_fog_shown.have = false;
		return;
	}

	FogShown shown;
	int going_to = -1;
	const bool stepped = fog_step(wish, &shown, &going_to);
	static FogWish told_wish;
	static int fog_told = 0;
	static bool fog_first = true;
	if (fog_told < 24 && (fog_first || memcmp(&told_wish, &wish, sizeof(wish)) != 0))
	{
		// (what the game's own fog is, and what is asked of it: for working out which number is which)
		++fog_told;
		fog_first = false;
		told_wish = wish;
		std::string numbers, asked;
		for (int i = 0; i < FOG_NUMBER_COUNT; ++i)
		{
			char one[40];
			snprintf(one, sizeof(one), " %g", shown.number[i]);
			numbers += one;
			if (wish.number_on[i])
			{
				snprintf(one, sizeof(one), " #%d=%g", i + 1, wish.number[i]);
				asked += one;
			}
		}

		char text[520];
		snprintf(text, sizeof(text), "fog: %s; going to set %d; the game's own: %s, start %g, half %g, colours %.2f %.2f %.2f / %.2f %.2f %.2f, numbers%s; asked: made %d (start %g half %g most %g bright %g), colour %d, second %d, start %d, half %d%s",
		         stepped ? "read" : "NOT read (a fault)", going_to, shown.atmos ? "atmosphere kind" : "plain kind", shown.start, shown.half, shown.colour[0], shown.colour[1],
		         shown.colour[2], shown.colour2[0], shown.colour2[1], shown.colour2[2], numbers.c_str(), wish.made ? 1 : 0, wish.start, wish.half, wish.most, wish.bright, wish.colour_on ? 1 : 0,
		         wish.colour2_on ? 1 : 0, wish.start_on ? 1 : 0, wish.half_on ? 1 : 0, asked.c_str());
		theater_log(text);
	}

	if (stepped)
	{
		std::lock_guard<std::mutex> lock(g_fog_lock);
		g_fog_shown = shown;
	}
}

// (as the frame is shown, before the editor copies the picture)
void passes_before_copy(IDXGISwapChain* swap_chain, ID3D11Device* device, ID3D11DeviceContext* context, const bool playing)
{
	if (!swap_chain || !device || !context)
	{
		return;
	}

	ID3D11Texture2D* back = nullptr;
	if (FAILED(swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))) || !back)
	{
		return;
	}

	D3D11_TEXTURE2D_DESC picture{};
	back->GetDesc(&picture);
	g_picture_width = picture.Width;
	g_picture_height = picture.Height;
	const bool wanted = g_depth && playing;
	if (wanted && !g_clear_hooked)
	{
		hook_clears(device, context);
	}

	ID3D11Texture2D* source = nullptr;
	bool copied = false, reversed = false, many = false;
	{
		std::lock_guard<std::mutex> lock(g_depth_lock);
		memcpy(g_last_candidates, g_candidates, sizeof(g_candidates));
		g_last_candidate_count = g_candidate_count;
		g_candidate_count = 0;
		memset(g_candidates, 0, sizeof(g_candidates));
		// the depth buffer: the one asked for by number, or of itself the one of the picture's size that
		// was drawn to most (failing that, the one drawn to most)
		if (wanted && g_last_candidate_count)
		{
			int best = 0;
			unsigned best_score = 0;
			for (int i = 0; i < g_last_candidate_count; ++i)
			{
				const Candidate& one = g_last_candidates[i];
				const unsigned score = (one.width == picture.Width && one.height == picture.Height ? 1000000u : 0u) + one.binds + one.clears;
				if (score > best_score)
				{
					best = i;
					best_score = score;
				}
			}

			// (0.63 took whatever was there, and showed what looks like the sun's shadow map)
			const bool by_number = g_depth_buffer > 0 && g_depth_buffer <= g_last_candidate_count;
			const int index = by_number ? g_depth_buffer - 1 : best;
			ID3D11Texture2D* chosen = by_number || best_score >= 1000000u ? g_last_candidates[index].texture : nullptr;
			if (g_depth_chosen != chosen)
			{
				g_depth_chosen = chosen;
				g_depth_copied = false;
				if (!chosen && g_depth_source)
				{
					g_depth_source->Release();
					g_depth_source = nullptr;
				}
			}
		}
		else if (!wanted)
		{
			g_depth_chosen = nullptr;
		}
		if (!wanted)
		{
			if (g_depth_source)
			{
				g_depth_source->Release();
				g_depth_source = nullptr;
			}
		}
		else if (g_depth_source)
		{
			source = g_depth_source;
			source->AddRef();
			reversed = g_depth_reversed;
			if (g_depth_taken == 0 && ready_copy(source))
			{
				context->CopyResource(g_depth_copy, source);
				g_depth_copied = true;
			}

			copied = g_depth_copied && g_depth_copy && g_depth_view;
			many = g_depth_copy_desc.SampleDesc.Count > 1;
		}
	}

	static ULONGLONG told_at = 0;
	static int told = 0;
	if (wanted)
	{
		const ULONGLONG now = GetTickCount64();
		if (told < 16 && now - told_at > 2000)
		{
			++told;
			told_at = now;
			std::string all;
			for (int i = 0; i < g_last_candidate_count; ++i)
			{
				char one[160];
				snprintf(one, sizeof(one), " [%d%s: %ux%u, format %d, %u samples, set %u times, cleared %u times to %g]", i + 1,
				         g_last_candidates[i].texture == g_depth_chosen ? "*" : "", g_last_candidates[i].width, g_last_candidates[i].height,
				         static_cast<int>(g_last_candidates[i].format), g_last_candidates[i].samples, g_last_candidates[i].binds, g_last_candidates[i].clears,
				         g_last_candidates[i].cleared_to);
				all += one;
			}

			ui_log("passes: the picture is %ux%u; depth buffers the game used this frame:%s%s", picture.Width, picture.Height, all.empty() ? " none" : all.c_str(),
			       copied ? "; a copy is drawn" : "; NO copy to draw yet");
		}

		if (g_last_candidate_count)
		{
			snprintf(g_depth_status, sizeof(g_depth_status), "%d depth buffer%s seen this frame%s.", g_last_candidate_count, g_last_candidate_count == 1 ? "" : "s",
			         copied ? "" : "; none of the picture's size is being shown (try Buffer 1, 2, ...)");
		}
		else
		{
			snprintf(g_depth_status, sizeof(g_depth_status), "The game's depth buffer has not been found yet.");
		}
	}

	if (wanted && copied && ready_shaders(device))
	{
		ID3D11RenderTargetView* target = nullptr;
		if (SUCCEEDED(device->CreateRenderTargetView(back, nullptr, &target)) && target)
		{
			std::lock_guard<std::mutex> lock(g_depth_lock);
			t_own_drawing = true;
			draw_depth(context, target, picture.Width, picture.Height, many, reversed);
			t_own_drawing = false;
			target->Release();
		}
	}

	if (source)
	{
		source->Release();
	}

	back->Release();
}

// (the picture's size is changing: nothing of the old one is held on to)
void passes_release()
{
	std::lock_guard<std::mutex> lock(g_depth_lock);
	if (g_depth_source)
	{
		g_depth_source->Release();
		g_depth_source = nullptr;
	}

	if (g_depth_view)
	{
		g_depth_view->Release();
		g_depth_view = nullptr;
	}

	if (g_depth_copy)
	{
		g_depth_copy->Release();
		g_depth_copy = nullptr;
	}

	g_depth_copied = false;
	g_depth_chosen = nullptr;
	g_candidate_count = 0;
}

int passes_count()
{
	return PASS_COUNT;
}

bool passes_ticked(const int index)
{
	return index >= 0 && index < PASS_COUNT && g_ticked[index];
}

// index -1: everything, the picture (what a recording ends on)
void passes_use(const int index)
{
	if (index < 0 || index >= PASS_COUNT)
	{
		g_elements = Everything;
		g_depth = false;
		return;
	}

	g_elements = PASSES[index].elements;
	g_depth = PASSES[index].depth;
}

// The word for what is shown now, for a file's name ("" for the whole picture).
const char* passes_tag()
{
	static const char* const WORDS[ELEMENT_COUNT] = {"", "nogun", "world", "players", "gun"};
	const int elements = g_elements;
	const bool depth = g_depth;
	snprintf(g_tag, sizeof(g_tag), "%s%s%s", WORDS[elements], WORDS[elements][0] && depth ? "_" : "", depth ? "depth" : "");
	return g_tag;
}

const char* passes_label(const int index)
{
	return index >= 0 && index < PASS_COUNT ? PASSES[index].label : "";
}

void passes_draw_ui(const bool busy)
{
	if (!ui_section(ICON_EYE, "Passes"))
	{
		return;
	}

	ImGui::BeginDisabled(busy);
	int elements = g_elements;
	ui_row_label("Show");
	ImGui::SetNextItemWidth(-FLT_MIN);
	if (ImGui::BeginCombo("##passElements", ELEMENT_NAMES[elements]))
	{
		for (int i = 0; i < ELEMENT_COUNT; ++i)
		{
			if (ImGui::Selectable(ELEMENT_NAMES[i], i == elements))
			{
				g_elements = i;
				if (i != elements)
				{
					// (effects that are playing were begun under the last pass's rules: the recording is gone
					// back in to where it is, which begins them again under this one's)
					theater_rebuild();
				}
			}
		}

		ImGui::EndCombo();
	}

	bool depth = g_depth;
	ui_row_label("As depth");
	if (ImGui::Checkbox("##passDepth", &depth))
	{
		g_depth = depth;
	}

	ImGui::SetItemTooltip("The picture's depth in greys instead of the picture: how far each point is from the camera.");
	ImGui::EndDisabled();
	if (elements == OnlyPlayers || elements == OnlyGun)
	{
		ui_row_label("Colour behind");
		ImGui::SetNextItemWidth(-FLT_MIN);
		ImGui::ColorEdit3("##passColour", g_colour, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
		if (ui_reset_menu("##passColourReset"))
		{
			g_colour[0] = 0.0f, g_colour[1] = 1.0f, g_colour[2] = 0.0f;
		}

		bool keep = g_keep_effects;
		ui_row_label("Keep effects");
		if (ImGui::Checkbox("##passEffects", &keep))
		{
			g_keep_effects = keep;
			theater_rebuild();
		}

		ImGui::SetItemTooltip("Off: no effects are drawn on the colour at all (rain, smoke, glows, muzzle flashes).\nOn: effects fixed to what is shown (the gun's, or the players') are drawn; others that were already playing may linger.");
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextDisabled("The map and its sky are not drawn. Glow and the film settings still act on the colour: turn them off in Look for a clean one.");
		ImGui::PopTextWrapPos();
	}

	if (depth)
	{
		ui_row_label("Range");
		ImGui::SetNextItemWidth(-FLT_MIN);
		ImGui::SliderFloat("##depthRange", &g_depth_range, 50.0f, 20000.0f, "%.0f units", ImGuiSliderFlags_Logarithmic);
		if (ui_reset_menu("##depthRangeReset"))
		{
			g_depth_range = 3000.0f;
		}

		ImGui::SetItemTooltip("How far away the grey runs out.");
		ui_row_label("Curve");
		ImGui::SetNextItemWidth(-FLT_MIN);
		ImGui::SliderFloat("##depthCurve", &g_depth_curve, 0.1f, 4.0f, "%.2f");
		if (ui_reset_menu("##depthCurveReset"))
		{
			g_depth_curve = 1.0f;
		}

		ui_row_label("Near is white");
		ImGui::Checkbox("##depthWhite", &g_depth_near_white);
		if (ui_reset_menu("##depthWhiteReset"))
		{
			g_depth_near_white = true;
		}

		if (ImGui::TreeNode("More about the depth"))
		{
			static const char* const TAKEN[] = {"When the frame is done", "Before its 1st clear", "Before its 2nd clear", "Before its 3rd clear", "Before its 4th clear"};
			static const char* const WAYS[] = {"As the game clears it", "Near is 0", "Near is 1"};
			ui_row_label("Buffer");
			ImGui::SetNextItemWidth(-FLT_MIN);
			int buffer = g_depth_buffer;
			if (ImGui::SliderInt("##depthBuffer", &buffer, 0, CANDIDATE_MOST, buffer ? "%d" : "found by itself"))
			{
				std::lock_guard<std::mutex> lock(g_depth_lock);
				g_depth_buffer = buffer;
			}

			ui_row_label("Taken");
			ImGui::SetNextItemWidth(-FLT_MIN);
			int taken = g_depth_taken;
			if (ImGui::Combo("##depthTaken", &taken, TAKEN, 5))
			{
				std::lock_guard<std::mutex> lock(g_depth_lock);
				g_depth_taken = taken;
				g_depth_copied = false;
			}

			ImGui::SetItemTooltip("If the depth shows only the gun, or lacks the gun, the game clears its depth part way through a frame:\ntry the others.");
			ui_row_label("Direction");
			ImGui::SetNextItemWidth(-FLT_MIN);
			ImGui::Combo("##depthWay", &g_depth_reversed_choice, WAYS, 3);
		if (ui_reset_menu("##depthWayReset"))
		{
			g_depth_reversed_choice = 0;
		}

			ui_row_label("Near plane");
			ImGui::SetNextItemWidth(-FLT_MIN);
			ImGui::SliderFloat("##depthNear", &g_depth_near, 0.01f, 16.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
		if (ui_reset_menu("##depthNearReset"))
		{
			g_depth_near = 4.0f;
		}

			ui_row_label("Far plane");
			ImGui::SetNextItemWidth(-FLT_MIN);
			ImGui::SliderFloat("##depthFar", &g_depth_far, 0.0f, 100000.0f, g_depth_far > 0.0f ? "%.0f" : "none");
		if (ui_reset_menu("##depthFarReset"))
		{
			g_depth_far = 0.0f;
		}

			ui_row_label("As stored");
			ImGui::Checkbox("##depthRaw", &g_depth_raw);
		if (ui_reset_menu("##depthRawReset"))
		{
			g_depth_raw = false;
		}

			ImGui::TextDisabled("%s", g_depth_status);
			ImGui::TreePop();
		}
	}

	bool true_depth = g_gun_true_depth;
	ui_row_label("Gun at its true depth");
	if (ImGui::Checkbox("##gunDepth", &true_depth))
	{
		g_gun_true_depth = true_depth;
		// (the gun's nearest parts are closer than the game's near plane of 4)
		theater_command(true_depth ? "r_znear 0.5" : "r_znear 4");
	}

	ImGui::SetItemTooltip("For depth effects such as ReShade's depth of field: the game's near plane is brought in from 4 to 0.5, which spreads the gun's depth out\nso a focus set very near takes in the gun and nothing else. Untick to put it back.");
	ImGui::Spacing();
	ImGui::TextDisabled("Recorded one after another when Start recording is pressed:");
	ImGui::BeginDisabled(busy);
	for (int i = 0; i < PASS_COUNT; ++i)
	{
		ImGui::Checkbox(PASSES[i].label, &g_ticked[i]);
		if (i % 2 == 0 && i + 1 < PASS_COUNT)
		{
			ImGui::SameLine(ImGui::GetWindowWidth() * 0.5f);
		}
	}

	ImGui::EndDisabled();
	ImGui::TextDisabled("None ticked: what is shown now is recorded once.");
}

void passes_draw_fog()
{
	FogWish wish;
	FogShown shown;
	{
		std::lock_guard<std::mutex> lock(g_fog_lock);
		wish = g_fog_wish;
		shown = g_fog_shown;
	}

	if (!shown.have)
	{
		ImGui::TextDisabled("The fog's own colour and thickness can be changed while a recording plays.");
		return;
	}

	bool changed = false;
	const auto colour_row = [&](const char* label, const char* id, bool* on, float* value, const float* own) {
		ImGui::PushID(id);
		ui_row_label(label);
		if (ImGui::Checkbox("##on", on))
		{
			changed = true;
			if (*on)
			{
				memcpy(value, own, 12);
			}
		}

		ImGui::SameLine();
		ImGui::BeginDisabled(!*on);
		float picked[3];
		memcpy(picked, *on ? value : own, 12);
		ImGui::SetNextItemWidth(-FLT_MIN);
		if (ImGui::ColorEdit3("##colour", picked, ImGuiColorEditFlags_NoLabel))
		{
			memcpy(value, picked, 12);
			changed = true;
		}

		ImGui::EndDisabled();
		ImGui::PopID();
	};
	const auto number_row = [&](const char* label, const char* id, bool* on, float* value, const float own) {
		ImGui::PushID(id);
		ui_row_label(label);
		if (ImGui::Checkbox("##on", on))
		{
			changed = true;
			if (*on)
			{
				*value = own;
			}
		}

		ImGui::SameLine();
		ImGui::BeginDisabled(!*on);
		float picked = *on ? *value : own;
		const float reach = (own > 1.0f || own < -1.0f ? (own < 0.0f ? -own : own) : 1.0f) * 8.0f;
		ImGui::SetNextItemWidth(-FLT_MIN);
		if (ImGui::DragFloat("##value", &picked, reach / 800.0f, -reach * 4.0f, reach * 4.0f, "%.4g"))
		{
			*value = picked;
			changed = true;
		}

		ImGui::EndDisabled();
		ImGui::PopID();
	};
	if (shown.none)
	{
		// (0.63's log: every one of the game's fog sets was empty on the map tried, so there was nothing to change)
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextDisabled("The game has no fog of its own in this recording (any haze you see is the map's effects or lighting). One can be made here.");
		ImGui::PopTextWrapPos();
		ui_row_label("Make fog");
		changed = ImGui::Checkbox("##fogMade", &wish.made) || changed;
		ImGui::BeginDisabled(!wish.made);
		ui_row_label("Colour");
		ImGui::SetNextItemWidth(-FLT_MIN);
		changed = ImGui::ColorEdit3("##fogMadeColour", wish.colour, ImGuiColorEditFlags_NoLabel) || changed;
		ui_row_label("Starts at");
		ImGui::SetNextItemWidth(-FLT_MIN);
		changed = ImGui::SliderFloat("##fogMadeStart", &wish.start, 0.0f, 5000.0f, "%.0f units") || changed;
		ui_row_label("Half-way distance");
		ImGui::SetNextItemWidth(-FLT_MIN);
		changed = ImGui::SliderFloat("##fogMadeHalf", &wish.half, 50.0f, 20000.0f, "%.0f units", ImGuiSliderFlags_Logarithmic) || changed;
		ImGui::SetItemTooltip("Shorter is thicker.");
		ui_row_label("Thickest");
		ImGui::SetNextItemWidth(-FLT_MIN);
		changed = ImGui::SliderFloat("##fogMadeMost", &wish.most, 0.0f, 1.0f, "%.2f") || changed;
		ui_row_label("Brightness");
		ImGui::SetNextItemWidth(-FLT_MIN);
		changed = ImGui::SliderFloat("##fogMadeBright", &wish.bright, 0.0f, 20.0f, "%.2f") || changed;
		ImGui::SetItemTooltip("How bright the fog's colour is. 1 is the usual.");
		ImGui::EndDisabled();
		if (changed)
		{
			std::lock_guard<std::mutex> lock(g_fog_lock);
			g_fog_wish = wish;
		}

		return;
	}

	colour_row("Fog colour", "fogColour", &wish.colour_on, wish.colour, shown.colour);
	colour_row(shown.atmos ? "Haze colour" : "Colour towards the sun", "fogColour2", &wish.colour2_on, wish.colour2, shown.colour2);
	if (!shown.atmos)
	{
		number_row("Fog starts at", "fogStart", &wish.start_on, &wish.start, shown.start);
		number_row("Half-way distance", "fogHalf", &wish.half_on, &wish.half, shown.half);
		ImGui::TextDisabled("A shorter half-way distance is a thicker fog.");
	}
	else
	{
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextDisabled("This map's fog is the game's atmosphere kind. Its numbers are below as the game has them; which is which is not worked out yet, so they go by number: tick one and "
		                    "drag it to see what it does (distances are the big ones; a shorter distance is a thicker fog).");
		ImGui::PopTextWrapPos();
		for (int i = 0; i < FOG_NUMBER_COUNT; ++i)
		{
			char label[24], id[16];
			snprintf(label, sizeof(label), "Fog number %d", i + 1);
			snprintf(id, sizeof(id), "fogN%d", i);
			number_row(label, id, &wish.number_on[i], &wish.number[i], shown.number[i]);
		}
	}

	if (ImGui::SmallButton("The game's own fog again"))
	{
		wish = FogWish{};
		changed = true;
	}

	if (changed)
	{
		std::lock_guard<std::mutex> lock(g_fog_lock);
		g_fog_wish = wish;
	}
}
