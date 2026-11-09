#include "StdInclude.hpp"
#include "Test.hpp"

#include <set>

#include "Components/ActorLogic.hpp"
#include "Components/PlayerLookLogic.hpp"
#include "Components/ShotLogic.hpp"

using namespace IWXMVM;
using namespace IWXMVM::Components;
namespace Shots = IWXMVM::Components::ShotLogic;

// Where an actor's bullet lands, a gun on a back and the Death Machine's walks

namespace
{
    bool Near(float a, float b, float tolerance = 0.001f)
    {
        return std::abs(a - b) <= tolerance;
    }

    bool Near(const glm::vec3& a, const glm::vec3& b, float tolerance = 0.001f)
    {
        return glm::distance(a, b) <= tolerance;
    }

    const glm::vec3 EAST(1.0f, 0.0f, 0.0f);
}  // namespace

TEST("shots: a line meets the side of a capsule at its radius")
{
    const auto hit = Shots::HitCapsule({0, 0, 40}, EAST, 1000.0f, {100, 0, 0}, {100, 0, 72}, 10.0f);
    CHECK(hit.has_value());
    CHECK(Near(*hit, 90.0f));
}

TEST("shots: it meets the round end past the capsule's axis")
{
    // straight down onto the top
    const auto top = Shots::HitCapsule({100, 0, 200}, {0, 0, -1}, 1000.0f, {100, 0, 0}, {100, 0, 72}, 10.0f);
    CHECK(top.has_value());
    CHECK(Near(*top, 200.0f - 82.0f));
    // over the top but within the round end
    const auto over = Shots::HitCapsule({0, 0, 78}, EAST, 1000.0f, {100, 0, 0}, {100, 0, 72}, 10.0f);
    CHECK(over.has_value());
    CHECK(Near(*over, 100.0f - 8.0f));
}

TEST("shots: a line that misses, ends short or points away meets nothing")
{
    CHECK(!Shots::HitCapsule({0, 30, 40}, EAST, 1000.0f, {100, 0, 0}, {100, 0, 72}, 10.0f));
    CHECK(!Shots::HitCapsule({0, 0, 40}, EAST, 50.0f, {100, 0, 0}, {100, 0, 72}, 10.0f));
    CHECK(!Shots::HitCapsule({0, 0, 40}, -EAST, 1000.0f, {100, 0, 0}, {100, 0, 72}, 10.0f));
    CHECK(!Shots::HitCapsule({0, 0, 90}, EAST, 1000.0f, {100, 0, 0}, {100, 0, 72}, 10.0f));
}

TEST("shots: a gun inside a body doesn't hit it")
{
    CHECK(!Shots::HitCapsule({100, 0, 40}, EAST, 1000.0f, {100, 0, 0}, {100, 0, 72}, 10.0f));
    CHECK(!Shots::HitCapsule({0, 0, 0}, EAST, 1000.0f, {100, 0, 0}, {100, 0, 72}, 0.0f));
}

TEST("shots: a line along a capsule's axis meets its end")
{
    const auto hit = Shots::HitCapsule({-100, 0, 0}, EAST, 1000.0f, {0, 0, 0}, {50, 0, 0}, 5.0f);
    CHECK(hit.has_value());
    CHECK(Near(*hit, 95.0f));
}

TEST("shots: the nearest of a body's parts is the one hit, with a normal out of it")
{
    const std::array<Shots::Capsule, 2> parts{Shots::Capsule{{100, 0, 0}, {100, 0, 40}, 8.0f, 2},
                                              Shots::Capsule{{90, 0, 0}, {90, 0, 40}, 6.0f, 0}};
    const auto hit = Shots::FindBodyHit({0, 0, 20}, EAST, 1000.0f, parts);
    CHECK(hit.has_value());
    CHECK(hit->part == 0);
    CHECK(Near(hit->distance, 84.0f));
    CHECK(Near(hit->point, {84, 0, 20}));
    CHECK(Near(hit->normal, -EAST));
    CHECK(!Shots::FindBodyHit({0, 0, 20}, EAST, 10.0f, parts));
}

