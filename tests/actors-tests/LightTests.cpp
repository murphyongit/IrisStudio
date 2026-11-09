#include "StdInclude.hpp"
#include "Test.hpp"

#include "Components/ActorLogic.hpp"
#include "Components/LightLogic.hpp"

using namespace IWXMVM;
using namespace IWXMVM::Components;

// Lights in the scene: placing them, their bar, keys, flicker and what they follow

namespace
{
    bool Near(float a, float b, float tolerance = 0.001f)
    {
        return std::abs(a - b) <= tolerance;
    }

    bool Near(glm::vec3 a, glm::vec3 b, float tolerance = 0.001f)
    {
        return glm::length(a - b) <= tolerance;
    }

    Types::Actor MakePoint(std::uint32_t start = 1000, std::uint32_t end = 5000)
    {
        Types::Actor actor;
        actor.id = 7;
        actor.light = Types::ActorLight{.start = start, .end = end};
        return actor;
    }

    Types::Keyframe MakeKey(std::uint32_t tick, Types::KeyframeValue value, Types::KeyframeValueType type)
    {
        const Types::KeyframeableProperty property(Types::KeyframeablePropertyType::SurfaceDepth, "test", type, 0.0f,
                                                   1.0f);
        return Types::Keyframe(property, tick, value);
    }
}  // namespace

TEST("a point lands off the surface it is put on, a spot at the camera aimed at the point")
{
    const glm::vec3 wall(100.0f, 0.0f, 0.0f), out(-1.0f, 0.0f, 0.0f), camera(0.0f, 0.0f, 60.0f);
    auto point = LightLogic::MakeLight(Types::LightKind::Point, wall, out, camera, 2000);
    CHECK(Near(point.position, glm::vec3(100.0f - LightLogic::POINT_OFF_SURFACE, 0.0f, 0.0f)));
    CHECK(point.light->start == 2000);
    CHECK(point.light->end == 2000 + LightLogic::LIGHT_LENGTH);

    auto spot = LightLogic::MakeLight(Types::LightKind::Spot, wall, out, camera, 2000);
    CHECK(spot.position == camera);
    const auto aim = LightLogic::Place(spot).direction;
    CHECK(Near(aim, glm::normalize(wall - camera)));
    // (it reaches what was clicked)
    spot.light->radius = 10.0f;
    LightLogic::PlaceOnSurface(spot, wall, out, camera);
    CHECK(spot.light->radius >= glm::length(wall - camera));
}

TEST("an effect's or a light's bar moves as long as it was, and other actors have none")
{
    Types::Actor light;
    light.light = Types::ActorLight{};
    light.light->start = 20000;
    light.light->end = 24000;
    const auto bar = ActorLogic::GetBar(light);
    CHECK(bar && bar->first == 20000 && bar->second == 24000);
    ActorLogic::MoveBar(light, 31000);
    CHECK(light.light->start == 31000);
    CHECK(light.light->end == 35000);

    Types::Actor effect;
    effect.effect = Types::ActorEffect{};
    effect.effect->start = 1000;
    effect.effect->end = 1500;
    ActorLogic::MoveBar(effect, 0);
    const auto moved = ActorLogic::GetBar(effect);
    CHECK(moved && moved->first == 0 && moved->second == 500);

    // (an actor that is neither has no bar to move)
    Types::Actor person;
    CHECK(!ActorLogic::GetBar(person));
    ActorLogic::MoveBar(person, 5000);
    CHECK(!ActorLogic::GetBar(person));
}

TEST("a light's values stay in their ranges and its bar a tenth of a second at least")
{
    Types::ActorLight light;
    light.color = {2.0f, -1.0f, 0.5f};
    light.intensity = 1000.0f;
    light.radius = 0.0f;
    light.cone = 400.0f;
    light.softness = 3.0f;
    light.flickerAmount = 5.0f;
    light.flickerRate = -1.0f;
    light.start = 500;
    light.end = 500;
    LightLogic::Normalize(light);
    CHECK(light.color == glm::vec3(1.0f, 0.0f, 0.5f));
    CHECK(light.intensity == LightLogic::MAX_INTENSITY);
    CHECK(light.radius == LightLogic::MIN_RADIUS);
    CHECK(light.cone == LightLogic::MAX_CONE);
    CHECK(light.softness == 1.0f);
    CHECK(light.flickerAmount == 1.0f);
    CHECK(light.flickerRate == 0.0f);
    CHECK(light.end == 500 + LightLogic::MIN_BAR);
}

