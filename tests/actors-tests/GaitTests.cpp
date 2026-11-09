#include "StdInclude.hpp"
#include "Test.hpp"

#include "Components/ActorLogic.hpp"

using namespace IWXMVM;

// Gait paths: a path's stretches go at gaits, as fast as each gait's animation goes for
// the body, at the actor's pace; the locomotion plays that gait; a stretch can wait at its first point; paths from
// before gaits walk as they did, and convert to the gait their speed picked and the pace that keeps that speed

namespace
{
    // the players' rifle animations as fast as the game measured them, an idle and a sprint of a second each
    ActorLogic::AnimationLookup Animations()
    {
        struct Known
        {
            std::string_view name;
            glm::vec2 motion;
            float duration = 1.0f;
        };
        static const std::vector<Known> known = {
            {"pb_stand_alert", {0.2f, 0}, 10.0f},    {"pb_stand_shoot_walk_forward", {94, 0}},
            {"pb_combatrun_forward_loop", {211, 0}}, {"pb_sprint", {281, 0}},
            {"pb_crouch_alert", {0.1f, 0}, 10.0f},   {"pb_crouch_shoot_run_forward", {83, 0}},
            {"pb_crouch_run_forward", {120, 0}},
        };
        return [](const std::string& name) -> std::optional<ActorLogic::AnimationInfo> {
            const auto found = std::ranges::find(known, name, &Known::name);
            if (found == known.end())
                return std::nullopt;
            return ActorLogic::AnimationInfo{
                found->duration, found->motion, {}, Types::AnimationKind::FullBody, Types::AnimationFamily::MpPlayer};
        };
    }

    bool Has(std::string_view name)
    {
        return Animations()(std::string(name)).has_value();
    }

    // an actor with Locomotion Auto walking a straight path along +x (points at 0, 100 and 400) with one clip of the
    // players' walk from 0 for 10 s, its stretches walking, running and running
    Types::Actor Walker()
    {
        Types::Actor actor;
        actor.locomotion = Types::ActorLocomotion::Auto;
        actor.movement = Types::ActorMovement::Path;
        actor.path = {{0, 0, 0}, {100, 0, 0}, {400, 0, 0}};
        actor.clips = {{.animation = "pb_stand_shoot_walk_forward", .start = 0, .length = 10000, .blendIn = 0}};
        ActorLogic::Normalize(actor);
        const auto body = ActorLogic::Body{};
        actor.stretches = {
            {.gait = Types::ActorGait::Walk}, {.gait = Types::ActorGait::Run}, {.gait = Types::ActorGait::Run}};
        ActorLogic::ResolveStretches(actor, body, Has);
        return actor;
    }

    float WalkedAt(const Types::Actor& actor, std::uint32_t tick)
    {
        return ActorLogic::GetWalkedDistance(actor, tick, Animations());
    }

    const ActorLogic::LegsSample* Legs(const ActorLogic::Layers& layers, std::string_view animation)
    {
        const auto found = std::ranges::find_if(
            layers.legs, [&](const ActorLogic::LegsSample& legs) { return !legs.clip && legs.animation == animation; });
        return found != layers.legs.end() ? &*found : nullptr;
    }
}  // namespace

TEST("gaits: a body's gaits and the animation each plays going forward")
{
    const auto gaits = ActorLogic::GetGaits(ActorLogic::Body{});
    CHECK(gaits.size() == 5);
    CHECK(ActorLogic::GetGaitAnimation({}, Types::ActorGait::Walk, false, Has) == "pb_stand_shoot_walk_forward");
    CHECK(ActorLogic::GetGaitAnimation({}, Types::ActorGait::Run, false, Has) == "pb_combatrun_forward_loop");
    CHECK(ActorLogic::GetGaitAnimation({}, Types::ActorGait::Sprint, false, Has) == "pb_sprint");
    CHECK(ActorLogic::GetGaitAnimation({}, Types::ActorGait::Run, true, Has) == "pb_crouch_run_forward");
    // a killstreak has none (it goes at its speeds)
    CHECK(ActorLogic::GetGaits(ActorLogic::Body::Vehicle(Types::Killstreak::AttackHelicopter)).empty());
}

