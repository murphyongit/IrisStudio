// S1iris: the HUD kept out of ReShade's effects.
// ReShade works on the finished picture as the game hands it over (in its own Present, which wraps the real
// one): a blur or a depth of field there takes the kill feed and the score popups with it. Here:
//  1. as the game draws, the moment its HUD begins is caught: the first draw with blending on the back
//     buffer (the scene is put there by plain, unblended draws; the HUD is blended over it). The back buffer
//     is copied there: the scene alone (A).
//  2. when the game hands the picture over (ReShade's Present, hooked: the game's swap chain is ReShade's
//     object, at 0x1409E42F0), the finished picture is copied (B) and the scene alone is put in its place: so
//     ReShade's effects are made on the scene alone.
//  3. in the real Present (ui.cpp's hook, which is after ReShade), the HUD is put back over ReShade's result
//     (R): where B differs from A the HUD drew. A faint difference is added on (R + B - A: a glow), a strong
//     one replaces the pixel with the game's own (B: the letters), with a smooth change between.
// Without ReShade (the game's swap chain is the real one) nothing is done.
#include "theater.hpp"

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <windows.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>

#include <MinHook.h>

void theater_log(const char* text);

namespace
{
	constexpr std::uintptr_t GAME_SWAP_CHAIN = 0x1409E42F0;

	std::atomic<bool> g_on{true};
	char g_status[160] = "waiting for the game to draw";
	ID3D11DeviceContext* g_context = nullptr;   // the real one (the hooks act on it alone)
	ID3D11Texture2D* g_back = nullptr;          // the real back buffer (not held)
	ID3D11Texture2D *g_scene = nullptr, *g_final = nullptr, *g_made = nullptr;
	ID3D11ShaderResourceView *g_scene_view = nullptr, *g_final_view = nullptr, *g_made_view = nullptr;
	ID3D11RenderTargetView* g_target = nullptr;
	ID3D11VertexShader* g_vertex = nullptr;
	ID3D11PixelShader* g_pixel = nullptr;
	ID3D11RasterizerState* g_raster = nullptr;
	ID3D11DepthStencilState* g_no_depth = nullptr;
	UINT g_width = 0, g_height = 0;
	bool g_hooked = false, g_wrapper_hooked = false, g_no_wrapper = false, g_failed = false;
	bool g_on_back = false, g_captured = false, g_swapped = false, g_inside = false;
	unsigned g_draws_before = 0, g_draws_after = 0, g_frames = 0;

	void say(const char* format, ...)
	{
		char text[300];
		va_list arguments;
		va_start(arguments, format);
		vsnprintf(text, sizeof(text), format, arguments);
		va_end(arguments);
		theater_log(text);
	}

