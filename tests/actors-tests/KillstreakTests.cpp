#include "StdInclude.hpp"
#include "Test.hpp"

#include "Components/ActorLogic.hpp"
#include "Components/AnimationCatalogLogic.hpp"
#include "Components/CharacterLogic.hpp"
#include "Components/KillstreakLogic.hpp"
#include "Components/TransitionLogic.hpp"

using namespace IWXMVM;

// Killstreaks as actors: the table of what each is made of, their motions as
// clips, their bodies for locomotion, their starting points, characters and looks, changing a person into one, and
// the fit filter's list for one

namespace
{
    namespace Killstreaks = Components::KillstreakLogic;
    using Types::Killstreak, Types::VehicleClass;
    using Gait = ActorLogic::Gait;

    const auto COBRA = ActorLogic::Body::Vehicle(Killstreak::AttackHelicopter);
    const auto RCXD = ActorLogic::Body::Vehicle(Killstreak::RcXd);

    // the motions, as Components::Actors looks them up, and a person's walk, run and idle
    ActorLogic::AnimationLookup Animations()
    {
        return [](const std::string& name) -> std::optional<ActorLogic::AnimationInfo> {
            if (Killstreaks::IsMotion(name))
                return Killstreaks::GetMotionInfo(name);
            if (name == "pb_stand_alert")
                return ActorLogic::AnimationInfo{10.0f, glm::vec2(0.0f), {}, Types::AnimationKind::FullBody,
                                                 Types::AnimationFamily::MpPlayer};
            if (name == "pb_combatrun_forward_loop")
                return ActorLogic::AnimationInfo{1.0f, glm::vec2(211.0f, 0.0f), {}, Types::AnimationKind::FullBody,
                                                 Types::AnimationFamily::MpPlayer};
            if (name == "pb_stand_shoot_walk_forward")
                return ActorLogic::AnimationInfo{1.0f, glm::vec2(94.0f, 0.0f), {}, Types::AnimationKind::FullBody,
                                                 Types::AnimationFamily::MpPlayer};
            return std::nullopt;
        };
    }

    // the Huey's bones (t5_veh_helo_huey_mp, common_mp)
    const std::vector<std::string> HUEY_BONES{
        "tag_origin", "origin_animate_jnt", "tag_body", "body_animate_jnt", "elevator_jnt", "front_door_l_jnt",
        "front_door_r_jnt", "main_rotor_jnt", "side_door_l_jnt", "side_door_r_jnt", "snd_cockpit", "snd_int_f_l",
        "snd_int_f_r", "snd_int_r_l", "snd_int_r_r", "snd_int_rotor", "snd_rotor", "snd_tail_rotor", "snd_under_body",
        "snd_wind_left", "snd_wind_right", "tag_deathfx", "tag_detach", "tag_driver", "tag_engine_left",
        "tag_engine_right", "tag_enter_driver", "tag_enter_gunner1", "tag_enter_gunner2", "tag_golight_green",
        "tag_golight_red", "tag_ground", "tag_gunmount_r", "tag_gunner_mount1", "tag_gunner_mount2", "tag_passenger",
        "tag_passenger2", "tag_passenger3", "tag_passenger4", "tag_passenger5", "tag_passenger6", "tag_player",
        "tag_playerride", "tag_turret", "tail_rotor_jnt", "tag_barrel", "tag_gunner1", "tag_gunner2",
        "tag_gunner_turret_anim", "tag_gunner_turret_anim2", "tag_flash", "tag_gunner_turret1", "tag_gunner_turret2",
        "tag_gunner_barrel1_anim", "tag_gunner_barrel2_anim", "tag_gunner_barrel1", "tag_gunner_barrel2",
        "tag_minigun_left", "tag_minigun_right", "tag_flash_gunner1", "tag_flash_gunner2", "tag_gunner_brass1",
        "tag_gunner_brass2", "tag_minigun_left_barrel", "tag_minigun_right_barrel"};

    const Types::AnimationCatalog& Shipped()
    {
        static const auto catalog = [] {
            const auto path = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / "core" /
                              "resources" / "animations.json";
            std::ifstream file(path);
            return AnimationCatalogLogic::Parse(nlohmann::json::parse(file));
        }();
        return catalog;
    }

    const Types::ActorClip* FindClip(const Types::Actor& actor, std::string_view animation)
    {
        const auto found = std::ranges::find(actor.clips, animation, &Types::ActorClip::animation);
        return found != actor.clips.end() ? &*found : nullptr;
    }
}  // namespace