TEST("gaits: a stretch goes as fast as its gait's animation, at the actor's pace")
{
    auto actor = Walker();
    CHECK_NEAR(ActorLogic::GetStretchSpeed(actor, actor.stretches[0], Animations()), 94.0, 1e-3);
    CHECK_NEAR(ActorLogic::GetStretchSpeed(actor, actor.stretches[1], Animations()), 211.0, 1e-3);
    actor.pace = 1.1f;
    CHECK_NEAR(ActorLogic::GetStretchSpeed(actor, actor.stretches[1], Animations()), 232.1, 1e-2);
    // (one the game doesn't have: as fast as the players' rifle's of the gait)
    Types::ActorStretch unknown{.gait = Types::ActorGait::Sprint, .animation = "pb_sprint_gone"};
    actor.pace = 1.0f;
    CHECK_NEAR(ActorLogic::GetStretchSpeed(actor, unknown, Animations()), 281.0, 1e-3);
}

TEST("gaits: the path is walked at each stretch's speed, standing at a point for its wait")
{
    auto actor = Walker();
    // walking to the point at 100 (94 a second: there by 1.064 s), then running (211 a second; with the join as before
    // joins, from the point on: the joins' blends are WorkflowTests')
    actor.stretches[1].joinAsBefore = true;
    CHECK_NEAR(WalkedAt(actor, 500), 47.0, 0.5);
    CHECK_NEAR(WalkedAt(actor, 1000), 94.0, 0.5);
    CHECK_NEAR(WalkedAt(actor, 2000), 100.0 + (2000.0 - 1063.8) * 0.211, 2.5);
    // at a pace of 110%
    actor.pace = 1.1f;
    CHECK_NEAR(WalkedAt(actor, 500), 51.7, 0.5);
    // a wait of a second at the point at 100
    actor.pace = 1.0f;
    actor.stretches[1].wait = 1000;
    CHECK_NEAR(WalkedAt(actor, 1500), 100.0, 0.5);
    CHECK_NEAR(WalkedAt(actor, 2000), 100.0, 0.5);
    CHECK_NEAR(WalkedAt(actor, 2600), 100.0 + (2600.0 - 2070.0) * 0.211, 3.0);
    // when it passes the points and arrives
    const auto timing = ActorLogic::GetPathTiming(actor, Animations());
    CHECK(timing.start && *timing.start == 0);
    CHECK(timing.reached[1] && std::abs(static_cast<int>(*timing.reached[1]) - 1064) <= 10);
    CHECK(timing.left[1] && std::abs(static_cast<int>(*timing.left[1]) - 2064) <= 10);
    CHECK(timing.arrival && std::abs(static_cast<int>(*timing.arrival) - (2070 + 1422)) <= 20);
}

TEST("gaits: the legs play the stretch's gait whatever the pace, at the rate the pace asks")
{
    auto actor = Walker();
    auto layers = ActorLogic::GetLayers(actor, 500, Animations(), ActorLogic::Body{});
    const auto* walk = Legs(layers, "pb_stand_shoot_walk_forward");
    CHECK(walk && walk->weight > 0.99f && std::abs(walk->rate - 1.0f) < 0.01f);
    layers = ActorLogic::GetLayers(actor, 2000, Animations(), ActorLogic::Body{});
    const auto* run = Legs(layers, "pb_combatrun_forward_loop");
    CHECK(run && run->weight > 0.99f && std::abs(run->rate - 1.0f) < 0.01f);
    // a sprint at 80% (225 a second) sprints, where the speed alone would pick a run
    actor.stretches = {{.gait = Types::ActorGait::Sprint}};
    ActorLogic::ResolveStretches(actor, {}, Has);
    ActorLogic::FitStretches(actor);
    actor.pace = 0.8f;
    layers = ActorLogic::GetLayers(actor, 500, Animations(), ActorLogic::Body{});
    const auto* sprint = Legs(layers, "pb_sprint");
    CHECK(sprint && sprint->weight > 0.99f && std::abs(sprint->rate - 0.8f) < 0.01f);
    // crouched: the crouched run
    actor.stretches = {{.gait = Types::ActorGait::Run, .crouched = true}};
    ActorLogic::ResolveStretches(actor, {}, Has);
    ActorLogic::FitStretches(actor);
    actor.pace = 1.0f;
    layers = ActorLogic::GetLayers(actor, 1500, Animations(), ActorLogic::Body{});
    const auto* crouched = Legs(layers, "pb_crouch_run_forward");
    CHECK(crouched && crouched->weight > 0.99f);
    // standing at a wait, its idle
    actor = Walker();
    actor.stretches[1].wait = 1000;
    layers = ActorLogic::GetLayers(actor, 1800, Animations(), ActorLogic::Body{});
    CHECK(Legs(layers, "pb_stand_alert") && Legs(layers, "pb_stand_alert")->weight > 0.99f);
}

