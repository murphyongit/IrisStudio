#include "StdInclude.hpp"
#include "Test.hpp"

#include "Components/ActorLogic.hpp"

using namespace IWXMVM;

namespace
{
    Types::ActorClip Clip(std::string animation, std::uint32_t start, std::uint32_t length,
                          std::uint32_t blendIn = 0, bool loop = true)
    {
        return {.animation = std::move(animation), .start = start, .length = length, .loop = loop, .blendIn = blendIn};
    }

    Types::Actor ActorWith(std::vector<Types::ActorClip> clips)
    {
        Types::Actor actor;
        actor.clips = std::move(clips);
        ActorLogic::Normalize(actor);
        return actor;
    }

    // one second plays: walk 100 units forward, back 118 back, left 50 to the left, right 60 to the right and a
    // little forward; idle and death don't move, unmeasured isn't measured yet
    ActorLogic::AnimationLookup Animations()
    {
        return [](const std::string& name) -> std::optional<ActorLogic::AnimationInfo> {
            if (name == "walk")
                return ActorLogic::AnimationInfo{1.0f, glm::vec2(100, 0)};
            if (name == "back")
                return ActorLogic::AnimationInfo{1.0f, glm::vec2(-118, 0)};
            if (name == "left")
                return ActorLogic::AnimationInfo{1.0f, glm::vec2(0, 50)};
            if (name == "right")
                return ActorLogic::AnimationInfo{1.0f, glm::vec2(8, -60)};
            if (name == "idle" || name == "death")
                return ActorLogic::AnimationInfo{1.0f, glm::vec2(0.5f, 0)};
            if (name == "unmeasured")
                return ActorLogic::AnimationInfo{1.0f, std::nullopt};
            // two seconds: a reload, and a burst that shoots as it starts and halfway
            static const std::vector<Types::ActorNotetrack> reload = {
                {"reload start", 0.05f}, {"drop clip", 0.1f},   {"attach clip left", 0.3f},
                {"detach clip left", 0.6f}, {"reload done", 0.9f}, {"end", 1.0f}};
            static const std::vector<Types::ActorNotetrack> burst = {
                {"fire", 0.0f}, {"sndnt#fly_shout", 0.25f}, {"fire", 0.5f}};
            static const std::vector<Types::ActorNotetrack> rifleReload = {
                {"sndnt#fly_assault_reload_npc_mag_out", 0.1f}, {"sndnt#fly_assault_reload_npc_mag_in", 0.4f}};
            if (name == "rifle_reload")
                return ActorLogic::AnimationInfo{2.0f, glm::vec2(0), rifleReload};
            if (name == "reload")
                return ActorLogic::AnimationInfo{2.0f, glm::vec2(0), reload};
            if (name == "burst")
                return ActorLogic::AnimationInfo{2.0f, glm::vec2(0), burst};
            return std::nullopt;
        };
    }

    // what the reload profiles of the reloads say (see ReloadLogic): the SP reload drops the magazine at its drop clip
    // and has a new one from its attach clip, in at its detach clip; the rifle reload pulls it out at the magazine out
    // sound, swaps it halfway to the magazine in sound and puts the new one in there
    ActorLogic::ReloadMomentsLookup Moments()
    {
        return [](const Types::ActorClip& clip) -> std::optional<ActorLogic::ReloadMoments> {
            if (clip.animation == "reload")
                return ActorLogic::ReloadMoments{.out = 0.1f, .in = 0.6f, .swap = 0.3f, .dropped = true};
            if (clip.animation == "rifle_reload")
                return ActorLogic::ReloadMoments{.out = 0.1f, .in = 0.4f, .swap = 0.25f};
            return std::nullopt;
        };
    }

    // the names of the notetracks passed playing from a tick to another in frames of 16 ms
    std::vector<std::string> PlayNotetracks(const Types::Actor& actor, std::uint32_t from, std::uint32_t to)
    {
        std::vector<std::string> names;
        for (std::uint32_t tick = from; tick < to; tick += 16)
            for (const auto& passed : ActorLogic::GetPassedNotetracks(actor, tick, std::min(tick + 16, to), Animations()))
                names.emplace_back(passed.name);
        return names;
    }

    // an actor on a straight path along y (its direction is 90 degrees)
    Types::Actor Walker(std::vector<Types::ActorClip> clips)
    {
        auto actor = ActorWith(std::move(clips));
        actor.movement = Types::ActorMovement::Path;
        actor.path = {{0, 0, 0}, {0, 1000, 0}};
        return actor;
    }

    double YawOff(float yaw, float expected)
    {
        return std::abs(std::remainder(yaw - expected, 360.0f));
    }
}  // namespace

TEST("a clip shows its animation from its start, at its speed and offset")
{
    auto clip = Clip("walk", 1000, 2000);
    clip.speed = 2.0f;
    clip.offset = 0.5f;
    const auto actor = ActorWith({clip});

    const auto samples = ActorLogic::Evaluate(actor, 1500);
    CHECK(samples.size() == 1);
    CHECK_NEAR(samples[0].weight, 1.0, 1e-6);
    CHECK_NEAR(samples[0].time, 0.5 + 0.5 * 2.0, 1e-6);
}

TEST("before the first clip the actor holds that clip's first frame")
{
    const auto actor = ActorWith({Clip("walk", 1000, 2000)});
    const auto samples = ActorLogic::Evaluate(actor, 0);
    CHECK(samples.size() == 1);
    CHECK_NEAR(samples[0].time, 0.0, 1e-6);
}

TEST("after a clip ends it holds the time it ended at until the next clip")
{
    const auto actor = ActorWith({Clip("walk", 0, 1000), Clip("idle", 5000, 1000)});
    const auto samples = ActorLogic::Evaluate(actor, 3000);
    CHECK(samples.size() == 1);
    CHECK(samples[0].clip->animation == "walk");
    CHECK_NEAR(samples[0].time, 1.0, 1e-6);
}

TEST("a clip blending in shares the weight with the clip before, adding up to 1")
{
    const auto actor = ActorWith({Clip("walk", 0, 2000), Clip("death", 2000, 1000, 400)});
    for (const std::uint32_t tick : {2000u, 2100u, 2200u, 2399u})
    {
        const auto samples = ActorLogic::Evaluate(actor, tick);
        CHECK(samples.size() == 2);
        CHECK(samples[0].clip->animation == "death");
        CHECK_NEAR(samples[0].weight + samples[1].weight, 1.0, 1e-6);
        CHECK_NEAR(samples[0].weight, (tick - 2000) / 400.0, 1e-6);
    }
    // the clip before goes on playing under the blend
    CHECK_NEAR(ActorLogic::Evaluate(actor, 2200)[1].time, 2.2, 1e-6);
    CHECK(ActorLogic::Evaluate(actor, 2400).size() == 1);
}

TEST("the samples at a tick don't depend on the ticks asked before")
{
    // the game has to weigh every tree entry from these alone, see Animate in t5/src/Actors.cpp
    const auto actor = ActorWith({Clip("walk", 0, 2000), Clip("death", 2000, 1000, 100, false)});
    const auto fresh = ActorLogic::Evaluate(actor, 500);
    ActorLogic::Evaluate(actor, 2050);
    ActorLogic::Evaluate(actor, 9000);
    const auto again = ActorLogic::Evaluate(actor, 500);
    CHECK(fresh.size() == 1 && again.size() == 1);
    CHECK(fresh[0].clip == again[0].clip);
    CHECK_NEAR(fresh[0].time, again[0].time, 1e-6);
}

TEST("adding a clip cuts the clip playing where it starts and gives it a new id")
{
    auto actor = ActorWith({Clip("walk", 0, 5000)});
    const auto id = ActorLogic::AddClip(actor, Clip("death", 2000, 1000));
    CHECK(actor.clips.size() == 2);
    CHECK(actor.clips[0].length == 2000);
    CHECK(actor.clips[1].id == id);
    CHECK(id != actor.clips[0].id);
}

TEST("a path is walked as far as the moving clips' animations step, not while they hold")
{
    auto actor = ActorWith({Clip("walk", 0, 2000, 0, false), Clip("idle", 3000, 1000)});
    actor.movement = Types::ActorMovement::Path;
    actor.path = {{0, 0, 0}, {1000, 0, 0}};
    const auto animations = Animations();
    // the walk plays once (1 s, 100 units) and then holds its last frame
    CHECK_NEAR(ActorLogic::GetWalkedDistance(actor, 500, animations), 50.0, 0.5);
    CHECK_NEAR(ActorLogic::GetWalkedDistance(actor, 1000, animations), 100.0, 0.5);
    CHECK_NEAR(ActorLogic::GetWalkedDistance(actor, 2500, animations), 100.0, 0.5);

    actor.clips[0].loop = true;
    CHECK_NEAR(ActorLogic::GetWalkedDistance(actor, 2000, animations), 200.0, 0.5);
    CHECK_NEAR(ActorLogic::EvaluatePlacement(actor, 1500, animations).position.x, 150.0, 0.5);

    actor.clips[0].moves = false;
    CHECK_NEAR(ActorLogic::GetWalkedDistance(actor, 2000, animations), 0.0, 1e-6);
}

TEST("a strafe walks along the path facing across it, a walk back facing back along it")
{
    const auto animations = Animations();
    const auto left = Walker({Clip("left", 0, 5000)});
    const auto atLeft = ActorLogic::EvaluatePlacement(left, 2000, animations);
    CHECK_NEAR(atLeft.position.y, 100.0, 0.5);
    // walking to its left along a path that goes +y, it faces +x
    CHECK_NEAR(YawOff(atLeft.yaw, 0.0f), 0.0, 0.01);

    const auto back = Walker({Clip("back", 0, 5000)});
    const auto atBack = ActorLogic::EvaluatePlacement(back, 1000, animations);
    CHECK_NEAR(atBack.position.y, 118.0, 0.5);
    CHECK_NEAR(YawOff(atBack.yaw, -90.0f), 0.0, 0.01);

    // the motion's length is walked, at the motion's angle
    const auto right = Walker({Clip("right", 0, 5000)});
    const auto atRight = ActorLogic::EvaluatePlacement(right, 1000, animations);
    CHECK_NEAR(atRight.position.y, std::hypot(8.0, 60.0), 0.5);
    CHECK_NEAR(YawOff(atRight.yaw, 90.0f + glm::degrees(std::atan2(60.0f, 8.0f))), 0.0, 0.01);

    const auto forward = Walker({Clip("walk", 0, 5000)});
    CHECK_NEAR(YawOff(ActorLogic::EvaluatePlacement(forward, 1000, animations).yaw, 90.0f), 0.0, 0.01);
}

TEST("stopping after a strafe keeps facing the way it strafed")
{
    const auto animations = Animations();
    const auto actor = Walker({Clip("left", 0, 1000), Clip("idle", 1000, 3000, 200)});
    const auto stopped = ActorLogic::EvaluatePlacement(actor, 3000, animations);
    CHECK_NEAR(YawOff(stopped.yaw, 0.0f), 0.0, 0.01);
    // and stands where the strafe ended, plus what it walked while the idle blended in
    const auto walk = ActorLogic::GetWalk(actor, 3000, animations);
    CHECK(walk.distance > 50.0f && walk.distance < 50.0f + 50.0f * 0.2f);
    CHECK_NEAR(ActorLogic::GetWalk(actor, 3000, animations).distance, ActorLogic::GetWalk(actor, 2000, animations).distance,
               1e-4);
}

