#include "StdInclude.hpp"
#include "Structures.hpp"

#include "Addresses.hpp"

namespace IWXMVM::IW4::Structures
{
    // the layouts the editor leans on, as the game's own code has them (disassembly of build 177)
    static_assert(offsetof(cg_s, refdef) == 0x6A778);
    static_assert(offsetof(cg_s, time) == 0x6A758);
    static_assert(offsetof(clientActive_t, snap) + offsetof(clSnapshot_t, serverTime) == 0x312C);
    static_assert(offsetof(clientActive_t, serverTime) == 0x3158);
    static_assert(offsetof(clientActive_t, serverTimeDelta) == 0x3164);
    static_assert(offsetof(clientStatic_t, realtime) == 0x11C);
    static_assert(offsetof(clientConnection_t, demoplaying) == 0x40194);
    static_assert(sizeof(cgEntity_s) == 0x204);
    static_assert(sizeof(clientInfo_t) == 0x52C);

    cg_s* GetClientGlobals()
    {
        return reinterpret_cast<cg_s*>(Addresses::cg);
    }

    cgs_t* GetClientGlobalsStatic()
    {
        return reinterpret_cast<cgs_t*>(Addresses::cgs);
    }

    clientActive_t* GetClientActive()
    {
        return reinterpret_cast<clientActive_t*>(Addresses::clientActive);
    }

    clientConnection_t* GetClientConnection()
    {
        return reinterpret_cast<clientConnection_t*>(Addresses::clientConnection);
    }

    clientStatic_t* GetClientStatic()
    {
        return reinterpret_cast<clientStatic_t*>(Addresses::clientStatic);
    }

    clientUIActive_t* GetClientUIActive()
    {
        return reinterpret_cast<clientUIActive_t*>(Addresses::clientUIActive);
    }

    cgEntity_s* GetEntities()
    {
        return reinterpret_cast<cgEntity_s*>(Addresses::cg_entities);
    }

    clientInfo_t* GetClientInfo()
    {
        return reinterpret_cast<clientInfo_t*>(Addresses::clientInfo);
    }

    gameState_t* GetGameState()
    {
        return &GetClientStatic()->cl_gameState;
    }

    Console* GetConsole()
    {
        return reinterpret_cast<Console*>(Addresses::con);
    }
}  // namespace IWXMVM::IW4::Structures
