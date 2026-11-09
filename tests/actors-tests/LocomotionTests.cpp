#include "StdInclude.hpp"
#include "Test.hpp"

#include "Components/ActorLogic.hpp"
#include "Components/KillstreakLogic.hpp"

using namespace IWXMVM;

// Locomotion: the players' walks and runs picked by an actor's speed (ActorLogic::GetLayers, ChooseLocomotion,
// GetClipWalk; step 3)

namespace
{
    using Kind = Types::AnimationKind;
    using Family = Types::AnimationFamily;
    using Gait = ActorLogic::Gait;
    using Heading = ActorLogic::Heading;

    // The game's animations as the tests know them: the rifle's movement (as fast as the game measured the forward
    // ones), the pistol's forward ones, upper body ones, a death and SP ones. Each plays a second unless it says.
    ActorLogic::AnimationLookup Animations()
    {
        struct Known
        {
            std::string_view name;
            glm::vec2 motion;  // in a play
            float duration = 1.0f;
            Kind kind = Kind::FullBody;
            Family family = Family::MpPlayer;
        };
        static const std::vector<Known> known = {
            {"pb_stand_alert", {0.2f, 0}, 10.0f},
            {"pb_stand_shoot_walk_forward", {94, 0}},
            {"pb_stand_shoot_walk_back", {-90, 0}},
            {"pb_stand_shoot_walk_left", {0, 76}},
            {"pb_stand_shoot_walk_right", {0, -76}},
            {"pb_combatrun_forward_loop", {211, 0}},
            {"pb_combatrun_back_loop", {-184, 0}},
            {"pb_combatrun_left_loop", {0, 184}},
            {"pb_combatrun_right_loop", {0, -189}},
            {"pb_sprint", {281, 0}},
            {"pb_crouch_alert", {0.1f, 0}, 10.0f},
            {"pb_crouch_shoot_run_forward", {83, 0}},
            {"pb_crouch_shoot_run_back", {-83, 0}},
            {"pb_crouch_shoot_run_left", {0, 83}},
            {"pb_crouch_shoot_run_right", {0, -83}},
            {"pb_crouch_run_forward", {120, 0}},
            {"pb_crouch_run_back", {-120, 0}},
            {"pb_crouch_run_left", {0, 120}},
            {"pb_crouch_run_right", {0, -120}},
            {"pb_prone_aim", {0, 0}, 10.0f},
            {"pb_prone_crawl", {77, 0}, 10.0f},
            {"pb_prone_crawl_back", {-77, 0}, 10.0f},
            {"pb_prone_crawl_left", {0, 77}, 10.0f},
            {"pb_prone_crawl_right", {0, -77}, 10.0f},
            {"pb_stand_alert_pistol", {0, 0}, 5.0f},
            {"pb_combatwalk_forward_loop_pistol", {99, 0}},
            {"pb_pistol_run_fast", {175, 0}},
            {"pb_sprint_pistol", {260, 0}},
            {"pt_reload_stand_auto", {0, 0}, 2.0f, Kind::Torso},
            {"pt_stand_shoot", {0, 0}, 1.0f, Kind::Torso},
            {"pb_stand_death_headchest_topple", {0, 0}, 2.0f},
            {"casual_stand_idle", {0, 0}, 5.0f, Kind::FullBody, Family::SpHuman},
            {"walk_lowready_f", {91, 0}, 1.0f, Kind::FullBody, Family::SpHuman},
        };
        return [](const std::string& name) -> std::optional<ActorLogic::AnimationInfo> {
            const auto found = std::ranges::find(known, name, &Known::name);
            if (found == known.end())
                return std::nullopt;
            return ActorLogic::AnimationInfo{found->duration, found->motion, {}, found->kind, found->family};
        };
    }

    Types::ActorClip Clip(std::string animation, std::uint32_t start, std::uint32_t length, std::uint32_t blendIn = 0,
                          bool loop = true)
    {
        return {.animation = std::move(animation), .start = start, .length = length, .loop = loop, .blendIn = blendIn};
    }

    // a clip that walks at a speed of its own
    Types::ActorClip Walking(std::string animation, std::uint32_t start, std::uint32_t length, float walkSpeed)
    {
        auto clip = Clip(std::move(animation), start, length);
        clip.walkSpeed = walkSpeed;
        return clip;
    }

    // an actor with Locomotion Auto on a long path along +x, its walking clips at a speed
    Types::Actor Mover(std::vector<Types::ActorClip> clips, float speed = 211.0f)
    {
        Types::Actor actor;
        actor.clips = std::move(clips);
        actor.locomotion = Types::ActorLocomotion::Auto;
        actor.pathSpeed = speed;
        actor.movement = Types::ActorMovement::Path;
        actor.path = {{0, 0, 0}, {100000, 0, 0}};
        ActorLogic::Normalize(actor);
        return actor;
    }

    ActorLogic::Layers Layers(const Types::Actor& actor, std::uint32_t tick,
                              Types::GunClass gun = Types::GunClass::Rifle)
    {
        return ActorLogic::GetLayers(actor, tick, Animations(), gun);
    }

    // the legs' weight of an animation, 0 when they don't play it, and its sample
    float LegsOf(const ActorLogic::Layers& layers, std::string_view animation)
    {
        float weight = 0.0f;
        for (const auto& legs : layers.legs)
            if ((legs.clip ? legs.clip->animation : legs.animation) == animation)
                weight += legs.weight;
        return weight;
    }
    const ActorLogic::LegsSample* Legs(const ActorLogic::Layers& layers, std::string_view animation)
    {
        const auto found = std::ranges::find_if(
            layers.legs, [&](const ActorLogic::LegsSample& legs) { return !legs.clip && legs.animation == animation; });
        return found != layers.legs.end() ? &*found : nullptr;
    }

    // what the players' animations do with an MP body's stabilizer: move it
    PoseLogic::SpineKind Spine(const std::string& animation)
    {
        return animation.starts_with("pb_") ? PoseLogic::SpineKind::Moves : PoseLogic::SpineKind::AtRest;
    }

