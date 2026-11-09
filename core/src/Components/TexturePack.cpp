#include "StdInclude.hpp"
#include "TexturePack.hpp"

#include <condition_variable>
#include <numeric>
#include <deque>
#include <dxgi1_4.h>
#include <set>
#include <thread>

#include "D3D9.hpp"
#include "Events.hpp"
#include "Mod.hpp"
#include "Utilities/PathUtils.hpp"

namespace IWXMVM::Components::TexturePack
{
    namespace
    {
        using TextureLogic::Group;

        // an image of the pack matched on this map
        struct Entry
        {
            std::string name;
            IDirect3DBaseTexture9* const* slot = nullptr;  // where the game keeps its texture
            std::filesystem::path file;
            std::uint64_t fileBytes = 0;
            Group group = Group::Maps;
            // the render thread's
            IDirect3DTexture9* texture = nullptr;
            std::uint64_t bytes = 0;
            std::uint64_t lastDrawn = 0;
            std::uint64_t retryAfter = 0;  // a frame, after the budget refused it
            std::chrono::steady_clock::time_point askedAt;  // when it was first drawn without its texture
            bool requested = false;     // its file asked of the worker
            bool prioritized = false;   // asked for as drawn, ahead of the rest
            bool drawnWaiting = false;  // drawn, its texture not made yet
            bool failed = false;
            bool heldBack = false;      // the harness's choice: the game's texture stays
        };

        // A match of the map's images with the packs, made by the worker
        struct Match
        {
            std::vector<Entry> entries;
            TextureLogic::Status status;  // its fixed part
            std::array<std::uint32_t, 3> groups{}, groupsOff{};
            std::uint64_t packBytes = 0;
            float ms = 0.0f;
        };

        std::atomic<bool> on = false;
        std::atomic<bool> inDemo = false;
        std::atomic<bool> reloadRequested = false;
        std::atomic<bool> forgetMatch = false;

        std::mutex settingsMutex;
        std::optional<std::filesystem::path> folderOverride;
        std::optional<std::uint64_t> budgetOverride;
        // the budget override gone: the card's is measured again
        std::atomic<bool> budgetAsked = false;
        std::optional<std::set<Group>> groupsOffOverride;

        // between the worker and the render thread
        struct Read
        {
            std::uint32_t generation = 0;
            std::size_t index = 0;
            std::filesystem::path file;
            bool prefetch = false;  // read ahead, not drawn yet
        };
        struct Ready
        {
            std::uint32_t generation = 0;
            std::size_t index = 0;
            std::vector<std::uint8_t> data;
            bool prefetch = false;
        };
        std::mutex queueMutex;
        std::condition_variable queueSignal;
        std::deque<Read> reads;
        std::deque<Ready> ready;
        std::uint64_t readyBytes = 0;
        // files read ahead of the render thread, in the game's own memory: a few of the largest at most
        constexpr std::uint64_t MAX_READY_BYTES = 96ull << 20;
        std::optional<Match> pendingMatch;
        // matches the worker started, and those done with (made and taken by the render thread, dropped, or none to
        // make): while they differ a match is under way (IsMatching)
        std::atomic<std::uint32_t> matchesStarted = 0, matchesSettled = 0;
        bool unloadRequested = false;
        std::atomic<bool> stopping = false;
        std::thread worker;

        // the render thread's
        std::vector<Entry> entries;
        std::uint32_t generation = 0;
        std::unordered_map<IDirect3DBaseTexture9*, std::size_t> byGameTexture;
        std::vector<Read> requested;
        std::uint64_t frame = 1;
        std::uint64_t inUse = 0;
        std::uint64_t budget = 0;
        std::uint32_t frameSwaps = 0;
        std::uint32_t frameCalls = 0;
        std::uint32_t evicted = 0;
        std::uint32_t refused = 0;
        bool budgetDue = false;
        // the map the budget was measured on: a match again on it keeps it (the textures just let go are still being
        // freed by the driver, and would make it look smaller)
        std::string budgetMap;
        // the next image read ahead (the rest of the map's pack, while the budget has room), and the bytes of those
        // asked for and not made yet
        std::size_t prefetchNext = 0;
        std::uint64_t pendingBytes = 0;
        // the read-ahead stopped where the budget has no room for the next
        bool prefetchFull = false;
        // the order they are read ahead in: the map's, then the players', then the weapons' (a pack of the guns and
        // hands has every gun of the game, most never drawn)
        std::vector<std::size_t> prefetchOrder;
        // how long images waited for their textures since the match
        double waitedMs = 0.0, mostWaitedMs = 0.0, mostMakeMs = 0.0;
        std::uint32_t waits = 0;

