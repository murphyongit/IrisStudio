#include "StdInclude.hpp"
#include "PlayerAnimationUI.hpp"

#include "UI/Theme.hpp"

#include "Components/PlayerAnimation.hpp"
#include "Components/PlayerLookLogic.hpp"
#include "IconsFontAwesome6.h"
#include "Mod.hpp"
#include "UI/Components/ActorsMenu.hpp"
#include "UI/Components/PrimaryTabs.hpp"
#include "UI/ImGuiEx/KeyframeableControls.hpp"
#include "UI/UIManager.hpp"

namespace IWXMVM::UI
{
    std::string DescribeAnimation(const std::string& name)
    {
        return name.empty() ? "Original animation" : name;
    }

    namespace
    {
        // what a player or team plays of its own, or what is chosen for all players
        std::optional<std::string> GetChosen(const Components::Loadouts::Target& target)
        {
            if (target.client)
                return Components::PlayerAnimation::GetAnimationForPlayer(*target.client);
            if (target.team)
                return Components::PlayerAnimation::GetAnimationForTeam(*target.team);
            return Components::PlayerAnimation::GetAnimationForAll();
        }

        void Choose(const Components::Loadouts::Target& target, std::optional<std::string_view> name)
        {
            if (target.client)
                Components::PlayerAnimation::SetAnimationForPlayer(*target.client, name);
            else if (target.team)
                Components::PlayerAnimation::SetAnimationForTeam(*target.team, name);
            else
                Components::PlayerAnimation::SetAnimationForAll(name.value_or(""));
        }
    }  // namespace