TEST("blending from a walk into a strafe turns the actor over the blend")
{
    const auto animations = Animations();
    const auto actor = Walker({Clip("walk", 0, 1000), Clip("left", 1000, 3000, 400)});
    CHECK_NEAR(ActorLogic::GetWalk(actor, 999, animations).angle, 0.0, 0.01);
    const auto halfway = ActorLogic::GetWalk(actor, 1200, animations).angle;
    CHECK_NEAR(halfway, 45.0, 1.0);
    CHECK_NEAR(ActorLogic::GetWalk(actor, 1400, animations).angle, 90.0, 0.01);
}

TEST("a clip's direction and speed can be set instead of the animation's")
{
    const auto animations = Animations();
    auto clip = Clip("walk", 0, 5000);
    clip.direction = Types::ActorWalkDirection::Right;
    auto walk = ActorLogic::GetClipWalk(clip, animations);
    CHECK(walk.angle && std::abs(*walk.angle + 90.0f) < 1e-4f);
    CHECK_NEAR(walk.speed, 100.0, 1e-4);

    // an animation not measured yet (or one that doesn't move) walks forward at a speed given to it
    auto unmeasured = Clip("unmeasured", 0, 5000);
    CHECK(!ActorLogic::GetClipWalk(unmeasured, animations).angle);
    unmeasured.walkSpeed = 40.0f;
    walk = ActorLogic::GetClipWalk(unmeasured, animations);
    CHECK(walk.angle && std::abs(*walk.angle) < 1e-4f);
    CHECK_NEAR(walk.speed, 40.0, 1e-4);
    unmeasured.speed = 2.0f;
    CHECK_NEAR(ActorLogic::GetWalk(Walker({unmeasured}), 1000, animations).distance, 80.0, 0.5);

    // idles barely move: they don't walk at all
    CHECK(!ActorLogic::GetClipWalk(Clip("idle", 0, 1000), animations).angle);
    auto noWalk = Clip("left", 0, 1000);
    noWalk.moves = false;
    CHECK(!ActorLogic::GetClipWalk(noWalk, animations).angle);
}

TEST("a ray picks an actor by its body, and misses beside it")
{
    const glm::vec3 feet(100, 0, 0);
    // through the chest, from the side
    const auto hit = ActorLogic::RayHitsActor({0, 0, 40}, {1, 0, 0}, feet);
    CHECK(hit.has_value());
    CHECK_NEAR(*hit, 84.0, 0.1);
    // over its head, beside it and behind the ray's origin
    CHECK(!ActorLogic::RayHitsActor({0, 0, 100}, {1, 0, 0}, feet));
    CHECK(!ActorLogic::RayHitsActor({0, 30, 40}, {1, 0, 0}, feet));
    CHECK(!ActorLogic::RayHitsActor({0, 0, 40}, {-1, 0, 0}, feet));
    // from above, down onto its head
    const auto down = ActorLogic::RayHitsActor({100, 0, 200}, {0, 0, -1}, feet);
    CHECK(down.has_value());
    CHECK_NEAR(*down, 128.0, 0.1);
}

TEST("notetracks are passed once each as a clip plays, again each play of a loop")
{
    const auto once = ActorWith({Clip("reload", 1000, 3000, 0, false)});
    const std::vector<std::string> reload = {"reload start", "drop clip",   "attach clip left",
                                             "detach clip left", "reload done", "end"};
    CHECK(PlayNotetracks(once, 0, 5000) == reload);
    // holding its last frame passes nothing more
    CHECK(PlayNotetracks(once, 3100, 5000).empty());

    const auto looping = ActorWith({Clip("reload", 1000, 4000)});
    CHECK(PlayNotetracks(looping, 0, 6000).size() == 2 * reload.size());

    // in a long step too (a jump forward plays a second of events at once)
    CHECK(ActorLogic::GetPassedNotetracks(once, 999, 2000, Animations()).size() == 3);
}

TEST("a notetrack at the very start of a clip is passed as it starts, and none after the next clip starts")
{
    const auto actor = ActorWith({Clip("burst", 1000, 3000), Clip("idle", 1800, 1000)});
    CHECK((PlayNotetracks(actor, 0, 4000) == std::vector<std::string>{"fire", "sndnt#fly_shout"}));
    // nor when the clip starts in the past
    CHECK(PlayNotetracks(actor, 1001, 1400).empty());
    CHECK(ActorLogic::IsShotNotetrack("fire") && ActorLogic::IsShotNotetrack("fire_spray"));
    CHECK(!ActorLogic::IsShotNotetrack("fire_start"));
    CHECK(ActorLogic::GetNotetrackSound("sndnt#fly_shout") == "fly_shout");
    CHECK(ActorLogic::GetNotetrackSound("footstep_left").empty());
}

TEST("a reload drops the magazine from the gun and has a new one in the hand from its swap, as its profile says")
{
    using Held = ActorLogic::HeldMagazine;
    const auto animations = Animations();
    // two plays of a looping SP reload: dropped at 0.1, a new one from 0.3, in at 0.6 of 2 s
    const auto actor = ActorWith({Clip("walk", 0, 1000), Clip("reload", 1000, 4000)});
    const auto reloads = ActorLogic::GetReloads(actor, 10000, animations, Moments());
    CHECK(reloads.size() == 2);
    CHECK(reloads[0].out == 1200 && reloads[0].in == 2200 && reloads[0].end == 5000);
    CHECK(reloads[0].dropped && reloads[0].swap == 1600u);
    CHECK(reloads[1].out == 3200 && reloads[1].in == 4200);
    // only those out by a tick
    CHECK(ActorLogic::GetReloads(actor, 1199, animations, Moments()).empty());
    CHECK(ActorLogic::GetReloads(actor, 3200, animations, Moments()).size() == 2);

    const auto& reload = reloads[0];
    CHECK(!ActorLogic::GetReloadState(reload, 1100).offGun);
    CHECK(ActorLogic::GetReloadState(reload, 1300).offGun);
    CHECK(ActorLogic::GetReloadState(reload, 1300).held == Held::None);
    CHECK(ActorLogic::GetReloadState(reload, 1700).held == Held::New);
    CHECK(!ActorLogic::GetReloadState(reload, 2200).offGun);
    CHECK(ActorLogic::GetFallStart(reload) == 1200u);
    // without a swap the hand never shows the new one
    auto noSwap = reload;
    noSwap.swap.reset();
    CHECK(ActorLogic::GetReloadState(noSwap, 1700).offGun &&
          ActorLogic::GetReloadState(noSwap, 1700).held == Held::None);
    CHECK(ActorLogic::GetFallStart(noSwap) == 1200u);
}

TEST("an MP rifle reload pulls the magazine out, swaps it for a new one, and stops with its clip")
{
    using Held = ActorLogic::HeldMagazine;
    const auto animations = Animations();
    // out at 0.1, the swap at 0.25 and in at 0.4 of 2 s
    const auto actor = ActorWith({Clip("rifle_reload", 0, 2000, 0, false)});
    const auto reloads = ActorLogic::GetReloads(actor, 2000, animations, Moments());
    CHECK(reloads.size() == 1);
    const auto& reload = reloads[0];
    CHECK(reload.out == 200 && reload.in == 800 && !reload.dropped && reload.swap == 500u);
    CHECK(ActorLogic::GetReloadState(reload, 300).held == Held::Old);
    CHECK(ActorLogic::GetReloadState(reload, 600).held == Held::New);
    CHECK(!ActorLogic::GetReloadState(reload, 900).offGun);
    CHECK(ActorLogic::GetFallStart(reload) == 500u);
    // without a swap the hand holds the old one until in, and never lets it fall
    auto noSwap = reload;
    noSwap.swap.reset();
    CHECK(ActorLogic::GetReloadState(noSwap, 700).held == Held::Old);
    CHECK(!ActorLogic::GetFallStart(noSwap));

    // the next clip cuts it before the swap: the hand puts the old one back
    const auto cut = ActorWith({Clip("rifle_reload", 0, 2000, 0, false), Clip("walk", 400, 1000)});
    const auto cutReload = ActorLogic::GetReloads(cut, 2000, animations, Moments()).at(0);
    CHECK(cutReload.end == 400);
    CHECK(ActorLogic::GetReloadState(cutReload, 300).held == Held::Old);
    CHECK(!ActorLogic::GetReloadState(cutReload, 450).offGun);
    CHECK(!ActorLogic::GetFallStart(cutReload));
}

TEST("a long looping reload clip goes on reloading, and lists only its last few reloads")
{
    const auto animations = Animations();
    // 100 plays of the 2 s SP reload, which drops the magazine 0.2 s into each
    const auto actor = ActorWith({Clip("reload", 0, 200000)});
    const auto reloads = ActorLogic::GetReloads(actor, 190500, animations, Moments());
    CHECK(!reloads.empty() && reloads.size() <= ActorLogic::RECENT_RELOAD_PLAYS + 1);
    CHECK(reloads.size() >= 4);
    CHECK(!reloads.empty() && reloads.back().out == 190200 && reloads.back().in == 191200);
    for (std::size_t i = 1; i < reloads.size(); ++i)
        CHECK(reloads[i].out == reloads[i - 1].out + 2000);
    // long after the clip ends, its last ones are still there to lie on the ground
    const auto after = ActorLogic::GetReloads(actor, 1000000, animations, Moments());
    CHECK(!after.empty() && after.back().out == 198200);
    // a clip that doesn't loop plays it once, even when it starts past the reload
    auto once = ActorWith({Clip("reload", 0, 10000, 0, false)});
    CHECK(ActorLogic::GetReloads(once, 10000, animations, Moments()).size() == 1);
    once.clips[0].offset = 0.5f;
    CHECK(ActorLogic::GetReloads(once, 10000, animations, Moments()).empty());
}

TEST("a dropped magazine falls, lands at its time and settles flat on its side")
{
    ActorLogic::MagazineFall fall;
    fall.start = glm::translate(glm::mat4(1.0f), glm::vec3(10, 20, 100)) *
                 glm::rotate(glm::mat4(1.0f), glm::radians(30.0f), glm::vec3(1, 0, 0));
    fall.middle = {0, 0, -3};
    fall.spin = 300.0f;
    fall.ground = 0.0f;
    const auto startMiddle = glm::vec3(fall.start * glm::vec4(fall.middle, 1.0f));
    const auto landing = ActorLogic::GetLandingTime(fall);
    CHECK_NEAR(landing,
               std::sqrt(2.0f * (startMiddle.z - ActorLogic::MAGAZINE_HALF_THICKNESS) / ActorLogic::MAGAZINE_GRAVITY),
               1e-4);

    // falling as gravity has it, from where it started
    const auto middleAt = [&](float seconds) {
        return glm::vec3(ActorLogic::GetMagazineTransform(fall, seconds) * glm::vec4(fall.middle, 1.0f));
    };
    CHECK_NEAR(glm::distance(middleAt(0.0f), startMiddle), 0.0, 1e-3);
    CHECK_NEAR(middleAt(0.25f).z, startMiddle.z - ActorLogic::MAGAZINE_GRAVITY / 2.0f * 0.0625f, 1e-3);
    CHECK_NEAR(middleAt(0.25f).x, 10.0, 1e-3);

    // and later on the ground, lying on its side: the model's y axis upright
    const auto rest = ActorLogic::GetMagazineTransform(fall, landing + 1.0f);
    CHECK_NEAR(middleAt(landing + 1.0f).z, ActorLogic::MAGAZINE_HALF_THICKNESS, 1e-3);
    CHECK_NEAR(std::abs(glm::vec3(rest * glm::vec4(0, 1, 0, 0)).z), 1.0, 1e-3);
    CHECK_NEAR(glm::distance(glm::vec3(middleAt(landing + 1.0f)), glm::vec3(middleAt(landing + 5.0f))), 0.0, 1e-4);

    // pushed sideways, it lands that far away
    fall.velocity = {50, 0, 0};
    CHECK_NEAR(middleAt(landing + 1.0f).x, 10.0f + 50.0f * landing, 1e-2);
}

