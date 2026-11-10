#include "StdInclude.hpp"
#include "Skybox.hpp"

#include <chrono>
#include <format>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>

#include "D3D9.hpp"
#include "Events.hpp"
#include "Mod.hpp"

namespace IWXMVM::Components::Skybox
{
    namespace
    {
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

            // the game's cube read into memory, each level (the first as the game has it, the rest halved from it)
            std::vector<SkyLogic::Cube> source;
            IDirect3DBaseTexture9* sourceFrom = nullptr;

            // the turned and graded cube (in the managed pool: it stays through a device reset), and what it was
            // made of and with
            IDirect3DCubeTexture9* baked = nullptr;
            IDirect3DBaseTexture9* bakedFrom = nullptr;
            std::optional<SkyLogic::Grade> bakedGrade;
            // a source that couldn't be read or baked, not tried again each frame, and why
            IDirect3DBaseTexture9* failedFrom = nullptr;
            std::string error;
            // the middle of the top face, the game's and the baked (0xRRGGBB), and how long the bake took
            std::uint32_t sourceTop = 0;
            std::uint32_t bakedTop = 0;
            std::uint32_t bakeMs = 0;
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

        std::string FormatName(D3DFORMAT format)
        {
            const auto value = static_cast<std::uint32_t>(format);
            if (format == D3DFMT_A8R8G8B8)
                return "A8R8G8B8";
            if (format == D3DFMT_X8R8G8B8)
                return "X8R8G8B8";
            std::string fourCC;
            for (int i = 0; i < 4; ++i)
            {
                const auto c = static_cast<char>((value >> (i * 8)) & 0xFF);
                if (c < 0x20 || c > 0x7E)
                    return std::format("format {}", value);
                fourCC += c;
            }
            return fourCC;
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

        // The first level of the game's cube into memory, 8 bits a channel whatever its format (D3DX reads and
        // converts it, DXT too). What failed otherwise.
        std::string ReadSource(IDirect3DDevice9* device, IDirect3DCubeTexture9* texture, SkyLogic::Cube& cube)
        {
            D3DSURFACE_DESC desc{};
            if (FAILED(texture->GetLevelDesc(0, &desc)) || desc.Width == 0)
                return "its size couldn't be read";
            cube.size = desc.Width;
            IDirect3DSurface9* copy = nullptr;
            auto hr = device->CreateOffscreenPlainSurface(desc.Width, desc.Width, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
                                                          &copy, nullptr);
            if (FAILED(hr))
                return std::format("no surface to read it into ({:#010x})", static_cast<std::uint32_t>(hr));
            std::string error;
            for (std::size_t face = 0; face < SkyLogic::FACE_COUNT && error.empty(); ++face)
            {
                IDirect3DSurface9* surface = nullptr;
                hr = texture->GetCubeMapSurface(static_cast<D3DCUBEMAP_FACES>(face), 0, &surface);
                if (FAILED(hr))
                {
                    error = std::format("face {} couldn't be got ({:#010x})", face, static_cast<std::uint32_t>(hr));
                    break;
                }
                hr = D3DXLoadSurfaceFromSurface(copy, nullptr, nullptr, surface, nullptr, nullptr, D3DX_FILTER_NONE, 0);
                surface->Release();
                D3DLOCKED_RECT rect{};
                if (FAILED(hr))
                    error = std::format("face {} couldn't be read ({:#010x})", face, static_cast<std::uint32_t>(hr));
                else if (FAILED(hr = copy->LockRect(&rect, nullptr, D3DLOCK_READONLY)))
                    error = std::format("face {} couldn't be locked ({:#010x})", face, static_cast<std::uint32_t>(hr));
                else
                {
                    auto& texels = cube.faces[face];
                    texels.resize(static_cast<std::size_t>(desc.Width) * desc.Width);
                    for (UINT y = 0; y < desc.Width; ++y)
                        std::memcpy(texels.data() + static_cast<std::size_t>(y) * desc.Width,
                                    static_cast<const std::uint8_t*>(rect.pBits) + static_cast<std::size_t>(y) * rect.Pitch,
                                    static_cast<std::size_t>(desc.Width) * sizeof(SkyLogic::Texel));
                    copy->UnlockRect();
                }
            }
            copy->Release();
            return error;
        }

        // Makes the entry's cube from the game's with the grade: every level and face on the CPU (a thread a face),
        // then into a cube of the mod's. What failed otherwise.
        std::string Bake(IDirect3DDevice9* device, Entry& entry, IDirect3DCubeTexture9* source,
                         const SkyLogic::Grade& grade)
        {
            const auto start = std::chrono::steady_clock::now();
            if (entry.sourceFrom != source || entry.source.empty())
            {
                entry.source.clear();
                entry.sourceFrom = nullptr;
                SkyLogic::Cube first;
                if (auto error = ReadSource(device, source, first); !error.empty())
                    return error;
                const auto levels = SkyLogic::GetLevelCount(first.size, source->GetLevelCount());
                entry.source.push_back(std::move(first));
                while (entry.source.size() < levels)
                    entry.source.push_back(SkyLogic::Halve(entry.source.back()));
                entry.sourceFrom = source;
                ReleaseBake(entry);
            }
            const auto size = entry.source.front().size;
            const auto levels = static_cast<UINT>(entry.source.size());
            if (!entry.baked)
            {
                const auto hr = device->CreateCubeTexture(size, levels, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
                                                          &entry.baked, nullptr);
                if (FAILED(hr))
                {
                    entry.baked = nullptr;
                    return std::format("no cube of {} could be made ({:#010x})", size, static_cast<std::uint32_t>(hr));
                }
            }

            // every face's levels, each face on a thread of its own
            std::array<std::vector<std::vector<SkyLogic::Texel>>, SkyLogic::FACE_COUNT> made;
            {
                std::vector<std::jthread> threads;
                for (std::size_t face = 0; face < SkyLogic::FACE_COUNT; ++face)
                    threads.emplace_back([&, face] {
                        made[face].resize(levels);
                        for (UINT level = 0; level < levels; ++level)
                        {
                            const auto& from = entry.source[level];
                            made[face][level].resize(static_cast<std::size_t>(from.size) * from.size);
                            SkyLogic::BakeFace(from, face, grade, made[face][level].data());
                        }
                    });
            }

            for (std::size_t face = 0; face < SkyLogic::FACE_COUNT; ++face)
                for (UINT level = 0; level < levels; ++level)
                {
                    const auto levelSize = entry.source[level].size;
                    D3DLOCKED_RECT rect{};
                    const auto hr = entry.baked->LockRect(static_cast<D3DCUBEMAP_FACES>(face), level, &rect, nullptr, 0);
                    if (FAILED(hr))
                        return std::format("the cube couldn't be written ({:#010x})", static_cast<std::uint32_t>(hr));
                    for (UINT y = 0; y < levelSize; ++y)
                        std::memcpy(static_cast<std::uint8_t*>(rect.pBits) + static_cast<std::size_t>(y) * rect.Pitch,
                                    made[face][level].data() + static_cast<std::size_t>(y) * levelSize,
                                    static_cast<std::size_t>(levelSize) * sizeof(SkyLogic::Texel));
                    entry.baked->UnlockRect(static_cast<D3DCUBEMAP_FACES>(face), level);
                }

            const auto middle = static_cast<std::size_t>(size / 2) * size + size / 2;
            entry.sourceTop = entry.source.front().faces[4][middle] & 0xFFFFFF;
            entry.bakedTop = made[4][0][middle] & 0xFFFFFF;
            entry.bakeMs = static_cast<std::uint32_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
            return {};
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
                    {
                        auto& entry = entries.emplace_back();
                        entry.slot = image.texture;
                        entry.name = image.name;
                        entry.material = image.material;
                        entry.techniqueSet = image.techniqueSet;
                    }
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
            const Entry* main = nullptr;
            for (auto& entry : entries)
            {
                entry.game = ReadSlot(entry.slot);
                if (!entry.game)
                    continue;
                entriesByGame.try_emplace(entry.game, &entry);
                const bool cube = entry.game->GetType() == D3DRTYPE_CUBETEXTURE;
                D3DSURFACE_DESC desc{};
                if (cube && SUCCEEDED(static_cast<IDirect3DCubeTexture9*>(entry.game)->GetLevelDesc(0, &desc)))
                {
                    ++status.cubes;
                    status.cubeList += std::format("{}{} ({}, {})", status.cubeList.empty() ? "" : "\n", entry.name,
                                                   desc.Width, FormatName(desc.Format));
                    // the sky itself: the largest cube
                    if (desc.Width > status.size)
                    {
                        main = &entry;
                        status.name = entry.name;
                        status.size = desc.Width;
                        status.format = FormatName(desc.Format);
                    }
                }
                if (!entry.logged)
                {
                    entry.logged = true;
                    LOG_INFO("Sky: image {} of material {} ({}), a {}", entry.name, entry.material, entry.techniqueSet,
                             cube ? "cube map" : "flat texture, left as it is");
                }
            }

            // nothing to draw: the game's own sky, and the memory back
            active = !SkyLogic::IsGameSky(grade) && !stopped;
            status.active = active;
            if (!active)
            {
                ReleaseBakes();
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
                if (entry.baked && entry.bakedFrom == sourceTexture && entry.bakedGrade == grade)
                {
                    ++status.baked;
                    continue;
                }
                if (entry.failedFrom == sourceTexture)
                {
                    ++status.failed;
                    if (status.error.empty())
                        status.error = std::format("{}: {}", entry.name, entry.error);
                    continue;
                }
                const auto error = Bake(device, entry, static_cast<IDirect3DCubeTexture9*>(sourceTexture), grade);
                if (error.empty())
                {
                    if (entry.bakedFrom != sourceTexture)
                        LOG_INFO("Sky: {} baked on the CPU ({} × {}, {} levels, {} ms)", entry.name,
                                 entry.source.front().size, entry.source.front().size, entry.source.size(),
                                 entry.bakeMs);
                    entry.bakedFrom = sourceTexture;
                    entry.bakedGrade = grade;
                    ++status.baked;
                }
                else
                {
                    LOG_ERROR("Sky: the bake of {} failed: {}", entry.name, error);
                    ReleaseBake(entry);
                    entry.failedFrom = sourceTexture;
                    entry.error = error;
                    ++status.failed;
                    if (status.error.empty())
                        status.error = std::format("{}: {}", entry.name, error);
                }
            }
            if (main && main->baked && main->bakedGrade)
            {
                status.sourceTop = main->sourceTop;
                status.bakedTop = main->bakedTop;
                status.bakeMs = main->bakeMs;
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
        if (!game || entriesByGame.empty() || GetCurrentThreadId() != renderThread)
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
        // (the baked cubes are in the managed pool and stay; what failed is tried again)
        for (auto& entry : entries)
            entry.failedFrom = nullptr;
    }

    void OnNewDevice()
    {
        ReleaseBakes();
        entriesByGame.clear();
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
    }

    Status GetStatus()
    {
        std::lock_guard lock(statusMutex);
        return lastStatus;
    }
}  // namespace IWXMVM::Components::Skybox
