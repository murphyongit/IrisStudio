#include "StdInclude.hpp"
#include "SurfaceDepth.hpp"

#include <condition_variable>
#include <thread>

#include "Events.hpp"
#include "Mod.hpp"
#include "SurfaceLogic.hpp"
#include "TextureLogic.hpp"
#include "TexturePack.hpp"
#include "Utilities/HookManager.hpp"
#include "Utilities/PathUtils.hpp"

namespace IWXMVM::Components::SurfaceDepth
{
    namespace
    {
        std::atomic<bool> on = false;
        std::atomic<bool> stopped = false;
        std::atomic<float> depth = SurfaceLogic::DEFAULT_DEPTH;
        std::atomic<float> bumpStrength = 1.0f;
        std::atomic<float> minViewSlope = SurfaceLogic::MIN_VIEW_SLOPE;
        std::atomic<float> cavity = SurfaceLogic::DEFAULT_CAVITY;
        std::atomic<float> shadow = SurfaceLogic::DEFAULT_SHADOW;

        // the patch of a shader, made by the worker, then made on the device by the render thread
        struct ShaderEntry
        {
            enum class State
            {
                Waiting,    // for the worker
                Assembled,  // code waiting for the device
                Ready,
                Unpatched,  // reads no normal map, or failed
            };
            std::atomic<State> state = State::Waiting;
            bool failed = false;
            std::vector<DWORD> code;
            IDirect3DPixelShader9* patched = nullptr;
            std::int32_t normalSampler = -1;
            std::optional<std::int32_t> heightSampler;
            bool selfShadow = false;
        };

        // the heights of a normal map, made by the worker, then made a texture by the render thread
        struct HeightEntry
        {
            enum class State
            {
                Unread,
                Waiting,
                Made,  // heights waiting for the device
                Ready,
                Unreadable,
            };
            std::atomic<State> state = State::Unread;
            // where the game keeps the image's texture, which streaming changes, and the width of the texture the heights
            // were made from (made again from a bigger one)
            IDirect3DBaseTexture9* const* slot = nullptr;
            std::uint32_t madeFrom = 0;
            std::string name;
            // what its relief counts for: above 0, the map's surfaces of the types that take depth use it; the others
            // are ready as they are, flat
            float reliefWeight = 0.0f;
            // the pack's file while Textures is Pack: the heights come from it, not from the game's texture
            std::filesystem::path packFile;
            // kept there once made, see HeightFile
            std::filesystem::path file;
            std::int32_t width = 0;
            std::int32_t height = 0;
            SurfaceLogic::Slopes slopes;
            std::vector<std::vector<std::uint8_t>> levels;
            // how much of the Depth it takes (SurfaceLogic::GetDepthShare): none, no texture
            float relief = 0.0f;
            float share = 0.0f;
            IDirect3DTexture9* texture = nullptr;
        };

        // the worker's queue: patches and heights
        std::mutex workMutex;
        std::condition_variable_any workReady;
        std::deque<std::pair<std::shared_ptr<ShaderEntry>, std::vector<DWORD>>> shaderWork;
        std::deque<std::shared_ptr<HeightEntry>> heightWork;
        std::vector<std::jthread> workers;
        // a level of more than a megatexel takes one at a time: its transforms take 150 MB of the game's memory
        std::mutex largeHeights;
        inline constexpr std::int32_t LARGE_HEIGHTS = 1024 * 1024;

        // the map's normal maps as the binding gave them, taken by the render thread at its next frame
        std::mutex mapMutex;
        std::vector<Types::NormalMap> mapNormalMaps;
        bool mapChanged = false;

        // the render thread's: the shaders seen, the map's normal maps with their heights, what the game set
        std::unordered_map<IDirect3DPixelShader9*, std::shared_ptr<ShaderEntry>> shaders;
        std::vector<std::shared_ptr<HeightEntry>> heights;
        // the map's normal maps by the texture each has this frame
        std::unordered_map<IDirect3DBaseTexture9*, HeightEntry*> heightsByTexture;
        std::array<IDirect3DBaseTexture9*, 16> gameTextures{};
        std::shared_ptr<ShaderEntry> current;
        // the game's shader the patched one stands for, and which of them is set: a surface with nothing to change draws
        // by the game's own
        IDirect3DPixelShader9* gameShader = nullptr;
        bool drawingPatched = false;
        IDirect3DTexture9* flatHeight = nullptr;
        // what the map's heights were last matched against: whether Textures is Pack, and which match of the pack
        std::pair<bool, std::uint32_t> packSeen{};
        DWORD renderThread = 0;