TEST("a point on a path stays on it, and turns around its corners on an arc")
{
    // the corner rounded with a radius of 20: an arc from 20 before it to 20 after it, a quarter of a circle long
    const auto path = ActorLogic::RoundPath({{0, 0, 0}, {100, 0, 0}, {100, 100, 0}}, 20.0f);
    const auto arc = 20.0 * glm::half_pi<double>();
    CHECK_NEAR(path.length, 80.0 + arc + 80.0, 1e-3);
    CHECK_NEAR(ActorLogic::PlaceOnPath(path, -10).position.x, 0.0, 1e-4);
    CHECK_NEAR(ActorLogic::PlaceOnPath(path, 1000).position.y, 100.0, 1e-4);
    CHECK_NEAR(ActorLogic::PlaceOnPath(path, 50).yaw, 0.0, 1e-3);
    CHECK_NEAR(ActorLogic::PlaceOnPath(path, 80.0f + static_cast<float>(arc) + 10.0f).yaw, 90.0, 1e-3);
    // halfway round, a few units inside the corner's point
    const auto middle = ActorLogic::PlaceOnPath(path, 80.0f + static_cast<float>(arc) / 2.0f);
    CHECK_NEAR(middle.yaw, 45.0, 1e-2);
    CHECK_NEAR(middle.position.x, 80.0 + 20.0 * std::sin(glm::quarter_pi<double>()), 1e-3);
    CHECK_NEAR(middle.position.y, 20.0 * (1.0 - std::cos(glm::quarter_pi<double>())), 1e-3);
}

TEST("keys are passed through smoothly, turning the short way round")
{
    Types::Actor actor;
    actor.movement = Types::ActorMovement::Keyframes;
    actor.keys = {{1000, {0, 0, 0}, 170.0f}, {2000, {100, 0, 0}, -170.0f}};
    const auto animations = Animations();
    CHECK_NEAR(ActorLogic::EvaluatePlacement(actor, 0, animations).position.x, 0.0, 1e-4);
    CHECK_NEAR(ActorLogic::EvaluatePlacement(actor, 1500, animations).position.x, 50.0, 1e-3);
    CHECK_NEAR(ActorLogic::EvaluatePlacement(actor, 1500, animations).yaw, 180.0, 1e-3);
    CHECK_NEAR(ActorLogic::EvaluatePlacement(actor, 3000, animations).position.x, 100.0, 1e-4);
}

TEST("moving an actor keys it at the tick, moves its path, or moves it")
{
    const auto animations = Animations();
    Types::Actor keyed;
    keyed.movement = Types::ActorMovement::Keyframes;
    ActorLogic::ApplyPlacement(keyed, 500, {1, 2, 3}, 45.0f, animations);
    CHECK(keyed.keys.size() == 1 && keyed.keys[0].tick == 500);

    Types::Actor walking;
    walking.movement = Types::ActorMovement::Path;
    walking.path = {{0, 0, 0}, {100, 0, 0}};
    ActorLogic::ApplyPlacement(walking, 0, {10, 20, 0}, 0.0f, animations);
    CHECK_NEAR(walking.path[1].x, 110.0, 1e-4);
    CHECK_NEAR(walking.path[1].y, 20.0, 1e-4);
}

TEST("Show and Hide events decide whether an actor is drawn")
{
    Types::Actor actor;
    CHECK(ActorLogic::IsVisible(actor, 0));
    actor.events = {{.type = Types::ActorEventType::Show, .tick = 1000},
                    {.type = Types::ActorEventType::Hide, .tick = 3000}};
    ActorLogic::Normalize(actor);
    CHECK(!ActorLogic::IsVisible(actor, 500));
    CHECK(ActorLogic::IsVisible(actor, 1000));
    CHECK(ActorLogic::IsVisible(actor, 2999));
    CHECK(!ActorLogic::IsVisible(actor, 3000));
    actor.hidden = true;
    CHECK(!ActorLogic::IsVisible(actor, 2000));
}

TEST("events go off once as the timeline plays past them, a burst shot by shot")
{
    Types::Actor actor;
    actor.events = {{.type = Types::ActorEventType::Fire, .tick = 1000, .shots = 3, .interval = 100},
                    {.type = Types::ActorEventType::Hit, .tick = 1150}};
    ActorLogic::Normalize(actor);
    // frames of 16 ms: every shot once, in order, and nothing twice at frame edges
    std::vector<std::uint32_t> shots;
    std::size_t hits = 0;
    for (std::uint32_t tick = 900; tick < 1400; tick += 16)
    {
        for (const auto& fired : ActorLogic::GetFiredEvents(actor, tick, tick + 16))
        {
            if (fired.event->type == Types::ActorEventType::Fire)
                shots.push_back(fired.shot);
            else
                ++hits;
        }
    }
    CHECK((shots == std::vector<std::uint32_t>{0, 1, 2}));
    CHECK(hits == 1);
    // the tick it starts from is included, the one it plays to excluded
    CHECK(ActorLogic::GetFiredEvents(actor, 1000, 1001).size() == 1);
    CHECK(ActorLogic::GetFiredEvents(actor, 999, 1000).empty());
    CHECK(ActorLogic::GetFiredEvents(actor, 1000, 1000).empty());
}

TEST("an event added at the paused playhead goes off as the timeline plays on, once (#26)")
{
    Types::Actor actor;
    actor.events = {{.type = Types::ActorEventType::Hit, .tick = 5000}};
    ActorLogic::Normalize(actor);
    // paused at 5000: frames from 5000 to 5000 fire nothing
    for (int frame = 0; frame < 10; frame++)
        CHECK(ActorLogic::GetFiredEvents(actor, 5000, 5000).empty());
    // Play: the first frame goes off, the next don't
    CHECK(ActorLogic::GetFiredEvents(actor, 5000, 5016).size() == 1);
    CHECK(ActorLogic::GetFiredEvents(actor, 5016, 5032).empty());
    // played up to it and paused there, then on: once as well
    std::size_t hits = 0;
    for (const auto& [from, to] : {std::pair(4984u, 5000u), std::pair(5000u, 5000u), std::pair(5000u, 5016u)})
        hits += ActorLogic::GetFiredEvents(actor, from, to).size();
    CHECK(hits == 1);
}

TEST("a scene is saved and read back as it was")
{
    Types::Actor actor = ActorWith({Clip("walk", 100, 2000, 250), Clip("death", 2100, 1500, 100, false)});
    actor.clips[0].direction = Types::ActorWalkDirection::Left;
    actor.clips[0].walkSpeed = 75.0f;
    actor.clips[1].notetrackShots = false;
    actor.clips[1].notetrackSounds = false;
    actor.clips[1].grip = false;
    actor.name = "Actor 1";
    actor.models = {"body", "head"};
    actor.movement = Types::ActorMovement::Path;
    actor.path = {{1, 2, 3}, {4, 5, 6}};
    actor.keys = {{10, {7, 8, 9}, 90.0f}};
    actor.weapon = "ak47_mp";
    actor.attachments = {"acog", "silencer"};
    actor.camo = 15;
    actor.followGround = false;
    actor.events = {{.type = Types::ActorEventType::Fire, .tick = 500, .shots = 4, .interval = 80, .tracers = false},
                    {.type = Types::ActorEventType::Effect, .tick = 900, .effect = "fx", .tag = "j_head"},
                    {.type = Types::ActorEventType::Look,
                     .tick = 950,
                     .lookTarget = Types::ActorLookTarget::Point,
                     .lookPoint = {1, 2, 3},
                     .lookPlayer = 4,
                     .lookTurn = 250}};
    ActorLogic::Normalize(actor);
    // ids are where the actors are in the scene, the actors looked at too
    actor.id = 1;
    actor.props = {{.model = "radio", .tag = "tag_weapon_left", .offset = {1, 2, 3}, .angles = {10, 20, 30}}};
    Types::Actor other;
    other.id = 7;
    other.name = "Actor 2";
    other.movement = Types::ActorMovement::Attached;
    other.attachment = {.kind = Types::ActorAttachment::Kind::Actor,
                        .actor = 1,
                        .tag = "j_spine4",
                        .offset = {0, 40, 0},
                        .yaw = 90.0f};
    other.events = {{.type = Types::ActorEventType::Look, .lookTarget = Types::ActorLookTarget::Actor, .lookActor = 1}};
    ActorLogic::Normalize(other);

    const ActorLogic::Scene scene{{actor, other}, "wmd"};
    const auto json = ActorLogic::SceneToJson(scene, "demo");
    const auto read = ActorLogic::SceneFromJson(nlohmann::json::parse(json.dump()), 99999);
    CHECK(read.pack == "wmd");
    CHECK(read.actors.size() == 2);
    CHECK(read.actors[0] == actor);
    CHECK(read.actors[1].id == 2);
    CHECK(read.actors[1].events[0].lookActor == 1);
    CHECK(read.actors[1].events[0].lookTarget == Types::ActorLookTarget::Actor);
    CHECK(read.actors[1].movement == Types::ActorMovement::Attached);
    CHECK(read.actors[1].attachment == other.attachment);
}

TEST("a look turns from what was looked at before over its turn time")
{
    Types::Actor actor;
    actor.events = {{.type = Types::ActorEventType::Look, .tick = 1000, .lookTarget = Types::ActorLookTarget::Camera,
                     .lookTurn = 400},
                    {.type = Types::ActorEventType::Fire, .tick = 1500},
                    {.type = Types::ActorEventType::Look, .tick = 2000, .lookTarget = Types::ActorLookTarget::Point,
                     .lookTurn = 200},
                    {.type = Types::ActorEventType::Look, .tick = 3000, .lookTarget = Types::ActorLookTarget::Ahead,
                     .lookTurn = 0}};
    ActorLogic::Normalize(actor);

    auto look = ActorLogic::GetLook(actor, 500);
    CHECK(!look.from && !look.to);
    look = ActorLogic::GetLook(actor, 1100);
    CHECK(!look.from && look.to && look.to->lookTarget == Types::ActorLookTarget::Camera);
    CHECK_NEAR(look.turned, 0.25, 1e-6);
    look = ActorLogic::GetLook(actor, 1900);
    CHECK_NEAR(look.turned, 1.0, 1e-6);
    look = ActorLogic::GetLook(actor, 2100);
    CHECK(look.from && look.from->lookTarget == Types::ActorLookTarget::Camera);
    CHECK(look.to && look.to->lookTarget == Types::ActorLookTarget::Point);
    CHECK_NEAR(look.turned, 0.5, 1e-6);
    // looking ahead again, at once
    look = ActorLogic::GetLook(actor, 3000);
    CHECK(look.from && look.from->lookTarget == Types::ActorLookTarget::Point);
    CHECK(!look.to);
    CHECK_NEAR(look.turned, 1.0, 1e-6);
}

namespace
{
    float AngleBetween(const glm::quat& a, const glm::quat& b)
    {
        const auto dot = std::abs(glm::dot(glm::normalize(a), glm::normalize(b)));
        return glm::degrees(2.0f * std::acos(std::min(dot, 1.0f)));
    }
}  // namespace

