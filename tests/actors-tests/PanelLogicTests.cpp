#include "StdInclude.hpp"
#include "Test.hpp"

#include "UI/GettingStartedLogic.hpp"
#include "UI/PanelLogic.hpp"

#include <set>

using namespace IWXMVM::UI;
using PanelLogic::Kind;
using PanelLogic::Page;
using PanelLogic::Place;
using PanelLogic::Tab;

namespace
{
    std::optional<Place> PlaceOf(std::initializer_list<Kind> kinds)
    {
        const std::vector<Kind> all(kinds);
        return PanelLogic::GetPlace(all);
    }
}  // namespace

TEST("what is picked opens the tab that shows it")
{
    CHECK(PlaceOf({Kind::Node}) == Place{Tab::Camera});
    CHECK(PlaceOf({Kind::Node, Kind::Node}) == Place{Tab::Camera});
    CHECK(PlaceOf({Kind::Key}) == Place{Tab::Look});
    // the bone camera's keys are its rows' on the Camera tab, with the campath's nodes
    CHECK(PlaceOf({Kind::CameraKey}) == Place{Tab::Camera});
    CHECK(PlaceOf({Kind::CameraKey, Kind::Node}) == Place{Tab::Camera});
    CHECK(!PlaceOf({Kind::CameraKey, Kind::Key}));
    CHECK(PlaceOf({Kind::Actor}) == (Place{Tab::Scene, Page::Actors}));
    CHECK(PlaceOf({Kind::Clip}) == (Place{Tab::Scene, Page::Actors}));
    // an actor's clips, events and keys together are all shown on its page
    CHECK(PlaceOf({Kind::Clip, Kind::Event, Kind::ActorKey}) == (Place{Tab::Scene, Page::Actors}));
    CHECK(PlaceOf({Kind::LightKey}) == (Place{Tab::Scene, Page::Actors}));
    // markers have no tab, and a mix of what different tabs show stays where it is
    CHECK(!PlaceOf({Kind::Marker}));
    CHECK(!PlaceOf({Kind::Node, Kind::Key}));
    CHECK(!PlaceOf({Kind::Node, Kind::Clip}));
    CHECK(!PlaceOf({Kind::Clip, Kind::Marker}));
    CHECK(!PlaceOf({}));
}

TEST("a clip or event picked opens the actor's page that shows it, anything else leaves the page")
{
    const auto pageOf = [](std::initializer_list<Kind> kinds) {
        const std::vector<Kind> all(kinds);
        return PanelLogic::GetActorPage(all);
    };
    CHECK(pageOf({Kind::Clip}) == PanelLogic::ActorPage::Animation);
    CHECK(pageOf({Kind::Event}) == PanelLogic::ActorPage::Events);
    // the actor, its keys (on the page that shows them now), or more than one, stay where they are
    CHECK(!pageOf({Kind::Actor}));
    CHECK(!pageOf({Kind::ActorKey}));
    CHECK(!pageOf({Kind::Clip, Kind::Clip}));
    CHECK(!pageOf({Kind::Clip, Kind::Event}));
    CHECK(!pageOf({Kind::Node}));
    CHECK(!pageOf({}));
}

TEST("Advanced is kept per section by the old fold names, written back in the sections' order")
{
    using ActorPage = PanelLogic::ActorPage;
    const std::vector<std::string> read{"event", "clip"};
    const auto sections = PanelLogic::ReadAdvancedSections(read);
    CHECK((sections == std::set<ActorPage>{ActorPage::Animation, ActorPage::Events}));
    CHECK((PanelLogic::WriteAdvancedSections(sections) == std::vector<std::string>{"clip", "event"}));
    CHECK((PanelLogic::WriteAdvancedSections({ActorPage::Events, ActorPage::Look, ActorPage::Movement}) ==
           std::vector<std::string>{"look", "movement", "event"}));
    CHECK(PanelLogic::WriteAdvancedSections({}).empty());
    CHECK(PanelLogic::ReadAdvancedSections(std::vector<std::string>{}).empty());
}

TEST("the panel-wide Advanced from before turns every section on; unknown names are dropped")
{
    using ActorPage = PanelLogic::ActorPage;
    const std::vector<std::string> all{"all"};
    CHECK(PanelLogic::ReadAdvancedSections(all).size() == 4);
    CHECK((PanelLogic::WriteAdvancedSections(PanelLogic::ReadAdvancedSections(all)) ==
           std::vector<std::string>{"look", "movement", "clip", "event"}));
    const std::vector<std::string> unknown{"keys", "look", "animation", ""};
    CHECK((PanelLogic::ReadAdvancedSections(unknown) == std::set<ActorPage>{ActorPage::Look}));
}

