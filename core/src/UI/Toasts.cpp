#include "StdInclude.hpp"
#include "Toasts.hpp"

#include "UI/Theme.hpp"

#include "Components/UndoHistory.hpp"
#include "UI/TimelineMath.hpp"

namespace IWXMVM::UI::Toasts
{
    namespace
    {
        struct Toast
        {
            std::string text;
            double shownAt = 0;
            std::uint64_t undoStep = 0;  // 0 without undo
            std::string action;          // the link's label instead of Undo, and what it runs
            std::function<void()> run;
        };

        constexpr double SHOWN_FOR = 4.0;
        constexpr double FADES_FOR = 0.5;
        constexpr std::size_t MAX_SHOWN = 3;

        // (shown and drawn on the render thread)
        std::vector<Toast> toasts;
    }  // namespace

    void Show(std::string text, bool undo)
    {
        LOG_INFO("{}", text);
        toasts.push_back({std::move(text), ImGui::GetTime(), undo ? Components::UndoHistory::Get().GetLatest() : 0});
        if (toasts.size() > MAX_SHOWN)
            toasts.erase(toasts.begin());
    }

    void Show(std::string text, std::string action, std::function<void()> run)
    {
        LOG_INFO("{} \xC2\xB7 {}", text, action);
        toasts.push_back({std::move(text), ImGui::GetTime(), 0, std::move(action), std::move(run)});
        if (toasts.size() > MAX_SHOWN)
            toasts.erase(toasts.begin());
    }

    void Clear()
    {
        toasts.clear();
    }

    std::vector<std::string> GetShown()
    {
        std::vector<std::string> shown;
        for (const auto& toast : toasts)
            shown.push_back(toast.text);
        return shown;
    }

    std::string FormatTime(std::uint32_t tick)
    {
        return TimelineMath::FormatTime(tick);
    }

    std::vector<std::pair<ImVec2, ImVec2>> Draw(ImVec2 min, ImVec2 max, bool mouseOver)
    {
        std::vector<std::pair<ImVec2, ImVec2>> drawn;
        const auto now = ImGui::GetTime();
        std::erase_if(toasts, [&](const Toast& toast) { return now - toast.shownAt > SHOWN_FOR; });
        if (toasts.empty())
            return drawn;

        const auto& style = ImGui::GetStyle();
        const auto padding = ImVec2(ImGui::GetFontSize() * 0.8f, ImGui::GetFontSize() * 0.4f);
        const auto latest = Components::UndoHistory::Get().GetLatest();
        auto bottom = max.y;
        auto drawList = ImGui::GetWindowDrawList();

        // the latest at the bottom
        for (auto it = toasts.rbegin(); it != toasts.rend(); ++it)
        {
            const auto& toast = *it;
            const auto age = now - toast.shownAt;
            const auto alpha = static_cast<float>(std::clamp((SHOWN_FOR - age) / FADES_FOR, 0.0, 1.0));
            const bool undoable = (toast.undoStep != 0 && toast.undoStep == latest) || toast.run;

            const auto textSize = ImGui::CalcTextSize(toast.text.c_str());
            const char* undoLabel = toast.run ? toast.action.c_str() : "Undo";
            const auto undoWidth = undoable ? ImGui::CalcTextSize(" \xC2\xB7 ").x + ImGui::CalcTextSize(undoLabel).x : 0;
            const auto size = ImVec2(textSize.x + undoWidth + padding.x * 2, textSize.y + padding.y * 2);
            const auto position = ImVec2(std::floor((min.x + max.x - size.x) / 2), bottom - size.y);
            if (position.y < min.y)
                break;

            drawList->AddRectFilled(position, position + size, Theme::Card(0.92f * alpha),
                                    style.FrameRounding + 4);
            drawn.emplace_back(position, position + size);
            drawList->AddText(position + padding, ImGui::GetColorU32(ImGuiCol_Text, alpha), toast.text.c_str());

            if (undoable)
            {
                const auto separator = position + padding + ImVec2(textSize.x, 0);
                drawList->AddText(separator, ImGui::GetColorU32(ImGuiCol_TextDisabled, alpha), " \xC2\xB7 ");
                const auto linkPosition = separator + ImVec2(ImGui::CalcTextSize(" \xC2\xB7 ").x, 0);
                const bool hovered =
                    mouseOver && ImGui::IsMouseHoveringRect(linkPosition, linkPosition + ImGui::CalcTextSize(undoLabel));
                const bool clicked = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
                auto link = Theme::Link(hovered);
                link.w = alpha;
                drawList->AddText(linkPosition, ImGui::ColorConvertFloat4ToU32(link), undoLabel);
                if (hovered)
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                if (clicked)
                {
                    // (once: the message goes)
                    if (auto run = std::move(it->run))
                    {
                        toasts.erase(std::next(it).base());
                        run();
                    }
                    else if (const auto label = Components::UndoHistory::Get().Undo())
                        Show("Undid " + *label);
                    break;
                }
            }
            bottom = position.y - style.ItemSpacing.y;
        }
        return drawn;
    }
}  // namespace IWXMVM::UI::Toasts