TEST("a look turns the head from where the clip has it, not from its rest")
{
    // pb_prone_aim_ads holds the head well up from the neck, the way a prone actor looks ahead; looking at something
    // above starts there
    const auto clip = glm::angleAxis(glm::radians(-60.0f), glm::vec3(0, 1, 0));
    const auto target = glm::angleAxis(glm::radians(-95.0f), glm::vec3(0, 1, 0)) *
                        glm::angleAxis(glm::radians(30.0f), glm::vec3(0, 0, 1));

    // before the turn starts the head is the clip's own
    CHECK(!ActorLogic::GetHeadTurn(std::nullopt, target, 0.0f));
    CHECK(!ActorLogic::GetHeadTurn(std::nullopt, std::nullopt, 0.5f));

    // its first frames are next to the clip's head, then it turns at an even pace and ends at the target
    auto turn = ActorLogic::GetHeadTurn(std::nullopt, target, 0.01f);
    CHECK(turn.has_value());
    CHECK(AngleBetween(ActorLogic::ApplyHeadTurn(clip, *turn), clip) < 0.02f * AngleBetween(clip, target));
    turn = ActorLogic::GetHeadTurn(std::nullopt, target, 0.5f);
    CHECK_NEAR(AngleBetween(ActorLogic::ApplyHeadTurn(clip, *turn), clip), AngleBetween(clip, target) / 2.0f, 0.05f);
    turn = ActorLogic::GetHeadTurn(std::nullopt, target, 1.0f);
    CHECK(AngleBetween(ActorLogic::ApplyHeadTurn(clip, *turn), target) < 0.05f);

    // no step of a 400 ms turn, a frame of 16 ms each, goes further than an even pace does, and the head follows the
    // clip as it moves on
    float largest = 0.0f;
    std::optional<glm::quat> previous;
    for (int frame = 0; frame <= 25; ++frame)
    {
        const auto moving = glm::angleAxis(glm::radians(0.4f * frame), glm::vec3(0, 0, 1)) * clip;
        const auto now = ActorLogic::GetHeadTurn(std::nullopt, target, frame / 25.0f);
        const auto head = now ? ActorLogic::ApplyHeadTurn(moving, *now) : moving;
        if (previous)
            largest = std::max(largest, AngleBetween(head, *previous));
        previous = head;
    }
    CHECK(largest < AngleBetween(clip, target) / 25.0f + 1.0f);
}

TEST("looking ahead turns the head back to the clip's own, and between two looks the clip doesn't count")
{
    const auto clip = glm::angleAxis(glm::radians(-60.0f), glm::vec3(0, 1, 0));
    const auto left = glm::angleAxis(glm::radians(50.0f), glm::vec3(0, 0, 1));
    const auto right = glm::angleAxis(glm::radians(-50.0f), glm::vec3(0, 0, 1));

    // halfway back from a look the head is halfway to the clip's, and once turned it is the clip's own (it used to
    // stay at its rest)
    auto turn = ActorLogic::GetHeadTurn(left, std::nullopt, 0.5f);
    CHECK(turn.has_value());
    CHECK_NEAR(AngleBetween(ActorLogic::ApplyHeadTurn(clip, *turn), clip), AngleBetween(clip, left) / 2.0f, 0.05f);
    CHECK(!ActorLogic::GetHeadTurn(left, std::nullopt, 1.0f));

    // from one look to another
    turn = ActorLogic::GetHeadTurn(left, right, 0.5f);
    CHECK(turn.has_value());
    CHECK_NEAR(turn->weight, 1.0, 1e-6);
    CHECK(AngleBetween(ActorLogic::ApplyHeadTurn(clip, *turn), glm::quat(1, 0, 0, 0)) < 0.05f);

    // the game's animations leave their rotations longer or shorter than 1, which the head keeps
    turn = ActorLogic::GetHeadTurn(std::nullopt, left, 1.0f);
    CHECK_NEAR(glm::length(ActorLogic::ApplyHeadTurn(clip * 0.8f, *turn)), 0.8, 1e-5);
}

namespace
{
    // as the game has them (fxlist on intro_cinematic): a large fire, steam that loops till it is stopped, a grenade's
    // explosion (sparks for 35 ms)
    constexpr Types::ActorCatalog::EffectInfo FIRE{.loopingLife = 12000, .loopingElements = 5};
    constexpr Types::ActorCatalog::EffectInfo STEAM{.loopingLife = Types::ActorCatalog::EffectInfo::FOREVER,
                                                    .loopingElements = 2};
    constexpr Types::ActorCatalog::EffectInfo GRENADE{.loopingLife = 35, .loopingElements = 1, .oneShotElements = 14};
}  // namespace

TEST("a scene effect faces out of the surface it is put on, its bar as long as it plays by itself")
{
    auto actor = ActorLogic::MakeEffect("destructibles/fx_dest_fire_lg", FIRE, {100, 200, 0}, {0, 0, 1}, 20000);
    CHECK(actor.effect.has_value());
    CHECK_NEAR(actor.effect->pitch, -90.0, 1e-3);
    CHECK_NEAR(actor.position.z, 1.0, 1e-4);
    CHECK(actor.effect->start == 20000 && actor.effect->end == 32000);
    CHECK(ActorLogic::GetEffectBaseName(actor.effect->name) == "Fire lg");

    // a wall facing west: the effect fires west, level
    actor = ActorLogic::MakeEffect("maps/mp_maps/fx_mp_smk_vent", STEAM, {0, 0, 50}, {-1, 0, 0}, 1000);
    CHECK_NEAR(actor.effect->pitch, 0.0, 1e-3);
    CHECK_NEAR(std::abs(actor.yaw), 180.0, 1e-3);
    CHECK(actor.effect->end == 1000 + ActorLogic::EFFECT_STOPPED_LENGTH);
    CHECK(ActorLogic::GetEffectBaseName(actor.effect->name) == "Smk vent");

    actor = ActorLogic::MakeEffect("explosions/fx_grenadeexp_default", GRENADE, {}, {0, 0, 1}, 0);
    CHECK(!ActorLogic::EffectLoops(GRENADE));
    CHECK(actor.effect->end == ActorLogic::EFFECT_ONCE_LENGTH);
}

TEST("a scene effect plays as made, once, or again as it ends till its bar's end")
{
    auto actor = ActorLogic::MakeEffect("fire", FIRE, {}, {0, 0, 1}, 10000);
    // Auto, a fire that loops for 12 s on a bar of 30 s: again as its looping life ends, each kept and faded
    actor.effect->end = 40000;
    auto plays = ActorLogic::GetEffectPlays(actor, FIRE);
    CHECK(plays.size() == 3);
    CHECK(plays[0] == (ActorLogic::EffectPlay{10000, 22000, true, false}));
    CHECK(plays[2] == (ActorLogic::EffectPlay{34000, 40000, true, false}));
    CHECK(ActorLogic::GetEffectPlayedEnd(*actor.effect, FIRE) == 40000);

    // Once: it burns its 12 s and is done, the rest of the bar unplayed; Cut ends it at once only at the bar's end
    actor.effect->plays = Types::EffectPlays::Once;
    actor.effect->ends = Types::EffectEnds::Cut;
    plays = ActorLogic::GetEffectPlays(actor, FIRE);
    CHECK(plays.size() == 1 && plays[0] == (ActorLogic::EffectPlay{10000, 22000, true, false}));
    CHECK(ActorLogic::GetEffectPlayedEnd(*actor.effect, FIRE) == 22000);
    actor.effect->end = 15000;
    CHECK(ActorLogic::GetEffectPlays(actor, FIRE)[0] == (ActorLogic::EffectPlay{10000, 15000, true, true}));

    // steam loops till it is stopped: one play over the whole bar
    actor = ActorLogic::MakeEffect("steam", STEAM, {}, {0, 0, 1}, 0);
    actor.effect->end = 30000;
    plays = ActorLogic::GetEffectPlays(actor, STEAM);
    CHECK(plays.size() == 1 && plays[0].end == 30000 && plays[0].kept);

    // a grenade plays once and is let go; looped, again every 2.5 s, each let go
    actor = ActorLogic::MakeEffect("grenade", GRENADE, {}, {0, 0, 1}, 1000);
    plays = ActorLogic::GetEffectPlays(actor, GRENADE);
    CHECK(plays.size() == 1 && !plays[0].kept && plays[0].end == 1000 + ActorLogic::EFFECT_ONCE_LENGTH);
    actor.effect->plays = Types::EffectPlays::Loop;
    actor.effect->every = 2500;
    actor.effect->end = 9000;
    plays = ActorLogic::GetEffectPlays(actor, GRENADE);
    CHECK(plays.size() == 4);
    CHECK(plays[3].begin == 8500 && !plays[3].kept);

    // what is on at a tick: before the bar nothing, inside it the play it is in, past it nothing
    CHECK(ActorLogic::GetEffectPlays(actor, GRENADE, 500).empty());
    CHECK(ActorLogic::GetEffectPlays(actor, GRENADE, 4000).size() == 1);
    CHECK(ActorLogic::GetEffectPlays(actor, GRENADE, 20000).empty());
}

TEST("the effect browser sorts effects by their folders and says how each plays")
{
    CHECK(ActorLogic::GetEffectCategory("maps/mp_maps/fx_mp_smk_vent") == "The map's own");
    CHECK(ActorLogic::GetEffectCategory("destructibles/fx_dest_fire_lg") == "Destructibles");
    CHECK(ActorLogic::GetEffectCategory("system_elements/fx_null") == "Other");
    CHECK(ActorLogic::GetEffectCategory("fx_without_folder") == "Other");
    for (const auto name : {"env/fire/a", "explosions/a", "impacts/a", "vehicle/a", "weapon/a", "bio/a", "trail/a"})
        CHECK(std::ranges::find(ActorLogic::GetEffectCategories(), ActorLogic::GetEffectCategory(name)) !=
              ActorLogic::GetEffectCategories().end());
    CHECK(ActorLogic::DescribeEffect(FIRE) == "Loops for 12.0 s, then ends by itself");
    CHECK(ActorLogic::DescribeEffect(STEAM) == "Loops till it is stopped");
    CHECK(ActorLogic::DescribeEffect(GRENADE) == "Plays once");
}

TEST("a scene effect is saved with the scene and read back")
{
    ActorLogic::Scene scene;
    auto actor = ActorLogic::MakeEffect("destructibles/fx_dest_fire_lg", FIRE, {1, 2, 3}, {0, 1, 0}, 5000);
    actor.id = 7;
    actor.name = "Fire lg 1";
    actor.effect->plays = Types::EffectPlays::Loop;
    actor.effect->ends = Types::EffectEnds::Cut;
    actor.effect->roll = 15.0f;
    actor.effect->every = 3000;
    scene.actors.push_back(actor);
    const auto read = ActorLogic::SceneFromJson(nlohmann::json::parse(ActorLogic::SceneToJson(scene, "demo").dump()),
                                                99999);
    CHECK(read.actors.size() == 1);
    CHECK(read.actors[0].effect == actor.effect);
    CHECK(read.actors[0].position == actor.position);
    CHECK_NEAR(read.actors[0].yaw, 90.0, 1e-3);
    CHECK(read.actors[0].models.empty());
}

