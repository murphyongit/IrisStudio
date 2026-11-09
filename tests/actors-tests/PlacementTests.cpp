#include "StdInclude.hpp"
#include "Test.hpp"

#include "Components/ActorLogic.hpp"

using namespace IWXMVM;

// Where actors stand: the root motion of clips that don't walk (ActorLogic::EvaluatePlacement)
// and paths walked through rounded corners (RoundPath, 2.8), step 4b

namespace
{
    std::shared_ptr<const ActorLogic::RootMotion> Track(const std::function<glm::vec3(float)>& at, int steps = 30)
    {
        auto motion = std::make_shared<ActorLogic::RootMotion>();
        for (int step = 0; step <= steps; ++step)
            motion->samples.push_back(at(static_cast<float>(step) / static_cast<float>(steps)));
        return motion;
    }

    // Animations as the tests know them, with the root's motion through a play (x forward, y left, turn left), each
    // playing a second unless it says: "walk" walks 100 units forward, "death" goes 60 forward (slowly at first) and 12
    // to the right as it falls, "turn" turns a quarter to the left stepping 20 forward, "flinch" stumbles 30 to the
    // left in half a second, "stumble" goes 50 forward and 20 to the left in half a second, "spin" (a loop) goes 10
    // forward turning a quarter to the left each play, "idle" stands, "left" strafes 50 to the left and "back" walks
    // 118 back; "pb_combatrun_forward_loop" is a locomotion run (211)
    ActorLogic::AnimationLookup Animations()
    {
        struct Known
        {
            std::string_view name;
            glm::vec2 motion;
            std::shared_ptr<const ActorLogic::RootMotion> root;
            float duration = 1.0f;
        };
        static const std::vector<Known> known = {
            {"walk", {100, 0}, Track([](float t) { return glm::vec3(100.0f * t, 0.0f, 0.0f); })},
            {"death", {60, -12}, Track([](float t) { return glm::vec3(60.0f * t * t, -12.0f * t, 0.0f); })},
            {"turn", {20, 0}, Track([](float t) { return glm::vec3(20.0f * t, 0.0f, 90.0f * t); })},
            {"flinch", {0, 30}, Track([](float t) { return glm::vec3(0.0f, 30.0f * t, 0.0f); }), 0.5f},
            {"stumble", {50, 20}, Track([](float t) { return glm::vec3(50.0f * t, 20.0f * t, 0.0f); }), 0.5f},
            {"spin", {10, 0}, Track([](float t) { return glm::vec3(10.0f * t, 0.0f, 90.0f * t); })},
            {"idle", {0, 0}, nullptr},
            {"left", {0, 50}, Track([](float t) { return glm::vec3(0.0f, 50.0f * t, 0.0f); })},
            {"back", {-118, 0}, Track([](float t) { return glm::vec3(-118.0f * t, 0.0f, 0.0f); })},
            {"pb_stand_alert", {0, 0}, nullptr, 10.0f},
            {"pb_combatrun_forward_loop", {211, 0}, Track([](float t) { return glm::vec3(211.0f * t, 0.0f, 0.0f); })},
        };
        return [](const std::string& name) -> std::optional<ActorLogic::AnimationInfo> {
            const auto found = std::ranges::find(known, name, &Known::name);
            if (found == known.end())
                return std::nullopt;
            ActorLogic::AnimationInfo info{found->duration, found->motion};
            info.kind = Types::AnimationKind::FullBody;
            info.family = name.starts_with("pb_") ? Types::AnimationFamily::MpPlayer : Types::AnimationFamily::SpHuman;
            info.rootMotion = found->root;
            info.delta = found->root != nullptr;
            return info;
        };
    }

    Types::ActorClip Clip(std::string animation, std::uint32_t start, std::uint32_t length, std::uint32_t blendIn = 0,
                          bool loop = true)
    {
        return {.animation = std::move(animation), .start = start, .length = length, .loop = loop, .blendIn = blendIn};
    }

    // a clip that doesn't walk (as deaths and one-offs played from the view)
    Types::ActorClip Still(std::string animation, std::uint32_t start, std::uint32_t length, std::uint32_t blendIn = 0,
                           bool loop = false)
    {
        auto clip = Clip(std::move(animation), start, length, blendIn, loop);
        clip.moves = false;
        return clip;
    }

