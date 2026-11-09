#pragma once

#include <glm/glm.hpp>

namespace IWXMVM::Types
{
    // Where the parts of a gun are, from its world model at rest. A gun hangs from the hand's tag_weapon_right as it
    // is held (its model's root on the tag), so each part is where it is relative to that tag: x along the barrel, y
    // to the left, z up.
    struct WeaponRig
    {
        std::string model;         // the world model
        std::string magazineBone;  // the bone the magazine is on (tag_clip, or a bigger one); empty for none
        glm::mat4 magazine{1.0f};  // where that bone is
    };
}  // namespace IWXMVM::Types
