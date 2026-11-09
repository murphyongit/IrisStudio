#pragma once
#include "../Structures.hpp"

namespace IWXMVM::T5::Hooks::Loadouts
{
    // changes a player's or corpse's state before the game updates its model; from CG_AddPacketEntity
    void ProcessEntity(Structures::centity_t& cent);
    // and puts the demo's values back once it has
    void ProcessEntityModel(Structures::centity_t& cent);

    // the same for the demo's own player in first person, once per frame
    void ProcessPredictedPlayer();

    // a live game's map is over: what was built for it goes (a demo's load does the same)
    void ResetLive();

    void Install();
}  // namespace IWXMVM::T5::Hooks::Loadouts
