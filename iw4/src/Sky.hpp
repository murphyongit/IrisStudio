#pragma once
#include "StdInclude.hpp"

#include "Types/SkyImage.hpp"

namespace IWXMVM::IW4::Sky
{
    // The images of the loaded map's sky (GfxWorld's skies), for the Look tab's Sky. Empty with no map loaded.
    std::vector<Types::SkyImage> GetSkyImages();
}  // namespace IWXMVM::IW4::Sky
