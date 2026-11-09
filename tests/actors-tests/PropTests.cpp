#include "StdInclude.hpp"
#include "Test.hpp"

#include "Components/ActorLogic.hpp"
#include "Components/PropLogic.hpp"
#include "UI/TimelineGroups.hpp"

using namespace IWXMVM;
using namespace IWXMVM::Components;

// Props in the scene: their place's keys and the browser's list (DD1)

namespace
{
    bool Near(glm::vec3 a, glm::vec3 b, float tolerance = 0.01f)
    {
        return glm::length(a - b) <= tolerance;
    }

    Types::Actor MakeChair()
    {
        auto actor = ActorLogic::MakeProp("p_rus_chair_metal", {100.0f, 200.0f, 0.0f}, 30.0f, 1000, 9000);
        actor.id = 3;
        actor.placedProp->id = 5;
        return actor;
    }

    Types::Keyframe MakeKey(std::uint32_t tick, glm::vec3 value)
    {
        const Types::KeyframeableProperty property(Types::KeyframeablePropertyType::SurfaceDepth, "test",
                                                   Types::KeyframeValueType::Vector3, 0.0f, 1.0f);
        auto key = Types::Keyframe(property, tick, Types::KeyframeValue(value));
        key.interpolation = Types::KeyInterpolation::Linear;
        return key;
    }
}  // namespace

TEST("a prop's keys are filed under its own id, which a property's id gives back")
{
    CHECK(PropLogic::GetPropertyId(5, PropLogic::Channel::Position) == "prop/5/position");
    CHECK(PropLogic::GetPropertyId(12, PropLogic::Channel::Turn) == "prop/12/turn");
    CHECK(PropLogic::ParseProp("prop/12/turn") == std::optional<std::uint32_t>(12));
    CHECK(!PropLogic::ParseProp("light/12/aim"));
    CHECK(!PropLogic::ParseProp("prop/x/turn"));
    CHECK(!PropLogic::ParseProp("prop/12"));
    const auto turn = PropLogic::Describe(5, PropLogic::Channel::Turn);
    CHECK(turn.valueType == Types::KeyframeValueType::Vector3);
    CHECK(turn.valueNames[2] == "Roll");
}

TEST("a prop's position and turn are its actor's place, and a new prop's id is none of the others'")
{
    auto chair = MakeChair();
    CHECK(Near(PropLogic::GetValue(chair, PropLogic::Channel::Position).vector3, {100.0f, 200.0f, 0.0f}));
    PropLogic::SetValue(chair, PropLogic::Channel::Turn, Types::KeyframeValue(glm::vec3(10.0f, 45.0f, -5.0f)));
    CHECK(chair.placedProp->pitch == 10.0f && chair.yaw == 45.0f && chair.placedProp->roll == -5.0f);
    CHECK(Near(PropLogic::GetValue(chair, PropLogic::Channel::Turn).vector3, {10.0f, 45.0f, -5.0f}));

    std::vector<Types::Actor> scene{chair};
    CHECK(PropLogic::MakeId(scene) == 6);
    // (keys of a prop not in the scene keep their id)
    CHECK(PropLogic::MakeId(scene, {9}) == 10);
    CHECK(PropLogic::MakeId({}) == 1);
}

TEST("a keyed prop goes between its keys, and its turn the short way")
{
    const auto chair = MakeChair();
    PropLogic::Keys keys;
    keys[0] = {MakeKey(1000, {0.0f, 0.0f, 0.0f}), MakeKey(3000, {200.0f, 0.0f, 50.0f})};
    keys[1] = {MakeKey(1000, {0.0f, 170.0f, 0.0f}), MakeKey(3000, {0.0f, -170.0f, 20.0f})};
    const auto halfway = PropLogic::ApplyKeys(chair, keys, 2000.0);
    CHECK(Near(halfway.position, {100.0f, 0.0f, 25.0f}));
    // 170 to -170 is 20 degrees through 180, not 340 back through 0
    CHECK(std::abs(std::remainder(halfway.yaw - 180.0f, 360.0f)) < 0.01f);
    CHECK(std::abs(halfway.placedProp->roll - 10.0f) < 0.01f);
    // (before its first key and after its last, as they are)
    CHECK(Near(PropLogic::ApplyKeys(chair, keys, 0.0).position, {0.0f, 0.0f, 0.0f}));
    CHECK(Near(PropLogic::ApplyKeys(chair, keys, 9000.0).position, {200.0f, 0.0f, 50.0f}));
    // (without keys, where it was put)
    CHECK(Near(PropLogic::ApplyKeys(chair, {}, 2000.0).position, chair.position));
}

