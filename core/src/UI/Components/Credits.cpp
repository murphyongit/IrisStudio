#include "StdInclude.hpp"
#include "Credits.hpp"

#include "Beta/BetaKey.hpp"
#include "Configuration/PreferencesConfiguration.hpp"
#include "Mod.hpp"
#include "UI/UIManager.hpp"
#include "Version.hpp"

namespace IWXMVM::UI
{
    void Credits::Initialize()
    {
    }

    void Credits::Render()
    {
        if (!visible)
            return;

        // a beta build's key: one line, wrapped, since a name can be 32 bytes, with
        // the window taller by its lines past the first
        constexpr float WIDTH = 300;
        const auto* betaKey = Beta::Active();
        const auto betaLine = betaKey ? Beta::AboutLine(*betaKey) : std::string();
        float betaExtra = 0.0f;
        if (betaKey)
        {
            const float wrap = WIDTH - ImGui::GetStyle().WindowPadding.x * 2;
            betaExtra = ImGui::CalcTextSize(betaLine.c_str(), nullptr, false, wrap).y - ImGui::GetTextLineHeight();
        }

        ImGui::SetNextWindowSize(ImVec2(WIDTH, 400 + betaExtra));
        const char* name = ModName(Mod::GetGameInterface()->GetGame());
        // the client the game runs in, credited
        const auto client = Mod::GetGameInterface()->GetClientName();
        if (ImGui::Begin(std::format("About {}", name).c_str(), &visible,
                         ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse))
        {
            ImGui::Text("%s %s", name, IWXMVM_VERSION);
            ImGui::TextDisabled("A demo movie making mod");
            ImGui::TextDisabled("Based on IWXMVM");
            if (!client.empty())
                ImGui::TextDisabled("Runs on %.*s", static_cast<int>(client.size()), client.data());
            if (betaKey)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::TextWrapped("%s", betaLine.c_str());
                ImGui::PopStyleColor();
            }
            ImGui::Dummy(ImVec2(0, 10));

            ImGui::PushFont(UIManager::Get().GetBoldFont());
            ImGui::Text("IWXMVM Contributors");
            ImGui::PopFont();

            ImGui::Text("luckyy");
            ImGui::Text("Anomaly");
            ImGui::Text("Res");
            ImGui::Text("kruumy");
            ImGui::Text("mollyaitch");

            ImGui::Dummy(ImVec2(0, 20));

            ImGui::PushFont(UIManager::Get().GetBoldFont());
            ImGui::Text("Special Thanks to");
            ImGui::PopFont();

            ImGui::Text("xoxor4d");
            ImGui::Text("Gmzorz");

            // (less by the "Based on" and "Runs on" lines and the beta's, so the content is as tall as before and the
            // window doesn't scroll)
            const float addedLines = 1.0f + (client.empty() ? 0.0f : 1.0f) + (betaKey ? 1.0f : 0.0f);
            ImGui::Dummy(ImVec2(50, std::max(0.0f, 50 - ImGui::GetTextLineHeightWithSpacing() * addedLines)));
        }
        ImGui::End();
    }

    void Credits::Release()
    {
    }
}  // namespace IWXMVM::UI