TEST("killstreaks: each has its models, looks, presets and effects, and its models tell it")
{
    CHECK(Killstreaks::GetAll().size() == 11);
    for (const auto& info : Killstreaks::GetAll())
    {
        CHECK(&Killstreaks::Get(info.killstreak) == &info);
        CHECK(!info.models.empty() && !info.name.empty());
        for (std::size_t look = 0; look < info.models.size(); ++look)
        {
            CHECK(Killstreaks::FindByModel(info.models[look]) == info.killstreak);
            CHECK(Killstreaks::GetLook(info, info.models[look]) == look);
        }
        // the presets slowest first, the moving ones' with a speed; the flying ones with a height
        for (std::size_t i = 1; i < info.presets.size(); ++i)
            CHECK(info.presets[i - 1].speed < info.presets[i].speed && info.presets[i - 1].gait < info.presets[i].gait);
        const bool moves = info.vehicleClass == VehicleClass::Car || Killstreaks::Flies(info.vehicleClass);
        CHECK(moves == !info.presets.empty());
        CHECK(Killstreaks::Flies(info.vehicleClass) == (info.flyHeight > 0.0f));
        CHECK((info.vehicleClass == VehicleClass::Plane) == (info.passLength > 0.0f));
    }
    CHECK(!Killstreaks::FindByModel("german_shepherd") && !Killstreaks::FindByModel("c_usa_cia_mp_body_armor"));
    CHECK(Killstreaks::FindByModels({"t5_veh_helo_huey_mp", "something_else"}) == Killstreak::ChopperGunner);
    CHECK(!Killstreaks::FindByModels({}));
    // mph as the game converts them
    CHECK_NEAR(Killstreaks::GetCruiseSpeed(Killstreaks::Get(Killstreak::AttackHelicopter)), 704.0f, 0.01f);
    CHECK_NEAR(Killstreaks::GetCruiseSpeed(Killstreaks::Get(Killstreak::RcXd)), 334.4f, 0.01f);
    CHECK(Killstreaks::GetCruiseSpeed(Killstreaks::Get(Killstreak::Napalm)) == 7000.0f);

    // the rotors on every helicopter; its lights by its look
    const auto& huey = Killstreaks::Get(Killstreak::ChopperGunner);
    const auto friendly = Killstreaks::GetEffects(huey, 0);
    const auto enemy = Killstreaks::GetEffects(huey, 1);
    const auto has = [](const std::vector<Killstreaks::Effect>& effects, std::string_view effect, std::string_view tag) {
        return std::ranges::any_of(effects, [&](const Killstreaks::Effect& each) {
            return each.effect == effect && each.tag == tag;
        });
    };
    CHECK(has(friendly, "vehicle/props/fx_huey_main_blade_full", "main_rotor_jnt"));
    CHECK(has(enemy, "vehicle/props/fx_huey_small_blade_full", "tail_rotor_jnt"));
    CHECK(has(friendly, "vehicle/light/fx_huey_exterior_lights_grn_mp", "tag_origin"));
    CHECK(!has(friendly, "vehicle/light/fx_huey_exterior_lights_red_mp", "tag_origin"));
    CHECK(has(enemy, "vehicle/light/fx_huey_exterior_lights_red_mp", "tag_origin"));
    for (const auto killstreak : {Killstreak::AttackHelicopter, Killstreak::Gunship, Killstreak::CarePackageChopper})
    {
        const auto effects = Killstreaks::GetEffects(Killstreaks::Get(killstreak), 1);
        CHECK(std::ranges::count(effects, "main_rotor_jnt", &Killstreaks::Effect::tag) == 1);
        CHECK(std::ranges::count(effects, "tail_rotor_jnt", &Killstreaks::Effect::tag) == 1);
    }
    CHECK(Killstreaks::GetLookName(0) == "Friendly" && Killstreaks::GetLookName(1) == "Enemy" &&
          Killstreaks::GetLookName(2) == "Booby-trapped");
}

TEST("killstreaks: motions are clips of each class's, the moving ones walking the path")
{
    for (const auto motion : Killstreaks::MOTIONS)
    {
        CHECK(Killstreaks::IsMotion(motion));
        const auto locomotion = ActorLogic::FindLocomotion(motion);
        CHECK(locomotion && locomotion->vehicle && !locomotion->dog);
        CHECK(locomotion && locomotion->gait == Killstreaks::GetMotionGait(motion));
        CHECK(!Killstreaks::GetMotionLabel(motion).empty() && !Killstreaks::DescribeMotion(motion).empty());
        const auto info = Killstreaks::GetMotionInfo(motion);
        CHECK(info.loop && info.duration == Killstreaks::MOTION_DURATION);
        CHECK((glm::length(*info.motionPerPlay) > 0.0f) == Killstreaks::IsMoving(motion));
    }
    CHECK(!Killstreaks::IsMotion("pb_stand_alert") && !ActorLogic::FindLocomotion("pb_stand_alert")->vehicle);

    CHECK(Killstreaks::GetMotion(VehicleClass::Helicopter, Gait::Idle) == Killstreaks::MOTION_HOVER);
    CHECK(Killstreaks::GetMotion(VehicleClass::Helicopter, Gait::Sprint) == Killstreaks::MOTION_FLY);
    CHECK(Killstreaks::GetMotion(VehicleClass::Car, Gait::Walk) == Killstreaks::MOTION_DRIVE);
    CHECK(Killstreaks::GetMotion(VehicleClass::Car, Gait::Idle) == Killstreaks::MOTION_IDLE);
    // a plane never stops; a turret and a crate stay where they are
    CHECK(Killstreaks::GetMotion(VehicleClass::Plane, Gait::Idle) == Killstreaks::MOTION_FLY);
    CHECK(Killstreaks::GetMotion(VehicleClass::Turret, Gait::Run) == Killstreaks::MOTION_IDLE);
    CHECK(Killstreaks::GetMotion(VehicleClass::Crate, Gait::Run) == Killstreaks::MOTION_IDLE);
    CHECK(Killstreaks::FitsMotion(VehicleClass::Turret, Killstreaks::MOTION_SCAN));
    CHECK(!Killstreaks::FitsMotion(VehicleClass::Car, Killstreaks::MOTION_FLY));
    CHECK(!Killstreaks::FitsMotion(VehicleClass::Plane, Killstreaks::MOTION_HOVER));

    // a killstreak's body: its motions as its locomotion, its presets, no legs
    CHECK(ActorLogic::GetLocomotionAnimation(COBRA, Types::ActorStance::Crouch, Gait::Run,
                                             ActorLogic::Heading::Back) == Killstreaks::MOTION_FLY);
    CHECK(ActorLogic::GetSpeedPresets(COBRA).size() == Killstreaks::Get(Killstreak::AttackHelicopter).presets.size());
    CHECK(ActorLogic::GetSpeedPresets(RCXD)[1].speed == Killstreaks::Get(Killstreak::RcXd).presets[1].speed);
    const auto animations = Animations();
    CHECK(ActorLogic::ChooseLocomotion(COBRA, Types::ActorStance::Auto, 700.0f, 90.0f, animations).animation ==
          Killstreaks::MOTION_FLY);
    CHECK(ActorLogic::ChooseLocomotion(COBRA, Types::ActorStance::Auto, 0.0f, 0.0f, animations).animation ==
          Killstreaks::MOTION_HOVER);
    CHECK(ActorLogic::GetAnimationSpeed(Killstreaks::MOTION_HOVER, animations) == 0.0f);

    // flying a path at the actor's speed (Locomotion Auto), setting off and stopping at the game's rate: its speed-up
    // the Fly's blend in, its stop the Hover's (704 at 12.5 mph a second: 3.2 s, and 0.3 s for its soft corners; see
    // VehicleTests)
    auto heli = Killstreaks::MakeStart(Killstreak::AttackHelicopter, 1000, 40000, 0.0f);
    heli.path = {{0, 0, 1100}, {20000, 0, 1100}};
    const auto& fly = heli.clips.front();
    CHECK(fly.animation == Killstreaks::MOTION_FLY);
    const auto at = [&](std::uint32_t tick) { return ActorLogic::EvaluatePlacement(heli, tick, animations).position; };
    const auto& hover = heli.clips[1];
    CHECK(hover.animation == Killstreaks::MOTION_HOVER && hover.start == fly.End());
    CHECK(hover.blendIn == Killstreaks::GetSpeedChangeTicks(Killstreaks::Get(Killstreak::AttackHelicopter), 704.0f));
    CHECK(hover.blendIn == 3500 && fly.blendIn == 3500);
    // no legs, no transitions
    CHECK(ActorLogic::GetLayers(heli, 1500, animations, COBRA).legs.empty());
    CHECK(ActorLogic::GetLegsLibrary(heli, COBRA, animations).empty());
    CHECK(TransitionLogic::GetTransitions(COBRA).empty() && TransitionLogic::GetTransitions(RCXD).empty());

    // fitted to its path, it cruises at its speed and gets to the end as the hover has taken over
    ActorLogic::FitWalk(heli, fly.id, animations, 704.0f);
    const auto speed = [&](std::uint32_t tick) { return (at(tick + 50).x - at(tick - 50).x) * 10.0f; };
    CHECK_NEAR(speed(heli.clips.front().End() - 500), 704.0f, 2.0f);
    CHECK_NEAR(at(1500).z, 1100.0f, 0.01f);
    const auto fitted = heli.clips.front().End() + heli.clips[1].blendIn;
    CHECK_NEAR(at(fitted).x, 20000.0f, 0.5f);
    CHECK(at(fitted - 1000).x < 20000.0f - 10.0f);
    CHECK_NEAR(speed(fitted + 100), 0.0f, 0.5f);
}