	using draw_indexed_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
	using draw_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
	using draw_indexed_instanced_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
	using draw_instanced_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
	using set_targets_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
	using set_targets_views_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*, UINT, UINT,
	                                                     ID3D11UnorderedAccessView* const*, const UINT*);
	using present_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
	draw_indexed_t g_original_draw_indexed = nullptr;
	draw_t g_original_draw = nullptr;
	draw_indexed_instanced_t g_original_draw_indexed_instanced = nullptr;
	draw_instanced_t g_original_draw_instanced = nullptr;
	set_targets_t g_original_set_targets = nullptr;
	set_targets_views_t g_original_set_targets_views = nullptr;
	present_t g_original_wrapper_present = nullptr;

	// (the game's thread that draws) A draw is about to be made.
	void before_draw(ID3D11DeviceContext* self)
	{
		if (!g_on_back || self != g_context || g_inside || !g_scene || !g_on)
		{
			return;
		}

		if (g_captured)
		{
			++g_draws_after;
			return;
		}

		ID3D11BlendState* state = nullptr;
		FLOAT factor[4];
		UINT mask = 0;
		self->OMGetBlendState(&state, factor, &mask);
		bool blended = false;
		if (state)
		{
			D3D11_BLEND_DESC desc;
			state->GetDesc(&desc);
			blended = desc.RenderTarget[0].BlendEnable != FALSE;
			state->Release();
		}

		if (!blended)
		{
			++g_draws_before;
			return;
		}

		// the HUD begins: the scene alone, as it is now
		self->CopyResource(g_scene, g_back);
		g_captured = true;
	}

	void note_targets(ID3D11DeviceContext* self, const UINT count, ID3D11RenderTargetView* const* views)
	{
		if (self != g_context || g_inside)
		{
			return;
		}

		g_on_back = false;
		if (count > 0 && views && views[0] && g_back)
		{
			ID3D11Resource* resource = nullptr;
			views[0]->GetResource(&resource);
			g_on_back = resource == g_back;
			if (resource)
			{
				resource->Release();
			}
		}
	}

	void STDMETHODCALLTYPE hooked_draw_indexed(ID3D11DeviceContext* self, const UINT count, const UINT start, const INT base)
	{
		before_draw(self);
		g_original_draw_indexed(self, count, start, base);
	}

	void STDMETHODCALLTYPE hooked_draw(ID3D11DeviceContext* self, const UINT count, const UINT start)
	{
		before_draw(self);
		g_original_draw(self, count, start);
	}

	void STDMETHODCALLTYPE hooked_draw_indexed_instanced(ID3D11DeviceContext* self, const UINT count, const UINT instances, const UINT start, const INT base,
	                                                     const UINT first)
	{
		before_draw(self);
		g_original_draw_indexed_instanced(self, count, instances, start, base, first);
	}

	void STDMETHODCALLTYPE hooked_draw_instanced(ID3D11DeviceContext* self, const UINT count, const UINT instances, const UINT start, const UINT first)
	{
		before_draw(self);
		g_original_draw_instanced(self, count, instances, start, first);
	}

	void STDMETHODCALLTYPE hooked_set_targets(ID3D11DeviceContext* self, const UINT count, ID3D11RenderTargetView* const* views, ID3D11DepthStencilView* depth)
	{
		g_original_set_targets(self, count, views, depth);
		note_targets(self, count, views);
	}

	void STDMETHODCALLTYPE hooked_set_targets_views(ID3D11DeviceContext* self, const UINT count, ID3D11RenderTargetView* const* views,
	                                                ID3D11DepthStencilView* depth, const UINT first, const UINT view_count,
	                                                ID3D11UnorderedAccessView* const* access, const UINT* initial)
	{
		g_original_set_targets_views(self, count, views, depth, first, view_count, access, initial);
		// (count 0xFFFFFFFF: the targets are kept as they are)
		if (count != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL)
		{
			note_targets(self, count, views);
		}
	}

	// The game hands its picture to ReShade: the scene alone is put there for ReShade to work on.
	HRESULT STDMETHODCALLTYPE hooked_wrapper_present(IDXGISwapChain* self, const UINT sync, const UINT flags)
	{
		if (g_captured && !g_swapped && g_on && g_context && g_back && g_final && !(flags & DXGI_PRESENT_TEST))
		{
			g_inside = true;
			g_context->CopyResource(g_final, g_back);
			g_context->CopyResource(g_back, g_scene);
			g_inside = false;
			g_swapped = true;
		}

		return g_original_wrapper_present(self, sync, flags);
	}

	template <typename T> void release(T*& one)
	{
		if (one)
		{
			one->Release();
			one = nullptr;
		}
	}

	bool hook_at(void** table, const int index, void* mine, void* original)
	{
		return MH_CreateHook(table[index], mine, static_cast<LPVOID*>(original)) == MH_OK && MH_EnableHook(table[index]) == MH_OK;
	}

	const char SHADER[] = R"(
