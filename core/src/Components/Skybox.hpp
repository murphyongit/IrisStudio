#pragma once

#include <optional>

#include "SkyLogic.hpp"
#include "Types/SkyImage.hpp"

namespace IWXMVM::Components::Skybox
{
    // The Look tab's Sky in the game's drawing (the grade's math is SkyLogic's). Once a frame on the render thread, each
    // cube map of the map's sky (the game's GetSkyImages, read after each demo's load) is drawn again into a cube of the
    // mod's, on the card: every face and level, sampled turned by the grade's rotation and coloured by its rows. The
    // device's SetTexture (SurfaceDepth's hook) hands Swap each texture the game sets, and the sky's comes back as the
    // baked one. Nothing to bake (the grade at its defaults), the game draws its own sky.
    //
    // A cube is baked again when the grade changes (each frame while keys move it, which costs a few draws) or the
    // game's texture does (it streams in, or a texture pack's takes its place: the bake samples what would have been
    // drawn). The baked cubes are in the card's memory only (D3DPOOL_DEFAULT) and go before a device reset.
    //
    // The sky swapper (an HDRI in place of the map's sky) belongs here too: a source of its own in place of the game's
    // cube (a 2D panorama the pixel shader samples by the direction's longitude and latitude), baked by the same passes
    // with the same turn and grade.

    // the map's sky images: another map's replace them, with what was baked for them
    void SetMapSkyImages(std::vector<Types::SkyImage> images);

    // listens for demo loads (the map's sky images) and frames (the bakes, on the render thread)
    void Install();

    // The Look tab's grade (UI::VisualsMenu sets it each frame)
    void SetGrade(const SkyLogic::Grade& grade);
    SkyLogic::Grade GetGrade();

    // On the render thread, from the device's SetTexture: the texture to draw for one the game set (game), drawn being
    // what would be drawn without the sky (a texture pack's in place of the game's, or the game's)
    IDirect3DBaseTexture9* Swap(IDirect3DBaseTexture9* game, IDirect3DBaseTexture9* drawn);

    // before the device resets, on the render thread: the baked cubes go, and are baked again at the next frame
    void OnReset();
    // before the game makes a new device: what was made on the old one goes (the map's images come with the next demo)
    void OnNewDevice();
    // before the mod is ejected, on the render thread: what was made goes, and Swap only passes through
    void Shutdown();

    // what the Look tab's Sky line says
    struct Status
    {
        std::uint32_t images = 0;  // of the map's sky
        std::uint32_t cubes = 0;   // of those, cube maps (the sky itself; the rest are cloud masks and such)
        std::uint32_t baked = 0;   // drawn turned and graded now
        std::uint32_t failed = 0;  // whose bake couldn't be made
        std::string name;          // the first cube's image
        std::uint32_t size = 0;    // its faces', in texels
        bool active = false;       // a grade to draw
        bool shaderFailed = false; // the bake's shaders didn't compile
        std::int32_t format = 0;   // the first cube's D3DFORMAT
        // the baked sky's top face read back at its smallest level (0xRRGGBB), when its format can be read: a check
        // that the bake drew
        std::optional<std::uint32_t> bakedTop;
    };
    Status GetStatus();
}  // namespace IWXMVM::Components::Skybox