    void PlayerAnimation::DrawAnimationList(ImVec2 size)
    {
        // what the target plays now; nullopt for a player or team that follows the animation for all players
        const auto forAll = Components::PlayerAnimation::GetAnimationForAll();
        const auto chosen = GetChosen(target);
        const auto choose = [&](std::optional<std::string_view> name) { Choose(target, name); };

        if (!ImGui::BeginChild("##deathAnimations", size, true))
        {
            ImGui::EndChild();
            return;
        }

        if ((target.client || target.team) &&
            ImGui::Selectable(std::format("Same as all players ({})", DescribeAnimation(forAll)).c_str(), !chosen))
            choose(std::nullopt);

        if (ImGui::Selectable("Original animation", chosen && chosen->empty()))
            choose("");

        const std::string_view search(filter.data());
        for (const auto& [name, index] : Components::PlayerAnimation::GetAnimations())
        {
            const bool favorite = Components::PlayerAnimation::IsFavorite(name);
            if ((favoritesOnly && !favorite) || (!search.empty() && name.find(search) == std::string_view::npos))
                continue;

            ImGui::PushID(name.data());
            ImGui::PushStyleColor(ImGuiCol_Text, favorite ? Theme::Favorite()
                                                          : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            if (ImGui::SmallButton(ICON_FA_STAR))
                Components::PlayerAnimation::SetFavorite(name, !favorite);
            ImGui::PopStyleColor(2);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(favorite ? "Remove from favorites" : "Add to favorites");

            ImGui::SameLine();
            if (ImGui::Selectable(name.data(), chosen && *chosen == name))
                choose(name);
            if (ImGui::IsItemHovered())
                hoveredAnimation = name;
            ImGui::PopID();
        }
        ImGui::EndChild();
    }

    // A bone on the screen, looking at the model from the side yaw (radians) turns to, z up
    ImVec2 Project(const glm::vec3& position, float yaw)
    {
        const float side = -position.x * std::sin(yaw) + position.y * std::cos(yaw);
        return ImVec2(-side, -position.z);
    }

    std::optional<std::pair<std::string_view, std::uint32_t>> FindAnimation(std::optional<std::string_view> name)
    {
        if (!name || name->empty())
            return std::nullopt;
        const auto& anims = Components::PlayerAnimation::GetAnimations();
        const auto it = std::ranges::find(anims, *name, &std::pair<std::string_view, std::uint32_t>::first);
        return it != anims.end() ? std::optional(*it) : std::nullopt;
    }

    void PlayerAnimation::DrawPreview(ImVec2 size)
    {
        if (!ImGui::BeginChild("##deathAnimationPreview", size, true, ImGuiWindowFlags_NoScrollbar))
        {
            ImGui::EndChild();
            return;
        }

        // turned by dragging; the drag is the preview's, so it doesn't move the window
        const auto area = ImGui::GetContentRegionAvail();
        const auto origin = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##deathAnimationPreviewDrag", ImVec2(std::max(area.x, 1.0f), std::max(area.y, 1.0f)));
        if (ImGui::IsItemActive())
            previewYaw += ImGui::GetIO().MouseDelta.x * 0.01f;
        ImGui::SetCursorScreenPos(origin);

        // the animation hovered in the list, else the one the player or all players play
        auto anim = FindAnimation(hoveredAnimation);
        if (!anim)
        {
            const auto chosen = target.client || target.team ? GetChosen(target) : std::nullopt;
            anim = FindAnimation(chosen.value_or(Components::PlayerAnimation::GetAnimationForAll()));
        }

        auto& gameInterface = *Mod::GetGameInterface();
        const auto first = anim ? gameInterface.GetAnimationPose(anim->second, 0.0f) : std::nullopt;
        if (!first)
        {
            ImGui::TextDisabled(anim ? "No preview for this animation" : "Hover an animation\nto preview it");
            ImGui::EndChild();
            return;
        }

        if (previewAnimation != anim->second)
        {
            previewAnimation = anim->second;
            previewStart = ImGui::GetTime();
            previewBounds.reset();
        }

        // framed to fit the whole animation, so the view stays put while it plays
        if (!previewBounds || previewBoundsYaw != previewYaw)
        {
            ImVec2 min(0, 0), max(0, 0);
            for (int i = 0; i <= 8; ++i)
            {
                if (const auto pose = gameInterface.GetAnimationPose(anim->second, i / 8.0f))
                {
                    for (const auto& position : pose->positions)
                    {
                        const auto p = Project(position, previewYaw);
                        min = ImVec2(std::min(min.x, p.x), std::min(min.y, p.y));
                        max = ImVec2(std::max(max.x, p.x), std::max(max.y, p.y));
                    }
                }
            }
            previewBounds = std::pair(min, max);
            previewBoundsYaw = previewYaw;
        }

        // plays, holds the last frame for a moment and starts again
        constexpr double HOLD = 0.75;
        const double elapsed = std::fmod(ImGui::GetTime() - previewStart, first->duration + HOLD);
        const auto pose = gameInterface.GetAnimationPose(anim->second, static_cast<float>(elapsed / first->duration));
        if (!pose)
        {
            ImGui::EndChild();
            return;
        }

        const auto [min, max] = *previewBounds;
        const auto textHeight = ImGui::GetTextLineHeightWithSpacing();
        const float scale = std::min(area.x / std::max(max.x - min.x, 1.0f),
                                     (area.y - textHeight) / std::max(max.y - min.y, 1.0f)) * 0.9f;
        const ImVec2 center(origin.x + area.x / 2, origin.y + (area.y - textHeight) / 2);
        const ImVec2 middle((min.x + max.x) / 2, (min.y + max.y) / 2);
        const auto toScreen = [&](const glm::vec3& position) {
            const auto p = Project(position, previewYaw);
            return ImVec2(center.x + (p.x - middle.x) * scale, center.y + (p.y - middle.y) * scale);
        };

        auto drawList = ImGui::GetWindowDrawList();
        const auto groundY = toScreen(glm::vec3(0)).y;
        drawList->AddLine(ImVec2(origin.x, groundY), ImVec2(origin.x + area.x, groundY),
                          ImGui::GetColorU32(ImGuiCol_TextDisabled));

        const auto color = ImGui::GetColorU32(ImGuiCol_Text);
        for (std::size_t b = 0; b < pose->positions.size(); ++b)
        {
            // the bones hanging from the model's origin would draw lines from the floor
            const auto parent = pose->parents[b];
            if (parent < 0 || pose->parents[parent] < 0)
                continue;
            drawList->AddLine(toScreen(pose->positions[parent]), toScreen(pose->positions[b]), color, 2.0f);
        }

        ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + area.y - textHeight));
        ImGui::TextDisabled("%s", anim->first.data());
        ImGui::EndChild();
    }

    void PlayerAnimation::DrawPlayerAnimations(const Types::DemoEvents* events)
    {
        const auto playerAnimations = Components::PlayerAnimation::GetPlayerAnimations();
        if (playerAnimations.empty())
            return;

        ImGui::Text("Players with their own animation");
        for (const auto& [client, name] : playerAnimations)
        {
            ImGui::PushID(client);
            if (ImGui::SmallButton(ICON_FA_XMARK))
                Components::PlayerAnimation::SetAnimationForPlayer(client, std::nullopt);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Back to the animation for all players");
            ImGui::SameLine();
            const auto player = events ? events->GetPlayerName(client) : std::format("Client {}", client);
            if (ImGui::Selectable(std::format("{}: {}", player, DescribeAnimation(name)).c_str(), target.client == client))
                target = {.client = client};
            ImGui::PopID();
        }
    }

    // Any death of the game from the animation browser the Actors page has; the list is the
    // game's own for players
    void PlayerAnimation::DrawBrowse()
    {
        constexpr auto BROWSER = "##playersDeathBrowser";
        const auto catalog = Components::Actors::GetCatalog();
        const auto loadouts = Components::Loadouts::GetCatalog();
        if (!catalog)
            return;
        if (catalog != countedCatalog)
        {
            countedCatalog = catalog;
            catalogDeaths = static_cast<std::size_t>(std::ranges::count_if(catalog->animations, [](const auto& animation) {
                return Components::Actors::GetAnimationCategory(animation) == "Deaths";
            }));
        }

        const auto chosen = GetChosen(target);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Death");
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", chosen ? DescribeAnimation(*chosen).c_str() : "Same as all players");
        const auto label = ICON_FA_FOLDER_OPEN " Browse...";
        ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetContentRegionMax().x - ImGui::CalcTextSize(label).x -
                                                             ImGui::GetStyle().FramePadding.x * 2));
        if (ImGui::Button(label))
        {
            browsedAnimation = chosen.value_or("");
            browserCategory = "Deaths";
            ImGui::OpenPopup(BROWSER);
        }
        ImGuiEx::Keyframeable::Record("players.browse");
        ImGui::TextDisabled("The game's %zu in the list, %zu in Browse...",
                            Components::PlayerAnimation::GetAnimations().size(), catalogDeaths);

        // what fits the body they'd die in
        auto models = Components::PlayerLookLogic::GetModels(Components::Loadouts::GetLoadoutFor(target));
        if (models.empty() && loadouts && !loadouts->bodies.empty())
            models.push_back(loadouts->bodies.front());
        if (AnimationBrowser(BROWSER, *catalog, models, browsedAnimation, browserFilter, browserCategory))
            Choose(target, browsedAnimation);
    }

    // (the Players page draws it, see RenderContent)
    void PlayerAnimation::Render()
    {
    }

    void PlayerAnimation::RenderContent()
    {
        if (ImGui::Button(ICON_FA_REPEAT " Reset"))
        {
            Components::PlayerAnimation::ResetAnimations();
            Components::PlayerAnimation::HideAllCorpses() = false;
            Components::PlayerAnimation::AttachWeaponToCorpse() = false;
        }
        ImGui::SetItemTooltip("Every player back to the death animations of the demo");
        ImGui::SameLine();
        ImGui::Checkbox(ICON_FA_STAR " Favorites only", &favoritesOnly);
        DrawBrowse();
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##deathAnimationFilter", ICON_FA_MAGNIFYING_GLASS " Search", filter.data(),
                                 filter.size());

        // the list and a preview of the animation hovered or chosen: beside it, or under it in a narrow panel
        hoveredAnimation.reset();
        const auto line = ImGui::GetTextLineHeightWithSpacing();
        if (ImGui::GetContentRegionAvail().x < ImGui::GetFontSize() * 30)
        {
            DrawAnimationList(ImVec2(0, line * 10));
            DrawPreview(ImVec2(0, line * 9));
        }
        else
        {
            DrawAnimationList(ImVec2(ImGui::GetContentRegionAvail().x * 0.55f, line * 16));
            ImGui::SameLine();
            DrawPreview(ImVec2(0, line * 16));
        }

        // the players come from the demo's scan, which may still be running
        const auto events = Mod::GetGameInterface()->GetDemoEvents();
        ImGui::Spacing();
        DrawPlayerAnimations(events.get());

        ImGui::Spacing();
        ImGui::Checkbox("Hide all corpses", &Components::PlayerAnimation::HideAllCorpses());
        ImGui::Checkbox("Attach weapon to player", &Components::PlayerAnimation::AttachWeaponToCorpse());
        if (const auto animName = Components::PlayerAnimation::GetLatestAnimationName(); !animName.empty())
            ImGui::TextDisabled("Latest death animation: %s", animName.data());
    }

    void PlayerAnimation::Release()
    {
    }

    void PlayerAnimation::Initialize()
    {
    }
} // namespace IWXMVM::UI
