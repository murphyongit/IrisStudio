#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

// The palettes of the launcher and the mod's own UI: neutral studio greys, each a set of named colors that both map
// onto ImGui the same way, so picking one recolors both. Mono, the user's pick, is the
// default. The launcher (launcher/) includes this file too, so it stays free of ImGui and the game.
namespace IWXMVM::UI::Palettes
{
    // a color as 0xRRGGBB
    struct Color
    {
        std::uint32_t rgb = 0;

        constexpr std::uint8_t R() const
        {
            return static_cast<std::uint8_t>(rgb >> 16);
        }
        constexpr std::uint8_t G() const
        {
            return static_cast<std::uint8_t>(rgb >> 8);
        }
        constexpr std::uint8_t B() const
        {
            return static_cast<std::uint8_t>(rgb);
        }
        // as ImGui's IM_COL32 packs it (A B G R from the top byte down), with an alpha of 0-255
        constexpr std::uint32_t Abgr(std::uint8_t alpha = 255) const
        {
            return static_cast<std::uint32_t>(alpha) << 24 | static_cast<std::uint32_t>(B()) << 16 |
                   static_cast<std::uint32_t>(G()) << 8 | R();
        }
        constexpr bool operator==(const Color&) const = default;
    };

    // this color moved toward another by an amount from 0 to 1, each channel rounded
    constexpr Color Mix(Color from, Color to, float amount)
    {
        auto channel = [amount](std::uint8_t a, std::uint8_t b) {
            return static_cast<std::uint32_t>(a + (static_cast<float>(b) - a) * amount + 0.5f);
        };
        return {channel(from.R(), to.R()) << 16 | channel(from.G(), to.G()) << 8 | channel(from.B(), to.B())};
    }

    struct Palette
    {
        std::string_view id;    // as preferences.palette and launcher.json keep it
        std::string_view name;  // as the pickers show it
        Color bg;               // window backgrounds
        Color surface;          // panels, child windows, frames
        Color raised;           // buttons, headers, tabs
        Color border;           // borders, separators
        Color text;
        Color muted;     // disabled text and hints
        Color accent;    // the primary button, selection, sliders, check marks, the active tab
        Color onAccent;  // text on accent-colored buttons
        Color ok;
        Color warn;
        Color bad;

        // the accent under the mouse and held down
        constexpr Color AccentHover() const
        {
            return Mix(accent, {0xFFFFFF}, 0.12f);
        }
        constexpr Color AccentActive() const
        {
            return Mix(accent, {0x000000}, 0.12f);
        }
    };

    // as the mockups have them; warn, which the launcher doesn't draw, a muted amber
    inline constexpr std::array<Palette, 5> ALL = {{
        {"graphite", "Graphite", {0x1E1E1E}, {0x252525}, {0x303030}, {0x3A3A3A}, {0xD6D6D6}, {0x8F8F8F}, {0x2F80D1},
         {0xFFFFFF}, {0x5AAE7A}, {0xD4AA55}, {0xD9534F}},
        {"workshop", "Workshop", {0x2B2B2B}, {0x333333}, {0x3F3F3F}, {0x222222}, {0xE2E2E2}, {0x9C9C9C}, {0xE08A33},
         {0x1C1206}, {0x6DB37F}, {0xD9B45A}, {0xE06055}},
        {"steel", "Steel", {0x23262A}, {0x2B2F34}, {0x363B41}, {0x41474E}, {0xD9DDE2}, {0x939AA3}, {0x6E98C4},
         {0x0E1620}, {0x6DB38A}, {0xD4AA55}, {0xD9625A}},
        {"mono", "Mono", {0x1C1C1C}, {0x242424}, {0x2E2E2E}, {0x383838}, {0xD6D6D6}, {0x8C8C8C}, {0xCFCFCF},
         {0x161616}, {0x7FB58F}, {0xD4AA55}, {0xD46A62}},
        {"sage", "Sage", {0x202221}, {0x282A29}, {0x323534}, {0x3D413F}, {0xD7DBD8}, {0x909893}, {0x7FA58A},
         {0x0F1712}, {0x7FA58A}, {0xD4AA55}, {0xD46A62}},
    }};

    inline constexpr std::string_view DEFAULT_ID = "mono";

    // the palette of an id; none for an id that isn't one
    constexpr std::optional<std::size_t> Find(std::string_view id)
    {
        for (std::size_t i = 0; i < ALL.size(); ++i)
            if (ALL[i].id == id)
                return i;
        return std::nullopt;
    }

    // the palette of an id, the default for one that isn't one
    constexpr const Palette& Get(std::string_view id)
    {
        return ALL[Find(id).value_or(*Find(DEFAULT_ID))];
    }
}  // namespace IWXMVM::UI::Palettes
