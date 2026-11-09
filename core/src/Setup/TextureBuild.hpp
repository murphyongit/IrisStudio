#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "Components/TextureLogic.hpp"

// The image work of a texture pack's build, as the research's build.py did it: the
// game's DXT texture decoded, wrap-padded so a tiling texture stays seamless through the model, cropped back, a normal
// map rebuilt as an RGB normal for the model and packed back renormalized, the 4x halved for 2x and for the 4,096 cap,
// and spec maps and alpha resized plainly. The mod doesn't build this file; Iris Studio and the unit tests do.
namespace IWXMVM::Setup::TextureBuild
{
    // texels as R, G, B, A bytes, row by row
    struct Image
    {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::vector<std::uint8_t> rgba;

        std::uint8_t* At(std::uint32_t x, std::uint32_t y)
        {
            return rgba.data() + (std::size_t{y} * width + x) * 4;
        }
        const std::uint8_t* At(std::uint32_t x, std::uint32_t y) const
        {
            return rgba.data() + (std::size_t{y} * width + x) * 4;
        }
    };
    Image MakeImage(std::uint32_t width, std::uint32_t height);

    // a level of DXT1, DXT3 or DXT5 (TextureLogic's formats) as texels
    Image DecodeDxt(std::uint8_t format, std::uint32_t width, std::uint32_t height, std::span<const std::uint8_t> data);

    // the padding a texture gets on each side: 32 texels, at most half of a side
    std::uint32_t GetPad(std::uint32_t width, std::uint32_t height);
    // the texture with pad texels of itself around it, as if tiled
    Image WrapPad(const Image& image, std::uint32_t pad);
    Image Crop(const Image& image, std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height);

    // A normal map as the game stores it (x in alpha, y in green, both 0..255 for -1..1) as an RGB normal with z made
    // from them, alpha 255
    Image ToNormal(const Image& stored);
    // and back: the RGB normal renormalized, x in alpha, y in green, red and blue
    Image FromNormal(const Image& normal);

    // half the size, each texel the mean of four (a normal map's renormalized when normal)
    Image Halve(const Image& image, bool normal);
    // a plain resize (Lanczos, 3 lobes), each channel; wraps at the edges when tiling
    Image Resize(const Image& image, std::uint32_t width, std::uint32_t height, bool tiling = true);

    // whether any texel isn't opaque
    bool HasAlpha(const Image& image);
    // the colour of one and the alpha of another of the same size
    Image WithAlpha(Image colour, const Image& alpha);
    // every texel opaque
    Image Opaque(Image image);

    // How far an image of a pack is from the game's: the largest change of the mean of an 8 x 8 grid's cells, in red,
    // green or blue and in alpha, and of the mean colour of the texels the game has under low alpha (0 when the sizes
    // differ or there are too few). Of the same picture at any two sizes.
    struct Difference
    {
        float colour = 0.0f;
        float alpha = 0.0f;
        float underAlpha = 0.0f;
    };
    Difference Compare(const Image& game, const Image& pack);
    // The limits, in levels of 255: what a good build stays within on the whole game. A spec map's levels
    // at the game's size and below are the game's own, so its limit is tight.
    inline constexpr float WRONG_LIMIT = 24.0f;
    inline constexpr float WRONG_SPEC_LIMIT = 6.0f;
    // whether a pack's image of that kind is wrong, not just more detailed
    bool IsWrong(Components::TextureLogic::Kind kind, const Difference& difference);
    // Whether an imported image is broken, whatever else its pack changes (AU1): one flat colour where the game's has a
    // picture, or its colour gone under low alpha. A pack is free to look different; these are what an old build left.
    bool IsBroken(const Image& game, const Image& pack);
}  // namespace IWXMVM::Setup::TextureBuild
