#include "StdInclude.hpp"
#include "FreeCamera.hpp"

#include "Input.hpp"
#include "Events.hpp"
#include "Resources.hpp"
#include "UI/UIManager.hpp"
#include "UI/LayoutMath.hpp"
#include "UI/Components/GameView.hpp"
#include "Components/CameraManager.hpp"
#include "Configuration/PreferencesConfiguration.hpp"

namespace IWXMVM::Components
{
    constexpr float PROPERTY_DISPLAY_TIME = 4.0f;
    float fovDisplayTimer = 0.0f;
    float rotationDisplayTimer = 0.0f;
    std::array<std::array<ImVec2, 2>, 2> readouts{};

    int GetAlphaForTimer(float timer)
    {
        return (int)(timer / PROPERTY_DISPLAY_TIME * 2 * 255.0f);
    }

    // A readout in a bottom corner of the picture, which its window can be wider than: drawn past the picture's edge
    // it is cut off
    void DrawReadout(const std::string& label, float timer, bool right, std::array<ImVec2, 2>& drawn)
    {
        const auto view = UI::UIManager::Get().GetUIComponent<UI::GameView>(UI::Component::GameView);
        const auto position = view->GetViewportPosition();
        const auto size = view->GetViewportSize();
        const auto text = ImGui::CalcTextSize(label.c_str());
        const auto [x, y] = UI::LayoutMath::PlaceReadout({position.x, position.y, size.x, size.y}, text.x, text.y,
                                                         ImGui::GetFontSize(), right);
        drawn = {ImVec2(x, y), ImVec2(x, y) + text};
        ImGui::GetWindowDrawList()->AddText(drawn[0], ImColor(255, 255, 255, GetAlphaForTimer(timer)), label.c_str());
    }

    void FreeCamera::Initialize()
    {
        Events::RegisterListener(EventType::OnRenderGameView, [&]() {

            if (CameraManager::Get().GetActiveCamera()->GetMode() != Camera::Mode::Free)
                return;

            if (fovDisplayTimer > 0)
            {
                DrawReadout(std::format(ICON_FA_VIDEO " {0:.0f} ", this->GetFov()), fovDisplayTimer, true, readouts[0]);
                fovDisplayTimer -= Input::GetDeltaTime();
            }

            if (rotationDisplayTimer > 0)
            {
                DrawReadout(std::format(ICON_FA_ARROW_ROTATE_RIGHT " {0:.0f} ", this->GetRotation()[2]),
                            rotationDisplayTimer, false, readouts[1]);
                rotationDisplayTimer -= Input::GetDeltaTime();
            }
        });
    }

    void FreeCamera::ShowReadouts()
    {
        fovDisplayTimer = rotationDisplayTimer = PROPERTY_DISPLAY_TIME;
    }

    std::array<std::array<ImVec2, 2>, 2> FreeCamera::GetReadouts()
    {
        return readouts;
    }

    void FreeCamera::Update()
    {
        if (!UI::UIManager::Get().GetUIComponent(UI::Component::GameView)->HasFocus())
            return;

        HandleFreecamInput(this->position, this->rotation, this->fov, this->GetForwardVector(), this->GetRightVector());
    }

    void FreeCamera::HandleFreecamInput(glm::vec3& position, glm::vec3& rotation, float& fov, glm::vec3 forward, glm::vec3 right)
    {
        auto& preferences = PreferencesConfiguration::Get();

        constexpr float SCROLL_LOWER_BOUNDARY = -0.001f;
        constexpr float SCROLL_UPPER_BOUNDARY = 0.001f;
        const float SMOOTHING_FACTOR = glm::clamp(1.0f - 15.0f * Input::GetDeltaTime(), 0.0f, 1.0f);

        static auto scrollDelta = 0.0f;

        auto speedModifier = UI::Commands::IsHeld("fly.faster") ? 3.0f : 1.0f;
        speedModifier *= UI::Commands::IsHeld("fly.slower") ? 0.1f : 1.0f;

        const auto cameraBaseSpeed = Input::GetDeltaTime() * preferences.freecamSpeed;
        const auto cameraMovementSpeed = cameraBaseSpeed * speedModifier;
        const auto cameraHeightSpeed = cameraMovementSpeed;

        if (UI::Commands::IsHeld("fly.forward"))
        {
            position += forward * cameraMovementSpeed;
        }

        if (UI::Commands::IsHeld("fly.backward"))
        {
            position -= forward * cameraMovementSpeed;
        }

        if (UI::Commands::IsHeld("fly.left"))
        {
            position += right * cameraMovementSpeed;
        }

        if (UI::Commands::IsHeld("fly.right"))
        {
            position -= right * cameraMovementSpeed;
        }

        if (Input::KeyHeld(ImGuiKey_LeftAlt))
        {
            rotation[2] += Input::GetScrollDelta();
            rotationDisplayTimer = PROPERTY_DISPLAY_TIME;
        }
        else
        {
            scrollDelta -= Input::GetScrollDelta();

            if (scrollDelta < SCROLL_LOWER_BOUNDARY || scrollDelta > SCROLL_UPPER_BOUNDARY)
            {
                fov += scrollDelta * Input::GetDeltaTime() * 32.0f;
                fov = glm::clamp(fov, 1.0f, 179.0f);
                scrollDelta *= SMOOTHING_FACTOR;
                fovDisplayTimer = PROPERTY_DISPLAY_TIME;
            }
            else if (scrollDelta != 0.0)
            {
                scrollDelta = 0.0;
            }
        }

        if (UI::Commands::IsHeld("fly.reset"))
        {
            rotation = {};
            fov = 90.0f;
        }

        rotation[0] += Input::GetMouseDelta()[1] * preferences.freecamMouseSpeed * fov / 90;
        rotation[1] -= Input::GetMouseDelta()[0] * preferences.freecamMouseSpeed * fov / 90;
        
        rotation[0] = glm::clamp(rotation[0], -89.9f, 89.9f);

        if (UI::Commands::IsHeld("fly.up"))
            position[2] += cameraHeightSpeed;

        if (UI::Commands::IsHeld("fly.down"))
            position[2] -= cameraHeightSpeed;
    }
}  // namespace IWXMVM::Components
