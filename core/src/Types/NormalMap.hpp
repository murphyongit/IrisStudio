#pragma once

namespace IWXMVM::Types
{
    // A normal map of the map's surfaces and props (Components::SurfaceDepth): where the game keeps its texture (which
    // changes as the image streams in and out), its image's name, and what its relief counts for where a material that
    // takes depth uses it (SurfaceLogic::GetReliefWeight), 0 where none does
    struct NormalMap
    {
        IDirect3DBaseTexture9* const* texture = nullptr;
        std::string name;
        float reliefWeight = 0.0f;

        bool operator==(const NormalMap&) const = default;
    };
}  // namespace IWXMVM::Types
