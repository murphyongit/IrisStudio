// S1iris: bump strength. How strongly the surfaces' normal maps (the fine relief of a floor, a wall, cloth,
// metal) stand out, without touching the game's shaders or its own textures:
//  - Direct3D's "these textures for the pixel shader" (PSSetShaderResources, of the real context) is hooked;
//  - a texture given there that is a two-channel normal map (BC5: x and y of the surface's slope, the shader
//    works z out) gets a copy made of it, each of its sizes drawn again with x and y multiplied by the strength
//    (kept within a slope's bounds), and the copy is given to the shader in its place;
//  - the copies are made at the picture's end (a few each frame, so a map's worth appears over a second or two),
//    and let go of when the game has let go of the original, or the strength is back at the game's own (1).
// What it assumes, and what the log says to check it: that this game's normal maps are BC5 textures
// ("surfaces: textures seen by format ...").
#include "theater.hpp"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <windows.h>

#include <d3d11.h>
#include <d3dcompiler.h>

#include <MinHook.h>

void theater_log(const char* text);

namespace
{
	struct Entry
	{
		bool normal = false, queued = false, is_signed = false, refused = false;
		ID3D11ShaderResourceView* made = nullptr;
		float strength = 0.0f;
		std::uint64_t bytes = 0;
		unsigned seen = 0;
	};

	using set_resources_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
	set_resources_t g_original_set_resources = nullptr;
	std::atomic<float> g_strength{1.0f};
	// (0.79) hooked on the game's own context object (see hudkeep.cpp), whichever thread and context it draws
	// with: the list is under a lock
	constexpr std::uintptr_t GAME_CONTEXT = 0x1409DD3F0;
	std::mutex g_lock;
	unsigned g_calls = 0;
	std::unordered_map<ID3D11ShaderResourceView*, Entry> g_seen;
	std::vector<ID3D11ShaderResourceView*> g_queue;
	std::map<int, int> g_formats;
	bool g_hooked = false, g_failed = false, g_inside = false, g_told = false;
	unsigned g_frame = 0, g_on_since = 0;
	std::uint64_t g_bytes = 0;
	constexpr std::uint64_t MOST_BYTES = 1400ull * 1024 * 1024;
	ID3D11VertexShader* g_vertex = nullptr;
	ID3D11PixelShader* g_pixel = nullptr;
	ID3D11Buffer* g_constants = nullptr;
	ID3D11RasterizerState* g_raster = nullptr;
	ID3D11DepthStencilState* g_no_depth = nullptr;

	void say(const char* format, ...)
	{
		char text[400];
		va_list arguments;
		va_start(arguments, format);
		vsnprintf(text, sizeof(text), format, arguments);
		va_end(arguments);
		theater_log(text);
	}

	template <typename T> void release(T*& one)
	{
		if (one)
		{
			one->Release();
			one = nullptr;
		}
	}

	// What a texture given to the pixel shader is: a normal map of the kind that can be redrawn?
	Entry look_at(ID3D11ShaderResourceView* view)
	{
		Entry entry;
		D3D11_SHADER_RESOURCE_VIEW_DESC desc;
		view->GetDesc(&desc);
		if (g_formats.size() < 64)
		{
			++g_formats[static_cast<int>(desc.Format)];
		}

		if (desc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D && (desc.Format == DXGI_FORMAT_BC5_UNORM || desc.Format == DXGI_FORMAT_BC5_SNORM))
		{
			entry.normal = true;
			entry.is_signed = desc.Format == DXGI_FORMAT_BC5_SNORM;
			// (held on to, so that its address is not another texture's while it is in the list)
			view->AddRef();
		}

		return entry;
	}