    float WeightOf(const ActorLogic::TreeWeights& weights, std::string_view animation)
    {
        float weight = 0.0f;
        for (const auto& entry : weights.animations)
            if ((entry.clip ? entry.clip->animation : entry.animation) == animation)
                weight += entry.weight;
        return weight;
    }

    void CheckChoice(Types::GunClass gun, Types::ActorStance stance, float speed, float angle,
                     std::string_view animation, float rate)
    {
        const auto choice = ActorLogic::ChooseLocomotion(gun, stance, speed, angle, Animations());
        if (choice.animation != animation || std::abs(choice.rate - rate) > 1e-3f)
            Test::Fail(__FILE__, __LINE__,
                       std::format("at {} units/s and {} degrees: {} at {:.4f}, expected {} at {:.4f}", speed, angle,
                                   choice.animation, choice.rate, animation, rate));
    }
}  // namespace

TEST("locomotion: the speed picks the idle, the walk, the run or the sprint, played as fast as the speed asks")
{
    using Gun = Types::GunClass;
    using Stance = Types::ActorStance;
    // standing: walking from 5 units a second, running above halfway between the walk's 94 and the run's 211
    // (152.5), sprinting above halfway between the run's and the sprint's 281 (246); within 0.1x and 2x
    CheckChoice(Gun::Rifle, Stance::Stand, 0.0f, 0.0f, "pb_stand_alert", 1.0f);
    CheckChoice(Gun::Rifle, Stance::Stand, 4.9f, 0.0f, "pb_stand_alert", 1.0f);
    CheckChoice(Gun::Rifle, Stance::Stand, 5.0f, 0.0f, "pb_stand_shoot_walk_forward", 0.1f);
    CheckChoice(Gun::Rifle, Stance::Stand, 90.0f, 0.0f, "pb_stand_shoot_walk_forward", 90.0f / 94.0f);
    CheckChoice(Gun::Rifle, Stance::Stand, 152.0f, 0.0f, "pb_stand_shoot_walk_forward", 152.0f / 94.0f);
    CheckChoice(Gun::Rifle, Stance::Stand, 153.0f, 0.0f, "pb_combatrun_forward_loop", 153.0f / 211.0f);
    CheckChoice(Gun::Rifle, Stance::Stand, 200.0f, 0.0f, "pb_combatrun_forward_loop", 200.0f / 211.0f);
    CheckChoice(Gun::Rifle, Stance::Stand, 246.0f, 0.0f, "pb_combatrun_forward_loop", 246.0f / 211.0f);
    CheckChoice(Gun::Rifle, Stance::Stand, 247.0f, 0.0f, "pb_sprint", 247.0f / 281.0f);
    CheckChoice(Gun::Rifle, Stance::Stand, 280.0f, 0.0f, "pb_sprint", 280.0f / 281.0f);
    CheckChoice(Gun::Rifle, Stance::Stand, 700.0f, 0.0f, "pb_sprint", 2.0f);
    // Auto stands
    CheckChoice(Gun::Rifle, Stance::Auto, 200.0f, 0.0f, "pb_combatrun_forward_loop", 200.0f / 211.0f);
    // crouching: the walk 83, the run 120 (above 101.5), no sprint
    CheckChoice(Gun::Rifle, Stance::Crouch, 0.0f, 0.0f, "pb_crouch_alert", 1.0f);
    CheckChoice(Gun::Rifle, Stance::Crouch, 50.0f, 0.0f, "pb_crouch_shoot_run_forward", 50.0f / 83.0f);
    CheckChoice(Gun::Rifle, Stance::Crouch, 110.0f, 0.0f, "pb_crouch_run_forward", 110.0f / 120.0f);
    CheckChoice(Gun::Rifle, Stance::Crouch, 300.0f, 0.0f, "pb_crouch_run_forward", 2.0f);
    // prone, it crawls at 7.7
    CheckChoice(Gun::Rifle, Stance::Prone, 3.0f, 0.0f, "pb_prone_aim", 1.0f);
    CheckChoice(Gun::Rifle, Stance::Prone, 7.0f, 0.0f, "pb_prone_crawl", 7.0f / 7.7f);
    CheckChoice(Gun::Rifle, Stance::Prone, 200.0f, 0.0f, "pb_prone_crawl", 2.0f);
    // a pistol's own (walk 99, run 175, sprint 260), the rifle's where the game hasn't the gun's
    CheckChoice(Gun::Pistol, Stance::Stand, 50.0f, 0.0f, "pb_combatwalk_forward_loop_pistol", 50.0f / 99.0f);
    CheckChoice(Gun::Pistol, Stance::Stand, 175.0f, 0.0f, "pb_pistol_run_fast", 1.0f);
    CheckChoice(Gun::Pistol, Stance::Stand, 250.0f, 0.0f, "pb_sprint_pistol", 250.0f / 260.0f);
    CheckChoice(Gun::Pistol, Stance::Stand, 0.0f, 0.0f, "pb_stand_alert_pistol", 1.0f);
    CheckChoice(Gun::Pistol, Stance::Stand, 180.0f, 180.0f, "pb_combatrun_back_loop", 180.0f / 184.0f);
    CheckChoice(Gun::DualWield, Stance::Stand, 200.0f, 0.0f, "pb_combatrun_forward_loop", 200.0f / 211.0f);

    const auto choice = ActorLogic::ChooseLocomotion(Gun::Rifle, Stance::Stand, 300.0f, 0.0f, Animations());
    CHECK(choice.gait == Gait::Sprint);
    CHECK(choice.heading == Heading::Forward);
    CHECK(ActorLogic::ChooseLocomotion(Gun::Rifle, Stance::Stand, 100.0f, 0.0f, Animations()).gait == Gait::Walk);
    CHECK(ActorLogic::ChooseLocomotion(Gun::Rifle, Stance::Stand, 4.0f, 0.0f, Animations()).gait == Gait::Idle);

    // the speeds the game measured, else about those of the rifle's of the gait
    CHECK_NEAR(ActorLogic::GetAnimationSpeed("pb_combatrun_forward_loop", Animations()), 211.0, 1e-3);
    const ActorLogic::AnimationLookup none = [](const std::string&) { return std::nullopt; };
    CHECK_NEAR(ActorLogic::GetAnimationSpeed("pb_sprint_rpg", none), 281.0, 1e-3);
    CHECK_NEAR(ActorLogic::GetAnimationSpeed("pb_crouch_run_left", none), 120.0, 1e-3);
    CHECK_NEAR(ActorLogic::GetAnimationSpeed("pb_crouch_walk_back_pistol", none), 83.0, 1e-3);
    CHECK_NEAR(ActorLogic::GetAnimationSpeed("pb_stand_alert", none), 0.0, 1e-6);
    CHECK_NEAR(ActorLogic::GetAnimationSpeed("casual_stand_idle", Animations()), 0.0, 1e-6);
}

