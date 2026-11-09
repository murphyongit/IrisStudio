#include "StdInclude.hpp"
#include "Theme.hpp"

#include "Configuration/PreferencesConfiguration.hpp"

namespace IWXMVM::UI::Theme
{
    namespace
    {
        using Palettes::Color;
        using Palettes::Mix;
    }  // namespace

    const Palettes::Palette& Current()
    {
        return Palettes::Get(PreferencesConfiguration::Get().palette);
    }

    ImVec4 Vec4(Color color, float alpha)
    {
        return {color.R() / 255.0f, color.G() / 255.0f, color.B() / 255.0f, alpha};
    }

    ImU32 U32(Color color, float alpha)
    {
        return color.Abgr(static_cast<std::uint8_t>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f));
    }

    void Apply()
    {
        const auto& p = Current();
        // a step up from raised for a hover, the accent mixed in for what is selected or held
        const auto hover = Mix(p.raised, p.text, 0.08f);
        const auto selected = Mix(p.raised, p.accent, 0.30f);
        const auto held = Mix(p.raised, p.accent, 0.40f);

        auto& c = ImGui::GetStyle().Colors;
        c[ImGuiCol_Text] = Vec4(p.text);
        c[ImGuiCol_TextDisabled] = Vec4(p.muted);
        c[ImGuiCol_WindowBg] = Vec4(p.bg);
        c[ImGuiCol_ChildBg] = Vec4(p.bg, 0.0f);
        c[ImGuiCol_PopupBg] = Vec4(p.surface, 0.98f);
        c[ImGuiCol_Border] = Vec4(p.border);
        c[ImGuiCol_BorderShadow] = Vec4(p.bg, 0.0f);
        c[ImGuiCol_FrameBg] = Vec4(p.surface);
        c[ImGuiCol_FrameBgHovered] = Vec4(Mix(p.surface, p.text, 0.07f));
        c[ImGuiCol_FrameBgActive] = Vec4(Mix(p.surface, p.text, 0.12f));
        c[ImGuiCol_TitleBg] = Vec4(p.surface);
        c[ImGuiCol_TitleBgActive] = Vec4(p.raised);
        c[ImGuiCol_TitleBgCollapsed] = Vec4(p.surface);
        c[ImGuiCol_MenuBarBg] = Vec4(p.surface);
        c[ImGuiCol_ScrollbarBg] = Vec4(p.bg, 0.6f);
        c[ImGuiCol_ScrollbarGrab] = Vec4(p.raised);
        c[ImGuiCol_ScrollbarGrabHovered] = Vec4(hover);
        c[ImGuiCol_ScrollbarGrabActive] = Vec4(held);
        c[ImGuiCol_CheckMark] = Vec4(p.accent);
        c[ImGuiCol_SliderGrab] = Vec4(p.accent);
        c[ImGuiCol_SliderGrabActive] = Vec4(p.AccentHover());
        c[ImGuiCol_Button] = Vec4(p.raised);
        c[ImGuiCol_ButtonHovered] = Vec4(hover);
        c[ImGuiCol_ButtonActive] = Vec4(held);
        c[ImGuiCol_Header] = Vec4(selected);
        c[ImGuiCol_HeaderHovered] = Vec4(hover);
        c[ImGuiCol_HeaderActive] = Vec4(held);
        c[ImGuiCol_Separator] = Vec4(p.border);
        c[ImGuiCol_SeparatorHovered] = Vec4(p.muted);
        c[ImGuiCol_SeparatorActive] = Vec4(p.accent);
        c[ImGuiCol_ResizeGrip] = Vec4(p.bg, 0.0f);
        c[ImGuiCol_ResizeGripHovered] = Vec4(p.muted, 0.6f);
        c[ImGuiCol_ResizeGripActive] = Vec4(p.accent, 0.8f);
        c[ImGuiCol_Tab] = Vec4(p.surface);
        c[ImGuiCol_TabHovered] = Vec4(hover);
        c[ImGuiCol_TabActive] = Vec4(selected);
        c[ImGuiCol_TabUnfocused] = Vec4(p.surface);
        c[ImGuiCol_TabUnfocusedActive] = Vec4(p.raised);
        c[ImGuiCol_PlotLines] = Vec4(p.muted);
        c[ImGuiCol_PlotLinesHovered] = Vec4(p.accent);
        c[ImGuiCol_PlotHistogram] = Vec4(p.raised);
        c[ImGuiCol_PlotHistogramHovered] = Vec4(hover);
        c[ImGuiCol_TableHeaderBg] = Vec4(p.raised);
        c[ImGuiCol_TableBorderStrong] = Vec4(p.border);
        c[ImGuiCol_TableBorderLight] = Vec4(Mix(p.border, p.bg, 0.4f));
        c[ImGuiCol_TableRowBg] = Vec4(p.bg, 0.0f);
        c[ImGuiCol_TableRowBgAlt] = Vec4(p.text, 0.03f);
        c[ImGuiCol_TextSelectedBg] = Vec4(p.accent, 0.30f);
        c[ImGuiCol_DragDropTarget] = Vec4(p.accent);
        c[ImGuiCol_NavHighlight] = Vec4(p.accent);
        c[ImGuiCol_NavWindowingHighlight] = Vec4(p.text, 0.7f);
        c[ImGuiCol_NavWindowingDimBg] = Vec4({0x000000}, 0.4f);
        c[ImGuiCol_ModalWindowDimBg] = Vec4({0x000000}, 0.5f);
    }

    ImVec4 Favorite()
    {
        return Vec4(Current().warn);
    }

    ImVec4 Misfit()
    {
        return Vec4(Current().bad);
    }

    ImVec4 Note()
    {
        return Vec4(Current().warn);
    }

    ImVec4 Problem()
    {
        return Vec4(Current().bad);
    }

    ImVec4 Done()
    {
        return Vec4(Current().ok);
    }

    ImU32 Playhead()
    {
        return U32(Current().accent);
    }

    ImU32 SelectionFill()
    {
        return U32(Current().accent, 0.16f);
    }

    ImU32 SelectionEdge()
    {
        return U32(Current().accent, 0.8f);
    }

    ImU32 Card(float alpha)
    {
        return U32(Current().surface, alpha);
    }

    ImVec4 Link(bool hovered)
    {
        const auto& p = Current();
        return Vec4(hovered ? p.AccentHover() : p.accent);
    }

    ImVec4 Keys()
    {
        return Vec4(Current().accent);
    }
}  // namespace IWXMVM::UI::Theme