    Types::Actor Standing(std::vector<Types::ActorClip> clips, glm::vec3 position = {10, 20, 5}, float yaw = 90.0f)
    {
        Types::Actor actor;
        actor.clips = std::move(clips);
        actor.position = position;
        actor.yaw = yaw;
        ActorLogic::Normalize(actor);
        return actor;
    }

    Types::Actor Walking(std::vector<Types::ActorClip> clips, std::vector<glm::vec3> path)
    {
        auto actor = Standing(std::move(clips));
        actor.movement = Types::ActorMovement::Path;
        actor.path = std::move(path);
        return actor;
    }

    Types::ActorPlacement At(const Types::Actor& actor, std::uint32_t tick)
    {
        return ActorLogic::EvaluatePlacement(actor, tick, Animations());
    }

    double YawOff(float yaw, float expected)
    {
        return std::abs(std::remainder(yaw - expected, 360.0f));
    }

    // the most the actor's placement changes from one millisecond to the next over some ticks
    struct Steps
    {
        float position = 0.0f;
        float yaw = 0.0f;
    };
    Steps GetSteps(const Types::Actor& actor, std::uint32_t from, std::uint32_t to)
    {
        Steps steps;
        auto before = At(actor, from);
        for (auto tick = from + 1; tick <= to; ++tick)
        {
            const auto now = At(actor, tick);
            steps.position = std::max(steps.position, glm::distance(now.position, before.position));
            steps.yaw = std::max(steps.yaw, static_cast<float>(YawOff(now.yaw, before.yaw)));
            before = now;
        }
        return steps;
    }
}  // namespace

TEST("root motion: a death moves the actor by its root's motion, turned the way it faced, and it lies there")
{
    const auto actor = Standing({Clip("idle", 0, 1000), Still("death", 1000, 1000)});
    // facing +y: forward is +y, its right +x
    CHECK(glm::distance(At(actor, 999).position, glm::vec3(10, 20, 5)) < 1e-4f);
    const auto halfway = At(actor, 1500);
    CHECK(glm::distance(halfway.position, glm::vec3(10 + 6, 20 + 15, 5)) < 1e-3f);
    // the whole play's motion where it ends (as the game's delta from 0 to 1 has it), and held there
    for (const std::uint32_t tick : {2000u, 2500u, 60000u})
    {
        const auto fallen = At(actor, tick);
        CHECK(glm::distance(fallen.position, glm::vec3(10 + 12, 20 + 60, 5)) < 1e-3f);
        CHECK_NEAR(YawOff(fallen.yaw, 90.0f), 0.0, 1e-3);
    }
    // the same whichever ticks were asked before (scrubbing)
    CHECK(glm::distance(At(actor, 1500).position, halfway.position) < 1e-6f);

    // the Die starting point's and the view menu's deaths (AddDeath, PlayAt) move it too, blending in over 0.1 s
    // (see below)
    auto died = Standing({Clip("idle", 0, 10000)});
    ActorLogic::AddDeath(died, 2000, "death", 1.0f);
    CHECK(glm::distance(At(died, 5000).position, glm::vec3(21.4f, 79.788f, 5)) < 1e-3f);
    auto played = Standing({Clip("idle", 0, 10000)});
    ActorLogic::PlayAt(played, 2000, "death", 1.0f, false, std::vector<Types::ActorNotetrack>{{"start_ragdoll", 0.9f}});
    CHECK(glm::distance(At(played, 5000).position, glm::vec3(21.4f, 79.788f, 5)) < 1e-3f);
}

