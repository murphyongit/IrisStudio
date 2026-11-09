#include "StdInclude.hpp"
#include "AnimationStore.hpp"

#include <deque>
#include <malloc.h>
#include <mutex>
#include <set>
#include <unordered_map>

#include "ContentPacks.hpp"
#include "Functions.hpp"
#include "Components/Actors.hpp"
#include "Components/AnimationPackLogic.hpp"
#include "Utilities/Inflate.hpp"
#include "Utilities/PathUtils.hpp"

namespace IWXMVM::T5::AnimationStore
{
    using namespace Structures;
    namespace Pack = AnimationPackLogic;

    // zeros after an animation's memory, should anything read a little past the end of its last piece (in the game's
    // zones more of the zone follows)
    inline constexpr std::size_t PADDING = 16;
    // the frames an animation unloaded is kept before it is freed: the game's other threads work out the skeletons of a
    // frame's models (with their animations) while the main thread goes on
    inline constexpr std::uint64_t RETIRE_FRAMES = 4;
    // what is loaded a frame of what the scene's clips name, and of what previews asked for
    inline constexpr auto WANTED_TIME = std::chrono::milliseconds(4);
    inline constexpr std::size_t REQUESTS_A_FRAME = 4;
    inline constexpr std::size_t MAX_REQUESTS = 8;

    struct Loaded
    {
        std::string key;
        std::uint8_t* memory = nullptr;
        std::uint32_t bytes = 0;  // laid out
        // (guarded by the state's mutex)
        std::uint32_t references = 0;
        std::uint64_t lastUsed = 0;
        std::uint64_t idleSince = 0;  // the frame its references last went to none
        std::uint64_t retiredAt = 0;  // the frame it was unloaded
        bool wanted = false;

        Loaded() = default;
        Loaded(const Loaded&) = delete;
        Loaded& operator=(const Loaded&) = delete;
        ~Loaded()
        {
            if (memory)
                _aligned_free(memory);
        }

        const XAnimParts* Parts() const
        {
            return reinterpret_cast<const XAnimParts*>(memory);
        }
    };

    struct State
    {
        std::mutex mutex;
        // the pack and its index, with the file's size and time it was read at
        std::filesystem::path path;
        std::uintmax_t indexedSize = 0;
        std::filesystem::file_time_type indexedTime{};
        std::unordered_map<std::string, Pack::IndexEntry> index;
        bool ready = false;
        std::string error;
        bool ignored = false;
        bool preferStore = false;
        bool unloaded = false;
        std::uint32_t generation = 0;

        std::unordered_map<std::string, std::unique_ptr<Loaded>> loaded;
        std::vector<std::unique_ptr<Loaded>> retired;
        std::deque<std::string> requests;
        std::unordered_map<std::string, std::string> failed;  // why each didn't load, until the next demo
        std::uint64_t budget = Pack::DEFAULT_BUDGET;
        std::uint64_t frame = 0;
        std::uint64_t uses = 0;
        bool changed = true;  // what is loaded, since the core was told

        std::uint64_t loads = 0, loadMicroseconds = 0, unloads = 0, frees = 0, failures = 0;

        // the script strings of the bones' and notetracks' names (SL_GetString adds a reference each time it is
        // called), by the demo; main thread
        std::unordered_map<std::string, std::uint16_t> scriptStrings;
    };

    // Never freed: the actors' references may outlive the mod's other statics as it unloads
    State& GetState()
    {
        static auto* state = new State();
        return *state;
    }

    bool IsUsable(const State& state)
    {
        return state.ready && !state.ignored && !state.unloaded;
    }

    // the name the game has an animation by (a copy that differs is the name and its zone: "name (zone)")
    std::string_view GetAssetName(std::string_view key)
    {
        return key.substr(0, key.find(' '));
    }

    // whether an animation is the store's to play rather than the game's copy (with the state's mutex held)
    bool IsStoresOwn(const State& state, std::string_view key)
    {
        return GetAssetName(key).size() != key.size() || state.preferStore;
    }

