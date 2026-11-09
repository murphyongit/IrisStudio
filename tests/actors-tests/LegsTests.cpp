#include "StdInclude.hpp"
#include "Test.hpp"

#include "Components/ActorLogic.hpp"

using namespace IWXMVM;

// The legs under clips of upper body animations (ActorLogic::GetLayers, WeighLayers)

namespace
{
    using Kind = Types::AnimationKind;

    Types::ActorClip Clip(std::string animation, std::uint32_t start, std::uint32_t length, std::uint32_t blendIn = 0,
                          bool loop = true)
    {
        return {.animation = std::move(animation), .start = start, .length = length, .loop = loop, .blendIn = blendIn};
    }

    Types::Actor ActorWith(std::vector<Types::ActorClip> clips, Types::ActorStance stance = Types::ActorStance::Auto)
    {
        Types::Actor actor;
        actor.clips = std::move(clips);
        actor.stance = stance;
        ActorLogic::Normalize(actor);
        return actor;
    }

    // The game's animations as the tests know them: upper body ones (pt_), the stance idles of the rifle, pistol and
    // rocket launcher (not those of other guns), a run that walks, an SP idle
    ActorLogic::AnimationLookup Animations()
    {
        return [](const std::string& name) -> std::optional<ActorLogic::AnimationInfo> {
            // a rocket launcher's reload made for a body kneeling from a tenth of it to 0.8, two seconds
            static const std::vector<Types::ActorNotetrack> kneeling = {
                {"anim_pose = \"crouch\"", 0.1f}, {"reload done", 0.7f}, {"anim_pose = \"stand\"", 0.8f}};
            // crouching and up again within 400 ms
            static const std::vector<Types::ActorNotetrack> bobbing = {{"anim_pose = \"crouch\"", 0.1f},
                                                                       {"anim_pose = \"stand\"", 0.2f}};
            if (name == "pt_stand_shoot" || name == "pt_reload_crouch" || name == "pt_reload_prone")
                return ActorLogic::AnimationInfo{1.0f, glm::vec2(0), {}, Kind::Torso, Types::AnimationFamily::MpPlayer};
            if (name == "pt_reload_stand_rpg")
                return ActorLogic::AnimationInfo{2.0f, glm::vec2(0), kneeling, Kind::Torso,
                                                 Types::AnimationFamily::MpPlayer};
            if (name == "pt_bob")
                return ActorLogic::AnimationInfo{2.0f, glm::vec2(0), bobbing, Kind::Torso,
                                                 Types::AnimationFamily::MpPlayer};
            for (const auto gun : {Types::GunClass::Rifle, Types::GunClass::Pistol, Types::GunClass::RocketLauncher})
                for (const auto stance :
                     {Types::ActorStance::Stand, Types::ActorStance::Crouch, Types::ActorStance::Prone})
                    if (name == ActorLogic::GetStanceIdle(gun, stance))
                        return ActorLogic::AnimationInfo{
                            10.0f, glm::vec2(0.2f, 0), {}, Kind::FullBody, Types::AnimationFamily::MpPlayer};
            if (name == "pb_combatrun")
                return ActorLogic::AnimationInfo{
                    1.0f, glm::vec2(211, 0), {}, Kind::FullBody, Types::AnimationFamily::MpPlayer};
            if (name == "casual_idle")
                return ActorLogic::AnimationInfo{
                    5.0f, glm::vec2(0), {}, Kind::FullBody, Types::AnimationFamily::SpHuman};
            if (name == "pf_firing")
                return ActorLogic::AnimationInfo{4.0f, glm::vec2(0), {}, Kind::Face, Types::AnimationFamily::Human};
            return std::nullopt;
        };
    }

    // what the animations do with an MP body's stabilizer: the players' full body ones move it, SP ones line it up
    PoseLogic::SpineKind Spine(const std::string& animation)
    {
        if (animation.starts_with("pb_"))
            return PoseLogic::SpineKind::Moves;
        if (animation.starts_with("casual"))
            return PoseLogic::SpineKind::Lined;
        return PoseLogic::SpineKind::AtRest;
    }

