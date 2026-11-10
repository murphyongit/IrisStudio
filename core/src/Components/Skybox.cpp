#include "StdInclude.hpp"
#include "Skybox.hpp"

#include <mutex>
#include <optional>
#include <unordered_map>

#include "D3D9.hpp"
#include "Events.hpp"
#include "Mod.hpp"
#include "SurfaceDepth.hpp"

namespace IWXMVM::Components::Skybox
{
    namespace
    {
        // the bake's pass: a quad over the face (moved half a texel so its texels' middles are sampled), handing the
        // pixel shader where on the face (s right, t down, -1 to 1) each texel is
        constexpr std::string_view VERTEX_SHADER = R"(
float4 halfTexel : register(c0);

struct Out
{
    float4 position : POSITION;
    float2 st : TEXCOORD0;
};

Out main(float4 position : POSITION)
{
    Out result;
    result.position = float4(position.xy + halfTexel.xy, 0.0, 1.0);
    result.st = float2(position.x, -position.y);
    return result;
}
)";

        // a texel of a face: the game's sky sampled along the face's direction (turned back by the grade's rotation, on
        // the CPU), at the level being made, its colour through the grade's rows; alpha (the HDR skies' scale) as it
        // was. Raw values in and out: no sRGB either way, so the game reads them as it read its own.
        constexpr std::string_view PIXEL_SHADER = R"(
samplerCUBE sky : register(s0);
float4 colorRows[3] : register(c0);
float4 faceMiddle : register(c3);
float4 faceRight : register(c4);
float4 faceDown : register(c5);
float4 level : register(c6);

float4 main(float2 st : TEXCOORD0) : COLOR
{
    float3 direction = faceMiddle.xyz + st.x * faceRight.xyz + st.y * faceDown.xyz;
    float4 texel = texCUBElod(sky, float4(direction, level.x));
    float3 color = float3(dot(colorRows[0].xyz, texel.rgb) + colorRows[0].w,
                          dot(colorRows[1].xyz, texel.rgb) + colorRows[1].w,
                          dot(colorRows[2].xyz, texel.rgb) + colorRows[2].w);
    return float4(max(color, 0.0), texel.a);
}
)";

        struct Vertex
        {
            float x, y, z, w;
        };
        constexpr std::array<Vertex, 4> QUAD = {
            Vertex{-1.0f, 1.0f, 0.0f, 1.0f}, Vertex{1.0f, 1.0f, 0.0f, 1.0f}, Vertex{-1.0f, -1.0f, 0.0f, 1.0f},
            Vertex{1.0f, -1.0f, 0.0f, 1.0f}};

        // an image of the map's sky, with its bake
        struct Entry
        {
            IDirect3DBaseTexture9* const* slot = nullptr;
            std::string name;
            std::string material;
            std::string techniqueSet;

            // this frame's texture of the game's, and what the game's last became as set (a pack's)
            IDirect3DBaseTexture9* game = nullptr;
            IDirect3DBaseTexture9* drawn = nullptr;
            IDirect3DBaseTexture9* drawnFor = nullptr;
            bool logged = false;

            // what the bake was made of and with
            IDirect3DCubeTexture9* baked = nullptr;
            IDirect3DBaseTexture9* bakedFrom = nullptr;
            UINT bakedSize = 0;
            D3DFORMAT bakedFormat = D3DFMT_UNKNOWN;
            DWORD bakedLevels = 0;
            std::optional<SkyLogic::Grade> bakedGrade;
            // a source whose cube couldn't be made, not tried again each frame
            IDirect3DBaseTexture9* failedFrom = nullptr;
        };

        std::atomic<bool> stopped = false;
        DWORD renderThread = 0;

        std::mutex mapMutex;
        std::vector<Types::SkyImage> mapImages;
        bool mapChanged = false;

        std::mutex gradeMutex;
        SkyLogic::Grade wantedGrade;

        // the render thread's
        std::vector<Entry> entries;
        std::unordered_map<IDirect3DBaseTexture9*, Entry*> entriesByGame;
        bool active = false;
        bool baking = false;
        IDirect3DVertexShader9* vertexShader = nullptr;
        IDirect3DPixelShader9* pixelShader = nullptr;
        IDirect3DVertexDeclaration9* declaration = nullptr;
        bool shaderFailed = false;

        std::mutex statusMutex;
        Status lastStatus;

        // the texture the game has for an image now, nothing if it can't be read
        IDirect3DBaseTexture9* ReadSlot(IDirect3DBaseTexture9* const* slot)
        {
            __try
            {
                return *slot;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return nullptr;
            }
        }

        bool IsCompressed(D3DFORMAT format)
        {
            return format == D3DFMT_DXT1 || format == D3DFMT_DXT2 || format == D3DFMT_DXT3 || format == D3DFMT_DXT4 ||
                   format == D3DFMT_DXT5;
        }

        void ReleaseBake(Entry& entry)
        {
            if (entry.baked)
                entry.baked->Release();
            entry.baked = nullptr;
            entry.bakedFrom = nullptr;
            entry.bakedGrade.reset();
        }

        void ReleaseBakes()
        {
            for (auto& entry : entries)
                ReleaseBake(entry);
        }

        void ReleaseShaders()
        {
            if (vertexShader)
                vertexShader->Release();
            if (pixelShader)
                pixelShader->Release();
            if (declaration)
                declaration->Release();
            vertexShader = nullptr;
            pixelShader = nullptr;
            declaration = nullptr;
        }

        template <typename Make>
        bool Compile(std::string_view source, const char* profile, Make make)
        {
            ID3DXBuffer* code = nullptr;
            ID3DXBuffer* errors = nullptr;
            const auto hr = D3DXCompileShader(source.data(), static_cast<UINT>(source.size()), nullptr, nullptr, "main",
                                              profile, 0, &code, &errors, nullptr);
            if (FAILED(hr))
            {
                LOG_ERROR("Sky: the bake's {} shader didn't compile: {}", profile,
                          errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no message");
                if (errors)
                    errors->Release();
                if (code)
                    code->Release();
                return false;
            }
            if (errors)
                errors->Release();
            const bool made = SUCCEEDED(make(static_cast<const DWORD*>(code->GetBufferPointer())));
            code->Release();
            return made;
        }

        bool MakeShaders(IDirect3DDevice9* device)
        {
            if (vertexShader && pixelShader && declaration)
                return true;
            if (shaderFailed)
                return false;
            ReleaseShaders();
            const D3DVERTEXELEMENT9 elements[] = {
                {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0}, D3DDECL_END()};
            const bool made =
                Compile(VERTEX_SHADER, "vs_3_0",
                        [&](const DWORD* code) { return device->CreateVertexShader(code, &vertexShader); }) &&
                Compile(PIXEL_SHADER, "ps_3_0",
                        [&](const DWORD* code) { return device->CreatePixelShader(code, &pixelShader); }) &&
                SUCCEEDED(device->CreateVertexDeclaration(elements, &declaration));
            if (!made)
            {
                LOG_ERROR("Sky: the bake's shaders couldn't be made; the game's sky is drawn");
                ReleaseShaders();
                shaderFailed = true;
            }
            return made;
        }

        // a cube for the bake of a game's cube of this size and format: the same format where the card draws into it,
        // 8 bits a channel for a compressed one (DXT can't be drawn into) or when it doesn't
        IDirect3DCubeTexture9* MakeCube(IDirect3DDevice9* device, UINT size, D3DFORMAT format, DWORD levels)
        {
            IDirect3DCubeTexture9* cube = nullptr;
            if (!IsCompressed(format) &&
                SUCCEEDED(device->CreateCubeTexture(size, levels, D3DUSAGE_RENDERTARGET, format, D3DPOOL_DEFAULT,
                                                    &cube, nullptr)))
                return cube;
            if (SUCCEEDED(device->CreateCubeTexture(size, levels, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                                    D3DPOOL_DEFAULT, &cube, nullptr)))
                return cube;
            return nullptr;
        }

        // Draws every face and level of the entry's cube from the game's, the device's state as the game had it after
        bool Bake(IDirect3DDevice9* device, Entry& entry, IDirect3DCubeTexture9* source, const SkyLogic::Grade& grade)
        {
            IDirect3DStateBlock9* state = nullptr;
            if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &state)))
                return false;
            std::array<IDirect3DSurface9*, 4> targets{};
            for (DWORD i = 0; i < targets.size(); ++i)
                if (FAILED(device->GetRenderTarget(i, &targets[i])))
                    targets[i] = nullptr;
            IDirect3DSurface9* depthStencil = nullptr;
            if (FAILED(device->GetDepthStencilSurface(&depthStencil)))
                depthStencil = nullptr;

            baking = true;
            for (DWORD i = 1; i < targets.size(); ++i)
                if (targets[i])
                    device->SetRenderTarget(i, nullptr);
            device->SetDepthStencilSurface(nullptr);
            for (const auto& [renderState, value] : std::initializer_list<std::pair<D3DRENDERSTATETYPE, DWORD>>{
                     {D3DRS_ZENABLE, FALSE},
                     {D3DRS_ZWRITEENABLE, FALSE},
                     {D3DRS_STENCILENABLE, FALSE},
                     {D3DRS_ALPHABLENDENABLE, FALSE},
                     {D3DRS_SEPARATEALPHABLENDENABLE, FALSE},
                     {D3DRS_ALPHATESTENABLE, FALSE},
                     {D3DRS_CULLMODE, D3DCULL_NONE},
                     {D3DRS_FILLMODE, D3DFILL_SOLID},
                     {D3DRS_SCISSORTESTENABLE, FALSE},
                     {D3DRS_CLIPPLANEENABLE, 0},
                     {D3DRS_FOGENABLE, FALSE},
                     {D3DRS_SRGBWRITEENABLE, FALSE},
                     {D3DRS_COLORWRITEENABLE, 0xF},
                 })
                device->SetRenderState(renderState, value);
            device->SetVertexShader(vertexShader);
            device->SetPixelShader(pixelShader);
            device->SetVertexDeclaration(declaration);
            SurfaceDepth::SetTextureDirect(device, 0, source);
            for (const auto& [samplerState, value] : std::initializer_list<std::pair<D3DSAMPLERSTATETYPE, DWORD>>{
                     {D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP},
                     {D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP},
                     {D3DSAMP_ADDRESSW, D3DTADDRESS_CLAMP},
                     {D3DSAMP_MAGFILTER, D3DTEXF_LINEAR},
                     {D3DSAMP_MINFILTER, D3DTEXF_LINEAR},
                     {D3DSAMP_MIPFILTER, D3DTEXF_LINEAR},
                     {D3DSAMP_MIPMAPLODBIAS, 0},
                     {D3DSAMP_MAXMIPLEVEL, 0},
                     {D3DSAMP_SRGBTEXTURE, FALSE},
                 })
                device->SetSamplerState(0, samplerState, value);

            const auto rows = SkyLogic::GetColorRows(grade);
            device->SetPixelShaderConstantF(0, glm::value_ptr(rows[0]), 3);
            const auto rotation = SkyLogic::GetRotation(grade);

            // (the game's level of the same size: the bake has as many levels as the game's, from the same size)
            bool drawn = true;
            const auto size = entry.bakedSize;
            for (DWORD level = 0; level < entry.baked->GetLevelCount() && drawn; ++level)
            {
                const auto faceSize = std::max<UINT>(1, size >> level);
                const std::array<float, 4> halfTexel = {-1.0f / static_cast<float>(faceSize),
                                                        1.0f / static_cast<float>(faceSize), 0.0f, 0.0f};
                device->SetVertexShaderConstantF(0, halfTexel.data(), 1);
                const std::array<float, 4> lod = {static_cast<float>(level), 0.0f, 0.0f, 0.0f};
                device->SetPixelShaderConstantF(6, lod.data(), 1);
                for (std::size_t face = 0; face < SkyLogic::FACE_COUNT; ++face)
                {
                    IDirect3DSurface9* surface = nullptr;
                    if (FAILED(entry.baked->GetCubeMapSurface(static_cast<D3DCUBEMAP_FACES>(face), level, &surface)))
                    {
                        drawn = false;
                        break;
                    }
                    device->SetRenderTarget(0, surface);
                    const D3DVIEWPORT9 viewport{0, 0, faceSize, faceSize, 0.0f, 1.0f};
                    device->SetViewport(&viewport);
                    const auto axes = SkyLogic::GetSampleFace(face, rotation);
                    const std::array<glm::vec4, 3> faceConstants = {glm::vec4(axes.middle, 0.0f),
                                                                    glm::vec4(axes.right, 0.0f),
                                                                    glm::vec4(axes.down, 0.0f)};
                    device->SetPixelShaderConstantF(3, glm::value_ptr(faceConstants[0]), 3);
                    drawn = SUCCEEDED(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, QUAD.data(), sizeof(Vertex)));
                    surface->Release();
                }
            }

            // the game's targets back, then the rest of its state (a target resets the viewport)
            for (DWORD i = 0; i < targets.size(); ++i)
                if (targets[i] || i == 0)
                    device->SetRenderTarget(i, targets[i]);
            device->SetDepthStencilSurface(depthStencil);
            state->Apply();
            state->Release();
            for (const auto target : targets)
                if (target)
                    target->Release();
            if (depthStencil)
                depthStencil->Release();
            baking = false;
            return drawn;
        }

        void PublishStatus(const Status& status)
        {
            std::lock_guard lock(statusMutex);
            lastStatus = status;
        }

        void OnFrame(IDirect3DDevice9* device)
        {
            renderThread = GetCurrentThreadId();
            {
                std::lock_guard lock(mapMutex);
                if (mapChanged)
                {
                    ReleaseBakes();
                    entries.clear();
                    entriesByGame.clear();
                    for (const auto& image : mapImages)
                        entries.push_back({image.texture, image.name, image.material, image.techniqueSet});
                    mapChanged = false;
                }
            }
            SkyLogic::Grade grade;
            {
                std::lock_guard lock(gradeMutex);
                grade = wantedGrade;
            }

            // the textures the map's sky images have now (none while a demo loads)
            entriesByGame.clear();
            Status status;
            status.images = static_cast<std::uint32_t>(entries.size());
            for (auto& entry : entries)
            {
                entry.game = ReadSlot(entry.slot);
                if (!entry.game)
                    continue;
                entriesByGame.try_emplace(entry.game, &entry);
                const bool cube = entry.game->GetType() == D3DRTYPE_CUBETEXTURE;
                status.cubes += cube;
                if (cube && status.name.empty())
                {
                    status.name = entry.name;
                    D3DSURFACE_DESC desc{};
                    if (SUCCEEDED(static_cast<IDirect3DCubeTexture9*>(entry.game)->GetLevelDesc(0, &desc)))
                        status.size = desc.Width;
                }
                if (!entry.logged)
                {
                    entry.logged = true;
                    LOG_INFO("Sky: image {} of material {} ({}), a {}", entry.name, entry.material, entry.techniqueSet,
                             cube ? "cube map" : "flat texture, left as it is");
                }
            }

            // nothing to draw: the game's own sky, and the card's memory back
            active = !SkyLogic::IsGameSky(grade) && !stopped;
            status.active = active;
            if (!active || !MakeShaders(device))
            {
                if (!active)
                    ReleaseBakes();
                status.shaderFailed = shaderFailed;
                PublishStatus(status);
                return;
            }

            for (auto& entry : entries)
            {
                if (!entry.game || entry.game->GetType() != D3DRTYPE_CUBETEXTURE)
                    continue;
                // (what would be drawn: a pack's in place of the game's, if the game set it since)
                const auto sourceTexture = entry.drawnFor == entry.game && entry.drawn ? entry.drawn : entry.game;
                if (sourceTexture->GetType() != D3DRTYPE_CUBETEXTURE)
                    continue;
                const auto source = static_cast<IDirect3DCubeTexture9*>(sourceTexture);
                D3DSURFACE_DESC desc{};
                if (FAILED(source->GetLevelDesc(0, &desc)))
                    continue;
                const auto levels = source->GetLevelCount();
                const bool sameSource = entry.baked && entry.bakedFrom == sourceTexture && entry.bakedSize == desc.Width &&
                                        entry.bakedFormat == desc.Format && entry.bakedLevels == levels;
                if (sameSource && entry.bakedGrade == grade)
                {
                    ++status.baked;
                    continue;
                }
                if (entry.failedFrom == sourceTexture)
                {
                    ++status.failed;
                    continue;
                }
                if (!sameSource)
                {
                    ReleaseBake(entry);
                    entry.baked =
                        MakeCube(device, desc.Width, desc.Format, SkyLogic::GetLevelCount(desc.Width, levels));
                    if (!entry.baked)
                    {
                        LOG_ERROR("Sky: no cube of {} × {} ({}) could be made for {}", desc.Width, desc.Width,
                                  static_cast<std::int32_t>(desc.Format), entry.name);
                        entry.failedFrom = sourceTexture;
                        ++status.failed;
                        continue;
                    }
                    entry.bakedFrom = sourceTexture;
                    entry.bakedSize = desc.Width;
                    entry.bakedFormat = desc.Format;
                    entry.bakedLevels = levels;
                    LOG_INFO("Sky: baking {} ({} × {}, format {}, {} levels)", entry.name, desc.Width, desc.Width,
                             static_cast<std::int32_t>(desc.Format), levels);
                }
                if (Bake(device, entry, source, grade))
                {
                    entry.bakedGrade = grade;
                    ++status.baked;
                }
                else
                {
                    LOG_ERROR("Sky: the bake of {} didn't draw", entry.name);
                    ReleaseBake(entry);
                    entry.failedFrom = sourceTexture;
                    ++status.failed;
                }
            }
            PublishStatus(status);
        }
    }  // namespace

    void SetMapSkyImages(std::vector<Types::SkyImage> images)
    {
        std::lock_guard lock(mapMutex);
        if (images == mapImages)
            return;
        LOG_INFO("Sky: the map has {} sky images", images.size());
        mapImages = std::move(images);
        mapChanged = true;
    }

    void Install()
    {
        // the map's images go as another demo loads
        Events::RegisterListener(EventType::PreDemoLoad, [] { SetMapSkyImages({}); });
        Events::RegisterListener(EventType::PostDemoLoad, [] {
            if (Mod::GetGameInterface()->GetSupportedFeatures() & Types::Features_Sky)
                SetMapSkyImages(Mod::GetGameInterface()->GetSkyImages());
        });
        Events::RegisterListener(EventType::OnFrame, [] {
            if (const auto device = D3D9::GetDevice(); device && !stopped)
                OnFrame(device);
        });
    }

    void SetGrade(const SkyLogic::Grade& grade)
    {
        std::lock_guard lock(gradeMutex);
        wantedGrade = grade;
    }

    SkyLogic::Grade GetGrade()
    {
        std::lock_guard lock(gradeMutex);
        return wantedGrade;
    }

    IDirect3DBaseTexture9* Swap(IDirect3DBaseTexture9* game, IDirect3DBaseTexture9* drawn)
    {
        if (!game || baking || entriesByGame.empty() || GetCurrentThreadId() != renderThread)
            return drawn;
        const auto found = entriesByGame.find(game);
        if (found == entriesByGame.end())
            return drawn;
        auto& entry = *found->second;
        entry.drawn = drawn;
        entry.drawnFor = game;
        return active && entry.baked && entry.bakedGrade.has_value() ? entry.baked : drawn;
    }

    void OnReset()
    {
        ReleaseBakes();
        for (auto& entry : entries)
            entry.failedFrom = nullptr;
    }

    void OnNewDevice()
    {
        ReleaseBakes();
        entriesByGame.clear();
        ReleaseShaders();
        shaderFailed = false;
        std::lock_guard lock(mapMutex);
        mapImages.clear();
        entries.clear();
        mapChanged = false;
    }

    void Shutdown()
    {
        stopped = true;
        active = false;
        entriesByGame.clear();
        ReleaseBakes();
        ReleaseShaders();
    }

    Status GetStatus()
    {
        std::lock_guard lock(statusMutex);
        return lastStatus;
    }
}  // namespace IWXMVM::Components::Skybox
