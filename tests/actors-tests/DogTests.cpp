#include "StdInclude.hpp"
#include "Test.hpp"

#include "Components/ActorLogic.hpp"

using namespace IWXMVM;

// Dogs: their locomotion with the dogs' own animations, their defaults, and an actor becoming a dog or a person again
// (ActorLogic::Body, ChooseLocomotion, GetLayers, GetDogStartAnimations, ChangeBody)

namespace
{
    using Kind = Types::AnimationKind;
    using Family = Types::AnimationFamily;
    using Gait = ActorLogic::Gait;
    using Heading = ActorLogic::Heading;
    using Stance = Types::ActorStance;
    using Breed = ActorLogic::DogBreed;

    // The dogs' animations as the catalog of every animation has them (their motion in a play and duration), and the
    // players' that the tests turn into them
    struct Known
    {
        std::string_view name;
        glm::vec2 motion;  // in a play
        float duration = 1.0f;
        Kind kind = Kind::Other;
        Family family = Family::Dog;
        bool death = false;
    };
    const std::vector<Known>& Dogs()
    {
        static const std::vector<Known> known = {
            {"german_shepherd_idle", {0, 0}, 3.1f},
            {"german_shepherd_walk", {52.4f, 0}, 0.8f},
            {"german_shepherd_run", {127.9f, 0}, 0.4f},
            {"german_shepherd_death_front", {-119.2f, -18.1f}, 1.1667f, Kind::Other, Family::Dog, true},
            {"zombie_dog_idle", {0, 0}, 3.0667f},
            {"zombie_dog_walk", {238.2f, 0}, 4.5f},
            {"zombie_dog_trot", {245.1f, 0}, 1.8f},
            {"zombie_dog_run", {724.3f, 0}, 2.0f},
            {"zombie_dog_death_front", {0, 0}, 1.1667f, Kind::Other, Family::Dog, true},
        };
        return known;
    }
    const std::vector<Known>& People()
    {
        static const std::vector<Known> known = {
            {"pb_stand_alert", {0.2f, 0}, 10.0f, Kind::FullBody, Family::MpPlayer},
            {"pb_stand_shoot_walk_forward", {94, 0}, 1.0f, Kind::FullBody, Family::MpPlayer},
            {"pb_combatrun_forward_loop", {211, 0}, 1.0f, Kind::FullBody, Family::MpPlayer},
            {"pb_sprint", {281, 0}, 1.0f, Kind::FullBody, Family::MpPlayer},
            {"pb_stand_death_headchest_topple", {0, 0}, 2.0f, Kind::FullBody, Family::MpPlayer, true},
            {"pt_reload_stand_auto", {0, 0}, 2.0f, Kind::Torso, Family::MpPlayer},
            {"pf_casual_idle", {0, 0}, 36.0f, Kind::Face, Family::Human},
        };
        return known;
    }

    // the dogs' and the people's; without the hellhound's, as a game without the animation pack
    ActorLogic::AnimationLookup Animations(bool hellhound = true)
    {
        return [hellhound](const std::string& name) -> std::optional<ActorLogic::AnimationInfo> {
            for (const auto* list : {&Dogs(), &People()})
            {
                const auto found = std::ranges::find(*list, name, &Known::name);
                if (found == list->end() || (!hellhound && name.starts_with("zombie_dog")))
                    continue;
                ActorLogic::AnimationInfo info{found->duration, found->motion, {}, found->kind, found->family};
                info.death = found->death;
                return info;
            }
            return std::nullopt;
        };
    }

    const auto SHEPHERD = ActorLogic::Body::Dog(Breed::Shepherd);
    const auto HELLHOUND = ActorLogic::Body::Dog(Breed::Hellhound);

    Types::ActorClip Clip(std::string animation, std::uint32_t start, std::uint32_t length)
    {
        return {.animation = std::move(animation), .start = start, .length = length, .blendIn = 0};
    }

