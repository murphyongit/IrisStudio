#include "StdInclude.hpp"
#include "PlayersMenu.hpp"

#include "Mod.hpp"
#include "Resources.hpp"
#include "Components/Actors.hpp"
#include "Components/KillFinder.hpp"
#include "Components/CharacterLogic.hpp"
#include "Components/Loadouts.hpp"
#include "Components/PlayerAnimation.hpp"
#include "Components/PlayerLookLogic.hpp"
#include "UI/Components/ActorsMenu.hpp"
#include "UI/Components/LoadoutsUI.hpp"
#include "UI/Components/PlayerAnimationUI.hpp"
#include "UI/UIManager.hpp"
#include "UI/ImGuiEx/KeyframeableControls.hpp"

namespace IWXMVM::UI
{
    namespace
    {
        // the players of the demo by client number and name, from the kill finder's scan, which may still be running
        std::vector<std::pair<std::int32_t, std::string>> GetDemoPlayers()
        {
            std::vector<std::pair<std::int32_t, std::string>> players;
            if (const auto events = Components::KillFinder::GetEvents())
                for (const auto& [client, player] : events->players)
                    players.emplace_back(client, player.name);
            std::ranges::sort(players, {}, [](const auto& player) { return player.second; });
            return players;
        }

        // the demo's teams with how many players each has, from the same scan
        std::vector<std::pair<std::int32_t, std::size_t>> GetDemoTeams()
        {
            std::vector<std::pair<std::int32_t, std::size_t>> teams;
            if (const auto events = Components::KillFinder::GetEvents())
                for (const auto team : {Components::PlayerLookLogic::TEAM_ALLIES, Components::PlayerLookLogic::TEAM_AXIS})
                    if (const auto count = std::ranges::count_if(
                            events->players, [&](const auto& player) { return player.second.team == team; }))
                        teams.emplace_back(team, static_cast<std::size_t>(count));
            return teams;
        }

        std::string GetTeamName(std::int32_t team)
        {
            const auto catalog = Components::Loadouts::GetCatalog();
            return Components::PlayerLookLogic::GetTeamName(team, catalog ? catalog->bodies : std::vector<std::string>{});
        }

        void Row(const char* label)
        {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label);
            ImGui::SameLine(ImGui::GetFontSize() * 7);
        }

