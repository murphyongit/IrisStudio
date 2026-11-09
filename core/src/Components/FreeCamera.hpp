#pragma once
#include "Camera.hpp"

namespace IWXMVM::Components
{
    class FreeCamera : public Camera
    {
       public:
        FreeCamera()
        {
            this->mode = Camera::Mode::Free;
        }

        void Initialize() override;
        void Update() override;

        static void HandleFreecamInput(glm::vec3& position, glm::vec3& rotation, float& fov, glm::vec3 forward,
                                       glm::vec3 right);

        // for the harness: the FOV and tilt readouts shown as a scroll shows them, and where they were last drawn
        // (top left and bottom right; the FOV's first)
        static void ShowReadouts();
        static std::array<std::array<ImVec2, 2>, 2> GetReadouts();
    };
}  // namespace IWXMVM::Components