TEST("shots: a line far from a body doesn't ask for its bones")
{
    CHECK(Shots::PassesNear({0, 0, 40}, EAST, 1000.0f, {500, 30, 36}, Shots::BODY_REACH));
    CHECK(!Shots::PassesNear({0, 0, 40}, EAST, 1000.0f, {500, 300, 36}, Shots::BODY_REACH));
    // nor past where it ends
    CHECK(!Shots::PassesNear({0, 0, 40}, EAST, 100.0f, {500, 0, 36}, Shots::BODY_REACH));
}

TEST("shots: a body's parts name bones that join up from the feet to the head")
{
    for (const auto& part : Shots::BODY_PARTS)
    {
        CHECK(part.radius > 0.0f);
        CHECK(!part.from.empty());
        CHECK(!part.to.empty());
    }
}

TEST("shots: an effect's axes on a surface are the normal and two across it, a right-handed set")
{
    for (const auto& normal : {glm::vec3(0, 0, 1), glm::vec3(0, 0, -1), glm::vec3(1, 0, 0), glm::vec3(0, 1, 0),
                               glm::normalize(glm::vec3(1, 2, 3)), glm::normalize(glm::vec3(-3, 1, 0.5f))})
        for (const float turn : {0.0f, 45.0f, 200.0f})
        {
            const auto axes = Shots::GetImpactAxes(normal, turn);
            CHECK(Near(axes.forward, normal));
            CHECK(Near(glm::length(axes.left), 1.0f));
            CHECK(Near(glm::length(axes.up), 1.0f));
            CHECK(Near(glm::dot(axes.left, normal), 0.0f));
            CHECK(Near(glm::cross(axes.forward, axes.left), axes.up));
        }
}

TEST("shots: the game's line in the surface, unturned, is the one it starts from")
{
    // (0x48C290: (-z, x, -y) of the normal, less what lies along it)
    const auto axes = Shots::GetImpactAxes({0, 0, 1}, 0.0f);
    CHECK(Near(axes.left, {-1, 0, 0}));
    CHECK(Near(Shots::GetImpactAxes({0, 0, 1}, 90.0f).left, {0, -1, 0}));
}

TEST("shots: a shot's turn comes from its tick, so a rewind gives the same decal, and shots differ")
{
    CHECK(Shots::GetImpactTurn(20100) == Shots::GetImpactTurn(20100));
    std::set<float> turns;
    for (std::uint32_t shot = 0; shot < 20; ++shot)
    {
        const auto turn = Shots::GetImpactTurn(20000 + shot * 50);
        CHECK(turn >= 0.0f && turn < 360.0f);
        turns.insert(turn);
    }
    CHECK(turns.size() >= 15);
}

TEST("shots: a rifle's bullets go a little off the barrel, a shotgun's pellets as its file spreads them")
{
    CHECK(Shots::GetSpread(1, 0.0f).pellets == 1);
    CHECK(Near(Shots::GetSpread(1, 0.0f).degrees, Shots::GUN_SCATTER));
    // a SPAS-12's, and the Death Machine's three a shot
    CHECK(Shots::GetSpread(8, 5.5f).pellets == 8);
    CHECK(Near(Shots::GetSpread(8, 5.5f).degrees, 5.5f));
    CHECK(Shots::GetSpread(3, 0.0f).pellets == 3);
    CHECK(Near(Shots::GetSpread(3, 0.0f).degrees, Shots::GUN_SCATTER));
    // a Fire event's own, nought too
    CHECK(Near(Shots::GetSpread(8, 5.5f, 2.0f).degrees, 2.0f));
    CHECK(Near(Shots::GetSpread(1, 0.0f, 0.0f).degrees, 0.0f));
    CHECK(Shots::MOST_SPREAD >= 5.5f);
    // what the file doesn't say
    CHECK(Shots::GetSpread(0, 0.0f).pellets == 1);
    CHECK(Shots::GetSpread(1000, 0.0f).pellets == Shots::MOST_PELLETS);
}

