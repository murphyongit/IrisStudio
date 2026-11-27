#pragma once
#include "StdInclude.hpp"

#include <Xinput.h>

// IW4x's header checks its own layouts with these
#define AssertSize(x, size) static_assert(sizeof(x) == (size), "Structure has an invalid size: " #x)
#define AssertOffset(x, y, offset) static_assert(offsetof(x, y) == (offset), #x "::" #y " is not at the right offset")

#pragma warning(push, 0)
#pragma warning(disable : 4200 4201 4324 4366)
#include "Game/Structs.hpp"
#pragma warning(pop)

#undef AssertSize
#undef AssertOffset

namespace IWXMVM::IW4::Structures
{
    using namespace ::Game;

    inline constexpr int MAX_CLIENTS = 18;

    cg_s* GetClientGlobals();
    cgs_t* GetClientGlobalsStatic();
    clientActive_t* GetClientActive();
    clientConnection_t* GetClientConnection();
    clientStatic_t* GetClientStatic();
    clientUIActive_t* GetClientUIActive();
    cgEntity_s* GetEntities();
    clientInfo_t* GetClientInfo();
    gameState_t* GetGameState();
    Console* GetConsole();
}  // namespace IWXMVM::IW4::Structures
