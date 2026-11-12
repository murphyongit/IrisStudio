// The sky: its hue, saturation and brightness, and turning it, as IrisStudio's Sky section has them.
//
// Advanced Warfare has no settings for this (the candidates 0.30 listed did nothing). The sky is a picture:
// a cube of six faces the map names as its sky image (the map's GfxWorld has a count of skies at +0x20 and
// the skies at +0x28, each with its image at +0x10; an image begins with its Direct3D texture and, at +8
// and +0x10, the two views of it shaders read; H1MVM's database.hpp and s1-mod's structs.hpp, the engine
// family's). So the picture itself is changed: the map's cube is drawn, face by face, through a shader that
// turns each direction and shifts each colour, into a cube of our own, and the image is pointed at ours
// while playback has the sky changed. The game's own cube is never written to and is put back afterwards
// (and before the map is let go of: the game releases what its images point at).
#include "theater.hpp"

#include <d3d11.h>
#include <d3dcompiler.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

void ui_log(const char* format, ...);

namespace
{
	constexpr size_t IMAGE_VIEW = 8, IMAGE_VIEW_ALTERNATE = 0x10;
	constexpr int MAX_SKIES = 4;
	constexpr float PI = 3.14159265358979f;

	const char SHADER[] = R"(
cbuffer Parameters : register(b0) { float4 change; float4 row0; float4 row1; float4 row2; };
TextureCube source : register(t0);
SamplerState smooth : register(s0);
struct Vertex { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Vertex vertex(uint id : SV_VertexID)
{
	Vertex o;
	o.uv = float2((id << 1) & 2, id & 2);
	o.position = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
	return o;
}
float3 to_hsv(float3 c)
{
	float4 K = float4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
	float4 p = c.g < c.b ? float4(c.bg, K.wz) : float4(c.gb, K.xy);
	float4 q = c.r < p.x ? float4(p.xyw, c.r) : float4(c.r, p.yzx);
	float d = q.x - min(q.w, q.y);
	return float3(abs(q.z + (q.w - q.y) / (6.0 * d + 1e-10)), d / (q.x + 1e-10), q.x);
}
float3 to_rgb(float3 c)
{
	float3 p = abs(frac(c.xxx + float3(1.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0);
	return c.z * lerp(float3(1, 1, 1), saturate(p - 1.0), c.y);
}
float4 pixel(Vertex i) : SV_Target
{
	float2 p = i.uv * 2 - 1;
	int face = (int)change.w;
	float3 d;
	if (face == 0) d = float3(1, -p.y, -p.x);
	else if (face == 1) d = float3(-1, -p.y, p.x);
	else if (face == 2) d = float3(p.x, 1, p.y);
	else if (face == 3) d = float3(p.x, -1, -p.y);
	else if (face == 4) d = float3(p.x, -p.y, 1);
	else d = float3(-p.x, -p.y, -1);
	float3 turned = float3(dot(d, row0.xyz), dot(d, row1.xyz), dot(d, row2.xyz));
	float4 c = source.SampleLevel(smooth, turned, 0);
	float3 h = to_hsv(max(c.rgb, 0));
	h.x = frac(h.x + change.x);
	h.y = saturate(h.y * change.y);
	h.z *= change.z;
	return float4(to_rgb(h), c.a);
}
)";

	struct Sky
	{
		unsigned char* image = nullptr;
		// the game's two views (the second may be the first again, or none), and ours for each
		ID3D11ShaderResourceView* original[2] = {};
		ID3D11Texture2D* texture[2] = {};
		ID3D11ShaderResourceView* made[2] = {};
		ID3D11RenderTargetView* faces[2][6] = {};
		bool swapped = false;
	};

	std::mutex g_lock;
	Sky g_skies[MAX_SKIES];
	int g_sky_count = 0;
	bool g_looked = false, g_failed = false, g_was_playing = false;
	char g_world[96] = {};
	// what the editor wants: hue in degrees, saturation and brightness as factors, three turns in degrees
	float g_hsv[3] = {0.0f, 1.0f, 1.0f}, g_turn[3] = {};
	bool g_dirty = false;
	ID3D11VertexShader* g_vertex = nullptr;
	ID3D11PixelShader* g_pixel = nullptr;
	ID3D11Buffer* g_parameters = nullptr;
	ID3D11SamplerState* g_sampler = nullptr;
	ID3D11RasterizerState* g_raster = nullptr;

	// Another map's sky. A map's sky picture is only in that map's own files, so it is kept while that map
	// is played: its cube drawn into one of ours (at most SAVED_SIZE across) and written to
	// theater\skies\<map>.s1sky (a header, then the six faces in 16-bit floats). On any other map that
	// file is the picture the sky is drawn from, in place of the map's own.
	constexpr UINT SAVED_SIZE = 1024;
	struct SavedHeader
	{
		char magic[8];
		std::uint32_t size, format;
	};
	bool g_save_request = false;
	char g_source_name[64] = "", g_source_loaded[64] = "", g_message[160] = "";
	ID3D11Texture2D* g_source_texture = nullptr;
	ID3D11ShaderResourceView* g_source_view = nullptr;

	bool changed_now()
	{
		return g_source_name[0] || g_hsv[0] != 0.0f || g_hsv[1] != 1.0f || g_hsv[2] != 1.0f || g_turn[0] != 0.0f || g_turn[1] != 0.0f ||
		       g_turn[2] != 0.0f;
	}

	std::string skies_folder()
	{
		const std::string folder = std::string(theater_folder()) + "\\skies";
		CreateDirectoryA(folder.c_str(), nullptr);
		return folder;
	}

	template <typename T> void release(T*& object)
	{
		if (object)
		{
			object->Release();
			object = nullptr;
		}
	}

	// A view the game's image has: what it is a view of.
	bool describe(ID3D11ShaderResourceView* view, D3D11_SHADER_RESOURCE_VIEW_DESC* desc)
	{
		__try
		{
			if (IsBadReadPtr(view, sizeof(void*)) || IsBadReadPtr(*reinterpret_cast<void**>(view), sizeof(void*) * 9))
			{
				return false;
			}

			view->GetDesc(desc);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	bool read_views(const unsigned char* image, ID3D11ShaderResourceView** first, ID3D11ShaderResourceView** second)
	{
		__try
		{
			if (IsBadReadPtr(image, 0x68))
			{
				return false;
			}

			*first = *reinterpret_cast<ID3D11ShaderResourceView* const*>(image + IMAGE_VIEW);
			*second = *reinterpret_cast<ID3D11ShaderResourceView* const*>(image + IMAGE_VIEW_ALTERNATE);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	void write_views(unsigned char* image, ID3D11ShaderResourceView* first, ID3D11ShaderResourceView* second)
	{
		__try
		{
			*reinterpret_cast<ID3D11ShaderResourceView**>(image + IMAGE_VIEW) = first;
			if (second)
			{
				*reinterpret_cast<ID3D11ShaderResourceView**>(image + IMAGE_VIEW_ALTERNATE) = second;
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
	}

	// The game's own cube again in every image of ours that still points at ours.
	void put_back()
	{
		for (int i = 0; i < g_sky_count; ++i)
		{
			Sky& sky = g_skies[i];
			if (!sky.swapped)
			{
				continue;
			}

			sky.swapped = false;
			ID3D11ShaderResourceView *first = nullptr, *second = nullptr;
			if (read_views(sky.image, &first, &second) && first == sky.made[0])
			{
				write_views(sky.image, sky.original[0], sky.original[1]);
			}
		}
	}

	void forget()
	{
		put_back();
		for (Sky& sky : g_skies)
		{
			for (int v = 0; v < 2; ++v)
			{
				for (ID3D11RenderTargetView*& face : sky.faces[v])
				{
					release(face);
				}

				release(sky.made[v]);
				release(sky.texture[v]);
			}

			sky = Sky();
		}

		g_sky_count = 0;
		g_looked = false;
		g_failed = false;
		g_world[0] = 0;
	}

	bool shaders(ID3D11Device* device)
	{
		if (g_vertex && g_pixel)
		{
			return true;
		}

		ID3DBlob *code = nullptr, *errors = nullptr;
		if (FAILED(D3DCompile(SHADER, sizeof(SHADER) - 1, "s1iris sky", nullptr, nullptr, "vertex", "vs_4_0", 0, 0, &code, &errors)))
		{
			ui_log("sky: the shader did not compile: %s", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
			release(errors);
			return false;
		}

		device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_vertex);
		release(code);
		release(errors);
		if (FAILED(D3DCompile(SHADER, sizeof(SHADER) - 1, "s1iris sky", nullptr, nullptr, "pixel", "ps_4_0", 0, 0, &code, &errors)))
		{
			ui_log("sky: the shader did not compile: %s", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
			release(errors);
			return false;
		}

		device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_pixel);
		release(code);
		release(errors);
		D3D11_BUFFER_DESC buffer{};
		buffer.ByteWidth = 64;
		buffer.Usage = D3D11_USAGE_DEFAULT;
		buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		device->CreateBuffer(&buffer, nullptr, &g_parameters);
		D3D11_SAMPLER_DESC sampler{};
		sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sampler.MaxLOD = D3D11_FLOAT32_MAX;
		device->CreateSamplerState(&sampler, &g_sampler);
		D3D11_RASTERIZER_DESC raster{};
		raster.FillMode = D3D11_FILL_SOLID;
		raster.CullMode = D3D11_CULL_NONE;
		raster.DepthClipEnable = TRUE;
		device->CreateRasterizerState(&raster, &g_raster);
		return g_vertex && g_pixel && g_parameters && g_sampler && g_raster;
	}

	// The map's sky images, and a cube of ours for each of their views.
	bool find(ID3D11Device* device)
	{
		void* images[MAX_SKIES] = {};
		const int count = theater_sky_images(images, MAX_SKIES, g_world, sizeof(g_world));
		if (count <= 0)
		{
			ui_log("sky: the map's sky was not found (%d)", count);
			return false;
		}

		for (int i = 0; i < count && g_sky_count < MAX_SKIES; ++i)
		{
			Sky sky;
			sky.image = static_cast<unsigned char*>(images[i]);
			ID3D11ShaderResourceView *first = nullptr, *second = nullptr;
			if (!read_views(sky.image, &first, &second) || !first)
			{
				ui_log("sky: image %d of %s cannot be read", i, g_world);
				continue;
			}

			sky.original[0] = first;
			sky.original[1] = second;
			bool good = true;
			for (int v = 0; v < 2 && good; ++v)
			{
				if (v == 1 && (!second || second == first))
				{
					break;
				}

				D3D11_SHADER_RESOURCE_VIEW_DESC view{};
				if (!describe(sky.original[v], &view) || view.ViewDimension != D3D11_SRV_DIMENSION_TEXTURECUBE)
				{
					ui_log("sky: image %d of %s is not a cube (view %d is of kind %d, format %d)", i, g_world, v, static_cast<int>(view.ViewDimension),
					       static_cast<int>(view.Format));
					good = false;
					break;
				}

				ID3D11Resource* resource = nullptr;
				sky.original[v]->GetResource(&resource);
				ID3D11Texture2D* cube = nullptr;
				D3D11_TEXTURE2D_DESC desc{};
				if (resource && SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&cube))))
				{
					cube->GetDesc(&desc);
					cube->Release();
				}

				release(resource);
				if (desc.Width < 4 || desc.Width > 8192)
				{
					good = false;
					break;
				}

				ui_log("sky: %s, image %d view %d: a cube of %u, format %d", g_world, i, v, desc.Width, static_cast<int>(view.Format));
				D3D11_TEXTURE2D_DESC ours{};
				ours.Width = ours.Height = desc.Width;
				ours.MipLevels = 0;
				ours.ArraySize = 6;
				ours.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
				ours.SampleDesc.Count = 1;
				ours.Usage = D3D11_USAGE_DEFAULT;
				ours.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
				ours.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE | D3D11_RESOURCE_MISC_GENERATE_MIPS;
				if (FAILED(device->CreateTexture2D(&ours, nullptr, &sky.texture[v])))
				{
					good = false;
					break;
				}

				D3D11_SHADER_RESOURCE_VIEW_DESC made{};
				made.Format = ours.Format;
				made.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
				made.TextureCube.MipLevels = static_cast<UINT>(-1);
				good = SUCCEEDED(device->CreateShaderResourceView(sky.texture[v], &made, &sky.made[v]));
				for (UINT face = 0; face < 6 && good; ++face)
				{
					D3D11_RENDER_TARGET_VIEW_DESC target{};
					target.Format = ours.Format;
					target.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
					target.Texture2DArray.FirstArraySlice = face;
					target.Texture2DArray.ArraySize = 1;
					good = SUCCEEDED(device->CreateRenderTargetView(sky.texture[v], &target, &sky.faces[v][face]));
				}
			}

			if (!good || !sky.made[0])
			{
				for (int v = 0; v < 2; ++v)
				{
					for (ID3D11RenderTargetView*& face : sky.faces[v])
					{
						release(face);
					}

					release(sky.made[v]);
					release(sky.texture[v]);
				}

				continue;
			}

			g_skies[g_sky_count++] = sky;
		}

		return g_sky_count > 0;
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
		ID3D11ShaderResourceView* resource = nullptr;
		ID3D11SamplerState* sampler = nullptr;
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
			c->PSGetSamplers(0, 1, &sampler);
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
			c->PSSetSamplers(0, 1, &sampler);
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
			release(sampler);
			release(constants);
			release(layout);
		}
	};

	// A cube drawn, face by face, into the faces of one of ours, with a colour change and a turn.
	void draw_cube(ID3D11DeviceContext* context, ID3D11ShaderResourceView* source, ID3D11RenderTargetView* const faces[6], const UINT size,
	               const float hsv[3], const float turn[3])
	{
		// the direction a pixel of ours looks up in the source: turned back by the three angles (around the
		// engine's up, then the two across)
		const float a = turn[0] * PI / 180.0f, b = turn[1] * PI / 180.0f, c = turn[2] * PI / 180.0f;
		const float ca = cosf(a), sa = sinf(a), cb = cosf(b), sb = sinf(b), cc = cosf(c), sc = sinf(c);
		// Rz(a) * Ry(b) * Rx(c)
		const float m[3][3] = {{ca * cb, ca * sb * sc - sa * cc, ca * sb * cc + sa * sc},
		                       {sa * cb, sa * sb * sc + ca * cc, sa * sb * cc - ca * sc},
		                       {-sb, cb * sc, cb * cc}};
		float parameters[16] = {};
		parameters[0] = hsv[0] / 360.0f;
		parameters[1] = hsv[1];
		parameters[2] = hsv[2];
		for (int r = 0; r < 3; ++r)
		{
			for (int col = 0; col < 3; ++col)
			{
				parameters[4 + r * 4 + col] = m[r][col];
			}
		}

		context->IASetInputLayout(nullptr);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		context->VSSetShader(g_vertex, nullptr, 0);
		context->PSSetShader(g_pixel, nullptr, 0);
		context->GSSetShader(nullptr, nullptr, 0);
		context->HSSetShader(nullptr, nullptr, 0);
		context->DSSetShader(nullptr, nullptr, 0);
		context->RSSetState(g_raster);
		context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
		context->OMSetDepthStencilState(nullptr, 0);
		context->PSSetSamplers(0, 1, &g_sampler);
		context->PSSetConstantBuffers(0, 1, &g_parameters);
		D3D11_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(size), static_cast<float>(size), 0.0f, 1.0f};
		context->RSSetViewports(1, &viewport);
		for (int face = 0; face < 6; ++face)
		{
			// (ours is not bound to be read while it is drawn to)
			ID3D11ShaderResourceView* none = nullptr;
			context->PSSetShaderResources(0, 1, &none);
			context->OMSetRenderTargets(1, &faces[face], nullptr);
			context->PSSetShaderResources(0, 1, &source);
			parameters[3] = static_cast<float>(face);
			context->UpdateSubresource(g_parameters, 0, nullptr, parameters, 0, 0);
			context->Draw(3, 0);
		}

		ID3D11RenderTargetView* no_target = nullptr;
		context->OMSetRenderTargets(1, &no_target, nullptr);
	}

	// The game's cubes (or the saved sky chosen) drawn into ours with the change wanted.
	void draw(ID3D11DeviceContext* context)
	{
		Kept kept;
		kept.keep(context);
		for (int i = 0; i < g_sky_count; ++i)
		{
			Sky& sky = g_skies[i];
			for (int v = 0; v < 2; ++v)
			{
				if (!sky.made[v])
				{
					continue;
				}

				D3D11_TEXTURE2D_DESC desc{};
				sky.texture[v]->GetDesc(&desc);
				draw_cube(context, g_source_view ? g_source_view : sky.original[v], sky.faces[v], desc.Width, g_hsv, g_turn);
				context->GenerateMips(sky.made[v]);
			}

			if (!sky.swapped)
			{
				write_views(sky.image, sky.made[0], sky.original[1] ? (sky.made[1] ? sky.made[1] : sky.made[0]) : nullptr);
				sky.swapped = true;
			}
		}

		kept.put_back(context);
	}

	// This map's sky written to a file.
	void save(ID3D11Device* device, ID3D11DeviceContext* context)
	{
		if (g_sky_count == 0)
		{
			snprintf(g_message, sizeof(g_message), "This map's sky was not found.");
			return;
		}

		// the map's name: maps/mp/mp_refraction.d3dbsp
		std::string name = g_world;
		const size_t slash = name.find_last_of("/\\");
		name = slash == std::string::npos ? name : name.substr(slash + 1);
		const size_t dot = name.find('.');
		name = dot == std::string::npos ? name : name.substr(0, dot);
		if (name.empty())
		{
			name = "sky";
		}

		D3D11_TEXTURE2D_DESC own{};
		g_skies[0].texture[0]->GetDesc(&own);
		const UINT size = own.Width < SAVED_SIZE ? own.Width : SAVED_SIZE;
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = desc.Height = size;
		desc.MipLevels = 1;
		desc.ArraySize = 6;
		desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_RENDER_TARGET;
		desc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
		ID3D11Texture2D *drawn = nullptr, *read = nullptr;
		ID3D11RenderTargetView* faces[6] = {};
		bool good = SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &drawn));
		for (UINT face = 0; face < 6 && good; ++face)
		{
			D3D11_RENDER_TARGET_VIEW_DESC target{};
			target.Format = desc.Format;
			target.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
			target.Texture2DArray.FirstArraySlice = face;
			target.Texture2DArray.ArraySize = 1;
			good = SUCCEEDED(device->CreateRenderTargetView(drawn, &target, &faces[face]));
		}

		D3D11_TEXTURE2D_DESC staging = desc;
		staging.Usage = D3D11_USAGE_STAGING;
		staging.BindFlags = 0;
		staging.MiscFlags = 0;
		staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		good = good && SUCCEEDED(device->CreateTexture2D(&staging, nullptr, &read));
		FILE* file = nullptr;
		const std::string path = skies_folder() + "\\" + name + ".s1sky";
		if (good)
		{
			Kept kept;
			kept.keep(context);
			const float plain[3] = {0.0f, 1.0f, 1.0f}, still[3] = {};
			draw_cube(context, g_skies[0].original[0], faces, size, plain, still);
			kept.put_back(context);
			for (UINT face = 0; face < 6; ++face)
			{
				context->CopySubresourceRegion(read, face, 0, 0, 0, drawn, face, nullptr);
			}

			good = fopen_s(&file, path.c_str(), "wb") == 0 && file;
		}

		if (good)
		{
			SavedHeader header{};
			memcpy(header.magic, "S1SKY001", 8);
			header.size = size;
			header.format = static_cast<std::uint32_t>(desc.Format);
			fwrite(&header, sizeof(header), 1, file);
			for (UINT face = 0; face < 6 && good; ++face)
			{
				D3D11_MAPPED_SUBRESOURCE mapped{};
				good = SUCCEEDED(context->Map(read, face, D3D11_MAP_READ, 0, &mapped));
				for (UINT row = 0; row < size && good; ++row)
				{
					good = fwrite(static_cast<const unsigned char*>(mapped.pData) + static_cast<size_t>(row) * mapped.RowPitch, static_cast<size_t>(size) * 8, 1, file) == 1;
				}

				if (mapped.pData)
				{
					context->Unmap(read, face);
				}
			}
		}

		if (file)
		{
			fclose(file);
		}

		for (ID3D11RenderTargetView*& face : faces)
		{
			release(face);
		}

		release(drawn);
		release(read);
		snprintf(g_message, sizeof(g_message), good ? "Saved this map's sky as %s." : "The sky could not be saved (%s).", name.c_str());
		ui_log("sky: %s", g_message);
	}

