#include "StdInclude.hpp"
#include "CampathManager.hpp"

#include "Mod.hpp"
#include "UI/UIManager.hpp"
#include "UI/Commands.hpp"
#include "UI/Toasts.hpp"
#include "UI/TimelineSelection.hpp"
#include "Playback.hpp"

namespace IWXMVM::Components
{
    namespace
    {
        const Types::KeyframeableProperty& GetCampathProperty()
        {
            return KeyframeManager::Get().GetProperty(Types::KeyframeablePropertyType::CampathCamera);
        }

        std::optional<std::string> NotInDemo()
        {
            if (Mod::GetGameInterface()->GetGameState() != Types::GameState::InDemo)
                return "Load a demo first";
            return std::nullopt;
        }

        // Nodes are placed from where the camera is, in any camera but the dolly, which plays them
        std::optional<std::string> WhyCantPlaceNodes()
        {
            if (auto reason = NotInDemo())
                return reason;
            if (CameraManager::Get().GetActiveCamera()->GetMode() == Camera::Mode::Dolly)
                return "The dolly camera plays the campath: switch to another camera to place nodes";
            return std::nullopt;
        }

        // the node U updates: the one selected on the timeline, else the one at the playhead
        Types::Keyframe* GetNodeToUpdate()
        {
            auto& keyframeManager = KeyframeManager::Get();
            auto& nodes = keyframeManager.GetKeyframes(GetCampathProperty());
            if (const auto selected = UI::TimelineSelection::GetOnlyNode())
            {
                if (const auto it = std::ranges::find(nodes, *selected, &Types::Keyframe::id); it != nodes.end())
                    return &*it;
            }
            const auto tick = Playback::GetTimelineTick();
            const auto it = std::ranges::find(nodes, tick, &Types::Keyframe::tick);
            return it != nodes.end() ? &*it : nullptr;
        }

        Types::CameraData GetCameraData()
        {
            const auto& camera = CameraManager::Get().GetActiveCamera();
            Types::CameraData data;
            data.position = camera->GetPosition();
            data.rotation = camera->GetRotation();
            data.fov = camera->GetFov();
            return data;
        }

        void AddNode()
        {
            const auto& property = GetCampathProperty();
            auto& keyframeManager = KeyframeManager::Get();
            const auto tick = Playback::GetTimelineTick();
            if (std::ranges::find(keyframeManager.GetKeyframes(property), tick, &Types::Keyframe::tick) !=
                keyframeManager.GetKeyframes(property).end())
            {
                UI::Toasts::Show(std::format("There's already a node at {}", UI::Toasts::FormatTime(tick)));
                return;
            }

            const auto node = GetCameraData();
            keyframeManager.AddKeyframe(property, Types::Keyframe(property, tick, node));
            keyframeManager.SortAndSaveKeyframes(keyframeManager.GetKeyframes(property));
            LOG_DEBUG("Placed node at (x: {}; y: {}; z: {}) with (pitch: {}; yaw: {}; roll: {}) at tick {}",
                      node.position.x, node.position.y, node.position.z, node.rotation.x, node.rotation.y,
                      node.rotation.z, tick);
            UI::Toasts::Show(std::format("Node {} added at {}", keyframeManager.GetKeyframes(property).size(),
                                         UI::Toasts::FormatTime(tick)),
                             true);
        }

        void PlayPath()
        {
            auto& cameraManager = CameraManager::Get();
            if (cameraManager.GetActiveCamera()->GetMode() == Camera::Mode::Dolly)
            {
                cameraManager.SetActiveCamera(cameraManager.GetPreviousActiveCamera()->GetMode());
                if (cameraManager.GetActiveCamera()->GetMode() == Camera::Mode::Free)
                    UI::UIManager::Get().GetUIComponent(UI::Component::GameView)->SetHasFocus(true);
                return;
            }

            cameraManager.SetActiveCamera(Camera::Mode::Dolly);
            const auto& nodes = KeyframeManager::Get().GetKeyframes(GetCampathProperty());
            if (!nodes.empty())
                Playback::SetTickDelta(nodes.front().tick - Playback::GetTimelineTick(), true);
        }

        void TogglePov()
        {
            auto& cameraManager = CameraManager::Get();
            if (cameraManager.GetActiveCamera()->GetMode() != Camera::Mode::FirstPerson)
            {
                cameraManager.SetActiveCamera(Camera::Mode::FirstPerson);
                return;
            }
            cameraManager.SetActiveCamera(cameraManager.GetPreviousActiveCamera()->GetMode());
            if (cameraManager.GetActiveCamera()->GetMode() == Camera::Mode::Free)
                UI::UIManager::Get().GetUIComponent(UI::Component::GameView)->SetHasFocus(true);
        }
    }  // namespace

