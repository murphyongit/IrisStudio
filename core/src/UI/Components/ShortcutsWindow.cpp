#include "StdInclude.hpp"
#include "ShortcutsWindow.hpp"

#include "UI/Theme.hpp"

#include "UI/Commands.hpp"
#include "UI/UIManager.hpp"
#include "UI/Toasts.hpp"

namespace IWXMVM::UI
{
    void ShortcutsWindow::Initialize()
    {
        Commands::Register({
            .id = "help.shortcuts",
            .label = "Keyboard shortcuts",
            .category = "Help",
            .defaults = {ImGuiMod_Shift | ImGuiKey_Slash},
            .run = [this] { ToggleVisibility(); },
            .keywords = "keys cheat sheet help",
        });
        Commands::Register({
            .id = "help.palette",
            .label = "Find a command",
            .category = "Help",
            .defaults = {ImGuiMod_Ctrl | ImGuiKey_K},
            .run = [this] { OpenPalette(); },
            .keywords = "command palette search",
        });
        Commands::Register({
            .id = "help.controls",
            .label = "Change the keys (Controls)",
            .category = "Help",
            .run = [] { UIManager::Get().GetUIComponent<ControlsMenu>(Component::ControlsMenu)->ToggleVisibility(); },
            .keywords = "binds keybinds rebind",
        });
    }

    void ShortcutsWindow::Render()
    {
        RenderSheet();
        RenderPalette();
    }

    // Every command with a key, under where it works: while flying, placing or recording, over the timeline,
    // anywhere. A command that works in several places is under each.
    void ShortcutsWindow::RenderSheet()
    {
        if (!visible)
            return;

        const auto& io = ImGui::GetIO();
        ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 44, io.DisplaySize.y * 0.7f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(io.DisplaySize * 0.5f, ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
        if (ImGui::Begin("Keyboard shortcuts", &visible, ImGuiWindowFlags_NoCollapse))
        {
            ImGui::TextDisabled("Keys reach the first of these that uses them; flying, typing and recording keep the "
                                "others from the editor. Change them in File > Controls.");

            static constexpr std::array<std::pair<Commands::Context, const char*>, 6> SECTIONS = {{
                {Commands::Context::Fly, "While flying"},
                {Commands::Context::Placing, "While placing path points"},
                {Commands::Context::Capturing, "While recording"},
                {Commands::Context::Viewport, "Over the view"},
                {Commands::Context::Timeline, "Over the timeline"},
                {Commands::Context::Global, "Anywhere else"},
            }};
            if (ImGui::BeginTable("##shortcutSections", 2, ImGuiTableFlags_SizingStretchSame))
            {
                for (const auto& [context, title] : SECTIONS)
                {
                    std::vector<const Commands::Command*> commands;
                    for (const auto& command : Commands::GetAll())
                    {
                        if (!(command.contexts & Commands::Bit(context)) || Commands::GetChords(command.id).empty() ||
                            command.category == "Developer")
                            continue;
                        // keys to hold under their first context only: flying moves over the view only in the orbit
                        // camera
                        const auto first = command.contexts & ~(command.contexts - 1);
                        if (command.held && first != Commands::Bit(context))
                            continue;
                        commands.push_back(&command);
                    }
                    if (commands.empty())
                        continue;

                    ImGui::TableNextColumn();
                    ImGui::PushFont(UIManager::Get().GetBoldFont());
                    ImGui::TextUnformatted(title);
                    ImGui::PopFont();
                    ImGui::Separator();
                    if (ImGui::BeginTable(title, 2))
                    {
                        ImGui::TableSetupColumn("Keys", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 7);
                        ImGui::TableSetupColumn("Command", ImGuiTableColumnFlags_WidthStretch);
                        for (const auto command : commands)
                        {
                            std::string keys;
                            for (const auto chord : Commands::GetChords(command->id))
                                keys += (keys.empty() ? "" : ", ") + Commands::FormatChord(chord);
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);
                            ImGui::TextColored(Theme::Keys(), "%s", keys.c_str());
                            ImGui::TableSetColumnIndex(1);
                            ImGui::TextWrapped("%.*s", static_cast<int>(command->label.size()),
                                               command->label.data());
                        }
                        ImGui::EndTable();
                    }
                    ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.5f));
                }
                ImGui::EndTable();
            }
        }
        ImGui::End();
    }

    // Finds a command by what is typed (its name, category, other words for it or keys) and runs it: Up and Down pick,
    // Enter runs, Escape closes
    void ShortcutsWindow::RenderPalette()
    {
        static constexpr const char* NAME = "##commandPalette";
        if (openPalette)
        {
            openPalette = false;
            query.clear();
            highlighted = 0;
            ImGui::OpenPopup(NAME);
        }

        const auto& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.15f), ImGuiCond_Always,
                                ImVec2(0.5f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 32, 0));
        std::optional<std::string_view> run;
        if (ImGui::BeginPopupModal(NAME, nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize))
        {
            if (ImGui::IsWindowAppearing())
                ImGui::SetKeyboardFocusHere();
            ImGui::SetNextItemWidth(-FLT_MIN);
            char buffer[128]{};
            query.copy(buffer, sizeof(buffer) - 1);
            if (ImGui::InputTextWithHint("##paletteQuery", "Find a command...", buffer, sizeof(buffer)))
            {
                query = buffer;
                highlighted = 0;
            }

            std::vector<const Commands::Command*> found;
            for (const auto& command : Commands::GetAll())
            {
                if (command.run && command.category != "Developer" && Commands::MatchesSearch(command, query))
                    found.push_back(&command);
            }
            if (!found.empty())
            {
                if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
                    highlighted = (highlighted + 1) % found.size();
                if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
                    highlighted = (highlighted + found.size() - 1) % found.size();
                highlighted = std::min(highlighted, found.size() - 1);
            }

            const auto rows = std::min<std::size_t>(found.size(), 12);
            if (ImGui::BeginChild("##paletteResults",
                                  ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * std::max<std::size_t>(rows, 1) + 4)))
            {
                if (found.empty())
                    ImGui::TextDisabled("No command has those words");
                for (std::size_t i = 0; i < found.size(); ++i)
                {
                    const auto& command = *found[i];
                    const auto reason = Commands::GetUnavailableReason(command.id);
                    ImGui::PushID(static_cast<int>(i));
                    ImGui::BeginDisabled(reason.has_value());
                    const auto label = std::format("{}: {}", command.category, command.label);
                    if (ImGui::Selectable(label.c_str(), i == highlighted))
                        run = command.id;
                    ImGui::EndDisabled();
                    if (reason)
                        ImGui::SetItemTooltip("%s", reason->c_str());
                    if (i == highlighted)
                        ImGui::SetScrollHereY();
                    const auto shortcut = Commands::GetShortcut(command.id);
                    if (!shortcut.empty())
                    {
                        ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize(shortcut.c_str()).x);
                        ImGui::TextDisabled("%s", shortcut.c_str());
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();

            if (ImGui::IsKeyPressed(ImGuiKey_Enter) && !found.empty())
                run = found[highlighted]->id;
            if (run || ImGui::IsKeyPressed(ImGuiKey_Escape))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        // (once the palette is closed, so the command acts on the editor as its key would)
        if (run && !Commands::Run(*run))
        {
            if (const auto reason = Commands::GetUnavailableReason(*run))
                Toasts::Show(*reason);
        }
    }

    void ShortcutsWindow::Release()
    {
    }
}  // namespace IWXMVM::UI
