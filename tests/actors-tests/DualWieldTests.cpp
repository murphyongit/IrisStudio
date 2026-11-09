#include "StdInclude.hpp"
#include "Test.hpp"

#include "Components/ActorLogic.hpp"

using namespace IWXMVM;

// A dual wield gun's left one (ActorLogic::GripsLeftGun): held at the grip unless the
// players' dual wield animations are all the actor shows

namespace
{
    using Kind = Types::AnimationKind;

    Types::ActorClip Clip(std::string animation, std::uint32_t start, std::uint32_t length, std::uint32_t blendIn = 0)
    {
        return {.animation = std::move(animation), .start = start, .length = length, .loop = true, .blendIn = blendIn};
    }

    Types::Actor ActorWith(std::vector<Types::ActorClip> clips)
    {
        Types::Actor actor;
        actor.clips = std::move(clips);
        ActorLogic::Normalize(actor);
        return actor;
    }

    // the dual wield shots and idles, a rifle's idle and a death, as the game has them
    ActorLogic::AnimationLookup Animations()
    {
        return [](const std::string& name) -> std::optional<ActorLogic::AnimationInfo> {
            if (name == "pt_dw_botharms_fire" || name == "pt_dw_botharms_reload" || name == "pt_stand_shoot")
                return ActorLogic::AnimationInfo{1.0f, glm::vec2(0), {}, Kind::Torso, Types::AnimationFamily::MpPlayer};
            for (const auto gun : {Types::GunClass::Rifle, Types::GunClass::DualWield})
                for (const auto stance :
                     {Types::ActorStance::Stand, Types::ActorStance::Crouch, Types::ActorStance::Prone})
                    if (name == ActorLogic::GetStanceIdle(gun, stance))
                        return ActorLogic::AnimationInfo{
                            10.0f, glm::vec2(0.2f, 0), {}, Kind::FullBody, Types::AnimationFamily::MpPlayer};
            if (name == "pb_stand_death_head_collapse")
                return ActorLogic::AnimationInfo{
                    2.0f, glm::vec2(0), {}, Kind::FullBody, Types::AnimationFamily::MpPlayer};
            return std::nullopt;
        };
    }

    ActorLogic::TreeWeights Weigh(const Types::Actor& actor, std::uint32_t tick)
    {
        const auto layers = ActorLogic::GetLayers(actor, tick, Animations(), Types::GunClass::DualWield);
        return ActorLogic::WeighLayers(layers, [](const std::string&) { return PoseLogic::SpineKind::Moves; });
    }

    ActorLogic::TreeWeights Weights(std::vector<std::pair<std::string, float>> animations)
    {
        ActorLogic::TreeWeights weights;
        for (auto& [animation, weight] : animations)
            weights.animations.push_back({.animation = std::move(animation), .weight = weight});
        return weights;
    }
}  // namespace

TEST("dual wield: the players' dual wield animations are those named with _dw_, in every stance")
{
    for (const auto name : {"pb_dw_botharms_idle_v1", "pt_dw_botharms_fire", "pt_crouch_dw_botharms_reload",
                            "pb_prone_dw_crawl_forward", "pt_dw_stand_melee_swipe"})
        CHECK(ActorLogic::IsDualWieldAnimation(name));
    for (const auto name : {"pb_stand_alert", "pt_stand_shoot", "pb_stand_death_head_collapse", "ai_dw_none", "dw_x"})
        CHECK(!ActorLogic::IsDualWieldAnimation(name));
}

TEST("dual wield: the left gun is left to dual wield animations, and gripped under anything else")
{
    CHECK(!ActorLogic::GripsLeftGun(Weights({{"pt_dw_botharms_fire", 1.0f}, {"pb_dw_botharms_idle_v1", 0.01f}})));
    CHECK(ActorLogic::GripsLeftGun(Weights({{"pt_stand_shoot", 1.0f}, {"pb_dw_botharms_idle_v1", 0.01f}})));
    CHECK(ActorLogic::GripsLeftGun(Weights({{"pb_stand_death_head_collapse", 1.0f}})));
    // nothing shown: at the grip, as the game draws an actor at rest
    CHECK(ActorLogic::GripsLeftGun(Weights({})));
    // a dual wield clip on the legs of another gun's idle (a hundredth): the clip's arms hold it
    CHECK(!ActorLogic::GripsLeftGun(Weights({{"pt_dw_botharms_fire", 1.0f}, {"pb_stand_alert", 0.01f}})));
    // a death blending in: gripped from LEFT_GUN_FREE_SHARE of the weight on
    CHECK(
        !ActorLogic::GripsLeftGun(Weights({{"pt_dw_botharms_fire", 0.96f}, {"pb_stand_death_head_collapse", 0.04f}})));
    CHECK(ActorLogic::GripsLeftGun(Weights({{"pt_dw_botharms_fire", 0.94f}, {"pb_stand_death_head_collapse", 0.06f}})));
}

