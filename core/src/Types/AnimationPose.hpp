#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace IWXMVM::Types
{
    // A model's bones at a time of an animation, for previews and for working out where the hands and the gun are
    // (see PoseLogic)
    struct AnimationPose
    {
        // in the model's space: x forward, y left, z up, from where it stands
        std::vector<glm::vec3> positions;
        std::vector<glm::quat> rotations;
        // -1 for the bones without one, and for the tags far off a bone with nothing below them, which previews leave
        // out (they are still where they are)
        std::vector<std::int32_t> parents;
        std::vector<std::string> names;
        float duration;  // of the whole animation, in seconds

        // the index of a bone by name, none for a bone the model doesn't have
        std::optional<std::size_t> FindBone(std::string_view name) const
        {
            for (std::size_t i = 0; i < names.size(); ++i)
                if (names[i] == name)
                    return i;
            return std::nullopt;
        }
    };
}  // namespace IWXMVM::Types
