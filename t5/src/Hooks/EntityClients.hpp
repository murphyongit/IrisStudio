#pragma once

namespace IWXMVM::T5::Hooks::EntityClients
{
    // Keeps the local client each entity of the snapshots belongs to right, which the client scripts' calls for the
    // entity go by (see EntityClients.cpp)
    void Install();

    // how many entities' local client was put right since the game started, for the harness
    std::size_t GetFixCount();
}  // namespace IWXMVM::T5::Hooks::EntityClients
