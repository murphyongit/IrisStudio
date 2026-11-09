#include "StdInclude.hpp"
#include "Structures.hpp"

#include "Addresses.hpp"

namespace IWXMVM::T5::Structures
{
    clientActive_t* GetClientActive()
    {
        return reinterpret_cast<clientActive_t*>(GetGameAddresses().cl_snap());
    }

    clSnapshot_t* GetClientSnapshots()
    {
        return reinterpret_cast<clSnapshot_t*>(GetGameAddresses().cl_snapshots());
    }

    clientConnection_t* GetClientConnection()
    {
        return reinterpret_cast<clientConnection_t*>(GetGameAddresses().clc_serverCommandSequence() -
                                                     offsetof(clientConnection_t, serverCommandSequence));
    }

    clientStatic_t* GetClientStatic()
    {
        return reinterpret_cast<clientStatic_t*>(GetGameAddresses().cls_realtime() -
                                                 offsetof(clientStatic_t, realtime));
    }

    gameState_t* GetGameState()
    {
        return reinterpret_cast<gameState_t*>(GetGameAddresses().cl_gameState());
    }

    const char* GetConfigString(std::int32_t index)
    {
        const auto gameState = GetGameState();
        return gameState->stringData + gameState->stringOffsets[index];
    }

    playlist_t* GetPlaylists()
    {
        return reinterpret_cast<playlist_t*>(GetGameAddresses().playlists_entryCount() -
                                             offsetof(playlist_t, entryCount));
    }

    std::int32_t* GetKeyCatchers()
    {
        return reinterpret_cast<std::int32_t*>(GetGameAddresses().clientUIActives_keyCatchers());
    }

    cg_t* GetClientGlobals()
    {
        return *reinterpret_cast<cg_t**>(GetGameAddresses().cgArray());
    }

    cgs_t* GetClientGlobalsStatic()
    {
        return *reinterpret_cast<cgs_t**>(GetGameAddresses().cgsArray());
    }

    clientInfo_t* GetCorpseInfo(std::uint32_t corpse)
    {
        constexpr std::size_t CORPSE_INFO = 0x17E0;  // in cgs_t, as the game copies them for ET_PLAYER_CORPSE (0x7F1E41)
        const auto cgs = reinterpret_cast<std::uint8_t*>(GetClientGlobalsStatic());
        return cgs ? reinterpret_cast<clientInfo_t*>(cgs + CORPSE_INFO) + corpse : nullptr;
    }

    centity_t* GetEntities()
    {
        // cg_entitiesArray[0]
        return *reinterpret_cast<centity_t**>(GetGameAddresses().cg_entitiesArray());
    }

    IKState* GetIKStates()
    {
        return *reinterpret_cast<IKState**>(GetGameAddresses().ik_states());
    }
}  // namespace IWXMVM::T5::Structures
