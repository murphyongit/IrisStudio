#pragma once

#include "Types/PackImages.hpp"

namespace IWXMVM::T5::Textures
{
    // The images the game has registered, for a texture pack, each with the group of what
    // uses it (AJ1): the models of weapons (view, world, hands), the killstreaks' and dogs', and the
    // materials of the guns' zone (camos) make Weapons; the characters' models (the map's multiplayer bodies, the
    // campaign's, those of the character zones) Players; the rest the map's. Main thread only.
    Types::PackImages GetPackImages();
}  // namespace IWXMVM::T5::Textures