TEST("root motion: a clip's motion is weighed as it blends in and out, as the game sums a tree's motion")
{
    // blending in over 0.1 s: the death goes 0.388 of its first 0.6 forward (as its samples have it) and 0.6 of its
    // first 1.2 to the right
    const auto actor = Standing({Clip("idle", 0, 1000), Still("death", 1000, 1000, 100)});
    CHECK(glm::distance(At(actor, 1500).position, glm::vec3(10 + 5.4f, 20 + 14.788f, 5)) < 1e-3f);
    CHECK(glm::distance(At(actor, 3000).position, glm::vec3(10 + 11.4f, 20 + 59.788f, 5)) < 1e-3f);
    // a flinch (half a second, 30 units) blending in over a quarter of a second loses a quarter of its motion
    auto flinching = Standing({Clip("idle", 0, 1000), Still("flinch", 1000, 500, 250), Clip("idle", 1500, 1000, 250)},
                              {0, 0, 0}, 0.0f);
    CHECK(glm::distance(At(flinching, 1250).position, glm::vec3(0, 7.5f, 0)) < 1e-3f);
    CHECK(glm::distance(At(flinching, 2000).position, glm::vec3(0, 22.5f, 0)) < 1e-3f);
    // a walk played once while the clip after it blends in over it: half of what it moves meanwhile
    const auto stopping = Standing({Clip("walk", 0, 600, 0, false), Clip("idle", 600, 1000, 400)}, {0, 0, 0}, 0.0f);
    CHECK(glm::distance(At(stopping, 3000).position, glm::vec3(60 + 20, 0, 0)) < 1e-2f);
    // and on its way from a walk to standing still the actor goes no faster than either
    CHECK(GetSteps(stopping, 0, 2000).position <= 0.1f + 1e-3f);
}

TEST("root motion: a turn adds to the way the actor faces, and what follows goes on from where it turned")
{
    const auto actor = Standing({Still("turn", 0, 1000), Still("death", 1000, 1000)}, {0, 0, 0}, 0.0f);
    CHECK(glm::distance(At(actor, 500).position, glm::vec3(10, 0, 0)) < 1e-3f);
    CHECK_NEAR(YawOff(At(actor, 500).yaw, 45.0f), 0.0, 1e-3);
    CHECK_NEAR(YawOff(At(actor, 1000).yaw, 90.0f), 0.0, 1e-3);
    // the death faces +y as it starts: its 60 forward and 12 right go to +y and +x
    const auto fallen = At(actor, 3000);
    CHECK(glm::distance(fallen.position, glm::vec3(20 + 12, 60, 0)) < 1e-3f);
    CHECK_NEAR(YawOff(fallen.yaw, 90.0f), 0.0, 1e-3);
}

TEST("root motion: a looping clip moves the actor by a play's motion each play")
{
    // a quarter turn and 10 forward a play: round a square and back where it started after four
    const auto spinning = Standing({Still("spin", 0, 10000, 0, true)}, {0, 0, 0}, 0.0f);
    CHECK(glm::distance(At(spinning, 1000).position, glm::vec3(10, 0, 0)) < 1e-3f);
    CHECK(glm::distance(At(spinning, 1500).position, glm::vec3(10, 5, 0)) < 1e-3f);
    CHECK_NEAR(YawOff(At(spinning, 1500).yaw, 135.0f), 0.0, 1e-3);
    CHECK(glm::distance(At(spinning, 2000).position, glm::vec3(10, 10, 0)) < 1e-3f);
    CHECK(glm::distance(At(spinning, 4000).position, glm::vec3(0, 0, 0)) < 1e-3f);
    CHECK_NEAR(YawOff(At(spinning, 4000).yaw, 0.0f), 0.0, 1e-3);
    // standing still (with no path to walk), a looping walk walks in place, as it did; played once, it walks on by
    // its own motion, from where the clip starts in it
    auto walk = Clip("walk", 0, 2500);
    walk.offset = 0.5f;
    auto walker = Standing({walk}, {0, 0, 0}, 0.0f);
    CHECK(ActorLogic::GetRootMotionUse(walker, walker.clips[0], Animations()) == ActorLogic::RootMotionUse::None);
    CHECK(glm::distance(At(walker, 2500).position, glm::vec3(0, 0, 0)) < 1e-6f);
    walker.clips[0].loop = false;
    CHECK(glm::distance(At(walker, 2500).position, glm::vec3(50, 0, 0)) < 1e-2f);
}

