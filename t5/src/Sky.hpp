#pragma once

#include "Types/SkyImage.hpp"

namespace IWXMVM::T5::Sky
{
    // The images of the map's sky for the Look tab's Sky: those of the materials of the zone that has the map's world
    // (a campaign level's too) whose technique set draws sky, the skybox model's (mc_sky_cubemap_hdr, with its cloud
    // layers _cl1 and _cl2) and the world's sky brushes' (wc_sky)
    std::vector<Types::SkyImage> GetSkyImages();
}  // namespace IWXMVM::T5::Sky
