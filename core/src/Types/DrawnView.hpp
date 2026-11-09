#pragma once

namespace IWXMVM::Types
{
    // Where a frame was drawn from
    struct DrawnView
    {
        glm::vec3 position{0.0f};
        glm::vec3 forward{1.0f, 0.0f, 0.0f};
        glm::vec3 up{0.0f, 0.0f, 1.0f};
        float fov = 90.0f;
        // what the game's projection does with depth, for what is drawn to compare with the frame's (0: nothing known)
        float depthScale = 0.0f;
        float depthOffset = 0.0f;
    };
}  // namespace IWXMVM::Types
