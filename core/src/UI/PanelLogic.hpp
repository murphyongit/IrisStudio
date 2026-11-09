#pragma once

#include <set>

// The side panel, apart from ImGui so it can be tested: its tabs in the order of the work, the pages of those that have
// two, which tab shows what is picked in the view, on the timeline or in a list, and the panel following a pick there
namespace IWXMVM::UI::PanelLogic
{
    enum class Tab : std::uint8_t
    {
        Demo,    // the demo's moments (its kills) and the demos to load
        Camera,  // the camera's mode and settings, the campath
        Look,    // the visuals
        Scene,   // the actors and the demo's players
        Record,

        Count,
    };

    // the pages of the tabs that have two
    enum class Page : std::uint8_t
    {
        None,
        Moments,  // Demo: the demo's kills
        Browse,   // Demo: the demos to load
        Actors,   // Scene: a demo's actors, and in a game you host its bots
        Players,  // Scene: death animations, loadouts and looks of the demo's players
        You,      // Scene, in a game you host: guns, killstreaks, Canswap, Explosive Bullet
    };

    // a tab and, for one with pages, a page
    struct Place
    {
        Tab tab = Tab::Demo;
        Page page = Page::None;

        bool operator==(const Place&) const = default;
    };

    // which tab a page is on
    Tab GetTab(Page page);
    // The page a tab shows of the one it was left on: the Scene tab has Actors and Players in a demo, Bots (Actors) and
    // You in a game you host, so what's on one side shows as its first page on the other
    Page GetShownPage(Page page, bool live);

    // what is picked: a campath node, a key of a property (the Look tab's), a key of the bone camera's (the Camera
    // tab's), an actor, or one of its clips, events or keys, or a marker
    enum class Kind : std::uint8_t
    {
        Node,
        Key,
        CameraKey,
        Actor,
        Clip,
        Event,
        ActorKey,
        LightKey,
        Marker,
    };

    // The place that shows what is picked: nodes and the bone camera's keys the Camera tab, keys the Look tab, actors and their clips, events and
    // keys the Scene tab's actors; none for markers, nothing, or a mix of those that show in different places
    std::optional<Place> GetPlace(std::span<const Kind> kinds);

    // the pages of the Scene tab's actor
    enum class ActorPage : std::uint8_t
    {
        Look,
        Animation,
        Movement,
        Events,
    };

    // The actor's page that shows what is picked, when it is one clip (Animation) or one event (Events); none for
    // anything else, which leaves the page as it is
    std::optional<ActorPage> GetActorPage(std::span<const Kind> kinds);

    // The sections of the actor whose Advanced is on, as config.json's actorsAdvanced
    // keeps them: by the names the pages' folds had before the panel's one Advanced switch, "look", "movement", "clip"
    // (Animation's) and "event" (Events'), written in that order. Read, "all" (written by the one switch) turns all
    // four on and unknown names are dropped.
    std::set<ActorPage> ReadAdvancedSections(std::span<const std::string> names);
    std::vector<std::string> WriteAdvancedSections(const std::set<ActorPage>& sections);

    // the path shown over what a tab shows, "Reznov > Clip > ai_walk_f"
    std::string JoinPath(std::span<const std::string> parts);

    // Follows picks to the place that shows them: told what is picked each frame, as an id that changes with it (0 for
    // nothing), it gives the place to go to once each time it changes to something shown somewhere. So the user can
    // go to another tab while it stays picked, and picking the same again after something else goes back.
    class Follower
    {
       public:
        std::optional<Place> Update(std::uint64_t id, std::optional<Place> place);

       private:
        std::uint64_t seen = 0;
    };
}  // namespace IWXMVM::UI::PanelLogic