TEST("a turn keyed at a tick takes the angles nearest the key before it")
{
    const std::vector keys{MakeKey(1000, {0.0f, 170.0f, 0.0f}), MakeKey(5000, {0.0f, 90.0f, 0.0f})};
    CHECK(Near(PropLogic::GetTurnForKey(keys, 2000, {0.0f, -170.0f, 0.0f}), {0.0f, 190.0f, 0.0f}));
    // (with none before, the one after)
    CHECK(Near(PropLogic::GetTurnForKey(keys, 500, {0.0f, -170.0f, 0.0f}), {0.0f, 190.0f, 0.0f}));
    // (with none, its own between -180 and 180)
    CHECK(Near(PropLogic::GetTurnForKey({}, 500, {0.0f, 370.0f, -200.0f}), {0.0f, 10.0f, 160.0f}));
}

TEST("the browser leaves out view models, arms and character parts, and groups the rest by their names")
{
    CHECK(PropLogic::IsListed("p_rus_chair_metal"));
    CHECK(!PropLogic::IsListed("viewmodel_m16"));
    CHECK(!PropLogic::IsListed("t5_weapon_ak47_viewmodel"));
    CHECK(!PropLogic::IsListed("viewhands_russian_spetsnaz"));
    CHECK(!PropLogic::IsListed("c_usa_mp_seal6_body"));
    CHECK(!PropLogic::IsListed("4a8f0c21"));
    CHECK(PropLogic::GetKind("p_rus_chair_metal") == PropLogic::Group::Props);
    CHECK(PropLogic::GetKind("t5_veh_military_police_car") == PropLogic::Group::Vehicles);
    CHECK(PropLogic::GetKind("foliage_red_pine_lg") == PropLogic::Group::Foliage);
    CHECK(PropLogic::GetKind("dest_nuked_mannequin") == PropLogic::Group::Destructibles);
    CHECK(PropLogic::GetKind("t5_weapon_ak47_world") == PropLogic::Group::Weapons);
    CHECK(PropLogic::GetKind("mp_supplydrop_ally") == PropLogic::Group::GameModes);
    CHECK(PropLogic::GetKind("tag_origin") == PropLogic::Group::Other);
}

TEST("the browser shows the map's own first, a group by kind, or every group with a search")
{
    const std::vector<std::string> models{
        "c_usa_mp_seal6_body", "dest_nuked_mannequin",       "mp_supplydrop_ally",      "p_glo_chair_metal",
        "p_rus_chair_metal",   "t5_veh_military_police_car", "t5_weapon_ak47_viewmodel"};
    const std::vector<std::string> map{"dest_nuked_mannequin", "p_rus_chair_metal", "t5_veh_military_police_car"};
    using PropLogic::Group;
    CHECK(PropLogic::GetShown(models, map, Group::Map, "").size() == 3);
    CHECK(PropLogic::GetShown(models, map, Group::Props, "").size() == 2);
    const auto chairs = PropLogic::GetShown(models, map, Group::Vehicles, "chair metal");
    CHECK(chairs.size() == 2 && chairs[0] == "p_glo_chair_metal");
    // (left out whatever the search)
    CHECK(PropLogic::GetShown(models, map, Group::Map, "viewmodel").empty());
    const auto counts = PropLogic::CountGroups(models, map);
    CHECK(counts[static_cast<std::size_t>(Group::Map)] == 3);
    CHECK(counts[static_cast<std::size_t>(Group::Props)] == 2);
    CHECK(counts[static_cast<std::size_t>(Group::GameModes)] == 1);
    CHECK(counts[static_cast<std::size_t>(Group::Weapons)] == 0);
}

TEST("a prop has a bar, saves its id with the scene, and has a row of its own type on the timeline")
{
    auto chair = MakeChair();
    chair.placedProp->scale = 2.5f;
    const auto bar = ActorLogic::GetBar(chair);
    CHECK(bar && bar->first == 1000 && bar->second == 9000);
    ActorLogic::Scene scene;
    scene.actors.push_back(chair);
    const auto back = ActorLogic::SceneFromJson(ActorLogic::SceneToJson(scene, "demo"), 20000);
    CHECK(back.actors.size() == 1 && back.actors[0].placedProp);
    CHECK(back.actors[0].placedProp->id == 5);
    CHECK(back.actors[0].placedProp->scale == 2.5f);
    CHECK(UI::TimelineGroups::GetGroup(chair) == UI::TimelineGroups::Group::Props);
}
