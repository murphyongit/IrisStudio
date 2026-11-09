#include "StdInclude.hpp"
#include "ViewMenu.hpp"

#include "Resources.hpp"
#include "Mod.hpp"
#include "Components/Actors.hpp"
#include "Components/CameraManager.hpp"
#include "Components/KillstreakLogic.hpp"
#include "Components/Playback.hpp"
#include "Configuration/PreferencesConfiguration.hpp"
#include "UI/ActorActions.hpp"
#include "UI/AreaMap.hpp"
#include "UI/Commands.hpp"
#include "UI/GaitColors.hpp"
#include "UI/UIManager.hpp"
#include "UI/Components/ActorsMenu.hpp"
#include "UI/ImGuiEx/KeyframeableControls.hpp"

namespace IWXMVM::UI::ViewMenu
{
    namespace
    {
        constexpr auto MENU_POPUP = "##viewMenu";
        constexpr auto BROWSER_POPUP = "##viewMenuBrowser";
        constexpr auto EFFECT_BROWSER_POPUP = "##viewMenuEffectBrowser";
        constexpr auto PROP_BROWSER_POPUP = "##viewMenuPropBrowser";

        // what was right-clicked
        std::optional<std::uint32_t> clickedActor;
        std::optional<std::size_t> clickedPoint, clickedLine;
        ImVec2 clickedAt;
        glm::vec3 rayOrigin{}, rayDirection{};
        bool openRequested = false;
        bool open = false;

        // the animation browser, for the actor to play what is chosen (Play > Browse...), or to die with it at the
        // playhead (Die > Browse..., on the Deaths)
        std::optional<std::uint32_t> browsingFor;
        bool browserRequested = false;
        bool browsingToDie = false;
        std::string browserChoice, browserFilter, browserCategory = "All", dieCategory = "Deaths";
        // the effect browser, for an effect on the ground clicked (Add Effect here > Browse...), and where that was
        bool effectBrowserRequested = false;
        bool browsingEffect = false;
        glm::vec3 effectOrigin{}, effectDirection{};
        std::string effectChoice, effectFilter, effectCategory = "All";
        // the prop browser, for a prop on the ground clicked (Add Prop here > Browse...), and where that was on screen
        bool propBrowserRequested = false;
        bool browsingProp = false;
        glm::vec3 propOrigin{}, propDirection{};
        ImVec2 propClickedAt;
        std::string propChoice, propFilter, propGroup = "map";

        // what the items are recorded as, for the harness: the view's, or the timeline path bar's
        std::string recordPrefix = "viewmenu.";

        void Record(const std::string& name)
        {
            ImGuiEx::Keyframeable::Record(recordPrefix + name);
        }

