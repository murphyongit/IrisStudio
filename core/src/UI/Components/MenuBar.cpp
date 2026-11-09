#include "StdInclude.hpp"
#include "MenuBar.hpp"

#include "Version.hpp"
#include "Mod.hpp"
#include "Utilities/PathUtils.hpp"
#include "UI/UIManager.hpp"
#include "UI/Commands.hpp"
#include "UI/ImGuiEx/KeyframeableControls.hpp"
#include "UI/Layout.hpp"
#include "UI/TimelineMath.hpp"
#include "UI/Toasts.hpp"
#include "Components/Actors.hpp"
#include "Components/CaptureManager.hpp"
#include "Components/KeyframeSerializer.hpp"
#include "Components/UndoHistory.hpp"
#include "Configuration/Configuration.hpp"
#include "Configuration/PreferencesConfiguration.hpp"

namespace IWXMVM::UI
{
    void MenuBar::Initialize()
    {
        SetPosition(0, 0);
        SetSize(ImGui::GetIO().DisplaySize.x, ImGui::GetFrameHeight());

        Commands::Register({
            .id = "file.loadDemo",
            .label = "Load a demo",
            .category = "File",
            .defaults = {ImGuiMod_Ctrl | ImGuiKey_O},
            .unavailable = []() -> std::optional<std::string> {
                if (Components::CaptureManager::Get().IsCapturing())
                    return "Not while recording";
                return std::nullopt;
            },
            .run =
                [] {
                    UIManager::Get().SelectTab(Tab::Demo, Page::Browse);
                    UIManager::Get().GetUIComponent<DemoLoader>(Component::DemoLoader)->FocusSearch();
                },
            .keywords = "open browse play demos",
        });
        Commands::Register({
            .id = "file.save",
            .label = "Save",
            .category = "File",
            .defaults = {ImGuiMod_Ctrl | ImGuiKey_S},
            .unavailable = []() -> std::optional<std::string> {
                if (Mod::GetGameInterface()->GetGameState() != Types::GameState::InDemo)
                    return "Load a demo first";
                return std::nullopt;
            },
            .run =
                [] {
                    // keys wait half a second after a change before they are written (KeyframeSerializer), actors are
                    // written at once
                    Components::KeyframeSerializer::WriteRecentIfChanged(true);
                    Toasts::Show("Saved");
                },
            .keywords = "write keys keyframes actors scene",
        });
        Commands::Register({
            .id = "file.preferences",
            .label = "Preferences",
            .category = "File",
            .run = [] { UIManager::Get().GetUIComponent<Preferences>(Component::Preferences)->ToggleVisibility(); },
            .keywords = "settings options ui scale speed developer",
        });
    }