TEST("a light is on from its bar's start to its end, and off while hidden or at no intensity")
{
    auto actor = MakePoint();
    CHECK(!LightLogic::GetState(actor, {}, 999));
    CHECK(LightLogic::GetState(actor, {}, 1000));
    CHECK(LightLogic::GetState(actor, {}, 4999));
    CHECK(!LightLogic::GetState(actor, {}, 5000));
    actor.hidden = true;
    CHECK(!LightLogic::GetState(actor, {}, 2000));
    actor.hidden = false;
    actor.light->intensity = 0.0f;
    CHECK(!LightLogic::GetState(actor, {}, 2000));
}

TEST("a light's colour goes to the renderer linear, as the game's own lights do, with its intensity in it")
{
    CHECK(Near(LightLogic::ToLinear(0.0f), 0.0f));
    CHECK(Near(LightLogic::ToLinear(1.0f), 1.0f));
    CHECK(Near(LightLogic::ToLinear(0.5f), 0.214f));
    auto actor = MakePoint();
    actor.light->color = {1.0f, 0.5f, 0.0f};
    actor.light->intensity = 2.0f;
    const auto state = LightLogic::GetState(actor, {}, 2000);
    CHECK(Near(state->color, glm::vec3(2.0f, 0.428f, 0.0f), 0.01f));
}

TEST("keys give a light its colour, intensity, radius and cone at a tick, and the rest stay as set")
{
    auto actor = MakePoint();
    actor.light->intensity = 3.0f;
    actor.light->radius = 250.0f;
    LightLogic::Keys keys;
    using Type = Types::KeyframeValueType;
    auto& intensity = keys[static_cast<std::size_t>(LightLogic::Channel::Intensity)];
    intensity = {MakeKey(1000, 0.0f, Type::FloatingPoint), MakeKey(2000, 4.0f, Type::FloatingPoint)};
    auto& color = keys[static_cast<std::size_t>(LightLogic::Channel::Color)];
    color = {MakeKey(1000, glm::vec3(1.0f, 0.0f, 0.0f), Type::Vector3)};

    const auto before = LightLogic::ApplyKeys(*actor.light, keys, 500.0);
    CHECK(before.intensity == 0.0f);  // the first key's, before it
    const auto after = LightLogic::ApplyKeys(*actor.light, keys, 3000.0);
    CHECK(Near(after.intensity, 4.0f));  // the last key's, after it
    CHECK(after.color == glm::vec3(1.0f, 0.0f, 0.0f));
    CHECK(after.radius == 250.0f);  // not keyed

    // a fade from nothing starts off, and is keyed, never added
    CHECK(!LightLogic::GetState(actor, keys, 1000));
    CHECK(LightLogic::GetState(actor, keys, 1500));
}

