#include "StdInclude.hpp"
#include "Test.hpp"

#include "Components/AreaLogic.hpp"

using namespace IWXMVM::Components;
using IWXMVM::Types::LevelArea;

// Going to a level's areas

TEST("areas: previous and next go round the ends")
{
    CHECK(AreaLogic::Step(0, 14, 1) == 1);
    CHECK(AreaLogic::Step(13, 14, 1) == 0);
    CHECK(AreaLogic::Step(0, 14, -1) == 13);
    CHECK(AreaLogic::Step(5, 14, -1) == 4);
    CHECK(AreaLogic::Step(0, 1, 1) == 0);
}

TEST("areas: the one nearest a position")
{
    const std::array<LevelArea, 3> areas{{{{0, 0, 0}}, {{1000, 0, 0}}, {{0, 2000, 0}}}};
    CHECK(AreaLogic::FindNearest(areas, {100, 0, 0}) == 0);
    CHECK(AreaLogic::FindNearest(areas, {900, 0, 0}) == 1);
    CHECK(AreaLogic::FindNearest(areas, {0, 1800, 500}) == 2);
}

TEST("areas: the map fits the level, north up, in its middle")
{
    const std::array<glm::vec2, 2> points{{{-1000, -500}, {3000, 1500}}};
    const auto view = AreaLogic::FitMap(points, {440, 320}, 20);
    // 400 of width for 4000 units, 280 of height for 2000
    CHECK(std::abs(view.scale - 0.1f) < 0.0001f);
    const auto low = AreaLogic::ToMap(view, points[0]);
    const auto high = AreaLogic::ToMap(view, points[1]);
    CHECK(std::abs(low.x - 20.0f) < 0.01f && std::abs(high.x - 420.0f) < 0.01f);
    // north is up: a higher y is nearer the top
    CHECK(high.y < low.y);
    CHECK(std::abs((high.y + low.y) * 0.5f - 160.0f) < 0.01f);
}

TEST("areas: a map of nothing, or of one point, stays usable")
{
    CHECK(AreaLogic::FitMap({}, {440, 320}, 20).scale == 1.0f);
    const std::array<glm::vec2, 1> point{{{500, 500}}};
    const auto view = AreaLogic::FitMap(point, {440, 320}, 20);
    const auto at = AreaLogic::ToMap(view, point[0]);
    CHECK(std::abs(at.x - 220.0f) < 0.01f && std::abs(at.y - 160.0f) < 0.01f);
}