        // what the UI and the harness read
        std::mutex statsMutex;
        Stats stats;
        TextureLogic::Status status;

        // making textures takes at most this much of a frame (one at least)
        constexpr float MAKE_MS_PER_FRAME = 4.0f;
        constexpr std::uint32_t MAKE_PER_FRAME = 24;
        // a texture the budget refused is asked for again after this many frames
        constexpr std::uint64_t RETRY_FRAMES = 600;
        // files read ahead waiting at once (to be read or made)
        constexpr std::size_t PREFETCH_QUEUE = 8;

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

        // the bytes of a texture's levels
        std::uint64_t GetBytes(IDirect3DTexture9* texture)
        {
            std::uint64_t bytes = 0;
            for (DWORD level = 0; level < texture->GetLevelCount(); ++level)
            {
                D3DSURFACE_DESC desc{};
                if (FAILED(texture->GetLevelDesc(level, &desc)))
                    break;
                const auto format = desc.Format == D3DFMT_DXT1   ? TextureLogic::FORMAT_DXT1
                                    : desc.Format == D3DFMT_DXT3 ? TextureLogic::FORMAT_DXT3
                                    : desc.Format == D3DFMT_DXT5 ? TextureLogic::FORMAT_DXT5
                                                                 : std::uint8_t{0};
                bytes += TextureLogic::GetLevelBytes(format, desc.Width, desc.Height);
            }
            return bytes;
        }

