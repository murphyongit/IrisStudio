#pragma once
#include "Types/Actor.hpp"

namespace IWXMVM::UI
{
    // The colours of gaits, the same in the view's path line, the timeline's path bar,
    // the menus and the gait buttons: walk blue, run amber, sprint red, crouched violet; a wait white. Flat colours.
    inline ImVec4 GetGaitColor(Types::ActorGait gait, bool crouched)
    {
        if (crouched)
            return {0.63f, 0.49f, 0.90f, 1.0f};
        switch (gait)
        {
            case Types::ActorGait::Walk:
                return {0.31f, 0.70f, 0.91f, 1.0f};
            case Types::ActorGait::Run:
                return {0.91f, 0.64f, 0.23f, 1.0f};
            case Types::ActorGait::Sprint:
                return {0.88f, 0.33f, 0.25f, 1.0f};
        }
        return {0.31f, 0.70f, 0.91f, 1.0f};
    }
    inline constexpr ImVec4 WAIT_COLOR{0.90f, 0.90f, 0.90f, 1.0f};

    // a menu item with a dot of a colour at its left (its label indented for it); returns whether it was chosen
    inline bool DotMenuItem(const char* label, ImVec4 color, bool selected = false, bool enabled = true)
    {
        const auto chosen = ImGui::MenuItem(std::format("     {}", label).c_str(), nullptr, selected, enabled);
        const auto min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
        const auto radius = ImGui::GetFontSize() * 0.24f;
        ImGui::GetWindowDrawList()->AddCircleFilled(
            ImVec2(min.x + ImGui::GetStyle().FramePadding.x + radius * 1.5f, (min.y + max.y) / 2.0f), radius,
            ImGui::GetColorU32(enabled ? color : ImVec4(color.x, color.y, color.z, 0.4f)));
        return chosen;
    }
}  // namespace IWXMVM::UI
