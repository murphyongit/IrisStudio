#include "StdInclude.hpp"
#include "GettingStarted.hpp"

#include "UI/Theme.hpp"

#include "Mod.hpp"
#include "Resources.hpp"
#include "Components/CaptureManager.hpp"
#include "Components/KeyframeManager.hpp"
#include "Components/KillFinder.hpp"
#include "Configuration/PreferencesConfiguration.hpp"
#include "UI/Commands.hpp"
#include "UI/ImGuiEx/KeyframeableControls.hpp"
#include "UI/Toasts.hpp"
#include "UI/UIManager.hpp"

namespace IWXMVM::UI::GettingStarted
{
    using GettingStartedLogic::Step;

    namespace
    {
        // the jumps to kills there had been when the steps were last reset
        std::uint32_t jumpsBefore = 0;

        bool IsInDemo()
        {
            return Mod::GetGameInterface()->GetGameState() == Types::GameState::InDemo;
        }

        // a step's name, and what to do for it while it is the next one
        std::pair<std::string, std::string> Describe(Step step)
        {
            const auto key = [](std::string_view id) {
                const auto shortcut = Commands::GetShortcut(id);
                return shortcut.empty() ? std::string("its key") : shortcut;
            };
            switch (step)
            {
                case Step::LoadDemo:
                    return {"Load a demo", "Pick one on the Browse page of this tab and press Play."};
                case Step::JumpToKill:
                    return {"Jump to a kill",
                            std::format("Press {} for the next kill, or click one on the Moments page.",
                                        key("kills.next"))};
                case Step::AddNodes:
                    return {"Fly and add campath nodes",
                            std::format("Hold the right mouse button in the view (or press {}) and fly with WASD, then "
                                        "press {} for a node where the camera is. Two make a path.",
                                        key("fly.toggle"), key("campath.addNode"))};
                case Step::Record:
                    return {"Record", std::format("Mark the clip's start and end with {} and {}, then press Record on "
                                                  "the Record tab.",
                                                  key("capture.setStart"), key("capture.setEnd"))};
                default:
                    return {};
            }
        }
    }  // namespace

    GettingStartedLogic::State GetState()
    {
        const auto& preferences = PreferencesConfiguration::Get();
        GettingStartedLogic::State state;
        state.hidden = preferences.gettingStartedHidden;
        return GettingStartedLogic::WithDoneNames(state, preferences.gettingStartedDone);
    }

    void SetState(const GettingStartedLogic::State& state)
    {
        auto& preferences = PreferencesConfiguration::Get();
        preferences.gettingStartedDone = GettingStartedLogic::GetDoneNames(state);
        preferences.gettingStartedHidden = state.hidden;
        Configuration::Get().Write(true);
    }

    void RegisterCommands()
    {
        Commands::Register({
            .id = "help.gettingStarted",
            .label = "Getting started",
            .category = "Help",
            .run = Show,
            .keywords = "first run tutorial steps card help",
        });
    }

    void Update()
    {
        const auto state = GetState();
        GettingStartedLogic::Seen seen;
        seen.inDemo = IsInDemo();
        seen.jumpedToKill = Components::KillFinder::GetJumps() > jumpsBefore;
        if (seen.inDemo)
        {
            auto& keyframeManager = Components::KeyframeManager::Get();
            seen.nodes = keyframeManager
                             .GetKeyframes(keyframeManager.GetProperty(Types::KeyframeablePropertyType::CampathCamera))
                             .size();
        }
        seen.recording = Components::CaptureManager::Get().IsCapturing();
        const auto updated = GettingStartedLogic::Update(state, seen);
        if (updated == state)
            return;
        SetState(updated);
        if (!updated.hidden)
            Toasts::Show(std::format("Getting started: {} of {} done", GettingStartedLogic::CountDone(updated),
                                     GettingStartedLogic::STEPS));
    }

