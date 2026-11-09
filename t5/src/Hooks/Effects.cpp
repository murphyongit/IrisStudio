#include "StdInclude.hpp"
#include "Effects.hpp"

#include "Events.hpp"
#include "Utilities/HookManager.hpp"
#include "../Addresses.hpp"
#include "../Functions.hpp"

namespace IWXMVM::T5::Hooks::Effects
{
    // A rewind starts the demo over and plays it up to the tick asked for, so what happens in it spawns its effects
    // again, among them looping ones the client scripts keep while an entity is there (the green lights over the doors
    // of Outskirts, the lights of equipment). The ones spawned before the rewind went on as well: Outskirts had two
    // more lights after each rewind, until the client's 1024 effects were all in use and no effect spawned anymore
    // (muzzle flashes, explosions; tracers aren't effects). So the looping effects are kept track of as they spawn,
    // and one spawned before a rewind is killed once the rewind has spawned the same effect in the same place again.
    // Nothing that isn't spawned again goes, like the map's ambient effects, which its scripts spawn as it loads.
    struct Looping
    {
        std::int32_t handle = 0;
        const void* def = nullptr;
        std::array<float, 7> frame{};  // where it spawned: a rotation (x y z w) and an origin
        std::uint32_t rewinds = 0;     // how many rewinds there were before it spawned
    };
    std::mutex mutex;
    std::vector<Looping> looping;
    std::vector<std::int32_t> doomed;  // spawned again since a rewind, killed on the game's thread
    std::atomic<std::uint32_t> rewinds = 0;
    // Those that are gone are forgotten about once a second (and as a rewind starts), so the list every looping spawn
    // looks through doesn't grow through a long demo played without rewinds
    std::chrono::steady_clock::time_point forgotAt;
    std::size_t forgotten = 0;  // since the map loaded, for the harness

    // An FxEffect: its definition first, where it spawned (and where it is now, and was), and its status, which has
    // this flag from its spawn while it has looping elements
    inline constexpr std::size_t EFFECT_FRAME_AT_SPAWN = 0x28;
    inline constexpr std::size_t EFFECT_STATUS = 0xC0;
    inline constexpr std::uint32_t EFFECT_STATUS_LOOPING = 0x10000;

    const std::uint8_t* GetEffect(std::int32_t handle)
    {
        typedef const std::uint8_t*(__cdecl * FX_EffectFromHandle_t)(void* system, std::int32_t handle);
        const auto system = *reinterpret_cast<void**>(GetGameAddresses().fx_system());
        return system && handle
                   ? reinterpret_cast<FX_EffectFromHandle_t>(GetGameAddresses().FX_EffectFromHandle())(system, handle)
                   : nullptr;
    }

    bool SpawnedAlike(const Looping& a, const Looping& b)
    {
        constexpr float MAX_ROTATION_DIFFERENCE = 0.001f;
        constexpr float MAX_DISTANCE = 0.5f;
        if (a.def != b.def)
            return false;
        for (std::size_t i = 0; i < 4; ++i)
            if (std::abs(a.frame[i] - b.frame[i]) > MAX_ROTATION_DIFFERENCE)
                return false;
        for (std::size_t i = 4; i < 7; ++i)
            if (std::abs(a.frame[i] - b.frame[i]) > MAX_DISTANCE)
                return false;
        return true;
    }

    // How many of the client's effects are in use: the ring of them (+0x35C) has the first in use at +0x18 and the
    // first free at +0x20
    std::size_t CountUsed()
    {
        const auto system = *reinterpret_cast<std::uint8_t**>(GetGameAddresses().fx_system());
        const auto ring = system ? *reinterpret_cast<std::uint8_t**>(system + 0x35C) : nullptr;
        if (!ring)
            return 0;
        const auto first = *reinterpret_cast<const volatile std::int32_t*>(ring + 0x18);
        const auto end = *reinterpret_cast<const volatile std::int32_t*>(ring + 0x20);
        return static_cast<std::size_t>(std::max(end - first, 0));
    }

    // spawns refused since the map loaded because the client's effects were all in use
    std::atomic<std::uint32_t> refused = 0;

    std::uint32_t CountRefused()
    {
        return refused;
    }

