#pragma once

#include "Types/NormalMap.hpp"

namespace IWXMVM::T5::Surfaces
{
    // The normal maps of the map's surfaces and props for surface depth: those of
    // the materials of the zone that has the map's world (a campaign level's too), the characters' left out
    std::vector<Types::NormalMap> GetMapNormalMaps();
}  // namespace IWXMVM::T5::Surfaces