        // a tooltip, also on an item that is greyed out (then why)
        void Tooltip(const char* text, const std::optional<std::string>& unavailable = std::nullopt)
        {
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayShort))
                ImGui::SetTooltip("%s", unavailable ? unavailable->c_str() : text);
        }

        // The favourite animations and the recent ones the game has, for Play: those that fit the actor's models (F3,
        // always on; while its models load, none until that is known)
        void DrawPlay(const Types::Actor& actor)
        {
            const auto catalog = Components::Actors::GetCatalog();
            const auto has = [&](const std::string& name) { return catalog && catalog->FindAnimation(name); };
            const bool waiting =
                !Components::Actors::GetFits(actor.models) && Components::Actors::AreModelsLoading(actor.models);
            std::size_t leftOut = 0;
            const auto shown = [&](const std::string& name) {
                if (!has(name))
                    return false;
                if (!Components::Actors::Fits(actor.models, name).value_or(true))
                {
                    ++leftOut;
                    return false;
                }
                return true;
            };
            std::vector<std::string> favourites, recent;
            for (const auto& name : PreferencesConfiguration::Get().favoriteActorAnimations)
                if (shown(name))
                    favourites.push_back(name);
            for (const auto& name : Components::Actors::GetRecentAnimations())
                if (std::ranges::find(favourites, name) == favourites.end() && shown(name))
                    recent.push_back(name);

            const auto list = [&](const char* title, const std::vector<std::string>& names) {
                if (names.empty())
                    return;
                ImGui::SeparatorText(title);
                for (const auto& name : names)
                {
                    const auto animation = catalog->FindAnimation(name);
                    const auto duration = std::format("{:.1f} s", animation->duration);
                    if (ImGui::MenuItem(name.c_str(), duration.c_str()))
                        ActorActions::Play(actor.id, name);
                    Record("play." + name);
                    Tooltip("Plays it from the playhead");
                }
            };
            if (waiting)
                ImGui::TextDisabled(ICON_FA_HOURGLASS_HALF " Loading its models...");
            else
            {
                list(ICON_FA_STAR " Favorites", favourites);
                list(ICON_FA_CLOCK_ROTATE_LEFT " Recent", recent);
                if (favourites.empty() && recent.empty())
                    ImGui::TextDisabled("No favorites or recent ones");
            }
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_MAGNIFYING_GLASS " Browse..."))
            {
                browsingFor = actor.id;
                browserRequested = true;
                browsingToDie = false;
                browserChoice.clear();
            }
            Record("play.browse");
        }

        // how a killstreak goes along a path, as a verb ("Drive", "Fly"); empty for others
        std::string_view GetVehicleGoes(const Types::Actor& actor)
        {
            const auto killstreak = Components::KillstreakLogic::FindByModels(actor.models);
            if (!killstreak)
                return {};
            return Components::KillstreakLogic::Get(*killstreak).vehicleClass == Types::VehicleClass::Car ? "Drive"
                                                                                                            : "Fly";
        }

        // Character: the favourites and the map's own, the others in the panel's picker
        void DrawCharacters(const Types::Actor& actor)
        {
            const auto catalog = Components::Actors::GetCatalog();
            if (!catalog)
                return;
            const auto kind = Components::Actors::GetActorKind(actor);
            const auto current = Components::Actors::GetCharacter(actor, *catalog);
            const auto item = [&](const Types::Character& character) {
                if (ImGui::MenuItem(character.name.c_str(), nullptr, current && current->id == character.id))
                {
                    auto changed = actor;
                    Components::Actors::ApplyCharacter(changed, character, {}, *catalog);
                    Components::Actors::Update(changed, false, "change the character of " + actor.name);
                    Components::Actors::Select(actor.id);
                }
                Record("character." + character.id);
            };
            bool any = false;
            for (const auto& character : catalog->characters)
                if (Components::Actors::IsFavoriteCharacter(character.id) && character.burntOf.empty())
                {
                    if (!any)
                        ImGui::SeparatorText(ICON_FA_STAR " Favorites");
                    any = true;
                    item(character);
                }
            ImGui::SeparatorText("This map");
            for (const auto& character : catalog->characters)
                if (character.level.empty() && character.burntOf.empty() &&
                    Components::CharacterLogic::GetActorKind(character.kind) == kind)
                    item(character);
            ImGui::Separator();
            if (ImGui::MenuItem("More..."))
            {
                Components::Actors::Select(actor.id);
                UIManager::Get().ShowPicked(PanelLogic::Kind::Actor);
            }
            Record("character.more");
            Tooltip("All characters, in the panel's Character");
        }

        void DrawActorMenu(const Types::Actor& actor)
        {
            // an effect or a light in the scene: what can be done to it
            if (actor.effect || actor.light)
            {
                ImGui::TextDisabled(actor.light ? ICON_FA_LIGHTBULB " %s" : ICON_FA_WAND_MAGIC_SPARKLES " %s",
                                    actor.name.c_str());
                ImGui::Separator();
                if (ImGui::MenuItem(ICON_FA_CLONE " Duplicate", Commands::GetShortcut("edit.duplicate").c_str()))
                    ActorActions::Duplicate(actor.id);
                Record("duplicate");
                if (ImGui::MenuItem(ICON_FA_TRASH " Delete", Commands::GetShortcut("edit.delete").c_str()))
                    ActorActions::Delete(actor.id);
                Record("delete");
                return;
            }
            const auto vehicle = GetVehicleGoes(actor);
            ImGui::TextDisabled(ICON_FA_PERSON " %s", actor.name.c_str());
            ImGui::Separator();
            const bool playing = ImGui::BeginMenu(ICON_FA_FILM " Play");
            if (!playing)
                Record("play");
            if (playing)
            {
                DrawPlay(actor);
                ImGui::EndMenu();
            }

            // what can follow the clip shown at the playhead (a killstreak has no transitions)
            const auto shown = ActorActions::GetClipAtPlayhead(actor);
            const bool following = vehicle.empty() && ImGui::BeginMenu(ICON_FA_FORWARD " Then", shown.has_value());
            if (!following && vehicle.empty())
            {
                Record("then");
                Tooltip("A transition or animation after the clip at the playhead: a stop, a turn, a crouch...",
                        shown ? std::nullopt : std::optional<std::string>("No clip at the playhead"));
            }
            if (following)
            {
                DrawThenItems(actor, *shown, "viewmenu.then");
                ImGui::EndMenu();
            }

            // its character (a killstreak's is its own)
            if (vehicle.empty())
            {
                const bool characters = ImGui::BeginMenu(ICON_FA_SHIRT " Character");
                if (!characters)
                    Record("character");
                if (characters)
                {
                    DrawCharacters(actor);
                    ImGui::EndMenu();
                }
            }

            const auto walk = ActorActions::GetWalkUnavailableReason(actor.id, true);
            if (ImGui::MenuItem(vehicle.empty() ? ICON_FA_PERSON_WALKING " Walk to..."
                                                : std::format(ICON_FA_ROUTE " {} to...", vehicle).c_str(),
                                nullptr, false, !walk))
                ActorActions::WalkOn(actor.id);
            Record("walk");
            Tooltip("From where it is at the playhead along the points you click next; Enter when done", walk);
            // a killstreak dies as the game's do, its own first
            if (const auto killstreak = Components::KillstreakLogic::FindByModels(actor.models))
            {
                const auto deaths =
                    Components::KillstreakLogic::GetDeaths(Components::KillstreakLogic::Get(*killstreak));
                const bool dying = !deaths.empty() && ImGui::BeginMenu(ICON_FA_SKULL " Die");
                if (!dying)
                {
                    Record("die");
                    if (deaths.empty())
                        ImGui::TextDisabled(ICON_FA_SKULL " Never dies in the game");
                }
                if (dying)
                {
                    for (const auto death : deaths)
                    {
                        if (ImGui::MenuItem(std::string(Components::KillstreakLogic::GetMotionLabel(death)).c_str()))
                            ActorActions::DieHere(actor.id, std::string(death));
                        Record(death == deaths.front() ? std::string("die.first") : "die." + std::string(death));
                        ImGui::SetItemTooltip("%s",
                                              std::string(Components::KillstreakLogic::DescribeMotion(death)).c_str());
                    }
                    ImGui::EndMenu();
                }
            }
            if (vehicle.empty())
            {
                if (ImGui::MenuItem(ICON_FA_VIDEO " Look at camera"))
                    ActorActions::LookAtCamera(actor.id);
                Record("look");
                Tooltip("Turns its head to the camera from the playhead");
                const auto deaths = ActorActions::GetDeaths(actor);
                const bool dying = ImGui::BeginMenu(ICON_FA_SKULL " Die", !deaths.empty());
                if (!dying)
                {
                    Record("die");
                    Tooltip("Is hit and dies at the playhead",
                            deaths.empty() ? std::optional<std::string>("This map has no death for it") : std::nullopt);
                }
                if (dying)
                {
                    // those that suit what the clip before does, then any death of
                    // the browser's
                    const auto candidates =
                        Components::Actors::GetDeathCandidates(actor, Components::Playback::GetActorTick(), 6);
                    ImGui::SeparatorText(std::format("Suit it {}", candidates.motion).c_str());
                    for (const auto& death : candidates.suited)
                    {
                        if (ImGui::MenuItem(death.c_str()))
                            ActorActions::DieHere(actor.id, death);
                        Record(death == candidates.suited.front() ? std::string("die.first") : "die." + death);
                    }
                    if (candidates.suited.empty())
                        ImGui::TextDisabled("None of this body's");
                    ImGui::Separator();
                    if (ImGui::MenuItem(ICON_FA_MAGNIFYING_GLASS " Browse..."))
                    {
                        browsingFor = actor.id;
                        browserRequested = true;
                        browsingToDie = true;
                        browserChoice.clear();
                        dieCategory = "Deaths";
                    }
                    Record("die.browse");
                    Tooltip("The animation browser on the Deaths; the one you pick dies at the playhead");
                    ImGui::EndMenu();
                }
            }

            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_CLONE " Duplicate", Commands::GetShortcut("edit.duplicate").c_str()))
                ActorActions::Duplicate(actor.id);
            Record("duplicate");
            // beside it, and along another's path with it
            if (ImGui::BeginMenu(ICON_FA_PEOPLE_ARROWS " Duplicate beside"))
            {
                if (ImGui::MenuItem("Left"))
                    ActorActions::DuplicateBeside(actor.id, true);
                Record("beside.left");
                if (ImGui::MenuItem("Right"))
                    ActorActions::DuplicateBeside(actor.id, false);
                Record("beside.right");
                ImGui::EndMenu();
            }
            else
                Record("beside");
            Tooltip("A copy 48 units to that side of its path, at the same times");
            const auto all = Components::Actors::GetActors();
            std::vector<const Types::Actor*> leaders;
            for (const auto& other : all)
                if (other.id != actor.id && other.movement == Types::ActorMovement::Path && other.path.size() >= 2)
                    leaders.push_back(&other);
            if (ImGui::BeginMenu(ICON_FA_PERSON_RUNNING " Run with", !leaders.empty()))
            {
                for (std::size_t i = 0; i < leaders.size(); ++i)
                {
                    if (ImGui::MenuItem(std::format("{}##runwith{}", leaders[i]->name, i).c_str()))
                        ActorActions::RunWith(actor.id, leaders[i]->id);
                    Record(std::format("runwith.{}", i));
                }
                ImGui::EndMenu();
            }
            else
                Record("runwith");
            Tooltip("Goes along another actor's path, beside it, at the gait and pace that get it there with it",
                    leaders.empty() ? std::optional<std::string>("No other actor has a path") : std::nullopt);
            if (ImGui::MenuItem(ICON_FA_TRASH " Delete", Commands::GetShortcut("edit.delete").c_str()))
                ActorActions::Delete(actor.id);
            Record("delete");
        }

        // the gaits of an actor's body as menu items with their colours; returns the one chosen
        std::optional<std::pair<Types::ActorGait, bool>> DrawGaits(const Types::Actor& actor, const char* suffix,
                                                                   const std::string& record)
        {
            std::optional<std::pair<Types::ActorGait, bool>> chosen;
            for (const auto& preset : Components::Actors::GetGaits(actor))
            {
                const auto gait = preset.gait == ActorLogic::Gait::Sprint ? Types::ActorGait::Sprint
                                  : preset.gait == ActorLogic::Gait::Run  ? Types::ActorGait::Run
                                                                          : Types::ActorGait::Walk;
                const bool crouched = preset.stance == Types::ActorStance::Crouch;
                if (DotMenuItem(std::format("{}{}", preset.name, suffix).c_str(), GetGaitColor(gait, crouched)))
                    chosen = std::pair(gait, crouched);
                std::string name(preset.name);
                std::erase(name, ' ');
                std::ranges::transform(name, name.begin(), [](char c) { return static_cast<char>(std::tolower(c)); });
                Record(record + "." + name);
            }
            return chosen;
        }

        void DrawGroundMenu()
        {
            const auto add = ActorActions::GetAddUnavailableReason();
            if (ImGui::MenuItem(ICON_FA_USER_PLUS " Add Actor", nullptr, false, !add))
                ActorActions::AddActor(rayOrigin, rayDirection);
            Record("add.actor");
            Tooltip("A person of the map, standing here", add);
            if (ImGui::BeginMenu(ICON_FA_HELICOPTER " Add Killstreak"))
            {
                for (const auto& info : Components::KillstreakLogic::GetAll())
                {
                    const auto unavailable = ActorActions::GetKillstreakUnavailableReason(info.killstreak);
                    if (ImGui::MenuItem(std::string(info.name).c_str(), nullptr, false, !unavailable))
                        ActorActions::AddKillstreak(info.killstreak, rayOrigin, rayDirection);
                    Record("add.killstreak." + ActorActions::GetKillstreakKey(info.killstreak));
                    Tooltip(ActorActions::GetKillstreakTooltip(info.killstreak).c_str(), unavailable);
                }
                // (no model: an actor of its own)
                const auto mortar = ActorActions::GetMortarUnavailableReason();
                if (ImGui::MenuItem(std::string(Components::KillstreakLogic::MORTAR_NAME).c_str(), nullptr, false,
                                    !mortar))
                    ActorActions::AddMortar(rayOrigin, rayDirection);
                Record("add.killstreak.mortarteam");
                Tooltip("Three points about here, its shells falling on them", mortar);
                // (an actor of its own kind, listed where a player gets them)
                const auto dog = ActorActions::GetAddUnavailableReason(Types::ActorKind::Dog);
                if (ImGui::MenuItem("Dog", nullptr, false, !dog))
                    ActorActions::AddActor(rayOrigin, rayDirection, Types::ActorKind::Dog);
                Record("add.dog");
                Tooltip("The map's german shepherd, standing here", dog);
                ImGui::EndMenu();
            }
            else
                Record("add.killstreak");
            // an effect of the map there: the recent ones, then the browser
            if (ImGui::BeginMenu(ICON_FA_WAND_MAGIC_SPARKLES " Add Effect here"))
            {
                const auto recent = Components::Actors::GetRecentEffects();
                std::size_t listed = 0;
                for (const auto& effect : recent)
                {
                    if (listed == 5 || !Components::Actors::GetEffectInfo(effect))
                        continue;
                    ++listed;
                    if (ImGui::MenuItem(effect.substr(effect.find_last_of('/') + 1).c_str()))
                        ActorActions::AddEffect(effect, rayOrigin, rayDirection);
                    Record("add.effect." + effect);
                    Tooltip(effect.c_str());
                }
                if (listed == 0)
                    ImGui::TextDisabled("No recent ones");
                ImGui::Separator();
                if (ImGui::MenuItem(ICON_FA_MAGNIFYING_GLASS " Browse..."))
                {
                    effectOrigin = rayOrigin;
                    effectDirection = rayDirection;
                    effectBrowserRequested = true;
                    browsingEffect = true;
                    effectChoice.clear();
                }
                Record("add.effect.browse");
                Tooltip("The map's effects; the one you pick plays here from the playhead");
                ImGui::EndMenu();
            }
            else
                Record("add.effect");
            // a model of the map there: the recent ones, then the browser
            if (ImGui::BeginMenu(ICON_FA_CUBE " Add Prop here"))
            {
                const auto catalog = Components::Actors::GetCatalog();
                std::size_t listed = 0;
                for (const auto& model : Components::Actors::GetRecentProps())
                {
                    if (listed == 5 || !catalog || !std::ranges::binary_search(catalog->props, model))
                        continue;
                    ++listed;
                    if (ImGui::MenuItem(model.c_str()))
                        ActorActions::AddProp(model, rayOrigin, rayDirection);
                    Record("add.prop." + model);
                }
                if (listed == 0)
                    ImGui::TextDisabled("No recent ones");
                ImGui::Separator();
                if (ImGui::MenuItem(ICON_FA_MAGNIFYING_GLASS " Browse..."))
                {
                    propOrigin = rayOrigin;
                    propDirection = rayDirection;
                    propClickedAt = clickedAt;
                    propBrowserRequested = true;
                    browsingProp = true;
                    propChoice.clear();
                }
                Record("add.prop.browse");
                Tooltip("The map's models; the one you pick stands here, facing you");
                ImGui::EndMenu();
            }
            else
                Record("add.prop");
            // a light there
            if (ImGui::MenuItem(ICON_FA_LIGHTBULB " Add Light here: Point"))
                ActorActions::AddLight(Types::LightKind::Point, rayOrigin, rayDirection);
            Record("add.light.point");
            Tooltip("Lights all around it, just off what you clicked");
            if (ImGui::MenuItem(ICON_FA_LIGHTBULB " Add Light here: Spot"))
                ActorActions::AddLight(Types::LightKind::Spot, rayOrigin, rayDirection);
            Record("add.light.spot");
            Tooltip("A cone from the camera onto what you clicked");
            // the level's parts
            if (AreaMap::HasAreas())
            {
                if (ImGui::MenuItem(ICON_FA_MAP " Go to Area", Commands::GetShortcut("area.map").c_str()))
                    AreaMap::Open();
                Record("area");
            }

            // the selected actor, there
            const auto selected = Components::Actors::GetSelected();
            const auto actor = selected ? Components::Actors::GetActor(*selected) : std::nullopt;
            if (!actor)
                return;
            // (an effect: put there, facing out of what was clicked)
            if (actor->effect)
            {
                ImGui::SeparatorText(actor->name.c_str());
                if (ImGui::MenuItem(ICON_FA_LOCATION_CROSSHAIRS " Move here"))
                    Components::Actors::RequestPlacement(actor->id, rayOrigin, rayDirection);
                Record("effect.movehere");
                Tooltip("Puts it here, facing out of what you clicked");
                return;
            }
            // (the Mortar Team: a point of its there)
            if (actor->mortar)
            {
                ImGui::SeparatorText(actor->name.c_str());
                for (std::size_t i = 0; i < actor->mortar->points.size(); ++i)
                {
                    if (ImGui::MenuItem(std::format(ICON_FA_LOCATION_DOT " Point {} here", i + 1).c_str()))
                        Components::Actors::RequestPlacement(actor->id, rayOrigin, rayDirection, i);
                    Record(std::format("mortar.point.{}", i));
                }
                Tooltip("Where its shells fall");
                return;
            }
            // (a prop: stands there, keyed at the playhead when its place has keys)
            if (actor->placedProp)
            {
                ImGui::SeparatorText(actor->name.c_str());
                if (ImGui::MenuItem(ICON_FA_LOCATION_CROSSHAIRS " Move here"))
                    Components::Actors::RequestPlacement(actor->id, rayOrigin, rayDirection);
                Record("prop.movehere");
                Tooltip("Stands it here");
                return;
            }
            // (a light: put there; a spot can be aimed there instead; on a bone, neither)
            if (actor->light)
            {
                if (actor->light->follow)
                    return;
                ImGui::SeparatorText(actor->name.c_str());
                const bool spot = actor->light->kind == Types::LightKind::Spot;
                if (ImGui::MenuItem(ICON_FA_LOCATION_CROSSHAIRS " Move here"))
                    Components::Actors::RequestPlacement(actor->id, rayOrigin, rayDirection);
                Record("light.movehere");
                Tooltip(spot ? "Puts it at the camera, aimed at what you clicked" : "Puts it just off what you clicked");
                if (spot)
                {
                    if (ImGui::MenuItem(ICON_FA_BULLSEYE " Aim here"))
                        Components::Actors::RequestAim(actor->id, rayOrigin, rayDirection);
                    Record("light.aimhere");
                    Tooltip("Points it at what you clicked, from where it is");
                }
                return;
            }
            ImGui::SeparatorText(actor->name.c_str());
            const auto walk = ActorActions::GetWalkUnavailableReason(actor->id, false);
            if (const auto vehicle = GetVehicleGoes(*actor); !vehicle.empty())
            {
                if (ImGui::MenuItem(std::format(ICON_FA_ROUTE " {} here", vehicle).c_str(), nullptr, false, !walk))
                    ActorActions::WalkTo(actor->id, rayOrigin, rayDirection);
                Record("walkto");
                Tooltip("Goes here from where it is at the playhead, at its speed, and stops", walk);
            }
            else if (walk)
            {
                ImGui::BeginDisabled();
                ImGui::MenuItem(ICON_FA_PERSON_WALKING " Walk here");
                ImGui::EndDisabled();
                Tooltip("", walk);
            }
            else if (const auto gait = DrawGaits(*actor, " here", "walkto"))
                ActorActions::WalkTo(actor->id, rayOrigin, rayDirection, false, gait->first, gait->second);
            if (actor->movement == Types::ActorMovement::Path)
            {
                if (ImGui::MenuItem(ICON_FA_LOCATION_DOT " Add point here"))
                    ActorActions::AddPathPoint(actor->id, rayOrigin, rayDirection);
                Record("point");
                Tooltip("After the selected point, or the last");
            }
            if (ImGui::MenuItem(ICON_FA_LOCATION_CROSSHAIRS " Move here"))
                ActorActions::MoveHere(actor->id, rayOrigin, rayDirection);
            Record("movehere");
            Tooltip(actor->movement == Types::ActorMovement::Path ? "Its path, from its first point here"
                                                                  : "Stands here at the playhead");
        }

        // a point of the selected actor's path
        void DrawPointMenu(const Types::Actor& actor, std::size_t point)
        {
            ImGui::TextDisabled(ICON_FA_LOCATION_DOT " %s",
                                point == 0 ? "Start" : std::format("Point {}", point).c_str());
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_TRASH " Delete point", Commands::GetShortcut("edit.delete").c_str(), false,
                                actor.path.size() > 1))
                ActorActions::DeletePathPoint(actor.id, point);
            Record("point.delete");
            if (point + 1 < actor.path.size() && !Components::Actors::GetGaits(actor).empty())
            {
                if (ImGui::BeginMenu(ICON_FA_PERSON_RUNNING " Gait from here"))
                {
                    if (const auto gait = DrawGaits(actor, "", "point.gait"))
                        ActorActions::SetGait(actor.id, point, actor.path.size(), gait->first, gait->second);
                    ImGui::EndMenu();
                }
                else
                    Record("point.gait");
            }
            if (point + 1 < actor.path.size() && !Components::Actors::GetGaits(actor).empty())
            {
                if (ImGui::BeginMenu(ICON_FA_HOURGLASS_HALF " Wait here"))
                {
                    const auto* stretch = ActorLogic::GetStretch(actor, point);
                    const auto now = stretch ? stretch->wait : 0u;
                    for (const auto wait : {0u, 1000u, 2000u, 5000u})
                    {
                        if (ImGui::MenuItem(wait == 0 ? "No wait" : std::format("{} s", wait / 1000).c_str(), nullptr,
                                            now == wait))
                            ActorActions::SetWait(actor.id, point, wait);
                        Record(std::format("point.wait.{}", wait / 1000));
                    }
                    ImGui::EndMenu();
                }
                else
                    Record("point.wait");
            }
            if (ImGui::MenuItem(ICON_FA_FLAG_CHECKERED " End path here", nullptr, false, point + 1 < actor.path.size()))
                ActorActions::EndPathAt(actor.id, point);
            Record("point.end");
            Tooltip("Takes out the points after it");
            if (ImGui::MenuItem(ICON_FA_LOCATION_CROSSHAIRS " Move to crosshair"))
                ActorActions::MovePointToCrosshair(actor.id, point);
            Record("point.crosshair");
            Tooltip("To the ground where the camera looks");
        }

        // the line from a point of the selected actor's path to the next; Insert point here puts it where insert says
        void DrawLineMenu(const Types::Actor& actor, std::size_t line, const std::function<void()>& insert)
        {
            ImGui::TextDisabled(ICON_FA_ROUTE " %s",
                                line == 0 ? "Start to point 1" : std::format("Point {} to {}", line, line + 1).c_str());
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_LOCATION_DOT " Insert point here"))
                insert();
            Record("line.insert");
            if (!Components::Actors::GetGaits(actor).empty())
            {
                if (ImGui::BeginMenu(ICON_FA_PERSON_RUNNING " Gait of this stretch"))
                {
                    if (const auto gait = DrawGaits(actor, "", "line.gait"))
                        ActorActions::SetGait(actor.id, line, line + 1, gait->first, gait->second);
                    ImGui::EndMenu();
                }
                else
                    Record("line.gait");
            }
        }
    }  // namespace

    void DrawPathPointMenu(const Types::Actor& actor, std::size_t point, const std::string& record)
    {
        recordPrefix = record;
        DrawPointMenu(actor, point);
        recordPrefix = "viewmenu.";
    }

    void DrawPathLineMenu(const Types::Actor& actor, std::size_t line, std::uint32_t insertTick,
                          const std::string& record)
    {
        recordPrefix = record;
        DrawLineMenu(actor, line, [&] {
            // on the ground where the actor is then: straight down from above it
            const auto placement = Components::Actors::EvaluatePlacement(actor, insertTick);
            ActorActions::InsertPathPoint(actor.id, line, placement.position + glm::vec3(0.0f, 0.0f, 64.0f),
                                          glm::vec3(0.0f, 0.0f, -1.0f));
        });
        recordPrefix = "viewmenu.";
    }

    void Open(std::optional<std::uint32_t> actor, glm::vec2 at, glm::vec3 origin, glm::vec3 direction,
              std::optional<std::size_t> point, std::optional<std::size_t> line)
    {
        // (a point or line is the selected actor's)
        clickedPoint = point;
        clickedLine = point ? std::nullopt : line;
        clickedActor = point || line ? Components::Actors::GetSelected() : actor;
        clickedAt = ImVec2(at.x, at.y);
        rayOrigin = origin;
        rayDirection = direction;
        // (the actor is selected, and the panel shows it, once something is chosen: a tab window appearing meanwhile
        // takes the focus, which closes the menu)
        openRequested = true;
    }

    void Draw()
    {
        if (openRequested)
        {
            ImGui::OpenPopup(MENU_POPUP);
            ImGui::SetNextWindowPos(clickedAt);
            openRequested = false;
        }
        open = ImGui::BeginPopup(MENU_POPUP);
        if (open)
        {
            const auto actor = clickedActor ? Components::Actors::GetActor(*clickedActor) : std::nullopt;
            if (clickedActor && !actor)
                ImGui::CloseCurrentPopup();
            else if (actor && clickedPoint && *clickedPoint < actor->path.size())
                DrawPointMenu(*actor, *clickedPoint);
            else if (actor && clickedLine && *clickedLine + 1 < actor->path.size())
                DrawLineMenu(*actor, *clickedLine, [&] {
                    ActorActions::InsertPathPoint(actor->id, *clickedLine, rayOrigin, rayDirection);
                });
            else if (actor && !clickedPoint && !clickedLine)
                DrawActorMenu(*actor);
            else
                DrawGroundMenu();
            ImGui::EndPopup();
        }

        // Add Effect here > Browse...: the effect browser over the view
        if (effectBrowserRequested)
        {
            ImGui::OpenPopup(EFFECT_BROWSER_POPUP);
            effectBrowserRequested = false;
        }
        if (browsingEffect)
        {
            const auto catalog = Components::Actors::GetCatalog();
            if (catalog && EffectBrowser(EFFECT_BROWSER_POPUP, *catalog, effectChoice, effectFilter, effectCategory,
                                         ICON_FA_LOCATION_DOT " Place it here"))
                ActorActions::AddEffect(effectChoice, effectOrigin, effectDirection);
            if (!ImGui::IsPopupOpen(EFFECT_BROWSER_POPUP))
                browsingEffect = false;
        }

        // Add Prop here > Browse...: the prop browser beside where it will stand, which shows the row hovered there
        if (propBrowserRequested)
        {
            ImGui::OpenPopup(PROP_BROWSER_POPUP);
            propBrowserRequested = false;
        }
        if (browsingProp)
        {
            const auto catalog = Components::Actors::GetCatalog();
            if (catalog && PropBrowser(PROP_BROWSER_POPUP, *catalog, propChoice, propFilter, propGroup,
                                       ICON_FA_LOCATION_DOT " Place it here",
                                       {propOrigin, propDirection, ActorActions::GetYawFacingCamera()}, propClickedAt))
                ActorActions::AddProp(propChoice, propOrigin, propDirection);
            if (!ImGui::IsPopupOpen(PROP_BROWSER_POPUP))
                browsingProp = false;
        }

        // Play > Browse... and Die > Browse...: the browser over the view
        if (browserRequested)
        {
            ImGui::OpenPopup(BROWSER_POPUP);
            browserRequested = false;
        }
        if (!browsingFor)
            return;
        const auto actor = Components::Actors::GetActor(*browsingFor);
        const auto catalog = Components::Actors::GetCatalog();
        if (!actor || !catalog)
        {
            browsingFor.reset();
            return;
        }
        if (AnimationBrowser(BROWSER_POPUP, *catalog, actor->models, browserChoice, browserFilter,
                             browsingToDie ? dieCategory : browserCategory))
        {
            if (browsingToDie)
                ActorActions::DieHere(actor->id, browserChoice);
            else
                ActorActions::Play(actor->id, browserChoice);
        }
        if (!ImGui::IsPopupOpen(BROWSER_POPUP))
            browsingFor.reset();
    }

    State GetState()
    {
        return {open, open ? clickedActor : std::nullopt, open ? clickedPoint : std::nullopt,
                open ? clickedLine : std::nullopt};
    }
}  // namespace IWXMVM::UI::ViewMenu
