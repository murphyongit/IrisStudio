#include "StdInclude.hpp"
#include "CameraMenu.hpp"

#include "Mod.hpp"
#include "Components/Actors.hpp"
#include "Components/BoneCameraLogic.hpp"
#include "UI/UIManager.hpp"
#include "UI/ImGuiEx/ImGuiExtensions.hpp"
#include "Components/CameraManager.hpp"
#include "Components/KillFinder.hpp"
#include "Components/Playback.hpp"
#include "Components/UndoHistory.hpp"
#include "UI/Components/Timeline.hpp"
#include "UI/ImGuiEx/KeyframeableControls.hpp"
#include "UI/TimelineMath.hpp"
#include "UI/TimelineSelection.hpp"
#include "UI/Commands.hpp"
#include "Configuration/PreferencesConfiguration.hpp"
#include "Input.hpp"
#include "Events.hpp"
#include "Utilities/MathUtils.hpp"
#include "Resources.hpp"

namespace IWXMVM::UI
{
    float fov = 90;

    void CameraMenu::Initialize()
    {
    }

    void DrawFirstPersonSettings()
    {
        auto columnPercent = 0.4f;
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Field of View");
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() * columnPercent);
        ImGui::SetNextItemWidth(ImGui::GetWindowWidth() * (1.0f - columnPercent) - ImGui::GetStyle().WindowPadding.x);
        if (ImGui::SliderFloat("##fovSlider", &fov, 45, 160, "%.1f"))
        {
            Mod::GetGameInterface()->SetFov(fov);
        }
    }

    // a row's label in the first column, its control filling the rest
    void BeginRow(const char* label)
    {
        constexpr auto COLUMN_PERCENT = 0.4f;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() * COLUMN_PERCENT);
        ImGui::SetNextItemWidth(ImGui::GetWindowWidth() * (1.0f - COLUMN_PERCENT) - ImGui::GetStyle().WindowPadding.x);
    }

    // a row of the bone camera's that has no keys: nothing where the keyed rows' diamond is, so that the labels line
    // up (as the Look tab's rows without keys)
    void BeginBoneRow(const char* label)
    {
        ImGui::Dummy(ImVec2(ImGui::GetFontSize() * 1.5f, ImGui::GetFrameHeight()));
        ImGui::SameLine();
        BeginRow(label);
    }

    // What the bone camera can follow: the demo's players and the scene's actors
    std::vector<Types::BoneCameraTarget> GetBoneCameraTargets()
    {
        std::vector<Types::BoneCameraTarget> targets;
        if (const auto events = Components::KillFinder::GetEvents())
        {
            for (const auto& [client, player] : events->players)
                targets.push_back({Types::BoneCameraTargetKind::Player, client});
        }
        for (const auto& actor : Components::Actors::GetActors())
            targets.push_back({Types::BoneCameraTargetKind::Actor, static_cast<std::int32_t>(actor.id)});
        return targets;
    }

    std::string DescribeBoneCameraTarget(const Types::BoneCameraTarget& target)
    {
        const auto name = Components::BoneCamera::DescribeTarget(target);
        return target.kind == Types::BoneCameraTargetKind::Player ? name + " (player)" : name;
    }

    std::string DescribeBoneCameraBone(const std::string& bone)
    {
        const auto label = Components::BoneCameraLogic::DescribeBone(bone);
        return label == bone ? bone : std::format("{}  ({})", label, bone);
    }

    std::string ToLower(std::string text)
    {
        std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }

    void DrawBoneCameraSettings()
    {
        auto& boneCamera = static_cast<Components::BoneCamera&>(*Components::CameraManager::Get().GetActiveCamera());
        auto& settings = boneCamera.GetSettings();

        BeginBoneRow("Follows");
        if (ImGui::BeginCombo("##boneCameraTarget", DescribeBoneCameraTarget(settings.target).c_str()))
        {
            const auto targets = GetBoneCameraTargets();
            for (std::size_t i = 0; i < targets.size(); ++i)
            {
                if (i > 0 && targets[i].kind != targets[i - 1].kind)
                    ImGui::Separator();
                const bool selected = targets[i] == settings.target;
                if (ImGui::Selectable(
                        std::format("{}##{}", DescribeBoneCameraTarget(targets[i]), static_cast<int>(i)).c_str(),
                        selected))
                    settings.target = targets[i];
                ImGuiEx::Keyframeable::Record(std::format(
                    "camera.bone.target.{}{}",
                    targets[i].kind == Types::BoneCameraTargetKind::Actor ? "actor" : "player", targets[i].id));
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            if (targets.empty())
                ImGui::TextDisabled("No players or actors yet");
            ImGui::EndCombo();
        }
        ImGuiEx::Keyframeable::Record("camera.bone.target");
        ImGui::SetItemTooltip("A player of the demo (through its body once it died) or an actor");

        // the target's bones, the useful ones first by what they are, the others below with a filter
        const auto bones = Mod::GetGameInterface()->GetTargetBones(settings.target);
        const auto [ordered, named] = Components::BoneCameraLogic::OrderBones(bones);
        BeginBoneRow("Bone");
        static char filter[64] = "";
        if (ImGui::BeginCombo("##boneCameraBone", DescribeBoneCameraBone(settings.bone).c_str(),
                              ImGuiComboFlags_HeightLarge))
        {
            if (ImGui::IsWindowAppearing())
            {
                filter[0] = '\0';
                ImGui::SetKeyboardFocusHere();
            }
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputTextWithHint("##boneCameraBoneFilter", "Filter", filter, sizeof(filter));
            const auto lowerFilter = ToLower(filter);
            for (std::size_t i = 0; i < ordered.size(); ++i)
            {
                const auto label = DescribeBoneCameraBone(ordered[i]);
                if (!lowerFilter.empty() && ToLower(label).find(lowerFilter) == std::string::npos)
                    continue;
                if (i == named && named > 0 && lowerFilter.empty())
                    ImGui::Separator();
                const bool selected = ordered[i] == settings.bone;
                if (ImGui::Selectable(label.c_str(), selected))
                    settings.bone = ordered[i];
                ImGuiEx::Keyframeable::Record("camera.bone.bone." + ordered[i]);
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            if (ordered.empty())
                ImGui::TextDisabled("Its bones show once it is drawn");
            ImGui::EndCombo();
        }
        ImGuiEx::Keyframeable::Record("camera.bone.bone");

        if (!Mod::GetGameInterface()->GetTargetBone(settings.target, settings.bone))
        {
            const bool lacksBone = !bones.empty() && std::ranges::find(bones, settings.bone) == bones.end();
            ImGui::TextDisabled(ICON_FA_TRIANGLE_EXCLAMATION " %s",
                                lacksBone ? "It has no such bone" : "It isn't in the demo now");
        }

        // the offsets and FOV are keyed on the timeline: with keys a row shows theirs at
        // the playhead and a change keys it there
        ImGui::Dummy(ImVec2(0, 4));
        Types::KeyframeValue offset(settings.positionOffset);
        ImGuiEx::Keyframeable::Row(
            "Offset", Types::KeyframeablePropertyType::BoneCameraOffset, offset,
            [](const char* id, Types::KeyframeValue& value) {
                const bool changed = ImGui::DragFloat3(id, &value.vector3.x, 0.25f, -10000, 10000, "%.1f");
                ImGuiEx::Keyframeable::Record("camera.bone.offset");
                ImGui::SetItemTooltip("Forward, left and up from the bone (from where it faces, with Turn with: "
                                      "Where it faces)");
                return changed;
            },
            "Bone Camera Offset");
        settings.positionOffset = offset.vector3;
        Types::KeyframeValue rotation(settings.rotationOffset);
        ImGuiEx::Keyframeable::Row(
            "Rotation", Types::KeyframeablePropertyType::BoneCameraRotation, rotation,
            [](const char* id, Types::KeyframeValue& value) {
                const bool changed = ImGui::DragFloat3(id, &value.vector3.x, 0.5f, -180, 180, "%.1f");
                ImGuiEx::Keyframeable::Record("camera.bone.rotation");
                ImGui::SetItemTooltip("Pitch, yaw and roll from the bone's own axes");
                return changed;
            },
            "Bone Camera Rotation");
        settings.rotationOffset = rotation.vector3;
        Types::KeyframeValue fov(settings.fov);
        ImGuiEx::Keyframeable::Row(
            "Field of View", Types::KeyframeablePropertyType::BoneCameraFov, fov,
            [](const char* id, Types::KeyframeValue& value) {
                const bool changed = ImGui::DragFloat(id, &value.floatingPoint, 0.5f, 1, 160, "%.0f",
                                                      ImGuiSliderFlags_AlwaysClamp);
                ImGuiEx::Keyframeable::Record("camera.bone.fov");
                return changed;
            },
            "Bone Camera FOV");
        settings.fov = fov.floatingPoint;

        BeginBoneRow("Steady");
        ImGui::SliderFloat("##boneCameraSteady", &settings.steady, 0.0f, 0.5f,
                           settings.steady > 0.0f ? "%.2f s" : "Off", ImGuiSliderFlags_AlwaysClamp);
        ImGuiEx::Keyframeable::Record("camera.bone.steady");
        ImGui::SetItemTooltip("Smooths the bone's sway (a head's bob) over this long, in its target's own space, so "
                              "the camera doesn't fall behind as it moves. A recording starts from the bone as it is.");

        static constexpr std::array<std::pair<Types::BoneCameraTurn, const char*>, 3> TURNS = {{
            {Types::BoneCameraTurn::Bone, "The bone"},
            {Types::BoneCameraTurn::Level, "The bone, level"},
            {Types::BoneCameraTurn::Facing, "Where it faces"},
        }};
        BeginBoneRow("Turn with");
        const auto turn = std::ranges::find_if(TURNS, [&](const auto& entry) { return entry.first == settings.turn; });
        if (ImGui::BeginCombo("##boneCameraTurn", turn != TURNS.end() ? turn->second : ""))
        {
            for (const auto& [value, label] : TURNS)
            {
                if (ImGui::Selectable(label, value == settings.turn))
                    settings.turn = value;
                ImGuiEx::Keyframeable::Record(std::format("camera.bone.turn.{}", static_cast<int>(value)));
            }
            ImGui::EndCombo();
        }
        ImGuiEx::Keyframeable::Record("camera.bone.turn");
        ImGui::SetItemTooltip("The bone: as the bone turns. The bone, level: so, with the horizon level. Where it "
                              "faces: only as its target turns the way it goes, never with the bone's sway.");

        BeginBoneRow("Show Bone");
        ImGui::Checkbox("##boneCameraShowBone", &settings.showBone);
        ImGuiEx::Keyframeable::Record("camera.bone.showbone");

        // (rows with keys keyed so at the playhead)
        if (ImGui::Button("Reset offsets"))
            boneCamera.ResetOffsets();
        ImGuiEx::Keyframeable::Record("camera.bone.reset");
        ImGui::TextDisabled("Fly to move it on the bone.");
    }

    void DrawThirdPersonSettings()
    {
        ImGui::TextDisabled("Follows the player from behind.");
    }

    void DrawDollycamSettings()
    {
        ImGui::TextWrapped("Plays the campath.");
    }

    // a speed the camera tab keeps in config.json, saved once a drag or typing ends
    void PreferenceRow(const char* label, const char* id, float* value, float speed, float min, float max,
                       const char* format)
    {
        const auto columnPercent = 0.4f;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() * columnPercent);
        ImGui::SetNextItemWidth(ImGui::GetWindowWidth() * (1.0f - columnPercent) - ImGui::GetStyle().WindowPadding.x);
        ImGui::DragFloat(id, value, speed, min, max, format, ImGuiSliderFlags_AlwaysClamp);
        if (ImGui::IsItemDeactivatedAfterEdit())
            Configuration::Get().Write(true);
    }

    void DrawFreeCameraSettings()
    {
        auto& preferences = PreferencesConfiguration::Get();
        PreferenceRow("Speed", "##freeCameraSpeed", &preferences.freecamSpeed, 1.0f, 1.0f, 500.0f, "%.0f");
        ImGui::SetItemTooltip("How fast it flies; Shift and Ctrl held go faster and slower");
        PreferenceRow("Mouse sensitivity", "##freeCameraMouse", &preferences.freecamMouseSpeed, 0.01f, 0.01f, 5.0f,
                      "%.2f");
        ImGui::TextDisabled("Fly: hold the right mouse button in the view, or press %s.",
                            Commands::GetShortcut("fly.toggle").c_str());
    }

    void DrawOrbitCameraSettings()
    {
        auto& preferences = PreferencesConfiguration::Get();
        PreferenceRow("Rotation speed", "##orbitRotation", &preferences.orbitRotationSpeed, 0.01f, 0.01f, 5.0f, "%.2f");
        PreferenceRow("Movement speed", "##orbitMove", &preferences.orbitMoveSpeed, 0.01f, 0.01f, 5.0f, "%.2f");
        PreferenceRow("Zoom speed", "##orbitZoom", &preferences.orbitZoomSpeed, 0.01f, 0.01f, 5.0f, "%.2f");
        ImGui::TextDisabled("Right drag orbits, Shift+right drag pans, the wheel zooms.");
    }

    // the camera modes as buttons, three a row, with their keys
    void DrawModes()
    {
        struct Mode
        {
            Components::Camera::Mode mode;
            const char* label;
            std::string_view command;
            const char* name;  // for the harness
        };
        static constexpr std::array<Mode, 6> MODES = {{
            {Components::Camera::Mode::FirstPerson, "First person", "camera.firstPerson", "first"},
            {Components::Camera::Mode::ThirdPerson, "Third person", "camera.thirdPerson", "third"},
            {Components::Camera::Mode::Free, "Free", "camera.free", "free"},
            {Components::Camera::Mode::Orbit, "Orbit", "camera.orbit", "orbit"},
            {Components::Camera::Mode::Dolly, "Dolly", "camera.dolly", "dolly"},
            {Components::Camera::Mode::Bone, "Bone", "camera.bone", "bone"},
        }};
        const auto current = Components::CameraManager::Get().GetActiveCamera()->GetMode();
        const auto spacing = ImGui::GetStyle().ItemSpacing.x;
        const auto width = std::floor((ImGui::GetContentRegionAvail().x - spacing * 2) / 3);
        for (std::size_t i = 0; i < MODES.size(); i++)
        {
            const auto& mode = MODES[i];
            if (i % 3 != 0)
                ImGui::SameLine();
            const bool selected = mode.mode == current;
            if (selected)
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::Button(mode.label, ImVec2(width, 0)))
                Commands::Run(mode.command);
            if (selected)
                ImGui::PopStyleColor();
            ImGuiEx::Keyframeable::Record(std::format("camera.mode.{}", mode.name));
            const auto key = Commands::GetShortcut(mode.command);
            if (!key.empty())
                ImGui::SetItemTooltip("%s (%s)", Commands::Find(mode.command)->label.data(), key.c_str());
        }
    }

    // A button that runs a command, with its key; greyed out with why when it can't run now
    void CommandButton(const char* label, std::string_view id, const char* name, ImVec2 size = ImVec2(0, 0))
    {
        const auto reason = Commands::GetUnavailableReason(id);
        ImGui::BeginDisabled(reason.has_value());
        if (ImGui::Button(label, size))
            Commands::Run(id);
        ImGui::EndDisabled();
        ImGuiEx::Keyframeable::Record(name);
        const auto key = Commands::GetShortcut(id);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("%s%s%s", Commands::Find(id)->label.data(),
                              key.empty() ? "" : std::format(" ({})", key).c_str(),
                              reason ? std::format("\n{}", *reason).c_str() : "");
    }

    // The campath's nodes, one a row: a click selects one and goes there; its buttons go there, set it to the camera
    // and delete it (each one step with a message, as on the timeline)
    // A node's ease in or out in the node list: how much, marked Auto when it follows Auto Ease,
    // or a dash for none
    void DrawNodeEase(const Types::Keyframe& node, std::size_t index, bool in)
    {
        const auto amount = in ? node.easeIn : node.easeOut;
        const bool follows = in ? node.autoEaseIn : node.autoEaseOut;
        ImGui::AlignTextToFramePadding();
        if (amount <= 0.0f)
        {
            ImGui::TextDisabled("-");
            ImGui::SetItemTooltip("No ease %s", in ? "in" : "out");
            return;
        }
        ImGui::BeginGroup();
        ImGui::Text("%.0f%%", amount * 100.0f);
        if (follows)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("Auto");
            ImGuiEx::Keyframeable::Record(std::format("node.{}.ease{}.auto", index, in ? "in" : "out"));
        }
        ImGui::EndGroup();
        ImGui::SetItemTooltip("Ease %s %.0f%%: %s", in ? "in" : "out", amount * 100.0f,
                              follows ? "Auto Ease's" : "the node's own");
    }

    void DrawNodes(const std::vector<Types::Keyframe>& nodes)
    {
        const auto& property =
            Components::KeyframeManager::Get().GetProperty(Types::KeyframeablePropertyType::CampathCamera);
        const auto select = [&](const Types::Keyframe& node) {
            const auto item = TimelineSelection::KeyItem(property.type, node.id);
            TimelineSelection::Set(std::span(&item, 1));
            TimelineSelection::SetAnchor(item);
        };
        const auto tick = Components::Playback::GetTimelineTick();
        const auto buttonWidth = ImGui::GetFrameHeight();
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp |
                                      ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersOuterH;
        // (a row more for the headers)
        const auto rows = std::min<std::size_t>(nodes.size(), 8) + 1;
        const auto height = (ImGui::GetFrameHeight() + ImGui::GetStyle().CellPadding.y * 2) * rows + 2;
        if (!ImGui::BeginTable("##campathNodes", 5, flags, ImVec2(0, height)))
            return;
        const auto easeWidth = ImGui::CalcTextSize("100% Auto").x;
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Node", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("At", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Ease in", ImGuiTableColumnFlags_WidthFixed, easeWidth);
        ImGui::TableSetupColumn("Ease out", ImGuiTableColumnFlags_WidthFixed, easeWidth);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed,
                                buttonWidth * 3 + ImGui::GetStyle().ItemSpacing.x * 2);
        ImGui::TableHeadersRow();

        std::optional<Types::Keyframe> toDelete;
        for (std::size_t i = 0; i < nodes.size(); i++)
        {
            const auto& node = nodes[i];
            ImGui::PushID(node.id);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const bool selected = TimelineSelection::IsSelected(TimelineSelection::KeyItem(property.type, node.id));
            const auto label = std::format("{}Node {}", node.tick == tick ? ICON_FA_CARET_RIGHT " " : "", i + 1);
            if (ImGui::Selectable(label.c_str(), selected,
                                  ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
                                  ImVec2(0, ImGui::GetFrameHeight())))
            {
                select(node);
                Seek(node.tick, false);
            }
            ImGuiEx::Keyframeable::Record(std::format("node.{}", i));
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(TimelineMath::FormatTime(node.tick).c_str());
            ImGui::TableNextColumn();
            DrawNodeEase(node, i, true);
            ImGui::TableNextColumn();
            DrawNodeEase(node, i, false);
            ImGui::TableNextColumn();
            if (ImGui::Button(ICON_FA_ARROW_RIGHT, ImVec2(buttonWidth, 0)))
                Seek(node.tick, false);
            ImGuiEx::Keyframeable::Record(std::format("node.{}.goto", i));
            ImGui::SetItemTooltip("Go to it");
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_CAMERA_ROTATE, ImVec2(buttonWidth, 0)))
            {
                select(node);
                Commands::Run("campath.updateNode");
            }
            ImGuiEx::Keyframeable::Record(std::format("node.{}.update", i));
            ImGui::SetItemTooltip("Set it to where the camera is (%s with it selected)",
                                  Commands::GetShortcut("campath.updateNode").c_str());
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_TRASH_CAN, ImVec2(buttonWidth, 0)))
                toDelete = node;
            ImGuiEx::Keyframeable::Record(std::format("node.{}.delete", i));
            ImGui::SetItemTooltip("Delete it");
            ImGui::PopID();
        }
        ImGui::EndTable();

        // (after the table: deleting changes the nodes it went through)
        if (toDelete)
        {
            select(*toDelete);
            TimelineSelection::Delete();
        }
    }

    // The campath, under every camera's settings: the node at the playhead with its diamond, its position, angles and
    // field of view (changing them keys them there, as on the Visuals tab), its interpolation and eases, and how fast
    // the camera goes along the path
    void DrawCampath()
    {
        const auto columnPercent = 0.4f;
        auto& keyframeManager = Components::KeyframeManager::Get();
        const auto& property = keyframeManager.GetProperty(Types::KeyframeablePropertyType::CampathCamera);
        auto& nodes = keyframeManager.GetKeyframes(property);
        const auto tick = Components::Playback::GetTimelineTick();

        if (!ImGuiEx::Keyframeable::PanelSection(ICON_FA_VIDEO "  Campath", "campath"))
            return;

        // a node made here from the view (as K does), or the path's values here
        const auto& camera = Components::CameraManager::Get().GetActiveCamera();
        Types::KeyframeValue value = nodes.empty()
                                         ? Types::KeyframeValue(Types::CameraData{
                                               camera->GetPosition(), camera->GetRotation(), camera->GetFov()})
                                         : keyframeManager.Interpolate(property, tick);
        // the node here, found again after what can add one (the diamond, + Node, a value changed): an added node can
        // move the nodes, and the first one is where nothing was, so one found before is no node at all
        const auto findHere = [&] { return std::ranges::find(nodes, tick, &Types::Keyframe::tick); };

        ImGui::PushID("campath");
        ImGuiEx::Keyframeable::Diamond(property, value, "The campath", ImGui::GetFontSize() * 1.5f);
        auto here = findHere();
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        if (nodes.empty())
            ImGui::TextUnformatted("No nodes yet");
        else if (here != nodes.end())
            ImGui::Text("Node %d of %zu, at %s", static_cast<int>(here - nodes.begin()) + 1, nodes.size(),
                        TimelineMath::FormatTime(tick).c_str());
        else
            ImGui::Text("Between nodes, at %s", TimelineMath::FormatTime(tick).c_str());
        if (!nodes.empty())
        {
            // the nodes before and after
            ImGui::SameLine();
            const auto before = std::ranges::find_if(nodes.rbegin(), nodes.rend(),
                                                     [&](const Types::Keyframe& node) { return node.tick < tick; });
            const auto after =
                std::ranges::find_if(nodes, [&](const Types::Keyframe& node) { return node.tick > tick; });
            ImGui::SetCursorPosX(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x -
                                 ImGui::GetFrameHeight() * 2 - ImGui::GetStyle().ItemSpacing.x);
            ImGui::BeginDisabled(before == nodes.rend());
            if (ImGui::ArrowButton("##before", ImGuiDir_Left))
                Seek(before->tick, false);
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("Go to the node before");
            ImGui::SameLine();
            ImGui::BeginDisabled(after == nodes.end());
            if (ImGui::ArrowButton("##after", ImGuiDir_Right))
                Seek(after->tick, false);
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("Go to the node after");
        }

        // nodes added, set to the camera, played
        const auto spacing = ImGui::GetStyle().ItemSpacing.x;
        const auto third = std::floor((ImGui::GetContentRegionAvail().x - spacing * 2) / 3);
        CommandButton(ICON_FA_PLUS " Node", "campath.addNode", "campath.add", ImVec2(third, 0));
        ImGui::SameLine();
        CommandButton(ICON_FA_CAMERA_ROTATE " Update", "campath.updateNode", "campath.update", ImVec2(third, 0));
        ImGui::SameLine();
        CommandButton(ICON_FA_PLAY " Play", "campath.play", "campath.play", ImVec2(third, 0));

        if (nodes.empty())
        {
            ImGui::TextDisabled("Fly and press %s.", Commands::GetShortcut("campath.addNode").c_str());
            ImGui::PopID();
            return;
        }

        // its values: changing one keys it here
        const auto row = [&](const char* label, const char* id, float* values, int count, float speed) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label);
            ImGui::SameLine();
            ImGui::SetCursorPosX(ImGui::GetWindowWidth() * columnPercent);
            ImGui::SetNextItemWidth(ImGui::GetWindowWidth() * (1.0f - columnPercent) -
                                    ImGui::GetStyle().WindowPadding.x);
            const bool changed = count == 3 ? ImGui::DragFloat3(id, values, speed, 0.0f, 0.0f, "%.1f")
                                            : ImGui::DragFloat(id, values, speed, 1.0f, 179.0f, "%.1f");
            ImGuiEx::Keyframeable::Record(std::format("control.CampathCamera.{}", id + 2));
            if (changed)
                ImGuiEx::Keyframeable::KeyValueHere(property, value, "the campath");
        };
        row("Position", "##position", glm::value_ptr(value.cameraData.position), 3, 1.0f);
        row("Angles", "##angles", glm::value_ptr(value.cameraData.rotation), 3, 0.25f);
        row("Field of View", "##fov", &value.cameraData.fov, 1, 0.25f);
        ImGuiEx::Keyframeable::EndEditWhenIdle();

        // the node's way to the next one
        here = findHere();
        if (here != nodes.end())
        {
            const auto change = [&](const std::function<void(Types::Keyframe&)>& edit, std::string label) {
                auto changed = *here;
                edit(changed);
                keyframeManager.ChangeKeyframes(property, {changed}, std::move(label));
            };
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Interpolation");
            ImGui::SameLine();
            ImGui::SetCursorPosX(ImGui::GetWindowWidth() * columnPercent);
            ImGui::SetNextItemWidth(ImGui::GetWindowWidth() * (1.0f - columnPercent) -
                                    ImGui::GetStyle().WindowPadding.x);
            const auto current = here->interpolation;
            if (ImGui::BeginCombo("##interpolation", std::string(magic_enum::enum_name(current)).c_str()))
            {
                // (Auto, which new nodes have, too)
                for (const auto interpolation : {Types::KeyInterpolation::Smooth, Types::KeyInterpolation::Linear,
                                                 Types::KeyInterpolation::Hold, Types::KeyInterpolation::Auto})
                {
                    const auto name = magic_enum::enum_name(interpolation);
                    if (ImGui::Selectable(std::string(name).c_str(), current == interpolation) &&
                        current != interpolation)
                        change([interpolation](Types::Keyframe& node) { node.interpolation = interpolation; },
                               std::format("make the node {}", name));
                }
                ImGui::EndCombo();
            }
            ImGuiEx::Keyframeable::Record("control.interpolation");
            ImGui::SetItemTooltip(
                "How the camera goes on from this node to the next: Smooth (a curve that doesn't "
                "overshoot the nodes), Linear, Hold (stays until the next node), or Auto, as the camera always "
                "moved and new nodes do: straight under four nodes, a curve through all from four");

            // how much, a drag of the amount one step; after it, Auto when it is Auto Ease's, or on a side Auto Ease
            // eases (an end of the path; with All Nodes every side between two nodes) whose ease is the node's own, a
            // button that gives it back
            const auto autoEase = keyframeManager.GetAutoEase();
            const bool allNodes = autoEase.nodes == Types::AutoEaseNodes::AllNodes;
            const auto markWidth = ImGui::CalcTextSize("Auto").x + ImGui::GetStyle().FramePadding.x * 2;
            const auto index = static_cast<std::size_t>(here - nodes.begin());
            for (const bool in : {true, false})
            {
                const auto end = in ? "in" : "out";
                // the tick where the labels are, the slider where the controls are
                ImGuiEx::Keyframeable::Ease(
                    in ? "Ease in" : "Ease out", in ? here->easeIn : here->easeOut,
                    ImGui::GetWindowWidth() * (1.0f - columnPercent) - ImGui::GetStyle().WindowPadding.x - markWidth -
                        ImGui::GetStyle().ItemSpacing.x,
                    ImGui::GetWindowWidth() * columnPercent, std::format("change the ease {} of the node", end),
                    [&](float amount, bool) {
                        change([&](Types::Keyframe& node) { (in ? node.easeIn : node.easeOut) = amount; },
                               amount > 0.0f ? std::format("ease {} the node", end)
                                             : std::format("no ease {} on the node", end));
                    },
                    in ? "Slow down to rest arriving at the node" : "Start from rest leaving the node",
                    std::format("control.ease{}", end), ImGuiEx::Keyframeable::Record);
                const bool follows = in ? here->autoEaseIn : here->autoEaseOut;
                const bool pathEnd = in ? index + 1 == nodes.size() : index == 0;
                const bool between = in ? index > 0 : index + 1 < nodes.size();
                const bool eased = pathEnd || (allNodes && between);
                if (!autoEase.on || (follows && (in ? here->easeIn : here->easeOut) <= 0.0f) || (!follows && !eased))
                    continue;
                ImGui::SameLine();
                if (follows)
                {
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("Auto");
                    ImGuiEx::Keyframeable::Record(std::format("control.ease{}.auto", end));
                    ImGui::SetItemTooltip(
                        "Auto Ease's (at the bottom of the tab); a change here makes it the node's "
                        "own");
                    continue;
                }
                if (ImGui::Button(std::format("Auto##ease{}", end).c_str(), ImVec2(markWidth, 0)))
                    change([&](Types::Keyframe& node) { (in ? node.autoEaseIn : node.autoEaseOut) = true; },
                           std::format("give the ease {} back to Auto Ease", end));
                ImGuiEx::Keyframeable::Record(std::format("control.ease{}.giveback", end));
                ImGui::SetItemTooltip("Give it back to Auto Ease: %.0f%%",
                                      (in ? autoEase.end : autoEase.start) * 100.0f);
            }
        }

        // the whole path
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Constant speed");
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() * columnPercent);
        bool constant = keyframeManager.IsConstantSpeed();
        if (ImGui::Checkbox("##constantSpeed", &constant))
            Commands::Run("campath.constantSpeed");
        ImGuiEx::Keyframeable::Record("control.constantSpeed");
        ImGui::SetItemTooltip(
            "The camera goes along the path at one speed from the first node to the last, "
            "whenever the nodes between are (the first node's ease out and the last's ease in "
            "still slow its ends)");
        if (nodes.size() >= 2)
        {
            const auto seconds = (nodes.back().tick - nodes.front().tick) / 1000.0;
            const auto length = keyframeManager.GetCampathLength();
            ImGui::TextDisabled("%zu nodes, %.2f s, %.0f units long%s", nodes.size(), seconds, length,
                                constant && seconds > 0 ? std::format(", {:.0f} u/s", length / seconds).c_str() : "");
        }
        else
        {
            ImGui::TextDisabled("One node: add another for a path.");
        }

        ImGui::Spacing();
        DrawNodes(nodes);
        ImGui::PopID();
    }

    // Auto Ease, at the bottom of the tab: new campaths ease out of their first node and
    // into their last by the amounts set, or out of and into every node (6.5); on or off, which nodes and the amounts
    // are preferences, and the campath's nodes that follow it follow them (KeyframeManager::SetAutoEase). A change is
    // one step, a slider's drag too.
    void DrawAutoEase()
    {
        static bool dragging = false;
        auto& keyframeManager = Components::KeyframeManager::Get();
        auto autoEase = keyframeManager.GetAutoEase();
        const auto columnPercent = 0.4f;

        if (!ImGuiEx::Keyframeable::PanelSection(ICON_FA_WAND_MAGIC_SPARKLES "  Auto Ease", "autoease"))
            return;

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("On");
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() * columnPercent);
        if (ImGui::Checkbox("##autoEase", &autoEase.on))
            keyframeManager.SetAutoEase(autoEase, autoEase.on ? "Auto Ease on" : "Auto Ease off");
        ImGuiEx::Keyframeable::Record("control.autoease");
        const bool allNodes = autoEase.nodes == Types::AutoEaseNodes::AllNodes;
        ImGui::SetItemTooltip(allNodes ? "Campaths ease out of every node and into every node; off, new nodes get none "
                                         "and eases already on nodes stay"
                                       : "New campaths ease out of their first node and into their last; off, they get "
                                         "none and eases already on nodes stay");

        ImGui::BeginDisabled(!autoEase.on);
        // which nodes: the first and last, or all of them
        {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Nodes");
            ImGui::SameLine();
            ImGui::SetCursorPosX(ImGui::GetWindowWidth() * columnPercent);
            const auto spacing = ImGui::GetStyle().ItemSpacing.x;
            const auto width = std::floor(
                (ImGui::GetWindowWidth() * (1.0f - columnPercent) - ImGui::GetStyle().WindowPadding.x - spacing) / 2);
            struct Choice
            {
                Types::AutoEaseNodes nodes;
                const char* label;
                const char* tooltip;
                const char* record;
                const char* step;
            };
            static constexpr std::array<Choice, 2> CHOICES{{
                {Types::AutoEaseNodes::FirstAndLast, "First and Last",
                 "Eases out of the campath's first node and into its last", "control.autoease.firstandlast",
                 "Auto Ease on the first and last nodes"},
                {Types::AutoEaseNodes::AllNodes, "All Nodes",
                 "Eases out of every node and into every node, nodes added later too; an ease changed on a node stays "
                 "its own",
                 "control.autoease.allnodes", "Auto Ease on all nodes"},
            }};
            for (std::size_t i = 0; i < CHOICES.size(); ++i)
            {
                const auto& choice = CHOICES[i];
                if (i > 0)
                    ImGui::SameLine();
                const bool selected = autoEase.nodes == choice.nodes;
                if (selected)
                    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                if (ImGui::Button(choice.label, ImVec2(width, 0)) && !selected)
                {
                    autoEase.nodes = choice.nodes;
                    keyframeManager.SetAutoEase(autoEase, choice.step);
                }
                if (selected)
                    ImGui::PopStyleColor();
                ImGuiEx::Keyframeable::Record(choice.record);
                ImGui::SetItemTooltip("%s", choice.tooltip);
            }
        }
        for (const bool start : {true, false})
        {
            ImGui::PushID(start ? "autoEaseStart" : "autoEaseEnd");
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(start ? "Ease start" : "Ease end");
            ImGui::SameLine();
            ImGui::SetCursorPosX(ImGui::GetWindowWidth() * columnPercent);
            ImGui::SetNextItemWidth(ImGui::GetWindowWidth() * (1.0f - columnPercent) -
                                    ImGui::GetStyle().WindowPadding.x);
            auto& amount = start ? autoEase.start : autoEase.end;
            float percent = amount * 100.0f;
            const bool changed =
                ImGui::SliderFloat("##amount", &percent, 0.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp);
            ImGuiEx::Keyframeable::Record(start ? "control.autoease.start" : "control.autoease.end");
            if (allNodes)
                ImGui::SetItemTooltip(start ? "How much the camera starts from rest leaving each node"
                                            : "How much the camera slows to rest arriving at each node");
            else
                ImGui::SetItemTooltip(start ? "How much the camera starts from rest leaving the first node"
                                            : "How much the camera slows to rest arriving at the last node");
            const auto label = std::format("Auto Ease {}", start ? "start" : "end");
            if (changed)
            {
                // (a drag is one step)
                if (!dragging)
                    Components::UndoHistory::Get().BeginGroup(label);
                dragging = true;
                amount = std::clamp(percent / 100.0f, 0.0f, 1.0f);
                keyframeManager.SetAutoEase(autoEase, label, false);
            }
            if (ImGui::IsItemDeactivated() && dragging)
            {
                keyframeManager.SetAutoEase(autoEase, label);
                Components::UndoHistory::Get().EndGroup();
                dragging = false;
            }
            ImGui::PopID();
        }
        ImGui::EndDisabled();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", !autoEase.on ? "Off: new campaths get no ease."
                                  : allNodes   ? "Campaths ease out of every node by Ease start and into every node by "
                                                 "Ease end, new nodes too."
                                               : "New campaths ease out of their first node and into their last.");
        ImGui::PopTextWrapPos();
    }

    void CameraMenu::Render()
    {
        // (scrolls: the node list can make it longer than the panel)
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar;
        // stays behind the tool windows (Preferences, Controls...) when clicked
        flags |= ImGuiWindowFlags_NoBringToFrontOnFocus;
        ImGui::Begin("Camera", NULL, flags);

        if (Mod::GetGameInterface()->GetGameState() != Types::GameState::InDemo)
        {
            UI::DrawInaccessibleTabWarning();
            ImGui::End();
            return;
        }

        auto& cameraManager = Components::CameraManager::Get();

        if (ImGuiEx::Keyframeable::PanelSection(ICON_FA_CAMERA "  Camera", "camera"))
        {
            DrawModes();
            ImGui::Dummy(ImVec2(0.0f, 2.0f));

            switch (cameraManager.GetActiveCamera()->GetMode())
            {
                case Components::Camera::Mode::FirstPerson:
                    DrawFirstPersonSettings();
                    break;
                case Components::Camera::Mode::ThirdPerson:
                    DrawThirdPersonSettings();
                    break;
                case Components::Camera::Mode::Free:
                    DrawFreeCameraSettings();
                    break;
                case Components::Camera::Mode::Orbit:
                    DrawOrbitCameraSettings();
                    break;
                case Components::Camera::Mode::Bone:
                    DrawBoneCameraSettings();
                    break;
                case Components::Camera::Mode::Dolly:
                    DrawDollycamSettings();
                    break;
                default:
                    break;
            }
        }

        DrawCampath();
        DrawAutoEase();

        ImGui::End();
    }

    void CameraMenu::Release()
    {
    }
}  // namespace IWXMVM::UI