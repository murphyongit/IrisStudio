#pragma once
#include <memory>
#include <string>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

namespace IWXMVM::Types
{
    // The parts of a level, to go to (a campaign level's or Zombies map's): each has a spot on
    // the ground and a way to face, in the order a walk from the level's start meets them
    struct LevelArea
    {
        glm::vec3 spot{0.0f};
        float yaw = 0.0f;
    };

    struct LevelAreas
    {
        std::string level;
        std::vector<LevelArea> areas;
        std::vector<glm::vec2> nodes;  // where a person can stand, from above, which draws the level on the map
    };
}  // namespace IWXMVM::Types