        // a section's header, opened (and scrolled to) once when asked
        bool Header(const char* label, bool open, const char* name, ImGuiTreeNodeFlags flags = 0)
        {
            if (open)
                ImGui::SetNextItemOpen(true);
            const bool shown = ImGui::CollapsingHeader(label, flags);
            ImGuiEx::Keyframeable::Record(name);
            if (open)
                ImGui::SetScrollHereY(0.0f);
            return shown;
        }
    }  // namespace

    void PlayersMenu::Initialize()
    {
    }

    void PlayersMenu::Open(Section section)
    {
        opening = section;
        UIManager::Get().SelectTab(Tab::Scene, Page::Players);
    }

    // who the choices are for: all of them, a team or one player, with what each has of their own
    void PlayersMenu::DrawTargetCombo()
    {
        const auto players = GetDemoPlayers();
        const auto teams = GetDemoTeams();
        const auto animations = Components::PlayerAnimation::GetPlayerAnimations();
        const auto loadouts = Components::Loadouts::GetPlayerLoadouts();
        const auto describe = [&](std::int32_t client, const std::string& name) {
            std::string text = name;
            if (animations.contains(client))
                text += "  " ICON_FA_PERSON_FALLING;
            if (loadouts.contains(client))
                text += "  " ICON_FA_USER_PEN;
            return text;
        };
        const auto describeTeam = [&](std::int32_t team, std::size_t count) {
            std::string text = std::format("{} ({} players)", GetTeamName(team), count);
            if (Components::PlayerAnimation::GetAnimationForTeam(team))
                text += "  " ICON_FA_PERSON_FALLING;
            if (Components::Loadouts::GetLoadoutForTeam(team))
                text += "  " ICON_FA_USER_PEN;
            return text;
        };

        const auto current =
            std::ranges::find(players, target.client.value_or(-1), &std::pair<std::int32_t, std::string>::first);
        if (target.client && current == players.end() && !players.empty())
            target = {};
        const auto team = std::ranges::find(teams, target.team.value_or(-1),
                                            &std::pair<std::int32_t, std::size_t>::first);

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("For");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        const auto preview = target.client ? (current != players.end() ? describe(current->first, current->second)
                                                                       : std::format("Client {}", *target.client))
                             : target.team ? (team != teams.end() ? describeTeam(team->first, team->second)
                                                                  : GetTeamName(*target.team))
                                           : std::string("All players");
        if (ImGui::BeginCombo("##playersTarget", preview.c_str()))
        {
            if (ImGui::Selectable("All players", !target.client && !target.team))
                target = {};
            ImGuiEx::Keyframeable::Record("players.target.all");
            if (!teams.empty())
                ImGui::SeparatorText("Teams");
            for (const auto& [number, count] : teams)
            {
                if (ImGui::Selectable(std::format("{}##team{}", describeTeam(number, count), number).c_str(),
                                      target.team == number))
                    target = {.team = number};
                ImGuiEx::Keyframeable::Record(std::format("players.target.team{}", number));
            }
            if (!players.empty())
                ImGui::SeparatorText("Players");
            for (const auto& [client, name] : players)
            {
                if (ImGui::Selectable(std::format("{}##{}", describe(client, name), client).c_str(),
                                      target.client == client))
                    target = {.client = client};
                ImGuiEx::Keyframeable::Record(std::format("players.target.{}", client));
            }
            ImGui::EndCombo();
        }
        ImGuiEx::Keyframeable::Record("players.target");
        if (players.empty())
            ImGui::TextDisabled("Finding the players of the demo...");
        else
            ImGui::TextDisabled(target.client || target.team ? "Unset options follow All players."
                                                             : "Pick a team or a player to give them their own.");
    }

    // what the game's limit on a model's bones left out of those the choices are for
    void PlayersMenu::DrawLeftOut()
    {
        std::size_t stowed = 0, worn = 0;
        for (const auto& [client, leftOut] : Components::Loadouts::GetLeftOut())
        {
            if ((target.client && client != *target.client) ||
                (target.team && Components::Loadouts::GetTeam(client) != *target.team))
                continue;
            stowed += leftOut.stowed ? 1 : 0;
            worn += leftOut.worn ? 1 : 0;
        }
        const auto line = [&](const char* what, std::size_t players) {
            if (players == 0)
                return;
            ImGui::TextDisabled("%s left out%s", what,
                                target.client ? "" : std::format(" on {}", players).c_str());
            ImGui::SetItemTooltip("A model can't have more than 160 bones");
        };
        line("Stowed gun", stowed);
        line("Worn parts", worn);
    }

    // a list of the paints, each shown live on those it is for as the mouse is on it (a paint is a number: nothing loads)
    void PlayersMenu::DrawFacePaint(const Types::ActorCatalog& catalog, Types::Loadout& loadout)
    {
        const char* keep = target.client || target.team ? "Keep (as for all players)" : "Keep (as in the demo)";
        const auto models = Components::PlayerLookLogic::GetModels(loadout);
        const bool possible = catalog.CanHaveFacePaint(models);
        const auto chosen = loadout.facePaint.value_or(-1);
        Row("Face paint");
        std::optional<std::int32_t> hovered, clicked;
        ImGui::BeginDisabled(!possible);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##playersFacePaint",
                              !possible    ? "No face to paint"
                              : chosen < 0 ? keep
                                           : Components::PlayerLookLogic::GetFacePaintName(chosen).data(),
                              ImGuiComboFlags_HeightLarge))
        {
            clicked = FacePaintRows(chosen, keep, hovered, "players.facepaint");
            ImGui::EndCombo();
        }
        else
        {
            ImGuiEx::Keyframeable::Record("players.facepaint");
        }
        ImGui::EndDisabled();

        if (clicked)
            loadout.facePaint = *clicked < 0 ? std::nullopt : std::optional(*clicked);
        Components::Loadouts::SetFacePaintPreview(
            hovered && *hovered >= 0
                ? std::optional(Components::Loadouts::FacePaintPreview{target.client, target.team, *hovered})
                : std::nullopt);
    }

    // who they look like: a character of the Actors page's list, its parts and a face paint
    void PlayersMenu::DrawLook()
    {
        const auto catalog = Components::Actors::GetCatalog();
        const auto loadouts = Components::Loadouts::GetCatalog();
        if (!catalog || !loadouts)
        {
            ImGui::TextDisabled("Reading what this map has to offer...");
            return;
        }

        auto loadout = Components::Loadouts::GetLoadoutFor(target);
        const auto before = loadout;
        const char* keep = target.client || target.team ? "Keep (as for all players)" : "Keep (as in the demo)";
        const auto character = loadout.character ? catalog->FindCharacter(*loadout.character) : nullptr;
        const auto models = Components::PlayerLookLogic::GetModels(loadout);
        const auto preview =
            character ? Components::CharacterLogic::Describe(*character, catalog->levels) +
                            (catalog->HasModels(models) ? "" : " (loading)")
            : loadout.body ? Components::Actors::DescribeModel(*loadout.body)
                           : std::string(keep);

        Row("Character");
        CharacterPicker("##playersCharacter", *catalog, character, preview, keep, characterFilter, "players.character",
                        "players.character", true, "Who they look like: the map's characters, the campaign's and Zombies'",
                        [&](const Types::Character* chosen) {
                            if (chosen)
                            {
                                Components::PlayerLookLogic::SetCharacter(loadout, *chosen);
                                return;
                            }
                            loadout.character.reset();
                            loadout.body.reset();
                            loadout.head.reset();
                            loadout.worn.reset();
                        });
        DrawLeftOut();

        const bool advanced = ImGui::TreeNode("Advanced##playersLook");
        ImGuiEx::Keyframeable::Record("players.advanced");
        if (advanced)
        {
            UIManager::Get().GetUIComponent<Loadouts>(Component::Loadouts)->DrawParts(*loadouts, loadout);
            ImGui::TreePop();
        }

        DrawFacePaint(*catalog, loadout);
        Row("Stowed");
        UIManager::Get().GetUIComponent<Loadouts>(Component::Loadouts)->DrawStowed(*loadouts, loadout, keep);
        if (loadout != before)
            Components::Loadouts::SetLoadoutFor(target, loadout);
    }

    // a player made to look like one of the actors: all of its look and its weapon
    void PlayersMenu::DrawLikeAnActor()
    {
        if (!target.client)
        {
            ImGui::TextDisabled("Pick a player above to give them an actor's look.");
            return;
        }
        const auto actors = Components::Actors::GetActors();
        if (actors.empty())
        {
            ImGui::TextDisabled("No actors yet: add one on the Actors page, then copy its look here.");
            if (ImGui::Button(ICON_FA_PERSON "  Actors"))
                UIManager::Get().SelectTab(Tab::Scene, Page::Actors);
            return;
        }
        for (const auto& actor : actors)
        {
            ImGui::PushID(static_cast<int>(actor.id));
            if (ImGui::Button(std::format("Look like {}", actor.name).c_str(), ImVec2(-FLT_MIN, 0)))
            {
                const auto error = Components::Actors::CopyLookToPlayer(actor, *target.client);
                const auto players = GetDemoPlayers();
                const auto player =
                    std::ranges::find(players, *target.client, &std::pair<std::int32_t, std::string>::first);
                lookMessage = !error.empty()
                                  ? error
                                  : std::format("{} looks like {} now",
                                                player != players.end() ? player->second : "The player", actor.name);
            }
            ImGui::PopID();
        }
        if (!lookMessage.empty())
            ImGui::TextDisabled("%s", lookMessage.c_str());
    }

    void PlayersMenu::Render()
    {
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse;
        // stays behind the tool windows (Preferences, Controls...) when clicked
        flags |= ImGuiWindowFlags_NoBringToFrontOnFocus;
        if (ImGui::Begin("Players", nullptr, flags))
        {
            if (Mod::GetGameInterface()->GetGameState() != Types::GameState::InDemo)
            {
                DrawInaccessibleTabWarning();
            }
            else
            {
                DrawTargetCombo();
                const auto features = Mod::GetGameInterface()->GetSupportedFeatures();

                if (Header(ICON_FA_SHIRT "  Look", opening == Section::Look, "players.look",
                           ImGuiTreeNodeFlags_DefaultOpen))
                    DrawLook();

                if (Header(ICON_FA_PERSON_FALLING "  Death animation", opening == Section::DeathAnimation,
                           "players.deathAnimation", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    if (features & Types::Features_ChangeAnimations)
                    {
                        auto animations = UIManager::Get().GetUIComponent<PlayerAnimation>(Component::PlayerAnimation);
                        animations->SetTarget(target);
                        animations->RenderContent();
                        // (a click on a player with their own animation picks them)
                        target = animations->GetTarget();
                    }
                    else
                    {
                        ImGui::TextDisabled("Death animations can't be changed in this game yet.");
                    }
                }

                if (Header(ICON_FA_PERSON_RIFLE "  Loadout", opening == Section::Loadout, "players.loadout"))
                {
                    if (features & Types::Features_ChangeLoadouts)
                    {
                        auto loadouts = UIManager::Get().GetUIComponent<Loadouts>(Component::Loadouts);
                        loadouts->SetTarget(target);
                        loadouts->RenderContent();
                    }
                    else
                    {
                        ImGui::TextDisabled("Loadouts can't be changed in this game yet.");
                    }
                }

                if (Header(ICON_FA_USER_PEN "  Look like an actor", opening == Section::LikeAnActor,
                           "players.likeactor"))
                    DrawLikeAnActor();
                opening.reset();
            }
        }
        ImGui::End();
    }

    void PlayersMenu::Release()
    {
    }
}  // namespace IWXMVM::UI
