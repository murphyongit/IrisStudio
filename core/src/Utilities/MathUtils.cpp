#include "StdInclude.hpp"
#include "MathUtils.hpp"

#include "UI/UIManager.hpp"

namespace IWXMVM::MathUtils
{
    glm::vec3 ForwardVectorFromAngles(glm::vec3 eulerAngles)
    {
        auto p = glm::radians(eulerAngles[0]);
        auto y = glm::radians(eulerAngles[1]);
        return glm::vec3(std::cos(y) * std::cos(p), std::sin(y) * std::cos(p), -std::sin(p));
    }

    glm::vec3 AnglesFromForwardVector(glm::vec3 forward)
    {
        auto yaw = std::atan2(forward.y, forward.x);
        auto pitch = std::atan2(-forward.z, std::sqrt(forward.x * forward.x + forward.y * forward.y));
        return glm::vec3(glm::degrees(pitch), glm::degrees(yaw), 0.0f);
    }

    constexpr int32_t PITCH = 0;
    constexpr int32_t YAW = 1;
    constexpr int32_t ROLL = 2;

    glm::mat3x3 AnglesToAxis(glm::vec3 angles)
    {
        float sy = sin(glm::radians(angles[YAW]));
        float cy = cos(glm::radians(angles[YAW]));

        float sp = sin(glm::radians(angles[PITCH]));
        float cp = cos(glm::radians(angles[PITCH]));

        float sr = sin(glm::radians(angles[ROLL]));
        float cr = cos(glm::radians(angles[ROLL]));

        glm::vec3 forward(cp * cy, cp * sy, -sp);
        glm::vec3 right(
            (-1 * sr * sp * cy + -1 * cr * -sy), 
            (-1 * sr * sp * sy + -1 * cr * cy), 
            -1 * sr * cp
        );
        glm::vec3 up(
            (cr * sp * cy + -sr * -sy), 
            (cr * sp * sy + -sr * cy),
            cr * cp
        );

        return glm::mat3x3(forward, glm::vector3::zero - right, up);
	}

    glm::vec3 AxisToAngles(glm::mat3x3 axis)
    {
        // axis was constructed as (forward, -right, up)
        glm::vec3 forward = axis[0];
        glm::vec3 negRight = axis[1];
        glm::vec3 right = glm::vector3::zero - negRight;
        glm::vec3 up = axis[2];

        // Pitch from forward.z = -sp => sp = -forward.z
        float sp = -forward.z;
        // Clamp to [-1,1]
        sp = glm::clamp(sp, -1.0f, 1.0f);
        float pitch = glm::degrees(std::asin(sp));

        // Yaw from forward x/y: forward = (cp * cy, cp * sy, -sp)
        float cp = std::cos(glm::radians(pitch));
        float yaw = 0.0f;
        if (std::abs(cp) > 1e-6f)
        {
            yaw = glm::degrees(std::atan2(forward.y / cp, forward.x / cp));
        }

        // Roll: from right and up vectors. Using up.z = cr * cp
        float cr = 1.0f;
        if (std::abs(cp) > 1e-6f)
            cr = up.z / cp;
        cr = glm::clamp(cr, -1.0f, 1.0f);
        float roll = glm::degrees(std::acos(cr));

        // Determine sign of roll using right.z = -sr * cp  (from AnglesToAxis)
        if (std::abs(cp) > 1e-6f)
        {
            float sr = -right.z / cp;
            // If sr is negative, roll should be negative
            if (sr < 0.0f)
                roll = -roll;
        }

        return glm::vec3(pitch, yaw, roll);
    }

    std::optional<ImVec2> WorldToScreenPoint(glm::vec3 point, Components::Camera& camera)
    {
        auto& gameView = UI::UIManager::Get().GetUIComponent(UI::Component::GameView);
        auto viewport = glm::vec4(gameView->GetPosition().x, gameView->GetPosition().y,
                                  gameView->GetPosition().x + gameView->GetSize().x,
                                  gameView->GetPosition().y + gameView->GetSize().y);

        auto lookat =
            glm::lookAtLH(camera.GetPosition(), camera.GetPosition() + camera.GetForwardVector(), glm::vector3::up);

        // this is quite the magic number, but it seems to be the scaling factor necessary to line this up with the
        // previous world to screen implementation
        const auto MAGIC_NUMBER = 0.65f;
        auto proj = glm::project(
            point, (lookat),
            glm::perspectiveFov(glm::radians(camera.GetFov() * 0.65f), viewport.z, viewport.w, 0.1f, 1000.0f),
            viewport);

        return std::make_optional(ImVec2(proj.x, proj.y));
    }

}  // namespace IWXMVM::MathUtils