        // the sampler the height took from the game, with its states as the game had them
        inline constexpr std::array<D3DSAMPLERSTATETYPE, 8> HEIGHT_SAMPLER_STATES{
            D3DSAMP_ADDRESSU,  D3DSAMP_ADDRESSV,      D3DSAMP_MAGFILTER,   D3DSAMP_MINFILTER,
            D3DSAMP_MIPFILTER, D3DSAMP_MIPMAPLODBIAS, D3DSAMP_MAXMIPLEVEL, D3DSAMP_SRGBTEXTURE,
        };
        inline constexpr std::array<DWORD, 8> HEIGHT_SAMPLER_VALUES{
            D3DTADDRESS_WRAP, D3DTADDRESS_WRAP, D3DTEXF_LINEAR, D3DTEXF_LINEAR, D3DTEXF_LINEAR, 0, 0, FALSE,
        };
        std::optional<std::int32_t> takenSampler;
        std::array<DWORD, 8> takenStates{};

        std::atomic<Stats> lastStats;
        Stats frameStats;

        typedef HRESULT(__stdcall* SetPixelShader_t)(IDirect3DDevice9* device, IDirect3DPixelShader9* shader);
        SetPixelShader_t SetPixelShader;
        typedef HRESULT(__stdcall* SetTexture_t)(IDirect3DDevice9* device, DWORD stage, IDirect3DBaseTexture9* texture);
        SetTexture_t SetTexture;
        typedef HRESULT(__stdcall* CreatePixelShader_t)(IDirect3DDevice9* device, const DWORD* function,
                                                         IDirect3DPixelShader9** shader);
        CreatePixelShader_t CreatePixelShader;

        // shaders made since the render thread last looked (on any thread, as zones load): one made where a released
        // one was isn't that one, so what was made for the old one goes
        std::mutex madeMutex;
        std::vector<IDirect3DPixelShader9*> madeShaders;
        std::atomic<std::uint32_t> madeCount = 0;
        std::uint32_t madeSeen = 0;

        HRESULT __stdcall CreatePixelShader_Hook(IDirect3DDevice9* device, const DWORD* function,
                                                 IDirect3DPixelShader9** shader)
        {
            const auto result = CreatePixelShader(device, function, shader);
            if (SUCCEEDED(result) && shader && *shader)
            {
                std::lock_guard lock(madeMutex);
                madeShaders.push_back(*shader);
                ++madeCount;
            }
            return result;
        }

        void ForgetRemadeShaders()
        {
            if (madeCount == madeSeen)
                return;
            std::lock_guard lock(madeMutex);
            for (const auto shader : madeShaders)
            {
                const auto found = shaders.find(shader);
                if (found == shaders.end())
                    continue;
                if (found->second->patched)
                {
                    found->second->patched->Release();
                    found->second->patched = nullptr;
                }
                shaders.erase(found);
            }
            madeShaders.clear();
            madeSeen = madeCount;
        }

        // <name>_<width>x<height>.height from the game's texture, with the pack's file's size from a pack's
        std::filesystem::path HeightFile(std::string name, std::int32_t width, std::int32_t height,
                                         std::uintmax_t packBytes)
        {
            constexpr std::string_view unfit = "<>:\"/\\|?*";
            std::ranges::replace_if(name, [&](char c) { return unfit.find(c) != std::string_view::npos; }, '_');
            const auto file = packBytes ? std::format("{}_{}x{}_{:x}.height", name, width, height, packBytes)
                                        : std::format("{}_{}x{}.height", name, width, height);
            return PathUtils::GetModPath() / "surface" / std::format("v{}", SurfaceLogic::HEIGHTS_VERSION) / file;
        }

        // a height's file: its relief, then its values unless it takes no depth
        std::size_t HeightFileBytes(float relief, std::int32_t width, std::int32_t height)
        {
            return sizeof(float) +
                   (SurfaceLogic::GetDepthShare(relief) > 0.0f ? static_cast<std::size_t>(width) * height : 0);
        }

        // whether a height's file is there, whole
        bool IsHeightMade(const HeightEntry& entry)
        {
            std::error_code error;
            const auto bytes = std::filesystem::file_size(entry.file, error);
            return !error && (bytes == sizeof(float) ||
                              bytes == sizeof(float) + static_cast<std::size_t>(entry.width) * entry.height);
        }