TEST("root motion: a clip set to stay in place doesn't move the actor, nor do walks and the locomotion's clips")
{
    auto death = Still("death", 1000, 1000);
    death.rootMotion = false;
    const auto staying = Standing({Clip("idle", 0, 1000), death});
    CHECK(glm::distance(At(staying, 3000).position, glm::vec3(10, 20, 5)) < 1e-6f);
    CHECK(ActorLogic::GetRootMotionUse(staying, staying.clips[1], Animations()) == ActorLogic::RootMotionUse::Stays);
    CHECK(ActorLogic::GetRootMotionUse(staying, staying.clips[0], Animations()) == ActorLogic::RootMotionUse::None);

    // a walk on a path walks it, as before
    auto walker = Walking({Clip("walk", 0, 2000)}, {{0, 0, 0}, {1000, 0, 0}});
    CHECK(ActorLogic::GetRootMotionUse(walker, walker.clips[0], Animations()) == ActorLogic::RootMotionUse::None);
    CHECK_NEAR(At(walker, 1500).position.x, 150.0, 1e-3);
    CHECK_NEAR(At(walker, 1500).position.y, 0.0, 1e-4);
    // with locomotion, a clip of the players' run stands for it, and walks at the actor's speed
    walker.clips = {Clip("pb_combatrun_forward_loop", 0, 2000)};
    walker.locomotion = Types::ActorLocomotion::Auto;
    walker.pathSpeed = 200.0f;
    CHECK(ActorLogic::GetRootMotionUse(walker, walker.clips[0], Animations()) == ActorLogic::RootMotionUse::None);
    CHECK_NEAR(At(walker, 1000).position.x, 200.0, 1e-3);
    // standing still, it stays (its body is the locomotion's idle)
    auto idling = walker;
    idling.movement = Types::ActorMovement::Still;
    CHECK(ActorLogic::GetRootMotionUse(idling, idling.clips[0], Animations()) == ActorLogic::RootMotionUse::None);
    CHECK(glm::distance(At(idling, 1500).position, idling.position) < 1e-6f);
    // attached, the game puts it where the bone is
    auto attached = Standing({Still("death", 0, 1000)});
    attached.movement = Types::ActorMovement::Attached;
    CHECK(ActorLogic::GetRootMotionUse(attached, attached.clips[0], Animations()) == ActorLogic::RootMotionUse::None);
}

TEST("root motion: after a one-off the actor goes on from where it ended")
{
    // a flinch played from the view: the idle resumed after it stands where the flinch left it (it blends in over a
    // quarter of a second, a quarter of its motion lost)
    auto actor = Standing({Clip("idle", 0, 10000)}, {0, 0, 0}, 0.0f);
    ActorLogic::PlayAt(actor, 2000, "flinch", 0.5f, false);
    CHECK(actor.clips.size() == 3);
    CHECK(glm::distance(At(actor, 2250).position, glm::vec3(0, 7.5f, 0)) < 1e-3f);
    CHECK(glm::distance(At(actor, 2500).position, glm::vec3(0, 22.5f, 0)) < 1e-3f);
    CHECK(glm::distance(At(actor, 8000).position, glm::vec3(0, 22.5f, 0)) < 1e-3f);

    // with keys, it goes on from there too, back onto the way its keys go as it moves
    auto keyed = Standing({Clip("idle", 0, 1000), Still("flinch", 1000, 500), Clip("idle", 1500, 10000)});
    keyed.movement = Types::ActorMovement::Keyframes;
    keyed.keys = {{0, {0, 0, 0}, 0.0f},     {1000, {0, 0, 0}, 0.0f},   {2000, {0, 0, 0}, 0.0f},
                  {3000, {0, 0, 0}, 0.0f},  {4000, {100, 0, 0}, 0.0f}, {5000, {200, 0, 0}, 0.0f},
                  {6000, {300, 0, 0}, 0.0f}};
    const auto flinched = At(keyed, 1800);
    CHECK(glm::distance(flinched.position, glm::vec3(0, 30, 0)) < 1e-3f);
    // (once it has gone the way back's length, 75 units, it is on its way again)
    CHECK(glm::distance(At(keyed, 6000).position, glm::vec3(300, 0, 0)) < 1e-3f);
    // (no faster than its keys move it, but for the way back)
    auto unmoved = keyed;
    unmoved.clips[1].rootMotion = false;
    CHECK(GetSteps(keyed, 1000, 6200).position < GetSteps(unmoved, 1000, 6200).position + 0.1f);
}

