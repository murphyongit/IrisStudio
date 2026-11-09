#pragma once

#include "Components/TextureLogic.hpp"

namespace IWXMVM::Types
{
    // An image the game has registered, for a texture pack (Components::TexturePack): its
    // name in lower case, where the game keeps its texture (which changes as it streams in and out), and the group of
    // what uses it (AJ1)
    struct PackImage
    {
        std::string name;
        IDirect3DBaseTexture9* const* texture = nullptr;
        Components::TextureLogic::Group group = Components::TextureLogic::Group::Maps;
    };

    // The images of the map loaded now, with the zones loaded (the map's, common_mp, character zones) and the map's
    struct PackImages
    {
        std::string map;  // its zone: mp_nuked, cuba
        std::vector<std::string> zones;
        std::vector<PackImage> images;
    };
}  // namespace IWXMVM::Types