    // a reference to one, with the state's mutex held
    Reference MakeReference(State& state, Loaded& loaded)
    {
        ++loaded.references;
        loaded.lastUsed = ++state.uses;
        return Reference(&loaded);
    }

    Reference::Reference(Loaded* loaded) : loaded(loaded)
    {
    }

    Reference::Reference(Reference&& other) noexcept : loaded(std::exchange(other.loaded, nullptr))
    {
    }

    Reference& Reference::operator=(Reference&& other) noexcept
    {
        if (this != &other)
        {
            Drop();
            loaded = std::exchange(other.loaded, nullptr);
        }
        return *this;
    }

    Reference::~Reference()
    {
        Drop();
    }

    void Reference::Drop()
    {
        if (!loaded)
            return;
        auto& state = GetState();
        std::lock_guard lock(state.mutex);
        if (--loaded->references == 0)
            loaded->idleSince = state.frame;
        loaded = nullptr;
    }

    const XAnimParts* Reference::Parts() const
    {
        return loaded ? loaded->Parts() : nullptr;
    }

    std::string_view Reference::Key() const
    {
        return loaded ? std::string_view(loaded->key) : std::string_view();
    }

    std::uint16_t GetScriptString(const std::string& name)
    {
        auto& strings = GetState().scriptStrings;
        auto it = strings.find(name);
        if (it == strings.end())
            it = strings.emplace(name, Functions::SL_GetString(name.c_str())).first;
        return it->second;
    }

    std::optional<std::vector<std::uint8_t>> ReadInflated(const std::filesystem::path& path,
                                                          const Pack::IndexEntry& entry)
    {
        std::ifstream file(path, std::ios::binary);
        std::vector<std::uint8_t> packed(entry.size);
        if (!file.seekg(entry.offset) || !file.read(reinterpret_cast<char*>(packed.data()), entry.size))
            return std::nullopt;
        return Inflate::Zlib(packed, entry.inflated);
    }

    // Loads an animation from the pack into memory of its own (main thread, without the mutex held)
    std::unique_ptr<Loaded> LoadFromPack(const std::string& key, std::string& why)
    {
        auto& state = GetState();
        Pack::IndexEntry entry;
        std::filesystem::path path;
        {
            std::lock_guard lock(state.mutex);
            const auto it = state.index.find(key);
            if (it == state.index.end())
            {
                why = "the pack hasn't it";
                return nullptr;
            }
            entry = it->second;
            path = state.path;
        }
        const auto inflated = ReadInflated(path, entry);
        if (!inflated)
        {
            why = "its entry doesn't inflate";
            return nullptr;
        }
        const auto read = Pack::ReadEntry(*inflated);
        if (!read)
        {
            why = "its entry doesn't read";
            return nullptr;
        }
        const auto layout = Pack::Plan(read->parts, &why);
        if (!layout)
            return nullptr;
        if (layout->name != GetAssetName(key))
        {
            why = "its entry is " + layout->name;
            return nullptr;
        }
        auto loaded = std::make_unique<Loaded>();
        loaded->key = key;
        loaded->memory = static_cast<std::uint8_t*>(_aligned_malloc(layout->size + PADDING, Pack::MEMORY_ALIGNMENT));
        if (!loaded->memory)
        {
            why = "there's no memory for it";
            return nullptr;
        }
        std::memset(loaded->memory + layout->size, 0, PADDING);
        const auto& strings = read->strings;
        if (!Pack::Write(*layout, read->parts, strings.size(), loaded->memory,
                         [&](std::uint16_t index) { return GetScriptString(strings[index]); }))
        {
            why = "a name of its bones or notetracks isn't in its entry";
            return nullptr;
        }
        loaded->bytes = layout->size;
        return loaded;
    }