TEST("pages belong to their tabs")
{
    CHECK(PanelLogic::GetTab(Page::Moments) == Tab::Demo);
    CHECK(PanelLogic::GetTab(Page::Browse) == Tab::Demo);
    CHECK(PanelLogic::GetTab(Page::Actors) == Tab::Scene);
    CHECK(PanelLogic::GetTab(Page::Players) == Tab::Scene);
    CHECK(PanelLogic::GetTab(Page::You) == Tab::Scene);
}

TEST("the Scene tab shows Bots and You in a game you host, Actors and Players in a demo")
{
    CHECK(PanelLogic::GetShownPage(Page::You, true) == Page::You);
    CHECK(PanelLogic::GetShownPage(Page::Actors, true) == Page::Actors);
    CHECK(PanelLogic::GetShownPage(Page::Players, true) == Page::Actors);
    CHECK(PanelLogic::GetShownPage(Page::You, false) == Page::Actors);
    CHECK(PanelLogic::GetShownPage(Page::Players, false) == Page::Players);
    CHECK(PanelLogic::GetShownPage(Page::Moments, true) == Page::Moments);
    CHECK(PanelLogic::GetShownPage(Page::None, true) == Page::None);
}

TEST("the path over a tab joins what it shows, leaving out empty parts")
{
    const std::vector<std::string> clip = {"Reznov", "Clip", "ai_walk_f"};
    CHECK(PanelLogic::JoinPath(clip) == "Reznov > Clip > ai_walk_f");
    const std::vector<std::string> node = {"Campath", "", "Node 3"};
    CHECK(PanelLogic::JoinPath(node) == "Campath > Node 3");
    CHECK(PanelLogic::JoinPath(std::vector<std::string>{}).empty());
}

TEST("the panel follows a pick once, so another tab can be opened while it stays picked")
{
    PanelLogic::Follower follower;
    const auto camera = Place{Tab::Camera};
    CHECK(follower.Update(7, camera) == camera);
    // the same pick, frame after frame, doesn't pull the panel back
    CHECK(!follower.Update(7, camera));
    CHECK(!follower.Update(7, camera));
    // another pick does
    const auto scene = Place{Tab::Scene, Page::Actors};
    CHECK(follower.Update(9, scene) == scene);
    // nothing picked goes nowhere, and picking the first again follows it again
    CHECK(!follower.Update(0, std::nullopt));
    CHECK(follower.Update(7, camera) == camera);
    // a pick with no place (a marker) goes nowhere
    CHECK(!follower.Update(11, std::nullopt));
}

TEST("the first-run steps tick off as they are done, in any order, and stay ticked")
{
    using namespace GettingStartedLogic;
    State state;
    CHECK(GetNext(state) == Step::LoadDemo);
    CHECK(CountDone(state) == 0);

    // nothing counts outside a demo
    state = Update(state, {.inDemo = false, .jumpedToKill = true, .nodes = 3, .recording = true});
    CHECK(CountDone(state) == 0);

    state = Update(state, {.inDemo = true});
    CHECK(state.done[0]);
    CHECK(GetNext(state) == Step::JumpToKill);
    // a campath of one node isn't a path yet; two are
    state = Update(state, {.inDemo = true, .nodes = 1});
    CHECK(!state.done[2]);
    state = Update(state, {.inDemo = true, .nodes = 2});
    CHECK(state.done[2]);
    CHECK(GetNext(state) == Step::JumpToKill);
    // leaving the demo and deleting the nodes leaves them ticked
    state = Update(state, {.inDemo = false, .nodes = 0});
    CHECK(CountDone(state) == 2);

    state = Update(state, {.inDemo = true, .recording = true});
    state = Update(state, {.inDemo = true, .jumpedToKill = true});
    CHECK(CountDone(state) == 4);
    CHECK(!GetNext(state));
}

TEST("the first-run steps done are kept by name")
{
    using namespace GettingStartedLogic;
    State state;
    state.done[0] = state.done[3] = true;
    const auto names = GetDoneNames(state);
    CHECK(names == (std::vector<std::string>{"loadDemo", "record"}));
    // read back, unknown names left out, the rest as it was
    const std::vector<std::string> read = {"record", "flyAround", "loadDemo"};
    CHECK(WithDoneNames(State{}, read) == state);
    State hidden;
    hidden.hidden = true;
    CHECK(WithDoneNames(hidden, read).hidden);
}