    void CampathManager::ClearNodes()
    {
        const auto& property = GetCampathProperty();
        auto& keyframeManager = KeyframeManager::Get();
        if (keyframeManager.GetKeyframes(property).empty())
            return;
        keyframeManager.ClearKeyframes(property);
        keyframeManager.SortAndSaveKeyframes(keyframeManager.GetKeyframes(property));
        UI::Toasts::Show("Campath cleared", true);
    }

    void CampathManager::Initialize()
    {
        using UI::Commands::Bit;
        using UI::Commands::Context;
        // (K and the others also while flying, which is where nodes are placed from)
        const auto flyingOrNot = Bit(Context::Fly) | Bit(Context::Global);

        UI::Commands::Register({
            .id = "campath.addNode",
            .label = "Add a campath node",
            .category = "Campath",
            .defaults = {ImGuiKey_K},
            .contexts = flyingOrNot,
            .unavailable = WhyCantPlaceNodes,
            .run = AddNode,
            .keywords = "dolly path keyframe camera",
        });
        UI::Commands::Register({
            .id = "campath.updateNode",
            .label = "Update the node to the camera",
            .category = "Campath",
            .defaults = {ImGuiKey_U},
            .contexts = flyingOrNot,
            .unavailable =
                []() -> std::optional<std::string> {
                if (auto reason = WhyCantPlaceNodes())
                    return reason;
                if (!GetNodeToUpdate())
                    return "Select a node on the timeline, or go to one";
                return std::nullopt;
            },
            .run =
                [] {
                    auto& keyframeManager = KeyframeManager::Get();
                    auto node = GetNodeToUpdate();
                    keyframeManager.BeginModifyingKeyframeValue(*node);
                    node->value = GetCameraData();
                    keyframeManager.EndModifyingKeyframeValue(GetCampathProperty(), *node);
                    keyframeManager.SortAndSaveKeyframes(keyframeManager.GetKeyframes(GetCampathProperty()));
                    UI::Toasts::Show(std::format("Node at {} moved to the camera", UI::Toasts::FormatTime(node->tick)),
                                     true);
                },
            .keywords = "dolly path keyframe set move",
        });
        UI::Commands::Register({
            .id = "campath.play",
            .label = "Play the campath (dolly camera) or leave it",
            .category = "Campath",
            .defaults = {ImGuiKey_J},
            .contexts = flyingOrNot,
            .unavailable = NotInDemo,
            .run = PlayPath,
            .keywords = "dolly path preview",
        });
        UI::Commands::Register({
            .id = "campath.clear",
            .label = "Clear the campath",
            .category = "Campath",
            .unavailable =
                []() -> std::optional<std::string> {
                if (auto reason = NotInDemo())
                    return reason;
                if (KeyframeManager::Get().GetKeyframes(GetCampathProperty()).empty())
                    return "The campath has no nodes";
                return std::nullopt;
            },
            .run = [this] { ClearNodes(); },
            .keywords = "dolly path delete remove nodes",
        });
        UI::Commands::Register({
            .id = "campath.constantSpeed",
            .label = "Constant speed along the campath",
            .category = "Campath",
            .unavailable = NotInDemo,
            .run =
                [] {
                    auto& keyframeManager = KeyframeManager::Get();
                    keyframeManager.SetConstantSpeed(!keyframeManager.IsConstantSpeed());
                    UI::Toasts::Show(keyframeManager.IsConstantSpeed() ? "The campath goes at constant speed"
                                                                       : "The campath goes by its nodes' times",
                                     true);
                },
            .keywords = "dolly path retime even speed velocity",
        });
        UI::Commands::Register({
            .id = "camera.togglePov",
            .label = "First person, or back",
            .category = "Camera",
            .defaults = {ImGuiKey_F2},
            .contexts = flyingOrNot,
            .unavailable =
                []() -> std::optional<std::string> {
                if (auto reason = NotInDemo())
                    return reason;
                if (CameraManager::Get().GetActiveCamera()->GetMode() == Camera::Mode::Dolly)
                    return "Not from the dolly camera";
                return std::nullopt;
            },
            .run = TogglePov,
            .keywords = "pov player",
        });
    }
}  // namespace IWXMVM::Components
