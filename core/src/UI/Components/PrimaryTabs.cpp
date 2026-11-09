#include "StdInclude.hpp"
#include "PrimaryTabs.hpp"

#include "Mod.hpp"
#include "Resources.hpp"
#include "UI/UIManager.hpp"
#include "UI/Layout.hpp"
#include "UI/TimelineSelection.hpp"
#include "UI/Toasts.hpp"
#include "UI/ImGuiEx/KeyframeableControls.hpp"
#include "Components/Actors.hpp"
#include "Components/BoneCameraLogic.hpp"
#include "Components/KillstreakLogic.hpp"
#include "Components/CaptureManager.hpp"
#include "Components/KeyframeManager.hpp"
#include "Components/RuntimeProperties.hpp"

namespace IWXMVM::UI
{
    namespace
    {
        struct TabButton
        {
            Tab tab;
            const char* icon;
            const char* label;
            const char* name;  // for the harness
        };
        constexpr std::array<TabButton, static_cast<std::size_t>(Tab::Count)> TABS = {{
            {Tab::Demo, ICON_FA_FILM, "Demo", "demo"},
            {Tab::Camera, ICON_FA_VIDEO, "Camera", "camera"},
            {Tab::Look, ICON_FA_SLIDERS, "Look", "look"},
            {Tab::Scene, ICON_FA_PERSON, "Scene", "scene"},
            {Tab::Record, ICON_FA_CIRCLE, "Record", "record"},
        }};

        struct PageButton
        {
            Page page;
            const char* label;
            const char* tooltip;
            const char* name;  // for the harness
        };

        bool IsLive()
        {
            return Mod::GetGameInterface()->IsHostingLiveGame();
        }

        // the pages of a tab that has two; the Scene tab's are others in a game you host
        std::span<const PageButton> GetPages(Tab tab)
        {
            static constexpr std::array<PageButton, 2> DEMO = {{
                {Page::Moments, ICON_FA_CROSSHAIRS "  Moments", "The demo's kills, to jump to", "moments"},
                {Page::Browse, ICON_FA_FOLDER_OPEN "  Browse", "The demos to load", "browse"},
            }};
            static constexpr std::array<PageButton, 2> SCENE = {{
                {Page::Actors, ICON_FA_PERSON "  Actors", "Characters you place and animate", "actors"},
                {Page::Players, ICON_FA_USERS "  Players",
                 "The demo's players: their death animations, loadouts and looks", "players"},
            }};
            static constexpr std::array<PageButton, 2> LIVE_SCENE = {{
                {Page::Actors, ICON_FA_ROBOT "  Bots", "Bots in your game", "bots"},
                {Page::You, ICON_FA_USER "  You", "Guns, killstreaks and your shots", "you"},
            }};
            if (tab == Tab::Demo)
                return DEMO;
            if (tab == Tab::Scene)
                return IsLive() ? std::span<const PageButton>(LIVE_SCENE) : std::span<const PageButton>(SCENE);
            return {};
        }

        bool IsInDemo()
        {
            return Mod::GetGameInterface()->GetGameState() == Types::GameState::InDemo;
        }

        const Types::KeyframeableProperty& GetCampathProperty()
        {
            return Components::KeyframeManager::Get().GetProperty(Types::KeyframeablePropertyType::CampathCamera);
        }

        bool IsNode(const TimelineSelection::Item& item)
        {
            return item.kind == TimelineSelection::Kind::Key &&
                   item.owner == static_cast<std::uint32_t>(Types::KeyframeablePropertyType::CampathCamera);
        }

        // a key of the bone camera's (shown on the Camera tab, not the Look tab)
        bool IsBoneCameraKey(const TimelineSelection::Item& item)
        {
            return item.kind == TimelineSelection::Kind::Key &&
                   Components::BoneCameraLogic::IsKeyedProperty(
                       static_cast<Types::KeyframeablePropertyType>(item.owner));
        }


        // FNV-1a over numbers, for an id of what is picked (0 for nothing)
        void Mix(std::uint64_t& hash, std::uint64_t value)
        {
            for (int i = 0; i < 8; i++)
            {
                hash ^= (value >> (i * 8)) & 0xFF;
                hash *= 1099511628211ull;
            }
        }