TEST("locomotion: the way it goes against the way it faces picks the back and strafe animations")
{
    CHECK(ActorLogic::GetHeading(0.0f) == Heading::Forward);
    CHECK(ActorLogic::GetHeading(45.0f) == Heading::Forward);
    CHECK(ActorLogic::GetHeading(-45.0f) == Heading::Forward);
    CHECK(ActorLogic::GetHeading(46.0f) == Heading::Left);
    CHECK(ActorLogic::GetHeading(90.0f) == Heading::Left);
    CHECK(ActorLogic::GetHeading(134.0f) == Heading::Left);
    CHECK(ActorLogic::GetHeading(135.0f) == Heading::Back);
    CHECK(ActorLogic::GetHeading(180.0f) == Heading::Back);
    CHECK(ActorLogic::GetHeading(-180.0f) == Heading::Back);
    CHECK(ActorLogic::GetHeading(-135.0f) == Heading::Back);
    CHECK(ActorLogic::GetHeading(-46.0f) == Heading::Right);
    CHECK(ActorLogic::GetHeading(270.0f) == Heading::Right);
    CHECK_NEAR(ActorLogic::GetHeadingAngle(Heading::Left), 90.0, 1e-6);
    CHECK_NEAR(ActorLogic::GetHeadingAngle(Heading::Back), 180.0, 1e-6);

    using Gun = Types::GunClass;
    using Stance = Types::ActorStance;
    // each heading's walk and run, and the speed between them (left: 76 and 184, 130 between)
    CheckChoice(Gun::Rifle, Stance::Stand, 80.0f, 90.0f, "pb_stand_shoot_walk_left", 80.0f / 76.0f);
    CheckChoice(Gun::Rifle, Stance::Stand, 131.0f, 90.0f, "pb_combatrun_left_loop", 131.0f / 184.0f);
    CheckChoice(Gun::Rifle, Stance::Stand, 100.0f, -90.0f, "pb_stand_shoot_walk_right", 100.0f / 76.0f);
    CheckChoice(Gun::Rifle, Stance::Stand, 180.0f, 180.0f, "pb_combatrun_back_loop", 180.0f / 184.0f);
    CheckChoice(Gun::Rifle, Stance::Stand, 120.0f, 180.0f, "pb_stand_shoot_walk_back", 120.0f / 90.0f);
    // no sprinting but forward
    CheckChoice(Gun::Rifle, Stance::Stand, 300.0f, -90.0f, "pb_combatrun_right_loop", 300.0f / 189.0f);
    CheckChoice(Gun::Rifle, Stance::Stand, 300.0f, 170.0f, "pb_combatrun_back_loop", 300.0f / 184.0f);
    CheckChoice(Gun::Rifle, Stance::Crouch, 60.0f, 90.0f, "pb_crouch_shoot_run_left", 60.0f / 83.0f);
    CheckChoice(Gun::Rifle, Stance::Prone, 7.0f, -100.0f, "pb_prone_crawl_right", 7.0f / 7.7f);
    CHECK(ActorLogic::ChooseLocomotion(Gun::Rifle, Stance::Stand, 80.0f, 90.0f, Animations()).heading == Heading::Left);
}