    // FX_SpawnEffect(system, def, ...) spawns an effect and returns its handle, 0 when it couldn't; the effects' own
    // elements spawn effects through it too, on the threads that update them
    typedef std::int32_t(__cdecl* FX_SpawnEffect_t)(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t,
                                                    std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t,
                                                    std::uint32_t, std::uint32_t, std::uint32_t);
    FX_SpawnEffect_t FX_SpawnEffect_Trampoline;

#ifdef IWXMVM_T5_HARNESS
    std::atomic<int> watching = 0;
    std::mutex watchMutex;
    std::vector<WatchedSpawn> watched;

    // (guarded: an argument that isn't a pointer reads nothing)
    bool ReadFloats(std::uint32_t address, float (&out)[3])
    {
        __try
        {
            const auto floats = reinterpret_cast<const float*>(address);
            out[0] = floats[0];
            out[1] = floats[1];
            out[2] = floats[2];
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }
    bool ReadName(std::uint32_t def, char (&out)[64])
    {
        __try
        {
            const auto name = *reinterpret_cast<const char* const*>(def);
            for (std::size_t i = 0; i + 1 < sizeof(out); ++i)
            {
                out[i] = name[i];
                if (!name[i])
                    return true;
            }
            out[sizeof(out) - 1] = 0;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void Watch(const std::array<std::uint32_t, 11>& args)
    {
        WatchedSpawn spawn{args};
        char name[64] = {};
        if (ReadName(args[1], name))
            spawn.name = name;
        float pointed[3] = {};
        if (ReadFloats(args[3], pointed))
            spawn.pointed = std::array{pointed[0], pointed[1], pointed[2]};
        std::lock_guard lock(watchMutex);
        watched.push_back(std::move(spawn));
    }

    void WatchSpawns(int count)
    {
        std::lock_guard lock(watchMutex);
        watched.clear();
        watching = count;
    }

    std::optional<std::array<float, 3>> Peek(std::uint32_t address)
    {
        float read[3] = {};
        if (!ReadFloats(address, read))
            return std::nullopt;
        return std::array{read[0], read[1], read[2]};
    }

    std::vector<WatchedSpawn> GetWatchedSpawns()
    {
        std::lock_guard lock(watchMutex);
        return watched;
    }

    std::mutex spawnsMutex;
    std::map<std::string, SpawnCounts, std::less<>> spawns;  // by the effect's name, since the last reset

    void CountSpawn(std::uint32_t def, bool spawned, bool full)
    {
        char name[64] = {};
        if (!ReadName(def, name))
            return;
        std::lock_guard lock(spawnsMutex);
        auto& counts = spawns[name];
        ++(spawned ? counts.spawned : counts.refused);
        if (full)
            ++counts.full;
    }

    SpawnCounts CountSpawns(std::string_view part, bool reset)
    {
        std::lock_guard lock(spawnsMutex);
        SpawnCounts total;
        for (const auto& [name, counts] : spawns)
        {
            if (name.find(part) == std::string::npos)
                continue;
            total.spawned += counts.spawned;
            total.refused += counts.refused;
            total.full += counts.full;
        }
        if (reset)
            spawns.clear();
        return total;
    }
#endif

    std::int32_t __cdecl FX_SpawnEffect_Hook(std::uint32_t a0, std::uint32_t a1, std::uint32_t a2, std::uint32_t a3,
                                             std::uint32_t a4, std::uint32_t a5, std::uint32_t a6, std::uint32_t a7,
                                             std::uint32_t a8, std::uint32_t a9, std::uint32_t a10)
    {
#ifdef IWXMVM_T5_HARNESS
        if (watching > 0 && watching-- > 0)
            Watch({a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10});
#endif
        const auto handle = FX_SpawnEffect_Trampoline(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10);
        const bool full = !handle && CountUsed() >= MAX_EFFECTS;
        if (full)
            ++refused;
#ifdef IWXMVM_T5_HARNESS
        CountSpawn(a1, handle != 0, full);
#endif
        const auto effect = GetEffect(handle);
        if (!effect || !(*reinterpret_cast<const std::uint32_t*>(effect + EFFECT_STATUS) & EFFECT_STATUS_LOOPING))
            return handle;

        Looping spawned{handle, *reinterpret_cast<const void* const*>(effect), {}, rewinds};
        std::memcpy(spawned.frame.data(), effect + EFFECT_FRAME_AT_SPAWN, sizeof(spawned.frame));
        std::lock_guard lock(mutex);
        // the one it replaces, spawned before the last rewind
        const auto before = std::ranges::find_if(looping, [&](const Looping& other) {
            return other.rewinds < spawned.rewinds && SpawnedAlike(other, spawned);
        });
        if (before != looping.end())
        {
            doomed.push_back(before->handle);
            looping.erase(before);
        }
        looping.push_back(spawned);
        return handle;
    }

    bool IsLooping(std::int32_t handle)
    {
        const auto effect = GetEffect(handle);
        return effect && (*reinterpret_cast<const std::uint32_t*>(effect + EFFECT_STATUS) & EFFECT_STATUS_LOOPING);
    }

    // (with the mutex held)
    void ForgetGone()
    {
        forgotten += std::erase_if(looping, [](const Looping& effect) { return !GetEffect(effect.handle); });
        forgotAt = std::chrono::steady_clock::now();
    }

    void OnRewind()
    {
        std::lock_guard lock(mutex);
        ForgetGone();
        ++rewinds;
    }

    void OnClientFrame()
    {
        std::vector<std::int32_t> handles;
        {
            std::lock_guard lock(mutex);
            handles.swap(doomed);
            if (std::chrono::steady_clock::now() - forgotAt >= std::chrono::seconds(1))
                ForgetGone();
        }
        for (const auto handle : handles)
            Functions::FX_KillEffect(0, handle);
    }

    LoopingCounts CountLooping()
    {
        std::lock_guard lock(mutex);
        LoopingCounts counts{looping.size(), 0, forgotten};
        counts.alive = static_cast<std::size_t>(
            std::ranges::count_if(looping, [](const Looping& effect) { return GetEffect(effect.handle) != nullptr; }));
        return counts;
    }

    std::size_t CountEffectsInUse()
    {
        const auto system = *reinterpret_cast<std::uint8_t**>(GetGameAddresses().fx_system());
        const auto ring = system ? *reinterpret_cast<std::uint8_t**>(system + 0x35C) : nullptr;
        if (!ring)
            return 0;
        return static_cast<std::size_t>(std::max(*reinterpret_cast<const std::int32_t*>(ring + 0x20) -
                                                     *reinterpret_cast<const std::int32_t*>(ring + 0x18),
                                                 0));
    }

    EffectCounts CountEffects()
    {
        // The system keeps the effects in use in a ring of 1024 (+0x35C: the first in use at +0x18, the first free at
        // +0x20, their indices at +0x30), indices into its effects (+0x290) in dwords
        EffectCounts counts{.refused = refused};
        const auto system = *reinterpret_cast<std::uint8_t**>(GetGameAddresses().fx_system());
        const auto ring = system ? *reinterpret_cast<std::uint8_t**>(system + 0x35C) : nullptr;
        const auto effects = system ? *reinterpret_cast<std::uint8_t**>(system + 0x290) : nullptr;
        if (!ring || !effects)
            return counts;
        const auto first = *reinterpret_cast<const std::int32_t*>(ring + 0x18);
        const auto end = *reinterpret_cast<const std::int32_t*>(ring + 0x20);
        std::map<std::string, std::size_t> names;
        for (auto slot = first; slot < end; ++slot)
        {
            const auto index = *reinterpret_cast<const std::uint16_t*>(ring + 0x30 + (slot & 0x3FF) * 2);
            const auto def = *reinterpret_cast<const char* const* const*>(effects + index * 4);
            ++names[def && *def ? *def : "?"];
        }
        counts.used = static_cast<std::size_t>(std::max(end - first, 0));
        counts.most.assign(names.begin(), names.end());
        std::ranges::sort(counts.most, std::greater{}, &std::pair<std::string, std::size_t>::second);
        return counts;
    }

    void Install()
    {
        // the effects go with the map
        Events::RegisterListener(EventType::PreDemoLoad, [] {
            std::lock_guard lock(mutex);
            looping.clear();
            doomed.clear();
            forgotten = 0;
            refused = 0;
        });

        HookManager::CreateHook(GetGameAddresses().FX_SpawnEffect(), (std::uintptr_t)FX_SpawnEffect_Hook,
                                (std::uintptr_t*)&FX_SpawnEffect_Trampoline);
    }
}  // namespace IWXMVM::T5::Hooks::Effects
