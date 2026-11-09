#include "StdInclude.hpp"
#include "ModelLighting.hpp"

#include "Components/ModelLightingLogic.hpp"
#include "PoolGrowth.hpp"
#include "Utilities/HookManager.hpp"

namespace IWXMVM::T5::ModelLighting
{
    namespace
    {
        namespace Logic = Components::ModelLightingLogic;

        // A model's record for the frame, which the cache copies out of its slot
        struct LightingRecord
        {
            std::uint8_t primaryLight;
            std::uint8_t reflectionProbe;
            std::uint8_t _unk02[2];
            std::uint32_t handle;
        };
        static_assert(sizeof(LightingRecord) == 8);

#pragma pack(push, 1)
        // the world's list of the frame's dynamic models
        struct DynamicModelEntry
        {
            std::uint8_t _unk00[4];
            std::uint16_t id;
        };
#pragma pack(pop)
        static_assert(sizeof(DynamicModelEntry) == 6);

        // the world (rgp.world) and what it has of its dynamic models
        constexpr std::uintptr_t WORLD = 0x46EC4CC;
        constexpr std::size_t WORLD_DYNAMIC_MODEL_LIST = 0x324;
        constexpr std::size_t WORLD_DYNAMIC_MODEL_COUNT = 0x3BC;
        // DynEntityClient: its lighting handle at +6
        constexpr std::uintptr_t DYNAMIC_MODEL_CLIENTS = 0x352FE18;
        constexpr std::size_t DYNAMIC_MODEL_CLIENT_SIZE = 20;
        constexpr std::size_t DYNAMIC_MODEL_LIGHTING_HANDLE = 6;
        // the frame's DObjs and models, lit before the dynamic models
        constexpr std::uintptr_t SCENE_DOBJS = 0x4856EAC;
        constexpr std::uintptr_t SCENE_MODELS = 0x4879AB0;

        // The cache: its slots of the frame's records, and an origin for each
        constexpr std::uintptr_t SLOT_COUNT = 0x5342BA0;
        constexpr std::uintptr_t SLOTS = 0x5342BA4;
        constexpr std::uintptr_t SLOT_ORIGINS = 0x5342BA8;
        constexpr std::uintptr_t CACHE_FULL = 0x5342BAC;

        // the primary light of a dynamic model, which the cache asks for
        constexpr std::uintptr_t DYNAMIC_MODEL_PRIMARY_LIGHT = 0x73A670;
        // DynEnt_GetPose(id, drawType): its origin at +0x10
        constexpr std::uintptr_t DYNAMIC_MODEL_POSE = 0x4B1850;
        constexpr std::size_t POSE_ORIGIN = 0x10;
        // R_SetAllSceneDynModelLighting(count, visible, records), the game's function this takes over
        constexpr std::uintptr_t LIGHT_DYNAMIC_MODELS = 0x6D12A0;
        constexpr std::array<std::uint8_t, 8> LIGHT_DYNAMIC_MODELS_BYTES{0x83, 0xEC, 0x08, 0x53, 0x33, 0xDB, 0x39, 0x5C};

        typedef void(__cdecl* LightDynamicModels_t)(std::uint32_t count, std::uint8_t* visible,
                                                    LightingRecord* records);
        LightDynamicModels_t LightDynamicModels = nullptr;

        template <typename T>
        T Read(std::uintptr_t address)
        {
            return *reinterpret_cast<const T*>(address);
        }

        // The cache's function at 0x73A3C0 hands a model a slot: its origin in edi, how far it may move in xmm3 (a
        // dynamic model stays), the rest on the stack
        __declspec(naked) std::uint32_t __cdecl AllocLighting(const float* origin, std::uint32_t setting,
                                                              std::uint16_t* handle, const void* callback,
                                                              const void* callbackData, LightingRecord* record)
        {
            __asm
            {
                push edi
                mov edi, [esp + 8]
                xorps xmm3, xmm3
                push [esp + 0x1C]  // record
                push [esp + 0x1C]  // callbackData
                push [esp + 0x1C]  // callback
                push [esp + 0x1C]  // handle
                push [esp + 0x1C]  // setting
                mov eax, 0x73A3C0
                call eax
                add esp, 0x14
                pop edi
                ret
            }
        }

        std::mutex statsMutex;
        Stats frame;
        Stats most;

        // The slots as FindNearestSlot reads them, made once a frame the first time one is needed
        std::vector<Logic::Slot> slotView;

        // A model that shares keeps the slot it found while that slot stays where it was: a search goes through 1024
        // slots, and the list can hold hundreds that share
        struct Shared
        {
            std::int32_t slot = -1;
            float origin[3]{};
        };
        std::vector<Shared> sharedSlots;
        std::uintptr_t sharedWorld = 0;