TEST("locomotion: the players' animations of every gun and stance, and what each is")
{
    using Gun = Types::GunClass;
    using Stance = Types::ActorStance;
    CHECK(ActorLogic::GetLocomotionAnimation(Gun::Rifle, Stance::Stand, Gait::Walk, Heading::Back) ==
          "pb_stand_shoot_walk_back");
    CHECK(ActorLogic::GetLocomotionAnimation(Gun::Pistol, Stance::Stand, Gait::Run, Heading::Forward) ==
          "pb_pistol_run_fast");
    CHECK(ActorLogic::GetLocomotionAnimation(Gun::RocketLauncher, Stance::Crouch, Gait::Walk, Heading::Left) ==
          "pb_crouch_walk_left_rpg");
    CHECK(ActorLogic::GetLocomotionAnimation(Gun::DualWield, Stance::Prone, Gait::Run, Heading::Back) ==
          "pb_prone_dw_crawl_back");
    CHECK(ActorLogic::GetLocomotionAnimation(Gun::BallisticKnife, Stance::Stand, Gait::Sprint, Heading::Forward) ==
          "pb_dw_sprint");
    // the nearest where there is none: a crouching sprint runs, a sprint sideways runs that way
    CHECK(ActorLogic::GetLocomotionAnimation(Gun::Rifle, Stance::Crouch, Gait::Sprint, Heading::Forward) ==
          "pb_crouch_run_forward");
    CHECK(ActorLogic::GetLocomotionAnimation(Gun::Rifle, Stance::Stand, Gait::Sprint, Heading::Left) ==
          "pb_combatrun_left_loop");
    // the idles are the stance idles of the legs under upper body clips
    for (const auto gun : {Gun::Rifle, Gun::Pistol, Gun::RocketLauncher, Gun::DualWield, Gun::BallisticKnife})
        for (const auto stance : {Stance::Stand, Stance::Crouch, Stance::Prone})
            CHECK(ActorLogic::GetLocomotionAnimation(gun, stance, Gait::Idle, Heading::Left) ==
                  ActorLogic::GetStanceIdle(gun, stance));

    const auto run = ActorLogic::FindLocomotion("pb_combatrun_left_loop");
    CHECK(run && run->gait == Gait::Run && run->heading == Heading::Left && run->stance == Stance::Stand);
    const auto crawl = ActorLogic::FindLocomotion("pb_rpg_prone_crawl_backward");
    CHECK(crawl && crawl->gun == Gun::RocketLauncher && crawl->stance == Stance::Prone && crawl->gait == Gait::Walk &&
          crawl->heading == Heading::Back);
    const auto idle = ActorLogic::FindLocomotion("pb_crouch_alert_pistol");
    CHECK(idle && idle->gait == Gait::Idle && idle->stance == Stance::Crouch);
    CHECK(!ActorLogic::FindLocomotion("pt_reload_stand_auto"));
    // the AI's walks are of their movement sets (step 8), which stand for the locomotion of actors of those sets only
    const auto aiWalk = ActorLogic::FindLocomotion("walk_lowready_f");
    CHECK(aiWalk && aiWalk->set == Types::ActorMovementSet::LowReady && aiWalk->gait == Gait::Walk);
    Types::Actor players;
    players.movement = Types::ActorMovement::Path;
    players.path = {{0, 0, 0}, {1000, 0, 0}};
    CHECK(!ActorLogic::FindLocomotion(players, "walk_lowready_f"));
    players.movementSet = Types::ActorMovementSet::Auto;
    CHECK(ActorLogic::FindLocomotion(players, "walk_lowready_f").has_value());
    CHECK(!ActorLogic::FindLocomotion("pb_turn_90left"));
    // with nowhere to go (standing still, attached, its path's second point not placed yet), a walk, run or sprint
    // plays as it is, in place; an idle still stands for the locomotion's (the gun's, the movement set's)
    CHECK(ActorLogic::HasWayToGo(players));
    for (const auto movement : {Types::ActorMovement::Still, Types::ActorMovement::Attached})
    {
        auto still = players;
        still.movement = movement;
        CHECK(!ActorLogic::HasWayToGo(still));
        CHECK(!ActorLogic::FindLocomotion(still, "walk_lowready_f"));
        CHECK(!ActorLogic::FindLocomotion(still, "pb_sprint"));
        CHECK(ActorLogic::FindLocomotion(still, "pb_stand_alert").has_value());
        CHECK(ActorLogic::FindLocomotion(still, "casual_stand_idle").has_value());
    }
    auto placing = players;
    placing.path.resize(1);
    CHECK(!ActorLogic::FindLocomotion(placing, "pb_combatrun_forward_loop"));
    auto keyed = players;
    keyed.movement = Types::ActorMovement::Keyframes;
    keyed.keys = {{0, {0, 0, 0}, 0.0f}};
    CHECK(!ActorLogic::FindLocomotion(keyed, "pb_combatrun_forward_loop"));
    keyed.keys.push_back({1000, {100, 0, 0}, 0.0f});
    CHECK(ActorLogic::FindLocomotion(keyed, "pb_combatrun_forward_loop").has_value());
    // a still dog runs in place too; a killstreak's motions aren't animations, which it goes by however it moves
    auto dog = players;
    dog.movement = Types::ActorMovement::Still;
    CHECK(!ActorLogic::FindLocomotion(dog, "german_shepherd_run"));
    CHECK(ActorLogic::FindLocomotion(dog, "german_shepherd_idle").has_value());
    CHECK(ActorLogic::FindLocomotion(dog, Components::KillstreakLogic::MOTION_FLY).has_value());
}