TEST("shots: each pellet stays within the spread, the same at every play, and they fill it")
{
    const Shots::Axes barrel = Shots::GetImpactAxes(glm::normalize(glm::vec3(1, 2, 0.5f)), 30.0f);
    constexpr float DEGREES = 5.5f;
    std::set<float> ways;
    float farthest = 0.0f;
    for (std::uint32_t tick = 20000; tick < 20050; ++tick)
        for (std::uint32_t pellet = 0; pellet < 8; ++pellet)
        {
            const auto direction = Shots::GetPelletDirection(barrel, tick, pellet, DEGREES);
            CHECK(Near(glm::length(direction), 1.0f));
            CHECK(Near(direction, Shots::GetPelletDirection(barrel, tick, pellet, DEGREES), 0.0f));
            const auto off = glm::degrees(std::acos(std::clamp(glm::dot(direction, barrel.forward), -1.0f, 1.0f)));
            CHECK(off <= DEGREES + 0.01f);
            farthest = std::max(farthest, off);
            ways.insert(direction.x);
        }
    CHECK(farthest >= DEGREES * 0.9f);
    CHECK(ways.size() >= 350);
}

TEST("shots: a rocket flies from its shot to where it lands, and isn't there before or after")
{
    const Shots::RocketFlight flight{20000, {0, 0, 40}, EAST, 1500.0f, 750.0f};
    CHECK(!Shots::GetRocketPoint(flight, 19999));
    CHECK(Near(*Shots::GetRocketPoint(flight, 20000), {0, 0, 40}));
    CHECK(Near(*Shots::GetRocketPoint(flight, 20200), {300, 0, 40}));
    CHECK(!Shots::HasLanded(flight, 20499));
    CHECK(Shots::HasLanded(flight, 20500));
    CHECK(!Shots::GetRocketPoint(flight, 20500));
    CHECK(!Shots::HasLanded(flight, 19000));
}

TEST("shots: a missile homes on where its target is at each tick, and is there once it has flown that far")
{
    // (the SAM's)
    const Shots::RocketFlight flight{20000, {0, 0, 0}, EAST, 3000.0f, 10000.0f};
    const glm::vec3 target(0, 3000, 4000);  // 5000 away
    CHECK(!Shots::GetHomingPoint(flight, target, 19999));
    CHECK(Near(*Shots::GetHomingPoint(flight, target, 21000), {0, 1800, 2400}, 0.01f));
    // (the same tick and target give the same point: a rewind or Speed sees what playing did)
    CHECK(Near(*Shots::GetHomingPoint(flight, target, 21000), *Shots::GetHomingPoint(flight, target, 21000)));
    // a target that moves: along the line to where it is now
    CHECK(Near(*Shots::GetHomingPoint(flight, {3000, 4000, 0}, 21000), {1800, 2400, 0}, 0.01f));
    CHECK(!Shots::HasHomed(flight, target, 21666));
    CHECK(Shots::HasHomed(flight, target, 21667));
    CHECK(!Shots::GetHomingPoint(flight, target, 21700));
}

TEST("shots: hits stop when the game's effects run short, so a long burst can't crowd out the rest")
{
    CHECK(Shots::HasRoomForHit(0));
    CHECK(Shots::HasRoomForHit(Shots::MOST_EFFECTS_FOR_HITS - 1));
    CHECK(!Shots::HasRoomForHit(Shots::MOST_EFFECTS_FOR_HITS));
    CHECK(Shots::MOST_EFFECTS_FOR_HITS < 1024);
}

TEST("shots: what is flesh to a bullet with no surface of its own")
{
    for (const auto type : {1, 2, 17, 19})
        CHECK(Shots::IsFleshEntity(type));
    for (const auto type : {0, 3, 6, 14, 18, 20})
        CHECK(!Shots::IsFleshEntity(type));
}