    void Open()
    {
        auto& state = GetState();
        const auto path = PathUtils::GetModPath() / "animations.pack";
        std::error_code error;
        const auto size = std::filesystem::file_size(path, error);
        const auto time = error ? std::filesystem::file_time_type{} : std::filesystem::last_write_time(path, error);
        {
            std::lock_guard lock(state.mutex);
            if (state.unloaded ||
                (!error && path == state.path && size == state.indexedSize && time == state.indexedTime))
                return;
        }

        std::unordered_map<std::string, Pack::IndexEntry> index;
        std::string why;
        std::size_t notCatalogs = 0;
        if (error)
        {
            why = "There's no animation pack (tools/t5-characters/setup.ps1 builds it)";
        }
        else
        {
            try
            {
                std::ifstream stream(path, std::ios::binary);
                index = Pack::ReadIndex(stream);
            }
            catch (const std::exception& e)
            {
                why = std::format("The animation pack doesn't read: {}", e.what());
            }
            // only the catalog's animations, as the catalog has them (a pack of other zones may differ)
            const auto& catalog = Components::Actors::GetAnimationCatalog();
            std::erase_if(index, [&](const auto& item) {
                const auto entry = catalog.Find(item.first);
                const bool same = entry && Pack::ParseHash(entry->hash) == item.second.hash;
                notCatalogs += same ? 0 : 1;
                return !same;
            });
            if (why.empty() && index.empty())
                why = "The animation pack has none of the catalog's animations";
        }

        std::lock_guard lock(state.mutex);
        const bool wasUsable = IsUsable(state);
        state.path = path;
        state.indexedSize = error ? 0 : size;
        state.indexedTime = time;
        state.index = std::move(index);
        state.ready = why.empty();
        state.error = why;
        if (IsUsable(state) != wasUsable)
            ++state.generation;
        if (state.ready)
            LOG_INFO("The animation pack has {} of the catalog's animations ({} others left out)", state.index.size(),
                     notCatalogs);
        else
            LOG_INFO("{}: actors play the animations the game has loaded", why);
    }

    bool IsReady()
    {
        auto& state = GetState();
        std::lock_guard lock(state.mutex);
        return IsUsable(state);
    }

    bool Has(std::string_view key)
    {
        auto& state = GetState();
        std::lock_guard lock(state.mutex);
        return IsUsable(state) && state.index.contains(std::string(key));
    }

    std::string GetError()
    {
        auto& state = GetState();
        std::lock_guard lock(state.mutex);
        return state.ignored ? "The animation pack is ignored" : state.error;
    }

    std::uint32_t GetGeneration()
    {
        auto& state = GetState();
        std::lock_guard lock(state.mutex);
        return state.generation;
    }

