#include "StdInclude.hpp"
#include "TextureBuild.hpp"

#include <cmath>
#include <cstring>
#include <numbers>

#include "Components/TextureLogic.hpp"

namespace IWXMVM::Setup::TextureBuild
{
    namespace
    {
        namespace TextureLogic = Components::TextureLogic;

        // a 5:6:5 colour to 8 bits a channel
        std::array<std::uint8_t, 3> Expand565(std::uint16_t c)
        {
            const auto r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
            return {static_cast<std::uint8_t>((r << 3) | (r >> 2)), static_cast<std::uint8_t>((g << 2) | (g >> 4)),
                    static_cast<std::uint8_t>((b << 3) | (b >> 2))};
        }

        // the colours of a block: DXT1's punch-through (a black transparent fourth) when c0 <= c1 and allowed
        void DecodeColours(const std::uint8_t* block, bool dxt1, std::array<std::array<std::uint8_t, 4>, 16>& out)
        {
            std::uint16_t c0, c1;
            std::memcpy(&c0, block, 2);
            std::memcpy(&c1, block + 2, 2);
            const auto a = Expand565(c0), b = Expand565(c1);
            std::array<std::array<std::uint8_t, 4>, 4> palette{};
            for (int i = 0; i < 3; ++i)
            {
                palette[0][i] = a[i];
                palette[1][i] = b[i];
                if (c0 > c1 || !dxt1)
                {
                    palette[2][i] = static_cast<std::uint8_t>((2 * a[i] + b[i] + 1) / 3);
                    palette[3][i] = static_cast<std::uint8_t>((a[i] + 2 * b[i] + 1) / 3);
                }
                else
                {
                    palette[2][i] = static_cast<std::uint8_t>((a[i] + b[i]) / 2);
                    palette[3][i] = 0;
                }
            }
            palette[0][3] = palette[1][3] = palette[2][3] = 255;
            palette[3][3] = (c0 > c1 || !dxt1) ? 255 : 0;
            std::uint32_t indices;
            std::memcpy(&indices, block + 4, 4);
            for (int i = 0; i < 16; ++i)
                out[i] = palette[(indices >> (2 * i)) & 3];
        }

        double Lanczos(double x)
        {
            constexpr double LOBES = 3.0;
            x = std::abs(x);
            if (x < 1e-8)
                return 1.0;
            if (x >= LOBES)
                return 0.0;
            const double px = std::numbers::pi * x;
            return LOBES * std::sin(px) * std::sin(px / LOBES) / (px * px);
        }

        struct Tap
        {
            std::uint32_t at;
            float weight;
        };
        // for each texel of the new size, the texels of the old one it is made of
        std::vector<std::vector<Tap>> GetTaps(std::uint32_t from, std::uint32_t to, bool tiling)
        {
            std::vector<std::vector<Tap>> taps(to);
            const double scale = double(to) / from;
            // shrinking widens the filter to cover the texels it takes in
            const double support = scale < 1.0 ? 3.0 / scale : 3.0;
            const double stretch = scale < 1.0 ? scale : 1.0;
            for (std::uint32_t i = 0; i < to; ++i)
            {
                const double center = (i + 0.5) / scale - 0.5;
                const auto first = static_cast<std::int64_t>(std::floor(center - support));
                const auto last = static_cast<std::int64_t>(std::ceil(center + support));
                double total = 0.0;
                for (auto j = first; j <= last; ++j)
                {
                    const double weight = Lanczos((j - center) * stretch);
                    if (weight == 0.0)
                        continue;
                    const auto at = tiling ? ((j % std::int64_t{from}) + from) % from
                                           : std::clamp<std::int64_t>(j, 0, from - 1);
                    taps[i].push_back({static_cast<std::uint32_t>(at), static_cast<float>(weight)});
                    total += weight;
                }
                for (auto& tap : taps[i])
                    tap.weight = static_cast<float>(tap.weight / total);
            }
            return taps;
        }

        std::uint8_t ToByte(float value)
        {
            return static_cast<std::uint8_t>(std::clamp(std::lround(value), 0L, 255L));
        }
    }  // namespace

    Image MakeImage(std::uint32_t width, std::uint32_t height)
    {
        return {width, height, std::vector<std::uint8_t>(std::size_t{width} * height * 4)};
    }