    ActorLogic::Layers Layers(const Types::Actor& actor, std::uint32_t tick,
                              Types::GunClass gun = Types::GunClass::Rifle)
    {
        return ActorLogic::GetLayers(actor, tick, Animations(), gun);
    }

    ActorLogic::TreeWeights Weigh(const Types::Actor& actor, std::uint32_t tick,
                                  Types::GunClass gun = Types::GunClass::Rifle)
    {
        return ActorLogic::WeighLayers(Layers(actor, tick, gun), Spine);
    }

    // the legs' weight of an animation, 0 when they don't play it
    float LegsOf(const ActorLogic::Layers& layers, std::string_view animation)
    {
        float weight = 0.0f;
        for (const auto& legs : layers.legs)
            if ((legs.clip ? legs.clip->animation : legs.animation) == animation)
                weight += legs.weight;
        return weight;
    }

    // The engine's blend of one bone (KisakCOD's XAnimCalc, as T5 has it): each animation that moves the bone in the
    // ratio of the weights of those that do. A body's bones are moved by: the legs by full body animations alone, the
    // upper body by all of them, the stabilizer by the full body ones of MP bodies and the made up ones of it.
    enum class Bone
    {
        Legs,
        UpperBody,
        Stabilizer,
    };
    // the share of the bone the animations a test picks have (the made up ones of the stabilizer are named lined and
    // resting)
    float ShareOf(const ActorLogic::TreeWeights& weights, Bone bone,
                  const std::function<bool(const ActorLogic::TreeWeight*, std::string_view)>& picked)
    {
        float total = 0.0f, share = 0.0f;
        const auto add = [&](const ActorLogic::TreeWeight* entry, std::string_view name, float weight) {
            total += weight;
            if (picked(entry, name))
                share += weight;
        };
        for (const auto& entry : weights.animations)
        {
            const auto& name = entry.clip ? entry.clip->animation : entry.animation;
            const auto info = Animations()(name);
            const bool fullBody = info && info->kind == Kind::FullBody;
            if (bone == Bone::UpperBody ||
                (fullBody && (bone == Bone::Legs || Spine(name) == PoseLogic::SpineKind::Moves)))
                add(&entry, name, entry.weight);
        }
        if (bone == Bone::Stabilizer)
        {
            add(nullptr, "lined", weights.lined);
            add(nullptr, "resting", weights.resting);
            for (const auto& [legs, weight] : weights.aimed)
                add(nullptr, "aimed", weight);
        }
        return total > 0.0f ? share / total : 0.0f;
    }
    float ShareOf(const ActorLogic::TreeWeights& weights, Bone bone, std::string_view animation)
    {
        return ShareOf(weights, bone,
                       [&](const ActorLogic::TreeWeight*, std::string_view name) { return name == animation; });
    }
    // the share of the clips' animations (not the idles')
    float ClipsShareOf(const ActorLogic::TreeWeights& weights, Bone bone)
    {
        return ShareOf(weights, bone,
                       [](const ActorLogic::TreeWeight* entry, std::string_view) { return entry && entry->clip; });
    }

    // the legs' weight of the clip they go on with
    float LegsOfClip(const ActorLogic::Layers& layers)
    {
        float weight = 0.0f;
        for (const auto& legs : layers.legs)
            if (legs.clip)
                weight += legs.weight;
        return weight;
    }
}  // namespace

TEST("legs: the stance comes from an upper body animation's name and its anim_pose notetracks")
{
    CHECK(ActorLogic::GetNameStance("pt_reload_stand_auto") == Types::ActorStance::Stand);
    CHECK(ActorLogic::GetNameStance("pt_reload_crouch_rifle") == Types::ActorStance::Crouch);
    CHECK(ActorLogic::GetNameStance("pt_reload_prone_auto") == Types::ActorStance::Prone);
    CHECK(ActorLogic::GetNotetrackStance("anim_pose = \"crouch\"") == Types::ActorStance::Crouch);
    CHECK(ActorLogic::GetNotetrackStance("anim_pose = \"stand\"") == Types::ActorStance::Stand);
    CHECK(ActorLogic::GetNotetrackStance("anim_pose=prone") == Types::ActorStance::Prone);
    CHECK(ActorLogic::GetNotetrackStance("anim_pose = \"crawl\"") == Types::ActorStance::Prone);
    CHECK(!ActorLogic::GetNotetrackStance("anim_movement = \"stop\""));
    CHECK(!ActorLogic::GetNotetrackStance("reload done"));
}