	void STDMETHODCALLTYPE hooked_set_resources(ID3D11DeviceContext* self, const UINT start, const UINT count, ID3D11ShaderResourceView* const* views)
	{
		const float strength = g_strength;
		if (g_inside || strength == 1.0f || !views || count == 0 || count > 128)
		{
			g_original_set_resources(self, start, count, views);
			return;
		}

		thread_local ID3D11ShaderResourceView* given[128];
		bool changed = false;
		std::unique_lock<std::mutex> lock(g_lock);
		++g_calls;
		for (UINT i = 0; i < count; ++i)
		{
			ID3D11ShaderResourceView* view = views[i];
			given[i] = view;
			if (!view)
			{
				continue;
			}

			auto found = g_seen.find(view);
			if (found == g_seen.end())
			{
				found = g_seen.emplace(view, look_at(view)).first;
			}

			Entry& entry = found->second;
			if (!entry.normal)
			{
				continue;
			}

			entry.seen = g_frame;
			if (entry.made && entry.strength == strength)
			{
				given[i] = entry.made;
				changed = true;
			}
			else if (!entry.queued && !entry.refused)
			{
				entry.queued = true;
				g_queue.push_back(view);
			}
			else if (entry.made)
			{
				// (the copy made for the strength before, until the new one is there)
				given[i] = entry.made;
				changed = true;
			}
		}

		lock.unlock();
		g_original_set_resources(self, start, count, changed ? given : views);
	}

	const char SHADER[] = R"(
Texture2D source : register(t0);
cbuffer values : register(b0)
{
	float strength;
	float is_signed;
	float2 unused;
};
float4 vertex(uint id : SV_VertexID) : SV_Position
{
	float2 uv = float2((id << 1) & 2, id & 2);
	return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
float4 pixel(float4 position : SV_Position) : SV_Target
{
	float2 slope = source.Load(int3(position.xy, 0)).rg;
	if (is_signed < 0.5)
	{
		slope = slope * 2.0 - 1.0;
	}

	slope *= strength;
	float steep = length(slope);
	if (steep > 0.99)
	{
		slope *= 0.99 / steep;
	}

	return float4(is_signed < 0.5 ? slope * 0.5 + 0.5 : slope, 0.0, 1.0);
}
)";

	bool make_tools(ID3D11Device* device)
	{
		if (g_vertex)
		{
			return true;
		}

		ID3DBlob *code = nullptr, *errors = nullptr;
		if (FAILED(D3DCompile(SHADER, sizeof(SHADER) - 1, "s1iris surfaces", nullptr, nullptr, "vertex", "vs_4_0", 0, 0, &code, &errors)) || !code)
		{
			release(errors);
			return false;
		}

		device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_vertex);
		release(code);
		if (FAILED(D3DCompile(SHADER, sizeof(SHADER) - 1, "s1iris surfaces", nullptr, nullptr, "pixel", "ps_4_0", 0, 0, &code, &errors)) || !code)
		{
			release(errors);
			return false;
		}

