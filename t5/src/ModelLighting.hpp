#pragma once

// Lights every dynamic model of a level with more of them in view than the game's lighting cache has slots.
// The array behind this moves in PoolGrowth, which outlives the mod.
namespace IWXMVM::T5::ModelLighting
{
    // (the harness's) the frame drawn last and the most since Reset
    struct Stats
    {
        std::uint32_t dynamicModels = 0;  // in the world
        std::uint32_t listed = 0;         // in the frame's list
        std::uint32_t lit = 0;            // that went to the game's cache
        std::uint32_t shared = 0;         // that took a lit slot's lighting
        std::uint32_t unlit = 0;          // not drawn: no slot was lit yet
        std::uint32_t mostListed = 0;
        std::uint32_t mostShared = 0;
        std::uint32_t mostUnlit = 0;
        std::uint32_t cacheFull = 0;      // frames the game found no free slot (for a model of any kind)
    };
    Stats GetStats();
    void ResetStats();

    void Install();
}  // namespace IWXMVM::T5::ModelLighting