TEST("legs: each gun class stands in its own idles, the rifle's where the game hasn't the gun's")
{
    using Gun = Types::GunClass;
    using Stance = Types::ActorStance;
    CHECK(ActorLogic::GetStanceIdle(Gun::Rifle, Stance::Stand) == "pb_stand_alert");
    CHECK(ActorLogic::GetStanceIdle(Gun::Rifle, Stance::Crouch) == "pb_crouch_alert");
    CHECK(ActorLogic::GetStanceIdle(Gun::Rifle, Stance::Prone) == "pb_prone_aim");
    CHECK(ActorLogic::GetStanceIdle(Gun::Pistol, Stance::Crouch) == "pb_crouch_alert_pistol");
    CHECK(ActorLogic::GetStanceIdle(Gun::RocketLauncher, Stance::Stand) == "pb_stand_alert_rpg");
    CHECK(ActorLogic::GetStanceIdle(Gun::DualWield, Stance::Stand) == "pb_dw_botharms_idle_v1");
    CHECK(ActorLogic::GetStanceIdle(Gun::DualWield, Stance::Prone) == "pb_prone_dw_idle_v1");
    CHECK(ActorLogic::GetStanceIdle(Gun::BallisticKnife, Stance::Crouch) == "pb_b_knife_crouch_idle");

    const auto has = [](std::string_view name) { return Animations()(std::string(name)).has_value(); };
    CHECK(ActorLogic::FindStanceIdle(Gun::Pistol, Stance::Prone, has) == "pb_prone_aim_pistol");
    CHECK(ActorLogic::FindStanceIdle(Gun::DualWield, Stance::Crouch, has) == "pb_crouch_alert");
    CHECK(ActorLogic::FindStanceIdle(Gun::Rifle, Stance::Stand, [](std::string_view) { return false; }).empty());

    // under a clip, as the actor's gun has it
    const auto actor = ActorWith({Clip("pt_stand_shoot", 1000, 2000)});
    CHECK_NEAR(LegsOf(Layers(actor, 1500, Gun::RocketLauncher), "pb_stand_alert_rpg"), 1.0f, 1e-6f);
    CHECK_NEAR(LegsOf(Layers(actor, 1500, Gun::Pistol), "pb_stand_alert_pistol"), 1.0f, 1e-6f);
    CHECK_NEAR(LegsOf(Layers(actor, 1500, Gun::DualWield), "pb_stand_alert"), 1.0f, 1e-6f);
}

TEST("legs: an upper body clip alone stands on the idle, looping from the clip's start, a hundredth as heavy")
{
    const auto actor = ActorWith({Clip("pt_stand_shoot", 1000, 2000)});
    const auto layers = Layers(actor, 2500);
    CHECK(layers.clipCount == 1);
    CHECK(layers.upperBody[0]);
    CHECK(layers.legs.size() == 1);
    if (layers.legs.size() == 1)
    {
        CHECK(layers.legs[0].clip == nullptr);
        CHECK(layers.legs[0].animation == "pb_stand_alert");
        CHECK_NEAR(layers.legs[0].time, 1.5f, 1e-4f);
        CHECK_NEAR(layers.legs[0].weight, 1.0f, 1e-6f);
    }
    // before the clip starts the idle holds its first frame, and it goes on after the clip ends
    CHECK_NEAR(Layers(actor, 500).legs.at(0).time, 0.0f, 1e-6f);
    CHECK_NEAR(Layers(actor, 4000).legs.at(0).time, 3.0f, 1e-4f);

    const auto weights = Weigh(actor, 2500);
    CHECK(weights.animations.size() == 2);
    for (const auto& entry : weights.animations)
        CHECK_NEAR(entry.weight, entry.clip ? 1.0f : ActorLogic::LEGS_WEIGHT, 1e-6f);

    // the stances by name and the actor's own
    CHECK_NEAR(LegsOf(Layers(ActorWith({Clip("pt_reload_crouch", 0, 1000)}), 500), "pb_crouch_alert"), 1.0f, 1e-6f);
    CHECK_NEAR(LegsOf(Layers(ActorWith({Clip("pt_reload_prone", 0, 1000)}), 500), "pb_prone_aim"), 1.0f, 1e-6f);
    CHECK_NEAR(LegsOf(Layers(ActorWith({Clip("pt_reload_prone", 0, 1000)}, Types::ActorStance::Crouch), 500),
                      "pb_crouch_alert"),
               1.0f, 1e-6f);
    CHECK_NEAR(
        LegsOf(Layers(ActorWith({Clip("pt_stand_shoot", 0, 1000)}, Types::ActorStance::Prone), 500), "pb_prone_aim"),
        1.0f, 1e-6f);
}

