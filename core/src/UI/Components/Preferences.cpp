#include "StdInclude.hpp"
#include "Preferences.hpp"

#include "UI/Theme.hpp"

#include "Configuration/PreferencesConfiguration.hpp"
#include "UI/UIManager.hpp"
#include "UI/Layout.hpp"

namespace IWXMVM::UI
{
    namespace
    {
        // whether a control changed a preference this frame, to save them once (not every frame of a drag)
        bool changed = false;
    }  // namespace

    void Preferences::Initialize()
    {
    }

    void DrawMiscSection()
    {
        auto& preferences = PreferencesConfiguration::Get();

        // the palette, shared with the launcher: a dot of each one's accent, the one in use ringed
        const auto& current = Theme::Current();
        const float dot = ImGui::GetFrameHeight();
        auto* drawList = ImGui::GetWindowDrawList();
        for (const auto& palette : Palettes::ALL)
        {
            ImGui::PushID(palette.id.data());
            if (&palette != &Palettes::ALL.front())
                ImGui::SameLine(0.0f, dot * 0.35f);
            if (ImGui::InvisibleButton("##palette", ImVec2(dot, dot)))
            {
                preferences.palette = palette.id;
                UIManager::Get().ApplyPalette();
                changed = true;
            }
            const auto center = (ImGui::GetItemRectMin() + ImGui::GetItemRectMax()) * 0.5f;
            if (palette.id == current.id)
                drawList->AddCircle(center, dot * 0.5f - 1.0f, Theme::U32(current.text), 24, 2.0f);
            else if (ImGui::IsItemHovered())
                drawList->AddCircle(center, dot * 0.5f - 1.0f, Theme::U32(current.muted), 24, 1.5f);
            drawList->AddCircleFilled(center, dot * 0.5f - 4.0f, Theme::U32(palette.accent), 24);
            drawList->AddCircle(center, dot * 0.5f - 4.0f, Theme::U32(palette.raised), 24, 1.0f);
            ImGui::SetItemTooltip("%s", std::string(palette.name).c_str());
            ImGui::PopID();
        }
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Palette: %s", std::string(current.name).c_str());
        ImGui::SetItemTooltip("The editor's colors, the launcher's too");
        changed |= ImGui::Checkbox("Developer tools", &preferences.developerTools);
        ImGui::SetItemTooltip("The ImGui demo and the debug panel, in the Help menu");
    }

    void Preferences::Render()
    {
        if (!visible)
            return;

        changed = false;
        if (ImGui::Begin("Preferences", &visible, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse))
        {
            DrawMiscSection();
        }
        ImGui::End();

        // once an edit is done (a drag let go, a box ticked), rather than every second while the window is open
        if (changed)
            Configuration::Get().Write(true);
    }

    void Preferences::Release()
    {
    }
}  // namespace IWXMVM::UI