TEST("killstreaks: their starting points, as the game shows them")
{
    // the RC-XD drives on the ground, its path to be placed, and stops in 0.6 s
    const auto car = Killstreaks::MakeStart(Killstreak::RcXd, 2000, 30000, 45.0f);
    CHECK(car.models == std::vector<std::string>{"t5_veh_rcbomb_allies"} && car.character == "map/t5_veh_rcbomb_allies");
    CHECK(car.movement == Types::ActorMovement::Path && car.path.size() == 1 && car.followGround);
    CHECK(car.locomotion == Types::ActorLocomotion::Auto && std::abs(car.pathSpeed - 334.4f) < 0.01f);
    CHECK(car.flyHeight == 0.0f && !car.autoTransitions && car.face == Types::ActorFace::Off);
    CHECK(car.clips.size() == 2 && car.clips[0].animation == Killstreaks::MOTION_DRIVE &&
          car.clips[1].animation == Killstreaks::MOTION_IDLE && car.clips[1].End() == 30000);
    CHECK(car.clips[0].blendIn == 933 && car.clips[1].blendIn == 933);

    // a helicopter flies, in the air
    const auto heli = Killstreaks::MakeStart(Killstreak::ChopperGunner, 2000, 30000, 0.0f);
    CHECK(!heli.followGround && heli.flyHeight == 850.0f && heli.models.front() == "t5_veh_helo_huey_mp_light");
    CHECK(FindClip(heli, Killstreaks::MOTION_FLY) && FindClip(heli, Killstreaks::MOTION_HOVER));

    // a plane's pass through the origin, the way it faces, at its speed, gone at its end
    const auto plane = Killstreaks::MakeStart(Killstreak::Napalm, 2000, 30000, 90.0f);
    CHECK(plane.path.size() == 2);
    CHECK_NEAR(plane.path[0].y, -24000.0f, 0.5f);
    CHECK_NEAR(plane.path[1].y, 24000.0f, 0.5f);
    CHECK(plane.clips.size() == 1 && plane.clips[0].length == 6857);
    CHECK(plane.events.size() == 1 && plane.events[0].type == Types::ActorEventType::Hide &&
          plane.events[0].tick == 2000 + 6857);
    CHECK(ActorLogic::IsVisible(plane, 5000) && !ActorLogic::IsVisible(plane, 9000));
    CHECK(Killstreaks::GetStartPlaceTick(plane, 2000) == 2000 + 6857 / 2);
    const auto mid = ActorLogic::EvaluatePlacement(plane, Killstreaks::GetStartPlaceTick(plane, 2000), Animations());
    CHECK(glm::length(glm::vec2(mid.position)) < 5.0f && std::abs(std::remainder(mid.yaw - 90.0f, 360.0f)) < 0.1f);

    // the turrets stand, the Sentry Gun scanning; the crate drops from the chopper and stands
    const auto sentry = Killstreaks::MakeStart(Killstreak::SentryGun, 2000, 30000, 0.0f);
    CHECK(sentry.movement == Types::ActorMovement::Still && sentry.clips.size() == 1 &&
          sentry.clips[0].animation == Killstreaks::MOTION_SCAN);
    CHECK(Killstreaks::MakeStart(Killstreak::SamTurret, 2000, 30000, 0.0f).clips[0].animation ==
          Killstreaks::MOTION_IDLE);
    const auto crate = Killstreaks::MakeStart(Killstreak::CarePackage, 2000, 30000, 0.0f);
    CHECK(crate.clips.size() == 2 && crate.clips[0].animation == Killstreaks::MOTION_DROP &&
          crate.clips[1].animation == Killstreaks::MOTION_IDLE && crate.clips[1].End() == 30000);
    CHECK(Killstreaks::GetStartPlaceTick(sentry, 2000) == 2000);
}