TEST("legs: per bone, the legs are the idle's alone and the upper body 99% the clip's")
{
    const auto actor = ActorWith({Clip("pt_stand_shoot", 0, 2000)});
    const auto weights = Weigh(actor, 1000);
    CHECK_NEAR(ShareOf(weights, Bone::Legs, "pb_stand_alert"), 1.0f, 1e-6f);
    CHECK_NEAR(ShareOf(weights, Bone::UpperBody, "pt_stand_shoot"), 1.0f / 1.01f, 1e-5f);
    // the idle moves the stabilizer, as the game's legs do under players' upper body animations
    CHECK_NEAR(ShareOf(weights, Bone::Stabilizer, "pb_stand_alert"), 1.0f, 1e-6f);
    CHECK_NEAR(weights.resting, 0.0f, 1e-6f);
    CHECK_NEAR(weights.lined, 0.0f, 1e-6f);
}

TEST("legs: blending in over a full body clip, the legs stay on it until the blend ends, then hand over to the idle")
{
    // the RPG of B3: its idle, then a shot blending in over 250 ms
    const auto actor = ActorWith({Clip("pb_stand_alert_rpg", 0, 3000), Clip("pt_stand_shoot", 3000, 2000, 250)});
    const auto gun = Types::GunClass::RocketLauncher;
    for (const std::uint32_t tick : {3000u, 3100u, 3249u})
    {
        const auto layers = Layers(actor, tick, gun);
        CHECK(layers.legs.size() == 1);
        CHECK(!layers.legs.empty() && layers.legs[0].clip == &actor.clips[0]);
        CHECK_NEAR(layers.legs.at(0).time, tick / 1000.0f, 1e-4f);
        const auto weights = ActorLogic::WeighLayers(layers, Spine);
        CHECK_NEAR(ShareOf(weights, Bone::Legs, "pb_stand_alert_rpg"), 1.0f, 1e-6f);
        // one animation each, the full body one as much as its sample and the legs' hundredth
        CHECK(weights.animations.size() == 2);
        const auto w = (tick - 3000) / 250.0f;
        for (const auto& entry : weights.animations)
            CHECK_NEAR(entry.weight, entry.clip == &actor.clips[0] ? 1.0f - w + 0.01f * w : w, 1e-5f);
    }
    // halfway through the blend the upper body is half the shot's
    CHECK_NEAR(ShareOf(Weigh(actor, 3125, gun), Bone::UpperBody, "pt_stand_shoot"), 0.5f / (0.5f + 0.505f), 1e-5f);

    // the hand over: in 170 ms (the idle doesn't walk), the full body clip going on as it played, the idle (the same
    // animation) from the shot's start
    const auto at = [&](std::uint32_t tick) { return Layers(actor, tick, gun); };
    CHECK_NEAR(LegsOfClip(at(3250)), 1.0f, 1e-6f);
    CHECK_NEAR(LegsOfClip(at(3335)), 0.5f, 1e-5f);
    CHECK_NEAR(LegsOf(at(3335), ActorLogic::GetStanceIdle(gun, Types::ActorStance::Stand)), 1.0f, 1e-5f);
    CHECK_NEAR(LegsOfClip(at(3420)), 0.0f, 1e-6f);
    CHECK(at(3420).legs.size() == 1);
    for (const auto& legs : at(3335).legs)
        CHECK_NEAR(legs.time, legs.clip ? 3.335f : 0.335f, 1e-4f);
    // the hand over is a hundredth of the shot, both of the legs: the legs blend evenly
    CHECK_NEAR(ClipsShareOf(Weigh(actor, 3335, gun), Bone::Legs), 0.5f, 1e-5f);

    // no tick to the next moves the legs' shares by much (the B3 pop was all of them at once)
    float largest = 0.0f;
    for (std::uint32_t tick = 2990; tick < 3500; ++tick)
        largest = std::max(largest, std::abs(ClipsShareOf(Weigh(actor, tick + 1, gun), Bone::Legs) -
                                             ClipsShareOf(Weigh(actor, tick, gun), Bone::Legs)));
    CHECK(largest <= 1.0f / 170.0f + 1e-4f);

    // from a run, the game's legs take 250 ms
    const auto running = ActorWith({Clip("pb_combatrun", 0, 3000), Clip("pt_stand_shoot", 3000, 2000, 100)});
    CHECK_NEAR(LegsOf(Layers(running, 3100 + 125), "pb_combatrun"), 0.5f, 1e-5f);
    // and with no blend at all, the legs still hand over
    const auto cut = ActorWith({Clip("pb_combatrun", 0, 3000), Clip("pt_stand_shoot", 3000, 2000, 0)});
    CHECK_NEAR(LegsOf(Layers(cut, 3000), "pb_combatrun"), 1.0f, 1e-6f);
    CHECK_NEAR(LegsOf(Layers(cut, 3250), "pb_stand_alert"), 1.0f, 1e-6f);
}