TEST("root motion: on a path, walking on goes back onto it from where a one-off left the actor, facing the way it goes")
{
    // walks 200 units, flinches 30 to its left, walks on
    const auto actor = Walking({Clip("walk", 0, 2000), Still("flinch", 2000, 500), Clip("walk", 2500, 5000)},
                               {{0, 0, 0}, {2000, 0, 0}});
    CHECK(glm::distance(At(actor, 2000).position, glm::vec3(200, 0, 0)) < 1e-2f);
    CHECK(glm::distance(At(actor, 2500).position, glm::vec3(200, 30, 0)) < 1e-2f);
    // back onto it over 75 units (2.5 times as long as the way back), halfway at 37.5 units, facing the way it goes
    const auto merging = At(actor, 2875);
    CHECK(glm::distance(merging.position, glm::vec3(237.5f, 15, 0)) < 1e-2f);
    CHECK_NEAR(YawOff(merging.yaw, glm::degrees(std::atan2(-30.0f * 1.5f / 75.0f, 1.0f))), 0.0, 1e-2);
    CHECK(glm::distance(At(actor, 3500).position, glm::vec3(300, 0, 0)) < 1e-2f);
    CHECK_NEAR(YawOff(At(actor, 3500).yaw, 0.0f), 0.0, 1e-3);
    // without a jump or a turn on the spot
    const auto steps = GetSteps(actor, 1900, 3600);
    CHECK(steps.position < 0.15f);
    CHECK(steps.yaw < 0.3f);

    // a stumble forward: it walks on from where it came to, at its pace, only its way off the path going away (over
    // 64 units)
    const auto stumbling = Walking({Clip("walk", 0, 2000), Still("stumble", 2000, 500), Clip("walk", 2500, 5000)},
                                   {{0, 0, 0}, {2000, 0, 0}});
    CHECK(glm::distance(At(stumbling, 2500).position, glm::vec3(250, 20, 0)) < 1e-2f);
    CHECK_NEAR(ActorLogic::GetPathProgress(stumbling, 2500, Animations()), 250.0, 1e-2);
    CHECK(glm::distance(At(stumbling, 2820).position, glm::vec3(282, 10, 0)) < 1e-2f);
    CHECK(glm::distance(At(stumbling, 3500).position, glm::vec3(350, 0, 0)) < 1e-2f);
    for (std::uint32_t tick = 2500; tick < 3500; tick += 10)
        CHECK_NEAR(At(stumbling, tick + 10).position.x - At(stumbling, tick).position.x, 1.0, 1e-3);
    // a walk fitted after it gets to the path's end as it ends
    auto fitted = Walking(
        {Clip("walk", 0, 2000), Still("stumble", 2000, 500), Clip("walk", 2500, 500), Clip("idle", 3000, 20000, 250)},
        {{0, 0, 0}, {600, 0, 0}});
    ActorLogic::FitWalk(fitted, fitted.clips[2].id, Animations(), 90.0f);
    const auto arrival = fitted.clips[2].End() + fitted.clips[3].blendIn;
    CHECK(ActorLogic::GetPathProgress(fitted, arrival, Animations()) >= 600.0f - 1e-3f);
    CHECK(ActorLogic::GetPathProgress(fitted, arrival - 100, Animations()) < 600.0f - 1.0f);
    CHECK(glm::distance(At(fitted, arrival).position, glm::vec3(600, 0, 0)) < 1e-2f);

    // a death on a path lies where it fell (the walk going on under its blend in walks 5.5 units more, the death
    // moving as it blends in as above)
    auto dying = Walking({Clip("walk", 0, 2000)}, {{0, 0, 0}, {2000, 0, 0}});
    ActorLogic::AddDeath(dying, 2000, "death", 1.0f);
    CHECK(glm::distance(At(dying, 6000).position, glm::vec3(200 + 5.5f + 59.788f, -11.4f, 0)) < 1e-2f);
}