    Reference Acquire(const std::string& key, std::string* why)
    {
        auto& state = GetState();
        {
            std::lock_guard lock(state.mutex);
            if (!IsUsable(state))
            {
                if (why)
                    *why = "the store can't load";
                return {};
            }
            if (const auto it = state.loaded.find(key); it != state.loaded.end())
                return MakeReference(state, *it->second);
            if (const auto it = state.failed.find(key); it != state.failed.end())
            {
                if (why)
                    *why = it->second;
                return {};
            }
        }
        std::string reason;
        const auto started = std::chrono::steady_clock::now();
        auto loaded = LoadFromPack(key, reason);
        const auto took = std::chrono::steady_clock::now() - started;
        std::lock_guard lock(state.mutex);
        if (!loaded)
        {
            ++state.failures;
            state.failed[key] = reason;
            LOG_ERROR("The animation {} didn't load from the animation pack: {}", key, reason);
            if (why)
                *why = reason;
            return {};
        }
        ++state.loads;
        state.loadMicroseconds +=
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(took).count());
        state.changed = true;
        auto& slot = state.loaded[key];
        slot = std::move(loaded);
        return MakeReference(state, *slot);
    }

    Found Find(const std::string& name)
    {
        auto& state = GetState();
        bool stores = false, inPack = false;
        {
            std::lock_guard lock(state.mutex);
            inPack = IsUsable(state) && state.index.contains(name);
            stores = inPack && IsStoresOwn(state, name);
        }
        if (!stores && GetAssetName(name).size() == name.size())
            if (const auto parts =
                    static_cast<const XAnimParts*>(ContentPacks::FindAsset(ASSET_TYPE_XANIMPARTS, name.c_str())))
                return {parts, {}};
        if (!inPack)
            return {};
        auto stored = Acquire(name);
        const auto parts = stored.Parts();
        return {parts, std::move(stored)};
    }

    Found FindLoaded(const std::string& name)
    {
        auto& state = GetState();
        bool stores = false, inPack = false;
        {
            std::lock_guard lock(state.mutex);
            inPack = IsUsable(state) && state.index.contains(name);
            stores = inPack && IsStoresOwn(state, name);
        }
        if (!stores && GetAssetName(name).size() == name.size())
            if (const auto parts =
                    static_cast<const XAnimParts*>(ContentPacks::FindAsset(ASSET_TYPE_XANIMPARTS, name.c_str())))
                return {parts, {}};
        if (!inPack)
            return {};
        std::lock_guard lock(state.mutex);
        if (const auto it = state.loaded.find(name); it != state.loaded.end())
        {
            auto stored = MakeReference(state, *it->second);
            const auto parts = stored.Parts();
            return {parts, std::move(stored)};
        }
        if (!state.failed.contains(name) && std::ranges::find(state.requests, name) == state.requests.end())
        {
            state.requests.push_back(name);
            if (state.requests.size() > MAX_REQUESTS)
                state.requests.pop_front();
        }
        return {};
    }

    // whether the store would load an animation for an actor (one the game has registered is played as the game has
    // it; main thread)
    bool WouldLoad(const std::string& key)
    {
        auto& state = GetState();
        {
            std::lock_guard lock(state.mutex);
            if (!IsUsable(state) || !state.index.contains(key) || state.loaded.contains(key) ||
                state.failed.contains(key))
                return false;
            if (IsStoresOwn(state, key))
                return true;
        }
        return !ContentPacks::FindAsset(ASSET_TYPE_XANIMPARTS, key.c_str());
    }

    void RunFrame(const std::vector<std::string>& wanted, bool capturing)
    {
        auto& state = GetState();
        std::deque<std::string> requests;
        {
            std::lock_guard lock(state.mutex);
            if (state.unloaded)
                return;
            ++state.frame;
            const std::set<std::string_view> named(wanted.begin(), wanted.end());
            for (auto& [key, loaded] : state.loaded)
                loaded->wanted = named.contains(key);
            for (std::size_t i = 0; i < REQUESTS_A_FRAME && !state.requests.empty(); ++i)
            {
                requests.push_back(std::move(state.requests.front()));
                state.requests.pop_front();
            }
        }

        // the scene's, then what previews asked for (loaded and kept as the scene's are, until they go)
        const auto started = std::chrono::steady_clock::now();
        for (const auto& name : wanted)
        {
            if (!capturing && std::chrono::steady_clock::now() - started > WANTED_TIME)
                break;
            if (WouldLoad(name))
                static_cast<void>(Acquire(name));
        }
        for (const auto& name : requests)
            if (WouldLoad(name))
                static_cast<void>(Acquire(name));

        std::shared_ptr<std::set<std::string, std::less<>>> published;
        {
            std::lock_guard lock(state.mutex);
            // past the budget, the least recently used go; nothing while capturing, which may need it again
            if (!capturing)
            {
                std::vector<Pack::Cached> cached;
                cached.reserve(state.loaded.size());
                for (const auto& [key, loaded] : state.loaded)
                    cached.push_back({key, loaded->bytes, loaded->references, loaded->wanted, loaded->lastUsed});
                for (const auto& key : Pack::ChooseUnloads(cached, state.budget))
                {
                    auto& loaded = state.loaded.at(key);
                    loaded->retiredAt = state.frame;
                    state.retired.push_back(std::move(loaded));
                    state.loaded.erase(key);
                    ++state.unloads;
                    state.changed = true;
                }
            }
            // freed once nothing uses them, and no skeleton of a frame they were used in can be being worked out
            state.frees += std::erase_if(state.retired, [&](const std::unique_ptr<Loaded>& loaded) {
                return loaded->references == 0 &&
                       state.frame >= std::max(loaded->retiredAt, loaded->idleSince) + RETIRE_FRAMES;
            });
            if (state.changed)
            {
                state.changed = false;
                published = std::make_shared<std::set<std::string, std::less<>>>();
                for (const auto& [key, loaded] : state.loaded)
                    published->insert(key);
            }
        }
        if (published)
            Components::Actors::SetStoreLoaded(std::move(published));
    }

    void Clear()
    {
        auto& state = GetState();
        {
            std::lock_guard lock(state.mutex);
            for (auto& [key, loaded] : state.loaded)
            {
                loaded->retiredAt = state.frame;
                state.retired.push_back(std::move(loaded));
            }
            state.loaded.clear();
            state.requests.clear();
            state.failed.clear();
            state.changed = false;
            // the next map's script strings
            state.scriptStrings.clear();
        }
        Components::Actors::SetStoreLoaded(std::make_shared<std::set<std::string, std::less<>>>());
    }

    void Unload()
    {
        auto& state = GetState();
        std::lock_guard lock(state.mutex);
        state.unloaded = true;
        for (auto& [key, loaded] : state.loaded)
            static_cast<void>(loaded.release());
        for (auto& loaded : state.retired)
            static_cast<void>(loaded.release());
        state.loaded.clear();
        state.retired.clear();
    }

    Stats GetStats()
    {
        auto& state = GetState();
        std::lock_guard lock(state.mutex);
        Stats stats;
        stats.ready = IsUsable(state);
        stats.error = state.ignored ? "ignored" : state.error;
        stats.packEntries = state.index.size();
        stats.loaded = state.loaded.size();
        for (const auto& [key, loaded] : state.loaded)
        {
            stats.loadedBytes += loaded->bytes;
            stats.referenced += loaded->references > 0 ? 1 : 0;
            stats.wanted += loaded->wanted ? 1 : 0;
        }
        stats.retired = state.retired.size();
        for (const auto& loaded : state.retired)
            stats.retiredBytes += loaded->bytes;
        stats.budget = state.budget;
        stats.loads = state.loads;
        stats.loadMilliseconds = static_cast<double>(state.loadMicroseconds) / 1000.0;
        stats.unloads = state.unloads;
        stats.frees = state.frees;
        stats.failures = state.failures;
        stats.scriptStrings = state.scriptStrings.size();
        return stats;
    }

    std::vector<LoadedInfo> GetLoaded()
    {
        auto& state = GetState();
        std::lock_guard lock(state.mutex);
        std::vector<LoadedInfo> loaded;
        for (const auto& [key, each] : state.loaded)
            loaded.push_back({key, each->bytes, each->references, each->wanted});
        std::ranges::sort(loaded, {}, &LoadedInfo::key);
        return loaded;
    }

    void SetBudget(std::uint64_t bytes)
    {
        auto& state = GetState();
        std::lock_guard lock(state.mutex);
        state.budget = bytes;
    }

    void SetPreferStore(bool prefer)
    {
        auto& state = GetState();
        std::lock_guard lock(state.mutex);
        state.preferStore = prefer;
    }

    void SetIgnored(bool ignore)
    {
        auto& state = GetState();
        std::lock_guard lock(state.mutex);
        const bool wasUsable = IsUsable(state);
        state.ignored = ignore;
        if (IsUsable(state) != wasUsable)
            ++state.generation;
    }

    std::optional<std::vector<std::uint8_t>> ReadEntry(const std::string& key)
    {
        auto& state = GetState();
        Pack::IndexEntry entry;
        std::filesystem::path path;
        {
            std::lock_guard lock(state.mutex);
            const auto it = state.index.find(key);
            if (it == state.index.end())
                return std::nullopt;
            entry = it->second;
            path = state.path;
        }
        return ReadInflated(path, entry);
    }

    std::vector<std::string> GetKeys()
    {
        auto& state = GetState();
        std::lock_guard lock(state.mutex);
        std::vector<std::string> keys;
        keys.reserve(state.index.size());
        for (const auto& [key, entry] : state.index)
            keys.push_back(key);
        std::ranges::sort(keys);
        return keys;
    }
}  // namespace IWXMVM::T5::AnimationStore