TEST("keys move a light and turn its aim at a tick, with the keys' ease, and the rest stay as placed")
{
    auto actor = MakePoint();
    actor.position = {100.0f, 0.0f, 50.0f};
    actor.yaw = 30.0f;
    actor.light->pitch = 20.0f;
    using Type = Types::KeyframeValueType;
    LightLogic::Keys keys;
    auto& position = keys[static_cast<std::size_t>(LightLogic::Channel::Position)];
    position = {MakeKey(1000, glm::vec3(0.0f, 0.0f, 0.0f), Type::Vector3),
                MakeKey(2000, glm::vec3(100.0f, 200.0f, 40.0f), Type::Vector3)};
    position[0].interpolation = Types::KeyInterpolation::Linear;
    position[1].interpolation = Types::KeyInterpolation::Linear;

    const auto middle = LightLogic::ApplyPlacementKeys(actor, keys, 1500.0);
    CHECK(Near(middle.position, glm::vec3(50.0f, 100.0f, 20.0f), 0.01f));
    // its aim isn't keyed: as placed
    CHECK(middle.yaw == 30.0f);
    CHECK(middle.light->pitch == 20.0f);
    // before the first key and after the last, as a value's are
    CHECK(Near(LightLogic::ApplyPlacementKeys(actor, keys, 500.0).position, glm::vec3(0.0f), 0.01f));
    CHECK(Near(LightLogic::ApplyPlacementKeys(actor, keys, 3000.0).position, glm::vec3(100.0f, 200.0f, 40.0f), 0.01f));

    // the aim, and where it points
    auto& aim = keys[static_cast<std::size_t>(LightLogic::Channel::Aim)];
    aim = {MakeKey(1000, glm::vec3(0.0f, 0.0f, 0.0f), Type::Vector3),
           MakeKey(2000, glm::vec3(90.0f, 90.0f, 0.0f), Type::Vector3)};
    aim[0].interpolation = Types::KeyInterpolation::Linear;
    aim[1].interpolation = Types::KeyInterpolation::Linear;
    const auto turned = LightLogic::ApplyPlacementKeys(actor, keys, 1000.0);
    CHECK(Near(LightLogic::Place(turned).direction, glm::vec3(1.0f, 0.0f, 0.0f), 0.01f));
    const auto down = LightLogic::ApplyPlacementKeys(actor, keys, 2000.0);
    CHECK(Near(LightLogic::Place(down).direction, glm::vec3(0.0f, 0.0f, -1.0f), 0.01f));
    // an eased key goes slower at its start than a straight line
    aim[0].interpolation = Types::KeyInterpolation::Smooth;
    aim[1].interpolation = Types::KeyInterpolation::Smooth;
    aim[0].easeOut = 1.0f;
    aim[1].easeIn = 1.0f;
    CHECK(LightLogic::ApplyPlacementKeys(actor, keys, 1100.0).light->pitch < 9.0f);

    // what isn't a light's place stays out of it
    CHECK(LightLogic::ApplyPlacementKeys(actor, keys, 1500.0).light->radius == actor.light->radius);
}

TEST("while a light follows a bone its position's keys are its offset from the bone")
{
    auto actor = MakePoint();
    actor.position = {500.0f, 500.0f, 500.0f};
    actor.light->follow = Types::LightFollow{.bone = "j_head", .offset = {1.0f, 2.0f, 3.0f}};
    using Type = Types::KeyframeValueType;
    LightLogic::Keys keys;
    keys[static_cast<std::size_t>(LightLogic::Channel::Position)] = {
        MakeKey(1000, glm::vec3(10.0f, 0.0f, 0.0f), Type::Vector3)};

    const auto keyed = LightLogic::ApplyPlacementKeys(actor, keys, 1000.0);
    CHECK(keyed.light->follow->offset == glm::vec3(10.0f, 0.0f, 0.0f));
    CHECK(keyed.position == actor.position);
    CHECK(Near(LightLogic::Place(keyed, {100.0f, 0.0f, 0.0f}, glm::mat3(1.0f)).position, glm::vec3(110.0f, 0.0f, 0.0f),
               0.01f));

    // a new key takes the value it has
    CHECK(LightLogic::GetValue(actor, LightLogic::Channel::Position).vector3 == glm::vec3(1.0f, 2.0f, 3.0f));
    actor.light->follow.reset();
    CHECK(LightLogic::GetValue(actor, LightLogic::Channel::Position).vector3 == actor.position);
}

TEST("a light's place keys are properties of its own, kept with its other keys")
{
    CHECK(LightLogic::GetPropertyId(12, LightLogic::Channel::Position) == "light/12/position");
    CHECK(LightLogic::GetPropertyId(12, LightLogic::Channel::Aim) == "light/12/aim");
    CHECK(LightLogic::ParseLight(LightLogic::GetPropertyId(7, LightLogic::Channel::Aim)) == 7);
    const auto position = LightLogic::Describe(12, LightLogic::Channel::Position);
    CHECK(position.valueType == Types::KeyframeValueType::Vector3);
    CHECK(position.min < -60000.0f);
    CHECK(position.valueNames[2] == "Z");
    const auto aim = LightLogic::Describe(12, LightLogic::Channel::Aim);
    CHECK(aim.valueNames[0] == "Pitch");
    CHECK(aim.valueNames[1] == "Yaw");
    CHECK(LightLogic::GetChannelName(LightLogic::Channel::Aim) == "Aim");

    // a value and back
    auto actor = MakePoint();
    actor.yaw = 15.0f;
    actor.light->pitch = -5.0f;
    const auto value = LightLogic::GetValue(actor, LightLogic::Channel::Aim);
    CHECK(value.vector3 == glm::vec3(-5.0f, 15.0f, 0.0f));
    auto other = MakePoint();
    LightLogic::SetValue(other, LightLogic::Channel::Aim, value);
    CHECK(other.yaw == 15.0f);
    CHECK(other.light->pitch == -5.0f);
    // the others go on as a light's own
    LightLogic::SetValue(other, LightLogic::Channel::Radius, 77.0f);
    CHECK(other.light->radius == 77.0f);
}

