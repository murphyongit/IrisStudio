#include "StdInclude.hpp"
#include "AreaMap.hpp"

#include "Mod.hpp"
#include "Components/AreaLogic.hpp"
#include "Components/CameraManager.hpp"
#include "UI/Commands.hpp"
#include "UI/Theme.hpp"
#include "UI/UIManager.hpp"
#include "UI/ImGuiEx/KeyframeableControls.hpp"

namespace IWXMVM::UI::AreaMap
{
    namespace
    {
        namespace Logic = Components::AreaLogic;

        constexpr auto POPUP = "##areaMap";
        constexpr auto OPEN_COMMAND = "area.map";

        bool openRequested = false;
        bool closeRequested = false;
        bool open = false;
        // the area gone to last, on that level
        std::optional<std::size_t> current;
        std::string currentLevel;

        std::shared_ptr<const Types::LevelAreas> GetAreas()
        {
            return Mod::GetGameInterface()->GetLevelAreas();
        }

        bool InDemo()
        {
            return Mod::GetGameInterface()->GetGameState() == Types::GameState::InDemo;
        }

        std::optional<std::size_t> GetCurrent(const Types::LevelAreas& areas)
        {
            return areas.level == currentLevel && current && *current < areas.areas.size() ? current : std::nullopt;
        }

        std::optional<std::string> WhyNot()
        {
            if (!InDemo() && !Mod::GetGameInterface()->IsHostingLiveGame())
                return "Load a demo first";
            const auto areas = GetAreas();
            if (!areas)
                return "Only on campaign levels and Zombies maps";
            if (areas->areas.empty())
                return "This level has no areas";
            return std::nullopt;
        }

        // A popup open is the map's own, or else a menu or the Controls window's key popup (a G there is the key being
        // set): only the map's own steps or closes
        std::optional<std::string> WhyNotHere()
        {
            if (Commands::IsActive(Commands::Context::Popup) && !open)
                return "Close the menu first";
            return WhyNot();
        }

        // from the area gone to last, or else where the view is
        void Step(int by)
        {
            const auto areas = GetAreas();
            if (!areas || areas->areas.empty())
                return;
            auto from = GetCurrent(*areas);
            if (!from)
            {
                const auto& camera = Components::CameraManager::Get().GetActiveCamera();
                const bool free = InDemo() && camera->GetMode() == Components::Camera::Mode::Free;
                from = Logic::FindNearest(areas->areas, free ? camera->GetPosition()
                                                             : Mod::GetGameInterface()->GetPlayerPosition());
            }
            GoTo(Logic::Step(*from, areas->areas.size(), by));
        }

        // the top line of the map: a colour the panels have behind what they name
        void DrawTitle(const Types::LevelAreas& areas, float width)
        {
            const auto cursor = ImGui::GetCursorScreenPos();
            const auto height = ImGui::GetFrameHeight();
            ImGui::GetWindowDrawList()->AddRectFilled(cursor, cursor + ImVec2(width, height),
                                                      ImGui::GetColorU32(ImGuiCol_FrameBg));
            const auto padding = ImGui::GetStyle().WindowPadding.x;
            const auto text = (height - ImGui::GetTextLineHeight()) / 2.0f;
            ImGui::SetCursorScreenPos(cursor + ImVec2(padding, text));
            ImGui::PushFont(UIManager::Get().GetBoldFont());
            ImGui::TextUnformatted(std::format("{}, {} areas", areas.level, areas.areas.size()).c_str());
            ImGui::PopFont();
            const auto closes = std::format("{} closes", Commands::GetShortcut(OPEN_COMMAND));
            ImGui::SetCursorScreenPos(cursor + ImVec2(width - padding - ImGui::CalcTextSize(closes.c_str()).x, text));
            ImGui::TextDisabled("%s", closes.c_str());
            ImGui::SetCursorScreenPos(cursor + ImVec2(0.0f, height));
        }

