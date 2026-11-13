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
	constexpr std::uintptr_t BSP_CAMERA = 0x1405CB8D0, SMODEL_CAMERA = 0x1405C9AE0, DOBJ_CAMERA = 0x1405C8BB0;
	constexpr std::uintptr_t COM_GET_CLIENT_DOBJ = 0x1403D2BA0, CHARACTERS = 0x141620568;
	constexpr int CHARACTER_COUNT = 42, CHARACTER_SIZE = 0xCC0, FIRST_PERSON_HANDLE = 2048, VIEW_HANDLES = 8;

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
	char g_tag[32] = "";

	// the models of the players (and their bodies) and of the first person, as of this frame
	std::atomic<void*> g_characters[CHARACTER_COUNT];
	std::atomic<void*> g_views[VIEW_HANDLES];
	std::atomic<int> g_held_back{0};
	std::atomic<unsigned> g_view_flags{0};

	using bsp_t = void (*)(void*, void*, void*);
	using smodel_t = void (*)(void*);
	using dobj_t = void (*)(void*, void*, unsigned char*, int, int, void*, void*, int);
	bsp_t g_original_bsp = nullptr;
	smodel_t g_original_smodel = nullptr;
	dobj_t g_original_dobj = nullptr;

	void hooked_bsp(void* a, void* b, void* c)
	{
		if (!g_hide_world)
		{
			g_original_bsp(a, b, c);
		}
	}

	void hooked_smodel(void* a)
	{
		if (!g_hide_world)
		{
			g_original_smodel(a);
		}
	}

	void hooked_dobj(void* a, void* b, unsigned char* entry, const int d, const int e, void* f, void* g, const int h)
	{
		void* model = entry ? *reinterpret_cast<void**>(entry + 0x68) : nullptr;
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
			const unsigned flags = *reinterpret_cast<unsigned*>(entry + 0x94);
			g_view_flags = flags;
			if (g_gun_true_depth && (flags & 0xE))
			{
				// (bits 1 to 3 of the entry's flags are what has a model drawn pressed to the front of the
				// depth: without them the gun is drawn at the depth it is really at)
				*reinterpret_cast<unsigned*>(entry + 0x94) = flags & ~0xEu;
				g_original_dobj(a, b, entry, d, e, f, g, h);
				*reinterpret_cast<unsigned*>(entry + 0x94) = flags;
				return;
			}
		}

		g_original_dobj(a, b, entry, d, e, f, g, h);
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
			const auto* at = reinterpret_cast<const int*>(CHARACTERS + static_cast<std::uintptr_t>(index) * CHARACTER_SIZE);
			*handle = at[0];
			return at[1] != 0 && at[0] >= 0 && at[0] < FIRST_PERSON_HANDLE;
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

	struct FogWish
	{
		bool colour_on = false, colour2_on = false, start_on = false, half_on = false;
		float colour[3] = {0.6f, 0.65f, 0.7f}, colour2[3] = {1.0f, 0.9f, 0.7f};
		float start = 0.0f, half = 2000.0f;
		bool number_on[FOG_NUMBER_COUNT] = {};
		float number[FOG_NUMBER_COUNT] = {};
	};

	struct FogShown
	{
		bool have = false, atmos = false;
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
				memcpy(set + 0x80, &wish.half, 4);
			}
		}
	}

	// (on the game's thread) false: a fault reading or writing the game's fog
	bool fog_step(const FogWish& wish, FogShown* shown)
	{
		__try
		{
			const int going_to = *reinterpret_cast<const int*>(FOG_GOING_TO);
			const int sets[2] = {going_to >= 0 && going_to < FOG_SET_COUNT ? going_to : 1, FOG_DRAWN};
			for (int i = 0; i < 2; ++i)
			{
				const int set = sets[i];
				if (i == 1 && set == sets[0])
				{
					continue;
				}

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
				if (i == 0)
				{
					const unsigned char* own = g_fog_original[set];
					shown->have = true;
					shown->atmos = own[0x3C] != 0;
					memcpy(&shown->start, own + 0x00, 4);
					memcpy(&shown->half, own + 0x80, 4);
					get_colour(own + (shown->atmos ? 0x94 : 0x7C), shown->colour);
					get_colour(own + (shown->atmos ? 0x98 : 0x84), shown->colour2);
					for (int n = 0; n < FOG_NUMBER_COUNT; ++n)
					{
						memcpy(&shown->number[n], own + FOG_ATMOS_NUMBERS[n], 4);
					}
				}
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
		unsigned samples, clears;
		float cleared_to;
	};
	constexpr int CANDIDATE_MOST = 6;
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

	void seen_clear(ID3D11DeviceContext* context, ID3D11DepthStencilView* view, const UINT flags, const FLOAT depth)
	{
		if (!g_depth || !view || !(flags & D3D11_CLEAR_DEPTH))
		{
			return;
		}

		ID3D11Resource* resource = nullptr;
		view->GetResource(&resource);
		if (!resource)
		{
			return;
		}

		ID3D11Texture2D* texture = nullptr;
		if (SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture))) && texture)
		{
			D3D11_TEXTURE2D_DESC desc{};
			texture->GetDesc(&desc);
			if (desc.Width == g_picture_width && desc.Height == g_picture_height && desc.Width)
			{
				std::lock_guard<std::mutex> lock(g_depth_lock);
				int index = -1;
				for (int i = 0; i < g_candidate_count; ++i)
				{
					index = g_candidates[i].texture == texture ? i : index;
				}

				if (index < 0 && g_candidate_count < CANDIDATE_MOST)
				{
					index = g_candidate_count++;
					g_candidates[index] = {texture, desc.Format, desc.SampleDesc.Count, 0, depth};
				}

				if (index >= 0)
				{
					Candidate& candidate = g_candidates[index];
					++candidate.clears;
					candidate.cleared_to = depth;
					if (index == g_depth_buffer)
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

	void STDMETHODCALLTYPE hooked_clear(ID3D11DeviceContext* context, ID3D11DepthStencilView* view, const UINT flags, const FLOAT depth, const UINT8 stencil)
	{
		seen_clear(context, view, flags, depth);
		g_original_clear(context, view, flags, depth, stencil);
	}

	void STDMETHODCALLTYPE hooked_deferred_clear(ID3D11DeviceContext* context, ID3D11DepthStencilView* view, const UINT flags, const FLOAT depth,
	                                             const UINT8 stencil)
	{
		seen_clear(context, view, flags, depth);
		g_original_deferred_clear(context, view, flags, depth, stencil);
	}

	void hook_clears(ID3D11Device* device, ID3D11DeviceContext* context)
	{
		g_clear_hooked = true;
		void* immediate = (*reinterpret_cast<void***>(context))[53];
		const bool first = MH_CreateHook(immediate, reinterpret_cast<LPVOID>(&hooked_clear), reinterpret_cast<LPVOID*>(&g_original_clear)) == MH_OK &&
		                   MH_EnableHook(immediate) == MH_OK;
		bool second = false;
		ID3D11DeviceContext* deferred = nullptr;
		if (SUCCEEDED(device->CreateDeferredContext(0, &deferred)) && deferred)
		{
			void* other = (*reinterpret_cast<void***>(deferred))[53];
			second = other != immediate &&
			         MH_CreateHook(other, reinterpret_cast<LPVOID>(&hooked_deferred_clear), reinterpret_cast<LPVOID*>(&g_original_deferred_clear)) == MH_OK &&
			         MH_EnableHook(other) == MH_OK;
			deferred->Release();
		}

		ui_log("passes: the depth buffer's clears are watched: on the game's own drawing %s, on its drawing ahead %s", first ? "yes" : "NO", second ? "yes" : "no");
	}

	const char* const DEPTH_VS = "float4 main(uint id : SV_VertexID) : SV_POSITION { return float4((id == 2) ? 3.0 : -1.0, (id == 1) ? 3.0 : -1.0, 0.0, 1.0); }";
	const char* const DEPTH_PS_HEAD = "cbuffer P : register(b0) { float4 a; float4 b; };\n";
	const char* const DEPTH_PS_ONE = "Texture2D<float> t : register(t0);\nfloat take(float2 p) { return t.Load(int3(p, 0)); }\n";
	const char* const DEPTH_PS_MANY = "Texture2DMS<float> t : register(t0);\nfloat take(float2 p) { return t.Load(int2(p), 0); }\n";
	const char* const DEPTH_PS_BODY =
		"float4 main(float4 pos : SV_POSITION) : SV_TARGET {\n"
		"  float d = take(pos.xy);\n"
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
		buffer.ByteWidth = 32;
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
		const float values[8] = {g_depth_near, g_depth_range > 1.0f ? g_depth_range : 1.0f, reverse, g_depth_raw ? 1.0f : 0.0f, g_depth_curve > 0.05f ? g_depth_curve : 0.05f,
		                         g_depth_near_white ? 1.0f : 0.0f, g_depth_far, 0.0f};
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
	const bool world = hook(BSP_CAMERA, &hooked_bsp, &g_original_bsp) && hook(SMODEL_CAMERA, &hooked_smodel, &g_original_smodel);
	const bool models = hook(DOBJ_CAMERA, &hooked_dobj, &g_original_dobj);
	char text[160];
	snprintf(text, sizeof(text), "hook: passes: the map's drawing %s, the models' drawing %s", world ? "held" : "NOT held", models ? "held" : "NOT held");
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
	if (state.playing && (elements != Everything || g_gun_true_depth) && told < 12 && now - told_at > 3000)
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
		snprintf(text, sizeof(text), "passes: showing \"%s\": %d characters' models, %d first-person models (flags %X), %d models held back since last told", ELEMENT_NAMES[elements],
		         characters, views, g_view_flags.load(), g_held_back.exchange(0));
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
	if (fog_step(wish, &shown))
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
		if (told < 8 && now - told_at > 2000)
		{
			++told;
			told_at = now;
			std::string all;
			for (int i = 0; i < g_last_candidate_count; ++i)
			{
				char one[96];
				snprintf(one, sizeof(one), " [%d: format %d, %u samples, cleared %u times to %g]", i + 1, static_cast<int>(g_last_candidates[i].format), g_last_candidates[i].samples,
				         g_last_candidates[i].clears, g_last_candidates[i].cleared_to);
				all += one;
			}

			ui_log("passes: depth buffers of the picture's size (%ux%u) cleared this frame:%s%s", picture.Width, picture.Height, all.empty() ? " none" : all.c_str(),
			       copied ? "; a copy is drawn" : "; NO copy to draw yet");
		}

		if (g_last_candidate_count)
		{
			snprintf(g_depth_status, sizeof(g_depth_status), "%d depth buffer%s found%s.", g_last_candidate_count, g_last_candidate_count == 1 ? "" : "s",
			         copied ? "" : ", but it could not be copied yet");
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
			draw_depth(context, target, picture.Width, picture.Height, many, reversed);
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
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextDisabled("The map and its sky are not drawn. Glow and the film settings still act on the colour: turn them off in Look for a clean one.");
		ImGui::PopTextWrapPos();
	}

	if (depth)
	{
		ui_row_label("Range");
		ImGui::SetNextItemWidth(-FLT_MIN);
		ImGui::SliderFloat("##depthRange", &g_depth_range, 50.0f, 20000.0f, "%.0f units", ImGuiSliderFlags_Logarithmic);
		ImGui::SetItemTooltip("How far away the grey runs out.");
		ui_row_label("Curve");
		ImGui::SetNextItemWidth(-FLT_MIN);
		ImGui::SliderFloat("##depthCurve", &g_depth_curve, 0.1f, 4.0f, "%.2f");
		ui_row_label("Near is white");
		ImGui::Checkbox("##depthWhite", &g_depth_near_white);
		if (ImGui::TreeNode("More about the depth"))
		{
			static const char* const TAKEN[] = {"When the frame is done", "Before its 1st clear", "Before its 2nd clear", "Before its 3rd clear", "Before its 4th clear"};
			static const char* const WAYS[] = {"As the game clears it", "Near is 0", "Near is 1"};
			ui_row_label("Buffer");
			ImGui::SetNextItemWidth(-FLT_MIN);
			int buffer = g_depth_buffer + 1;
			if (ImGui::SliderInt("##depthBuffer", &buffer, 1, CANDIDATE_MOST))
			{
				std::lock_guard<std::mutex> lock(g_depth_lock);
				g_depth_buffer = buffer - 1;
				g_depth_copied = false;
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
			ui_row_label("Near plane");
			ImGui::SetNextItemWidth(-FLT_MIN);
			ImGui::SliderFloat("##depthNear", &g_depth_near, 0.01f, 16.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
			ui_row_label("Far plane");
			ImGui::SetNextItemWidth(-FLT_MIN);
			ImGui::SliderFloat("##depthFar", &g_depth_far, 0.0f, 100000.0f, g_depth_far > 0.0f ? "%.0f" : "none");
			ui_row_label("As stored");
			ImGui::Checkbox("##depthRaw", &g_depth_raw);
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

	ImGui::SetItemTooltip("The game draws the gun pressed flat against the front of its depth, so depth effects (ReShade's depth of field)\nsee it as nearer than anything can be. On: the gun is drawn at the depth it is really at, a little in front\nof the camera, so a focus set very near takes in the gun and nothing else. It can poke through walls you stand against.\nExperimental: look at \"As depth\" to see what it does.");
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