    void Reset()
    {
        jumpsBefore = Components::KillFinder::GetJumps();
        SetState({});
    }

    void Show()
    {
        auto state = GetState();
        state.hidden = false;
        SetState(state);
        UIManager::Get().SelectTab(Tab::Demo, IsInDemo() ? Page::Moments : Page::Browse);
    }

    void DrawCard()
    {
        auto state = GetState();
        if (state.hidden)
            return;

        const auto fontSize = ImGui::GetFontSize();
        const auto padding = ImVec2(fontSize * 0.6f, fontSize * 0.5f);
        const auto start = ImGui::GetCursorScreenPos();
        const auto width = ImGui::GetContentRegionAvail().x;
        auto drawList = ImGui::GetWindowDrawList();
        // (its background goes under it once its height is known)
        drawList->ChannelsSplit(2);
        drawList->ChannelsSetCurrent(1);

        ImGui::PushID("gettingStarted");
        ImGui::SetCursorScreenPos(start + padding);
        ImGui::BeginGroup();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width - padding.x * 2);

        const auto done = GettingStartedLogic::CountDone(state);
        ImGui::PushFont(UIManager::Get().GetBoldFont());
        ImGui::TextUnformatted(ICON_FA_FLAG_CHECKERED "  Getting started");
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::TextDisabled("%zu of %zu", done, GettingStartedLogic::STEPS);
        ImGui::SameLine();
        const auto closeSize = ImGui::GetFrameHeight();
        ImGui::SetCursorScreenPos(ImVec2(start.x + width - padding.x - closeSize, ImGui::GetCursorScreenPos().y));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        if (ImGui::Button(ICON_FA_XMARK "##close", ImVec2(closeSize, 0)))
        {
            state.hidden = true;
            SetState(state);
            Toasts::Show("Help > Getting started shows it again");
        }
        ImGui::PopStyleColor();
        ImGuiEx::Keyframeable::Record("firstrun.close");
        ImGui::SetItemTooltip("Close it (Help > Getting started shows it again)");

        const auto next = GettingStartedLogic::GetNext(state);
        for (std::size_t i = 0; i < GettingStartedLogic::STEPS; i++)
        {
            const auto step = static_cast<Step>(i);
            const auto [name, hint] = Describe(step);
            if (state.done[i])
                ImGui::TextColored(Theme::Done(), ICON_FA_CIRCLE_CHECK);
            else if (step == next)
                ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_CheckMark), ICON_FA_CIRCLE_RIGHT);
            else
                ImGui::TextDisabled(ICON_FA_CIRCLE);
            ImGui::SameLine();
            if (step == next || state.done[i])
                ImGui::TextUnformatted(name.c_str());
            else
                ImGui::TextDisabled("%s", name.c_str());
            ImGuiEx::Keyframeable::Record(std::format("firstrun.step.{}{}", i, state.done[i] ? ".done" : ""));
            if (step == next)
            {
                ImGui::Indent(fontSize * 1.4f);
                ImGui::TextDisabled("%s", hint.c_str());
                ImGui::Unindent(fontSize * 1.4f);
            }
        }
        if (!next)
            ImGui::TextDisabled("All done. Help > Getting started shows this again.");

        ImGui::PopTextWrapPos();
        ImGui::EndGroup();
        ImGui::PopID();

        const auto end = ImVec2(start.x + width, ImGui::GetItemRectMax().y + padding.y);
        drawList->ChannelsSetCurrent(0);
        drawList->AddRectFilled(start, end, Theme::Card(), fontSize * 0.4f);
        drawList->AddRect(start, end, ImGui::GetColorU32(ImGuiCol_Border), fontSize * 0.4f);
        drawList->ChannelsMerge();
        ImGui::SetCursorScreenPos(ImVec2(start.x, end.y));
        ImGui::Dummy(ImVec2(width, ImGui::GetStyle().ItemSpacing.y));
    }
}  // namespace IWXMVM::UI::GettingStarted