        // the level of a pack's normal map the heights come from: its size and file, and its slopes unless they're made
        bool ReadPackLevel(HeightEntry& entry)
        {
            std::ifstream file(entry.packFile, std::ios::binary);
            std::array<std::uint8_t, TextureLogic::DDS_HEADER_SIZE> head{};
            std::error_code error;
            const auto bytes = std::filesystem::file_size(entry.packFile, error);
            if (error || !file.read(reinterpret_cast<char*>(head.data()), head.size()))
                return false;
            const auto dds = TextureLogic::ParseDdsHeader(head, bytes);
            if (!dds || dds->format != TextureLogic::FORMAT_DXT5 || !dds->complete)
                return false;
            auto width = dds->width;
            auto height = dds->height;
            while (std::max(width, height) > SurfaceLogic::MAX_PACK_HEIGHTS)
            {
                width = std::max(width / 2, 1u);
                height = std::max(height / 2, 1u);
            }
            const auto level = TextureLogic::FindDdsLevel(*dds, width, height);
            if (!level || width < 4 || height < 4)
                return false;
            entry.width = static_cast<std::int32_t>(width);
            entry.height = static_cast<std::int32_t>(height);
            entry.file = HeightFile(entry.name, entry.width, entry.height, bytes);
            if (IsHeightMade(entry))
                return true;
            std::vector<std::uint8_t> blocks(level->bytes);
            file.seekg(static_cast<std::streamoff>(level->offset));
            if (!file.read(reinterpret_cast<char*>(blocks.data()), blocks.size()))
                return false;
            entry.slopes = SurfaceLogic::ReadDxt5Slopes(blocks.data(), entry.width, entry.height, ((width + 3) / 4) * 16);
            return true;
        }

        // the heights of a normal map, from their file or made, ready for the render thread
        bool MakeHeights(HeightEntry& entry)
        {
            if (!entry.packFile.empty() && !ReadPackLevel(entry))
                return false;
            std::vector<std::uint8_t> values;
            float relief = 0.0f;
            if (entry.slopes.x.empty())
            {
                // made before
                std::ifstream file(entry.file, std::ios::binary);
                if (!file.read(reinterpret_cast<char*>(&relief), sizeof(relief)))
                    return false;
                values.resize(HeightFileBytes(relief, entry.width, entry.height) - sizeof(relief));
                if (!file.read(reinterpret_cast<char*>(values.data()), values.size()))
                    return false;
            }
            else
            {
                std::unique_lock large(largeHeights, std::defer_lock);
                if (entry.width * entry.height > LARGE_HEIGHTS)
                    large.lock();
                auto made = SurfaceLogic::MakeHeights(entry.slopes);
                entry.slopes = {};
                if (made.values.empty())
                    return false;
                relief = made.relief;
                if (SurfaceLogic::GetDepthShare(relief) > 0.0f)
                    values = std::move(made.values);
                std::error_code error;
                std::filesystem::create_directories(entry.file.parent_path(), error);
                std::ofstream file(entry.file, std::ios::binary);
                file.write(reinterpret_cast<const char*>(&relief), sizeof(relief));
                file.write(reinterpret_cast<const char*>(values.data()), values.size());
            }
            entry.relief = relief;
            entry.share = values.empty() ? 0.0f : SurfaceLogic::GetDepthShare(relief * entry.reliefWeight);
            if (entry.share > 0.0f)
                entry.levels = SurfaceLogic::MakeLevels(values, entry.width, entry.height);
            return true;
        }