TEST("locomotion: its clips of the players' movement walk the path at the actor's speed, upper body ones walk on")
{
    const auto animations = Animations();
    auto actor = Mover({Clip("pb_combatrun_forward_loop", 0, 2000), Clip("pb_stand_alert", 2000, 3000)}, 150.0f);
    auto walk = ActorLogic::GetClipWalk(actor, actor.clips[0], animations);
    CHECK(walk.angle && std::abs(*walk.angle) < 1e-4f);
    CHECK_NEAR(walk.speed, 150.0, 1e-4);
    // an idle stands
    CHECK(!ActorLogic::GetClipWalk(actor, actor.clips[1], animations).angle);
    CHECK_NEAR(ActorLogic::GetWalkedDistance(actor, 1000, animations), 150.0, 0.5);
    CHECK_NEAR(ActorLogic::GetWalkedDistance(actor, 2000, animations), 300.0, 0.5);
    CHECK_NEAR(ActorLogic::GetWalkedDistance(actor, 4000, animations), 300.0, 0.5);
    // without locomotion, at the animation's own speed
    auto off = actor;
    off.locomotion = Types::ActorLocomotion::Off;
    CHECK_NEAR(ActorLogic::GetClipWalk(off, off.clips[0], animations).speed, 211.0, 1e-3);
    CHECK_NEAR(ActorLogic::GetWalkedDistance(off, 2000, animations), 422.0, 0.5);
    CHECK(ActorLogic::GetLayers(off, 1000, animations, Types::GunClass::Rifle).legs.empty());

    // the way its animation goes, or its direction; a speed of its own; none when it doesn't walk the path
    actor.clips[0].animation = "pb_combatrun_left_loop";
    walk = ActorLogic::GetClipWalk(actor, actor.clips[0], animations);
    CHECK(walk.angle && std::abs(*walk.angle - 90.0f) < 1e-4f);
    actor.clips[0].direction = Types::ActorWalkDirection::Back;
    walk = ActorLogic::GetClipWalk(actor, actor.clips[0], animations);
    CHECK(walk.angle && std::abs(std::abs(*walk.angle) - 180.0f) < 1e-4f);
    actor.clips[0].walkSpeed = 50.0f;
    CHECK_NEAR(ActorLogic::GetClipWalk(actor, actor.clips[0], animations).speed, 50.0, 1e-4);
    actor.clips[0].moves = false;
    CHECK(!ActorLogic::GetClipWalk(actor, actor.clips[0], animations).angle);

    // an upper body clip walks on with the walk it interrupts, as long as it is (not only while its animation
    // plays): this reload plays two seconds of three
    const auto reloading =
        Mover({Clip("pb_combatrun_forward_loop", 0, 1000), Clip("pt_reload_stand_auto", 1000, 3000, 0, false),
               Clip("pb_stand_alert", 4000, 1000)},
              150.0f);
    walk = ActorLogic::GetClipWalk(reloading, reloading.clips[1], animations);
    CHECK(walk.angle && std::abs(*walk.angle) < 1e-4f);
    CHECK_NEAR(walk.speed, 150.0, 1e-4);
    CHECK_NEAR(ActorLogic::GetWalkedDistance(reloading, 4000, animations), 600.0, 0.5);
    CHECK_NEAR(ActorLogic::GetWalkedDistance(reloading, 4500, animations), 600.0, 0.5);
    // not the one after an idle, or one that doesn't walk the path
    auto standing = Mover({Clip("pb_stand_alert", 0, 1000), Clip("pt_reload_stand_auto", 1000, 3000)});
    CHECK(!ActorLogic::GetClipWalk(standing, standing.clips[1], animations).angle);
    auto still = reloading;
    still.clips[1].moves = false;
    CHECK(!ActorLogic::GetClipWalk(still, still.clips[1], animations).angle);
    // SP walks go at their own speed
    const auto sp = Mover({Clip("walk_lowready_f", 0, 1000)}, 150.0f);
    CHECK_NEAR(ActorLogic::GetClipWalk(sp, sp.clips[0], animations).speed, 91.0, 1e-3);
}

TEST("locomotion: the legs are a locomotion clip's body, and run on under an upper body clip")
{
    const auto actor =
        Mover({Clip("pb_stand_shoot_walk_forward", 0, 2000), Clip("pt_reload_stand_auto", 2000, 3000, 250)});
    // at 211 units a second it runs, though the clip is of a walk
    auto layers = Layers(actor, 1000);
    CHECK(layers.clipCount == 1 && layers.locomotion[0] && !layers.upperBody[0]);
    CHECK(layers.legs.size() == 1);
    const auto* run = Legs(layers, "pb_combatrun_forward_loop");
    CHECK(run != nullptr);
    if (run)
    {
        CHECK_NEAR(run->weight, 1.0, 1e-6);
        CHECK_NEAR(run->rate, 1.0, 1e-4);
        CHECK_NEAR(run->time, 1.0, 1e-4);
    }
    // the tree plays the run, not the clip's walk
    auto weights = ActorLogic::WeighLayers(layers, Spine);
    CHECK(weights.animations.size() == 1);
    CHECK_NEAR(WeightOf(weights, "pb_combatrun_forward_loop"), 1.0, 1e-6);
    CHECK_NEAR(WeightOf(weights, "pb_stand_shoot_walk_forward"), 0.0, 1e-6);

    // halfway into the reload's blend: the run is the legs, as much as the reload's hundredth and the clip's half
    layers = Layers(actor, 2125);
    CHECK_NEAR(LegsOf(layers, "pb_combatrun_forward_loop"), 1.0, 1e-6);
    weights = ActorLogic::WeighLayers(layers, Spine);
    CHECK_NEAR(WeightOf(weights, "pt_reload_stand_auto"), 0.5, 1e-5);
    CHECK_NEAR(WeightOf(weights, "pb_combatrun_forward_loop"), 0.505, 1e-5);
    // and under it, the legs run on: a hundredth of the reload, as fast as before, as far into the run as it ran
    layers = Layers(actor, 3000);
    CHECK(layers.upperBody[0] && !layers.locomotion[0]);
    run = Legs(layers, "pb_combatrun_forward_loop");
    CHECK(run && std::abs(run->weight - 1.0f) < 1e-6f && std::abs(run->rate - 1.0f) < 1e-4f &&
          std::abs(run->time - 3.0f) < 1e-3f);
    weights = ActorLogic::WeighLayers(layers, Spine);
    CHECK_NEAR(WeightOf(weights, "pt_reload_stand_auto"), 1.0, 1e-6);
    CHECK_NEAR(WeightOf(weights, "pb_combatrun_forward_loop"), ActorLogic::LEGS_WEIGHT, 1e-6);
    CHECK_NEAR(ActorLogic::GetWalkedDistance(actor, 3000, Animations()), 633.0, 0.5);

    // left out (for the harness), the clip is its own animation again
    layers = Layers(actor, 1000);
    layers.legs.clear();
    weights = ActorLogic::WeighLayers(layers, Spine);
    CHECK_NEAR(WeightOf(weights, "pb_stand_shoot_walk_forward"), 1.0, 1e-6);
}