TEST("legs: a full body clip blending in over an upper body one takes the legs over evenly")
{
    const auto actor = ActorWith({Clip("pt_stand_shoot", 0, 2000), Clip("pb_combatrun", 2000, 3000, 200)});
    for (const std::uint32_t tick : {2000u, 2050u, 2100u, 2150u})
    {
        const auto w = (tick - 2000) / 200.0f;
        const auto layers = Layers(actor, tick);
        CHECK_NEAR(LegsOf(layers, "pb_stand_alert"), 1.0f, 1e-6f);
        const auto weights = ActorLogic::WeighLayers(layers, Spine);
        CHECK_NEAR(ShareOf(weights, Bone::Legs, "pb_combatrun"), w, 1e-5f);
        CHECK_NEAR(ShareOf(weights, Bone::Legs, "pb_stand_alert"), 1.0f - w, 1e-5f);
        CHECK_NEAR(ShareOf(weights, Bone::Stabilizer, "pb_combatrun"), w, 1e-5f);
    }
    // then it alone, as it was before there were legs
    const auto layers = Layers(actor, 2300);
    CHECK(layers.legs.empty());
    const auto weights = ActorLogic::WeighLayers(layers, Spine);
    CHECK(weights.animations.size() == 1);
    CHECK_NEAR(weights.animations.at(0).weight, 1.0f, 1e-6f);
}

TEST("legs: between upper body clips the idle goes on, blending to another stance's in 400 ms")
{
    const auto actor = ActorWith({Clip("pt_stand_shoot", 0, 2000), Clip("pt_reload_crouch", 2000, 2000, 250)});
    CHECK_NEAR(LegsOf(Layers(actor, 1999), "pb_stand_alert"), 1.0f, 1e-6f);
    CHECK_NEAR(LegsOf(Layers(actor, 2000), "pb_stand_alert"), 1.0f, 1e-6f);
    const auto half = Layers(actor, 2200);
    CHECK_NEAR(LegsOf(half, "pb_stand_alert"), 0.5f, 1e-5f);
    CHECK_NEAR(LegsOf(half, "pb_crouch_alert"), 0.5f, 1e-5f);
    // both from the start of the run of upper body clips
    for (const auto& legs : half.legs)
        CHECK_NEAR(legs.time, 2.2f, 1e-4f);
    CHECK_NEAR(LegsOf(Layers(actor, 2400), "pb_crouch_alert"), 1.0f, 1e-6f);
    // both clips are upper body ones: the idle under them is a hundredth of them together
    const auto weights = Weigh(actor, 2100);
    float legs = 0.0f;
    for (const auto& entry : weights.animations)
        if (!entry.clip)
            legs += entry.weight;
    CHECK_NEAR(legs, ActorLogic::LEGS_WEIGHT, 1e-6f);

    // the actor's stance keeps them as it says
    const auto standing = ActorWith(actor.clips, Types::ActorStance::Stand);
    CHECK_NEAR(LegsOf(Layers(standing, 2200), "pb_stand_alert"), 1.0f, 1e-6f);
}