    Image DecodeDxt(std::uint8_t format, std::uint32_t width, std::uint32_t height, std::span<const std::uint8_t> data)
    {
        auto image = MakeImage(width, height);
        const std::size_t blockBytes = format == TextureLogic::FORMAT_DXT1 ? 8 : 16;
        const std::uint32_t blocksWide = std::max(1u, (width + 3) / 4), blocksHigh = std::max(1u, (height + 3) / 4);
        if (data.size() < std::size_t{blocksWide} * blocksHigh * blockBytes)
            return image;
        std::array<std::array<std::uint8_t, 4>, 16> texels{};
        for (std::uint32_t by = 0; by < blocksHigh; ++by)
            for (std::uint32_t bx = 0; bx < blocksWide; ++bx)
            {
                const auto block = data.data() + (std::size_t{by} * blocksWide + bx) * blockBytes;
                const auto colour = format == TextureLogic::FORMAT_DXT1 ? block : block + 8;
                DecodeColours(colour, format == TextureLogic::FORMAT_DXT1, texels);
                if (format == TextureLogic::FORMAT_DXT3)
                {
                    // four bits of alpha each
                    for (int i = 0; i < 16; ++i)
                    {
                        const auto nibble = (block[i / 2] >> ((i % 2) * 4)) & 0xF;
                        texels[i][3] = static_cast<std::uint8_t>(nibble * 17);
                    }
                }
                else if (format == TextureLogic::FORMAT_DXT5)
                {
                    // two ends and three bits each between them
                    const int a0 = block[0], a1 = block[1];
                    std::array<int, 8> alphas{a0, a1};
                    if (a0 > a1)
                        for (int i = 1; i < 7; ++i)
                            alphas[i + 1] = ((7 - i) * a0 + i * a1 + 3) / 7;
                    else
                    {
                        for (int i = 1; i < 5; ++i)
                            alphas[i + 1] = ((5 - i) * a0 + i * a1 + 2) / 5;
                        alphas[6] = 0;
                        alphas[7] = 255;
                    }
                    std::uint64_t bits = 0;
                    for (int i = 0; i < 6; ++i)
                        bits |= std::uint64_t{block[2 + i]} << (8 * i);
                    for (int i = 0; i < 16; ++i)
                        texels[i][3] = static_cast<std::uint8_t>(alphas[(bits >> (3 * i)) & 7]);
                }
                for (int i = 0; i < 16; ++i)
                {
                    const auto x = bx * 4 + i % 4, y = by * 4 + i / 4;
                    if (x < width && y < height)
                        std::memcpy(image.At(x, y), texels[i].data(), 4);
                }
            }
        return image;
    }

    std::uint32_t GetPad(std::uint32_t width, std::uint32_t height)
    {
        return std::min({32u, width / 2, height / 2});
    }

    Image WrapPad(const Image& image, std::uint32_t pad)
    {
        auto out = MakeImage(image.width + pad * 2, image.height + pad * 2);
        for (std::uint32_t y = 0; y < out.height; ++y)
        {
            const auto sy = (y + image.height - pad % image.height) % image.height;
            for (std::uint32_t x = 0; x < out.width; ++x)
            {
                const auto sx = (x + image.width - pad % image.width) % image.width;
                std::memcpy(out.At(x, y), image.At(sx, sy), 4);
            }
        }
        return out;
    }

    Image Crop(const Image& image, std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height)
    {
        auto out = MakeImage(width, height);
        for (std::uint32_t row = 0; row < height; ++row)
            std::memcpy(out.At(0, row), image.At(x, y + row), std::size_t{width} * 4);
        return out;
    }

    Image ToNormal(const Image& stored)
    {
        auto out = MakeImage(stored.width, stored.height);
        for (std::size_t i = 0; i < out.rgba.size(); i += 4)
        {
            const float x = stored.rgba[i + 3] / 255.0f * 2.0f - 1.0f;
            const float y = stored.rgba[i + 1] / 255.0f * 2.0f - 1.0f;
            const float z = std::sqrt(std::clamp(1.0f - x * x - y * y, 0.0f, 1.0f));
            out.rgba[i] = ToByte(x * 127.5f + 127.5f);
            out.rgba[i + 1] = ToByte(y * 127.5f + 127.5f);
            out.rgba[i + 2] = ToByte(z * 127.5f + 127.5f);
            out.rgba[i + 3] = 255;
        }
        return out;
    }