    // an actor with Locomotion Auto on a long path along +x, its walking clips at a speed
    Types::Actor Mover(std::vector<Types::ActorClip> clips, float speed)
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

    // the animation the legs play most at a tick
    std::string LegsAt(const Types::Actor& actor, std::uint32_t tick, const ActorLogic::Body& body)
    {
        const auto layers = ActorLogic::GetLayers(actor, tick, Animations(), body);
        if (layers.legs.empty())
            return {};
        const auto& legs = *std::ranges::max_element(layers.legs, {}, &ActorLogic::LegsSample::weight);
        return legs.clip ? legs.clip->animation : legs.animation;
    }

    bool IsDogAnimation(std::string_view name)
    {
        return name.starts_with("german_shepherd") || name.starts_with("zombie_dog");
    }
}  // namespace

TEST("dogs: a breed's animations of each gait, and which of them an animation is")
{
    CHECK(ActorLogic::GetDogAnimation(Breed::Shepherd, Gait::Idle) == "german_shepherd_idle");
    CHECK(ActorLogic::GetDogAnimation(Breed::Shepherd, Gait::Walk) == "german_shepherd_walk");
    // the shepherd's run is its fastest
    CHECK(ActorLogic::GetDogAnimation(Breed::Shepherd, Gait::Run) == "german_shepherd_run");
    CHECK(ActorLogic::GetDogAnimation(Breed::Shepherd, Gait::Sprint) == "german_shepherd_run");
    // the hellhound trots, and runs as its sprint
    CHECK(ActorLogic::GetDogAnimation(Breed::Hellhound, Gait::Run) == "zombie_dog_trot");
    CHECK(ActorLogic::GetDogAnimation(Breed::Hellhound, Gait::Sprint) == "zombie_dog_run");
    CHECK(ActorLogic::GetDogDeath(Breed::Shepherd) == "german_shepherd_death_front");
    CHECK(ActorLogic::GetDogDeath(Breed::Hellhound) == "zombie_dog_death_front");
    // a dog's whatever the stance and heading
    CHECK(ActorLogic::GetLocomotionAnimation(SHEPHERD, Stance::Crouch, Gait::Walk, Heading::Back) ==
          "german_shepherd_walk");

    const auto run = ActorLogic::FindLocomotion("german_shepherd_run");
    CHECK(run && run->gait == Gait::Run && run->dog == Breed::Shepherd && run->heading == Heading::Forward);
    const auto trot = ActorLogic::FindLocomotion("zombie_dog_trot");
    CHECK(trot && trot->gait == Gait::Run && trot->dog == Breed::Hellhound);
    const auto sprint = ActorLogic::FindLocomotion("zombie_dog_run");
    CHECK(sprint && sprint->gait == Gait::Sprint && sprint->dog == Breed::Hellhound);
    CHECK(ActorLogic::FindLocomotion("german_shepherd_idle")->gait == Gait::Idle);
    // a person's aren't a dog's
    CHECK(!ActorLogic::FindLocomotion("pb_combatrun_forward_loop")->dog.has_value());
    CHECK(!ActorLogic::FindLocomotion("german_shepherd_death_front"));

    // how fast they go before the game has measured them: as the catalog has them
    const auto unmeasured = [](const std::string&) { return std::optional<ActorLogic::AnimationInfo>(); };
    CHECK_NEAR(ActorLogic::GetAnimationSpeed("german_shepherd_walk", unmeasured), 65.5f, 0.1f);
    CHECK_NEAR(ActorLogic::GetAnimationSpeed("german_shepherd_run", unmeasured), 319.7f, 0.1f);
    CHECK_NEAR(ActorLogic::GetAnimationSpeed("zombie_dog_trot", unmeasured), 136.2f, 0.1f);
    // and measured, as the game measured them
    CHECK_NEAR(ActorLogic::GetAnimationSpeed("german_shepherd_run", Animations()), 319.75f, 0.01f);
}