TEST("legs: anim_pose notetracks change the stance where the animation passes them")
{
    // kneeling from 200 ms to 1600 ms of the two second reload
    const auto actor = ActorWith({Clip("pt_reload_stand_rpg", 1000, 2000, 0, false)});
    const auto gun = Types::GunClass::RocketLauncher;
    const auto stand = ActorLogic::GetStanceIdle(gun, Types::ActorStance::Stand);
    const auto crouch = ActorLogic::GetStanceIdle(gun, Types::ActorStance::Crouch);
    CHECK_NEAR(LegsOf(Layers(actor, 1199, gun), stand), 1.0f, 1e-6f);
    CHECK_NEAR(LegsOf(Layers(actor, 1400, gun), crouch), 0.5f, 1e-5f);
    CHECK_NEAR(LegsOf(Layers(actor, 1600, gun), crouch), 1.0f, 1e-6f);
    CHECK_NEAR(LegsOf(Layers(actor, 2599, gun), crouch), 1.0f, 1e-6f);
    CHECK_NEAR(LegsOf(Layers(actor, 2800, gun), stand), 0.5f, 1e-5f);
    CHECK_NEAR(LegsOf(Layers(actor, 3500, gun), stand), 1.0f, 1e-6f);
    // the actor's stance overrides them
    CHECK_NEAR(LegsOf(Layers(ActorWith(actor.clips, Types::ActorStance::Stand), 1600, gun), stand), 1.0f, 1e-6f);
    // played faster, it passes them sooner
    auto fast = actor;
    fast.clips[0].speed = 2.0f;
    CHECK_NEAR(LegsOf(Layers(fast, 1300, gun), crouch), 0.5f, 1e-5f);

    // a change while the last one blends blends on from where that one was: crouching from 200 ms, standing again
    // from 400 ms (each 400 ms long)
    const auto bobbing = ActorWith({Clip("pt_bob", 0, 2000, 0, false)});
    CHECK_NEAR(LegsOf(Layers(bobbing, 400), "pb_crouch_alert"), 0.5f, 1e-5f);
    CHECK_NEAR(LegsOf(Layers(bobbing, 600), "pb_crouch_alert"), 0.25f, 1e-5f);
    CHECK_NEAR(LegsOf(Layers(bobbing, 600), "pb_stand_alert"), 0.75f, 1e-5f);
    CHECK_NEAR(LegsOf(Layers(bobbing, 800), "pb_stand_alert"), 1.0f, 1e-6f);

    // looping, the stance it ends a play in goes on into the next, and each play passes them again
    const auto looping = ActorWith({Clip("pt_reload_stand_rpg", 0, 10000)});
    CHECK_NEAR(LegsOf(Layers(looping, 2100, gun), stand), 1.0f, 1e-6f);
    CHECK_NEAR(LegsOf(Layers(looping, 2400, gun), crouch), 0.5f, 1e-5f);
    CHECK_NEAR(LegsOf(Layers(looping, 4100, gun), stand), 1.0f, 1e-6f);
}

