#pragma once

namespace IWXMVM::Types
{
    // Bullet time: a part of the timeline where the demo stands still at one of its ticks
    // for so long while the camera and the Look keys go on, after which it plays on from there. Actors hold with it
    // unless it lets them move.
    struct FrozenSpan
    {
        std::uint32_t worldTick = 0;  // the demo's tick it holds at
        std::uint32_t length = 0;     // how long it holds, in ms of the timeline
        bool actorsMove = false;

        bool operator==(const FrozenSpan&) const = default;
    };
}  // namespace IWXMVM::Types