TEST("dogs: Locomotion Auto picks the dog's walk and run for its speed, never a person's")
{
    const auto animations = Animations();
    const auto choose = [&](const ActorLogic::Body& body, float speed, float angle = 0.0f,
                            Stance stance = Stance::Auto) {
        return ActorLogic::ChooseLocomotion(body, stance, speed, angle, animations);
    };
    CHECK(choose(SHEPHERD, 0.0f).animation == "german_shepherd_idle");
    const auto walk = choose(SHEPHERD, 60.0f);
    CHECK(walk.animation == "german_shepherd_walk" && walk.gait == Gait::Walk);
    CHECK_NEAR(walk.rate, 60.0f / 65.5f, 0.001f);
    // the run above halfway between the walk's and the run's speeds (192.6), as fast as the speed asks
    CHECK(choose(SHEPHERD, 180.0f).animation == "german_shepherd_walk");
    const auto run = choose(SHEPHERD, 211.0f);
    CHECK(run.animation == "german_shepherd_run" && run.gait == Gait::Run);
    CHECK_NEAR(run.rate, 211.0f / 319.75f, 0.001f);
    // no sprint: faster runs faster, up to twice its speed
    const auto fast = choose(SHEPHERD, 900.0f);
    CHECK(fast.animation == "german_shepherd_run" && fast.rate == ActorLogic::MAX_LOCOMOTION_RATE);
    // one stance and one way: crouching or going back, it still walks forward on its feet
    CHECK(choose(SHEPHERD, 60.0f, 180.0f).animation == "german_shepherd_walk");
    CHECK(choose(SHEPHERD, 60.0f, 180.0f).heading == Heading::Forward);
    CHECK(choose(SHEPHERD, 60.0f, 0.0f, Stance::Prone).animation == "german_shepherd_walk");

    // the hellhound: walk, trot, run
    CHECK(choose(HELLHOUND, 50.0f).animation == "zombie_dog_walk");
    CHECK(choose(HELLHOUND, 140.0f).animation == "zombie_dog_trot");
    CHECK(choose(HELLHOUND, 360.0f).animation == "zombie_dog_run");
    CHECK(choose(HELLHOUND, 360.0f).gait == Gait::Sprint);
    // without the animation pack, the shepherd's
    CHECK(ActorLogic::ChooseLocomotion(HELLHOUND, Stance::Auto, 300.0f, 0.0f, Animations(false)).animation ==
          "german_shepherd_run");

    // never a person's, at any speed, way and stance
    for (const auto& body : {SHEPHERD, HELLHOUND})
        for (float speed = 0.0f; speed <= 800.0f; speed += 7.0f)
            for (const auto angle : {0.0f, 45.0f, 90.0f, 180.0f, -90.0f})
                for (const auto stance : {Stance::Auto, Stance::Stand, Stance::Crouch, Stance::Prone})
                    CHECK(IsDogAnimation(choose(body, speed, angle, stance).animation));
    // and a person's a person's
    CHECK(choose(Types::GunClass::Rifle, 211.0f).animation == "pb_combatrun_forward_loop");
}