        // the level from above: where a person can stand, and a number on each area
        void DrawMap(const Types::LevelAreas& areas, float width)
        {
            const ImVec2 size(width, width * 0.625f);
            const auto corner = ImGui::GetCursorScreenPos();
            const auto drawList = ImGui::GetWindowDrawList();
            const auto radius = ImGui::GetFontSize() * 0.75f;
            const auto view = Logic::FitMap(areas.nodes, {size.x, size.y}, radius * 2.0f);
            const auto at = [&](glm::vec2 point) {
                const auto pixel = Logic::ToMap(view, point);
                return corner + ImVec2(pixel.x, pixel.y);
            };

            drawList->AddRectFilled(corner, corner + size, ImGui::GetColorU32(ImGuiCol_WindowBg));
            const auto nodeColor = ImGui::GetColorU32(ImGuiCol_TextDisabled);
            for (const auto& node : areas.nodes)
            {
                const auto point = at(node);
                drawList->AddRectFilled(point, point + ImVec2(1.2f, 1.2f), nodeColor);
            }

            const auto selected = GetCurrent(areas);
            std::optional<std::size_t> picked;
            for (std::size_t i = 0; i < areas.areas.size(); ++i)
            {
                const auto middle = at({areas.areas[i].spot.x, areas.areas[i].spot.y});
                ImGui::SetCursorScreenPos(middle - ImVec2(radius, radius));
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::InvisibleButton("##area", ImVec2(radius * 2.0f, radius * 2.0f)))
                    picked = i;
                ImGuiEx::Keyframeable::Record(std::format("areamap.{}", i + 1));
                const bool hovered = ImGui::IsItemHovered();
                ImGui::PopID();
                if (hovered)
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

                const bool on = selected == i;
                const auto fill = on ? ImGui::GetColorU32(ImGuiCol_Text)
                                     : ImGui::GetColorU32(hovered ? ImGuiCol_HeaderHovered : ImGuiCol_FrameBg);
                drawList->AddCircleFilled(middle, radius, fill);
                drawList->AddCircle(middle, radius, ImGui::GetColorU32(ImGuiCol_TextDisabled));
                const auto number = std::to_string(i + 1);
                drawList->AddText(ImFloor(middle - ImGui::CalcTextSize(number.c_str()) * 0.5f),
                                  ImGui::GetColorU32(on ? ImGuiCol_WindowBg : ImGuiCol_Text), number.c_str());
            }
            ImGui::SetCursorScreenPos(corner + ImVec2(0.0f, size.y));
            ImGui::Dummy(ImVec2(size.x, 0.0f));
            if (picked)
            {
                GoTo(*picked);
                ImGui::CloseCurrentPopup();
            }
        }
    }  // namespace

    void RegisterCommands()
    {
        using Commands::Context;
        using CommandDispatch::Bit;
        // (M is the timeline's marker, CL1; [ and ] are the keys before and after unless
        // flying or with the map open, CM1)
        constexpr auto MAPS = "campaign and Zombies maps";
        Commands::Register({
            .id = OPEN_COMMAND,
            .label = "Go to Area",
            .category = "Camera",
            .defaults = {ImGuiKey_G},
            .contexts = Bit(Context::Fly) | Bit(Context::Popup) | Bit(Context::Global),
            .unavailable = WhyNotHere,
            .run = [] { open || openRequested ? Close() : Open(); },
            .keywords = "map teleport level part jump",
            .maps = MAPS,
        });
        const auto steps = Bit(Context::Fly) | Bit(Context::Popup);
        Commands::Register({
            .id = "area.previous",
            .label = "Previous area",
            .category = "Camera",
            .defaults = {ImGuiKey_LeftBracket},
            .contexts = steps,
            .unavailable = WhyNotHere,
            .run = [] { Step(-1); },
            .keywords = "map teleport level part jump",
            .maps = MAPS,
        });
        Commands::Register({
            .id = "area.next",
            .label = "Next area",
            .category = "Camera",
            .defaults = {ImGuiKey_RightBracket},
            .contexts = steps,
            .unavailable = WhyNotHere,
            .run = [] { Step(1); },
            .keywords = "map teleport level part jump",
            .maps = MAPS,
        });
    }

    bool HasAreas()
    {
        const auto areas = GetAreas();
        return areas && !areas->areas.empty();
    }

    void Open()
    {
        // (the mouse is the map's: flying has it locked)
        if (UIManager::Get().IsFlying())
            Commands::Run("fly.stop");
        openRequested = true;
    }

    void Close()
    {
        closeRequested = true;
        openRequested = false;
    }

    void GoTo(std::size_t area)
    {
        const auto areas = GetAreas();
        if (!areas || area >= areas->areas.size())
            return;
        current = area;
        currentLevel = areas->level;
        const auto& spot = areas->areas[area];
        const auto eye = spot.spot + glm::vec3(0.0f, 0.0f, Logic::EYE_HEIGHT);
        if (!InDemo())
        {
            Mod::GetGameInterface()->MovePlayer(eye, spot.yaw);
            return;
        }
        auto& cameras = Components::CameraManager::Get();
        cameras.SetActiveCamera(Components::Camera::Mode::Free);
        auto& camera = cameras.GetActiveCamera();
        camera->GetPosition() = eye;
        camera->GetRotation() = glm::vec3(0.0f, spot.yaw, 0.0f);
    }

    void Draw()
    {
        if (openRequested)
        {
            ImGui::OpenPopup(POPUP);
            openRequested = false;
        }
        const auto areas = GetAreas();
        const auto viewPosition = ImGui::GetWindowPos();
        const auto viewSize = ImGui::GetWindowSize();
        const auto width = std::min(viewSize.x * 0.92f, ImGui::GetFontSize() * 49.0f);
        ImGui::SetNextWindowPos(viewPosition + ImVec2(viewSize.x / 2.0f, viewSize.y * 0.04f), ImGuiCond_Always,
                                ImVec2(0.5f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(width, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGui::GetColorU32(ImGuiCol_WindowBg));
        open = ImGui::BeginPopup(POPUP, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
        if (!open)
            return;
        if (!areas || areas->areas.empty() || closeRequested || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        {
            closeRequested = false;
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }
        DrawTitle(*areas, width);
        DrawMap(*areas, width);
        ImGui::SetCursorPosX(ImGui::GetStyle().WindowPadding.x);
        ImGui::Dummy(ImVec2(0.0f, 2.0f));
        ImGui::SetCursorPosX(ImGui::GetStyle().ItemSpacing.x + ImGui::GetFontSize() * 0.6f);
        ImGui::TextDisabled("Click a number");
        ImGui::Dummy(ImVec2(0.0f, 2.0f));
        ImGui::EndPopup();
    }

    State GetState()
    {
        const auto areas = GetAreas();
        return {open, areas ? areas->areas.size() : 0, areas ? GetCurrent(*areas) : std::nullopt};
    }
}  // namespace IWXMVM::UI::AreaMap