Texture2D made : register(t0);
Texture2D scene : register(t1);
Texture2D final : register(t2);
float4 vertex(uint id : SV_VertexID) : SV_Position
{
	float2 uv = float2((id << 1) & 2, id & 2);
	return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
float4 pixel(float4 position : SV_Position) : SV_Target
{
	int3 at = int3(position.xy, 0);
	float3 r = made.Load(at).rgb, a = scene.Load(at).rgb, b = final.Load(at).rgb;
	float3 over = b - a;
	float much = saturate(max(abs(over.r), max(abs(over.g), abs(over.b))) * 5.0);
	return float4(lerp(r + over, b, much), 1.0);
}
)";

	bool make(ID3D11Device* device, ID3D11Texture2D* back)
	{
		D3D11_TEXTURE2D_DESC desc;
		back->GetDesc(&desc);
		if (desc.SampleDesc.Count != 1)
		{
			snprintf(g_status, sizeof(g_status), "not possible: the game's picture is multisampled");
			return false;
		}

		g_width = desc.Width;
		g_height = desc.Height;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		desc.CPUAccessFlags = 0;
		desc.MiscFlags = 0;
		ID3D11Texture2D** textures[] = {&g_scene, &g_final, &g_made};
		ID3D11ShaderResourceView** views[] = {&g_scene_view, &g_final_view, &g_made_view};
		for (int i = 0; i < 3; ++i)
		{
			if (FAILED(device->CreateTexture2D(&desc, nullptr, textures[i])) || FAILED(device->CreateShaderResourceView(*textures[i], nullptr, views[i])))
			{
				snprintf(g_status, sizeof(g_status), "not possible: no copies of the picture could be made (format %d)", static_cast<int>(desc.Format));
				return false;
			}
		}

		if (FAILED(device->CreateRenderTargetView(back, nullptr, &g_target)))
		{
			snprintf(g_status, sizeof(g_status), "not possible: the back buffer cannot be drawn to");
			return false;
		}

		if (!g_vertex)
		{
			ID3DBlob *code = nullptr, *errors = nullptr;
			if (FAILED(D3DCompile(SHADER, sizeof(SHADER) - 1, "s1iris hud", nullptr, nullptr, "vertex", "vs_4_0", 0, 0, &code, &errors)) || !code)
			{
				snprintf(g_status, sizeof(g_status), "not possible: its shader did not compile");
				release(errors);
				return false;
			}

			device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_vertex);
			release(code);
			if (FAILED(D3DCompile(SHADER, sizeof(SHADER) - 1, "s1iris hud", nullptr, nullptr, "pixel", "ps_4_0", 0, 0, &code, &errors)) || !code)
			{
				snprintf(g_status, sizeof(g_status), "not possible: its shader did not compile");
				release(errors);
				return false;
			}

			device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_pixel);
			release(code);
			D3D11_RASTERIZER_DESC raster = {};
			raster.FillMode = D3D11_FILL_SOLID;
			raster.CullMode = D3D11_CULL_NONE;
			raster.DepthClipEnable = TRUE;
			device->CreateRasterizerState(&raster, &g_raster);
			D3D11_DEPTH_STENCIL_DESC depth = {};
			depth.DepthEnable = FALSE;
			depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
			depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
			device->CreateDepthStencilState(&depth, &g_no_depth);
		}

		return g_vertex && g_pixel && g_raster && g_no_depth;
	}

	// What of Direct3D's state the drawing changes, kept and put back: the game takes its own for granted.
	struct Kept
	{
		ID3D11RenderTargetView* targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
		ID3D11DepthStencilView* depth = nullptr;
		UINT viewport_count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
		D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
		ID3D11RasterizerState* raster = nullptr;
		ID3D11BlendState* blend = nullptr;
		FLOAT blend_factor[4] = {};
		UINT sample_mask = 0;
		ID3D11DepthStencilState* depth_state = nullptr;
		UINT stencil = 0;
		ID3D11VertexShader* vertex = nullptr;
		ID3D11PixelShader* pixel = nullptr;
		ID3D11GeometryShader* geometry = nullptr;
		ID3D11HullShader* hull = nullptr;
		ID3D11DomainShader* domain = nullptr;
		ID3D11ShaderResourceView* resources[3] = {};
		ID3D11InputLayout* layout = nullptr;
		D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;

		void keep(ID3D11DeviceContext* c)
		{
			c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets, &depth);
			c->RSGetViewports(&viewport_count, viewports);
			c->RSGetState(&raster);
			c->OMGetBlendState(&blend, blend_factor, &sample_mask);
			c->OMGetDepthStencilState(&depth_state, &stencil);
			c->VSGetShader(&vertex, nullptr, nullptr);
			c->PSGetShader(&pixel, nullptr, nullptr);
			c->GSGetShader(&geometry, nullptr, nullptr);
			c->HSGetShader(&hull, nullptr, nullptr);
			c->DSGetShader(&domain, nullptr, nullptr);
			c->PSGetShaderResources(0, 3, resources);
			c->IAGetInputLayout(&layout);
			c->IAGetPrimitiveTopology(&topology);
		}

		void put_back(ID3D11DeviceContext* c)
		{
			c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets, depth);
			c->RSSetViewports(viewport_count, viewports);
			c->RSSetState(raster);
			c->OMSetBlendState(blend, blend_factor, sample_mask);
			c->OMSetDepthStencilState(depth_state, stencil);
			c->VSSetShader(vertex, nullptr, 0);
			c->PSSetShader(pixel, nullptr, 0);
			c->GSSetShader(geometry, nullptr, 0);
			c->HSSetShader(hull, nullptr, 0);
			c->DSSetShader(domain, nullptr, 0);
			c->PSSetShaderResources(0, 3, resources);
			c->IASetInputLayout(layout);
			c->IASetPrimitiveTopology(topology);
			for (ID3D11RenderTargetView*& target : targets)
			{
				release(target);
			}

			release(depth);
			release(raster);
			release(blend);
			release(depth_state);
			release(vertex);
			release(pixel);
			release(geometry);
			release(hull);
			release(domain);
			for (ID3D11ShaderResourceView*& resource : resources)
			{
				release(resource);
			}

			release(layout);
		}
	};
}