        void Work(std::stop_token stop, bool makesShaders)
        {
            while (!stop.stop_requested())
            {
                std::shared_ptr<ShaderEntry> shader;
                std::vector<DWORD> code;
                std::shared_ptr<HeightEntry> height;
                {
                    std::unique_lock lock(workMutex);
                    workReady.wait(lock, stop, [&] {
                        return (makesShaders && !shaderWork.empty()) || !heightWork.empty();
                    });
                    if (stop.stop_requested())
                        return;
                    if (makesShaders && !shaderWork.empty())
                    {
                        std::tie(shader, code) = std::move(shaderWork.front());
                        shaderWork.pop_front();
                    }
                    else
                    {
                        height = std::move(heightWork.front());
                        heightWork.pop_front();
                    }
                }

                if (shader)
                {
                    LPD3DXBUFFER text = nullptr;
                    std::optional<SurfaceLogic::ShaderPatch> patch;
                    if (SUCCEEDED(D3DXDisassembleShader(code.data(), FALSE, nullptr, &text)))
                    {
                        patch = SurfaceLogic::PatchShader(std::string_view(
                            static_cast<const char*>(text->GetBufferPointer()), text->GetBufferSize() - 1));
                        text->Release();
                    }
                    LPD3DXBUFFER assembled = nullptr;
                    LPD3DXBUFFER errors = nullptr;
                    if (patch && SUCCEEDED(D3DXAssembleShader(patch->source.data(),
                                                              static_cast<UINT>(patch->source.size()), nullptr,
                                                              nullptr, 0, &assembled, &errors)))
                    {
                        const auto words = static_cast<const DWORD*>(assembled->GetBufferPointer());
                        shader->code.assign(words, words + assembled->GetBufferSize() / sizeof(DWORD));
                        shader->normalSampler = patch->normalSamplers.front();
                        shader->heightSampler = patch->heightSampler;
                        shader->selfShadow = patch->selfShadow;
                        shader->state = ShaderEntry::State::Assembled;
                    }
                    else
                    {
                        if (patch)
                        {
                            shader->failed = true;
                            LOG_WARN("Surface depth: a patched shader didn't assemble: {}",
                                     errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
                        }
                        shader->state = ShaderEntry::State::Unpatched;
                    }
                    if (assembled)
                        assembled->Release();
                    if (errors)
                        errors->Release();
                }
                else if (height)
                {
                    bool made = false;
                    try
                    {
                        made = MakeHeights(*height);
                    }
                    catch (const std::bad_alloc&)
                    {
                        LOG_WARN("Surface depth: no memory left for the heights of {}", height->name);
                    }
                    height->state = made ? HeightEntry::State::Made : HeightEntry::State::Unreadable;
                }
            }
        }

        // the render thread's work of a frame (below)
        void OnFrame(IDirect3DDevice9* device);

        void Queue(std::shared_ptr<ShaderEntry> shader, std::vector<DWORD> code)
        {
            {
                std::lock_guard lock(workMutex);
                shaderWork.emplace_back(std::move(shader), std::move(code));
            }
            // (all: only the first worker makes shaders, and another taking the wake would leave this one queued)
            workReady.notify_all();
        }

        void Queue(std::shared_ptr<HeightEntry> height)
        {
            {
                std::lock_guard lock(workMutex);
                heightWork.push_back(std::move(height));
            }
            workReady.notify_one();
        }

        // the entry of a shader the game sets, its patch queued the first time
        std::shared_ptr<ShaderEntry> FindShader(IDirect3DDevice9* device, IDirect3DPixelShader9* shader)
        {
            const auto found = shaders.find(shader);
            if (found != shaders.end())
            {
                const auto entry = found->second;
                if (entry->state == ShaderEntry::State::Assembled)
                {
                    if (SUCCEEDED(device->CreatePixelShader(entry->code.data(), &entry->patched)))
                        entry->state = ShaderEntry::State::Ready;
                    else
                    {
                        entry->failed = true;
                        entry->state = ShaderEntry::State::Unpatched;
                    }
                    entry->code = {};
                }
                return entry;
            }

            auto entry = std::make_shared<ShaderEntry>();
            shaders.emplace(shader, entry);
            UINT size = 0;
            std::vector<DWORD> code;
            if (SUCCEEDED(shader->GetFunction(nullptr, &size)) && size > 0)
            {
                code.resize(size / sizeof(DWORD));
                if (FAILED(shader->GetFunction(code.data(), &size)))
                    code.clear();
            }
            if (code.empty())
                entry->state = ShaderEntry::State::Unpatched;
            else
                Queue(entry, std::move(code));
            return entry;
        }

        // gives the game back the sampler the height took
        void ReturnSampler(IDirect3DDevice9* device)
        {
            if (!takenSampler)
                return;
            const auto stage = static_cast<DWORD>(*takenSampler);
            SetTexture(device, stage, TexturePack::Swap(gameTextures[stage]));
            for (std::size_t i = 0; i < HEIGHT_SAMPLER_STATES.size(); ++i)
                device->SetSamplerState(stage, HEIGHT_SAMPLER_STATES[i], takenStates[i]);
            takenSampler.reset();
        }

        // the shader for the normal map the game set, with the draw's constants and height: the patched one where the
        // surface has depth or a strength to draw, the game's own everywhere else (concrete, vehicles, characters)
        void Bind(IDirect3DDevice9* device)
        {
            const auto normal = gameTextures[current->normalSampler];
            const auto found = heightsByTexture.find(normal);
            const auto entry = found != heightsByTexture.end() ? found->second : nullptr;
            const auto share = entry && entry->state == HeightEntry::State::Ready ? entry->share : 0.0f;
            const auto drawnDepth = SurfaceLogic::GetDrawnDepth(on, depth) * share;
            const bool hasDepth = current->heightSampler && drawnDepth > 0.0f;
            const bool patched = hasDepth || (entry && bumpStrength != 1.0f);
            if (patched != drawingPatched)
            {
                SetPixelShader(device, patched ? current->patched : gameShader);
                drawingPatched = patched;
            }
            if (!patched)
            {
                ReturnSampler(device);
                return;
            }

            const auto params = SurfaceLogic::GetParams(hasDepth ? drawnDepth : 0.0f, bumpStrength, minViewSlope);
            device->SetPixelShaderConstantF(SurfaceLogic::PARAMS_REGISTER, params.data(), 1);
            const auto shading = SurfaceLogic::GetShading(hasDepth ? cavity.load() : 0.0f,
                                                          hasDepth ? shadow.load() : 0.0f);
            device->SetPixelShaderConstantF(SurfaceLogic::PARAMS2_REGISTER, shading.data(), 1);
            ++frameStats.mapDraws;

            if (!current->heightSampler)
                return;
            const auto stage = *current->heightSampler;
            if (takenSampler != stage)
            {
                ReturnSampler(device);
                for (std::size_t i = 0; i < HEIGHT_SAMPLER_STATES.size(); ++i)
                    device->GetSamplerState(stage, HEIGHT_SAMPLER_STATES[i], &takenStates[i]);
                for (std::size_t i = 0; i < HEIGHT_SAMPLER_STATES.size(); ++i)
                    device->SetSamplerState(stage, HEIGHT_SAMPLER_STATES[i], HEIGHT_SAMPLER_VALUES[i]);
                takenSampler = stage;
            }
            SetTexture(device, stage, hasDepth ? entry->texture : flatHeight);
        }

        HRESULT __stdcall SetPixelShader_Hook(IDirect3DDevice9* device, IDirect3DPixelShader9* shader)
        {
            current.reset();
            drawingPatched = false;
            if (SurfaceLogic::IsPatched(on, bumpStrength) && !stopped && shader && GetCurrentThreadId() == renderThread)
            {
                ForgetRemadeShaders();
                auto entry = FindShader(device, shader);
                if (entry->state == ShaderEntry::State::Ready)
                {
                    const auto result = SetPixelShader(device, shader);
                    current = std::move(entry);
                    gameShader = shader;
                    Bind(device);
                    frameStats.patchedDraws += drawingPatched;
                    return result;
                }
            }
            if (takenSampler && GetCurrentThreadId() == renderThread)
                ReturnSampler(device);
            return SetPixelShader(device, shader);
        }

        HRESULT __stdcall SetTexture_Hook(IDirect3DDevice9* device, DWORD stage, IDirect3DBaseTexture9* texture)
        {
            if (stage >= gameTextures.size() || GetCurrentThreadId() != renderThread)
                return SetTexture(device, stage, texture);
            gameTextures[stage] = texture;
            // (a texture pack's in place of the game's)
            const auto result = SetTexture(device, stage, TexturePack::Swap(texture));
            if (current && (static_cast<std::int32_t>(stage) == current->normalSampler || takenSampler == stage))
                Bind(device);
            return result;
        }

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

        // the width of a texture's first level: an image streamed out has a smaller texture (4 texels across)
        std::uint32_t GetWidth(IDirect3DBaseTexture9* base)
        {
            D3DSURFACE_DESC desc{};
            if (base->GetType() != D3DRTYPE_TEXTURE ||
                FAILED(static_cast<IDirect3DTexture9*>(base)->GetLevelDesc(0, &desc)))
                return 0;
            return desc.Width;
        }

        // below this an image's texture is taken as streamed out
        inline constexpr std::uint32_t MIN_STREAMED_WIDTH = 16;
        // heights made textures in a frame
        inline constexpr std::int32_t MADE_PER_FRAME = 4;

        // a normal map's level the heights are made from: the first of at most 512 texels across (DXT5 only)
        std::optional<std::pair<DWORD, D3DSURFACE_DESC>> FindLevel(IDirect3DBaseTexture9* base)
        {
            if (!base || base->GetType() != D3DRTYPE_TEXTURE)
                return std::nullopt;
            const auto texture = static_cast<IDirect3DTexture9*>(base);
            for (DWORD level = 0; level < texture->GetLevelCount(); ++level)
            {
                D3DSURFACE_DESC desc{};
                if (FAILED(texture->GetLevelDesc(level, &desc)) || desc.Format != D3DFMT_DXT5 || desc.Width < 4 ||
                    desc.Height < 4)
                    return std::nullopt;
                if (desc.Width <= 512 && desc.Height <= 512)
                    return std::pair(level, desc);
            }
            return std::nullopt;
        }

        // that level's slopes, for the worker (not for a texture the device keeps alone)
        bool ReadSlopes(IDirect3DBaseTexture9* base, DWORD level, const D3DSURFACE_DESC& desc,
                        SurfaceLogic::Slopes& slopes)
        {
            const auto texture = static_cast<IDirect3DTexture9*>(base);
            D3DLOCKED_RECT rect{};
            if (desc.Pool == D3DPOOL_DEFAULT || FAILED(texture->LockRect(level, &rect, nullptr, D3DLOCK_READONLY)))
                return false;
            slopes = SurfaceLogic::ReadDxt5Slopes(static_cast<const std::uint8_t*>(rect.pBits),
                                                  static_cast<std::int32_t>(desc.Width),
                                                  static_cast<std::int32_t>(desc.Height), rect.Pitch);
            texture->UnlockRect(level);
            return true;
        }

        // heights on the card only (D3DPOOL_DEFAULT): they don't take the game's 32-bit address space. Let go
        // before a device reset (OnReset)
        IDirect3DTexture9* MakeHeightTexture(IDirect3DDevice9* device,
                                             const std::vector<std::vector<std::uint8_t>>& levels, std::int32_t width,
                                             std::int32_t height)
        {
            const auto count = static_cast<UINT>(levels.size());
            IDirect3DTexture9* copy = nullptr;
            if (FAILED(device->CreateTexture(width, height, count, 0, D3DFMT_L8, D3DPOOL_SYSTEMMEM, &copy, nullptr)))
                return nullptr;
            for (UINT level = 0; level < count; ++level)
            {
                D3DLOCKED_RECT rect{};
                if (FAILED(copy->LockRect(level, &rect, nullptr, 0)))
                {
                    copy->Release();
                    return nullptr;
                }
                const auto levelWidth = std::max(width >> level, 1);
                const auto levelHeight = std::max(height >> level, 1);
                for (std::int32_t y = 0; y < levelHeight; ++y)
                    std::memcpy(static_cast<std::uint8_t*>(rect.pBits) + y * rect.Pitch,
                                levels[level].data() + static_cast<std::size_t>(y) * levelWidth, levelWidth);
                copy->UnlockRect(level);
            }
            IDirect3DTexture9* texture = nullptr;
            const bool made =
                SUCCEEDED(device->CreateTexture(width, height, count, 0, D3DFMT_L8, D3DPOOL_DEFAULT, &texture, nullptr)) &&
                SUCCEEDED(device->UpdateTexture(copy, texture));
            copy->Release();
            if (!made && texture)
            {
                texture->Release();
                texture = nullptr;
            }
            return texture;
        }

        // the height of a surface without one (managed: it stays over a reset)
        IDirect3DTexture9* MakeFlatTexture(IDirect3DDevice9* device)
        {
            IDirect3DTexture9* texture = nullptr;
            if (FAILED(device->CreateTexture(1, 1, 1, 0, D3DFMT_L8, D3DPOOL_MANAGED, &texture, nullptr)))
                return nullptr;
            D3DLOCKED_RECT rect{};
            if (SUCCEEDED(texture->LockRect(0, &rect, nullptr, 0)))
            {
                *static_cast<std::uint8_t*>(rect.pBits) = 255;
                texture->UnlockRect(0);
            }
            return texture;
        }

        std::shared_ptr<HeightEntry> MakeEntry(const Types::NormalMap& normalMap)
        {
            auto entry = std::make_shared<HeightEntry>();
            entry->slot = normalMap.texture;
            entry->name = normalMap.name;
            entry->reliefWeight = normalMap.reliefWeight;
            if (entry->reliefWeight == 0.0f)
                entry->state = HeightEntry::State::Ready;
            return entry;
        }

        void ReleaseMap()
        {
            for (auto& [_, entry] : shaders)
                if (entry->patched)
                {
                    entry->patched->Release();
                    entry->patched = nullptr;
                }
            shaders.clear();
            for (auto& entry : heights)
                if (entry->texture)
                {
                    entry->texture->Release();
                    entry->texture = nullptr;
                }
            heights.clear();
            heightsByTexture.clear();
            current.reset();
        }
    }  // namespace