        // "Node 3 at 0:41.20", "3 nodes"
        std::string DescribeKeys(std::span<const TimelineSelection::Item> items,
                                 const Types::KeyframeableProperty& property, std::string_view one,
                                 std::string_view many)
        {
            if (items.size() != 1)
                return std::format("{} {}", items.size(), many);
            const auto& keys = Components::KeyframeManager::Get().GetKeyframes(property);
            const auto it = std::ranges::find(keys, static_cast<std::int32_t>(items.front().id), &Types::Keyframe::id);
            if (it == keys.end())
                return std::string(one);
            if (property.type == Types::KeyframeablePropertyType::CampathCamera)
                return std::format("{} {} at {}", one, it - keys.begin() + 1, Toasts::FormatTime(it->tick));
            return std::format("{} at {}", one, Toasts::FormatTime(it->tick));
        }

        // "Bone Camera FOV > Key at 0:25.00", "Look > 3 keys"
        std::string DescribePropertyKeys(std::span<const TimelineSelection::Item> keys, const char* group)
        {
            const bool oneProperty = std::ranges::all_of(
                keys, [&](const TimelineSelection::Item& item) { return item.owner == keys.front().owner; });
            if (!oneProperty)
                return PanelLogic::JoinPath(std::vector<std::string>{group, std::format("{} keys", keys.size())});
            const auto& property = Components::KeyframeManager::Get().GetProperty(
                static_cast<Types::KeyframeablePropertyType>(keys.front().owner));
            const std::vector<std::string> parts = {std::string(property.name),
                                                    DescribeKeys(keys, property, "Key", "keys")};
            return PanelLogic::JoinPath(parts);
        }

        // The path over what a tab shows: the nodes selected on the Camera tab ("Campath > Node 3 at 0:41.20"), the
        // keys on the Look tab ("Filmtweak Brightness > Key at 0:12.00"), the actor and what of it is picked on the
        // Scene tab's actors ("Reznov > Clip > ai_walk_f")
        std::string GetPath(Tab tab, Page page)
        {
            using TimelineSelection::Item;
            const auto& selection = TimelineSelection::Get();
            if (tab == Tab::Camera)
            {
                std::vector<Item> nodes;
                std::ranges::copy_if(selection, std::back_inserter(nodes), IsNode);
                if (nodes.empty())
                {
                    std::vector<Item> keys;
                    std::ranges::copy_if(selection, std::back_inserter(keys), IsBoneCameraKey);
                    return keys.empty() ? std::string() : DescribePropertyKeys(keys, "Bone camera");
                }
                const std::vector<std::string> parts = {"Campath",
                                                        DescribeKeys(nodes, GetCampathProperty(), "Node", "nodes")};
                return PanelLogic::JoinPath(parts);
            }
            if (tab == Tab::Look)
            {
                std::vector<Item> keys;
                std::ranges::copy_if(selection, std::back_inserter(keys), [](const Item& item) {
                    return item.kind == TimelineSelection::Kind::Key && !IsNode(item) && !IsBoneCameraKey(item) &&
                           !Components::RuntimeProperties::IsActorProperty(
                               static_cast<Types::KeyframeablePropertyType>(item.owner));
                });
                if (keys.empty())
                    return {};
                return DescribePropertyKeys(keys, "Look");
            }
            if (tab == Tab::Scene && page == Page::Actors)
            {
                const auto id = Components::Actors::GetSelected();
                const auto actor = id ? Components::Actors::GetActor(*id) : std::nullopt;
                if (!actor)
                    return {};
                std::vector<std::string> parts = {actor->name};
                const auto clip = Components::Actors::GetSelectedClip();
                const auto faceClip = Components::Actors::GetSelectedFaceClip();
                const auto event = Components::Actors::GetSelectedEvent();
                const auto clipIt =
                    clip ? std::ranges::find(actor->clips, *clip, &Types::ActorClip::id) : actor->clips.end();
                const auto faceClipIt = faceClip ? std::ranges::find(actor->faceClips, *faceClip, &Types::ActorClip::id)
                                                 : actor->faceClips.end();
                const auto eventIt =
                    event ? std::ranges::find(actor->events, *event, &Types::ActorEvent::id) : actor->events.end();
                std::size_t keys = 0;
                std::optional<std::int64_t> keyTick;
                for (const auto& item : selection)
                {
                    if (item.kind == TimelineSelection::Kind::ActorKey && item.owner == actor->id)
                    {
                        keys++;
                        keyTick = item.id;
                    }
                }
                // (a walk of the path: its piece shown, by its look)
                const auto pieces = clipIt != actor->clips.end() ? Components::Actors::GetWalkPieces(*actor)
                                                                 : std::vector<ActorLogic::WalkPiece>{};
                const auto point = Components::Actors::GetSelectedWaypoint();
                const ActorLogic::WalkPiece* piece = nullptr;
                for (const auto& each : pieces)
                    if (each.clip == clipIt->id && (!piece || (point && *point >= each.first && *point <= each.last)))
                        piece = &each;
                if (piece)
                    parts.insert(parts.end(), {"Walk", piece->animation});
                else if (clipIt != actor->clips.end())
                    // (a killstreak's motion by its name: Fly, Hover...)
                    parts.insert(parts.end(),
                                 {"Clip", Components::KillstreakLogic::IsMotion(clipIt->animation)
                                              ? std::string(Components::KillstreakLogic::GetMotionLabel(clipIt->animation))
                                              : clipIt->animation});
                else if (faceClipIt != actor->faceClips.end())
                    parts.insert(parts.end(), {"Face", faceClipIt->animation});
                else if (eventIt != actor->events.end())
                    parts.insert(parts.end(), {"Event", std::format("{} at {}", magic_enum::enum_name(eventIt->type),
                                                                    Toasts::FormatTime(eventIt->tick))});
                else if (keys == 1)
                    parts.push_back(std::format("Key at {}", Toasts::FormatTime(static_cast<std::uint32_t>(*keyTick))));
                else if (keys > 1)
                    parts.push_back(std::format("{} keys", keys));
                return PanelLogic::JoinPath(parts);
            }
            return {};
        }
    }  // namespace