TEST("killstreaks: a person made a helicopter flies where it ran, and back")
{
    const auto animations = Animations();
    Types::Actor actor;
    actor.locomotion = Types::ActorLocomotion::Auto;
    actor.pathSpeed = 211.0f;
    actor.movement = Types::ActorMovement::Path;
    actor.path = {{0, 0, 0}, {5000, 0, 0}};
    actor.face = Types::ActorFace::Auto;
    actor.clips = {{.animation = "pb_combatrun_forward_loop", .start = 1000, .length = 2000},
                   {.animation = "pb_stand_alert", .start = 3000, .length = 3000}};
    ActorLogic::Normalize(actor);
    ActorLogic::StartAnimations heliDefaults{.idle = std::string(Killstreaks::MOTION_HOVER),
                                             .walk = std::string(Killstreaks::MOTION_FLY),
                                             .run = std::string(Killstreaks::MOTION_FLY),
                                             .locomotion = true};
    ActorLogic::ChangeBody(actor, Types::GunClass::Rifle, COBRA, heliDefaults, animations);
    CHECK(actor.clips[0].animation == Killstreaks::MOTION_FLY && actor.clips[1].animation == Killstreaks::MOTION_HOVER);
    CHECK(actor.face == Types::ActorFace::Off && actor.stance == Types::ActorStance::Auto);
    // the run preset is the helicopter's cruise
    CHECK_NEAR(actor.pathSpeed, 704.0f, 0.01f);

    ActorLogic::StartAnimations person{.idle = "pb_stand_alert", .walk = "pb_stand_shoot_walk_forward",
                                       .run = "pb_combatrun_forward_loop", .locomotion = true};
    ActorLogic::ChangeBody(actor, COBRA, Types::GunClass::Rifle, person, animations);
    CHECK(actor.clips[0].animation == "pb_combatrun_forward_loop" && actor.clips[1].animation == "pb_stand_alert");
    CHECK_NEAR(actor.pathSpeed, 211.0f, 0.01f);
}

TEST("killstreaks: the map's killstreaks are characters, each look a body")
{
    using Kind = Types::Character::Kind;
    const std::vector<std::string> models{"c_usa_cia_mp_body_armor",   "c_usa_cia_mp_head_1",
                                          "german_shepherd",           "t5_veh_helo_huey_mp_light",
                                          "t5_veh_helo_huey_mp",       "t5_veh_rcbomb_allies",
                                          "t5_veh_rcbomb_axis",        "mp_supplydrop_ally",
                                          "mp_supplydrop_axis",        "mp_supplydrop_boobytrapped",
                                          "t5_weapon_minigun_turret"};
    const auto characters = Components::CharacterLogic::MakeMapCharacters(models);
    const auto huey = Components::CharacterLogic::Find(characters, "map/t5_veh_helo_huey_mp_light");
    CHECK(huey != nullptr);
    if (!huey)
        return;
    CHECK(huey->kind == Kind::Killstreak && huey->name == "Chopper Gunner" && huey->head.empty());
    CHECK((huey->bodies == std::vector<std::string>{"t5_veh_helo_huey_mp_light", "t5_veh_helo_huey_mp"}));
    CHECK(Components::CharacterLogic::GetActorKind(Kind::Killstreak) == Types::ActorKind::Vehicle);
    // the enemy's look, and back from the models
    Components::CharacterLogic::Look enemy;
    enemy.body = 1;
    CHECK(Components::CharacterLogic::GetModels(*huey, enemy, nullptr) ==
          std::vector<std::string>{"t5_veh_helo_huey_mp"});
    CHECK(Components::CharacterLogic::GetLook(*huey, {"t5_veh_helo_huey_mp"}, nullptr).body == 1);
    CHECK(Components::CharacterLogic::GetModels(*huey, {}, nullptr) ==
          std::vector<std::string>{"t5_veh_helo_huey_mp_light"});
    const auto crate = Components::CharacterLogic::Find(characters, "map/mp_supplydrop_ally");
    CHECK(crate && crate->bodies.size() == 3);
    CHECK(Components::CharacterLogic::Find(characters, "map/t5_weapon_minigun_turret") != nullptr);
    // only those the map has
    CHECK(!Components::CharacterLogic::Find(characters, "map/vehicle_cobra_helicopter_mp_light"));
    CHECK(Components::CharacterLogic::Find(characters, "map/german_shepherd") != nullptr);
    // (and they file no models as bodies of people's)
    CHECK(Components::CharacterLogic::FindByModels(characters, {"t5_veh_rcbomb_axis"})->id ==
          "map/t5_veh_rcbomb_allies");
}

