#include "StdInclude.hpp"
#include "ReloadStrip.hpp"

namespace IWXMVM::UI
{
    namespace
    {
        // how near a marker the mouse grabs it, in pixels
        constexpr float GRAB_DISTANCE = 6.0f;
        constexpr float HANDLE_SIZE = 6.0f;
        constexpr float HATCH_SPACING = 6.0f;
        // how many frames ago a control may have been drawn for the harness to find it
        constexpr int RECENT_FRAMES = 3;

        // the marker being dragged, of which strip, and how far right of the mouse it was grabbed
        struct Drag
        {
            ImGuiID strip = 0;
            std::optional<std::size_t> marker;
            float offset = 0.0f;
        };
        Drag drag;

        std::map<std::string, ReloadControl, std::less<>> controls;
    }  // namespace

    ReloadStripResult ReloadStrip(const char* id, const ReloadStripInput& input)
    {
        ReloadStripResult result;
        const auto& style = ImGui::GetStyle();
        const auto width = std::max(ImGui::GetContentRegionAvail().x, 50.0f);
        const auto height = std::round(ImGui::GetFrameHeight() * 1.6f);
        const auto min = ImGui::GetCursorScreenPos();
        const ImVec2 max(min.x + width, min.y + height);
        ImGui::InvisibleButton(id, ImVec2(width, height));
        const auto strip = ImGui::GetItemID();
        const bool hovered = ImGui::IsItemHovered();
        RecordReloadControl("strip");

        const auto toX = [&](float time) { return min.x + std::clamp(time, 0.0f, 1.0f) * width; };
        const auto toTime = [&](float x) { return std::clamp((x - min.x) / width, 0.0f, 1.0f); };
        const auto mouseX = ImGui::GetIO().MousePos.x;
        // the marker under the mouse: the nearest within reach
        std::optional<std::size_t> under;
        if (hovered || drag.strip == strip)
        {
            auto nearest = GRAB_DISTANCE;
            for (std::size_t i = 0; i < input.markers.size(); ++i)
            {
                const auto distance = std::abs(mouseX - toX(input.markers[i].time));
                if (distance <= nearest)
                {
                    nearest = distance;
                    under = i;
                }
            }
        }

        // a press on a marker drags it, elsewhere it scrubs
        if (ImGui::IsItemActivated())
        {
            drag = {strip, under, under ? toX(input.markers[*under].time) - mouseX : 0.0f};
        }
        const bool dragging = drag.strip == strip && drag.marker && *drag.marker < input.markers.size();
        if (drag.strip == strip && (ImGui::IsItemActive() || ImGui::IsItemDeactivated()))
        {
            if (dragging)
            {
                result.dragged = drag.marker;
                result.time = toTime(mouseX + drag.offset);
                result.released = ImGui::IsItemDeactivated();
            }
            else if (ImGui::IsItemActive())
                result.scrubbed = toTime(mouseX);
            if (ImGui::IsItemDeactivated())
                drag = {};
        }
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && under)
            result.menu = under;
        if (under || dragging)
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (hovered && under && !ImGui::IsItemActive() && !input.markers[*under].tooltip.empty())
            ImGui::SetTooltip("%s", input.markers[*under].tooltip.c_str());
        else if (!under && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && !ImGui::IsItemActive())
            ImGui::SetTooltip("Drag a marker to move it; click or drag elsewhere to go there");