TEST("a flicker is the same for a tick every time, dips as far as its amount, and differs between lights")
{
    CHECK(LightLogic::GetFlicker(7, 0.0f, 8.0f, 1234.0) == 1.0f);
    CHECK(LightLogic::GetFlicker(7, 0.5f, 0.0f, 1234.0) == 1.0f);
    CHECK(LightLogic::GetFlicker(7, 0.5f, 8.0f, 1234.0) == LightLogic::GetFlicker(7, 0.5f, 8.0f, 1234.0));

    auto least = 1.0f, most = 0.0f, biggestStep = 0.0f;
    auto differs = false;
    auto last = LightLogic::GetFlicker(7, 0.6f, 8.0f, 0.0);
    for (auto tick = 1; tick < 20000; ++tick)
    {
        const auto flicker = LightLogic::GetFlicker(7, 0.6f, 8.0f, tick);
        least = std::min(least, flicker);
        most = std::max(most, flicker);
        biggestStep = std::max(biggestStep, std::abs(flicker - last));
        differs = differs || flicker != LightLogic::GetFlicker(8, 0.6f, 8.0f, tick);
        last = flicker;
    }
    CHECK(least >= 0.4f - 0.001f);
    CHECK(most <= 1.0f);
    CHECK(most - least > 0.3f);
    CHECK(differs);
    // smooth: no jump from one tick to the next
    CHECK(biggestStep < 0.01f);
}