TEST("a scene from before clips gets one clip to the end of the demo")
{
    const auto root = nlohmann::json::parse(R"({"actors": [{"name": "Old", "models": ["body"],
        "animation": "walk", "animationStart": 1000, "animationSpeed": 2.0}]})");
    const auto scene = ActorLogic::SceneFromJson(root, 5000);
    CHECK(scene.actors.size() == 1);
    CHECK(scene.actors[0].clips.size() == 1);
    CHECK(scene.actors[0].clips[0].start == 1000);
    CHECK(scene.actors[0].clips[0].End() == 5000);
    CHECK_NEAR(scene.actors[0].clips[0].speed, 2.0, 1e-6);
}

TEST("a duplicated clip starts where it ends and a duplicated event after its last shot")
{
    Types::Actor actor = ActorWith({Clip("walk", 1000, 2000), Clip("idle", 5000, 1000)});
    actor.events = {{.type = Types::ActorEventType::Fire, .tick = 500, .shots = 3, .interval = 100}};
    ActorLogic::Normalize(actor);

    const auto clip = ActorLogic::DuplicateClip(actor, actor.clips[0].id);
    CHECK(actor.clips.size() == 3);
    const auto copy = std::ranges::find(actor.clips, clip, &Types::ActorClip::id);
    CHECK(copy->start == 3000);
    CHECK(copy->animation == "walk");
    CHECK(copy->length == 2000);

    const auto event = ActorLogic::DuplicateEvent(actor, actor.events[0].id);
    CHECK(actor.events.size() == 2);
    CHECK(std::ranges::find(actor.events, event, &Types::ActorEvent::id)->tick == 1200);
    CHECK(ActorLogic::DuplicateClip(actor, 999) == 0);
    CHECK(ActorLogic::DuplicateEvent(actor, 999) == 0);
}

TEST("a hit and its death are one: linked by the clip's id, moving either moves both")
{
    Types::Actor actor = ActorWith({Clip("walk", 0, 2000), Clip("death", 2000, 1500, 100, false)});
    actor.events = {{.type = Types::ActorEventType::Hit, .tick = 2000},
                    {.type = Types::ActorEventType::Fire, .tick = 2000}};
    ActorLogic::Normalize(actor);
    const auto hitId = std::ranges::find(actor.events, Types::ActorEventType::Hit, &Types::ActorEvent::type)->id;
    const auto hit = [&]() -> Types::ActorEvent& {
        return *std::ranges::find(actor.events, hitId, &Types::ActorEvent::id);
    };
    const auto& fire = *std::ranges::find(actor.events, Types::ActorEventType::Fire, &Types::ActorEvent::type);
    // (sharing a tick doesn't link them: scenes from before links are linked that way as they are read)
    CHECK(ActorLogic::FindDeathClip(actor, hit()) == nullptr);
    ActorLogic::LinkDeaths(actor);
    CHECK(ActorLogic::FindDeathClip(actor, hit()) != nullptr);
    CHECK(ActorLogic::FindDeathClip(actor, hit())->animation == "death");
    CHECK(ActorLogic::FindDeathClip(actor, fire) == nullptr);

    ActorLogic::MoveEvent(actor, hitId, 2500);
    CHECK(hit().tick == 2500);
    CHECK(actor.clips[1].animation == "death");
    CHECK(actor.clips[1].start == 2500);
    // the walk is left as it was
    CHECK(actor.clips[0].start == 0);
    CHECK(actor.clips[0].length == 2000);

    // the death dragged on the timeline: its Hit goes with it, still linked
    actor.clips[1].start = 2200;
    ActorLogic::Normalize(actor);
    CHECK(hit().tick == 2200 && ActorLogic::FindDeathClip(actor, hit()) == &actor.clips[1]);
    // the clips after a walk moved on (as a re-plan does): the same
    for (auto& clip : actor.clips)
        if (clip.start >= 2000)
            clip.start += 700;
    ActorLogic::Normalize(actor);
    CHECK(hit().tick == 2900 && ActorLogic::FindDeathClip(actor, hit()));
    // a copy of the Hit dies of nothing
    const auto copy = ActorLogic::DuplicateEvent(actor, hitId);
    const auto& copied = *std::ranges::find(actor.events, copy, &Types::ActorEvent::id);
    CHECK(copied.tick == 3400 && !ActorLogic::FindDeathClip(actor, copied));
    // the death taken out (Death None): the Hit stays where it was, dying of nothing
    std::erase_if(actor.clips, [](const Types::ActorClip& clip) { return clip.animation == "death"; });
    ActorLogic::Normalize(actor);
    CHECK(hit().tick == 2900 && hit().death == 0 && !ActorLogic::FindDeathClip(actor, hit()));

    // a looping clip there isn't a death
    Types::Actor looping = ActorWith({Clip("walk", 0, 2000), Clip("idle", 2000, 1500)});
    looping.events = {{.type = Types::ActorEventType::Hit, .tick = 2000}};
    ActorLogic::Normalize(looping);
    ActorLogic::LinkDeaths(looping);
    CHECK(ActorLogic::FindDeathClip(looping, looping.events[0]) == nullptr);
    ActorLogic::MoveEvent(looping, looping.events[0].id, 100);
    CHECK(looping.clips[1].start == 2000);
}

TEST("a hit's death is kept in scenes by the clip's place; scenes from before link the clip starting with the hit")
{
    ActorLogic::Scene scene;
    auto actor = ActorWith({Clip("walk", 0, 2000), Clip("idle", 2000, 500), Clip("death", 2500, 1500, 100, false)});
    actor.id = 1;
    ActorLogic::AddDeath(actor, 2500, "death2", 1.5f);
    // (a plain Hit as well, at the start of a clip that doesn't loop)
    actor.clips.push_back(Clip("stumble", 1000, 300, 100, false));
    ActorLogic::Normalize(actor);
    ActorLogic::AddEvent(actor, {.type = Types::ActorEventType::Hit, .tick = 1000});
    scene.actors.push_back(actor);
    auto read = ActorLogic::SceneFromJson(ActorLogic::SceneToJson(scene, "demo"), 60000);
    const auto deathOf = [](const Types::Actor& each, std::uint32_t tick) -> std::string {
        const auto hit = std::ranges::find(each.events, tick, &Types::ActorEvent::tick);
        const auto* clip = hit != each.events.end() ? ActorLogic::FindDeathClip(each, *hit) : nullptr;
        return clip ? clip->animation : "";
    };
    CHECK(deathOf(read.actors[0], 2500) == "death2");
    CHECK(deathOf(read.actors[0], 1000).empty());
    // from before: none written, so the clips that start with them
    auto json = ActorLogic::SceneToJson(scene, "demo");
    for (auto& event : json["actors"][0]["events"])
        event.erase("death");
    read = ActorLogic::SceneFromJson(json, 60000);
    CHECK(deathOf(read.actors[0], 2500) == "death2");
    CHECK(deathOf(read.actors[0], 1000) == "stumble");
}

TEST("+ Clip adds the selected or last clip's animation, never a death: the idle instead")
{
    const auto animations = [](const std::string& name) -> std::optional<ActorLogic::AnimationInfo> {
        ActorLogic::AnimationInfo info{1.0f};
        if (name == "run_death_roll")
            info.death = true;
        return info;
    };
    auto actor = ActorWith({Clip("walk", 0, 2000), Clip("reload", 2000, 1500, 100, false)});
    CHECK(ActorLogic::GetAddedClipAnimation(actor, std::nullopt, "idle", animations) == "reload");
    CHECK(ActorLogic::GetAddedClipAnimation(actor, actor.clips[0].id, "idle", animations) == "walk");
    ActorLogic::AddDeath(actor, 3000, "run_death_roll", 1.5f);
    CHECK(ActorLogic::GetAddedClipAnimation(actor, std::nullopt, "idle", animations) == "idle");
    CHECK(ActorLogic::GetAddedClipAnimation(actor, actor.clips.back().id, "idle", animations) == "idle");
    // (named so, before the catalog says)
    actor.clips.back().animation = "pb_stand_death_headchest_topple";
    CHECK(ActorLogic::GetAddedClipAnimation(actor, std::nullopt, "idle", [](const std::string&) {
              return std::optional<ActorLogic::AnimationInfo>();
          }) == "idle");
    CHECK(ActorLogic::GetAddedClipAnimation(ActorWith({}), std::nullopt, "idle", animations) == "idle");
}

TEST("trimming a clip's start keeps its animation where it was on the timeline")
{
    auto clip = Clip("walk", 2000, 3000);
    clip.speed = 2.0f;
    clip.offset = 0.5f;
    // half a second later: a second into the animation at 2x
    const auto later = ActorLogic::TrimClipStart(clip, 2500, 100);
    CHECK(later.start == 2500);
    CHECK(later.End() == 5000);
    CHECK_NEAR(later.offset, 1.5, 1e-5);
    // earlier, back to the animation's start and no further back in it
    const auto earlier = ActorLogic::TrimClipStart(clip, 1000, 100);
    CHECK(earlier.start == 1000);
    CHECK(earlier.End() == 5000);
    CHECK_NEAR(earlier.offset, 0.0, 1e-5);
    // not past its end, nor before 0
    CHECK(ActorLogic::TrimClipStart(clip, 9000, 100).start == 4900);
    CHECK(ActorLogic::TrimClipStart(clip, -500, 100).start == 0);

    // the end alone
    const auto shorter = ActorLogic::TrimClipEnd(clip, 3000, 100);
    CHECK(shorter.start == 2000);
    CHECK(shorter.length == 1000);
    CHECK_NEAR(shorter.offset, 0.5, 1e-5);
    CHECK(ActorLogic::TrimClipEnd(clip, 1000, 100).length == 100);
}

TEST("a path point is inserted halfway to the next or past the end")
{
    Types::Actor actor;
    actor.position = {5, 6, 7};
    CHECK(ActorLogic::InsertWaypoint(actor, 0) == 0);
    CHECK(actor.path.size() == 1);
    CHECK(actor.path[0] == glm::vec3(5, 6, 7));

    actor.path = {{0, 0, 0}, {100, 0, 20}};
    CHECK(ActorLogic::InsertWaypoint(actor, 0) == 1);
    CHECK(actor.path.size() == 3);
    CHECK(actor.path[1] == glm::vec3(50, 0, 10));

    // past the end, along the last stretch (level)
    CHECK(ActorLogic::InsertWaypoint(actor, 2) == 3);
    CHECK_NEAR(actor.path[3].x, 164.0, 1e-3);
    CHECK_NEAR(actor.path[3].y, 0.0, 1e-3);
    CHECK_NEAR(actor.path[3].z, 20.0, 1e-3);
    // an index past the end counts as the last point
    CHECK(ActorLogic::InsertWaypoint(actor, 99) == 4);
    CHECK(actor.path.size() == 5);
}

