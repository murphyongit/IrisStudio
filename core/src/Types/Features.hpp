#pragma once

namespace IWXMVM::Types
{
    enum Features : uint32_t
    {
        Features_None = 0,

        Features_ChangeAnimations = 1 << 0,

        // The filmtweaks panel shows the per-range controls of Black Ops' film system
        Features_FilmtweakRanges = 1 << 1,

        // GetDemoEvents finds the kills of a demo for the timeline
        Features_DemoEvents = 1 << 2,

        // SetRenderSize renders above the window's resolution, for captures
        Features_RenderSize = 1 << 3,

        // The game reports what players can look like (Components::Loadouts::SetCatalog) and applies the choices
        Features_ChangeLoadouts = 1 << 4,

        // The game draws actors (Components::Actors), reports what they can be made of (SetCatalog) and loads content
        // packs (LoadContentPack)
        Features_Actors = 1 << 5,

        // GetSunShadowMapSize and SetSunShadowMapSize change the sun's shadow map (the Look tab's Shadow quality), and
        // sm_sunSampleSizeNear sets how far its shadows reach (Shadow distance)
        Features_ShadowQuality = 1 << 6,

        // GetMapNormalMaps gives the normal maps of the map's surfaces and props, for Surface depth and Bump strength
        // (Components::SurfaceDepth)
        Features_SurfaceDepth = 1 << 7,

        // GetPackImages gives the images the game has registered with the groups of what uses them, for texture packs
        // (Components::TexturePack and the Look tab's Textures)
        Features_TexturePack = 1 << 8,
    };
}  // namespace IWXMVM::Types