		device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_pixel);
		release(code);
		D3D11_BUFFER_DESC buffer = {};
		buffer.ByteWidth = 16;
		buffer.Usage = D3D11_USAGE_DEFAULT;
		buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		device->CreateBuffer(&buffer, nullptr, &g_constants);
		D3D11_RASTERIZER_DESC raster = {};
		raster.FillMode = D3D11_FILL_SOLID;
		raster.CullMode = D3D11_CULL_NONE;
		raster.DepthClipEnable = TRUE;
		device->CreateRasterizerState(&raster, &g_raster);
		D3D11_DEPTH_STENCIL_DESC depth = {};
		depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
		device->CreateDepthStencilState(&depth, &g_no_depth);
		return g_vertex && g_pixel && g_constants && g_raster && g_no_depth;
	}

	// A normal map drawn again, each of its sizes, with its slopes multiplied. (The state it changes is the caller's
	// to keep.) Null: it could not be.
	ID3D11ShaderResourceView* redraw(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11ShaderResourceView* view, const Entry& entry,
	                                 const float strength, std::uint64_t* bytes)
	{
		D3D11_SHADER_RESOURCE_VIEW_DESC view_desc;
		view->GetDesc(&view_desc);
		ID3D11Resource* resource = nullptr;
		view->GetResource(&resource);
		if (!resource)
		{
			return nullptr;
		}

		D3D11_TEXTURE2D_DESC from;
		static_cast<ID3D11Texture2D*>(resource)->GetDesc(&from);
		resource->Release();
		const UINT first = view_desc.Texture2D.MostDetailedMip;
		const UINT levels = view_desc.Texture2D.MipLevels == static_cast<UINT>(-1) ? from.MipLevels - first : view_desc.Texture2D.MipLevels;
		const UINT width = (std::max)(1u, from.Width >> first), height = (std::max)(1u, from.Height >> first);
		if (from.ArraySize != 1 || from.SampleDesc.Count != 1 || levels == 0 || levels > 14 || width > 4096 || height > 4096)
		{
			return nullptr;
		}

		D3D11_TEXTURE2D_DESC to = {};
		to.Width = width;
		to.Height = height;
		to.MipLevels = levels;
		to.ArraySize = 1;
		to.Format = entry.is_signed ? DXGI_FORMAT_R8G8_SNORM : DXGI_FORMAT_R8G8_UNORM;
		to.SampleDesc.Count = 1;
		to.Usage = D3D11_USAGE_DEFAULT;
		to.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		ID3D11Texture2D* texture = nullptr;
		if (FAILED(device->CreateTexture2D(&to, nullptr, &texture)) || !texture)
		{
			return nullptr;
		}

		const float values[4] = {strength, entry.is_signed ? 1.0f : 0.0f, 0.0f, 0.0f};
		context->UpdateSubresource(g_constants, 0, nullptr, values, 0, 0);
		context->VSSetShader(g_vertex, nullptr, 0);
		context->PSSetShader(g_pixel, nullptr, 0);
		context->GSSetShader(nullptr, nullptr, 0);
		context->HSSetShader(nullptr, nullptr, 0);
		context->DSSetShader(nullptr, nullptr, 0);
		context->PSSetConstantBuffers(0, 1, &g_constants);
		context->RSSetState(g_raster);
		const FLOAT factor[4] = {};
		context->OMSetBlendState(nullptr, factor, 0xFFFFFFFF);
		context->OMSetDepthStencilState(g_no_depth, 0);
		context->IASetInputLayout(nullptr);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		bool drawn = true;
		for (UINT level = 0; level < levels && drawn; ++level)
		{
			// (one size of the original at a time, as the shader's only one)
			D3D11_SHADER_RESOURCE_VIEW_DESC one = view_desc;
			one.Texture2D.MostDetailedMip = first + level;
			one.Texture2D.MipLevels = 1;
			ID3D11ShaderResourceView* source = nullptr;
			D3D11_RENDER_TARGET_VIEW_DESC target_desc = {};
			target_desc.Format = to.Format;
			target_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
			target_desc.Texture2D.MipSlice = level;
			ID3D11RenderTargetView* target = nullptr;
			ID3D11Resource* original = nullptr;
			view->GetResource(&original);
			if (!original || FAILED(device->CreateShaderResourceView(original, &one, &source)) || FAILED(device->CreateRenderTargetView(texture, &target_desc, &target)))
			{
				drawn = false;
			}
			else
			{
				const D3D11_VIEWPORT viewport = {0.0f, 0.0f, static_cast<float>((std::max)(1u, width >> level)), static_cast<float>((std::max)(1u, height >> level)), 0.0f,
				                                 1.0f};
				context->OMSetRenderTargets(1, &target, nullptr);
				context->RSSetViewports(1, &viewport);
				context->PSSetShaderResources(0, 1, &source);
				context->Draw(3, 0);
				ID3D11ShaderResourceView* none = nullptr;
				context->PSSetShaderResources(0, 1, &none);
			}

			release(original);
			release(source);
			release(target);
		}

		ID3D11RenderTargetView* no_target = nullptr;
		context->OMSetRenderTargets(1, &no_target, nullptr);
		ID3D11ShaderResourceView* made = nullptr;
		if (drawn)
		{
			device->CreateShaderResourceView(texture, nullptr, &made);
		}

		texture->Release();
		*bytes = static_cast<std::uint64_t>(width) * height * 2 * 4 / 3;
		return made;
	}

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
		ID3D11ShaderResourceView* resource = nullptr;
		ID3D11Buffer* constants = nullptr;
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
			c->PSGetShaderResources(0, 1, &resource);
			c->PSGetConstantBuffers(0, 1, &constants);
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
			c->PSSetShaderResources(0, 1, &resource);
			c->PSSetConstantBuffers(0, 1, &constants);
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
			release(resource);
			release(constants);
			release(layout);
		}
	};

	void forget_all()
	{
		for (auto& one : g_seen)
		{
			if (one.second.normal)
			{
				release(one.second.made);
				one.first->Release();
			}
		}

		g_seen.clear();
		g_queue.clear();
		g_bytes = 0;
	}
}

// How many normal maps have been found and how many drawn again, for the editor to show.
void surfaces_status(int* found, int* made, int* megabytes)
{
	std::lock_guard<std::mutex> lock(g_lock);
	*found = *made = 0;
	for (const auto& one : g_seen)
	{
		*found += one.second.normal;
		*made += one.second.made != nullptr;
	}

	*megabytes = static_cast<int>(g_bytes / (1024 * 1024));
}