TEST("undo and redo go back and forth over the steps")
{
    ActorLogic::History history;
    auto named = [](std::string name) {
        Types::Actor actor;
        actor.name = std::move(name);
        return std::vector<Types::Actor>{actor};
    };
    CHECK(!history.CanUndo());
    CHECK(!history.Undo(named("a")));

    // a, then a drag (three changes in one step) to c, then d; only a change that starts a step says so (the editor's
    // undo history gets one step for it)
    CHECK(history.Record(named("a")));
    history.EndStep();
    CHECK(history.Record(named("b")));
    CHECK(!history.Record(named("b2")));
    CHECK(!history.Record(named("b3")));
    history.EndStep();
    history.Record(named("c"));
    history.EndStep();
    const auto current = named("d");

    auto back = history.Undo(current);
    CHECK(back && (*back)[0].name == "c");
    back = history.Undo(*back);
    CHECK(back && (*back)[0].name == "b");
    CHECK(history.CanRedo());
    auto forward = history.Redo(*back);
    CHECK(forward && (*forward)[0].name == "c");
    forward = history.Redo(*forward);
    CHECK(forward && (*forward)[0].name == "d");
    CHECK(!history.CanRedo());

    // a new change drops what could be redone
    back = history.Undo(*forward);
    history.Record(*back);
    CHECK(!history.CanRedo());

    // the game's part of a step joins it even after it ended
    ActorLogic::History placing;
    placing.Record(named("before add"));
    placing.EndStep();
    placing.Record(named("added"), true);
    back = placing.Undo(named("placed"));
    CHECK(back && (*back)[0].name == "before add");
    CHECK(!placing.CanUndo());

    history.Clear();
    CHECK(!history.CanUndo());
    CHECK(!history.CanRedo());
}

TEST("a look and clips are kept as presets and applied to another actor")
{
    Types::Actor actor = ActorWith({Clip("walk", 1000, 2000), Clip("death", 3000, 1500, 100, false)});
    actor.models = {"body", "head"};
    actor.weapon = "ak47_mp";
    actor.attachments = {"silencer", "acog"};
    actor.camo = 3;
    actor.props = {{.model = "radio", .tag = "j_head", .offset = {1, 2, 3}}};
    const auto look = ActorLogic::LookToJson(actor);
    const auto clips = ActorLogic::ClipsToJson(actor);

    Types::Actor other = ActorWith({Clip("idle", 0, 10000)});
    other.models = {"someone"};
    ActorLogic::ApplyLook(other, nlohmann::json::parse(look.dump()));
    CHECK(other.models == actor.models);
    CHECK(other.weapon == "ak47_mp");
    CHECK((other.attachments == std::vector<std::string>{"acog", "silencer"}));
    CHECK(other.camo == 3);
    CHECK(other.props == actor.props);

    // from the playhead on, the idle cut there
    ActorLogic::ApplyClips(other, nlohmann::json::parse(clips.dump()), 5000);
    CHECK(other.clips.size() == 3);
    CHECK(other.clips[0].animation == "idle");
    CHECK(other.clips[0].length == 5000);
    CHECK(other.clips[1].animation == "walk");
    CHECK(other.clips[1].start == 5000);
    CHECK(other.clips[2].animation == "death");
    CHECK(other.clips[2].start == 7000);
    CHECK(!other.clips[2].loop);
}

TEST("clips presets keep the Face row's clips, and presets of before it still apply")
{
    Types::Actor actor = ActorWith({Clip("walk", 1000, 2000)});
    actor.faceClips = {Clip("pf_pain", 500, 1100, 100, false)};
    ActorLogic::Normalize(actor);
    const auto clips = nlohmann::json::parse(ActorLogic::ClipsToJson(actor).dump());

    // from the first start of either row
    Types::Actor other = ActorWith({});
    ActorLogic::ApplyClips(other, clips, 5000);
    CHECK(other.clips.size() == 1 && other.clips[0].animation == "walk" && other.clips[0].start == 5500);
    CHECK(other.faceClips.size() == 1 && other.faceClips[0].animation == "pf_pain" && other.faceClips[0].start == 5000);
    CHECK(other.faceClips.size() == 1 && other.faceClips[0].length == 1100 && !other.faceClips[0].loop);

    // a preset kept as the list of clips alone
    Types::Actor old = ActorWith({});
    ActorLogic::ApplyClips(old, clips.at("clips"), 2000);
    CHECK(old.clips.size() == 1 && old.clips[0].start == 2500 && old.faceClips.empty());
}

TEST("a formation puts copies in rows to the right and behind, later by the stagger")
{
    Types::Actor actor = ActorWith({Clip("walk", 1000, 2000)});
    actor.position = {100, 0, 0};
    actor.events = {{.type = Types::ActorEventType::Fire, .tick = 1500}};
    ActorLogic::Normalize(actor);
    // facing along y: its right is +x, behind it is -y
    const auto copies = ActorLogic::MakeFormation(actor, {.count = 4, .columns = 2, .spacing = {50, 60}, .stagger = 100},
                                                  90.0f);
    CHECK(copies.size() == 3);
    CHECK_NEAR(copies[0].position.x, 150.0, 1e-3);
    CHECK_NEAR(copies[0].position.y, 0.0, 1e-3);
    CHECK_NEAR(copies[1].position.x, 100.0, 1e-3);
    CHECK_NEAR(copies[1].position.y, -60.0, 1e-3);
    CHECK_NEAR(copies[2].position.x, 150.0, 1e-3);
    CHECK_NEAR(copies[2].position.y, -60.0, 1e-3);
    CHECK(copies[0].clips[0].start == 1100);
    CHECK(copies[2].clips[0].start == 1300);
    CHECK(copies[2].events[0].tick == 1800);
    CHECK(copies[0].id == 0);

    // attached, along the bone's axes: facing the bone's way, the right is -y
    Types::Actor attached = actor;
    attached.movement = Types::ActorMovement::Attached;
    const auto riders = ActorLogic::MakeFormation(attached, {.count = 2, .columns = 2, .spacing = {50, 60}}, 0.0f);
    CHECK_NEAR(riders[0].attachment.offset.x, 0.0, 1e-3);
    CHECK_NEAR(riders[0].attachment.offset.y, -50.0, 1e-3);
}

TEST("models too many bones together leave out the head, then what is worn, never the body")
{
    // S.O.G.'s Bowman (a full body, face and all) with the Huey pilot's head and helmet: 115 + 54 + 1 = 170 of 160
    const std::vector<std::uint32_t> bowman{115, 54, 1};
    CHECK((ActorLogic::FitBones(bowman, 160) == std::vector<bool>{true, false, true}));
    // a gun after them still fits once the head is out
    const std::vector<std::uint32_t> armed{115, 54, 1, 30};
    CHECK((ActorLogic::FitBones(armed, 160) == std::vector<bool>{true, false, true, true}));
    // exactly the limit is drawn
    const std::vector<std::uint32_t> full{100, 54, 6};
    CHECK((ActorLogic::FitBones(full, 160) == std::vector<bool>{true, true, true}));
    // a body over the limit alone isn't drawn at all, nor what goes on it
    const std::vector<std::uint32_t> huge{161, 1};
    CHECK((ActorLogic::FitBones(huge, 160) == std::vector<bool>{false, false}));
    // no limit
    CHECK((ActorLogic::FitBones(bowman, 0) == std::vector<bool>{true, true, true}));
    CHECK(ActorLogic::FitBones({}, 160).empty());
}

TEST("a starting point sets up an actor's clips, movement and events at once")
{
    const ActorLogic::StartAnimations animations{
        .idle = "idle", .walk = "walk", .run = "back", .shoot = "burst", .death = "death", .deathDuration = 1.5f};
    using ActorLogic::Start;

    // standing: idling from the playhead to the end of the timeline
    const auto standing = ActorLogic::MakeStart(Start::Standing, animations, 2000, 20000, "ak47_mp");
    CHECK(standing.movement == Types::ActorMovement::Still);
    CHECK(standing.clips.size() == 1);
    CHECK(standing.clips[0].animation == "idle" && standing.clips[0].start == 2000 && standing.clips[0].End() == 20000);
    CHECK(standing.weapon.empty());
    CHECK(standing.events.empty());

    // a path of one point to place, walked (or run) for a moment and then stood at, until it is fitted to the path
    const auto walking = ActorLogic::MakeStart(Start::WalkPath, animations, 2000, 20000, "");
    CHECK(walking.movement == Types::ActorMovement::Path);
    CHECK(walking.path.size() == 1);
    CHECK(walking.clips.size() == 2);
    CHECK(walking.clips[0].animation == "walk" && walking.clips[0].start == 2000 && walking.clips[0].moves);
    CHECK(walking.clips[0].End() == 2000 + ActorLogic::START_WALK_LENGTH);
    CHECK(walking.clips[1].animation == "idle" && walking.clips[1].start == walking.clips[0].End());
    CHECK(walking.clips[1].End() == 20000);
    const auto running = ActorLogic::MakeStart(Start::RunPath, animations, 2000, 20000, "");
    CHECK(running.clips[0].animation == "back");

    // shooting: the gun given, aimed, and a burst as an event to move and change, not as the animation shoots
    const auto shooting = ActorLogic::MakeStart(Start::Shoot, animations, 2000, 20000, "ak47_mp");
    CHECK(shooting.weapon == "ak47_mp");
    CHECK(shooting.clips.size() == 1 && shooting.clips[0].animation == "burst" && !shooting.clips[0].moves);
    CHECK(!shooting.clips[0].notetrackShots);
    CHECK(shooting.events.size() == 1);
    CHECK(shooting.events[0].type == Types::ActorEventType::Fire && shooting.events[0].tick == 2000);
    CHECK(shooting.events[0].shots == 10);

    // dying: standing, then a hit a moment later that the death clip starts with
    const auto dying = ActorLogic::MakeStart(Start::Die, animations, 2000, 20000, "");
    CHECK(dying.clips.size() == 2);
    CHECK(dying.clips[0].animation == "idle" && dying.clips[0].End() == 2000 + ActorLogic::DIE_AFTER);
    CHECK(dying.events.size() == 1);
    const auto& hit = dying.events[0];
    CHECK(hit.type == Types::ActorEventType::Hit && hit.tick == 2000 + ActorLogic::DIE_AFTER);
    CHECK(hit.effect == ActorLogic::FATAL_BLOOD);
    const auto death = ActorLogic::FindDeathClip(dying, hit);
    CHECK(death && death->animation == "death" && death->length == 1500 && !death->loop && !death->moves);

    // what the game doesn't have is left out
    const auto noWalk = ActorLogic::MakeStart(Start::WalkPath, {.idle = "idle"}, 2000, 20000, "");
    CHECK(noWalk.clips.size() == 1 && noWalk.clips[0].animation == "idle");

    // the first animation of a list the game has
    const auto has = [](std::string_view name) { return name == "pb_stand_alert" || name == "casual_stand_idle"; };
    CHECK(ActorLogic::FindFirst(ActorLogic::IDLE_ANIMATIONS, has) == "casual_stand_idle");
    CHECK(ActorLogic::FindFirst(ActorLogic::WALK_ANIMATIONS, has).empty());
}

