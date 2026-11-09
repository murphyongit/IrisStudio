#pragma once
#include "Types/Dof.hpp"
#include "Types/Filmtweaks.hpp"
#include "Types/Sun.hpp"

namespace IWXMVM::Types
{
    // what a map has of the Look tab's settings, whatever was changed since it loaded
    struct MapVisuals
    {
        Sun sun;
        DoF dof;
        Filmtweaks filmtweaks;
    };
}  // namespace IWXMVM::Types
