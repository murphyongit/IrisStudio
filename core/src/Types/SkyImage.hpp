#pragma once

namespace IWXMVM::Types
{
    // An image of the map's sky (Components::Skybox): where the game keeps its texture (which changes as the image
    // streams in and out), its image's name, and the material and technique set that draw it as sky, for the Look
    // tab's line and the log
    struct SkyImage
    {
        IDirect3DBaseTexture9* const* texture = nullptr;
        std::string name;
        std::string material;
        std::string techniqueSet;

        bool operator==(const SkyImage&) const = default;
    };
}  // namespace IWXMVM::Types