TEST("killstreaks: only their own animations fit them, and why the others don't (F3)")
{
    auto rig = AnimationCatalogLogic::GetModelRig(HUEY_BONES);
    // by its bones alone, the props of the campaign that key its root bones would fit it
    const auto byBones = std::ranges::count(AnimationCatalogLogic::GetFits(Shipped(), rig), true);
    CHECK(byBones > 100);
    rig.vehicle = true;
    for (const auto animation : Killstreaks::Get(Killstreak::ChopperGunner).animations)
        rig.own.emplace_back(animation);
    const auto fits = AnimationCatalogLogic::GetFits(Shipped(), rig);
    std::vector<std::string> fitting;
    for (std::size_t i = 0; i < fits.size(); ++i)
        if (fits[i])
            fitting.push_back(Shipped().entries[i].name);
    CHECK((fitting == std::vector<std::string>{"v_huey_door_close_state", "v_huey_door_open", "v_huey_door_open_state"}));
    const auto why = [&](std::string_view name) {
        const auto entry = Shipped().Find(name);
        return entry ? AnimationCatalogLogic::DescribeMisfit(Shipped(), *entry, rig) : std::string("missing");
    };
    CHECK(why("v_huey_door_open").empty());
    CHECK(why("pb_stand_alert") == "Made for a person's skeleton: the actor is a vehicle");
    CHECK(why("german_shepherd_run") == "Made for a dog's skeleton: the actor is a vehicle");
    CHECK(why("pf_death") == "Made for a person's face: the actor is a vehicle");
    CHECK(why("p_cub_b01_bar_exit_door").starts_with("Made for a prop or vehicle of its own"));
    // the motions are the browser's first, under their own heading
    Types::ActorCatalog game;
    for (const auto motion : Killstreaks::MOTIONS)
        game.animations.push_back({.name = std::string(motion), .duration = 1.0f, .loop = true});
    std::ranges::sort(game.animations, {}, &Types::ActorCatalog::Animation::name);
    const auto list = AnimationCatalogLogic::List(Shipped(), game);
    CHECK(list.animations.size() == Shipped().entries.size() + Killstreaks::MOTIONS.size());
    CHECK(Killstreaks::IsMotion(list.animations.front().name));
    CHECK(list.headings[list.animations.front().heading] == "Vehicle motions");
    CHECK(AnimationCatalogLogic::GetCategories()[list.animations.front().category] == "Movement");
}

TEST("killstreaks: a flying one's height is saved with the scene, others' scenes stay as they were")
{
    ActorLogic::Scene scene;
    scene.actors.push_back(Killstreaks::MakeStart(Killstreak::Gunship, 1000, 9000, 0.0f));
    scene.actors.push_back(Killstreaks::MakeStart(Killstreak::RcXd, 1000, 9000, 0.0f));
    const auto json = ActorLogic::SceneToJson(scene, "demo");
    CHECK(json["actors"][0].contains("flyHeight") && !json["actors"][1].contains("flyHeight"));
    const auto read = ActorLogic::SceneFromJson(nlohmann::json::parse(json.dump()), 9000);
    CHECK(read.actors.size() == 2 && read.actors[0].flyHeight == 1100.0f && read.actors[1].flyHeight == 0.0f);
    CHECK(read.actors[0].clips[0].animation == Killstreaks::MOTION_FLY && !read.actors[0].followGround);
}