// (the picture's end, each frame) strength: what the Look tab asks for (1: the game's own, nothing is done).
void surfaces_frame(ID3D11Device* device, ID3D11DeviceContext* context, const float strength)
{
	++g_frame;
	if (g_failed)
	{
		return;
	}

	if (strength == 1.0f)
	{
		g_strength = 1.0f;
		g_told = false;
		g_on_since = 0;
		std::lock_guard<std::mutex> lock(g_lock);
		if (!g_seen.empty())
		{
			forget_all();
		}

		return;
	}

	if (!g_hooked)
	{
		// (the object the game itself draws with: ReShade's, in front of the real one, when ReShade is there)
		ID3D11DeviceContext* games_context = nullptr;
		if (!IsBadReadPtr(reinterpret_cast<const void*>(GAME_CONTEXT), sizeof(void*)))
		{
			games_context = *reinterpret_cast<ID3D11DeviceContext**>(GAME_CONTEXT);
		}

		if (!games_context || IsBadReadPtr(games_context, sizeof(void*)))
		{
			return;
		}

		void** table = *reinterpret_cast<void***>(games_context);
		if (!make_tools(device) || MH_CreateHook(table[8], reinterpret_cast<LPVOID>(&hooked_set_resources), reinterpret_cast<LPVOID*>(&g_original_set_resources)) != MH_OK ||
		    MH_EnableHook(table[8]) != MH_OK)
		{
			g_failed = true;
			say("surfaces: bump strength could not be set up");
			return;
		}

		g_hooked = true;
		say("surfaces: Direct3D's textures-for-the-shader call is hooked");
	}

	g_strength = strength;
	if (g_on_since == 0)
	{
		g_on_since = g_frame;
	}

	// a few of those waiting are drawn again
	std::unique_lock<std::mutex> lock(g_lock);
	if (!g_queue.empty())
	{
		g_inside = true;
		Kept kept;
		kept.keep(context);
		int done = 0;
		while (!g_queue.empty() && done < 10)
		{
			ID3D11ShaderResourceView* view = g_queue.back();
			g_queue.pop_back();
			const auto found = g_seen.find(view);
			if (found == g_seen.end() || !found->second.normal)
			{
				continue;
			}

			Entry& entry = found->second;
			entry.queued = false;
			if (entry.made)
			{
				g_bytes -= entry.bytes;
				release(entry.made);
			}

			if (g_bytes > MOST_BYTES)
			{
				entry.refused = true;
				continue;
			}

			std::uint64_t bytes = 0;
			entry.made = redraw(device, context, view, entry, strength, &bytes);
			entry.strength = strength;
			entry.bytes = entry.made ? bytes : 0;
			entry.refused = entry.made == nullptr;
			g_bytes += entry.bytes;
			++done;
		}

		kept.put_back(context);
		g_inside = false;
	}

	// every five seconds or so: what the game has let go of is let go of here, and the rest is looked at afresh
	if (g_frame % 300 == 0)
	{
		for (auto one = g_seen.begin(); one != g_seen.end();)
		{
			bool gone = !one->second.normal;
			if (one->second.normal && !one->second.queued && g_frame - one->second.seen > 240)
			{
				const ULONG held = one->first->AddRef() - 1;
				one->first->Release();
				if (held <= 1)
				{
					g_bytes -= one->second.bytes;
					release(one->second.made);
					one->first->Release();
					gone = true;
				}
			}

			one = gone ? g_seen.erase(one) : std::next(one);
		}
	}

	if (!g_told && g_frame - g_on_since > 400)
	{
		g_told = true;
		char formats[300] = "";
		for (const auto& format : g_formats)
		{
			char one[32];
			snprintf(one, sizeof(one), " %d:%d", format.first, format.second);
			strncat_s(formats, one, _TRUNCATE);
		}

		int found = 0, made = 0;
		for (const auto& one : g_seen)
		{
			found += one.second.normal;
			made += one.second.made != nullptr;
		}

		say("surfaces: %u calls seen; textures by format (DXGI number:how many; 83 and 84 are BC5):%s; normal maps found %d, drawn again %d (%d MB)", g_calls, formats,
		    found, made, static_cast<int>(g_bytes / (1024 * 1024)));
	}
}