    void SetMapNormalMaps(std::vector<Types::NormalMap> normalMaps)
    {
        std::lock_guard lock(mapMutex);
        if (normalMaps == mapNormalMaps)
            return;
        LOG_INFO("Surface depth: the map has {} normal maps", normalMaps.size());
        mapNormalMaps = std::move(normalMaps);
        mapChanged = true;
    }

    void Install(void** deviceMethods)
    {
        HookManager::CreateHook(reinterpret_cast<std::uintptr_t>(deviceMethods[107]),
                                reinterpret_cast<std::uintptr_t>(SetPixelShader_Hook),
                                reinterpret_cast<std::uintptr_t*>(&SetPixelShader));
        HookManager::CreateHook(reinterpret_cast<std::uintptr_t>(deviceMethods[106]),
                                reinterpret_cast<std::uintptr_t>(CreatePixelShader_Hook),
                                reinterpret_cast<std::uintptr_t*>(&CreatePixelShader));
        HookManager::CreateHook(reinterpret_cast<std::uintptr_t>(deviceMethods[65]),
                                reinterpret_cast<std::uintptr_t>(SetTexture_Hook),
                                reinterpret_cast<std::uintptr_t*>(&SetTexture));
        const auto threads = std::clamp(std::thread::hardware_concurrency() / 3, 1u, 4u);
        for (std::uint32_t i = 0; i < threads; ++i)
            workers.emplace_back(Work, i == 0);

        // the map's images go as another demo loads
        Events::RegisterListener(EventType::PreDemoLoad, [] { SetMapNormalMaps({}); });
        Events::RegisterListener(EventType::PostDemoLoad,
                                 [] { SetMapNormalMaps(Mod::GetGameInterface()->GetMapNormalMaps()); });
        Events::RegisterListener(EventType::OnFrame, [] {
            if (const auto device = D3D9::GetDevice(); device && !stopped)
                OnFrame(device);
        });
    }