    Image FromNormal(const Image& normal)
    {
        auto out = MakeImage(normal.width, normal.height);
        for (std::size_t i = 0; i < out.rgba.size(); i += 4)
        {
            float x = normal.rgba[i] / 127.5f - 1.0f;
            float y = normal.rgba[i + 1] / 127.5f - 1.0f;
            float z = normal.rgba[i + 2] / 127.5f - 1.0f;
            const float length = std::max(std::sqrt(x * x + y * y + z * z), 1e-6f);
            x /= length;
            y /= length;
            const auto green = ToByte(y * 127.5f + 127.5f);
            out.rgba[i] = out.rgba[i + 1] = out.rgba[i + 2] = green;
            out.rgba[i + 3] = ToByte(x * 127.5f + 127.5f);
        }
        return out;
    }

    Image Halve(const Image& image, bool normal)
    {
        const auto width = std::max(1u, image.width / 2), height = std::max(1u, image.height / 2);
        if (normal)
            return FromNormal(Halve(ToNormal(image), false));
        auto out = MakeImage(width, height);
        for (std::uint32_t y = 0; y < height; ++y)
            for (std::uint32_t x = 0; x < width; ++x)
            {
                const auto x0 = std::min(x * 2, image.width - 1), x1 = std::min(x * 2 + 1, image.width - 1);
                const auto y0 = std::min(y * 2, image.height - 1), y1 = std::min(y * 2 + 1, image.height - 1);
                for (int c = 0; c < 4; ++c)
                    out.At(x, y)[c] = static_cast<std::uint8_t>(
                        (image.At(x0, y0)[c] + image.At(x1, y0)[c] + image.At(x0, y1)[c] + image.At(x1, y1)[c] + 2) / 4);
            }
        return out;
    }

    Image Resize(const Image& image, std::uint32_t width, std::uint32_t height, bool tiling)
    {
        if (width == image.width && height == image.height)
            return image;
        // across, then down, in floats
        const auto across = GetTaps(image.width, width, tiling);
        std::vector<float> middle(std::size_t{width} * image.height * 4);
        for (std::uint32_t y = 0; y < image.height; ++y)
            for (std::uint32_t x = 0; x < width; ++x)
            {
                std::array<float, 4> sum{};
                for (const auto& tap : across[x])
                    for (int c = 0; c < 4; ++c)
                        sum[c] += image.At(tap.at, y)[c] * tap.weight;
                std::memcpy(&middle[(std::size_t{y} * width + x) * 4], sum.data(), sizeof(sum));
            }
        const auto down = GetTaps(image.height, height, tiling);
        auto out = MakeImage(width, height);
        for (std::uint32_t y = 0; y < height; ++y)
            for (std::uint32_t x = 0; x < width; ++x)
            {
                std::array<float, 4> sum{};
                for (const auto& tap : down[y])
                    for (int c = 0; c < 4; ++c)
                        sum[c] += middle[(std::size_t{tap.at} * width + x) * 4 + c] * tap.weight;
                for (int c = 0; c < 4; ++c)
                    out.At(x, y)[c] = ToByte(sum[c]);
            }
        return out;
    }

    bool HasAlpha(const Image& image)
    {
        for (std::size_t i = 3; i < image.rgba.size(); i += 4)
            if (image.rgba[i] != 255)
                return true;
        return false;
    }

    Image WithAlpha(Image colour, const Image& alpha)
    {
        for (std::size_t i = 3; i < colour.rgba.size() && i < alpha.rgba.size(); i += 4)
            colour.rgba[i] = alpha.rgba[i];
        return colour;
    }

    Image Opaque(Image image)
    {
        for (std::size_t i = 3; i < image.rgba.size(); i += 4)
            image.rgba[i] = 255;
        return image;
    }