TEST("root motion: a one-off that carries the actor round a corner of its path keeps the way it faces")
{
    // the user's hanoi_actor_test (2026-10-28): runs, then dies a few units before a right angle corner (the arc from
    // 275 to 314 units), the death's 60 units forward carrying it past the corner. It lies facing the way it fell
    // (it turned by the path's turn there, about 90 degrees on the spot, as the death ended: the way it went on along
    // the path had it face the way the path goes past the corner)
    auto dying = Walking({Clip("walk", 0, 2500)}, {{0, 0, 0}, {300, 0, 0}, {300, 300, 0}});
    ActorLogic::AddDeath(dying, 2500, "death", 1.0f);
    const auto fell = At(dying, 3500);
    CHECK_NEAR(YawOff(fell.yaw, 0.0f), 0.0, 1e-3);
    for (const std::uint32_t tick : {3501u, 3600u, 20000u})
    {
        const auto lying = At(dying, tick);
        CHECK_NEAR(YawOff(lying.yaw, fell.yaw), 0.0, 1e-2);
        CHECK(glm::distance(lying.position, fell.position) < 0.1f);
    }
    const auto steps = GetSteps(dying, 2400, 4000);
    CHECK(steps.yaw < 0.2f);
    CHECK(steps.position < 0.15f);
    // dying in the corner's arc: the walk going on under the death's blend in carries it 5.5 units further round the
    // arc, but it faces the way it faced as it was hit (it turned 12.6 degrees with the arc)
    auto cornered = Walking({Clip("walk", 0, 2900)}, {{0, 0, 0}, {300, 0, 0}, {300, 300, 0}});
    ActorLogic::AddDeath(cornered, 2900, "death", 1.0f);
    const auto hit = At(cornered, 2900);
    for (const std::uint32_t tick : {2950u, 3000u, 3899u, 3900u, 20000u})
        CHECK_NEAR(YawOff(At(cornered, tick).yaw, hit.yaw), 0.0, 1e-2);
    CHECK(GetSteps(cornered, 2900, 4000).position < 0.15f);

    // a stumble round the corner: it walks on from where it came to without turning on the spot, and turns with the
    // path as it goes back onto it
    const auto stumbling = Walking({Clip("walk", 0, 2500), Still("stumble", 2500, 500), Clip("walk", 3000, 6000)},
                                   {{0, 0, 0}, {300, 0, 0}, {300, 600, 0}});
    const auto stumbled = GetSteps(stumbling, 2400, 7000);
    CHECK(stumbled.yaw < 0.5f);
    CHECK(stumbled.position < 0.3f);
    CHECK_NEAR(YawOff(At(stumbling, 7000).yaw, 90.0f), 0.0, 1e-2);
}

TEST("root motion: moving an actor that a clip moved puts it where it is asked")
{
    auto still = Standing({Clip("idle", 0, 1000), Still("death", 1000, 1000)});
    const auto there = At(still, 3000);
    ActorLogic::ApplyPlacement(still, 3000, there.position + glm::vec3(5, -5, 0), there.yaw + 30.0f, Animations());
    const auto moved = At(still, 3000);
    CHECK(glm::distance(moved.position, there.position + glm::vec3(5, -5, 0)) < 1e-3f);
    CHECK_NEAR(YawOff(moved.yaw, there.yaw + 30.0f), 0.0, 1e-3);

    auto keyed = Standing({Clip("idle", 0, 1000), Still("death", 1000, 1000)});
    keyed.movement = Types::ActorMovement::Keyframes;
    keyed.keys = {{0, {0, 0, 0}, 0.0f}};
    // (its key there moves it from the first one as it lies, and turns the death with it: found until it stays put)
    ActorLogic::ApplyPlacement(keyed, 3000, {50, 50, 0}, 45.0f, Animations());
    CHECK(keyed.keys.size() == 2);
    CHECK(glm::distance(At(keyed, 3000).position, glm::vec3(50, 50, 0)) < 0.05f);
    CHECK_NEAR(YawOff(At(keyed, 3000).yaw, 45.0f), 0.0, 1e-2);
}