bool hud_keep_get() { return g_on; }
void hud_keep_set(const bool on) { g_on = on; }
const char* hud_keep_status() { return g_status; }

// (a change of the picture's size: the copies are made anew)
void hud_keep_release()
{
	release(g_scene_view);
	release(g_final_view);
	release(g_made_view);
	release(g_scene);
	release(g_final);
	release(g_made);
	release(g_target);
	g_back = nullptr;
	g_captured = g_swapped = g_on_back = false;
}

// (the real Present, after ReShade) The HUD put back over ReShade's picture; and the hooks set up the first time.
void hud_keep_frame(IDXGISwapChain* real, ID3D11Device* device, ID3D11DeviceContext* context)
{
	if (g_failed || g_no_wrapper)
	{
		return;
	}

	if (!g_wrapper_hooked)
	{
		// the game's own swap chain: ReShade's object if ReShade is there, else the real one (nothing to do then)
		IDXGISwapChain* games = nullptr;
		if (!IsBadReadPtr(reinterpret_cast<const void*>(GAME_SWAP_CHAIN), sizeof(void*)))
		{
			games = *reinterpret_cast<IDXGISwapChain**>(GAME_SWAP_CHAIN);
		}

		if (!games)
		{
			return;
		}

		if (games == real)
		{
			g_no_wrapper = true;
			snprintf(g_status, sizeof(g_status), "not needed: ReShade is not in use");
			say("hud keep: the game's swap chain is the real one (no ReShade): nothing to do");
			return;
		}

		void** table = *reinterpret_cast<void***>(games);
		if (!hook_at(table, 8, reinterpret_cast<void*>(&hooked_wrapper_present), &g_original_wrapper_present))
		{
			g_failed = true;
			snprintf(g_status, sizeof(g_status), "not possible: ReShade's Present could not be hooked");
			say("hud keep: ReShade's Present could not be hooked");
			return;
		}

		g_wrapper_hooked = true;
		say("hud keep: the game's swap chain %p is not the real one %p (ReShade): its Present is hooked", static_cast<void*>(games), static_cast<void*>(real));
	}

	if (!g_hooked)
	{
		g_context = context;
		void** table = *reinterpret_cast<void***>(context);
		const bool hooked = hook_at(table, 12, reinterpret_cast<void*>(&hooked_draw_indexed), &g_original_draw_indexed) &&
		                    hook_at(table, 13, reinterpret_cast<void*>(&hooked_draw), &g_original_draw) &&
		                    hook_at(table, 20, reinterpret_cast<void*>(&hooked_draw_indexed_instanced), &g_original_draw_indexed_instanced) &&
		                    hook_at(table, 21, reinterpret_cast<void*>(&hooked_draw_instanced), &g_original_draw_instanced) &&
		                    hook_at(table, 33, reinterpret_cast<void*>(&hooked_set_targets), &g_original_set_targets) &&
		                    hook_at(table, 34, reinterpret_cast<void*>(&hooked_set_targets_views), &g_original_set_targets_views);
		if (!hooked)
		{
			g_failed = true;
			snprintf(g_status, sizeof(g_status), "not possible: Direct3D's drawing could not be hooked");
			say("hud keep: Direct3D's drawing could not be hooked");
			return;
		}

		g_hooked = true;
		say("hud keep: Direct3D's drawing is hooked");
	}

	ID3D11Texture2D* back = nullptr;
	if (FAILED(real->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))) || !back)
	{
		return;
	}

	D3D11_TEXTURE2D_DESC desc;
	back->GetDesc(&desc);
	if (back != g_back || desc.Width != g_width || desc.Height != g_height || !g_scene)
	{
		hud_keep_release();
		if (!make(device, back))
		{
			g_failed = true;
			say("hud keep: %s", g_status);
			back->Release();
			return;
		}

		g_back = back;
	}

	back->Release();
	if (g_swapped && g_on)
	{
		// R: what ReShade made of the scene. The HUD over it.
		g_inside = true;
		Kept kept;
		kept.keep(context);
		context->CopyResource(g_made, g_back);
		D3D11_VIEWPORT viewport = {0.0f, 0.0f, static_cast<float>(g_width), static_cast<float>(g_height), 0.0f, 1.0f};
		ID3D11ShaderResourceView* views[3] = {g_made_view, g_scene_view, g_final_view};
		const FLOAT factor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
		context->OMSetRenderTargets(1, &g_target, nullptr);
		context->RSSetViewports(1, &viewport);
		context->RSSetState(g_raster);
		context->OMSetBlendState(nullptr, factor, 0xFFFFFFFF);
		context->OMSetDepthStencilState(g_no_depth, 0);
		context->VSSetShader(g_vertex, nullptr, 0);
		context->PSSetShader(g_pixel, nullptr, 0);
		context->GSSetShader(nullptr, nullptr, 0);
		context->HSSetShader(nullptr, nullptr, 0);
		context->DSSetShader(nullptr, nullptr, 0);
		context->PSSetShaderResources(0, 3, views);
		context->IASetInputLayout(nullptr);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		context->Draw(3, 0);
		kept.put_back(context);
		g_inside = false;
		++g_frames;
		if (g_frames == 1 || g_frames == 600)
		{
			say("hud keep: frame %u: the scene was taken after %u plain draws on the back buffer, %u draws came after it (the HUD)", g_frames, g_draws_before,
			    g_draws_after);
		}

		snprintf(g_status, sizeof(g_status), "working (%u draws of the HUD in the last picture)", g_draws_after + 1);
	}
	else if (g_on && !g_captured)
	{
		snprintf(g_status, sizeof(g_status), "ready (no HUD is being drawn right now)");
	}

	g_captured = g_swapped = false;
	g_draws_before = g_draws_after = 0;
}