TEST("locomotion: a full body clip shows in its place, and stops the walk")
{
    auto death = Clip("pb_stand_death_headchest_topple", 2000, 2000, 100, false);
    death.moves = false;
    const auto actor = Mover({Clip("pb_combatrun_forward_loop", 0, 2000), death});
    auto layers = Layers(actor, 2050);
    CHECK_NEAR(LegsOf(layers, "pb_combatrun_forward_loop"), 1.0, 1e-6);
    auto weights = ActorLogic::WeighLayers(layers, Spine);
    CHECK_NEAR(WeightOf(weights, "pb_combatrun_forward_loop"), 0.5, 1e-5);
    CHECK_NEAR(WeightOf(weights, "pb_stand_death_headchest_topple"), 0.5, 1e-5);
    // once it has blended in, it alone
    layers = Layers(actor, 2200);
    CHECK(layers.legs.empty());
    weights = ActorLogic::WeighLayers(layers, Spine);
    CHECK(weights.animations.size() == 1);
    CHECK_NEAR(WeightOf(weights, "pb_stand_death_headchest_topple"), 1.0, 1e-6);
    // the run walks on as it blends out (11.6 units in its steps), then the actor stays
    const auto animations = Animations();
    CHECK_NEAR(ActorLogic::GetWalkedDistance(actor, 2000, animations), 422.0, 0.5);
    CHECK_NEAR(ActorLogic::GetWalkedDistance(actor, 2500, animations), 433.6, 0.5);
    CHECK_NEAR(ActorLogic::GetWalkedDistance(actor, 3500, animations), 433.6, 0.5);
}

TEST("locomotion: the legs blend to what the speed, heading and stance change to, over the game's times")
{
    // speeding up: from a walk to a run in 120 ms
    const auto faster = Mover({Walking("pb_stand_shoot_walk_forward", 0, 2000, 90.0f),
                               Walking("pb_stand_shoot_walk_forward", 2000, 2000, 211.0f)});
    CHECK_NEAR(LegsOf(Layers(faster, 1999), "pb_stand_shoot_walk_forward"), 1.0, 1e-6);
    CHECK_NEAR(Legs(Layers(faster, 1999), "pb_stand_shoot_walk_forward")->rate, 90.0 / 94.0, 1e-4);
    CHECK_NEAR(LegsOf(Layers(faster, 2000), "pb_stand_shoot_walk_forward"), 1.0, 1e-6);
    CHECK_NEAR(LegsOf(Layers(faster, 2060), "pb_combatrun_forward_loop"), 0.5, 1e-5);
    CHECK_NEAR(LegsOf(Layers(faster, 2060), "pb_stand_shoot_walk_forward"), 0.5, 1e-5);
    CHECK_NEAR(LegsOf(Layers(faster, 2120), "pb_combatrun_forward_loop"), 1.0, 1e-6);

    // stopping: from a run to the idle in 250 ms
    const auto stopping = Mover({Clip("pb_combatrun_forward_loop", 0, 2000), Clip("pb_stand_alert", 2000, 3000)});
    CHECK_NEAR(LegsOf(Layers(stopping, 2125), "pb_stand_alert"), 0.5, 1e-5);
    CHECK_NEAR(LegsOf(Layers(stopping, 2250), "pb_stand_alert"), 1.0, 1e-6);
    // the idle loops from the stretch's start, its rate 1
    CHECK_NEAR(Legs(Layers(stopping, 2250), "pb_stand_alert")->time, 2.25, 1e-4);
    CHECK_NEAR(Legs(Layers(stopping, 2250), "pb_stand_alert")->rate, 1.0, 1e-6);

    // crouching: to the crouching run in 400 ms, as fast as 211 asks of it (120: 1.76x)
    const auto crouching =
        Mover({Clip("pb_combatrun_forward_loop", 0, 2000), Clip("pb_crouch_run_forward", 2000, 2000)});
    CHECK_NEAR(LegsOf(Layers(crouching, 2200), "pb_crouch_run_forward"), 0.5, 1e-5);
    CHECK_NEAR(LegsOf(Layers(crouching, 2400), "pb_crouch_run_forward"), 1.0, 1e-6);
    CHECK_NEAR(Legs(Layers(crouching, 2400), "pb_crouch_run_forward")->rate, 211.0 / 120.0, 1e-4);
    // the actor's stance crouches them all along
    auto crouched = crouching;
    crouched.stance = Types::ActorStance::Crouch;
    CHECK_NEAR(LegsOf(Layers(crouched, 1000), "pb_crouch_run_forward"), 1.0, 1e-6);

    // turning to run left: 120 ms
    const auto turning =
        Mover({Clip("pb_combatrun_forward_loop", 0, 2000), Clip("pb_combatrun_left_loop", 2000, 2000)});
    CHECK_NEAR(LegsOf(Layers(turning, 2060), "pb_combatrun_left_loop"), 0.5, 1e-5);
    CHECK_NEAR(Legs(Layers(turning, 2060), "pb_combatrun_left_loop")->rate, 211.0 / 184.0, 1e-4);

    // slowing down as an idle blends in over 250 ms: the speed is 211 x the run's weight, under 152.5 from 2070 (a
    // walk in 120 ms), under 5 from 2245 (the idle in 250 ms)
    const auto slowing = Mover({Clip("pb_combatrun_forward_loop", 0, 2000), Clip("pb_stand_alert", 2000, 3000, 250)});
    CHECK_NEAR(LegsOf(Layers(slowing, 2069), "pb_combatrun_forward_loop"), 1.0, 1e-6);
    CHECK_NEAR(LegsOf(Layers(slowing, 2130), "pb_stand_shoot_walk_forward"), 0.5, 1e-5);
    CHECK_NEAR(LegsOf(Layers(slowing, 2190), "pb_stand_shoot_walk_forward"), 1.0, 1e-6);
    CHECK_NEAR(LegsOf(Layers(slowing, 2370), "pb_stand_alert"), 0.5, 1e-5);
    CHECK_NEAR(LegsOf(Layers(slowing, 2495), "pb_stand_alert"), 1.0, 1e-6);
    // and no tick to the next moves the legs' shares by more than a tick of the shortest blend
    float largest = 0.0f;
    for (std::uint32_t tick = 1990; tick < 2600; ++tick)
    {
        const auto now = Layers(slowing, tick), next = Layers(slowing, tick + 1);
        for (const auto* animation : {"pb_combatrun_forward_loop", "pb_stand_shoot_walk_forward", "pb_stand_alert"})
            largest = std::max(largest, std::abs(LegsOf(next, animation) - LegsOf(now, animation)));
    }
    CHECK(largest <= 1.0f / 120.0f + 1e-4f);
}