TEST("shots: a Fire leaves hits unless turned off, and that and its spread stay in the scene's file")
{
    Types::Actor actor;
    CHECK(Types::ActorEvent{}.hits);
    CHECK(Types::ActorEvent{}.spread < 0.0f);
    Types::ActorEvent fire{.type = Types::ActorEventType::Fire, .tick = 100, .hits = false, .spread = 2.5f};
    actor.events.push_back(fire);
    actor.stowedWeapon = "ak47_mp";
    actor.stowedAttachments = {"acog", "grip"};
    actor.stowedCamo = 3;
    ActorLogic::Scene scene;
    scene.actors.push_back(actor);
    const auto read = ActorLogic::SceneFromJson(nlohmann::json::parse(ActorLogic::SceneToJson(scene, "demo").dump()), 60000);
    CHECK(read.actors.size() == 1);
    CHECK(read.actors[0].events.size() == 1);
    CHECK(!read.actors[0].events[0].hits);
    CHECK(Near(read.actors[0].events[0].spread, 2.5f));
    CHECK(read.actors[0].stowedWeapon == "ak47_mp");
    CHECK((read.actors[0].stowedAttachments == std::vector<std::string>{"acog", "grip"}));
    CHECK(read.actors[0].stowedCamo == 3);

    // a scene from before them has hits on and nothing stowed
    auto old = ActorLogic::SceneToJson(scene, "demo");
    for (auto& node : old["actors"])
    {
        node.erase("stowedWeapon");
        node.erase("stowedAttachments");
        node.erase("stowedCamo");
        for (auto& event : node["events"])
        {
            event.erase("hits");
            event.erase("spread");
        }
    }
    const auto before = ActorLogic::SceneFromJson(old, 60000);
    CHECK(before.actors[0].events[0].hits);
    CHECK(before.actors[0].events[0].spread < 0.0f);
    CHECK(before.actors[0].stowedWeapon.empty());
    CHECK(before.actors[0].stowedAttachments.empty());
    CHECK(before.actors[0].stowedCamo == 0);
}

TEST("stowed: a look keeps the gun on the back")
{
    Types::Actor actor;
    actor.stowedWeapon = "ak47_mp";
    actor.stowedAttachments = {"acog"};
    actor.stowedCamo = 5;
    Types::Actor other;
    ActorLogic::ApplyLook(other, nlohmann::json::parse(ActorLogic::LookToJson(actor).dump()));
    CHECK(other.stowedWeapon == "ak47_mp");
    CHECK((other.stowedAttachments == std::vector<std::string>{"acog"}));
    CHECK(other.stowedCamo == 5);
}

namespace
{
    namespace Look = IWXMVM::Components::PlayerLookLogic;
    const Look::Models STOWED_PLAYER{{"c_usa_cia_mp_body", ""},
                                     {"c_usa_cia_mp_head_1", ""},
                                     {"t5_weapon_ex41_world", "tag_stowed_back"}};
    constexpr std::size_t MOST_MODELS = 7;
}  // namespace

TEST("stowed: a player's chosen gun takes the place of the demo's, and none takes it off")
{
    Look::ResolvedCharacter chosen;
    chosen.stowed = "ak47_mp";
    CHECK(chosen.HasModels());
    CHECK((Look::GetWanted(STOWED_PLAYER, chosen, MOST_MODELS, std::string("t5_weapon_ak47_world")) ==
           Look::Models{{"c_usa_cia_mp_body", ""},
                        {"c_usa_cia_mp_head_1", ""},
                        {"t5_weapon_ak47_world", "tag_stowed_back"}}));
    chosen.stowed = std::string();
    CHECK((Look::GetWanted(STOWED_PLAYER, chosen, MOST_MODELS, std::string()) ==
           Look::Models{{"c_usa_cia_mp_body", ""}, {"c_usa_cia_mp_head_1", ""}}));
}

TEST("stowed: a player with nothing stowed gets the chosen gun, after what it wears")
{
    Look::ResolvedCharacter chosen;
    chosen.stowed = "ak47_mp";
    chosen.worn = std::vector<std::string>{"gear"};
    const Look::Models bare(STOWED_PLAYER.begin(), STOWED_PLAYER.begin() + 2);
    CHECK((Look::GetWanted(bare, chosen, MOST_MODELS, std::string("gun")) ==
           Look::Models{{"c_usa_cia_mp_body", ""}, {"c_usa_cia_mp_head_1", ""}, {"gear", ""}, {"gun", "tag_stowed_back"}}));
}

