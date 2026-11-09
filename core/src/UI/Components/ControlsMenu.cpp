#include "StdInclude.hpp"
#include "ControlsMenu.hpp"

#include "UI/Theme.hpp"

#include "Resources.hpp"
#include "UI/Commands.hpp"
#include "UI/UIManager.hpp"
#include "UI/ImGuiEx/KeyframeableControls.hpp"

namespace IWXMVM::UI
{
    void ControlsMenu::Initialize()
    {
    }

    namespace
    {
        const char* KEY_POPUP = "Press a key##controlsKeyPopup";

        // the command whose key is being changed, and which of its chords (one past the last adds one)
        std::string_view editedCommand;
        std::size_t editedChord = 0;
        // (opened where it is drawn, so its id is the same)
        bool openKeyPopup = false;
        // a key pressed in the popup that another command has, waiting for Use anyway
        std::optional<ImGuiKeyChord> takenChord;
        // only the rows of keys more than one command has (CN1)
        bool onlyShared = false;

        // keys one command can have
        constexpr std::size_t MAX_CHORDS = 2;

        bool IsModifierKey(ImGuiKey key)
        {
            return key == ImGuiKey_LeftCtrl || key == ImGuiKey_RightCtrl || key == ImGuiKey_LeftShift ||
                   key == ImGuiKey_RightShift || key == ImGuiKey_LeftAlt || key == ImGuiKey_RightAlt ||
                   key == ImGuiKey_LeftSuper || key == ImGuiKey_RightSuper;
        }

        // The chord pressed this frame: a key with the modifiers held; a held command (fly forward) takes a key alone,
        // modifiers and the middle, right and side mouse buttons too
        std::optional<ImGuiKeyChord> GetPressedChord(bool held)
        {
            for (auto key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; key = static_cast<ImGuiKey>(key + 1))
            {
                if (key >= ImGuiKey_GamepadStart && key <= ImGuiKey_GamepadRStickDown)
                    continue;
                if (key >= ImGuiKey_ReservedForModCtrl && key <= ImGuiKey_ReservedForModSuper)
                    continue;
                const bool mouse = key >= ImGuiKey_MouseLeft && key <= ImGuiKey_MouseWheelY;
                if (mouse && (!held || key == ImGuiKey_MouseLeft || key >= ImGuiKey_MouseWheelX))
                    continue;
                if (!held && IsModifierKey(key))
                    continue;
                if (!ImGui::IsKeyPressed(key, false))
                    continue;
                return held ? static_cast<ImGuiKeyChord>(key) : (ImGui::GetIO().KeyMods | key);
            }
            return std::nullopt;
        }