TEST("root motion: a clip's setting is saved, and scenes from before it move by it")
{
    auto actor = Standing({Clip("idle", 0, 1000), Still("death", 1000, 1000)});
    actor.clips[1].rootMotion = false;
    const auto read = ActorLogic::SceneFromJson(ActorLogic::SceneToJson({{actor}, ""}, "demo"), 90000);
    CHECK(read.actors.size() == 1 && read.actors[0].clips.size() == 2);
    CHECK(read.actors[0].clips[0].rootMotion && !read.actors[0].clips[1].rootMotion);

    auto old = ActorLogic::SceneToJson({{actor}, ""}, "demo");
    for (auto& clip : old["actors"][0]["clips"])
        clip.erase("rootMotion");
    CHECK(ActorLogic::SceneFromJson(old, 90000).actors[0].clips[1].rootMotion);
}

TEST("turns: a path turns through its corners at a steady rate, never on the spot")
{
    // walked at 100 units a second: corners of a radius of 25, turned at 4 radians a second
    const auto rate = glm::degrees(100.0f / 25.0f) / 1000.0f;
    CHECK_NEAR(ActorLogic::GetCornerRadius(100.0f), 25.0, 1e-4);
    for (const auto corner : {30.0f, 90.0f, 135.0f})
    {
        const auto turn = glm::radians(corner);
        const auto actor = Walking({Clip("walk", 0, 20000)},
                                   {{0, 0, 0}, {300, 0, 0}, {300 + 300 * std::cos(turn), 300 * std::sin(turn), 0}});
        const auto steps = GetSteps(actor, 2000, 4500);
        CHECK(steps.yaw <= rate * 1.01f + 1e-3f);
        CHECK(steps.position <= 0.1f * 1.01f);
        // it ends up facing the way the path goes on
        CHECK_NEAR(YawOff(At(actor, 5000).yaw, corner), 0.0, 1e-3);
    }
}

TEST("turns: a walk fitted to a path with corners gets to its end as the clip ends")
{
    auto actor = Walking({Clip("walk", 0, 1000), Clip("idle", 1000, 20000, 250)},
                         {{0, 0, 0}, {200, 0, 0}, {200, 200, 0}, {0, 200, 0}});
    ActorLogic::FitWalk(actor, actor.clips[0].id, Animations(), 90.0f);
    const auto end = actor.clips[0].End() + actor.clips[1].blendIn;
    const auto total = ActorLogic::GetPathLength(actor, Animations());
    // the rounded path is shorter than the points' (two corners of 25)
    CHECK_NEAR(total, 600.0 - 2.0 * (50.0 - 25.0 * glm::half_pi<double>()), 1e-2);
    CHECK(ActorLogic::GetWalkedDistance(actor, end, Animations()) >= total - 1e-3f);
    CHECK(ActorLogic::GetWalkedDistance(actor, end - 100, Animations()) < total - 1.0f);
    CHECK(glm::distance(At(actor, end).position, glm::vec3(0, 200, 0)) < 1e-3f);
}

TEST("turns: short stretches between corners round within half of each")
{
    const auto actor = Walking({Clip("walk", 0, 20000)},
                               {{0, 0, 0}, {100, 0, 0}, {110, 10, 0}, {120, 0, 0}, {130, 10, 0}, {300, 10, 0}});
    // the corners reach 7.07 units, half of the 14.14 of the stretches between them: the right angles turn on arcs of
    // a radius of 7.07, meeting each other
    const auto steps = GetSteps(actor, 0, 3000);
    CHECK(steps.position <= 0.101f);
    CHECK(steps.yaw <= glm::degrees(100.0f / 7.0711f) / 1000.0f * 1.01f);
    CHECK(glm::distance(At(actor, 5000).position, glm::vec3(300, 10, 0)) < 1e-3f);
}