TEST("killstreaks: their effects are rows that each turn off, the dust and treads too, and Damaged smokes")
{
    using Row = Killstreaks::EffectRow;
    // the rows of each, in order: what it runs, its rotor wash and treads (S3: effects only)
    const auto rows = [](Killstreak killstreak) { return Killstreaks::GetEffectRows(Killstreaks::Get(killstreak)); };
    CHECK((rows(Killstreak::AttackHelicopter) == std::vector{Row::Rotors, Row::Lights, Row::Exhaust, Row::RotorWash}));
    CHECK(rows(Killstreak::CarePackageChopper) == rows(Killstreak::Gunship));
    CHECK((rows(Killstreak::RcXd) == std::vector{Row::Lights, Row::Treads}));
    CHECK((rows(Killstreak::Napalm) == std::vector{Row::Afterburners, Row::Contrails}));
    CHECK(rows(Killstreak::SentryGun).empty() && rows(Killstreak::CarePackage).empty());
    for (const auto& info : Killstreaks::GetAll())
    {
        const auto its = Killstreaks::GetEffectRows(info);
        for (const auto& effect : info.effects)
            CHECK(std::ranges::find(its, effect.row) != its.end());
    }
    CHECK(Killstreaks::GetEffectRowName(Row::RotorWash) == "Rotor wash");

    // on until turned off, saved by key with the scene, as is Damaged
    auto heli = Killstreaks::MakeStart(Killstreak::AttackHelicopter, 1000, 20000, 0.0f);
    CHECK(Killstreaks::IsEffectRowOn(heli, Row::Lights));
    Killstreaks::SetEffectRowOn(heli, Row::Lights, false);
    Killstreaks::SetEffectRowOn(heli, Row::RotorWash, false);
    Killstreaks::SetEffectRowOn(heli, Row::RotorWash, false);
    CHECK(!Killstreaks::IsEffectRowOn(heli, Row::Lights) && Killstreaks::IsEffectRowOn(heli, Row::Rotors));
    CHECK((heli.effectsOff == std::vector<std::string>{"lights", "rotorwash"}));
    heli.damage = Types::ActorDamage::Heavy;
    heli.damagedFrom = 4500;
    ActorLogic::Normalize(heli);
    const ActorLogic::Scene scene{{heli}, ""};
    const auto read =
        ActorLogic::SceneFromJson(nlohmann::json::parse(ActorLogic::SceneToJson(scene, "demo").dump()), 99999);
    CHECK(read.actors.size() == 1 && read.actors[0].effectsOff == heli.effectsOff &&
          read.actors[0].damage == Types::ActorDamage::Heavy && read.actors[0].damagedFrom == 4500);
    Killstreaks::SetEffectRowOn(heli, Row::Lights, true);
    CHECK(heli.effectsOff == std::vector<std::string>{"rotorwash"});
    // (scenes from before have every row on and no damage)
    auto old = ActorLogic::SceneToJson(ActorLogic::Scene{{Killstreaks::MakeStart(Killstreak::RcXd, 0, 9000, 0.0f)}, ""},
                                       "demo");
    CHECK(!old["actors"][0].contains("effectsOff") && !old["actors"][0].contains("damage"));

    // Damaged: the game's engine smoke of a helicopter's damage states
    CHECK(Killstreaks::CanBeDamaged(Killstreaks::Get(Killstreak::ChopperGunner)));
    CHECK(!Killstreaks::CanBeDamaged(Killstreaks::Get(Killstreak::RcXd)));
    CHECK(Killstreaks::GetDamageEffect(Types::ActorDamage::Light) == "trail/fx_trail_heli_killstreak_engine_smoke_33");
    CHECK(Killstreaks::GetDamageEffect(Types::ActorDamage::None).empty());

    // the dust by the surface under it, as each helicopter's vehicle definition has it
    const auto& cobra = Killstreaks::Get(Killstreak::AttackHelicopter);
    const auto& hind = Killstreaks::Get(Killstreak::Gunship);
    CHECK(Killstreaks::GetSurfaceName(10) == "grass" && Killstreaks::GetSurfaceName(22) == "asphalt");
    CHECK(Killstreaks::GetDustEffect(cobra, 10) == "vehicle/treadfx/fx_heli_dust_concrete");
    CHECK(Killstreaks::GetDustEffect(hind, 10) == "vehicle/treadfx/fx_heli_dust_default");
    CHECK(Killstreaks::GetDustEffect(hind, 22) == "vehicle/treadfx/fx_heli_dust_concrete");
    CHECK(Killstreaks::GetDustEffect(cobra, 19) == "vehicle/treadfx/fx_heli_snow_spray");
    CHECK(Killstreaks::GetDustEffect(cobra, 20) == "vehicle/treadfx/fx_heli_water_spray");
    CHECK(Killstreaks::GetDustEffect(Killstreaks::Get(Killstreak::RcXd), 10).empty());
    // the RC-XD's treads: earth's dust, hard ground's concrete, snow; none on wood
    CHECK(Killstreaks::GetTreadSurface(18) == "dust" && Killstreaks::GetTreadSurface(22) == "concrete" &&
          Killstreaks::GetTreadSurface(12) == "snow" && Killstreaks::GetTreadSurface(21).empty());
}

