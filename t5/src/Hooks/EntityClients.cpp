#include "StdInclude.hpp"
#include "EntityClients.hpp"

#include "Utilities/HookManager.hpp"
#include "../Addresses.hpp"
#include "../Structures.hpp"

namespace IWXMVM::T5::Hooks::EntityClients
{
    // Each entity of a local client's snapshots keeps the local client it belongs to in its pose (the byte at +4), and
    // the client scripts' calls for the entity go by it: entityspawned, run as an entity first shows, is run for that
    // local client, and a field of the entity read for a local client without a snapshot fails with "Lost connection
    // to host. Please try again." (EXE_ENTITY_BEFORE_SNAPSHOT).
    //
    // On Plutonium, code of Plutonium's own, run from the game's frame, now and then writes a byte into the entities
    // of local client 0, also while a demo loads: the local client of an entity became the low byte of its number
    // (entity 285 belonged to local client 29). When such an entity showed in the first snapshot, its entityspawned
    // ran for a local client the demo doesn't have and failed, and the client scripts were torn down in the middle of
    // it, which left their variables and memory broken: the demo dropped to the menu, or hung or crashed in the
    // scripts' variable code later. Since that code runs outside the client's game, the
    // entities are put right each frame as the client's game starts on them.
    inline constexpr std::size_t POSE_LOCAL_CLIENT = 4;
    inline constexpr std::int32_t MAX_LOCAL_CLIENTS = 4;

    std::atomic<std::size_t> fixes = 0;
#ifdef IWXMVM_T5_HARNESS
    // IWXMVM_NO_ENTITY_GUARD leaves the entities as they are, for comparing (tools/t5-harness)
    bool guarded = true;
#endif

    void PutRight(std::int32_t localClientNum)
    {
#ifdef IWXMVM_T5_HARNESS
        if (!guarded)
            return;
#endif
        if (localClientNum < 0 || localClientNum >= MAX_LOCAL_CLIENTS)
            return;
        const auto entities =
            reinterpret_cast<std::uint8_t**>(GetGameAddresses().cg_entitiesArray())[localClientNum];
        if (!entities)
            return;
        for (std::size_t i = 0; i < Structures::MAX_GENTITIES; ++i)
        {
            auto& owner = entities[i * Structures::CENTITY_SIZE + POSE_LOCAL_CLIENT];
            if (owner != localClientNum)
            {
                owner = static_cast<std::uint8_t>(localClientNum);
                ++fixes;
            }
        }
    }

    typedef void(__cdecl* CG_ProcessSnapshots_t)(std::int32_t localClientNum);
    CG_ProcessSnapshots_t CG_ProcessSnapshots_Trampoline;
    void __cdecl CG_ProcessSnapshots_Hook(std::int32_t localClientNum)
    {
        PutRight(localClientNum);
        CG_ProcessSnapshots_Trampoline(localClientNum);
    }

    std::size_t GetFixCount()
    {
        return fixes.load();
    }

    void Install()
    {
#ifdef IWXMVM_T5_HARNESS
        guarded = GetEnvironmentVariableA("IWXMVM_NO_ENTITY_GUARD", nullptr, 0) == 0;
#endif
        HookManager::CreateHook(GetGameAddresses().CG_ProcessSnapshots(), (std::uintptr_t)CG_ProcessSnapshots_Hook,
                                (std::uintptr_t*)&CG_ProcessSnapshots_Trampoline);
    }
}  // namespace IWXMVM::T5::Hooks::EntityClients