TEST("an animation played at the playhead goes in among the clips")
{
    // a one-off: then what was shown goes on, its animation where it would have been
    auto actor = ActorWith({Clip("idle", 0, 10000)});
    ActorLogic::PlayAt(actor, 5000, "reload", 2.0f, false);
    CHECK(actor.clips.size() == 3);
    CHECK(actor.clips[0].animation == "idle" && actor.clips[0].End() == 5000);
    CHECK(actor.clips[1].animation == "reload" && actor.clips[1].start == 5000 && actor.clips[1].length == 2000);
    CHECK(!actor.clips[1].loop);
    CHECK(actor.clips[2].animation == "idle" && actor.clips[2].start == 7000 && actor.clips[2].End() == 10000);
    CHECK_NEAR(actor.clips[2].offset, 7.0, 1e-4);

    // a loop: one play of it too, looping (a longer clip plays it again), as picked; then what was shown goes on
    const auto walk = ActorLogic::PlayAt(actor, 3000, "walk", 1.0f, true);
    CHECK(actor.clips.size() == 5);
    CHECK(actor.clips[0].End() == 3000);
    const auto walking = std::ranges::find(actor.clips, walk, &Types::ActorClip::id);
    CHECK(walking != actor.clips.end() && walking->start == 3000 && walking->End() == 4000 && walking->loop &&
          walking->picked);
    CHECK(actor.clips[2].animation == "idle" && actor.clips[2].start == 4000 && actor.clips[2].End() == 5000);

    // where a clip starts, in its place
    ActorLogic::PlayAt(actor, 5000, "burst", 2.0f, false);
    CHECK(actor.clips.size() == 5);
    CHECK(actor.clips[3].animation == "burst" && actor.clips[3].start == 5000);
    CHECK(actor.clips[4].animation == "idle" && actor.clips[4].start == 7000);

    // with no clips, one play; one play up to the next clip at most; the blend given
    Types::Actor empty;
    ActorLogic::PlayAt(empty, 1000, "idle", 1.0f, true);
    CHECK(empty.clips.size() == 1 && empty.clips[0].start == 1000 && empty.clips[0].length == 1000);
    auto shortly = ActorWith({Clip("idle", 0, 2000), Clip("aim", 2000, 3000)});
    const auto cut = ActorLogic::PlayAt(shortly, 1500, "reload", 2.0f, false, {}, 180u);
    const auto reload = std::ranges::find(shortly.clips, cut, &Types::ActorClip::id);
    CHECK(reload != shortly.clips.end() && reload->End() == 2000 && reload->blendIn == 180);

    // a death: the hit and the clip it starts, which holds its last frame
    auto victim = ActorWith({Clip("idle", 0, 10000)});
    const auto hitId = ActorLogic::AddDeath(victim, 4000, "death", 1.5f);
    CHECK(victim.clips.size() == 2 && victim.clips[0].End() == 4000);
    const auto hit = std::ranges::find(victim.events, hitId, &Types::ActorEvent::id);
    CHECK(hit != victim.events.end() && hit->tick == 4000 && hit->type == Types::ActorEventType::Hit);
    const auto death = ActorLogic::FindDeathClip(victim, *hit);
    CHECK(death && death->start == 4000 && death->length == 1500 && death->blendIn == 100);
}

TEST("deaths are told from their notetracks and names")
{
    const std::vector<Types::ActorNotetrack> ragdoll = {{"start_ragdoll", 0.3f}, {"bodyfall large", 0.5f}};
    const std::vector<Types::ActorNotetrack> landing = {{"bodyfall small", 0.4f}, {"end", 1.0f}};
    CHECK(ActorLogic::IsDeathAnimation("ai_death_fallforward", false, ragdoll));
    CHECK(ActorLogic::IsDeathAnimation("ch_khe_e1c_mortarexplosion_guy01", false, ragdoll));
    CHECK(ActorLogic::IsDeathAnimation("pb_stand_death_headchest_topple", false, {}));
    CHECK(ActorLogic::IsDeathAnimation("corner_standl_deatha", false, {}));
    CHECK(ActorLogic::IsDeathAnimation("ai_flame_death_run_die", false, {}));
    CHECK(ActorLogic::IsDeathAnimation("dying_crawl", false, landing));
    // landing a jump, a soldier's, the undead's, and one that loops
    CHECK(!ActorLogic::IsDeathAnimation("ai_jump_down_40", false, landing));
    CHECK(!ActorLogic::IsDeathAnimation("ch_khe_e1b_soldiershelpwounded_guy01", false, {}));
    CHECK(!ActorLogic::IsDeathAnimation("ai_zombie_undead_walk", false, {}));
    CHECK(!ActorLogic::IsDeathAnimation("pb_death_loop", true, ragdoll));
}

TEST("a death played at the playhead holds its last frame")
{
    // the view menu's Play of a death: the idle cut, the death, nothing after it; it lies there as the idle would
    // have gone on
    const std::vector<Types::ActorNotetrack> ragdoll = {{"start_ragdoll", 0.325f}, {"bodyfall large", 0.55f}};
    auto actor = ActorWith({Clip("idle", 0, 10000)});
    const auto death = ActorLogic::PlayAt(actor, 5000, "ai_death_fallforward", 1.333f, false, ragdoll);
    CHECK(actor.clips.size() == 2);
    CHECK(actor.clips[0].animation == "idle" && actor.clips[0].End() == 5000);
    CHECK(actor.clips[1].id == death && actor.clips[1].animation == "ai_death_fallforward");
    CHECK(actor.clips[1].start == 5000 && actor.clips[1].length == 1333 && !actor.clips[1].loop);
    CHECK(!actor.clips[1].moves && actor.clips[1].blendIn == 100);
    // (with its Hit and blood, as every death)
    CHECK(actor.events.size() == 1 && actor.events[0].death == death &&
          actor.events[0].effect == ActorLogic::FATAL_BLOOD && actor.events[0].tick == 5000);
    for (const auto tick : {6400u, 8000u, 9999u, 20000u})
    {
        const auto samples = ActorLogic::Evaluate(actor, tick);
        CHECK(samples.size() == 1 && samples[0].clip->id == death);
        CHECK_NEAR(samples[0].time, 1.333, 1e-4);
    }
    CHECK(ActorLogic::GetClipAfter(actor, death) == ActorLogic::ClipAfter::Hold);

    // named so, without notetracks
    auto named = ActorWith({Clip("idle", 0, 10000)});
    ActorLogic::PlayAt(named, 5000, "pb_stand_death_headchest_topple", 2.0f, false);
    CHECK(named.clips.size() == 2);

    // a flinch, and a jump that lands with a bodyfall, still give what was shown back after one play
    auto flinching = ActorWith({Clip("idle", 0, 10000)});
    const auto flinch = ActorLogic::PlayAt(flinching, 5000, "exposed_pain_flinch", 1.0f, false);
    CHECK(flinching.clips.size() == 3);
    CHECK(flinching.clips[2].animation == "idle" && flinching.clips[2].start == 6000);
    CHECK(ActorLogic::GetClipAfter(flinching, flinch) == ActorLogic::ClipAfter::Resume);
    auto jumping = ActorWith({Clip("idle", 0, 10000)});
    const std::vector<Types::ActorNotetrack> landing = {{"bodyfall small", 0.6f}};
    ActorLogic::PlayAt(jumping, 5000, "ai_jump_down_40", 1.0f, false, landing);
    CHECK(jumping.clips.size() == 3);

    // played over a walk, a death doesn't walk on
    auto walking = ActorWith({Clip("walk", 0, 10000)});
    ActorLogic::PlayAt(walking, 4000, "ai_death_fallforward", 1.333f, false, ragdoll);
    CHECK(walking.clips.size() == 2 && !walking.clips[1].moves);

    // it stays dead: the clips after it go (an idle that went on after a crouch, the crouch's way back up), so the
    // death plays whole; AddDeath's too
    auto crouched = ActorWith(
        {Clip("idle", 0, 2000), Clip("crouch", 2000, 3000), Clip("crouch2stand", 5000, 500), Clip("idle", 5500, 4500)});
    const auto fall = ActorLogic::PlayAt(crouched, 3000, "ai_death_fallforward", 1.333f, false, ragdoll);
    CHECK(crouched.clips.size() == 3 && crouched.clips[1].End() == 3000);
    CHECK(crouched.clips[2].id == fall && crouched.clips[2].length == 1333);
    const auto lying = ActorLogic::Evaluate(crouched, 9000);
    CHECK(lying.size() == 1 && lying[0].clip->id == fall);
    auto shot = ActorWith({Clip("idle", 0, 2000), Clip("crouch", 2000, 3000), Clip("idle", 5000, 5000)});
    ActorLogic::AddDeath(shot, 3000, "death", 1.5f);
    CHECK(shot.clips.size() == 3 && shot.clips[2].animation == "death" && shot.clips[2].length == 1500);
}

TEST("a clip holds its last frame or resumes what played before it")
{
    using ActorLogic::ClipAfter;
    auto actor = ActorWith({Clip("idle", 0, 10000)});
    const auto flinch = ActorLogic::PlayAt(actor, 5000, "flinch", 1.0f, false);
    CHECK(ActorLogic::GetClipAfter(actor, actor.clips[0].id) == ClipAfter::Next);

    // holding: the idle after it goes
    CHECK(ActorLogic::SetClipAfter(actor, flinch, ClipAfter::Hold, 30000));
    CHECK(actor.clips.size() == 2 && actor.clips[1].id == flinch);
    CHECK(ActorLogic::GetClipAfter(actor, flinch) == ClipAfter::Hold);
    CHECK(!ActorLogic::SetClipAfter(actor, flinch, ClipAfter::Hold, 30000));
    const auto held = ActorLogic::Evaluate(actor, 8000);
    CHECK(held.size() == 1 && held[0].clip->id == flinch);

    // resuming: the idle again from where it ends, its animation where it would have been, to the end
    CHECK(ActorLogic::SetClipAfter(actor, flinch, ClipAfter::Resume, 30000));
    CHECK(actor.clips.size() == 3);
    CHECK(actor.clips[2].animation == "idle" && actor.clips[2].start == 6000 && actor.clips[2].End() == 30000);
    CHECK_NEAR(actor.clips[2].offset, 6.0, 1e-4);
    CHECK(actor.clips[2].loop && actor.clips[2].blendIn == actor.clips[0].blendIn);
    CHECK(ActorLogic::GetClipAfter(actor, flinch) == ClipAfter::Resume);

    // to the next clip, at the speed the clip before it played
    auto next = ActorWith({Clip("idle", 0, 4000), Clip("death", 4000, 1000, 100, false), Clip("walk", 8000, 2000)});
    next.clips[0].speed = 0.5f;
    next.clips[0].offset = 1.0f;
    const auto death = next.clips[1].id;
    CHECK(ActorLogic::SetClipAfter(next, death, ClipAfter::Resume, 30000));
    CHECK(next.clips.size() == 4 && next.clips[2].animation == "idle");
    CHECK(next.clips[2].start == 5000 && next.clips[2].End() == 8000);
    CHECK_NEAR(next.clips[2].offset, 3.5, 1e-4);

    // a clip the next one starts right after, or the first clip, can't
    auto busy = ActorWith({Clip("idle", 0, 4000), Clip("reload", 4000, 2000), Clip("walk", 6000, 2000)});
    CHECK(ActorLogic::GetClipAfter(busy, busy.clips[1].id) == ClipAfter::Next);
    CHECK(!ActorLogic::SetClipAfter(busy, busy.clips[1].id, ClipAfter::Hold, 30000));
    CHECK(!ActorLogic::SetClipAfter(busy, busy.clips[1].id, ClipAfter::Resume, 30000));
    auto alone = ActorWith({Clip("death", 1000, 1000, 0, false)});
    CHECK(!ActorLogic::SetClipAfter(alone, alone.clips[0].id, ClipAfter::Resume, 30000));
    CHECK(alone.clips.size() == 1);
    // with the end of the timeline before it ends, for 5 s
    auto late = ActorWith({Clip("idle", 0, 1000), Clip("death", 1000, 1000, 0, false)});
    CHECK(ActorLogic::SetClipAfter(late, late.clips[1].id, ClipAfter::Resume, 1500));
    CHECK(late.clips.size() == 3 && late.clips[2].start == 2000 && late.clips[2].End() == 7000);
}