    // A command as a menu item, with its shortcut; greyed out with the reason in a tooltip when it can't run now.
    // Ticked when selected.
    void CommandMenuItem(std::string_view id, std::optional<std::string> label = std::nullopt, bool selected = false)
    {
        const auto command = Commands::Find(id);
        if (!command)
            return;
        const auto reason = Commands::GetUnavailableReason(id);
        const auto text = label.value_or(std::string(command->label));
        if (ImGui::MenuItem(text.c_str(), Commands::GetShortcut(id).c_str(), selected, !reason))
            Commands::Run(id);
        if (reason && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("%s", reason->c_str());
    }

    void DrawFileMenu()
    {
        const bool open = ImGui::BeginMenu("File");
        ImGuiEx::Keyframeable::Record("menu.file");
        if (!open)
            return;

        CommandMenuItem("file.loadDemo", "Load demo...");
        const auto loader = UIManager::Get().GetUIComponent<DemoLoader>(Component::DemoLoader);
        const auto& recent = loader->GetRecentDemos();
        // (none while a demo loads: see DemoLoader::Play)
        if (ImGui::BeginMenu("Recent demos", !recent.empty() && !loader->IsLoading() &&
                                                 !Components::CaptureManager::Get().IsCapturing()))
        {
            // (copied: playing one reorders them)
            const auto demos = std::vector(recent.begin(), recent.end());
            for (const auto& demo : demos)
            {
                if (ImGui::MenuItem(demo.filename().string().c_str()))
                    UIManager::Get().GetUIComponent<DemoLoader>(Component::DemoLoader)->Play(demo);
                ImGui::SetItemTooltip("%s", demo.string().c_str());
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        CommandMenuItem("file.save");
        CommandMenuItem("file.importKeys");
        CommandMenuItem("file.exportKeys");
        ImGui::Separator();
        CommandMenuItem("file.preferences", "Preferences...");
        CommandMenuItem("help.controls", "Controls...");
        ImGui::Separator();
        if (ImGui::MenuItem("Exit"))
        {
            // (what changed in the last moment first, as keys wait half a second to be written)
            Components::KeyframeSerializer::WriteRecentIfChanged(true);
            Components::Actors::SaveIfChanged();
            Mod::GetGameInterface()->Quit();
        }
        ImGui::SetItemTooltip("Closes the game");

        ImGui::EndMenu();
    }

    void DrawEditMenu()
    {
        const bool open = ImGui::BeginMenu("Edit");
        ImGuiEx::Keyframeable::Record("menu.edit");
        if (!open)
            return;

        // named for the step they take back ("Undo move 3 keys"), wherever it was
        const auto& history = Components::UndoHistory::Get();
        const auto undo = history.GetUndoLabel();
        const auto redo = history.GetRedoLabel();
        CommandMenuItem("edit.undo", undo ? "Undo " + *undo : "Undo");
        CommandMenuItem("edit.redo", redo ? "Redo " + *redo : "Redo");
        ImGui::Separator();
        CommandMenuItem("edit.cut");
        CommandMenuItem("edit.copy");
        CommandMenuItem("edit.paste");
        CommandMenuItem("edit.duplicate");
        CommandMenuItem("edit.delete");
        CommandMenuItem("edit.selectAll");
        ImGui::Separator();
        CommandMenuItem("campath.clear");

        ImGui::EndMenu();
    }

    void DrawViewMenu()
    {
        const bool open = ImGui::BeginMenu("View");
        ImGuiEx::Keyframeable::Record("menu.view");
        if (!open)
            return;

        CommandMenuItem("view.toggleOverlay", "Hide the editor");
        ImGui::Separator();

        const auto& preferences = PreferencesConfiguration::Get();
        if (ImGui::BeginMenu("UI scale"))
        {
            for (const auto scale : LayoutMath::SCALES)
            {
                if (ImGui::MenuItem(std::format("{:.0f}%", scale * 100.0f).c_str(), nullptr,
                                    std::abs(preferences.uiScale - scale) < 0.001f))
                    Layout::SetScale(scale);
            }
            ImGui::Separator();
            CommandMenuItem("view.biggerUI", "Bigger");
            CommandMenuItem("view.smallerUI", "Smaller");
            CommandMenuItem("view.resetUI", "100%");
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Layout"))
        {
            CommandMenuItem("view.layoutDefault", "Default");
            CommandMenuItem("view.layoutEditing", "For editing: a tall timeline");
            CommandMenuItem("view.layoutWatching", "For watching: a big view");
            ImGui::Separator();
            ImGui::TextDisabled("Drag the edges between the view, the panel and the timeline to size them");
            ImGui::EndMenu();
        }
        ImGui::Separator();

        ImGui::TextDisabled("Over the view");
        CommandMenuItem("view.campath", "Campath nodes and line", preferences.showCampath);
        CommandMenuItem("view.actorRings", "Rings under actors", preferences.showActorRings);
        CommandMenuItem("view.actorPaths", "Other actors' paths", preferences.showActorPaths);
        CommandMenuItem("view.safeFrames", "Safe frames", preferences.showSafeFrames);
        CommandMenuItem("view.keyHints", "Keys in the mode strip", preferences.showKeybindHints);
        ImGui::Separator();

        // (saved as a slider is let go)
        auto& lines = PreferencesConfiguration::Get();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
        ImGui::SliderFloat("Line width", &lines.viewLineWidth, 1.0f, 4.0f, "%.1f px");
        ImGuiEx::Keyframeable::Record("view.linewidth");
        if (ImGui::IsItemDeactivatedAfterEdit())
            Configuration::Get().Write(true);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
        auto strength = lines.viewLineStrength * 100.0f;
        if (ImGui::SliderFloat("Strength", &strength, 20.0f, 100.0f, "%.0f%%"))
            lines.viewLineStrength = strength / 100.0f;
        ImGuiEx::Keyframeable::Record("view.linestrength");
        if (ImGui::IsItemDeactivatedAfterEdit())
            Configuration::Get().Write(true);

        ImGui::EndMenu();
    }

    void DrawHelpMenu()
    {
        const bool open = ImGui::BeginMenu("Help");
        ImGuiEx::Keyframeable::Record("menu.help");
        if (!open)
            return;

        CommandMenuItem("help.shortcuts");
        CommandMenuItem("help.palette");
        CommandMenuItem("help.gettingStarted");
        ImGui::Separator();
        if (ImGui::MenuItem(std::format("About {}", ModName(Mod::GetGameInterface()->GetGame())).c_str()))
            UIManager::Get().GetUIComponent<Credits>(Component::Credits)->ToggleVisibility();

        // (File > Preferences turns them on)
        if (PreferencesConfiguration::Get().developerTools)
        {
            ImGui::Separator();
            if (ImGui::BeginMenu("Developer"))
            {
                CommandMenuItem("developer.imguiDemo", std::nullopt, UIManager::Get().IsImGuiDemoShown());
                CommandMenuItem("developer.debugPanel", std::nullopt, UIManager::Get().IsDebugPanelShown());
                ImGui::EndMenu();
            }
        }

        ImGui::EndMenu();
    }

    void MenuBar::Render()
    {
        if (ImGui::BeginMainMenuBar())
        {
            // (the layout goes under it, at its height for the font of this frame)
            SetSize(ImGui::GetWindowSize());

            DrawFileMenu();
            DrawEditMenu();
            DrawViewMenu();
            DrawHelpMenu();

            // the demo and how long it is, then the version, on the right
            const auto version = std::format("{} {}", ModName(Mod::GetGameInterface()->GetGame()), IWXMVM_VERSION);
            std::string demo;
            if (Mod::GetGameInterface()->GetGameState() == Types::GameState::InDemo)
            {
                const auto info = Mod::GetGameInterface()->GetDemoInfo();
                demo = std::format("{} \xC2\xB7 {}", info.name, TimelineMath::FormatTime(info.endTick));
            }
            const auto space = ImGui::CalcTextSize(" ").x;
            const auto versionWidth = ImGui::CalcTextSize(version.c_str()).x;
            const auto demoWidth = demo.empty() ? 0.0f : ImGui::CalcTextSize(demo.c_str()).x + space * 6;
            const auto right = ImGui::GetIO().DisplaySize.x - space;
            if (right - versionWidth - demoWidth > ImGui::GetCursorPosX())
            {
                ImGui::SetCursorPosX(right - versionWidth - demoWidth);
                if (!demo.empty())
                {
                    ImGui::TextUnformatted(demo.c_str());
                    ImGui::SameLine(0, space * 6);
                }
                ImGui::TextDisabled("%s", version.c_str());
            }

            ImGui::EndMainMenuBar();
        }
    }

    void MenuBar::Release()
    {
    }
}  // namespace IWXMVM::UI