TEST("gaits: a path from before gaits walks as it did, and converts to the gait its speed picked at its pace")
{
    for (const auto speed : {150.0f, 200.0f, 260.0f})
    {
        auto before = Walker();
        before.stretches.clear();
        before.pathSpeed = speed;
        auto after = before;
        CHECK(ActorLogic::MakeStretches(after, {}, Animations(), Has));
        CHECK(after.stretches.size() == after.path.size());
        // the gait the speed picks (152.5 and 246 between them), and the speed kept
        const auto gait = speed < 152.5f   ? Types::ActorGait::Walk
                          : speed < 246.0f ? Types::ActorGait::Run
                                           : Types::ActorGait::Sprint;
        CHECK(after.stretches.front().gait == gait);
        CHECK_NEAR(ActorLogic::GetStretchSpeed(after, after.stretches.front(), Animations()), speed, 0.01);
        // walked the same, with the same legs
        for (const auto tick : {300u, 1000u, 1700u})
        {
            CHECK_NEAR(WalkedAt(after, tick), WalkedAt(before, tick), 0.05);
            const auto old = ActorLogic::GetLayers(before, tick, Animations(), ActorLogic::Body{});
            const auto now = ActorLogic::GetLayers(after, tick, Animations(), ActorLogic::Body{});
            CHECK(old.legs.size() == now.legs.size());
            for (std::size_t i = 0; i < std::min(old.legs.size(), now.legs.size()); ++i)
            {
                CHECK(old.legs[i].animation == now.legs[i].animation);
                CHECK_NEAR(old.legs[i].rate, now.legs[i].rate, 1e-3);
            }
        }
    }
    // a walk run at a speed of its own (Run here): that speed, its own taken away
    auto running = Walker();
    running.stretches.clear();
    running.clips.front().walkSpeed = 211.0f;
    CHECK(ActorLogic::MakeStretches(running, {}, Animations(), Has));
    CHECK(running.stretches.front().gait == Types::ActorGait::Run && running.clips.front().walkSpeed == 0.0f);
    CHECK_NEAR(running.pace, 1.0, 1e-3);
    // standing still, or a killstreak: none
    Types::Actor still;
    still.locomotion = Types::ActorLocomotion::Auto;
    CHECK(!ActorLogic::MakeStretches(still, {}, Animations(), Has));
}

TEST("gaits: setting a stretch's gait, and points added and taken out keep the stretches in step")
{
    auto actor = Walker();
    ActorLogic::SetGait(actor, 1, 2, Types::ActorGait::Sprint, false, {}, Animations(), Has);
    // (the last point's, which nothing walks, goes as the last stretch walked)
    CHECK(actor.stretches[0].gait == Types::ActorGait::Walk && actor.stretches[1].gait == Types::ActorGait::Sprint &&
          actor.stretches[2].gait == Types::ActorGait::Sprint);
    CHECK(actor.stretches[1].animation == "pb_sprint");
    actor.stretches[1].wait = 500;
    // a point after the first: it goes on as the stretch it is put in, without its wait
    const auto index = ActorLogic::InsertWaypoint(actor, 0);
    CHECK(index == 1 && actor.path.size() == 4 && actor.stretches.size() == 4);
    CHECK(actor.stretches[1].gait == Types::ActorGait::Walk && actor.stretches[1].wait == 0);
    CHECK(actor.stretches[2].gait == Types::ActorGait::Sprint && actor.stretches[2].wait == 500);
    // taken out again: the stretch before it goes on
    ActorLogic::RemoveWaypoint(actor, 1);
    CHECK(actor.path.size() == 3 && actor.stretches.size() == 3 && actor.stretches[1].gait == Types::ActorGait::Sprint);
    // a point at the end goes on as the last stretch
    ActorLogic::AppendWaypoint(actor, {500, 0, 0});
    CHECK(actor.stretches.size() == 4 && actor.stretches[2].gait == Types::ActorGait::Sprint &&
          actor.stretches[3].gait == Types::ActorGait::Sprint);
}

