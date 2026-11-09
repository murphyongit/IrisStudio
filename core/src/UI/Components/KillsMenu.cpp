#include "StdInclude.hpp"
#include "KillsMenu.hpp"

#include "Mod.hpp"
#include "Resources.hpp"
#include "Components/KillFinder.hpp"
#include "Components/Playback.hpp"
#include "UI/UIManager.hpp"
#include "UI/GettingStarted.hpp"
#include "UI/ImGuiEx/ImGuiExtensions.hpp"

namespace IWXMVM::UI
{
    void KillsMenu::Initialize()
    {
    }

    std::string KeyName(std::string_view command)
    {
        return Commands::GetShortcut(command);
    }

    void KillsMenu::RenderFilters()
    {
        using namespace Components;
        auto& settings = KillFinder::GetSettings();

        const auto events = KillFinder::GetEvents();
        std::size_t ownKills = 0, ownDeaths = 0;
        for (const auto& entry : KillFinder::GetEntries())
        {
            ownKills += entry.byPov;
            ownDeaths += entry.ofPov;
        }
        ImGui::TextWrapped("%zu kills and %zu deaths of %s, %zu kills in the match", ownKills, ownDeaths,
                           events->GetPlayerName(events->recorder).c_str(), events->kills.size());
        ImGui::Dummy(ImVec2(0, 4));

        const auto spacing = ImGui::GetStyle().ItemSpacing.x;
        const auto buttonWidth = (ImGui::GetContentRegionAvail().x - spacing) / 2;
        if (ImGui::Button(std::format(ICON_FA_BACKWARD_STEP " Previous ({})", KeyName("kills.previous")).c_str(),
                          ImVec2(buttonWidth, 0)))
            KillFinder::JumpToPrevious();
        ImGui::SameLine();
        if (ImGui::Button(std::format("Next ({}) " ICON_FA_FORWARD_STEP, KeyName("kills.next")).c_str(),
                          ImVec2(buttonWidth, 0)))
            KillFinder::JumpToNext();

        ImGui::Dummy(ImVec2(0, 4));

        const auto column = ImGui::GetContentRegionAvail().x / 2;
        auto colored = [&](const char* label, bool& value, ImVec4 color) {
            ImGui::PushStyleColor(ImGuiCol_CheckMark, color);
            ImGui::Checkbox(label, &value);
            ImGui::PopStyleColor();
        };
        colored("Your kills", settings.showOwnKills, ImVec4(0.30f, 0.85f, 0.45f, 1.0f));
        ImGui::SameLine(column);
        colored("Your deaths", settings.showOwnDeaths, ImVec4(0.92f, 0.30f, 0.30f, 1.0f));
        colored("Other players", settings.showOtherKills, ImVec4(0.62f, 0.62f, 0.62f, 1.0f));
        ImGui::SameLine(column);
        colored("Highlights only", settings.onlyHighlights, ImVec4(1.0f, 0.78f, 0.22f, 1.0f));
        ImGui::SetItemTooltip("Multikills, collaterals, knife kills, kills in the air and from %.0f m or more",
                              KillFinder::LONGSHOT_DISTANCE / KillFinder::UNITS_PER_METER);

        ImGui::Dummy(ImVec2(0, 4));
        const auto labelWidth = ImGui::GetContentRegionAvail().x * 0.4f;
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Jump to");
        ImGui::SameLine(labelWidth);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        ImGuiEx::DragSlider("##killPreroll", &settings.preroll, 0, 10, "%.1f s before",
                            {.resetValue = 3.0f, .flags = ImGuiSliderFlags_AlwaysClamp});
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Clips end");
        ImGui::SameLine(labelWidth);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        ImGuiEx::DragSlider("##killPostroll", &settings.postroll, 0, 10, "%.1f s after",
                            {.resetValue = 2.0f, .flags = ImGuiSliderFlags_AlwaysClamp});

        ImGui::Dummy(ImVec2(0, 4));
        ImGui::Separator();
    }