TEST("a path is cut where it has been walked to")
{
    // its corner rounded with a radius of 20 (the arc from 80 to 111.4 units along it)
    const std::vector<glm::vec3> path{{0, 0, 0}, {100, 0, 0}, {100, 100, 0}};
    const auto arcEnd = 80.0f + 20.0f * glm::half_pi<float>();
    const auto halfway = ActorLogic::CutPath(path, arcEnd + 40.0f, 20.0f);
    CHECK(halfway.size() == 3);
    CHECK_NEAR(halfway[2].x, 100.0, 1e-3);
    CHECK_NEAR(halfway[2].y, 60.0, 1e-3);
    // in the arc: exactly there, the corner's point moved back to where the arc's tangent there meets the way it came
    // (a radian round it: 20 tan 0.5 on from where the arc starts)
    const auto inArc = ActorLogic::CutPath(path, 100.0f, 20.0f);
    CHECK(inArc.size() == 3);
    CHECK_NEAR(inArc[1].x, 80.0 + 20.0 * std::tan(0.5), 1e-3);
    CHECK_NEAR(inArc[1].y, 0.0, 1e-3);
    CHECK_NEAR(inArc[2].x, 80.0 + 20.0 * std::sin(1.0), 1e-3);
    CHECK_NEAR(inArc[2].y, 20.0 - 20.0 * std::cos(1.0), 1e-3);
    // just past it, there too
    CHECK_NEAR(ActorLogic::CutPath(path, arcEnd + 5.0f, 20.0f)[2].y, 25.0, 1e-3);
    CHECK(ActorLogic::CutPath(path, 0.0f, 20.0f).size() == 1);
    CHECK(ActorLogic::CutPath(path, 500.0f, 20.0f) == path);
    const auto straight = ActorLogic::CutPath(path, 30.0f, 20.0f);
    CHECK(straight.size() == 2);
    CHECK_NEAR(straight[1].x, 30.0, 1e-3);

    // the way up to where it was cut stays as it was, with a stop there and any point after it, and it stops exactly
    // where it was
    const auto keeps = [](const std::vector<glm::vec3>& path, float radius, float distance, float within = 1e-3f,
                          float turned = 1e-2f) {
        auto cut = ActorLogic::CutPath(path, distance, radius);
        const auto before = ActorLogic::RoundPath(path, radius);
        CHECK(glm::distance(cut.back(), ActorLogic::PlaceOnPath(before, distance).position) < within);
        cut.push_back(cut.back());
        cut.push_back({-200, 300, 0});
        const auto after = ActorLogic::RoundPath(cut, radius);
        for (float d = 0.0f; d <= distance; d += 0.5f)
        {
            const auto a = ActorLogic::PlaceOnPath(before, d), b = ActorLogic::PlaceOnPath(after, d);
            CHECK(glm::distance(a.position, b.position) < within);
            CHECK(std::abs(std::remainder(a.yaw - b.yaw, 360.0f)) < turned);
        }
        return cut.size() - 2;
    };
    for (const auto distance : {30.0f, 79.0f, 80.5f, 95.0f, 111.0f, arcEnd + 1.0f, arcEnd + 50.0f})
        keeps(path, 20.0f, distance);

    // corners as wide as the stretches let them (half of each): cut in the second's arc, the first's is pinned by a
    // point where it ends, which the path goes straight on through
    const std::vector<glm::vec3> square{{0, 0, 0}, {100, 0, 0}, {100, 100, 0}, {0, 100, 0}};
    const auto firstEnd = 50.0f + 50.0f * glm::half_pi<float>();
    for (const auto along : {2.0f, 5.0f, 30.0f, 70.0f})
        CHECK(keeps(square, 200.0f, firstEnd + along) == 5);
    // (so near the arc's start that its point moved back is all but on the line: still there, the turn under 0.2
    // degrees off over the half unit to it)
    keeps(square, 200.0f, firstEnd + 0.3f, 1e-3f, 0.2f);
    const auto pinned = ActorLogic::CutPath(square, firstEnd + 30.0f, 200.0f);
    CHECK(glm::distance(pinned[2], glm::vec3(100, 50, 0)) < 1e-3f);
    // and on the straight after a corner that reached half the stretch to its next point (which the stop would let
    // it reach past)
    const std::vector<glm::vec3> hook{{-200, 0, 0}, {100, 0, 0}, {100, 100, 0}, {90, 200, 0}};
    const auto hookArc = 250.0f + 50.0f * glm::half_pi<float>();
    CHECK(keeps(hook, 200.0f, hookArc + 20.0f) == 4);
    CHECK(keeps(hook, 200.0f, hookArc + 0.005f, 0.01f) <= 4);
    // on the curve leaving a stop: there (its curve as wide as before; the way it leaves for the stop a tenth of a
    // degree off at most, in its last hundredth of a unit)
    const std::vector<glm::vec3> stopped{{0, 0, 0}, {100, 0, 0}, {100, 0, 0}, {100, 100, 0}};
    for (const auto along : {2.0f, 8.0f, 14.0f})
        CHECK(keeps(stopped, 20.0f, 100.0f + along, 1e-3f, 0.1f) == 4);
}

TEST("a stop moves as one point: both copies of it")
{
    auto actor = ActorWith({Clip("walk", 0, 5000)});
    actor.movement = Types::ActorMovement::Path;
    actor.path = {{0, 0, 0}, {100, 0, 0}, {100, 0, 0}, {100, 100, 0}};
    ActorLogic::MoveWaypoint(actor, 2, {120, 10, 0});
    CHECK(actor.path.size() == 4 && actor.path[1] == glm::vec3(120, 10, 0) && actor.path[2] == glm::vec3(120, 10, 0));
    // a point of its own alone
    ActorLogic::MoveWaypoint(actor, 3, {100, 200, 0});
    CHECK(actor.path[3] == glm::vec3(100, 200, 0) && actor.path[2] == glm::vec3(120, 10, 0));
    ActorLogic::MoveWaypoint(actor, 0, {0, -50, 0});
    CHECK(actor.path[0] == glm::vec3(0, -50, 0) && actor.path[1] == glm::vec3(120, 10, 0));
    ActorLogic::MoveWaypoint(actor, 9, {0, 0, 0});
    CHECK(actor.path.size() == 4);
}

TEST("walk to here walks an actor from where it stands at the playhead and stands it there")
{
    // standing still: a path from where it stands, the walk (100 units a second) as long as it takes, then the idle
    // it had goes on to where it would have ended
    auto actor = ActorWith({Clip("idle", 0, 20000, 0)});
    const auto walk = ActorLogic::WalkTo(actor, 5000, {300, 0, 0}, "walk", "idle", Animations(), 80.0f);
    CHECK(actor.movement == Types::ActorMovement::Path);
    CHECK(actor.path.size() == 2);
    CHECK_NEAR(actor.path[1].x, 300.0, 1e-3);
    CHECK(actor.clips.size() == 3);
    CHECK(actor.clips[0].End() == 5000 && !actor.clips[0].moves);
    const auto& walking = actor.clips[1];
    CHECK(walking.id == walk && walking.animation == "walk" && walking.start == 5000 && walking.moves);
    // 3 s for 300 units, and an eighth of a second more as it blends in from standing
    CHECK_NEAR(walking.length, 3125.0, 15.0);
    CHECK(actor.clips[2].animation == "idle" && actor.clips[2].start == walking.End() && actor.clips[2].End() == 20000);
    const auto there = ActorLogic::EvaluatePlacement(actor, walking.End() + 500, Animations());
    CHECK_NEAR(there.position.x, 300.0, 1.0);
    CHECK_NEAR(ActorLogic::EvaluatePlacement(actor, 4000, Animations()).position.x, 0.0, 1e-3);

    // on its way: the path cut where it is, the walk on to the new point, then the idle it was to stand with
    const auto middle = 6500u;
    const auto from = ActorLogic::EvaluatePlacement(actor, middle, Animations()).position;
    ActorLogic::WalkTo(actor, middle, {from.x, 200, 0}, "walk", "idle", Animations(), 80.0f);
    // (with a stop where it was, which it leaves curving round to the new point)
    CHECK(actor.path.size() == 4);
    CHECK_NEAR(actor.path[1].x, from.x, 1e-3);
    CHECK(actor.path[2] == actor.path[1]);
    CHECK(actor.clips.size() == 4);
    CHECK(actor.clips[1].End() == middle);
    CHECK(actor.clips[2].animation == "walk" && actor.clips[2].start == middle);
    // (taking over from the walk before it, at the same speed, as far as is left of the path)
    const auto left =
        ActorLogic::GetPathLength(actor, Animations()) - ActorLogic::GetWalkedDistance(actor, middle, Animations());
    CHECK(left > 200.0f && left < 215.0f);
    CHECK_NEAR(actor.clips[2].length, left * 10.0f, 15.0);
    CHECK(actor.clips[3].animation == "idle" && actor.clips[3].start == actor.clips[2].End());
    CHECK(actor.clips[3].End() == 20000);
    const auto arrived = ActorLogic::EvaluatePlacement(actor, actor.clips[3].start + 500, Animations());
    CHECK_NEAR(arrived.position.x, from.x, 1.0);
    CHECK_NEAR(arrived.position.y, 200.0, 1.0);

    // a walk not measured yet goes at the guessed speed, and FitWalk fits it again once it is
    auto guessed = ActorWith({Clip("idle", 0, 20000, 0)});
    const auto unmeasured = ActorLogic::WalkTo(guessed, 5000, {300, 0, 0}, "unmeasured", "idle", Animations(), 80.0f);
    CHECK(guessed.clips[1].length == 3750);
    CHECK(guessed.clips[2].start == 8750 && guessed.clips[2].End() == 20000);
    guessed.clips[1].animation = "walk";
    ActorLogic::FitWalk(guessed, unmeasured, Animations(), 80.0f);
    CHECK_NEAR(guessed.clips[1].length, 3125.0, 15.0);
    CHECK(guessed.clips[2].start == guessed.clips[1].End() && guessed.clips[2].End() == 20000);

    // with keys: one where it stands at the playhead and one where it gets to, facing the way it goes
    auto keyed = ActorWith({Clip("idle", 0, 20000, 0)});
    keyed.movement = Types::ActorMovement::Keyframes;
    keyed.keys = {{0, {0, 0, 0}, 0.0f}};
    ActorLogic::WalkTo(keyed, 2000, {0, 200, 0}, "walk", "idle", Animations(), 80.0f);
    CHECK(keyed.keys.size() == 3);
    CHECK(keyed.keys[1].tick == 2000);
    CHECK_NEAR(keyed.keys[1].yaw, 90.0, 1e-3);
    CHECK(keyed.keys[2].tick == 4000);
    CHECK_NEAR(keyed.keys[2].position.y, 200.0, 1e-3);
    CHECK(keyed.clips[1].animation == "walk" && keyed.clips[1].length == 2000);
    CHECK(keyed.clips[2].start == 4000);
    CHECK_NEAR(ActorLogic::EvaluatePlacement(keyed, 3000, Animations()).position.y, 100.0, 1.0);

    // attached, it can't
    auto attached = ActorWith({Clip("idle", 0, 20000)});
    attached.movement = Types::ActorMovement::Attached;
    const auto before = attached;
    CHECK(ActorLogic::WalkTo(attached, 2000, {0, 200, 0}, "walk", "idle", Animations(), 80.0f) == 0);
    CHECK(attached == before);
}

TEST("recent ones come first, once each, and only so many")
{
    std::vector<std::string> recent{"a", "b"};
    ActorLogic::AddRecent(recent, "b");
    CHECK((recent == std::vector<std::string>{"b", "a"}));
    ActorLogic::AddRecent(recent, "c", 2);
    CHECK((recent == std::vector<std::string>{"c", "b"}));
}