TEST("stowed: with nothing chosen the demo's stays, and a loadout keeps the choice")
{
    CHECK((Look::GetWanted(STOWED_PLAYER, {}, MOST_MODELS) == STOWED_PLAYER));

    Types::Loadout loadout;
    CHECK(loadout.IsEmpty());
    loadout.stowed = "ak47_mp";
    CHECK(!loadout.IsEmpty());
    CHECK(Look::Resolve(&loadout, nullptr, {}).stowed == "ak47_mp");
    // the player's over all players'
    Types::Loadout all;
    all.stowed = "m16_mp";
    CHECK(Look::Resolve(&loadout, nullptr, all).stowed == "ak47_mp");
    CHECK(Look::Resolve(nullptr, nullptr, all).stowed == "m16_mp");
    Types::PlayerChoices choices;
    choices.loadoutForAll = all;
    choices.playerLoadouts[3] = loadout;
    const auto read = Look::ChoicesFromJson(nlohmann::json::parse(Look::ChoicesToJson(choices).dump()));
    CHECK(read.loadoutForAll.stowed == "m16_mp");
    CHECK(read.playerLoadouts.at(3).stowed == "ak47_mp");
}

TEST("a new Fire goes at the gun's speed, and a launcher's is one rocket")
{
    const auto ak = ActorLogic::MakeGunFire(80, Types::GunClass::Rifle, 20000);
    CHECK(ak.type == Types::ActorEventType::Fire);
    CHECK(ak.tick == 20000);
    CHECK(ak.shots == 5);
    CHECK(ak.interval == 80);
    CHECK(ActorLogic::MakeGunFire(50, Types::GunClass::Minigun, 0).interval == 50);
    CHECK(ActorLogic::MakeGunFire(480, Types::GunClass::RocketLauncher, 0).shots == 1);
    // what the map doesn't know keeps the default
    CHECK(ActorLogic::MakeGunFire(0, Types::GunClass::Rifle, 0).interval == Types::ActorEvent{}.interval);
}

TEST("the Death Machine stands, walks and runs as its own, and never sprints or lies down")
{
    using Stance = Types::ActorStance;
    const auto gun = Types::GunClass::Minigun;
    CHECK(ActorLogic::GetStanceIdle(gun, Stance::Stand) == "pb_minigun_stand_idle");
    CHECK(ActorLogic::GetStanceIdle(gun, Stance::Crouch) == "pb_minigun_crouch_idle");
    const ActorLogic::Body body(gun);
    CHECK(ActorLogic::GetLocomotionAnimation(body, Stance::Stand, ActorLogic::Gait::Walk, ActorLogic::Heading::Forward) ==
          "pb_minigun_stand_walk");
    CHECK(ActorLogic::GetLocomotionAnimation(body, Stance::Stand, ActorLogic::Gait::Run, ActorLogic::Heading::Left) ==
          "pb_minigun_stand_run_left");
    CHECK(ActorLogic::GetLocomotionAnimation(body, Stance::Crouch, ActorLogic::Gait::Run, ActorLogic::Heading::Back) ==
          "pb_minigun_crouch_run_back");

    // lying down the rifle's, by the fallback
    const auto has = [](std::string_view name) { return !name.empty(); };
    CHECK(ActorLogic::FindStanceIdle(gun, Stance::Prone, has) ==
          std::string(ActorLogic::GetStanceIdle(Types::GunClass::Rifle, Stance::Prone)));
    // and a sprint is its run, as a set with no sprint has it
    CHECK(ActorLogic::FindLocomotionAnimation(body, Stance::Stand, ActorLogic::Gait::Sprint, ActorLogic::Heading::Forward,
                                              has) == "pb_minigun_stand_run");

    // and its clips are locomotion of that gun
    const auto found = ActorLogic::FindLocomotion("pb_minigun_stand_run");
    CHECK(found.has_value());
    CHECK(found->gun == gun);
    CHECK(found->gait == ActorLogic::Gait::Run);
}