TEST("locomotion: a movement animation is as far into it as its rates over time have played it")
{
    // a second at 160 units a second (0.758x), then at 240 (1.137x): both run
    const auto actor = Mover({Walking("pb_combatrun_forward_loop", 0, 1000, 160.0f),
                              Walking("pb_combatrun_forward_loop", 1000, 2000, 240.0f)});
    const auto timeAt = [&](std::uint32_t tick) {
        return Legs(Layers(actor, tick), "pb_combatrun_forward_loop")->time;
    };
    CHECK_NEAR(timeAt(1000), 160.0 / 211.0, 1e-4);
    CHECK_NEAR(timeAt(2000), 160.0 / 211.0 + 240.0 / 211.0, 1e-4);
    CHECK_NEAR(Legs(Layers(actor, 2000), "pb_combatrun_forward_loop")->rate, 240.0 / 211.0, 1e-4);
    // a tick on, a tick's worth at its rate, from wherever the timeline came
    CHECK_NEAR(timeAt(1501) - timeAt(1500), 0.001 * 240.0 / 211.0, 1e-5);
}

TEST("locomotion: a path is walked at the actor's speed, and the legs stand where it ends")
{
    auto actor = Mover({Clip("pb_combatrun_forward_loop", 0, 5000)}, 200.0f);
    actor.path = {{0, 0, 0}, {100, 0, 0}};
    const auto animations = Animations();
    CHECK_NEAR(ActorLogic::EvaluatePlacement(actor, 400, animations).position.x, 80.0, 0.5);
    CHECK_NEAR(ActorLogic::EvaluatePlacement(actor, 600, animations).position.x, 100.0, 1e-3);
    // it gets there at 500: then the run hands over to the idle in 250 ms
    CHECK_NEAR(LegsOf(Layers(actor, 400), "pb_combatrun_forward_loop"), 1.0, 1e-6);
    CHECK_NEAR(LegsOf(Layers(actor, 499), "pb_combatrun_forward_loop"), 1.0, 1e-6);
    CHECK_NEAR(LegsOf(Layers(actor, 625), "pb_stand_alert"), 0.5, 1e-5);
    CHECK_NEAR(LegsOf(Layers(actor, 750), "pb_stand_alert"), 1.0, 1e-6);

    // a walk fitted to a path takes as long as the path at the actor's speed, less half the idle's blend after it
    auto start = ActorLogic::MakeStart(ActorLogic::Start::WalkPath,
                                       {.idle = "pb_stand_alert",
                                        .walk = "pb_stand_shoot_walk_forward",
                                        .run = "pb_combatrun_forward_loop",
                                        .locomotion = true},
                                       1000, 20000, "");
    start.path = {{0, 0, 0}, {188, 0, 0}};
    ActorLogic::FitWalk(start, start.clips[0].id, animations, ActorLogic::WALK_SPEED_GUESS);
    const auto walkEnd = start.clips[0].End();
    CHECK_NEAR(static_cast<double>(start.clips[0].length), 2000.0 - 125.0, 15.0);
    CHECK_NEAR(ActorLogic::EvaluatePlacement(start, walkEnd + 250, animations).position.x, 188.0, 1e-3);
    CHECK_NEAR(ActorLogic::GetWalkedDistance(start, walkEnd + 250, animations), 188.0, 1.5);

    // twice as fast, the one walk is fitted again: half as long, still to the end, the idle starting where it ends
    start.pathSpeed = 188.0f;
    ActorLogic::FitOnlyWalk(start, animations);
    CHECK_NEAR(static_cast<double>(start.clips[0].length), 1000.0 - 125.0, 15.0);
    CHECK(start.clips[1].start == start.clips[0].End());
    CHECK_NEAR(ActorLogic::GetWalkedDistance(start, start.clips[0].End() + 250, animations), 188.0, 1.5);
    // not when two clips walk the path (a reload walking on with the run)
    auto two = Mover({Clip("pb_combatrun_forward_loop", 0, 1000), Clip("pt_reload_stand_auto", 1000, 2000, 0, false),
                      Clip("pb_stand_alert", 3000, 1000)},
                     150.0f);
    two.path = {{0, 0, 0}, {1000, 0, 0}};
    ActorLogic::FitOnlyWalk(two, animations);
    CHECK(two.clips[0].length == 1000 && two.clips[1].length == 2000);
}

TEST("locomotion: with keys, the legs go as fast as the actor goes between them, the way it goes against its facing")
{
    Types::Actor actor;
    actor.locomotion = Types::ActorLocomotion::Auto;
    actor.movement = Types::ActorMovement::Keyframes;
    actor.clips = {Clip("pb_stand_alert", 0, 10000)};
    // evenly along x, 211 units a second, facing +x
    for (std::uint32_t i = 0; i < 4; ++i)
        actor.keys.push_back({i * 1000, {i * 211.0f, 0, 0}, 0.0f});
    ActorLogic::Normalize(actor);
    auto layers = Layers(actor, 1500);
    const auto* run = Legs(layers, "pb_combatrun_forward_loop");
    CHECK(run && std::abs(run->weight - 1.0f) < 1e-5f && std::abs(run->rate - 1.0f) < 1e-3f);
    // standing after the last key
    CHECK_NEAR(LegsOf(Layers(actor, 4000), "pb_stand_alert"), 1.0, 1e-6);

    // along +y facing +x: strafing left, at 184 its run
    for (auto& key : actor.keys)
        key.position = {0, key.tick * 0.184f, 0};
    layers = Layers(actor, 1500);
    const auto* left = Legs(layers, "pb_combatrun_left_loop");
    CHECK(left && std::abs(left->weight - 1.0f) < 1e-5f && std::abs(left->rate - 1.0f) < 1e-3f);
    // without locomotion the idle is the idle
    actor.locomotion = Types::ActorLocomotion::Off;
    CHECK(Layers(actor, 1500).legs.empty());
}