        void DrawKeyPopup()
        {
            if (openKeyPopup)
            {
                openKeyPopup = false;
                takenChord.reset();
                ImGui::OpenPopup(KEY_POPUP);
            }
            ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 22, 0));
            if (!ImGui::BeginPopupModal(KEY_POPUP, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
                return;

            const auto command = Commands::Find(editedCommand);
            if (!command)
            {
                ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
                return;
            }

            ImGui::TextWrapped("Press the key%s for \"%.*s\".", command->held ? "" : " (with Ctrl, Shift or Alt)",
                               static_cast<int>(command->label.size()), command->label.data());
            // the modifiers held so far
            const auto modifiers = ImGui::GetIO().KeyMods;
            std::string held;
            if (!command->held)
            {
                for (const auto& [modifier, name] : {std::pair{ImGuiMod_Ctrl, "Ctrl+"}, std::pair{ImGuiMod_Shift, "Shift+"},
                                                     std::pair{ImGuiMod_Alt, "Alt+"}, std::pair{ImGuiMod_Super, "Win+"}})
                {
                    if (modifiers & modifier)
                        held += name;
                }
            }
            ImGui::TextDisabled("%s", held.empty() ? " " : (held + "...").c_str());

            auto chords = Commands::GetChords(command->id);
            const auto setChord = [&](ImGuiKeyChord pressed) {
                if (editedChord < chords.size())
                    chords[editedChord] = pressed;
                else
                    chords.push_back(pressed);
                // (the same chord twice is once)
                std::vector<ImGuiKeyChord> unique;
                for (const auto chord : chords)
                    if (std::ranges::find(unique, chord) == unique.end())
                        unique.push_back(chord);
                Commands::SetChords(command->id, unique);
            };
            bool done = false;
            // a key another command has waits for Use anyway; another key pressed instead is checked again
            if (const auto pressed = GetPressedChord(command->held))
            {
                if (Commands::GetConflicts(command->id, {*pressed}).empty())
                {
                    setChord(*pressed);
                    done = true;
                }
                else
                {
                    takenChord = pressed;
                }
            }

            if (takenChord && !done)
            {
                std::string what;
                const auto others = Commands::GetConflicts(command->id, {*takenChord});
                for (std::size_t i = 0; i < others.size(); ++i)
                {
                    what += i == 0 ? "" : (i + 1 == others.size() ? " and " : ", ");
                    what += others[i]->label;
                }
                ImGui::PushStyleColor(ImGuiCol_Text, Theme::Problem());
                ImGui::TextWrapped("%s is %s.", Commands::FormatChord(*takenChord).c_str(), what.c_str());
                ImGui::PopStyleColor();
                ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.3f));
                if (ImGui::Button("Use anyway"))
                {
                    setChord(*takenChord);
                    done = true;
                }
                ImGuiEx::Keyframeable::Record("controls.useanyway");
                ImGui::SameLine();
            }
            else
            {
                ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.3f));
                if (editedChord < chords.size() && ImGui::Button("No key"))
                {
                    chords.erase(chords.begin() + editedChord);
                    Commands::SetChords(command->id, chords);
                    done = true;
                }
                if (editedChord < chords.size())
                    ImGui::SameLine();
            }
            if (ImGui::Button("Cancel"))
                done = true;
            ImGuiEx::Keyframeable::Record("controls.cancel");
            if (done)
            {
                takenChord.reset();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // A row: the command, its keys (a click changes one, + adds one), conflicts and Reset
        void DrawCommandRow(const Commands::Command& command)
        {
            ImGui::PushID(command.id.data(), command.id.data() + command.id.size());
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(command.label.data(), command.label.data() + command.label.size());
            if (ImGui::BeginItemTooltip())
            {
                ImGui::Text("Works %s", Commands::DescribeContexts(command.contexts).c_str());
                if (command.held)
                    ImGui::TextUnformatted("Held down, with any of Ctrl, Shift or Alt");
                if (command.repeat)
                    ImGui::TextUnformatted("Goes on while the key is held");
                if (const auto reason = Commands::GetUnavailableReason(command.id))
                    ImGui::TextDisabled("Not now: %s", reason->c_str());
                ImGui::EndTooltip();
            }

            ImGui::TableSetColumnIndex(1);
            const auto conflicts = Commands::GetConflicts(command.id);
            const auto chords = Commands::GetChords(command.id);
            const auto buttonWidth = ImGui::GetFontSize() * 6.5f;
            for (std::size_t i = 0; i <= chords.size(); ++i)
            {
                if (i == chords.size() && !chords.empty())
                {
                    if (chords.size() >= MAX_CHORDS)
                        break;
                    // another key for it
                    ImGui::SameLine();
                    if (ImGui::Button("+"))
                    {
                        editedCommand = command.id;
                        editedChord = chords.size();
                        openKeyPopup = true;
                    }
                    ImGui::SetItemTooltip("Add another key");
                    break;
                }
                if (i > 0)
                    ImGui::SameLine();
                ImGui::PushID(static_cast<int>(i));
                const auto text = i < chords.size() ? Commands::FormatChord(chords[i]) : std::string("-");
                if (!conflicts.empty())
                    ImGui::PushStyleColor(ImGuiCol_Text, Theme::Problem());
                if (ImGui::Button(text.c_str(), ImVec2(buttonWidth, 0)))
                {
                    editedCommand = command.id;
                    editedChord = i;
                    openKeyPopup = true;
                }
                ImGuiEx::Keyframeable::Record(std::format("controls.key.{}.{}", command.id, i));
                if (!conflicts.empty())
                    ImGui::PopStyleColor();
                ImGui::SetItemTooltip(i < chords.size() ? "Click and press another key" : "Click and press a key");
                ImGui::PopID();
            }

            ImGui::TableSetColumnIndex(2);
            if (!conflicts.empty())
            {
                ImGui::AlignTextToFramePadding();
                ImGui::TextColored(Theme::Problem(), ICON_FA_TRIANGLE_EXCLAMATION);
                if (ImGui::BeginItemTooltip())
                {
                    // each key it shares: what else has it, and which runs
                    bool first = true;
                    for (const auto chord : chords)
                    {
                        const auto others = Commands::GetConflicts(command.id, {chord});
                        if (others.empty())
                            continue;
                        if (!first)
                            ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.3f));
                        first = false;
                        ImGui::PushFont(UIManager::Get().GetBoldFont());
                        ImGui::Text("%s also:", Commands::FormatChord(chord).c_str());
                        ImGui::PopFont();
                        for (const auto other : others)
                            ImGui::BulletText("%.*s: %.*s", static_cast<int>(other->category.size()),
                                              other->category.data(), static_cast<int>(other->label.size()),
                                              other->label.data());
                        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 22);
                        ImGui::TextDisabled("%s", Commands::DescribeClash(command.id, others).c_str());
                        ImGui::PopTextWrapPos();
                    }
                    ImGui::EndTooltip();
                }
                ImGui::SameLine();
            }
            if (Commands::IsCustomized(command.id))
            {
                if (ImGui::SmallButton("Reset"))
                    Commands::SetChords(command.id, std::nullopt);
                ImGuiEx::Keyframeable::Record(std::format("controls.reset.{}", command.id));
                std::string defaults;
                for (const auto chord : command.defaults)
                    defaults += (defaults.empty() ? "" : ", ") + Commands::FormatChord(chord);
                ImGui::SetItemTooltip("Back to %s", defaults.empty() ? "no key" : defaults.c_str());
            }

            ImGui::PopID();
        }
    }  // namespace

    // Every command by category, with readable names and keys: a click on a key changes it; conflicts are flagged and
    // each command (or all) can go back to its defaults
    void ControlsMenu::Render()
    {
        if (!visible)
            return;

        const auto& io = ImGui::GetIO();
        ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 40, io.DisplaySize.y * 0.75f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(io.DisplaySize * 0.5f, ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
        if (ImGui::Begin("Controls", &visible, ImGuiWindowFlags_NoCollapse))
        {
            static char search[64] = "";
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::GetFontSize() * 6);
            ImGui::InputTextWithHint("##controlsSearch", ICON_FA_MAGNIFYING_GLASS " Find a command or key", search,
                                     sizeof(search));
            ImGuiEx::Keyframeable::Record("controls.search");
            ImGui::SameLine();
            const bool anyCustomized = std::ranges::any_of(
                Commands::GetAll(), [](const Commands::Command& command) { return Commands::IsCustomized(command.id); });
            ImGui::BeginDisabled(!anyCustomized);
            if (ImGui::Button("Reset all", ImVec2(-FLT_MIN, 0)))
                Commands::ResetAll();
            ImGui::EndDisabled();

            // keys more than one command has, above the list, and a switch to just their rows (CN1)
            const auto shared = Commands::CountSharedKeys();
            if (shared == 0)
            {
                onlyShared = false;
            }
            else
            {
                const auto cursor = ImGui::GetCursorScreenPos();
                const auto start = ImGui::GetCursorPosX();
                const auto width = ImGui::GetContentRegionAvail().x;
                const auto height = ImGui::GetFrameHeight();
                ImGui::GetWindowDrawList()->AddRectFilled(cursor, cursor + ImVec2(width, height),
                                                          ImGui::GetColorU32(ImGuiCol_FrameBg));
                const auto padding = ImGui::GetStyle().FramePadding.x;
                ImGui::SetCursorScreenPos(cursor + ImVec2(padding, 0.0f));
                ImGui::AlignTextToFramePadding();
                ImGui::TextColored(Theme::Problem(), ICON_FA_TRIANGLE_EXCLAMATION);
                ImGui::SameLine();
                ImGui::Text("%zu %s used twice", shared, shared == 1 ? "key" : "keys");
                const auto label = onlyShared ? "Show all" : "Show";
                const auto buttonWidth = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2;
                ImGui::SameLine(start + width - buttonWidth);
                if (ImGui::Button(label))
                    onlyShared = !onlyShared;
                ImGuiEx::Keyframeable::Record("controls.shared");
                ImGui::SetItemTooltip(onlyShared ? "Every command" : "Only the commands that share a key");
            }

            if (ImGui::BeginChild("##controlsList"))
            {
                // the categories in the order of the work, then any others as they were registered
                std::vector<std::string_view> categories = {"Edit",   "Playback", "Camera", "Fly",  "Campath",
                                                            "Actors", "Record",   "View",   "Help", "Developer"};
                for (const auto& command : Commands::GetAll())
                {
                    if (std::ranges::find(categories, command.category) == categories.end())
                        categories.insert(categories.end() - 1, command.category);
                }

                for (const auto category : categories)
                {
                    std::vector<const Commands::Command*> commands;
                    for (const auto& command : Commands::GetAll())
                    {
                        if (command.category == category && Commands::MatchesSearch(command, search) &&
                            (!onlyShared || !Commands::GetConflicts(command.id).empty()))
                            commands.push_back(&command);
                    }
                    if (commands.empty())
                        continue;

                    ImGui::PushFont(UIManager::Get().GetBoldFont());
                    ImGui::SeparatorText(std::string(category).c_str());
                    ImGui::PopFont();
                    const auto tableName = std::format("##controls{}", category);
                    if (ImGui::BeginTable(tableName.c_str(), 3, ImGuiTableFlags_SizingFixedFit))
                    {
                        // (the same widths in every category, so the keys line up)
                        const auto& style = ImGui::GetStyle();
                        ImGui::TableSetupColumn("Command", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("Keys", ImGuiTableColumnFlags_WidthFixed,
                                                ImGui::GetFontSize() * 6.5f * static_cast<float>(MAX_CHORDS) +
                                                    style.ItemSpacing.x * static_cast<float>(MAX_CHORDS) + ImGui::GetFrameHeight());
                        ImGui::TableSetupColumn("Reset", ImGuiTableColumnFlags_WidthFixed,
                                                ImGui::GetFontSize() * 1.5f + ImGui::CalcTextSize("Reset").x +
                                                    style.FramePadding.x * 2 + style.ItemSpacing.x);
                        for (const auto command : commands)
                            DrawCommandRow(*command);
                        ImGui::EndTable();
                    }
                }
                DrawKeyPopup();
            }
            ImGui::EndChild();
        }
        ImGui::End();
    }

    void ControlsMenu::Release()
    {
    }
}  // namespace IWXMVM::UI