TEST("dogs: a dog's legs are its locomotion's, from its clips of either breed's or a person's")
{
    // a run along the path at the shepherd's own speed, then the idle
    const auto dog = Mover({Clip("german_shepherd_run", 1000, 2000), Clip("german_shepherd_idle", 3000, 3000)}, 320.0f);
    CHECK(LegsAt(dog, 2000, SHEPHERD) == "german_shepherd_run");
    const auto layers = ActorLogic::GetLayers(dog, 2000, Animations(), SHEPHERD);
    CHECK(layers.clipCount == 1 && layers.locomotion[0] && !layers.upperBody[0]);
    CHECK(layers.legs.size() == 1 && std::abs(layers.legs[0].rate - 320.0f / 319.75f) < 0.001f);
    CHECK(LegsAt(dog, 5500, SHEPHERD) == "german_shepherd_idle");
    // the same clips on a hellhound play the hellhound's: 320 runs (above 249, halfway from its trot), 140 trots
    CHECK(LegsAt(dog, 2000, HELLHOUND) == "zombie_dog_run");
    auto slower = dog;
    slower.pathSpeed = 140.0f;
    CHECK(LegsAt(slower, 2000, HELLHOUND) == "zombie_dog_trot");

    // a person's clips (a starting point's, before the actor became a dog) play the dog's
    const auto person =
        Mover({Clip("pb_combatrun_forward_loop", 1000, 2000), Clip("pb_stand_alert", 3000, 3000)}, 211.0f);
    CHECK(LegsAt(person, 2000, SHEPHERD) == "german_shepherd_run");
    CHECK(LegsAt(person, 5500, SHEPHERD) == "german_shepherd_idle");
    CHECK(LegsAt(person, 2000, Types::GunClass::Rifle) == "pb_combatrun_forward_loop");

    // no legs under a person's upper body clip: those are a person's
    Types::Actor reload;
    reload.clips = {Clip("pt_reload_stand_auto", 1000, 2000)};
    CHECK(ActorLogic::GetLayers(reload, 1500, Animations(), SHEPHERD).legs.empty());
    CHECK(!ActorLogic::GetLayers(reload, 1500, Animations(), Types::GunClass::Rifle).legs.empty());
}

TEST("dogs: the legs' library is the breed's idle, walk, run and sprint")
{
    const auto animations = Animations();
    const auto dog = Mover({Clip("german_shepherd_walk", 1000, 2000), Clip("german_shepherd_idle", 3000, 3000)}, 66.0f);
    CHECK((ActorLogic::GetLegsLibrary(dog, SHEPHERD, animations) ==
           std::vector<std::string>{"german_shepherd_idle", "german_shepherd_walk", "german_shepherd_run"}));
    CHECK((ActorLogic::GetLegsLibrary(dog, HELLHOUND, animations) ==
           std::vector<std::string>{"zombie_dog_idle", "zombie_dog_walk", "zombie_dog_trot", "zombie_dog_run"}));
    // standing still: the idle; Locomotion Off: none, as no legs play under its clips
    auto still = dog;
    still.movement = Types::ActorMovement::Still;
    CHECK(
        (ActorLogic::GetLegsLibrary(still, SHEPHERD, animations) == std::vector<std::string>{"german_shepherd_idle"}));
    auto off = dog;
    off.locomotion = Types::ActorLocomotion::Off;
    off.clips.push_back(Clip("pt_reload_stand_auto", 7000, 2000));
    ActorLogic::Normalize(off);
    CHECK(ActorLogic::GetLegsLibrary(off, SHEPHERD, animations).empty());
    // nothing of a person's in a dog's
    for (const auto& name : ActorLogic::GetLegsLibrary(dog, HELLHOUND, animations))
        CHECK(IsDogAnimation(name));

    // the speed presets: a person's, a breed's
    const auto shepherd = ActorLogic::GetSpeedPresets(SHEPHERD);
    CHECK(shepherd.size() == 2 && shepherd[0].name == "Walk" && shepherd[0].speed == 66.0f &&
          shepherd[1].name == "Run" && shepherd[1].speed == 320.0f);
    const auto hellhound = ActorLogic::GetSpeedPresets(HELLHOUND);
    CHECK(hellhound.size() == 3 && hellhound[1].name == "Trot" && hellhound[2].gait == Gait::Sprint);
    CHECK(ActorLogic::GetSpeedPresets(Types::GunClass::Pistol).size() == ActorLogic::SPEED_PRESETS.size());
}