    void PrimaryTabs::Initialize()
    {
    }

    void DrawInaccessibleTabWarning()
    {
        auto label = ICON_FA_TRIANGLE_EXCLAMATION "  Not in a demo";
        auto windowWidth = ImGui::GetWindowSize().x;
        auto textWidth = ImGui::CalcTextSize(label).x;
        ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.5f));
        ImGui::SetCursorPosX((windowWidth - textWidth) * 0.5f);
        ImGui::PushFont(UIManager::Get().GetBoldFont());
        ImGui::Text(label);
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, 5));
        ImGui::TextWrapped("Play a demo first (Demo > Browse).");
        ImGui::Dummy(ImVec2(0, 5));
        if (ImGui::Button(ICON_FA_FOLDER_OPEN "  Browse demos"))
            UIManager::Get().SelectTab(Tab::Demo, Page::Browse);
        ImGuiEx::Keyframeable::Record("panel.browseDemos");
    }

    void PrimaryTabs::RenderSelectedTab()
    {
        // under the tabs, down to the bottom of the panel
        const auto panel = Layout::GetPanel();
        ImGui::SetNextWindowPos(ImVec2(panel.Min.x, GetPosition().y + GetSize().y));
        ImGui::SetNextWindowSize(ImVec2(panel.GetWidth(), std::max(panel.Max.y - GetPosition().y - GetSize().y, 1.0f)));

        const auto tab = UIManager::Get().GetSelectedTab();
        const auto page = PanelLogic::GetShownPage(UIManager::Get().GetPage(tab), IsLive());
        switch (tab)
        {
            case Tab::Demo:
                UIManager::Get()
                    .GetUIComponent(page == Page::Moments ? UI::Component::KillsMenu : UI::Component::DemoLoader)
                    ->Render();
                break;
            case Tab::Camera:
                UIManager::Get().GetUIComponent(UI::Component::CameraMenu)->Render();
                break;
            case Tab::Look:
                UIManager::Get().GetUIComponent(UI::Component::VisualsMenu)->Render();
                break;
            case Tab::Scene:
                // (Bots and You are the Actors window's, see ActorsMenu::Render)
                UIManager::Get()
                    .GetUIComponent(page == Page::Players ? UI::Component::PlayersMenu : UI::Component::ActorsMenu)
                    ->Render();
                break;
            case Tab::Record:
                UIManager::Get().GetUIComponent(UI::Component::CaptureMenu)->Render();
                break;
            default:
                break;
        }
    }

    // The panel follows what is picked to the tab that shows it, once each time it changes: what is selected on the
    // timeline (and nodes in the view), and the actor picked in the view or on the Scene tab
    void PrimaryTabs::FollowPicks()
    {
        // in a demo the Demo tab shows its moments; out of one, the demos, and the other tabs have nothing to show
        const bool inDemo = IsInDemo();
        if (inDemo != wasInDemo)
        {
            wasInDemo = inDemo;
            UIManager::Get().SetPage(inDemo ? Page::Moments : Page::Browse);
            if (!inDemo)
                UIManager::Get().SelectTab(Tab::Demo);
        }
        if (!inDemo)
            return;

        const auto& selection = TimelineSelection::Get();
        std::uint64_t timelineId = 0;
        std::vector<PanelLogic::Kind> kinds;
        if (!selection.empty())
        {
            timelineId = 14695981039346656037ull;
            for (const auto& item : selection)
            {
                Mix(timelineId, static_cast<std::uint64_t>(item.kind));
                Mix(timelineId, item.owner);
                Mix(timelineId, static_cast<std::uint64_t>(item.id));
                kinds.push_back(TimelineSelection::GetPanelKind(item));
            }
        }
        std::optional<PanelLogic::Place> place = timelineFollower.Update(timelineId, PanelLogic::GetPlace(kinds));

        std::uint64_t actorsId = 0;
        if (const auto actor = Components::Actors::GetSelected())
        {
            actorsId = 14695981039346656037ull;
            Mix(actorsId, *actor);
            Mix(actorsId, Components::Actors::GetSelectedClip().value_or(~0u));
            Mix(actorsId, Components::Actors::GetSelectedFaceClip().value_or(~0u));
            Mix(actorsId, Components::Actors::GetSelectedEvent().value_or(~0u));
        }
        if (const auto actorsPlace = actorsFollower.Update(actorsId, PanelLogic::Place{Tab::Scene, Page::Actors}))
        {
            if (!place)
                place = actorsPlace;
        }

        // not while recording, when the tabs can't be changed
        if (place && !Components::CaptureManager::Get().IsCapturing())
        {
            UIManager::Get().SelectTab(place->tab, place->page);
            // (a key: its section opens, and its row shows)
            if (place->tab == Tab::Look)
                UIManager::Get()
                    .GetUIComponent<VisualsMenu>(Component::VisualsMenu)
                    ->ShowRow(static_cast<Types::KeyframeablePropertyType>(selection.begin()->owner));
        }
    }

    // the two pages of the Demo and Scene tabs, as a row of two buttons
    void PrimaryTabs::DrawPages()
    {
        const auto tab = UIManager::Get().GetSelectedTab();
        const auto pages = GetPages(tab);
        if (pages.empty())
            return;

        const auto& style = ImGui::GetStyle();
        const auto width = (ImGui::GetContentRegionAvail().x - style.ItemSpacing.x * (pages.size() - 1)) /
                           static_cast<float>(pages.size());
        const auto current = PanelLogic::GetShownPage(UIManager::Get().GetPage(tab), IsLive());
        for (std::size_t i = 0; i < pages.size(); i++)
        {
            const auto& page = pages[i];
            if (i > 0)
                ImGui::SameLine();
            const bool selected = page.page == current;
            // the demo's moments need a demo
            const bool disabled = page.page == Page::Moments && !IsInDemo();
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  selected ? ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive) : ImVec4(0, 0, 0, 0));
            ImGui::BeginDisabled(disabled);
            if (ImGui::Button(page.label, ImVec2(width, 0)))
                UIManager::Get().SelectTab(tab, page.page);
            ImGui::EndDisabled();
            ImGui::PopStyleColor();
            ImGuiEx::Keyframeable::Record(std::format("page.{}", page.name));
            ImGui::SetItemTooltip("%s", disabled ? "Load a demo first" : page.tooltip);
        }
    }

    // what the tab shows, as a path ("Reznov > Clip > ai_walk_f")
    void PrimaryTabs::DrawPath()
    {
        const auto tab = UIManager::Get().GetSelectedTab();
        const auto path = IsInDemo() ? GetPath(tab, UIManager::Get().GetPage(tab)) : std::string();
        shownPath = path;
        if (path.empty())
            return;
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextUnformatted(ICON_FA_LOCATION_CROSSHAIRS);
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(path.c_str());
        ImGui::PopTextWrapPos();
    }

    void PrimaryTabs::Render()
    {
        // a game of the player's own opens on its actors, all that can be done there
        static bool wasLive = false;
        const bool live = IsLive();
        if (live && !wasLive)
            UIManager::Get().SelectTab(Tab::Scene, Page::Actors);
        wasLive = live;

        FollowPicks();

        // at the top of the side panel, where the layout puts it, as tall as what it shows
        const auto panel = Layout::GetPanel();
        SetPosition(panel.Min);
        ImGui::SetNextWindowPos(GetPosition());
        ImGui::SetNextWindowSize(ImVec2(panel.GetWidth(), GetSize().y));

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                            ImVec2(ImGui::GetFontSize() * 0.5f, ImGui::GetFontSize() * 0.4f));
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking |
                                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                 ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
        // stays behind the tool windows (Preferences, Controls...) when clicked
        flags |= ImGuiWindowFlags_NoBringToFrontOnFocus;
        if (ImGui::Begin("PrimaryTabs", nullptr, flags))
        {
            ImGui::BeginDisabled(Components::CaptureManager::Get().IsCapturing());

            // the tabs: an icon and its name under it, only the icon when the names don't fit
            const auto& style = ImGui::GetStyle();
            const auto count = static_cast<float>(TABS.size());
            const auto width =
                std::floor((ImGui::GetContentRegionAvail().x - style.ItemSpacing.x * (count - 1)) / count);
            const auto line = ImGui::GetTextLineHeight();
            bool iconsOnly = false;
            for (const auto& button : TABS)
                iconsOnly |= ImGui::CalcTextSize(button.label).x + style.FramePadding.x * 2 > width;
            const auto height =
                std::floor(iconsOnly ? ImGui::GetFrameHeight() * 1.5f : line * 2 + style.FramePadding.y * 3);
            for (std::size_t i = 0; i < TABS.size(); i++)
            {
                const auto& button = TABS[i];
                if (i > 0)
                    ImGui::SameLine();
                const bool selected = UIManager::Get().GetSelectedTab() == button.tab;
                ImGui::PushStyleColor(ImGuiCol_Button, selected ? ImGui::GetStyleColorVec4(ImGuiCol_Header)
                                                                : ImGui::GetStyleColorVec4(ImGuiCol_Button));
                if (ImGui::Button(std::format("##tab.{}", button.name).c_str(), ImVec2(width, height)))
                    UIManager::Get().SelectTab(button.tab);
                ImGui::PopStyleColor();
                ImGuiEx::Keyframeable::Record(std::format("tab.{}", button.name));
                if (iconsOnly)
                    ImGui::SetItemTooltip("%s", button.label);

                const auto min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
                auto drawList = ImGui::GetWindowDrawList();
                const auto text =
                    ImGui::GetColorU32(ImGui::IsItemHovered() || selected ? ImGuiCol_Text : ImGuiCol_TextDisabled);
                const auto iconSize = ImGui::CalcTextSize(button.icon);
                const auto top = iconsOnly ? (min.y + max.y - iconSize.y) / 2 : min.y + style.FramePadding.y;
                drawList->AddText(ImVec2(std::floor((min.x + max.x - iconSize.x) / 2), std::floor(top)), text,
                                  button.icon);
                if (!iconsOnly)
                {
                    const auto labelSize = ImGui::CalcTextSize(button.label);
                    drawList->AddText(ImVec2(std::floor((min.x + max.x - labelSize.x) / 2),
                                             std::floor(top + line + style.FramePadding.y * 0.5f)),
                                      text, button.label);
                }
                // the selected tab underlined
                if (selected)
                    drawList->AddRectFilled(ImVec2(min.x, max.y - 2), max, ImGui::GetColorU32(ImGuiCol_CheckMark));
            }

            DrawPages();
            ImGui::EndDisabled();
            DrawPath();

            // as tall as it got, for the tab's window under it
            SetSize(panel.GetWidth(), std::floor(ImGui::GetCursorPosY() + style.WindowPadding.y - style.ItemSpacing.y));
        }
        ImGui::End();
        ImGui::PopStyleVar();

        RenderSelectedTab();
    }

    void PrimaryTabs::Release()
    {
    }
}  // namespace IWXMVM::UI