    void KillsMenu::RenderList()
    {
        using namespace Components;

        const auto& entries = KillFinder::GetEntries();
        std::vector<const KillFinder::Entry*> shown;
        for (const auto& entry : entries)
        {
            if (KillFinder::IsShown(entry))
                shown.push_back(&entry);
        }

        if (shown.empty())
        {
            ImGui::PushTextWrapPos(0.0f);
            if (entries.empty())
                ImGui::TextDisabled("No kills in this demo.");
            else
                ImGui::TextDisabled(
                    "No kills to show with these filters: tick Other players, or untick Highlights "
                    "only.");
            ImGui::PopTextWrapPos();
            return;
        }

        // the kill the playback is at: the last one whose preroll has started
        const auto current =
            Playback::GetWorldTick() + static_cast<std::uint32_t>(KillFinder::GetSettings().preroll * 1000);
        const KillFinder::Entry* currentEntry = nullptr;
        for (const auto entry : shown)
        {
            if (KillFinder::GetKill(*entry).tick <= current)
                currentEntry = entry;
        }

        if (!ImGui::BeginChild("##killList"))
        {
            ImGui::EndChild();
            return;
        }

        const auto lineHeight = ImGui::GetTextLineHeight();
        const auto rowHeight = lineHeight * 2 + ImGui::GetStyle().FramePadding.y * 2;
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(shown.size()), rowHeight + ImGui::GetStyle().ItemSpacing.y);
        while (clipper.Step())
        {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++)
            {
                const auto& entry = *shown[i];
                const auto& kill = KillFinder::GetKill(entry);

                ImGui::PushID(static_cast<int>(entry.index));
                const auto start = ImGui::GetCursorScreenPos();
                if (ImGui::Selectable("##kill", &entry == currentEntry, ImGuiSelectableFlags_None,
                                      ImVec2(0, rowHeight)))
                    KillFinder::JumpTo(kill.tick);

                if (ImGui::BeginPopupContextItem())
                {
                    if (ImGui::Selectable(ICON_FA_FORWARD_STEP " Jump here"))
                        KillFinder::JumpTo(kill.tick);
                    const auto rangeLabel =
                        entry.seriesCount > 1
                            ? std::format(ICON_FA_SCISSORS " Set capture range to the {} kills", entry.seriesCount)
                            : std::string(ICON_FA_SCISSORS " Set capture range to this kill");
                    if (ImGui::Selectable(rangeLabel.c_str()))
                        KillFinder::SetCaptureRange(entry);
                    ImGui::EndPopup();
                }
                ImGui::SetItemTooltip("Click to jump, right-click for a capture range");

                auto drawList = ImGui::GetWindowDrawList();
                const auto padding = ImGui::GetStyle().FramePadding;
                drawList->AddRectFilled(start, ImVec2(start.x + 3, start.y + rowHeight),
                                        ImGui::GetColorU32(KillFinder::GetColor(entry)));
                const ImVec2 text(start.x + 10, start.y + padding.y);
                drawList->AddText(text, ImGui::GetColorU32(ImGuiCol_Text),
                                  std::format("{}  {}", KillFinder::FormatTick(kill.tick), KillFinder::Describe(entry))
                                      .c_str());
                drawList->AddText(ImVec2(text.x, text.y + lineHeight), ImGui::GetColorU32(ImGuiCol_TextDisabled),
                                  KillFinder::GetDetails(entry).c_str());
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
    }

    void KillsMenu::Render()
    {
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar;
        // stays behind the tool windows (Preferences, Player Loadouts...) when clicked
        flags |= ImGuiWindowFlags_NoBringToFrontOnFocus;
        if (ImGui::Begin("Kills", nullptr, flags))
        {
            GettingStarted::DrawCard();
            if (Mod::GetGameInterface()->GetGameState() != Types::GameState::InDemo)
            {
                UI::DrawInaccessibleTabWarning();
            }
            else if (!(Mod::GetGameInterface()->GetSupportedFeatures() & Types::Features_DemoEvents))
            {
                ImGui::TextWrapped("Finding kills isn't supported for this game yet.");
            }
            else if (!Components::KillFinder::GetEvents())
            {
                ImGui::TextWrapped(ICON_FA_CROSSHAIRS "  Finding the kills in this demo...");
            }
            else
            {
                RenderFilters();
                RenderList();
            }
        }
        ImGui::End();
    }

    void KillsMenu::Release()
    {
    }
}  // namespace IWXMVM::UI