TEST("dogs: what a dog starts with, and its starting points")
{
    const auto shepherd = ActorLogic::GetDogStartAnimations(Breed::Shepherd, Animations());
    CHECK(shepherd.idle == "german_shepherd_idle" && shepherd.walk == "german_shepherd_walk" &&
          shepherd.run == "german_shepherd_run");
    CHECK(shepherd.death == "german_shepherd_death_front" && std::abs(shepherd.deathDuration - 1.1667f) < 0.001f);
    CHECK(shepherd.locomotion && !shepherd.face && shepherd.shoot.empty());
    CHECK(shepherd.walkSpeed == 66.0f && shepherd.runSpeed == 320.0f);
    const auto hellhound = ActorLogic::GetDogStartAnimations(Breed::Hellhound, Animations());
    CHECK(hellhound.idle == "zombie_dog_idle" && hellhound.run == "zombie_dog_trot" &&
          hellhound.death == "zombie_dog_death_front" && hellhound.runSpeed == 136.0f);
    // without the animation pack, the shepherd's (every map has them), at its speeds
    const auto fallback = ActorLogic::GetDogStartAnimations(Breed::Hellhound, Animations(false));
    CHECK(fallback.walk == "german_shepherd_walk" && fallback.death == "german_shepherd_death_front" &&
          fallback.walkSpeed == 66.0f);

    // running a path: the dog's run at its speed, then its idle; no gun, no face
    const auto runner = ActorLogic::MakeStart(ActorLogic::Start::RunPath, shepherd, 1000, 60000, {});
    CHECK(runner.locomotion == Types::ActorLocomotion::Auto && runner.pathSpeed == 320.0f);
    CHECK(runner.clips.size() == 2 && runner.clips[0].animation == "german_shepherd_run" &&
          runner.clips[1].animation == "german_shepherd_idle");
    CHECK(runner.weapon.empty() && runner.face == Types::ActorFace::Off);
    CHECK(ActorLogic::MakeStart(ActorLogic::Start::WalkPath, shepherd, 1000, 60000, {}).pathSpeed == 66.0f);
    // dying on cue: the dog's death a second later, which falls back where its root motion takes it
    const auto dies = ActorLogic::MakeStart(ActorLogic::Start::Die, shepherd, 1000, 60000, {});
    CHECK(dies.clips.size() == 2 && dies.clips[1].animation == "german_shepherd_death_front" &&
          dies.clips[1].start == 1000 + ActorLogic::DIE_AFTER && !dies.clips[1].loop && !dies.clips[1].moves);
    CHECK(dies.events.size() == 1 && dies.events[0].type == Types::ActorEventType::Hit);
}