	// The saved sky chosen, as a cube to draw from.
	bool load(ID3D11Device* device)
	{
		if (strcmp(g_source_loaded, g_source_name) == 0)
		{
			return g_source_view != nullptr || !g_source_name[0];
		}

		release(g_source_view);
		release(g_source_texture);
		strncpy_s(g_source_loaded, g_source_name, _TRUNCATE);
		if (!g_source_name[0])
		{
			return true;
		}

		FILE* file = nullptr;
		const std::string path = skies_folder() + "\\" + g_source_name + ".s1sky";
		SavedHeader header{};
		std::vector<unsigned char> pixels;
		bool good = fopen_s(&file, path.c_str(), "rb") == 0 && file;
		good = good && fread(&header, sizeof(header), 1, file) == 1 && memcmp(header.magic, "S1SKY001", 8) == 0 && header.size >= 4 &&
		       header.size <= 4096 && header.format == static_cast<std::uint32_t>(DXGI_FORMAT_R16G16B16A16_FLOAT);
		const size_t face_bytes = static_cast<size_t>(header.size) * header.size * 8;
		if (good)
		{
			pixels.resize(face_bytes * 6);
			good = fread(pixels.data(), 1, pixels.size(), file) == pixels.size();
		}

		if (file)
		{
			fclose(file);
		}

		if (good)
		{
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = desc.Height = header.size;
			desc.MipLevels = 1;
			desc.ArraySize = 6;
			desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D11_USAGE_IMMUTABLE;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			desc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
			D3D11_SUBRESOURCE_DATA data[6] = {};
			for (int face = 0; face < 6; ++face)
			{
				data[face].pSysMem = pixels.data() + face_bytes * static_cast<size_t>(face);
				data[face].SysMemPitch = header.size * 8;
			}

			good = SUCCEEDED(device->CreateTexture2D(&desc, data, &g_source_texture));
			D3D11_SHADER_RESOURCE_VIEW_DESC view{};
			view.Format = desc.Format;
			view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
			view.TextureCube.MipLevels = 1;
			good = good && SUCCEEDED(device->CreateShaderResourceView(g_source_texture, &view, &g_source_view));
		}

		if (!good)
		{
			release(g_source_view);
			release(g_source_texture);
			snprintf(g_message, sizeof(g_message), "The saved sky %s could not be read.", g_source_name);
			ui_log("sky: %s", g_message);
		}

		return good;
	}
}

SkyState sky_state()
{
	std::lock_guard<std::mutex> lock(g_lock);
	SkyState state;
	state.known = g_sky_count > 0;
	state.failed = g_failed;
	state.changed = changed_now();
	memcpy(state.hsv, g_hsv, sizeof(g_hsv));
	memcpy(state.turn, g_turn, sizeof(g_turn));
	strncpy_s(state.picture, g_source_name, _TRUNCATE);
	strncpy_s(state.message, g_message, _TRUNCATE);
	return state;
}

void sky_set(const float hsv[3], const float turn[3])
{
	std::lock_guard<std::mutex> lock(g_lock);
	if (memcmp(g_hsv, hsv, sizeof(g_hsv)) != 0 || memcmp(g_turn, turn, sizeof(g_turn)) != 0)
	{
		memcpy(g_hsv, hsv, sizeof(g_hsv));
		memcpy(g_turn, turn, sizeof(g_turn));
		g_dirty = true;
	}
}

void sky_reset()
{
	const float hsv[3] = {0.0f, 1.0f, 1.0f}, turn[3] = {};
	sky_set(hsv, turn);
	sky_use("");
}

// The picture the sky is drawn from: a saved sky's name, or "" for the map's own.
void sky_use(const char* name)
{
	std::lock_guard<std::mutex> lock(g_lock);
	if (strcmp(g_source_name, name) != 0)
	{
		strncpy_s(g_source_name, name, _TRUNCATE);
		g_dirty = true;
	}
}

void sky_save()
{
	std::lock_guard<std::mutex> lock(g_lock);
	g_save_request = true;
}

int sky_saved(char names[][64], const int max)
{
	WIN32_FIND_DATAA found;
	const HANDLE search = FindFirstFileA((skies_folder() + "\\*.s1sky").c_str(), &found);
	if (search == INVALID_HANDLE_VALUE)
	{
		return 0;
	}

	int count = 0;
	do
	{
		std::string name = found.cFileName;
		name = name.substr(0, name.size() - 6);
		if (count < max)
		{
			strncpy_s(names[count++], 64, name.c_str(), _TRUNCATE);
		}
	} while (FindNextFileA(search, &found));
	FindClose(search);
	return count;
}

// Before the game lets go of the map (playback stopping, an error): its own cube back in its image.
void sky_put_back_now()
{
	std::lock_guard<std::mutex> lock(g_lock);
	put_back();
	g_dirty = changed_now();
}

// Each frame, where the editor draws.
void sky_frame(ID3D11Device* device, ID3D11DeviceContext* context, const TheaterState& state)
{
	std::lock_guard<std::mutex> lock(g_lock);
	if (!state.playing)
	{
		put_back();
		g_was_playing = false;
		g_dirty = changed_now();
		return;
	}

	if (!g_was_playing)
	{
		// another playback may be another map: what was found is of the last one (and ours of it are
		// only let go of now, long after the game last drew with them)
		g_was_playing = true;
		forget();
		g_dirty = changed_now();
	}

	if (g_save_request && state.settled)
	{
		g_save_request = false;
		if (!g_looked)
		{
			g_looked = true;
			g_failed = !shaders(device) || !find(device);
		}

		if (!g_failed)
		{
			save(device, context);
		}
		else
		{
			snprintf(g_message, sizeof(g_message), "This map's sky was not found.");
		}
	}

	if (!changed_now())
	{
		put_back();
		g_dirty = false;
		return;
	}

	if (!g_dirty || g_failed)
	{
		return;
	}

	if (!g_looked)
	{
		if (!state.settled)
		{
			return;
		}

		g_looked = true;
		g_failed = !shaders(device) || !find(device);
		if (g_failed)
		{
			return;
		}
	}

	g_dirty = false;
	if (!load(device))
	{
		// the saved sky cannot be read: the map's own
		g_source_name[0] = 0;
		if (!changed_now())
		{
			put_back();
			return;
		}
	}

	draw(context);
}