        auto* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_FrameBg), style.FrameRounding);
        // the magazine off the gun
        if (input.markers.size() >= 2)
            draw->AddRectFilled(ImVec2(toX(input.markers.front().time), min.y),
                                ImVec2(toX(input.markers.back().time), max.y), IM_COL32(240, 170, 70, 45));
        // what the clip doesn't show, darker and hatched
        const auto hide = [&](float from, float to) {
            if (to - from < 1.0f)
                return;
            draw->PushClipRect(ImVec2(from, min.y), ImVec2(to, max.y), true);
            draw->AddRectFilled(ImVec2(from, min.y), ImVec2(to, max.y), IM_COL32(0, 0, 0, 90));
            for (auto x = from - height; x < to; x += HATCH_SPACING)
                draw->AddLine(ImVec2(x, max.y), ImVec2(x + height, min.y), IM_COL32(255, 255, 255, 30));
            draw->PopClipRect();
        };
        hide(min.x, toX(input.shownFrom));
        hide(toX(input.shownTo), max.x);
        if (input.playhead && *input.playhead >= 0.0f && *input.playhead <= 1.0f)
        {
            const auto x = std::round(toX(*input.playhead)) + 0.5f;
            draw->AddLine(ImVec2(x, min.y), ImVec2(x, max.y), IM_COL32(255, 255, 255, 210), 1.0f);
        }

        // each marker: a line and a handle at the top (filled for the user's)
        std::vector<float> xs;
        for (std::size_t i = 0; i < input.markers.size(); ++i)
        {
            const auto& marker = input.markers[i];
            const auto x = xs.emplace_back(std::round(toX(marker.time)) + 0.5f);
            const bool active = (dragging && drag.marker == i) || (!dragging && under == i);
            draw->AddLine(ImVec2(x, min.y), ImVec2(x, max.y), marker.color, active ? 3.0f : 1.5f);
            const ImVec2 left(x - HANDLE_SIZE, min.y), right(x + HANDLE_SIZE, min.y), tip(x, min.y + HANDLE_SIZE);
            if (marker.own)
                draw->AddTriangleFilled(left, right, tip, marker.color);
            else
                draw->AddTriangle(left, right, tip, marker.color, 1.5f);
            RecordReloadControl(marker.label, ImVec2(x - GRAB_DISTANCE, min.y), ImVec2(x + GRAB_DISTANCE, max.y));
        }
        // and its label on the upper row, right of it, else left of it, where that crosses no other marker or label;
        // else on the lower row
        auto upperEnd = min.x;
        for (std::size_t i = 0; i < input.markers.size(); ++i)
        {
            const auto& marker = input.markers[i];
            const auto size = ImGui::CalcTextSize(marker.label.c_str());
            const auto x = xs[i];
            const auto fits = [&](float from) {
                const auto to = from + size.x;
                return from >= upperEnd + 2.0f && to <= max.x - 2.0f && (i == 0 || from >= xs[i - 1] + 2.0f) &&
                       (i + 1 == xs.size() || to <= xs[i + 1] - HANDLE_SIZE - 2.0f);
            };
            const auto right = x + HANDLE_SIZE, left = x - HANDLE_SIZE - size.x;
            ImVec2 at(right, min.y + 1.0f);
            if (fits(right) || fits(left))
            {
                at.x = fits(right) ? right : left;
                upperEnd = at.x + size.x;
            }
            else
            {
                at = ImVec2(right + size.x > max.x - 2.0f ? left : right, max.y - size.y - 1.0f);
            }
            draw->AddText(at, marker.color, marker.label.c_str());
        }
        draw->AddRect(min, max, ImGui::GetColorU32(ImGuiCol_Border), style.FrameRounding);
        return result;
    }

    void RecordReloadControl(const std::string& name)
    {
        RecordReloadControl(name, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    }

    void RecordReloadControl(const std::string& name, ImVec2 min, ImVec2 max)
    {
        controls[name] = {min, max, ImGui::GetFrameCount()};
    }

    std::vector<std::pair<std::string, ReloadControl>> GetReloadControls()
    {
        std::vector<std::pair<std::string, ReloadControl>> recent;
        for (const auto& [name, control] : controls)
            if (ImGui::GetFrameCount() - control.frame <= RECENT_FRAMES)
                recent.emplace_back(name, control);
        return recent;
    }
}  // namespace IWXMVM::UI