TEST("killstreaks: they die as the game's do, each death's effects and Hide its own linked events")
{
    const auto animations = Animations();
    const auto deaths = [](Killstreak killstreak) { return Killstreaks::GetDeaths(Killstreaks::Get(killstreak)); };
    // D3: a helicopter explodes or spins out (the Attack Helicopter explodes first), the RC-XD detonates, the turrets
    // are destroyed, the Spy Plane explodes, a crate goes off (booby-trapped); the passes never die
    CHECK(deaths(Killstreak::AttackHelicopter).front() == Killstreaks::MOTION_EXPLODE &&
          deaths(Killstreak::AttackHelicopter).size() == 2);
    CHECK(deaths(Killstreak::Gunship).front() == Killstreaks::MOTION_SPINOUT);
    CHECK(deaths(Killstreak::ChopperGunner).front() == Killstreaks::MOTION_SPINOUT);
    CHECK(deaths(Killstreak::RcXd).front() == Killstreaks::MOTION_DETONATE);
    CHECK(deaths(Killstreak::SentryGun).front() == Killstreaks::MOTION_DESTROYED);
    CHECK(deaths(Killstreak::SpyPlane).front() == Killstreaks::MOTION_EXPLODE);
    CHECK(deaths(Killstreak::Napalm).empty());
    CHECK(deaths(Killstreak::CarePackage).front() == Killstreaks::MOTION_DETONATE);
    // deaths to the editor: once, as long as they last, never walking
    const auto info = animations(std::string(Killstreaks::MOTION_SPINOUT));
    CHECK(info && !info->loop && info->death && *info->death && info->duration == 3.0f);
    CHECK(!ActorLogic::FindLocomotion(Killstreaks::MOTION_EXPLODE));
    CHECK(Killstreaks::FitsMotion(VehicleClass::Helicopter, Killstreaks::MOTION_SPINOUT) &&
          !Killstreaks::FitsMotion(VehicleClass::Car, Killstreaks::MOTION_SPINOUT));

    // the Cobra flying a path explodes where it is: the blast as its Hit, gone a moment after
    auto heli = Killstreaks::MakeStart(Killstreak::AttackHelicopter, 1000, 40000, 0.0f);
    heli.path = {{0, 0, 1100}, {8000, 0, 1100}};
    ActorLogic::FitWalk(heli, heli.clips.front().id, animations, heli.pathSpeed);
    const auto at = [&](std::uint32_t tick) { return ActorLogic::EvaluatePlacement(heli, tick, animations); };
    const auto flying = at(7000);
    const auto hit = ActorLogic::AddDeath(heli, 7000, std::string(Killstreaks::MOTION_EXPLODE), 0.5f);
    const auto death = std::ranges::find(heli.clips, Killstreaks::MOTION_EXPLODE, &Types::ActorClip::animation);
    CHECK(death != heli.clips.end() && !death->loop && !death->moves && death->length == 500);
    const auto blast = std::ranges::find(heli.events, hit, &Types::ActorEvent::id);
    CHECK(blast != heli.events.end() && blast->effect == "vehicle/vexplosion/fx_vexplode_helicopter_exp_mp" &&
          blast->tag == "tag_origin" && blast->death == death->id);
    const auto hide = std::ranges::find(heli.events, Types::ActorEventType::Hide, &Types::ActorEvent::type);
    CHECK(hide != heli.events.end() && hide->tick == 7100 && hide->death == death->id);
    CHECK(ActorLogic::IsVisible(heli, 7050) && !ActorLogic::IsVisible(heli, 7150));
    // (its blast stays where it went off as it is gone, as the game's playfx)
    CHECK(ActorLogic::EffectStays(heli, *blast));
    // (on from where it was at the speed it had: off its path)
    CHECK(glm::distance(at(7000).position, flying.position) < 0.5f);
    CHECK_NEAR(at(7050).position.x - at(7000).position.x, 704.0f * 0.05f, 1.0f);

    // a Gunship spins out: hit in its engine, its tail rotor damaged, smoke and fire trailing as it spins, then blows
    // up at its end and is gone
    auto gunship = Killstreaks::MakeStart(Killstreak::Gunship, 1000, 40000, 0.0f);
    gunship.path = {{0, 0, 1100}, {3000, 0, 1100}};
    ActorLogic::FitWalk(gunship, gunship.clips.front().id, animations, gunship.pathSpeed);
    const auto spinAt = gunship.clips[0].End() - 500;
    const auto before = ActorLogic::EvaluatePlacement(gunship, spinAt, animations);
    ActorLogic::AddDeath(gunship, spinAt, std::string(Killstreaks::MOTION_SPINOUT), 3.0f);
    const auto spin = std::ranges::find(gunship.clips, Killstreaks::MOTION_SPINOUT, &Types::ActorClip::animation);
    CHECK(spin != gunship.clips.end() && spin->length == 3000 && spin->End() == spinAt + 3000);
    std::vector<std::string> effects;
    for (const auto& event : gunship.events)
        if (event.death == spin->id && event.type != Types::ActorEventType::Hide)
            effects.push_back(event.effect + "@" + std::to_string(event.tick - spinAt));
    CHECK((effects == std::vector<std::string>{"vehicle/vexplosion/fx_vexplode_heli_killstreak_exp_sm@0",
                                               "vehicle/props/fx_huey_small_blade_dmg@0",
                                               "trail/fx_trail_heli_killstreak_tail_smoke@0",
                                               "trail/fx_trail_heli_killstreak_engine_smoke@0",
                                               "vehicle/vexplosion/fx_vexplode_helicopter_exp_mp@3000"}));
    CHECK(ActorLogic::IsVisible(gunship, spinAt + 3050) && !ActorLogic::IsVisible(gunship, spinAt + 3150));
    // (the blast at its start and its trails go with it, the explosion at its end stays where it went off; its running
    // tail rotor stops as the damaged one starts)
    for (const auto& event : gunship.events)
        if (event.death == spin->id && event.type != Types::ActorEventType::Hide)
            CHECK(ActorLogic::EffectStays(gunship, event) == event.afterDeathEnd);
    CHECK(!Killstreaks::HasBrokenTailRotor(gunship, spinAt - 1) && Killstreaks::HasBrokenTailRotor(gunship, spinAt));
    CHECK(!Killstreaks::HasBrokenTailRotor(heli, 9000));
    // spinning up to its 90 degrees a second over 3 s: 135 by then, on along the way it went
    const auto spun = ActorLogic::EvaluatePlacement(gunship, spinAt + 3000, animations);
    CHECK_NEAR(std::remainder(spun.yaw - before.yaw, 360.0f), 135.0f, 1.0f);
    CHECK(spun.position.x > before.position.x + 100.0f && std::abs(spun.position.y - before.position.y) < 1.0f);
    // its own Spin and Speed: 180 degrees a second turns twice as far, 80 mph flies
    // farther; Auto (0) is the killstreak's, and both are saved with the scene
    {
        auto own = gunship;
        auto& clip = *std::ranges::find(own.clips, Killstreaks::MOTION_SPINOUT, &Types::ActorClip::animation);
        CHECK(Killstreaks::GetSpinRate(Killstreaks::Get(Killstreak::Gunship), clip) == 90.0f);
        clip.spinRate = 180.0f;
        clip.crashSpeed = Killstreaks::FromMph(80.0f);
        const auto ownSpun = ActorLogic::EvaluatePlacement(own, spinAt + 3000, animations);
        CHECK_NEAR(std::remainder(ownSpun.yaw - before.yaw, 360.0f), -90.0f, 1.0f);  // 270 to the left
        CHECK(ownSpun.position.x > spun.position.x + 50.0f);
        const auto saved = ActorLogic::SceneFromJson(
            nlohmann::json::parse(ActorLogic::SceneToJson(ActorLogic::Scene{{own}, ""}, "demo").dump()), 99999);
        const auto read =
            std::ranges::find(saved.actors[0].clips, Killstreaks::MOTION_SPINOUT, &Types::ActorClip::animation);
        CHECK(read->spinRate == 180.0f && read->crashSpeed == Killstreaks::FromMph(80.0f));
    }

    // its effects and Hide move with it, keep where in it they are, and go with it
    auto moved = gunship;
    const auto spinId = spin->id;
    const auto endId = std::ranges::find_if(moved.events, [&](const Types::ActorEvent& event) {
                           return event.afterDeathEnd && event.type == Types::ActorEventType::Effect;
                       })->id;
    const auto tickOf = [&](std::uint32_t id) {
        return std::ranges::find(moved.events, id, &Types::ActorEvent::id)->tick;
    };
    std::ranges::find(moved.clips, spinId, &Types::ActorClip::id)->start += 400;
    ActorLogic::Normalize(moved);
    CHECK(tickOf(endId) == spinAt + 3400);
    ActorLogic::MoveEvent(moved, endId, spinAt + 3600);
    std::ranges::find(moved.clips, spinId, &Types::ActorClip::id)->length = 4000;
    ActorLogic::Normalize(moved);
    CHECK(tickOf(endId) == spinAt + 400 + 4000 + 200);
    // (saved with the scene as they are)
    const auto read = ActorLogic::SceneFromJson(
        nlohmann::json::parse(ActorLogic::SceneToJson(ActorLogic::Scene{{moved}, ""}, "demo").dump()), 99999);
    const auto describe = [](const Types::Actor& actor) {
        std::vector<std::string> events;
        for (const auto& event : actor.events)
        {
            const auto death = std::ranges::find(actor.clips, event.death, &Types::ActorClip::id);
            events.push_back(std::format("{} {} {} {} {} {} {}", static_cast<int>(event.type), event.tick,
                                         event.effect, event.tag, event.deathAt, event.afterDeathEnd,
                                         death != actor.clips.end() ? death->animation : "-"));
        }
        return events;
    };
    CHECK(read.actors.size() == 1 && describe(read.actors[0]) == describe(moved));
    // (killstreaks play no sounds: a scene of an older build that gave its events sounds reads without them)
    auto older = ActorLogic::SceneToJson(ActorLogic::Scene{{moved}, ""}, "demo");
    for (auto& event : older["actors"][0]["events"])
        event["sound"] = "evt_helicopter_midair_exp";
    CHECK(describe(ActorLogic::SceneFromJson(older, 99999).actors[0]) == describe(moved));
    const auto saved = ActorLogic::SceneToJson(ActorLogic::Scene{{moved}, ""}, "demo").dump();
    CHECK(saved.find("\"sound\"") == std::string::npos);
    std::erase_if(moved.clips, [&](const Types::ActorClip& clip) { return clip.id == spinId; });
    ActorLogic::Normalize(moved);
    CHECK(std::ranges::none_of(moved.events, [](const Types::ActorEvent& event) {
        return event.type != Types::ActorEventType::Hit && event.death != 0;
    }));

    // swapped to Explode its events are Explode's; swapped to a Hover, none is left
    ActorLogic::SwapAnimation(gunship, spinId, std::string(Killstreaks::MOTION_EXPLODE), animations);
    const auto linked =
        std::ranges::count_if(gunship.events, [&](const Types::ActorEvent& event) { return event.death == spinId; });
    CHECK(linked == 2);
    CHECK(std::ranges::find(gunship.events, std::string("vehicle/vexplosion/fx_vexplode_helicopter_exp_mp"),
                            &Types::ActorEvent::effect) != gunship.events.end());
    ActorLogic::SwapAnimation(gunship, spinId, std::string(Killstreaks::MOTION_HOVER), animations);
    CHECK(std::ranges::none_of(gunship.events, [&](const Types::ActorEvent& event) { return event.death == spinId; }));

    // the RC-XD goes off, a turret sparks then blows up
    auto car = Killstreaks::MakeStart(Killstreak::RcXd, 1000, 30000, 0.0f);
    ActorLogic::AddDeath(car, 3000, std::string(Killstreaks::MOTION_DETONATE), 0.5f);
    CHECK(std::ranges::find(car.events, std::string("maps/mp_maps/fx_mp_exp_rc_bomb"), &Types::ActorEvent::effect) !=
          car.events.end());
    auto sentry = Killstreaks::MakeStart(Killstreak::SentryGun, 1000, 30000, 0.0f);
    ActorLogic::AddDeath(sentry, 3000, std::string(Killstreaks::MOTION_DESTROYED), 0.5f);
    const auto blew =
        std::ranges::find(sentry.events, std::string("explosions/fx_exp_equipment_lg"), &Types::ActorEvent::effect);
    CHECK(blew != sentry.events.end() && blew->tick == 3100);
    CHECK(!ActorLogic::IsVisible(sentry, 3250));
}

