#pragma once
#include <set>
#include <string>
#include <vector>

namespace IWXMVM::Types
{
    // The ambient effects of the map that the mod plays (a campaign level's createfx), as
    // the Look tab lists them: a row for each definition, with how many the level places and play now
    struct MapEffectKind
    {
        std::string definition;  // "env/fire/fx_fire_sm"
        std::string name;        // "fx_fire_sm"
        std::uint32_t placed = 0;
        std::uint32_t playing = 0;
        bool shown = true;
    };

    struct MapEffects
    {
        std::string level;
        std::vector<MapEffectKind> kinds;
    };
}  // namespace IWXMVM::Types