        bool ShareSlot(std::uint16_t id, const float* origin, LightingRecord* record)
        {
            const auto slots = Read<const LightingRecord*>(SLOTS);
            const auto origins = Read<const float*>(SLOT_ORIGINS);
            const auto count = Read<std::uint32_t>(SLOT_COUNT);
            auto& shared = sharedSlots[id];
            if (shared.slot >= 0 && static_cast<std::uint32_t>(shared.slot) < count &&
                std::memcmp(origins + shared.slot * 3, shared.origin, sizeof(shared.origin)) == 0 &&
                slots[shared.slot].primaryLight != Logic::SLOT_BEING_LIT)
            {
                *record = slots[shared.slot];
                return true;
            }
            if (slotView.empty())
            {
                slotView.resize(count);
                for (std::uint32_t i = 0; i < count; ++i)
                {
                    std::memcpy(slotView[i].origin, origins + i * 3, sizeof(slotView[i].origin));
                    slotView[i].primaryLight = slots[i].primaryLight;
                }
            }
            const auto nearest = Logic::FindNearestSlot(slotView, origin);
            if (nearest < 0)
                return false;
            shared.slot = nearest;
            std::memcpy(shared.origin, origins + nearest * 3, sizeof(shared.origin));
            *record = slots[nearest];
            return true;
        }

        void __cdecl LightDynamicModels_Hook(std::uint32_t count, std::uint8_t* visible, LightingRecord* records)
        {
            if (!PoolGrowth::IsGrown())
            {
                // the records' array holds 1024 without the growth, as multiplayer's maps need
                LightDynamicModels(count, visible, records);
                return;
            }
            const auto world = Read<std::uintptr_t>(WORLD);
            const auto list = Read<const DynamicModelEntry*>(world + WORLD_DYNAMIC_MODEL_LIST);
            const auto clients = Read<std::uint8_t*>(DYNAMIC_MODEL_CLIENTS);
            const auto pose = reinterpret_cast<const std::uint8_t*(__cdecl*)(std::uint32_t, std::int32_t)>(
                DYNAMIC_MODEL_POSE);
            const auto dynamicModels = Read<std::uint32_t>(world + WORLD_DYNAMIC_MODEL_COUNT);
            if (sharedWorld != world || sharedSlots.size() < dynamicModels)
            {
                sharedWorld = world;
                sharedSlots.assign(dynamicModels, {});
            }
            slotView.clear();

            const auto own = Logic::GetDynamicModelSlots(Read<std::uint32_t>(SLOT_COUNT), Read<std::uint32_t>(SCENE_DOBJS),
                                                         Read<std::uint32_t>(SCENE_MODELS));
            std::uint32_t lit = 0, shared = 0, unlit = 0;
            for (std::uint32_t place = 0; place < count; ++place)
            {
                const auto id = list[place].id;
                if (visible[id] != 1)
                    continue;
                const auto origin = reinterpret_cast<const float*>(pose(id, 0) + POSE_ORIGIN);
                const auto handle = reinterpret_cast<std::uint16_t*>(clients + id * DYNAMIC_MODEL_CLIENT_SIZE +
                                                                     DYNAMIC_MODEL_LIGHTING_HANDLE);
                std::uint32_t data = id;
                if (lit < own && AllocLighting(origin, 0, handle, reinterpret_cast<const void*>(DYNAMIC_MODEL_PRIMARY_LIGHT),
                                               &data, &records[place]) != 0)
                {
                    ++lit;
                    continue;
                }
                if (ShareSlot(id, origin, &records[place]))
                    ++shared;
                else
                {
                    // as the game does for any model it can't light, with the id where the list's place was
                    visible[id] = 0;
                    ++unlit;
                }
            }
            Stats stats{dynamicModels, count, lit, shared, unlit};
            std::lock_guard lock(statsMutex);
            stats.mostListed = most.mostListed = std::max(most.mostListed, count);
            stats.mostShared = most.mostShared = std::max(most.mostShared, shared);
            stats.mostUnlit = most.mostUnlit = std::max(most.mostUnlit, unlit);
            stats.cacheFull = most.cacheFull += Read<std::uint32_t>(CACHE_FULL) != 0 ? 1 : 0;
            frame = stats;
        }
    }  // namespace

    Stats GetStats()
    {
        std::lock_guard lock(statsMutex);
        return frame;
    }

    void ResetStats()
    {
        std::lock_guard lock(statsMutex);
        most = {};
    }

    void Install()
    {
        if (std::memcmp(reinterpret_cast<const void*>(LIGHT_DYNAMIC_MODELS), LIGHT_DYNAMIC_MODELS_BYTES.data(),
                        LIGHT_DYNAMIC_MODELS_BYTES.size()) != 0)
        {
            LOG_ERROR("The dynamic models' lighting isn't the code expected; they stay as the game lights them");
            return;
        }
        HookManager::CreateHook(LIGHT_DYNAMIC_MODELS, reinterpret_cast<std::uintptr_t>(LightDynamicModels_Hook),
                                reinterpret_cast<std::uintptr_t*>(&LightDynamicModels));
    }
}  // namespace IWXMVM::T5::ModelLighting
