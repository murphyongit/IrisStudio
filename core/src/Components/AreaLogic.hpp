#pragma once
#include "Types/LevelAreas.hpp"

namespace IWXMVM::Components::AreaLogic
{
    // Going to a level's areas, apart from the game so the tests in tests/actors-tests build
    // it on their own

    // the eye of a person standing on a spot
    inline constexpr float EYE_HEIGHT = 60.0f;

    // the area that many steps on, round the ends; `count` must not be 0
    std::size_t Step(std::size_t current, std::size_t count, int by);

    // the area whose spot is nearest; `areas` must not be empty
    std::size_t FindNearest(std::span<const Types::LevelArea> areas, const glm::vec3& position);

    // The level from above in a rectangle of a size, north up, as much of it as fits within the padding, in its middle
    struct MapView
    {
        glm::vec2 middle{0.0f};  // of the points, in the world
        float scale = 1.0f;      // pixels a unit
        glm::vec2 size{0.0f};
    };
    MapView FitMap(std::span<const glm::vec2> points, glm::vec2 size, float padding);
    // where a point of the world is, from the rectangle's top left
    glm::vec2 ToMap(const MapView& view, glm::vec2 point);
}  // namespace IWXMVM::Components::AreaLogic