    namespace
    {
        // the mean of each channel in each cell of a grid
        std::vector<float> GetCells(const Image& image, std::uint32_t across, std::uint32_t down)
        {
            std::vector<double> sums(std::size_t{across} * down * 4);
            std::vector<std::uint32_t> counts(std::size_t{across} * down);
            for (std::uint32_t y = 0; y < image.height; ++y)
            {
                const auto row = static_cast<std::size_t>(y * std::uint64_t{down} / image.height) * across;
                for (std::uint32_t x = 0; x < image.width; ++x)
                {
                    const auto cell = row + static_cast<std::size_t>(x * std::uint64_t{across} / image.width);
                    const auto texel = image.At(x, y);
                    for (int c = 0; c < 4; ++c)
                        sums[cell * 4 + c] += texel[c];
                    ++counts[cell];
                }
            }
            std::vector<float> cells(sums.size());
            for (std::size_t i = 0; i < cells.size(); ++i)
                cells[i] = static_cast<float>(sums[i] / std::max(1u, counts[i / 4]));
            return cells;
        }

        constexpr int LOW_ALPHA = 64;
    }  // namespace

    Difference Compare(const Image& game, const Image& pack)
    {
        Difference difference;
        const auto across = std::min({8u, game.width, pack.width}), down = std::min({8u, game.height, pack.height});
        if (across == 0 || down == 0)
            return difference;
        const auto a = GetCells(game, across, down), b = GetCells(pack, across, down);
        for (std::size_t i = 0; i < a.size(); ++i)
        {
            auto& largest = i % 4 == 3 ? difference.alpha : difference.colour;
            largest = std::max(largest, std::abs(a[i] - b[i]));
        }
        if (game.width != pack.width || game.height != pack.height)
            return difference;
        std::array<double, 3> gameSum{}, packSum{};
        std::size_t count = 0;
        for (std::size_t i = 0; i < game.rgba.size(); i += 4)
            if (game.rgba[i + 3] < LOW_ALPHA)
            {
                for (int c = 0; c < 3; ++c)
                {
                    gameSum[c] += game.rgba[i + c];
                    packSum[c] += pack.rgba[i + c];
                }
                ++count;
            }
        // (a few texels of a level don't show)
        if (count * 100 < game.rgba.size() / 4)
            return difference;
        for (int c = 0; c < 3; ++c)
            difference.underAlpha = std::max(difference.underAlpha,
                                             static_cast<float>(std::abs(gameSum[c] - packSum[c]) / count));
        return difference;
    }

    bool IsBroken(const Image& game, const Image& pack)
    {
        const auto across = std::min({8u, game.width, pack.width}), down = std::min({8u, game.height, pack.height});
        if (across == 0 || down == 0)
            return false;
        // the range of each channel's cells: a flat picture has none
        const auto range = [&](const Image& image) {
            const auto cells = GetCells(image, across, down);
            float largest = 0.0f;
            for (int c = 0; c < 3; ++c)
            {
                float least = 255.0f, most = 0.0f;
                for (std::size_t i = c; i < cells.size(); i += 4)
                {
                    least = std::min(least, cells[i]);
                    most = std::max(most, cells[i]);
                }
                largest = std::max(largest, most - least);
            }
            return largest;
        };
        if (range(pack) < 2.0f && range(game) > 8.0f)
            return true;
        if (game.width != pack.width || game.height != pack.height)
            return false;
        // the colour under low alpha: a quarter of the game's, or less
        double gameSum = 0.0, packSum = 0.0;
        std::size_t count = 0;
        for (std::size_t i = 0; i < game.rgba.size(); i += 4)
            if (game.rgba[i + 3] < LOW_ALPHA)
            {
                for (int c = 0; c < 3; ++c)
                {
                    gameSum += game.rgba[i + c];
                    packSum += pack.rgba[i + c];
                }
                ++count;
            }
        if (count * 100 < game.rgba.size() / 4)
            return false;
        const auto gameMean = gameSum / (count * 3.0), packMean = packSum / (count * 3.0);
        return gameMean > 24.0 && packMean < gameMean / 4.0;
    }

    bool IsWrong(TextureLogic::Kind kind, const Difference& difference)
    {
        if (kind == TextureLogic::Kind::Spec)
            return std::max(difference.colour, difference.alpha) > WRONG_SPEC_LIMIT;
        // (a normal map's alpha is its x)
        return std::max(difference.colour, difference.alpha) > WRONG_LIMIT ||
               (kind != TextureLogic::Kind::Normal && difference.underAlpha > WRONG_LIMIT);
    }
}  // namespace IWXMVM::Setup::TextureBuild