TEST("turns: a straight path is walked as before, and a clip's walk angle turns over its blend in")
{
    const auto straight = Walking({Clip("walk", 0, 5000)}, {{0, 0, 0}, {0, 1000, 0}});
    CHECK(glm::distance(At(straight, 1234).position, glm::vec3(0, 123.4f, 0)) < 1e-3f);
    CHECK_NEAR(YawOff(At(straight, 1234).yaw, 90.0f), 0.0, 1e-4);

    // from standing to a strafe: turns from facing along the path to across it over the strafe's blend in
    const auto strafing = Walking({Clip("idle", 0, 1000), Clip("left", 1000, 3000, 400)}, {{0, 0, 0}, {0, 1000, 0}});
    CHECK_NEAR(YawOff(At(strafing, 1000).yaw, 90.0f), 0.0, 1e-3);
    CHECK_NEAR(YawOff(At(strafing, 1200).yaw, 45.0f), 0.0, 1e-3);
    CHECK_NEAR(YawOff(At(strafing, 1400).yaw, 0.0f), 0.0, 1e-3);
    CHECK(GetSteps(strafing, 900, 1500).yaw <= 90.0f / 400.0f + 1e-3f);
    // a walk back after a forward one turns round to the left
    const auto turning = Walking({Clip("walk", 0, 1000), Clip("back", 1000, 3000, 400)}, {{0, 0, 0}, {0, 1000, 0}});
    CHECK_NEAR(ActorLogic::GetWalk(turning, 1200, Animations()).angle, 90.0, 1e-3);
    CHECK(GetSteps(turning, 900, 1500).yaw <= 180.0f / 400.0f + 1e-3f);
}

TEST("turns: keys turn the actor without steps")
{
    Types::Actor keyed;
    keyed.movement = Types::ActorMovement::Keyframes;
    keyed.keys = {{0, {0, 0, 0}, 0.0f}, {1000, {100, 0, 0}, 90.0f}, {2000, {100, 100, 0}, 90.0f}};
    const auto steps = GetSteps(keyed, 0, 2000);
    CHECK(steps.yaw <= 0.09f + 1e-3f);
    CHECK(steps.position <= 0.2f);
}

TEST("turns: walking on from where the actor stopped curves from the way it came, and keeps the way there")
{
    // it walks east and stands; then walks to a point north of it from there
    auto actor = Standing({Clip("idle", 0, 30000)}, {0, 0, 0}, 0.0f);
    ActorLogic::WalkTo(actor, 1000, {300, 0, 0}, "walk", "idle", Animations(), 90.0f);
    const auto arrived = actor.clips[1].End() + 1000;
    const auto before = At(actor, arrived);
    ActorLogic::WalkTo(actor, arrived, {300, 300, 0}, "walk", "idle", Animations(), 90.0f);
    CHECK(glm::distance(At(actor, arrived).position, before.position) < 1e-3f);
    CHECK_NEAR(YawOff(At(actor, arrived).yaw, before.yaw), 0.0, 1e-3);
    const auto steps = GetSteps(actor, arrived, arrived + 1500);
    CHECK(steps.position <= 0.101f);
    CHECK(steps.yaw <= glm::degrees(100.0f / 12.5f) / 1000.0f * 1.01f);
    CHECK(glm::distance(At(actor, 29000).position, glm::vec3(300, 300, 0)) < 1e-3f);

    // told to walk elsewhere on its way round a corner, it keeps the way it was going up to then
    auto walker = Walking({Clip("walk", 0, 10000)}, {{0, 0, 0}, {300, 0, 0}, {300, 300, 0}});
    const auto cornering = 2900u;
    std::vector<Types::ActorPlacement> was;
    for (std::uint32_t tick = 0; tick <= cornering; tick += 50)
        was.push_back(At(walker, tick));
    ActorLogic::WalkTo(walker, cornering, {0, 600, 0}, "walk", "idle", Animations(), 90.0f);
    for (std::uint32_t tick = 0, i = 0; tick <= cornering; tick += 50, ++i)
    {
        CHECK(glm::distance(At(walker, tick).position, was[i].position) < 1e-2f);
        CHECK(YawOff(At(walker, tick).yaw, was[i].yaw) < 1e-2);
    }
    CHECK(GetSteps(walker, cornering - 100, cornering + 2000).yaw <= glm::degrees(100.0f / 12.5f) / 1000.0f * 1.01f);
}
