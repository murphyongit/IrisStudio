#pragma once
#include "Components/TransitionLogic.hpp"

namespace IWXMVM::Components::PoseSignatures
{
    // The first and last poses of the animations actors play (see TransitionLogic::PoseSignature), each worked out once
    // for an animation and a body from the game's decoded poses (GameInterface::GetAnimationPoseByName), on the mod's
    // own thread rather than the game's, and kept until the catalog changes (another map or pack). They say how cleanly
    // one animation goes on from another where no state does (Q18).

    void Initialize();
    // Works out the signatures asked for since; the mod's own thread calls it over and over
    void Work();

    // An animation's signature on a body model: null until it is worked out (the first call asks for it), and for one
    // the decoder can't read (one the game can't load)
    std::shared_ptr<const TransitionLogic::PoseSignature> Get(const std::string& animation, const std::string& body);
    // the signatures on an actor's body
    TransitionLogic::PoseLookup GetLookup(const Types::Actor& actor);
    // how many were worked out since the mod started, for the harness
    std::uint32_t CountWorkedOut();
}  // namespace IWXMVM::Components::PoseSignatures