        // What the card has free for this process now: the budget Windows gives it less what it uses (DXGI 1.4), on the
        // adapter of the game's device; none when it can't be read
        std::optional<std::uint64_t> QueryFreeVideoMemory(IDirect3DDevice9* device)
        {
            IDirect3D9* d3d = nullptr;
            D3DDEVICE_CREATION_PARAMETERS parameters{};
            D3DADAPTER_IDENTIFIER9 identifier{};
            if (FAILED(device->GetDirect3D(&d3d)) || !d3d)
                return std::nullopt;
            const bool known = SUCCEEDED(device->GetCreationParameters(&parameters)) &&
                               SUCCEEDED(d3d->GetAdapterIdentifier(parameters.AdapterOrdinal, 0, &identifier));
            d3d->Release();
            if (!known)
                return std::nullopt;

            static const auto createFactory = [] {
                const auto dxgi = LoadLibraryW(L"dxgi.dll");
                return dxgi ? reinterpret_cast<HRESULT(WINAPI*)(REFIID, void**)>(
                                  GetProcAddress(dxgi, "CreateDXGIFactory1"))
                            : nullptr;
            }();
            IDXGIFactory1* factory = nullptr;
            if (!createFactory || FAILED(createFactory(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))))
                return std::nullopt;
            std::optional<std::uint64_t> free;
            IDXGIAdapter1* adapter = nullptr;
            for (UINT i = 0; !free && factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
            {
                DXGI_ADAPTER_DESC1 desc{};
                IDXGIAdapter3* adapter3 = nullptr;
                if (SUCCEEDED(adapter->GetDesc1(&desc)) && desc.VendorId == identifier.VendorId &&
                    desc.DeviceId == identifier.DeviceId &&
                    SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&adapter3))))
                {
                    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
                    if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
                        free = info.Budget > info.CurrentUsage ? info.Budget - info.CurrentUsage : 0;
                    adapter3->Release();
                }
                adapter->Release();
            }
            factory->Release();
            return free;
        }

        std::vector<TextureLogic::Folder> ReadPacks(const std::filesystem::path& folder)
        {
            std::vector<TextureLogic::Folder> packs;
            const auto read = [&](const std::filesystem::path& path) {
                std::ifstream file(path / TextureLogic::INDEX_FILE);
                if (!file)
                    return;
                try
                {
                    packs.push_back({path, TextureLogic::IndexFromJson(nlohmann::json::parse(file))});
                }
                catch (const std::exception& e)
                {
                    LOG_WARN("Texture pack: {} doesn't read: {}", (path / TextureLogic::INDEX_FILE).string(), e.what());
                }
            };
            std::error_code error;
            read(folder / TextureLogic::BUILT_FOLDER);
            for (const auto& imported : std::filesystem::directory_iterator(folder / TextureLogic::IMPORTED_FOLDER, error))
                if (imported.is_directory())
                    read(imported.path());
            return packs;
        }

        TextureLogic::Use ReadUse(const std::filesystem::path& folder)
        {
            std::ifstream file(folder / TextureLogic::USE_FILE);
            if (!file)
                return {};
            try
            {
                return TextureLogic::UseFromJson(nlohmann::json::parse(file));
            }
            catch (const std::exception&)
            {
                return {};
            }
        }

        // the map's images with the packs (the game's on its main thread, the rest here)
        std::optional<Match> MakeMatch()
        {
            const auto started = std::chrono::steady_clock::now();
            const auto images = Mod::GetGameInterface()->GetPackImages();
            if (!images)
                return std::nullopt;
            const auto folder = GetFolder();
            const auto packs = ReadPacks(folder);
            auto use = ReadUse(folder);
            {
                std::lock_guard lock(settingsMutex);
                if (groupsOffOverride)
                    use.groupsOff = *groupsOffOverride;
            }
            const auto sources = TextureLogic::Resolve(packs, use, images->zones);

            Match match;
            std::set<std::string> seen, partsUsed, importedUsed;
            for (const auto& image : images->images)
            {
                const auto source = sources.find(image.name);
                if (source == sources.end() || !seen.insert(image.name).second)
                    continue;
                const auto group = static_cast<std::size_t>(image.group);
                ++match.groups[group];
                if (use.groupsOff.contains(image.group))
                {
                    ++match.groupsOff[group];
                    continue;
                }
                Entry entry;
                entry.name = image.name;
                entry.slot = image.texture;
                entry.file = source->second.file;
                entry.group = image.group;
                std::error_code error;
                entry.fileBytes = std::filesystem::file_size(entry.file, error);
                match.packBytes += entry.fileBytes;
                if (!source->second.pack.empty())
                    importedUsed.insert(source->second.pack);
                else
                    for (const auto& pack : packs)
                        if (!pack.index.imported)
                            if (const auto built = pack.index.images.find(image.name); built != pack.index.images.end())
                            {
                                match.status.repair |= built->second.build < TextureLogic::BUILD_VERSION;
                                for (const auto& part : built->second.parts)
                                    if (!use.partsOff.contains(part) &&
                                        std::ranges::find(images->zones, part) != images->zones.end())
                                        partsUsed.insert(part);
                            }
                match.entries.push_back(std::move(entry));
            }

            // the line: the map's part first, the guns, other built parts, then the imported packs
            auto& line = match.status;
            line.map = TextureLogic::GetPartName(images->map);
            std::vector<std::string> parts(partsUsed.begin(), partsUsed.end());
            std::ranges::sort(parts, [&](const std::string& a, const std::string& b) {
                return (a == images->map) != (b == images->map) ? a == images->map : TextureLogic::ListsBefore(a, b);
            });
            for (const auto& pack : packs)
                if (!pack.index.imported && pack.index.parts.contains(images->map))
                    line.mapHasPack = true;
            for (const auto& part : parts)
                for (const auto& pack : packs)
                    if (const auto found = pack.index.parts.find(part); !pack.index.imported && found != pack.index.parts.end())
                        line.packs.push_back(std::format("{} {}x", TextureLogic::GetPartName(part), found->second.scale));
            for (const auto& name : importedUsed)
            {
                line.packs.push_back(name + " (imported)");
                line.mapHasPack = true;
            }
            line.groupsOff = TextureLogic::DescribeGroupsOff(use);
            match.ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - started).count();
            LOG_INFO("Texture pack: {} of {} images matched on {} ({} groups off) in {:.0f} ms", match.entries.size(),
                     images->images.size(), images->map, line.groupsOff.empty() ? "no" : line.groupsOff, match.ms);
            return match;
        }

        void RunWorker()
        {
            std::string matchedKey, lastKey;
            auto keyChanged = std::chrono::steady_clock::now();
            while (!stopping)
            {
                // read the files asked for, a few ahead of the render thread
                while (!stopping)
                {
                    Read read;
                    {
                        std::unique_lock lock(queueMutex);
                        queueSignal.wait_for(lock, std::chrono::milliseconds(250), [] {
                            return stopping.load() || (!reads.empty() && readyBytes < MAX_READY_BYTES);
                        });
                        if (reads.empty() || readyBytes >= MAX_READY_BYTES)
                            break;
                        read = std::move(reads.front());
                        reads.pop_front();
                    }
                    Ready done{read.generation, read.index, {}, read.prefetch};
                    if (std::ifstream file{read.file, std::ios::binary | std::ios::ate})
                    {
                        done.data.resize(static_cast<std::size_t>(file.tellg()));
                        file.seekg(0);
                        file.read(reinterpret_cast<char*>(done.data.data()), done.data.size());
                    }
                    std::lock_guard lock(queueMutex);
                    readyBytes += done.data.size();
                    ready.push_back(std::move(done));
                }

                // a match once the map is in and its zones have settled for a second (a character zone loading
                // during the demo matches again); none outside a demo
                if (forgetMatch.exchange(false))
                    matchedKey.clear();
                if (!inDemo)
                    continue;
                const auto key = Mod::GetGameInterface()->GetLoadedZonesKey();
                if (key != lastKey)
                {
                    lastKey = key;
                    keyChanged = std::chrono::steady_clock::now();
                }
                if ((key == matchedKey && !reloadRequested) ||
                    std::chrono::steady_clock::now() - keyChanged < std::chrono::seconds(1))
                    continue;
                reloadRequested = false;
                ++matchesStarted;
                if (auto match = MakeMatch())
                {
                    matchedKey = key;
                    std::lock_guard lock(queueMutex);
                    if (pendingMatch)
                        ++matchesSettled;
                    pendingMatch = std::move(match);
                }
                else
                    ++matchesSettled;
            }
        }

        void Release(Entry& entry)
        {
            if (!entry.texture)
                return;
            entry.texture->Release();
            entry.texture = nullptr;
            inUse -= std::min(inUse, entry.bytes);
            entry.bytes = 0;
        }

        void TakeMatch(IDirect3DDevice9* device, Match match)
        {
            // the textures made already stay with their images
            std::unordered_map<std::string, Entry*> old;
            for (auto& entry : entries)
                old[entry.name] = &entry;
            for (auto& entry : match.entries)
                if (const auto found = old.find(entry.name); found != old.end() && found->second->file == entry.file)
                {
                    std::swap(entry.texture, found->second->texture);
                    entry.bytes = found->second->bytes;
                    entry.lastDrawn = found->second->lastDrawn;
                    found->second->bytes = 0;
                }
            for (auto& entry : entries)
                Release(entry);
            entries = std::move(match.entries);
            ++generation;
            inUse = 0;
            for (const auto& entry : entries)
                inUse += entry.bytes;
            evicted = refused = 0;
            budgetDue = budgetDue || match.status.map != budgetMap;
            budgetMap = match.status.map;
            prefetchNext = 0;
            prefetchOrder.resize(entries.size());
            std::iota(prefetchOrder.begin(), prefetchOrder.end(), std::size_t{0});
            std::ranges::stable_sort(prefetchOrder, {}, [](std::size_t i) { return static_cast<int>(entries[i].group); });
            waitedMs = mostWaitedMs = mostMakeMs = 0.0;
            waits = 0;

            std::lock_guard lock(statsMutex);
            status = std::move(match.status);
            stats.groups = match.groups;
            stats.groupsOff = match.groupsOff;
            stats.packBytes = match.packBytes;
            stats.matchMs = match.ms;
            ++stats.matches;
        }

        void Unload()
        {
            for (auto& entry : entries)
                Release(entry);
            entries.clear();
            prefetchOrder.clear();
            prefetchNext = 0;
            budgetMap.clear();
            byGameTexture.clear();
            ++generation;
            inUse = 0;
            std::lock_guard lock(statsMutex);
            status = {};
            stats.groups = stats.groupsOff = {};
            stats.packBytes = 0;
        }

        // the files read since the last frame, made into textures within the frame's time, room made in the budget
        void MakeTextures(IDirect3DDevice9* device, Stats& frameStats)
        {
            const auto started = std::chrono::steady_clock::now();
            std::uint32_t made = 0;
            // a budget smaller than what is held: the least recently drawn go down to it (never one drawn in the last
            // frame)
            if (inUse > budget)
            {
                std::vector<std::size_t> held;
                for (std::size_t i = 0; i < entries.size(); ++i)
                    if (entries[i].texture && entries[i].lastDrawn + 1 < frame)
                        held.push_back(i);
                std::ranges::sort(held, {}, [](std::size_t i) { return entries[i].lastDrawn; });
                for (std::size_t i = 0; i < held.size() && inUse > budget; ++i)
                {
                    Release(entries[held[i]]);
                    ++evicted;
                }
            }
            while (made < MAKE_PER_FRAME &&
                   (made == 0 || std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - started)
                                         .count() < MAKE_MS_PER_FRAME))
            {
                Ready item;
                {
                    std::lock_guard lock(queueMutex);
                    if (ready.empty())
                        break;
                    // what is drawn before what is read ahead
                    auto next = std::ranges::find(ready, false, &Ready::prefetch);
                    if (next == ready.end())
                        next = ready.begin();
                    item = std::move(*next);
                    ready.erase(next);
                    readyBytes -= std::min(readyBytes, std::uint64_t{item.data.size()});
                }
                queueSignal.notify_one();
                if (item.generation != generation || item.index >= entries.size())
                    continue;
                auto& entry = entries[item.index];
                entry.requested = entry.prioritized = false;
                if (entry.texture)
                    continue;
                if (item.data.empty())
                {
                    entry.failed = true;
                    continue;
                }
                // room in the budget: the least recently drawn go, never one drawn in the last frame
                const std::uint64_t needed = item.data.size() - std::min<std::size_t>(item.data.size(), 128);
                // (read ahead and not drawn yet: only into room there is)
                if (!entry.drawnWaiting && inUse + needed > budget - budget / 10)
                    continue;
                std::vector<TextureLogic::Resident> residents;
                for (std::size_t i = 0; i < entries.size(); ++i)
                    if (entries[i].texture)
                        residents.push_back({i, entries[i].bytes, entries[i].lastDrawn});
                const auto evictions = TextureLogic::ChooseEvictions(std::move(residents), inUse, needed, budget, frame - 1);
                if (!evictions)
                {
                    entry.retryAfter = frame + RETRY_FRAMES;
                    ++refused;
                    continue;
                }
                for (const auto index : *evictions)
                {
                    Release(entries[index]);
                    ++evicted;
                }
                // the file's own levels and format, nothing filtered, in the card's memory only
                const auto result = D3DXCreateTextureFromFileInMemoryEx(
                    device, item.data.data(), static_cast<UINT>(item.data.size()), D3DX_DEFAULT_NONPOW2,
                    D3DX_DEFAULT_NONPOW2, D3DX_FROM_FILE, 0, D3DFMT_FROM_FILE, D3DPOOL_DEFAULT, D3DX_FILTER_NONE,
                    D3DX_FILTER_NONE, 0, nullptr, nullptr, &entry.texture);
                if (FAILED(result))
                {
                    entry.texture = nullptr;
                    entry.failed = true;
                    LOG_WARN("Texture pack: {} didn't load ({:08X})", entry.file.string(),
                             static_cast<std::uint32_t>(result));
                    continue;
                }
                entry.bytes = GetBytes(entry.texture);
                inUse += entry.bytes;
                ++made;
                if (entry.drawnWaiting)
                {
                    const double waited =
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - entry.askedAt)
                            .count();
                    waitedMs += waited;
                    mostWaitedMs = std::max(mostWaitedMs, waited);
                    ++waits;
                    entry.drawnWaiting = false;
                }
            }
            frameStats.madeLastFrame = made;
            if (made)
            {
                frameStats.lastMakeMs =
                    std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - started).count();
                mostMakeMs = std::max(mostMakeMs, double(frameStats.lastMakeMs));
            }
            frameStats.mostMakeMs = static_cast<float>(mostMakeMs);
            frameStats.meanWaitMs = waits ? static_cast<float>(waitedMs / waits) : 0.0f;
            frameStats.drawnWaits = waits;
            frameStats.mostWaitMs = static_cast<float>(mostWaitedMs);
        }

        void OnFrame(IDirect3DDevice9* device)
        {
            ++frame;
            inDemo = Mod::GetGameInterface()->GetGameState() == Types::GameState::InDemo;

            std::optional<Match> match;
            bool unload = false;
            {
                std::lock_guard lock(queueMutex);
                match = std::exchange(pendingMatch, std::nullopt);
                unload = std::exchange(unloadRequested, false);
            }
            if (unload)
                Unload();
            if (match)
            {
                TakeMatch(device, std::move(*match));
                ++matchesSettled;
            }

            Stats frameStats;
            {
                std::lock_guard lock(statsMutex);
                frameStats = stats;
            }
            if (budgetDue || budgetAsked.exchange(false))
            {
                // what the card has free now, the pack's own textures counted as the pack's
                const auto free = QueryFreeVideoMemory(device);
                frameStats.freeAtLoad = free.value_or(0);
                budget = TextureLogic::GetBudget(free ? *free + inUse : device->GetAvailableTextureMem() / 2);
                budgetDue = false;
            }
            {
                std::lock_guard lock(settingsMutex);
                if (budgetOverride)
                    budget = *budgetOverride;
            }

            // the files the last frame drew for the first time, to the worker ahead of the rest; then, while the
            // budget has room, the rest of the map's pack read ahead a few at a time, so a camera turning finds it made
            {
                std::lock_guard lock(queueMutex);
                for (auto read = requested.rbegin(); read != requested.rend(); ++read)
                    reads.push_front(std::move(*read));
                if (on && reads.size() + ready.size() < PREFETCH_QUEUE)
                {
                    std::uint64_t asked = pendingBytes;
                    std::size_t queued = reads.size() + ready.size();
                    prefetchFull = false;
                    for (; prefetchNext < prefetchOrder.size() && queued < PREFETCH_QUEUE; ++prefetchNext)
                    {
                        const auto index = prefetchOrder[prefetchNext];
                        auto& entry = entries[index];
                        if (entry.texture || entry.requested || entry.failed || frame < entry.retryAfter)
                            continue;
                        // (a tenth of the budget kept for what is drawn)
                        if (inUse + asked + entry.fileBytes > budget - budget / 10)
                        {
                            prefetchFull = true;
                            break;
                        }
                        entry.requested = true;
                        asked += entry.fileBytes;
                        reads.push_back({generation, index, entry.file, true});
                        ++queued;
                    }
                    // round again now and then for those the budget or a reset turned away
                    if (prefetchNext >= prefetchOrder.size() && frame % 600 == 0)
                        prefetchNext = 0;
                }
            }
            if (!requested.empty())
            {
                requested.clear();
                queueSignal.notify_one();
            }
            if (on)
                queueSignal.notify_one();
            frameStats.madeLastFrame = 0;
            if (on)
                MakeTextures(device, frameStats);

            // the textures the images have now (none while a demo loads)
            byGameTexture.clear();
            frameStats.images = static_cast<std::uint32_t>(entries.size());
            frameStats.made = frameStats.waiting = frameStats.failed = 0;
            frameStats.drawnLeft = frameStats.aheadLeft = 0;
            const bool moreAhead = !prefetchFull && prefetchNext < prefetchOrder.size();
            pendingBytes = 0;
            for (std::size_t i = 0; i < entries.size(); ++i)
            {
                const auto& entry = entries[i];
                frameStats.made += entry.texture != nullptr;
                frameStats.waiting += entry.requested;
                if (entry.requested)
                    pendingBytes += entry.fileBytes;
                frameStats.failed += entry.failed;
                if (!entry.texture && !entry.failed && frame >= entry.retryAfter)
                {
                    frameStats.drawnLeft += entry.drawnWaiting;
                    frameStats.aheadLeft += !entry.drawnWaiting && (entry.requested || moreAhead);
                }
                if (const auto texture = inDemo ? ReadSlot(entry.slot) : nullptr)
                    byGameTexture[texture] = i;
            }
            frameStats.swaps = std::exchange(frameSwaps, 0);
            frameStats.calls = std::exchange(frameCalls, 0);
            frameStats.watched = static_cast<std::uint32_t>(byGameTexture.size());
            frameStats.inUse = inUse;
            frameStats.budget = budget;
            frameStats.evicted = evicted;
            frameStats.refused = refused;

            std::lock_guard lock(statsMutex);
            stats = frameStats;
            status.on = on;
            status.images = frameStats.images;
            status.drawn = frameStats.made;
            status.waiting = frameStats.waiting;
            status.bytes = frameStats.packBytes;
            status.inUse = inUse;
            status.budget = budget;
        }
    }  // namespace

    std::filesystem::path GetFolder()
    {
        std::lock_guard lock(settingsMutex);
        return folderOverride.value_or(PathUtils::GetModPath() / "textures");
    }

    void SetFolder(std::optional<std::filesystem::path> folder)
    {
        {
            std::lock_guard lock(settingsMutex);
            folderOverride = std::move(folder);
        }
        Reload();
    }

    void Reload()
    {
        reloadRequested = true;
    }

    bool IsMatching()
    {
        return matchesStarted != matchesSettled;
    }

    void SetOn(bool value)
    {
        on = value;
    }

    bool IsOn()
    {
        return on;
    }

    void SetBudgetOverride(std::optional<std::uint64_t> bytes)
    {
        std::lock_guard lock(settingsMutex);
        budgetOverride = bytes;
        budgetAsked = !bytes;
    }

    void SetGroupsOffOverride(std::optional<std::set<TextureLogic::Group>> groups)
    {
        {
            std::lock_guard lock(settingsMutex);
            groupsOffOverride = std::move(groups);
        }
        Reload();
    }

    std::vector<std::string> GetDrawnImages()
    {
        std::vector<std::string> names;
        for (const auto& entry : entries)
            if (entry.lastDrawn + 1 >= frame)
                names.push_back(entry.name);
        return names;
    }

    void SetSwappedImages(std::optional<std::set<std::string>> names)
    {
        for (auto& entry : entries)
            entry.heldBack = names && !names->contains(entry.name);
    }

    std::vector<std::vector<std::string>> GetSharedImages()
    {
        std::unordered_map<IDirect3DBaseTexture9*, std::vector<std::string>> byTexture;
        for (const auto& entry : entries)
            if (const auto texture = ReadSlot(entry.slot))
                byTexture[texture].push_back(entry.name);
        std::vector<std::vector<std::string>> shared;
        for (auto& [texture, names] : byTexture)
            if (names.size() > 1)
                shared.push_back(std::move(names));
        return shared;
    }

    std::filesystem::path FindFile(std::string_view image)
    {
        std::string name(image);
        std::ranges::transform(name, name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const auto found = std::ranges::find(entries, name, &Entry::name);
        return found == entries.end() ? std::filesystem::path{} : found->file;
    }

    std::uint32_t GetGeneration()
    {
        return generation;
    }

    IDirect3DBaseTexture9* Swap(IDirect3DBaseTexture9* texture)
    {
        ++frameCalls;
        if (!on || !texture || byGameTexture.empty())
            return texture;
        const auto found = byGameTexture.find(texture);
        if (found == byGameTexture.end())
            return texture;
        auto& entry = entries[found->second];
        entry.lastDrawn = frame;
        if (entry.heldBack)
            return texture;
        if (entry.texture)
        {
            ++frameSwaps;
            return entry.texture;
        }
        if (!entry.drawnWaiting)
        {
            entry.drawnWaiting = true;
            entry.askedAt = std::chrono::steady_clock::now();
        }
        // (asked for again ahead of the rest when it was only read ahead)
        if (!entry.prioritized && !entry.failed && frame >= entry.retryAfter)
        {
            entry.requested = entry.prioritized = true;
            requested.push_back({generation, found->second, entry.file});
        }
        return texture;
    }

    void OnReset()
    {
        byGameTexture.clear();
        for (auto& entry : entries)
        {
            Release(entry);
            entry.drawnWaiting = false;
        }
        prefetchNext = 0;
        prefetchFull = false;
    }

    void Install()
    {
        Events::RegisterListener(EventType::OnFrame, [] {
            if (const auto device = D3D9::GetDevice())
                OnFrame(device);
        });
        // a new demo: its map is matched once it is in
        Events::RegisterListener(EventType::PreDemoLoad, [] {
            forgetMatch = true;
            std::lock_guard lock(queueMutex);
            if (pendingMatch)
                ++matchesSettled;
            pendingMatch.reset();
            unloadRequested = true;
        });
        if (Mod::GetGameInterface()->GetSupportedFeatures() & Types::Features_TexturePack)
            worker = std::thread(RunWorker);
    }

    void Shutdown()
    {
        stopping = true;
        queueSignal.notify_all();
        if (worker.joinable())
            worker.join();
        on = false;
        Unload();
    }

    Stats GetStats()
    {
        std::lock_guard lock(statsMutex);
        return stats;
    }

    RestartLogic::Load GetLoad()
    {
        std::lock_guard lock(statsMutex);
        return {.on = on && stats.images > 0, .drawn = stats.drawnLeft, .ahead = stats.aheadLeft};
    }

    TextureLogic::Status GetStatus()
    {
        std::lock_guard lock(statsMutex);
        return status;
    }
}  // namespace IWXMVM::Components::TexturePack