TEST("dual wield: an actor that fires and reloads leaves the gun to the animations, and its death grips it")
{
    // the dual wield idle, a shot, a reload, then a death blending in over 200 ms
    const auto actor =
        ActorWith({Clip("pb_dw_botharms_idle_v1", 0, 1000), Clip("pt_dw_botharms_fire", 1000, 1000),
                   Clip("pt_dw_botharms_reload", 2000, 1000), Clip("pb_stand_death_head_collapse", 3000, 2000, 200)});
    for (const std::uint32_t tick : {500u, 1500u, 2500u, 2999u})
        CHECK(!ActorLogic::GripsLeftGun(Weigh(actor, tick)));
    CHECK(ActorLogic::GripsLeftGun(Weigh(actor, 3100)));
    CHECK(ActorLogic::GripsLeftGun(Weigh(actor, 4000)));
}

TEST("dual wield: a rifle's clip on a dual wield actor grips the left gun")
{
    const auto actor = ActorWith({Clip("pt_stand_shoot", 0, 1000)});
    CHECK(ActorLogic::GripsLeftGun(Weigh(actor, 500)));
}

TEST("dual wield: the guns take turns, by each shot's place among the actor's shots (#40)")
{
    Types::Actor actor;
    // listed out of order, as events added later are: a burst of 3 at 2000, one shot at 500, a burst of 2 at 1000,
    // and an effect, which isn't a shot
    actor.events = {{.id = 1, .type = Types::ActorEventType::Fire, .tick = 2000, .shots = 3},
                    {.id = 2, .type = Types::ActorEventType::Fire, .tick = 500},
                    {.id = 3, .type = Types::ActorEventType::Effect, .tick = 700},
                    {.id = 4, .type = Types::ActorEventType::Fire, .tick = 1000, .shots = 2}};
    CHECK(ActorLogic::GetShotPlace(actor, actor.events[1], 0) == 0);
    CHECK(ActorLogic::GetShotPlace(actor, actor.events[3], 0) == 1);
    CHECK(ActorLogic::GetShotPlace(actor, actor.events[3], 1) == 2);
    CHECK(ActorLogic::GetShotPlace(actor, actor.events[0], 0) == 3);
    CHECK(ActorLogic::GetShotPlace(actor, actor.events[0], 2) == 5);
    // right, left, right...
    CHECK(!ActorLogic::IsLeftGunShot(0));
    CHECK(ActorLogic::IsLeftGunShot(1));
    CHECK(!ActorLogic::IsLeftGunShot(2));
    CHECK(ActorLogic::IsLeftGunShot(5));
    // two events at the same tick: by their ids
    actor.events[3].tick = 500;
    CHECK(ActorLogic::GetShotPlace(actor, actor.events[1], 0) == 0);
    CHECK(ActorLogic::GetShotPlace(actor, actor.events[3], 0) == 1);
}

TEST("dual wield: a clip's shot notetracks are counted from its start, its plays included")
{
    // an AI shot of two fire notetracks a play, looping over 1 s plays
    static const std::vector<Types::ActorNotetrack> notetracks = {
        {"fire", 0.25f}, {"sndnt#weap_fire", 0.3f}, {"fire", 0.75f}, {"end", 1.0f}};
    const ActorLogic::AnimationLookup animations =
        [](const std::string& name) -> std::optional<ActorLogic::AnimationInfo> {
            if (name != "ai_shoot")
                return std::nullopt;
            return ActorLogic::AnimationInfo{1.0f, glm::vec2(0), notetracks, Kind::Torso,
                                             Types::AnimationFamily::SpHuman};
        };
    const auto actor = ActorWith({Clip("ai_shoot", 0, 3000)});
    std::vector<std::uint32_t> shots;
    for (std::uint32_t tick = 0; tick < 3000; tick += 100)
        for (const auto& passed : ActorLogic::GetPassedNotetracks(actor, tick, tick + 100, animations))
            if (ActorLogic::IsShotNotetrack(passed.name))
                shots.push_back(passed.shot);
    CHECK((shots == std::vector<std::uint32_t>{0, 1, 2, 3, 4, 5}));
    // played from the middle (a jump, or the timeline started there): the same places
    const auto later = ActorLogic::GetPassedNotetracks(actor, 2000, 2300, animations);
    CHECK(later.size() == 2 && later[0].name == "fire" && later[0].shot == 4);
}