TEST("locomotion: the legs library has what the locomotion may play")
{
    const auto animations = Animations();
    using Gun = Types::GunClass;
    // without it, the stance idles under upper body clips
    Types::Actor off;
    off.clips = {Clip("pt_stand_shoot", 0, 1000)};
    CHECK((ActorLogic::GetLegsLibrary(off, Gun::Rifle, animations) ==
           std::vector<std::string>{"pb_stand_alert", "pb_crouch_alert", "pb_prone_aim"}));
    off.clips = {Clip("pb_combatrun_forward_loop", 0, 1000)};
    CHECK(ActorLogic::GetLegsLibrary(off, Gun::Rifle, animations).empty());

    // with it on a path: standing's idle, walks, runs and sprint
    auto actor = Mover({Clip("pb_combatrun_forward_loop", 0, 1000)});
    auto library = ActorLogic::GetLegsLibrary(actor, Gun::Rifle, animations);
    CHECK(library.size() == 10);
    for (const auto* animation : {"pb_stand_alert", "pb_stand_shoot_walk_left", "pb_combatrun_back_loop", "pb_sprint"})
        CHECK(std::ranges::find(library, animation) != library.end());
    // crouching too where a clip's name says so, or crouching alone where the actor says so
    actor.clips.push_back(Clip("pb_crouch_run_forward", 1000, 1000));
    CHECK(ActorLogic::GetLegsLibrary(actor, Gun::Rifle, animations).size() == 19);
    actor.stance = Types::ActorStance::Crouch;
    library = ActorLogic::GetLegsLibrary(actor, Gun::Rifle, animations);
    CHECK(library.size() == 9 && library.front() == "pb_crouch_alert");
    // the gun's where the game has them, else the rifle's
    actor.stance = Types::ActorStance::Stand;
    library = ActorLogic::GetLegsLibrary(actor, Gun::Pistol, animations);
    CHECK(library.size() == 10);
    for (const auto* animation : {"pb_stand_alert_pistol", "pb_pistol_run_fast", "pb_combatrun_left_loop"})
        CHECK(std::ranges::find(library, animation) != library.end());
    // standing still its runs play as they are (no legs), and an idle clip stands for the idle; nothing for an actor
    // without such clips
    actor.movement = Types::ActorMovement::Still;
    CHECK(ActorLogic::GetLegsLibrary(actor, Gun::Rifle, animations).empty());
    actor.clips.push_back(Clip("pb_stand_alert", 2000, 1000));
    CHECK((ActorLogic::GetLegsLibrary(actor, Gun::Rifle, animations) == std::vector<std::string>{"pb_stand_alert"}));
    actor.clips = {Clip("casual_stand_idle", 0, 1000)};
    CHECK(ActorLogic::GetLegsLibrary(actor, Gun::Rifle, animations).empty());
}

TEST("locomotion: the starting points walk with it, and scenes save it; scenes from before walk as they did")
{
    ActorLogic::StartAnimations starts{.idle = "pb_stand_alert",
                                       .walk = "pb_stand_shoot_walk_forward",
                                       .run = "pb_combatrun_forward_loop",
                                       .locomotion = true};
    auto walking = ActorLogic::MakeStart(ActorLogic::Start::WalkPath, starts, 0, 10000, "");
    CHECK(walking.locomotion == Types::ActorLocomotion::Auto);
    CHECK_NEAR(walking.pathSpeed, 94.0, 1e-6);
    const auto running = ActorLogic::MakeStart(ActorLogic::Start::RunPath, starts, 0, 10000, "");
    CHECK_NEAR(running.pathSpeed, 211.0, 1e-6);
    starts.locomotion = false;
    CHECK(ActorLogic::MakeStart(ActorLogic::Start::WalkPath, starts, 0, 10000, "").locomotion ==
          Types::ActorLocomotion::Off);

    ActorLogic::Scene scene;
    scene.actors.push_back(running);
    scene.actors.push_back(ActorLogic::MakeStart(ActorLogic::Start::Standing, starts, 0, 10000, ""));
    const auto read = ActorLogic::SceneFromJson(ActorLogic::SceneToJson(scene, "demo"), 10000);
    CHECK(read.actors.at(0).locomotion == Types::ActorLocomotion::Auto);
    CHECK_NEAR(read.actors.at(0).pathSpeed, 211.0, 1e-6);
    CHECK(read.actors.at(1).locomotion == Types::ActorLocomotion::Off);
    auto old = ActorLogic::SceneToJson(scene, "demo");
    old["actors"][0].erase("locomotion");
    old["actors"][0].erase("pathSpeed");
    const auto before = ActorLogic::SceneFromJson(old, 10000).actors.at(0);
    CHECK(before.locomotion == Types::ActorLocomotion::Off);
    CHECK_NEAR(before.pathSpeed, Types::Actor{}.pathSpeed, 1e-6);

    // walking to a point at its speed: there when the walk has handed over to the idle
    Types::Actor actor;
    actor.locomotion = Types::ActorLocomotion::Auto;
    actor.pathSpeed = 211.0f;
    actor.clips = {Clip("pb_stand_alert", 0, 10000)};
    ActorLogic::Normalize(actor);
    const auto animations = Animations();
    const auto walk =
        ActorLogic::WalkTo(actor, 1000, {422, 0, 0}, "pb_combatrun_forward_loop", "pb_stand_alert", animations, 211.0f);
    const auto clip = std::ranges::find(actor.clips, walk, &Types::ActorClip::id);
    CHECK(clip != actor.clips.end() && actor.movement == Types::ActorMovement::Path);
    if (clip != actor.clips.end())
    {
        CHECK_NEAR(ActorLogic::EvaluatePlacement(actor, clip->End() + 250, animations).position.x, 422.0, 1e-3);
        CHECK_NEAR(LegsOf(Layers(actor, clip->start + 1000), "pb_combatrun_forward_loop"), 1.0, 1e-6);
    }
}
