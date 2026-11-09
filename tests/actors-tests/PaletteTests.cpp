#include "StdInclude.hpp"
#include "Test.hpp"

#include "UI/Palettes.hpp"

#include <set>

using namespace IWXMVM::UI;

// The palettes the launcher and the mod share (UI/Palettes.hpp)

TEST("the five studio greys of the mockups, Mono the default, each with an id of its own")
{
    CHECK(Palettes::ALL.size() == 5);
    CHECK(Palettes::DEFAULT_ID == "mono");
    std::set<std::string_view> ids;
    for (const auto& palette : Palettes::ALL)
        ids.insert(palette.id);
    CHECK(ids.size() == Palettes::ALL.size());
    CHECK(Palettes::Get("mono").accent == Palettes::Color{0xCFCFCF});
    CHECK(Palettes::Get("mono").bg == Palettes::Color{0x1C1C1C});
    CHECK(Palettes::Get("workshop").accent == Palettes::Color{0xE08A33});
    CHECK(Palettes::Get("steel").name == "Steel");
}

TEST("an id that isn't a palette's gives the default")
{
    CHECK(!Palettes::Find("signal").has_value());
    CHECK(Palettes::Find("sage") == 4u);
    CHECK(Palettes::Get("signal").id == "mono");
    CHECK(Palettes::Get("").id == "mono");
}

TEST("a color packs as ImGui's IM_COL32 does and mixes channel by channel")
{
    constexpr Palettes::Color color{0x2F80D1};
    CHECK(color.R() == 0x2F && color.G() == 0x80 && color.B() == 0xD1);
    CHECK(color.Abgr() == 0xFFD1802Fu);
    CHECK(color.Abgr(0x80) == 0x80D1802Fu);
    CHECK(Palettes::Mix({0x000000}, {0xFFFFFF}, 0.5f) == Palettes::Color{0x808080});
    CHECK(Palettes::Mix(color, {0xFFFFFF}, 0.0f) == color);
    CHECK(Palettes::Mix(color, {0x000000}, 1.0f) == Palettes::Color{0x000000});
    // the accent's hover is lighter and its press darker, in every palette
    for (const auto& palette : Palettes::ALL)
    {
        const auto sum = [](Palettes::Color c) { return c.R() + c.G() + c.B(); };
        CHECK(sum(palette.AccentHover()) > sum(palette.accent));
        CHECK(sum(palette.AccentActive()) < sum(palette.accent));
    }
}
