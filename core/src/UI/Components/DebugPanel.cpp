#include "StdInclude.hpp"
#include "DebugPanel.hpp"

#include "Components/Playback.hpp"
#include "Utilities/HookManager.hpp"
#include "UI/UIManager.hpp"
#include "Mod.hpp"

namespace IWXMVM::UI
{
    void DebugPanel::Initialize()
    {
    }

    void DebugPanel::Render()
    {
        if (ImGui::Begin("Debug", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            const auto demoInfo = Mod::GetGameInterface()->GetDemoInfo();

            ImGui::Text("Game State: %s", Types::ToString(Mod::GetGameInterface()->GetGameState()).data());
            ImGui::Text("Demo Name: %s", demoInfo.name.c_str());
            if (Components::Playback::IsGameFrozen())
            {
                ImGui::Text("Frozen Demo Tick: %d", Components::Playback::GetFrozenTick().value());
            }
            ImGui::Text("Demo Tick: %d", Components::Playback::GetTimelineTick());
            ImGui::Text("Demo End Tick: %d", demoInfo.endTick);

            const auto view = UIManager::Get().GetUIComponent<Timeline>(UI::Component::Timeline)->GetView();
            ImGui::Text("Timeline shows %d to %d", view.start, view.end);

            auto& camera = Components::CameraManager::Get().GetActiveCamera();
            ImGui::Text("Camera: %f %f %f", camera->GetPosition().x, camera->GetPosition().y, camera->GetPosition().z);
            if (ImGui::Button("Eject"))
                Mod::RequestEject();
        }
        ImGui::End();
    }

    void DebugPanel::Release()
    {
    }
}  // namespace IWXMVM::UI