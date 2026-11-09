#include "StdInclude.hpp"
#include "Test.hpp"

#include "UI/TimelineGroups.hpp"

using namespace IWXMVM;
using namespace IWXMVM::UI;
using TimelineGroups::Group;

namespace
{
    Types::Actor MakeActor(std::uint32_t id, std::string_view name = "")
    {
        Types::Actor actor;
        actor.id = id;
        actor.name = name;
        return actor;
    }

    Types::Actor MakeEffect(std::uint32_t id)
    {
        auto actor = MakeActor(id);
        actor.effect = Types::ActorEffect{};
        actor.effect->start = 1000;
        actor.effect->end = 2000;
        return actor;
    }

    Types::Actor MakeLight(std::uint32_t id)
    {
        auto actor = MakeActor(id);
        actor.light = Types::ActorLight{};
        actor.light->start = 3000;
        actor.light->end = 4000;
        return actor;
    }

    Types::Actor MakeKillstreak(std::uint32_t id)
    {
        auto actor = MakeActor(id);
        actor.models = {"vehicle_cobra_helicopter_mp_light"};
        return actor;
    }

    std::vector<std::uint32_t> Ids(const std::vector<const Types::Actor*>& rows)
    {
        std::vector<std::uint32_t> ids;
        for (const auto* row : rows)
            ids.push_back(row->id);
        return ids;
    }

    bool SameOrder(const std::vector<std::uint32_t>& order, std::initializer_list<std::uint32_t> expected)
    {
        return std::ranges::equal(order, expected);
    }
}  // namespace

TEST("each row of the timeline goes under its type")
{
    CHECK(TimelineGroups::GetGroup(MakeActor(1)) == Group::Actors);
    CHECK(TimelineGroups::GetGroup(MakeKillstreak(2)) == Group::Killstreaks);
    CHECK(TimelineGroups::GetGroup(MakeEffect(3)) == Group::Effects);
    CHECK(TimelineGroups::GetGroup(MakeLight(4)) == Group::Lights);
    // (a dog is an actor)
    auto dog = MakeActor(5);
    dog.models = {"german_shepherd"};
    CHECK(TimelineGroups::GetGroup(dog) == Group::Killstreaks);
}

TEST("a group's rows keep the order they were placed in, whatever came between")
{
    // a scene: a helicopter first, then people, then planes
    const std::vector actors{MakeKillstreak(1), MakeActor(2), MakeEffect(3), MakeActor(4), MakeKillstreak(5), MakeLight(6)};
    CHECK(SameOrder(Ids(TimelineGroups::GetRows(actors, Group::Actors)), {2, 4}));
    CHECK(SameOrder(Ids(TimelineGroups::GetRows(actors, Group::Killstreaks)), {1, 5}));
    CHECK(SameOrder(Ids(TimelineGroups::GetRows(actors, Group::Effects)), {3}));
    CHECK(SameOrder(Ids(TimelineGroups::GetRows(actors, Group::Lights)), {6}));
    // (an empty group has no rows, and doesn't show)
    const std::vector people{MakeActor(1)};
    CHECK(TimelineGroups::GetRows(people, Group::Lights).empty());
}

TEST("a row dragged by its name moves within its group, and nothing else does")
{
    const std::vector actors{MakeActor(1), MakeKillstreak(2), MakeActor(3), MakeEffect(4), MakeActor(5)};
    // the last before the first
    CHECK(SameOrder(TimelineGroups::MoveRow(actors, 5, 1), {5, 1, 2, 3, 4}));
    // the first to the end of its group: after the group's last row
    CHECK(SameOrder(TimelineGroups::MoveRow(actors, 1, std::nullopt), {2, 3, 4, 5, 1}));
    // on itself, or onto a row of another group: where it was, or the end of its own group
    CHECK(SameOrder(TimelineGroups::MoveRow(actors, 3, 3), {1, 2, 3, 4, 5}));
    CHECK(SameOrder(TimelineGroups::MoveRow(actors, 1, 2), {2, 3, 4, 5, 1}));
    // the other groups' rows keep their order among themselves
    const auto order = TimelineGroups::MoveRow(actors, 5, 3);
    CHECK(SameOrder(order, {1, 2, 5, 3, 4}));
    // no such row
    CHECK(SameOrder(TimelineGroups::MoveRow(actors, 9, 1), {1, 2, 3, 4, 5}));
}

TEST("a group's header stays at the top while its rows scroll by")
{
    const std::vector<std::optional<TimelineGroups::Span>> groups{TimelineGroups::Span{100.0f, 300.0f}, std::nullopt,
                                                                  TimelineGroups::Span{300.0f, 500.0f}};
    // all in sight: none
    CHECK(!TimelineGroups::GetPin(groups, 50.0f, 20.0f));
    // the first scrolled past: it stays there
    const auto first = TimelineGroups::GetPin(groups, 150.0f, 20.0f);
    CHECK(first.has_value());
    CHECK(first->group == 0);
    CHECK_NEAR(first->y, 150.0f, 0.01f);
    // its last row going: pushed up by it
    const auto going = TimelineGroups::GetPin(groups, 290.0f, 20.0f);
    CHECK(going.has_value());
    CHECK(going->group == 0);
    CHECK_NEAR(going->y, 280.0f, 0.01f);
    const auto last = TimelineGroups::GetPin(groups, 275.0f, 20.0f);
    CHECK(last->group == 0);
    CHECK_NEAR(last->y, 275.0f, 0.01f);
    // the next group's turn
    const auto next = TimelineGroups::GetPin(groups, 350.0f, 20.0f);
    CHECK(next->group == 2);
}

TEST("a folded group's summary has each row's whole time")
{
    auto walker = MakeActor(1);
    walker.clips.push_back({.animation = "a", .start = 5000, .length = 1000});
    walker.clips.push_back({.animation = "b", .start = 2000, .length = 500});
    const auto span = TimelineGroups::GetRowSpan(walker);
    CHECK(span.has_value());
    CHECK(span->first == 2000);
    CHECK(span->second == 6000);
    const auto bar = TimelineGroups::GetRowSpan(MakeLight(2));
    CHECK(bar.has_value());
    CHECK(bar->first == 3000);
    CHECK(bar->second == 4000);
    CHECK(!TimelineGroups::GetRowSpan(MakeActor(3)));
}

TEST("a dog's row goes with the killstreaks, as the Add menus have it")
{
    auto shepherd = MakeActor(1);
    shepherd.models = {"german_shepherd"};
    auto campaign = MakeActor(2);
    campaign.models = {"german_sheperd_dog"};
    auto hellhound = MakeActor(3);
    hellhound.models = {"zombie_wolf"};
    auto person = MakeActor(4);
    person.models = {"c_usa_cia_mp_body_standard", "c_usa_cia_mp_head_4"};
    CHECK(TimelineGroups::GetGroup(shepherd) == Group::Killstreaks);
    CHECK(TimelineGroups::GetGroup(campaign) == Group::Killstreaks);
    CHECK(TimelineGroups::GetGroup(hellhound) == Group::Killstreaks);
    CHECK(TimelineGroups::GetGroup(person) == Group::Actors);
}