    void Shutdown(IDirect3DDevice9* device)
    {
        stopped = true;
        workers.clear();
        ReturnSampler(device);
        ReleaseMap();
        if (flatHeight)
        {
            flatHeight->Release();
            flatHeight = nullptr;
        }
    }

    namespace
    {
        void OnFrame(IDirect3DDevice9* device)
        {
            renderThread = GetCurrentThreadId();
            ForgetRemadeShaders();
            if (takenSampler)
                ReturnSampler(device);
            current.reset();

            {
                std::lock_guard lock(mapMutex);
                if (mapChanged)
                {
                    ReleaseMap();
                    for (const auto& normalMap : mapNormalMaps)
                        heights.push_back(MakeEntry(normalMap));
                    mapChanged = false;
                    packSeen = {};
                }
            }
            if (!flatHeight)
                flatHeight = MakeFlatTexture(device);

            // Textures on Pack, the heights come from the pack's file of each normal map it has; another pack or Game
            // make new entries (one the worker is making finishes on its own, and goes)
            const std::pair packNow{TexturePack::IsOn(), TexturePack::GetGeneration()};
            if (packNow != packSeen)
            {
                packSeen = packNow;
                for (auto& entry : heights)
                {
                    if (entry->reliefWeight == 0.0f)
                        continue;
                    auto file = packNow.first ? TexturePack::FindFile(entry->name) : std::filesystem::path{};
                    if (file == entry->packFile)
                        continue;
                    if (entry->texture)
                        entry->texture->Release();
                    entry->texture = nullptr;
                    auto fresh = std::make_shared<HeightEntry>();
                    fresh->slot = entry->slot;
                    fresh->name = entry->name;
                    fresh->reliefWeight = entry->reliefWeight;
                    fresh->packFile = std::move(file);
                    entry = std::move(fresh);
                }
            }

            Stats stats = frameStats;
            frameStats = {};
            // the textures the map's images have now (none while a demo loads); a few normal maps read a frame, once their
            // image is streamed in whole, and the worker makes their heights
            heightsByTexture.clear();
            const bool inDemo = Mod::GetGameInterface()->GetGameState() == Types::GameState::InDemo;
            std::int32_t reads = 0, made = 0;
            for (auto& entry : heights)
            {
                const auto texture = inDemo ? ReadSlot(entry->slot) : nullptr;
                if (texture)
                    heightsByTexture[texture] = entry.get();
                const auto width = texture ? GetWidth(texture) : 0;
                // streamed in bigger than the heights were made from: made again
                const auto done = entry->state == HeightEntry::State::Ready ||
                                  entry->state == HeightEntry::State::Unreadable;
                if (done && entry->packFile.empty() && entry->madeFrom > 0 && width > entry->madeFrom)
                {
                    if (entry->texture)
                        entry->texture->Release();
                    entry->texture = nullptr;
                    entry->state = HeightEntry::State::Unread;
                }
                const bool unread = entry->state == HeightEntry::State::Unread;
                const bool fromPack = !entry->packFile.empty();
                const bool streamedIn = fromPack || width >= MIN_STREAMED_WIDTH;
                stats.streaming += unread && !streamedIn;
                if (unread && on && fromPack)
                {
                    entry->state = HeightEntry::State::Waiting;
                    Queue(entry);
                }
                else if (unread && streamedIn && on && reads++ < 8)
                {
                    entry->madeFrom = width;
                    const auto level = FindLevel(texture);
                    if (level)
                    {
                        const auto& [index, desc] = *level;
                        entry->width = static_cast<std::int32_t>(desc.Width);
                        entry->height = static_cast<std::int32_t>(desc.Height);
                        entry->file = HeightFile(entry->name, entry->width, entry->height, 0);
                        if (IsHeightMade(*entry) || ReadSlopes(texture, index, desc, entry->slopes))
                        {
                            entry->state = HeightEntry::State::Waiting;
                            Queue(entry);
                        }
                    }
                    if (entry->state == HeightEntry::State::Unread)
                    {
                        entry->state = HeightEntry::State::Unreadable;
                        LOG_DEBUG("Surface depth: can't read the normal map {}", entry->name);
                    }
                }
                if (entry->state == HeightEntry::State::Made && made < MADE_PER_FRAME)
                {
                    ++made;
                    if (entry->share > 0.0f)
                        entry->texture = MakeHeightTexture(device, entry->levels, entry->width, entry->height);
                    entry->levels = {};
                    entry->state = entry->texture || entry->share == 0.0f ? HeightEntry::State::Ready
                                                                          : HeightEntry::State::Unreadable;
                }
                ++stats.normalMaps;
                const bool ready = entry->state == HeightEntry::State::Ready;
                stats.heights += ready;
                stats.unreadable += entry->state == HeightEntry::State::Unreadable;
                stats.withDepthMaps += ready && entry->texture;
                stats.fromPack += ready && entry->texture && fromPack;
                if (ready && entry->texture)
                    stats.heightBytes += entry->width * entry->height * 4 / 3;
            }
            for (const auto& [_, entry] : shaders)
            {
                if (entry->state == ShaderEntry::State::Unpatched && !entry->failed)
                    continue;
                ++stats.shaders;
                // (assembled ones are made on the device when next drawn)
                const bool made =
                    entry->state == ShaderEntry::State::Ready || entry->state == ShaderEntry::State::Assembled;
                stats.patched += made;
                stats.withDepth += made && entry->heightSampler.has_value();
                stats.failed += entry->failed;
            }
            lastStats = stats;
        }
    }  // namespace