TEST("dogs: a person who becomes a dog does what it did as the dog does it, and back")
{
    const auto animations = Animations();
    // a person running a path of 640 units (a starting point's), with a reload after it and a death at the end
    ActorLogic::StartAnimations person{.idle = "pb_stand_alert",
                                       .walk = "pb_stand_shoot_walk_forward",
                                       .run = "pb_combatrun_forward_loop",
                                       .death = "pb_stand_death_headchest_topple",
                                       .deathDuration = 2.0f,
                                       .locomotion = true,
                                       .face = true};
    auto actor = ActorLogic::MakeStart(ActorLogic::Start::RunPath, person, 1000, 60000, {});
    actor.path = {{0, 0, 0}, {640, 0, 0}};
    ActorLogic::FitOnlyWalk(actor, animations);
    // (the idle's blend in walks the last few units)
    const auto runLength = actor.clips[0].length;
    CHECK(runLength > 2800 && runLength < 3100);
    ActorLogic::AddClip(actor, Clip("pt_reload_stand_auto", 20000, 2000));
    ActorLogic::AddDeath(actor, 30000, "pb_stand_death_headchest_topple", 2.0f);
    actor.face = Types::ActorFace::Auto;
    actor.weapon = "ak47_mp";

    const auto shepherd = ActorLogic::GetDogStartAnimations(Breed::Shepherd, animations);
    auto dog = actor;
    ActorLogic::ChangeBody(dog, Types::GunClass::Rifle, SHEPHERD, shepherd, animations);
    std::vector<std::string> clips;
    for (const auto& clip : dog.clips)
        clips.push_back(clip.animation);
    // its run and idle are the dog's, its death the dog's (a play of it), the reload stays (it doesn't fit)
    CHECK((clips == std::vector<std::string>{"german_shepherd_run", "german_shepherd_idle", "pt_reload_stand_auto",
                                             "german_shepherd_death_front"}));
    CHECK(dog.clips.back().length == 1167 && !dog.clips.back().loop);
    // it runs at the dog's Run: the path in two seconds
    CHECK(dog.pathSpeed == 320.0f);
    CHECK(dog.clips[0].length > 1800 && dog.clips[0].length < 2050);
    CHECK(dog.face == Types::ActorFace::Off && dog.stance == Types::ActorStance::Auto);
    // the gun stays the scene's (a dog holds none), for when it is a person again
    CHECK(dog.weapon == "ak47_mp");

    // and back: a person's again, at the person's Run
    auto again = dog;
    ActorLogic::ChangeBody(again, SHEPHERD, Types::GunClass::Rifle, person, animations);
    CHECK(again.clips[0].animation == "pb_combatrun_forward_loop" && again.clips[1].animation == "pb_stand_alert");
    CHECK(again.clips.back().animation == "pb_stand_death_headchest_topple" && again.clips.back().length == 2000);
    CHECK(again.pathSpeed == 211.0f && again.face == Types::ActorFace::Auto);
    CHECK(again.clips[0].length == runLength);

    // the same body: nothing changes
    auto same = actor;
    ActorLogic::ChangeBody(same, Types::GunClass::Rifle, Types::GunClass::Rifle, person, animations);
    CHECK(same == actor);
    // another person's body (the user's request, 2026-10-28): its clips stay (no ChangeBody), and Moves as Auto keeps
    // the set it stood for where the new body would pick another (an MP body made a campaign one, which Auto would
    // have walk with the AI's low ready); an explicit set, or one Auto picks the same, stays as it is
    using Set = Types::ActorMovementSet;
    auto person2 = actor;
    person2.movementSet = Set::Auto;
    ActorLogic::KeepMovementSet(person2, Set::Players, Set::LowReady);
    CHECK(person2.movementSet == Set::Players);
    ActorLogic::KeepMovementSet(person2, Set::LowReady, Set::Players);
    CHECK(person2.movementSet == Set::Players);
    auto campaign = actor;
    campaign.movementSet = Set::Auto;
    ActorLogic::KeepMovementSet(campaign, Set::LowReady, Set::LowReady);
    CHECK(campaign.movementSet == Set::Auto);
    ActorLogic::KeepMovementSet(campaign, Set::LowReady, Set::Players);
    CHECK(campaign.movementSet == Set::LowReady);
    CHECK(campaign.clips == actor.clips);
    // one still as it was added (standing, the Actors tab's + Add) has nothing to keep yet: made a campaign character,
    // Auto goes with the AI's set
    auto added = ActorLogic::MakeStart(ActorLogic::Start::Standing, person, 1000, 60000, {});
    CHECK(ActorLogic::IsAsAdded(added));
    ActorLogic::KeepMovementSet(added, Set::Players, Set::LowReady);
    CHECK(added.movementSet == Set::Auto);
    CHECK(!ActorLogic::IsAsAdded(actor));
    // a shepherd becoming a hellhound runs the hellhound's: the shepherd's Run is its Trot
    auto hound = dog;
    ActorLogic::ChangeBody(hound, SHEPHERD, HELLHOUND, ActorLogic::GetDogStartAnimations(Breed::Hellhound, animations),
                           animations);
    CHECK(hound.clips[0].animation == "zombie_dog_trot" && hound.clips.back().animation == "zombie_dog_death_front");
    CHECK(hound.pathSpeed == 136.0f);
}