TEST("killstreaks: helicopters alone raise dust, and the blasts the game plays with playfx face up")
{
    for (const auto& info : Killstreaks::GetAll())
        CHECK((info.dust != nullptr) == (info.vehicleClass == VehicleClass::Helicopter));

    const auto effects = [](Killstreak killstreak, std::string_view motion) {
        std::vector<std::string> names;
        for (const auto& event : Killstreaks::GetDeathEvents(Killstreaks::Get(killstreak), motion, 5000))
        {
            if (!event.effect.empty())
                names.emplace_back(event.effect);
        }
        return names;
    };
    const auto faceUp = [](const std::vector<std::string>& names) {
        return std::ranges::count_if(names, [](const std::string& name) { return Killstreaks::FacesUp(name); });
    };
    // heli_explode and lbExplode: straight up, wherever its nose was
    for (const auto killstreak :
         {Killstreak::AttackHelicopter, Killstreak::Gunship, Killstreak::ChopperGunner, Killstreak::CarePackageChopper})
    {
        CHECK(faceUp(effects(killstreak, Killstreaks::MOTION_EXPLODE)) == 1);
        CHECK(faceUp(effects(killstreak, Killstreaks::MOTION_SPINOUT)) == 1);
    }
    // a turret's blast too, not the sparks before it
    CHECK(faceUp(effects(Killstreak::SentryGun, Killstreaks::MOTION_DESTROYED)) == 1);
    // the Spy Plane's goes on its tag, the RC-XD's sideways
    CHECK(faceUp(effects(Killstreak::SpyPlane, Killstreaks::MOTION_EXPLODE)) == 0);
    CHECK(faceUp(effects(Killstreak::RcXd, Killstreaks::MOTION_DETONATE)) == 0);
}