TEST("legs: the stabilizer blends from at rest to the idle's after an SP clip")
{
    // an SP idle lines the stabilizer up; under the shot it is at rest for the shot's own spine, and as the SP legs
    // hand over to the idle, it blends evenly to the idle's
    const auto actor = ActorWith({Clip("casual_idle", 0, 3000), Clip("pt_stand_shoot", 3000, 2000, 200)});
    const auto at = [&](std::uint32_t tick) { return Weigh(actor, tick); };
    CHECK_NEAR(ShareOf(at(2999), Bone::Stabilizer, "lined"), 1.0f, 1e-6f);
    CHECK_NEAR(ShareOf(at(3100), Bone::Stabilizer, "resting"), 0.5f / (0.5f + 0.505f * 0.5f), 1e-5f);
    CHECK_NEAR(ShareOf(at(3199), Bone::Stabilizer, "resting"), 1.0f, 0.01f);
    CHECK_NEAR(ShareOf(at(3200), Bone::Stabilizer, "resting"), 1.0f, 1e-6f);
    CHECK_NEAR(ShareOf(at(3285), Bone::Stabilizer, "resting"), 0.5f, 1e-5f);
    CHECK_NEAR(ShareOf(at(3285), Bone::Stabilizer, "pb_stand_alert"), 0.5f, 1e-5f);
    CHECK_NEAR(ShareOf(at(3370), Bone::Stabilizer, "pb_stand_alert"), 1.0f, 1e-6f);
    // and the SP legs are the SP clip's alone until then
    CHECK_NEAR(ShareOf(at(3150), Bone::Legs, "casual_idle"), 1.0f, 1e-6f);

    // the other way round, the SP clip lines it up as it takes the legs over
    const auto back = ActorWith({Clip("pt_stand_shoot", 0, 3000), Clip("casual_idle", 3000, 2000, 200)});
    CHECK_NEAR(ShareOf(Weigh(back, 3100), Bone::Stabilizer, "lined"), 0.5f, 1e-5f);
    CHECK_NEAR(ShareOf(Weigh(back, 3100), Bone::Legs, "casual_idle"), 0.5f, 1e-5f);
}

TEST("legs: without upper body clips (or idles in the game) the clips weigh as they did")
{
    // a full body blend, an SP one on an MP body, a face alone: no legs
    const auto actor = ActorWith(
        {Clip("pb_combatrun", 0, 2000), Clip("casual_idle", 2000, 2000, 400), Clip("pf_firing", 4000, 2000, 0)});
    auto weights = Weigh(actor, 2100);
    CHECK(Layers(actor, 2100).legs.empty());
    CHECK(weights.animations.size() == 2);
    for (const auto& entry : weights.animations)
        CHECK_NEAR(entry.weight, entry.clip == &actor.clips[0] ? 0.75f : 0.25f, 1e-6f);
    CHECK_NEAR(weights.lined, 0.25f, 1e-6f);
    CHECK_NEAR(weights.resting, 0.0f, 1e-6f);
    weights = Weigh(actor, 5000);
    CHECK_NEAR(weights.resting, 1.0f, 1e-6f);

    // an upper body clip with no idle in the game, after an SP clip: the stabilizer at rest under it, as before
    const auto bare = ActorWith({Clip("casual_idle", 0, 1000), Clip("pt_stand_shoot", 1000, 1000, 100)});
    const auto layers = ActorLogic::GetLayers(
        bare, 1050,
        [](const std::string& name) -> std::optional<ActorLogic::AnimationInfo> {
            return name.starts_with("pb_") ? std::nullopt : Animations()(name);
        },
        Types::GunClass::Rifle);
    CHECK(layers.legs.empty());
    weights = ActorLogic::WeighLayers(layers, Spine);
    CHECK_NEAR(weights.lined, 0.5f, 1e-6f);
    CHECK_NEAR(weights.resting, 0.5f, 1e-6f);
}

TEST("legs: the actor's stance is saved with the scene")
{
    ActorLogic::Scene scene;
    scene.actors.push_back(ActorWith({Clip("pt_stand_shoot", 0, 1000)}, Types::ActorStance::Crouch));
    scene.actors.push_back(ActorWith({Clip("pt_stand_shoot", 0, 1000)}));
    const auto read = ActorLogic::SceneFromJson(ActorLogic::SceneToJson(scene, "demo"), 10000);
    CHECK(read.actors.size() == 2);
    CHECK(read.actors.at(0).stance == Types::ActorStance::Crouch);
    CHECK(read.actors.at(1).stance == Types::ActorStance::Auto);
    // scenes from before have the stance as the clips say
    auto old = ActorLogic::SceneToJson(scene, "demo");
    old["actors"][0].erase("stance");
    CHECK(ActorLogic::SceneFromJson(old, 10000).actors.at(0).stance == Types::ActorStance::Auto);
}