TEST("gaits: scenes keep the stretches, the pace and the clips picked; scenes from before have none")
{
    ActorLogic::Scene scene;
    auto actor = Walker();
    actor.pace = 0.9f;
    actor.stretches[1].wait = 1500;
    actor.clips.front().picked = true;
    scene.actors.push_back(actor);
    const auto read = ActorLogic::SceneFromJson(ActorLogic::SceneToJson(scene, "demo"), 60000);
    CHECK(read.actors.size() == 1);
    CHECK(read.actors[0].stretches == actor.stretches);
    CHECK_NEAR(read.actors[0].pace, 0.9, 1e-6);
    CHECK(read.actors[0].clips.front().picked);
    // (and from before: none, as they walk at pathSpeed)
    auto json = ActorLogic::SceneToJson(scene, "demo");
    json["actors"][0].erase("stretches");
    json["actors"][0].erase("pace");
    json["actors"][0]["clips"][0].erase("picked");
    const auto old = ActorLogic::SceneFromJson(json, 60000);
    CHECK(old.actors[0].stretches.empty() && old.actors[0].pace == 1.0f && !old.actors[0].clips.front().picked);
    // a scene of actors without gaits writes none of them, as it was
    scene.actors.front().stretches.clear();
    scene.actors.front().clips.front().picked = false;
    const auto plain = ActorLogic::SceneToJson(scene, "demo");
    CHECK(!plain["actors"][0].contains("stretches") && !plain["actors"][0]["clips"][0].contains("picked"));
}

TEST("gaits: a clip the user picked plays its own animation, never the locomotion")
{
    auto actor = Walker();
    actor.clips = {{.animation = "pb_sprint", .start = 0, .length = 10000, .blendIn = 0, .picked = true}};
    ActorLogic::Normalize(actor);
    const auto layers = ActorLogic::GetLayers(actor, 500, Animations(), ActorLogic::Body{});
    CHECK(layers.clipCount == 1 && !layers.locomotion[0] && layers.legs.empty());
    // walking its path as fast as its own animation goes
    CHECK_NEAR(WalkedAt(actor, 500), 140.5, 1.0);
}

TEST("gaits: no hidden stretch past the last point: points added go on as the last stretch walked (H3)")
{
    // run, then a walk from point 1; the path ended at point 1 (End path here)
    auto actor = Walker();
    actor.stretches = {
        {.gait = Types::ActorGait::Run}, {.gait = Types::ActorGait::Walk}, {.gait = Types::ActorGait::Walk}};
    ActorLogic::ResolveStretches(actor, {}, Has);
    actor.path.pop_back();
    ActorLogic::FitStretches(actor);
    CHECK(actor.stretches.size() == 2 && actor.stretches[1].gait == Types::ActorGait::Run);
    CHECK(actor.stretches[1].animation == "pb_combatrun_forward_loop");
    // + Points: the new stretch runs, as the path does
    ActorLogic::AppendWaypoint(actor, {300, 0, 0});
    CHECK(actor.stretches.size() == 3 && actor.stretches[1].gait == Types::ActorGait::Run &&
          actor.stretches[2].gait == Types::ActorGait::Run);
    // a point taken out at the end, the same
    actor = Walker();
    ActorLogic::SetGait(actor, 1, 2, Types::ActorGait::Sprint, false, {}, Animations(), Has);
    CHECK(actor.stretches[2].gait == Types::ActorGait::Sprint);
    ActorLogic::RemoveWaypoint(actor, 2);
    CHECK(actor.stretches.size() == 2 && actor.stretches[1].gait == Types::ActorGait::Walk);
    // and kept so by every change (Normalize): the stretch walked last set to a run, the last point's runs
    actor = Walker();
    actor.stretches[1].gait = Types::ActorGait::Sprint;
    ActorLogic::Normalize(actor);
    CHECK(actor.stretches[2].gait == Types::ActorGait::Sprint);
    // (a path of one point keeps the gait it is to go at)
    Types::Actor placing;
    placing.movement = Types::ActorMovement::Path;
    placing.path = {{0, 0, 0}};
    placing.stretches = {{.gait = Types::ActorGait::Sprint}};
    ActorLogic::Normalize(placing);
    CHECK(placing.stretches.size() == 1 && placing.stretches[0].gait == Types::ActorGait::Sprint);
    ActorLogic::AppendWaypoint(placing, {100, 0, 0});
    CHECK(placing.stretches.size() == 2 && placing.stretches[0].gait == Types::ActorGait::Sprint);
}