    void SetOn(bool value)
    {
        on = value;
    }

    bool IsOn()
    {
        return on;
    }

    void SetDepth(float value)
    {
        depth = value;
    }

    float GetDepth()
    {
        return depth;
    }

    void SetBumpStrength(float strength)
    {
        bumpStrength = strength;
    }

    float GetBumpStrength()
    {
        return bumpStrength;
    }

    void SetMinViewSlope(float slope)
    {
        minViewSlope = slope;
    }

    float GetMinViewSlope()
    {
        return minViewSlope;
    }

    void SetCavity(float value)
    {
        cavity = value;
    }

    float GetCavity()
    {
        return cavity;
    }

    void SetShadow(float value)
    {
        shadow = value;
    }

    float GetShadow()
    {
        return shadow;
    }

    std::vector<HeightInfo> GetHeights()
    {
        std::vector<HeightInfo> infos;
        for (const auto& entry : heights)
            if (entry->reliefWeight > 0.0f)
                infos.push_back({entry->name, entry->width, entry->height, entry->relief, entry->share,
                                 !entry->packFile.empty(), entry->state == HeightEntry::State::Ready && entry->texture});
        return infos;
    }

    void OnReset(IDirect3DDevice9* device)
    {
        // (the card's heights go; read from their files again as drawn)
        ReturnSampler(device);
        current.reset();
        drawingPatched = false;
        for (auto& entry : heights)
            if (entry->texture)
            {
                entry->texture->Release();
                entry->texture = nullptr;
                entry->state = HeightEntry::State::Unread;
            }
    }

    void OnNewDevice()
    {
        takenSampler.reset();
        drawingPatched = false;
        ReleaseMap();
        packSeen = {};
        if (flatHeight)
        {
            flatHeight->Release();
            flatHeight = nullptr;
        }
    }

    Stats GetStats()
    {
        return lastStats;
    }

    RestartLogic::Load GetLoad()
    {
        const Stats stats = lastStats;
        const auto done = stats.patched + stats.failed;
        const auto heights = stats.heights + stats.unreadable + stats.streaming;
        return {.on = SurfaceLogic::IsPatched(on, bumpStrength),
                .drawn = (stats.shaders - std::min(stats.shaders, done)) +
                         (on ? stats.normalMaps - std::min(stats.normalMaps, heights) : 0)};
    }
}  // namespace IWXMVM::Components::SurfaceDepth