TEST("a spot points by its pitch and the actor's yaw, and what follows a bone goes and turns with it")
{
    CHECK(Near(LightLogic::GetAim(0.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f)));
    CHECK(Near(LightLogic::GetAim(90.0f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f)));
    CHECK(Near(LightLogic::GetAim(0.0f, 90.0f), glm::vec3(0.0f, 1.0f, 0.0f)));

    auto actor = MakePoint();
    actor.light->kind = Types::LightKind::Spot;
    actor.position = {5.0f, 6.0f, 7.0f};
    actor.light->pitch = 0.0f;
    CHECK(LightLogic::Place(actor, {100.0f, 0.0f, 0.0f}, glm::mat3(1.0f)).position == actor.position);

    actor.light->follow = Types::LightFollow{.offset = {10.0f, 0.0f, 0.0f}};
    // a bone turned a quarter left: its forward is y
    const glm::mat3 axes(glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(-1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    const auto placed = LightLogic::Place(actor, {100.0f, 0.0f, 0.0f}, axes);
    CHECK(Near(placed.position, glm::vec3(100.0f, 10.0f, 0.0f)));
    CHECK(Near(placed.direction, glm::vec3(0.0f, 1.0f, 0.0f)));
}

TEST("one spot casts shadows at a time: the first on that wants them, points never")
{
    std::vector<Types::Actor> scene(3, MakePoint());
    scene[0].id = 1;
    scene[1].id = 2;
    scene[2].id = 3;
    scene[1].light->kind = Types::LightKind::Spot;
    scene[2].light->kind = Types::LightKind::Spot;
    scene[0].light->shadows = true;  // a point: nothing
    const auto states = [&](double tick) {
        std::vector<std::optional<LightLogic::State>> list;
        for (const auto& actor : scene)
            list.push_back(LightLogic::GetState(actor, {}, tick));
        return list;
    };
    CHECK(!LightLogic::GetShadowed(scene, states(2000)));
    scene[1].light->shadows = true;
    scene[2].light->shadows = true;
    CHECK(LightLogic::GetShadowed(scene, states(2000)) == 2u);
    // the first's bar over, the other has them
    scene[1].light->end = 1500;
    CHECK(LightLogic::GetShadowed(scene, states(2000)) == 3u);
}

TEST("a spot wanting shadows another has says so; the one that has them, a point and one past its bar don't")
{
    auto first = MakePoint();
    first.id = 1;
    first.light->kind = Types::LightKind::Spot;
    first.light->shadows = true;
    auto second = first;
    second.id = 2;
    auto third = first;
    third.id = 3;
    third.light->shadows = false;
    auto point = first;
    point.id = 4;
    point.light->kind = Types::LightKind::Point;
    const std::vector<Types::Actor> scene = {first, second, third, point};
    CHECK(!LightLogic::LosesShadows(scene, 1, 2000));
    CHECK(LightLogic::LosesShadows(scene, 2, 2000));
    CHECK(!LightLogic::LosesShadows(scene, 3, 2000));
    CHECK(!LightLogic::LosesShadows(scene, 4, 2000));
    CHECK(!LightLogic::LosesShadows(scene, 9, 2000));

    // with the first out of its bar the second has them
    auto later = scene;
    later[0].light->end = 1500;
    CHECK(!LightLogic::LosesShadows(later, 2, 2000));
    CHECK(LightLogic::LosesShadows(scene, 2, 2000));
}

TEST("a spot aimed at a point stays where it is and points there; a point it is on leaves its aim")
{
    auto actor = MakePoint();
    actor.light->kind = Types::LightKind::Spot;
    actor.position = {100.0f, 0.0f, 50.0f};
    LightLogic::AimAt(actor, {100.0f, 100.0f, -50.0f});
    CHECK(Near(actor.position, {100.0f, 0.0f, 50.0f}));
    CHECK(Near(LightLogic::GetAim(actor.light->pitch, actor.yaw), glm::normalize(glm::vec3(0.0f, 1.0f, -1.0f))));

    const auto pitch = actor.light->pitch;
    const auto yaw = actor.yaw;
    LightLogic::AimAt(actor, actor.position);
    CHECK(Near(actor.light->pitch, pitch));
    CHECK(Near(actor.yaw, yaw));
}

TEST("a light's bar and flicker go by the actors' tick, its keys by the timeline's")
{
    auto actor = MakePoint(1000, 5000);
    LightLogic::Keys keys;
    keys[static_cast<std::size_t>(LightLogic::Channel::Radius)] = {
        MakeKey(100, 100.0f, Types::KeyframeValueType::FloatingPoint),
        MakeKey(200, 300.0f, Types::KeyframeValueType::FloatingPoint)};
    const auto state = LightLogic::GetState(actor, keys, 2000, 150);
    CHECK(state.has_value());
    CHECK(Near(state->radius, 200.0f, 0.5f));
    CHECK(!LightLogic::GetState(actor, keys, 500, 150));
    CHECK(LightLogic::GetState(actor, keys, 2000) == LightLogic::GetState(actor, keys, 2000, 2000));
}

TEST("a light's icon is around it, facing the camera, and its reach as wide as it")
{
    const glm::vec3 position{10.0f, 20.0f, 30.0f};
    const auto icon = LightLogic::GetIconLines(position, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, 8.0f);
    CHECK(!icon.empty());
    for (const auto& line : icon)
    {
        CHECK(line.size() >= 2);
        for (const auto& point : line)
        {
            CHECK(Near(point.y, position.y));
            CHECK(glm::length(point - position) <= 8.0f * 1.5f);
        }
    }

    const LightLogic::Placed placed{.position = position, .direction = {0.0f, 0.0f, -1.0f}};
    const auto rings = LightLogic::GetReachLines(Types::LightKind::Point, placed, 100.0f, 60.0f);
    CHECK(rings.size() == 3);
    for (const auto& ring : rings)
        for (const auto& point : ring)
            CHECK(Near(glm::length(point - position), 100.0f, 0.01f));

    // a spot's cone ends in a ring at its radius, as wide as its cone
    const auto cone = LightLogic::GetReachLines(Types::LightKind::Spot, placed, 100.0f, 60.0f);
    CHECK(cone.size() > 1);
    bool ring = false;
    for (const auto& line : cone)
        for (const auto& point : line)
        {
            CHECK(point.z >= position.z - 100.0f - 0.01f);
            if (Near(glm::length(point - position), 100.0f, 0.01f) && Near(point.z, position.z - 100.0f * 0.866f, 0.1f))
                ring = true;
        }
    CHECK(ring);
}

TEST("a light's rectangle on the view holds its sphere, the whole view when it reaches behind the camera")
{
    // a camera at the origin looking along -z, 90 degrees across, a 1600 x 900 view from (10, 20)
    const auto projection = glm::perspectiveLH_NO(glm::radians(90.0f), 16.0f / 9.0f, 1.0f, 10000.0f);
    const auto viewProjection =
        projection * glm::lookAtLH(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    const LightLogic::ScreenRect view{10, 20, 1600, 900};

    // straight ahead, 100 away, 10 wide: around the middle, and not the whole view
    auto rect = LightLogic::GetScissor({0.0f, 0.0f, 100.0f}, 10.0f, viewProjection, view);
    CHECK(rect.x < 810 && rect.x + rect.width > 810);
    CHECK(rect.y < 470 && rect.y + rect.height > 470);
    CHECK(rect.width < 200);
    CHECK(rect.height < 200);

    // up and to the right of the middle: y goes down in pixels
    rect = LightLogic::GetScissor({40.0f, 30.0f, 100.0f}, 10.0f, viewProjection, view);
    CHECK(rect.x > 810);
    CHECK(rect.y + rect.height < 470);

    // the camera inside it, and one behind the camera that reaches past it
    CHECK(LightLogic::GetScissor({0.0f, 0.0f, 5.0f}, 10.0f, viewProjection, view) == view);
    CHECK(LightLogic::GetScissor({0.0f, 0.0f, -5.0f}, 10.0f, viewProjection, view) == view);

    // off to the side: nothing to draw
    rect = LightLogic::GetScissor({1000.0f, 0.0f, 100.0f}, 10.0f, viewProjection, view);
    CHECK(rect.width == 0);

    // a corner past the view's edge stays on the view
    rect = LightLogic::GetScissor({150.0f, 0.0f, 100.0f}, 40.0f, viewProjection, view);
    CHECK(rect.x + rect.width == 1610);
}

TEST("a new light gets an id no light of the scene has")
{
    std::vector<Types::Actor> scene(3, MakePoint());
    CHECK(LightLogic::MakeId({}) == 1);
    scene[0].light->id = 4;
    scene[1].light.reset();
    scene[2].light->id = 2;
    CHECK(LightLogic::MakeId(scene) == 5);

    // nor have keys in the demo's file, of a scene not loaded, which it would take on
    CHECK(LightLogic::MakeId({}, {3}) == 4);
    CHECK(LightLogic::MakeId(scene, {2, 9}) == 10);
    CHECK(LightLogic::MakeId(scene, {1}) == 5);

    CHECK(LightLogic::ParseLight("light/12/intensity") == 12);
    CHECK(LightLogic::ParseLight(LightLogic::GetPropertyId(7, LightLogic::Channel::Cone)) == 7);
    CHECK(!LightLogic::ParseLight("light/12"));
    CHECK(!LightLogic::ParseLight("light//cone"));
    CHECK(!LightLogic::ParseLight("qUINT_ssr.fx/SSR_REFLECTION_INTENSITY"));
}

TEST("a light's property ids stay with the light, and a light is saved with the scene")
{
    CHECK(LightLogic::GetPropertyId(12, LightLogic::Channel::Intensity) == "light/12/intensity");
    CHECK(LightLogic::Describe(12, LightLogic::Channel::Color).valueType == Types::KeyframeValueType::Vector3);
    CHECK(LightLogic::Describe(12, LightLogic::Channel::Cone).max == LightLogic::MAX_CONE);

    auto actor = MakePoint();
    actor.name = "Spot 1";
    actor.light->id = 9;
    actor.position = {1.0f, 2.0f, 3.0f};
    actor.light->kind = Types::LightKind::Spot;
    actor.light->color = {0.25f, 0.5f, 0.75f};
    actor.light->shadows = true;
    actor.light->flickerAmount = 0.5f;
    actor.light->highlights = 2.5f;
    actor.light->follow = Types::LightFollow{.target = {.kind = Types::BoneCameraTargetKind::Actor, .id = 4},
                                             .bone = "j_wrist_ri",
                                             .offset = {1, 2, 3},
                                             .firstPerson = Types::LightFirstPerson::ViewGun,
                                             .steady = 0.2f};
    ActorLogic::Scene scene;
    scene.actors = {actor};
    const auto read = ActorLogic::SceneFromJson(ActorLogic::SceneToJson(scene, "test"), 100000).actors;
    CHECK(read.size() == 1);
    CHECK(read[0].light == actor.light);
    CHECK(!read[0].effect);
}

TEST("a light's aim turns the short way between keys, and a key takes the yaw nearest the key before it")
{
    auto actor = MakePoint();
    actor.light->kind = Types::LightKind::Spot;
    using Type = Types::KeyframeValueType;
    LightLogic::Keys keys;
    auto& aim = keys[static_cast<std::size_t>(LightLogic::Channel::Aim)];
    aim = {MakeKey(1000, glm::vec3(0.0f, 170.0f, 0.0f), Type::Vector3),
           MakeKey(2000, glm::vec3(0.0f, -170.0f, 0.0f), Type::Vector3)};
    aim[0].interpolation = Types::KeyInterpolation::Linear;
    aim[1].interpolation = Types::KeyInterpolation::Linear;
    // 170 to -170 is 20 degrees across 180, not 340 across 0
    const auto middle = LightLogic::ApplyPlacementKeys(actor, keys, 1500.0);
    CHECK(Near(std::remainder(middle.yaw, 360.0f), 180.0f, 0.01f) ||
          Near(std::remainder(middle.yaw, 360.0f), -180.0f, 0.01f));
    CHECK(Near(LightLogic::Place(middle).direction, glm::vec3(-1.0f, 0.0f, 0.0f), 0.01f));
    const auto quarter = LightLogic::ApplyPlacementKeys(actor, keys, 1250.0);
    CHECK(Near(quarter.yaw, 175.0f, 0.01f));
    // at the keys, where they point
    CHECK(Near(LightLogic::Place(LightLogic::ApplyPlacementKeys(actor, keys, 2000.0)).direction,
               LightLogic::GetAim(0.0f, -170.0f), 0.001f));
    // a third key goes on from where the second turned to
    aim.push_back(MakeKey(3000, glm::vec3(0.0f, 100.0f, 0.0f), Type::Vector3));
    aim[2].interpolation = Types::KeyInterpolation::Linear;
    CHECK(Near(LightLogic::ApplyPlacementKeys(actor, keys, 2500.0).yaw, 145.0f, 0.01f));
    // the pitch isn't turned
    aim[1].value.vector3.x = 40.0f;
    CHECK(Near(LightLogic::ApplyPlacementKeys(actor, keys, 2000.0).light->pitch, 40.0f, 0.01f));

    CHECK(Near(LightLogic::NearestYaw(-170.0f, 170.0f), 190.0f));
    CHECK(Near(LightLogic::NearestYaw(10.0f, 350.0f), 370.0f));
    CHECK(Near(LightLogic::NearestYaw(90.0f, 80.0f), 90.0f));
    // a key added between two takes the yaw nearest the one before it, one after the last the last's
    const std::vector two = {aim[0], aim[1]};
    CHECK(Near(LightLogic::GetAimForKey(two, 1500, glm::vec3(0.0f, -175.0f, 0.0f)).y, 185.0f));
    CHECK(Near(LightLogic::GetAimForKey(two, 2500, glm::vec3(0.0f, 170.0f, 0.0f)).y, 170.0f));
    CHECK(Near(LightLogic::GetAimForKey(two, 2500, glm::vec3(0.0f, -160.0f, 0.0f)).y, 200.0f));
    // one before the first takes the yaw nearest the first, the key there changing takes the one before it
    CHECK(Near(LightLogic::GetAimForKey(two, 500, glm::vec3(0.0f, -175.0f, 0.0f)).y, 185.0f));
    CHECK(Near(LightLogic::GetAimForKey(two, 2000, glm::vec3(10.0f, -150.0f, 0.0f)).y, 210.0f));
    CHECK(Near(LightLogic::GetAimForKey(two, 2000, glm::vec3(10.0f, -150.0f, 0.0f)).x, 10.0f));
    // with no keys it's the aim's own
    CHECK(Near(LightLogic::GetAimForKey({}, 2000, glm::vec3(0.0f, 270.0f, 0.0f)).y, -90.0f));
}

TEST("the selected light's path runs through its position's keys as it goes along them")
{
    using Type = Types::KeyframeValueType;
    CHECK(LightLogic::GetPathLine({}).empty());
    CHECK(LightLogic::GetPathLine({MakeKey(1000, glm::vec3(1.0f), Type::Vector3)}).empty());

    std::vector keys = {MakeKey(1000, glm::vec3(0.0f, 0.0f, 0.0f), Type::Vector3),
                        MakeKey(2000, glm::vec3(400.0f, 0.0f, 0.0f), Type::Vector3),
                        MakeKey(3000, glm::vec3(400.0f, 400.0f, 0.0f), Type::Vector3)};
    for (auto& key : keys)
        key.interpolation = Types::KeyInterpolation::Linear;
    const auto line = LightLogic::GetPathLine(keys);
    // from the first key to the last, through the one between, a point every 20 units
    CHECK(Near(line.front(), glm::vec3(0.0f)));
    CHECK(Near(line.back(), glm::vec3(400.0f, 400.0f, 0.0f)));
    CHECK(std::ranges::any_of(line, [](glm::vec3 point) { return Near(point, glm::vec3(400.0f, 0.0f, 0.0f)); }));
    CHECK(line.size() == 41);
    for (std::size_t i = 1; i < line.size(); ++i)
        CHECK(glm::distance(line[i - 1], line[i]) <= 20.01f);
    // keys close together still get a few points, and an eased one bends: its points aren't evenly spaced
    std::vector close = {MakeKey(1000, glm::vec3(0.0f), Type::Vector3),
                         MakeKey(2000, glm::vec3(10.0f, 0.0f, 0.0f), Type::Vector3)};
    close[0].easeOut = 1.0f;
    close[1].easeIn = 1.0f;
    const auto eased = LightLogic::GetPathLine(close);
    CHECK(eased.size() == 9);
    CHECK(glm::distance(eased[0], eased[1]) < glm::distance(eased[3], eased[4]));
}

TEST("in first person a light on a gun's bone goes by the gun on screen, Body never, View gun its arms too")
{
    using Mode = Types::LightFirstPerson;
    const std::vector<std::string> arms = {"tag_view", "tag_weapon", "j_wrist_ri", "j_wrist_le"};
    const std::vector<std::string> gun = {"j_gun", "tag_flash", "tag_brass", "tag_clip"};
    // Auto: the gun's bones, the hand's gun tag as the gun's root, the rest the body's
    CHECK(LightLogic::GetViewGunBone(Mode::Auto, "tag_flash", arms, gun) == "tag_flash");
    CHECK(LightLogic::GetViewGunBone(Mode::Auto, "tag_weapon_right", arms, gun) == "j_gun");
    CHECK(!LightLogic::GetViewGunBone(Mode::Auto, "j_wrist_ri", arms, gun));
    CHECK(!LightLogic::GetViewGunBone(Mode::Auto, "j_head", arms, gun));
    // Body: never
    CHECK(!LightLogic::GetViewGunBone(Mode::Body, "tag_flash", arms, gun));
    // View gun: any it has, the body's for the rest
    CHECK(LightLogic::GetViewGunBone(Mode::ViewGun, "j_wrist_ri", arms, gun) == "j_wrist_ri");
    CHECK(LightLogic::GetViewGunBone(Mode::ViewGun, "tag_flash", arms, gun) == "tag_flash");
    CHECK(!LightLogic::GetViewGunBone(Mode::ViewGun, "j_head", arms, gun));
    // no gun on screen: the body's
    CHECK(!LightLogic::GetViewGunBone(Mode::Auto, "tag_flash", {}, {}));
}
