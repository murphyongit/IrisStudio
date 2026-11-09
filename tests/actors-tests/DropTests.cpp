#include "StdInclude.hpp"
#include "Test.hpp"

#include "Components/ActorLogic.hpp"
#include "Components/DropLogic.hpp"
#include "Components/KillstreakLogic.hpp"
#include "Components/VehicleLogic.hpp"

using namespace IWXMVM;

// What a killstreak's Fire does: the game's numbers, from _napalm.gsc, _airstrike.gsc
// and _supplydrop.gsc, and that it all comes from the actor alone. And a prop on its own (11).

namespace
{
    namespace Killstreaks = Components::KillstreakLogic;
    namespace Drops = Components::DropLogic;
    using Types::Killstreak;

    constexpr std::uint32_t START = 1000;
    constexpr float GROUND = 0.0f;

    // flat ground at a height, everywhere
    std::optional<Drops::Hit> Flat(glm::vec3 from, glm::vec3 to)
    {
        if (from.z < GROUND || to.z > GROUND)
            return std::nullopt;
        const auto share = (from.z - GROUND) / (from.z - to.z);
        return Drops::Hit{glm::mix(from, to, share), glm::vec3(0.0f, 0.0f, 1.0f)};
    }

    // going along x at a speed from x 0 at tick 0, at a height
    Drops::Place Going(float speed, float height)
    {
        return [=](std::uint32_t tick) {
            return Types::ActorPlacement{glm::vec3(speed * static_cast<float>(tick) / 1000.0f, 0.0f, height), 0.0f};
        };
    }

    // an event as the panel adds it, with what an actor of the killstreak is
    Types::ActorEvent Fire(Killstreak killstreak, const Types::Actor& actor, std::uint32_t tick)
    {
        return Killstreaks::MakeFire(Killstreaks::Get(killstreak), actor, tick);
    }

    float Seconds(std::uint32_t ticks)
    {
        return static_cast<float>(ticks) / 1000.0f;
    }

    std::size_t CountPlays(const Drops::Drop& drop, std::string_view part)
    {
        return static_cast<std::size_t>(std::ranges::count_if(
            drop.plays, [&](const Drops::Play& play) { return play.effect.find(part) != std::string_view::npos; }));
    }
}  // namespace

TEST("fire: every killstreak fires what the game has it fire, or says why not")
{
    const auto can = [](Killstreak killstreak, std::size_t look = 0) {
        return Killstreaks::CanFire(Killstreaks::Get(killstreak), look);
    };
    for (const auto killstreak : {Killstreak::AttackHelicopter, Killstreak::Gunship, Killstreak::ChopperGunner,
                                  Killstreak::SentryGun, Killstreak::SamTurret, Killstreak::Napalm,
                                  Killstreak::RollingThunder, Killstreak::CarePackageChopper, Killstreak::RcXd})
        CHECK(can(killstreak));
    // the spy plane has nothing; a care package only goes off booby-trapped
    CHECK(!can(Killstreak::SpyPlane));
    CHECK(!can(Killstreak::CarePackage, 0));
    CHECK(!can(Killstreak::CarePackage, 1));
    CHECK(can(Killstreak::CarePackage, 2));
}

TEST("fire: a gun's Fire is a second of its shots, as before")
{
    const auto sentry = Killstreaks::MakeStart(Killstreak::SentryGun, START, 30000, 0.0f);
    const auto fire = Fire(Killstreak::SentryGun, sentry, 5000);
    CHECK(fire.tick == 5000 && fire.shots == 20 && fire.interval == 50 && fire.tracers);
}

TEST("fire: a plane's bombs go when the game's do, so they land under the middle of its pass")
{
    auto napalm = Killstreaks::MakeStart(Killstreak::Napalm, START, 30000, 0.0f);
    const auto fire = Fire(Killstreak::Napalm, napalm, 20000);
    CHECK(fire.shots == 3 && fire.interval == 200 && !fire.tracers);
    // half its fall of 1.6 s before the middle of its 6.857 s pass, wherever the playhead is
    const auto fall = std::sqrt(2.0f * 1030.0f / Components::VehicleLogic::GRAVITY);
    const auto middle = START + napalm.clips.front().length / 2;
    CHECK_NEAR(static_cast<float>(fire.tick), static_cast<float>(middle) - fall * 500.0f, 1.0f);
    // the bomb flies on 3500 u/s for that fall: where the plane is by the middle
    CHECK_NEAR(3500.0f * fall, 7000.0f * fall / 2.0f, 0.01f);

    // a second one, there already, goes at the playhead
    ActorLogic::AddEvent(napalm, fire);
    CHECK(Fire(Killstreak::Napalm, napalm, 20000).tick == 20000);

    // Rolling Thunder bombs from 1.5 s and a quarter of the fall before the middle, for about 4.4 s
    const auto b52 = Killstreaks::MakeStart(Killstreak::RollingThunder, START, 30000, 0.0f);
    const auto run = Fire(Killstreak::RollingThunder, b52, 20000);
    const auto b52Fall = std::sqrt(2.0f * 1230.0f / Components::VehicleLogic::GRAVITY);
    CHECK_NEAR(static_cast<float>(run.tick), START + 6000.0f - 1500.0f - b52Fall * 250.0f, 1.0f);
    CHECK(run.End() - run.tick == 29 * 150);
}

TEST("fire: the chopper's crate goes at the playhead, and the RC-XD's Fire is its death")
{
    const auto chopper = Killstreaks::MakeStart(Killstreak::CarePackageChopper, START, 30000, 0.0f);
    const auto fire = Fire(Killstreak::CarePackageChopper, chopper, 7777);
    CHECK(fire.tick == 7777 && fire.shots == 1);
    CHECK(Killstreaks::Get(Killstreak::RcXd).fireDrop == Killstreaks::FireDrop::Detonate);
    CHECK(Killstreaks::Get(Killstreak::CarePackage).deaths.front() == Killstreaks::MOTION_DETONATE);
    CHECK(Killstreaks::FitsMotion(Types::VehicleClass::Crate, Killstreaks::MOTION_DETONATE));
    // a booby-trapped care package's blast is its own
    const auto blast =
        Killstreaks::GetDeathEvents(Killstreaks::Get(Killstreak::CarePackage), Killstreaks::MOTION_DETONATE, 500);
    CHECK(blast.front().effect == "explosions/fx_grenadeexp_default");
}

TEST("drops: a fall lands where gravity and the ground say")
{
    // from 1030 up: 1.605 s, the bomb going on as it falls
    Drops::Release release{.tick = 0, .point = {0.0f, 0.0f, 1030.0f}, .velocity = {3500.0f, 0.0f, 0.0f}};
    const auto landing = Drops::FindLanding(release, Flat);
    CHECK(landing.has_value());
    const auto fall = std::sqrt(2.0f * 1030.0f / Components::VehicleLogic::GRAVITY);
    CHECK_NEAR(landing->seconds, fall, 0.01f);
    CHECK_NEAR(landing->point.x, 3500.0f * fall, 30.0f);
    CHECK_NEAR(landing->point.z, 0.0f, 1e-3);

    // nothing to land on, within the longest fall
    CHECK(!Drops::FindLanding(release, [](glm::vec3, glm::vec3) { return std::optional<Drops::Hit>{}; }));
}

TEST("drops: the napalm strike's three bombs land 1400 apart, as the game's")
{
    auto napalm = Killstreaks::MakeStart(Killstreak::Napalm, START, 30000, 0.0f);
    const auto& info = Killstreaks::Get(Killstreak::Napalm);
    auto fire = Fire(Killstreak::Napalm, napalm, 0);
    const auto drops = Drops::MakeDrops(info, napalm, fire, Going(7000.0f, 1030.0f), Flat);
    CHECK(drops.size() == 3);
    const auto fall = std::sqrt(2.0f * 1030.0f / Components::VehicleLogic::GRAVITY);
    const auto start = 7000.0f * Seconds(fire.tick);
    for (std::size_t i = 0; i < drops.size(); ++i)
    {
        const auto& drop = drops[i];
        CHECK(drop.model == "projectile_napalm_bomb");
        CHECK(drop.release.tick == fire.tick + i * 200);
        CHECK_NEAR(drop.release.velocity.x, 3500.0f, 1.0f);
        CHECK_NEAR(drop.landing->point.x, start + 1400.0f * i + 3500.0f * fall, 30.0f);
        // the bomb is gone half a second before it lands, the drop effect a tenth after that, in the air
        const auto swap = drop.release.tick + static_cast<std::uint32_t>(std::lround((fall - 0.5f) * 1000.0f));
        CHECK_NEAR(static_cast<float>(drop.goneAt), static_cast<float>(swap), 15.0f);
        CHECK(CountPlays(drop, "fx_napalm_drop_mp") == 1);
        CHECK(drop.plays[0].tick == drop.goneAt + 100);
        CHECK(drop.plays[0].point.z > 400.0f);
        // it bursts where it lands, facing up
        CHECK(CountPlays(drop, "fx_napalm_ground_burst_mp") == 1);
        CHECK_NEAR(glm::distance(drop.plays[1].point, drop.landing->point), 0.0f, 1e-3);
        CHECK_NEAR(drop.plays[1].angles.x, -90.0f, 1e-3);
        // seven fires, small at the ends and large between, on the flat
        CHECK(CountPlays(drop, "ground_fire_sm") == 2);
        CHECK(CountPlays(drop, "ground_fire_lg") == 5);
        CHECK(drop.plays[2].effect.find("_sm_") != std::string_view::npos);
        CHECK(drop.plays[8].effect.find("_sm_") != std::string_view::npos);
    }

    // the fires: 200 apart along the way it goes, 0.4 s apart, the last 0.5 s, facing up, all after the
    // burst... save the first's half a second, which is the game's order
    const auto& plays = drops[0].plays;
    for (std::size_t i = 3; i < plays.size(); ++i)
    {
        CHECK_NEAR(plays[i].point.x - plays[i - 1].point.x, 200.0f, 0.5f);
        CHECK_NEAR(plays[i].point.y, 0.0f, 1e-3);
        CHECK(plays[i].tick - plays[i - 1].tick == (i < 8 ? 400u : 500u));
    }
    CHECK(plays[2].tick == drops[0].goneAt + 650);
    CHECK(plays[8].tick == drops[0].goneAt + 150 + 3000);
    CHECK_NEAR(plays[2].angles.x, 270.0f, 1e-3);
    CHECK_NEAR(plays[2].angles.y, -180.0f, 1e-3);
    // the row runs up to about where the bomb hit
    CHECK(plays[2].point.x < drops[0].landing->point.x && plays[8].point.x > drops[0].landing->point.x - 500.0f);
}

TEST("drops: a ground fire isn't on a wall, and is small on a step")
{
    auto napalm = Killstreaks::MakeStart(Killstreak::Napalm, START, 30000, 0.0f);
    const auto& info = Killstreaks::Get(Killstreak::Napalm);
    auto fire = Fire(Killstreak::Napalm, napalm, 0);
    fire.shots = 1;
    // (where the fires of flat ground are: the last on a wall from here on, a step at the fourth)
    const auto first = Drops::MakeDrops(info, napalm, fire, Going(7000.0f, 1030.0f), Flat);
    CHECK(first.size() == 1);
    const auto lastX = first[0].plays.back().point.x;
    const auto middleX = first[0].plays[5].point.x;
    const auto steep = [&](glm::vec3 from, glm::vec3 to) -> std::optional<Drops::Hit> {
        auto hit = Flat(from, to);
        if (hit && std::abs(hit->point.x - lastX) < 1.0f)
            hit->normal = glm::normalize(glm::vec3(1.0f, 0.0f, 0.2f));
        if (hit && std::abs(hit->point.x - middleX) < 1.0f)
            hit->point.z += 50.0f;
        return hit;
    };
    const auto drops = Drops::MakeDrops(info, napalm, fire, Going(7000.0f, 1030.0f), steep);
    // one on the wall gone, and the one on the step and the one past it small
    CHECK(CountPlays(drops[0], "ground_fire_sm") + CountPlays(drops[0], "ground_fire_lg") == 6);
    CHECK(CountPlays(drops[0], "ground_fire_sm") == 3);
}

TEST("drops: a shot while it is hidden drops nothing, nor what past the most")
{
    auto napalm = Killstreaks::MakeStart(Killstreak::Napalm, START, 30000, 0.0f);
    const auto& info = Killstreaks::Get(Killstreak::Napalm);
    auto fire = Fire(Killstreak::Napalm, napalm, 0);
    fire.tick = 10000;  // past its pass, which hides it
    CHECK(Drops::MakeDrops(info, napalm, fire, Going(7000.0f, 1030.0f), Flat).empty());
    fire.tick = 2000;
    fire.shots = 200;
    fire.interval = 1;
    CHECK(Drops::MakeDrops(info, napalm, fire, Going(7000.0f, 1030.0f), Flat).size() == Drops::MOST_DROPS);
    // a gun's Fire drops nothing
    const auto cobra = Killstreaks::MakeStart(Killstreak::AttackHelicopter, START, 30000, 0.0f);
    CHECK(Drops::MakeDrops(Killstreaks::Get(Killstreak::AttackHelicopter), cobra,
                           Fire(Killstreak::AttackHelicopter, cobra, 2000), Going(0.0f, 800.0f), Flat)
              .empty());
}

TEST("drops: the bomb is where the tick says, the same after a rewind, and falls nose first")
{
    auto napalm = Killstreaks::MakeStart(Killstreak::Napalm, START, 30000, 0.0f);
    const auto& info = Killstreaks::Get(Killstreak::Napalm);
    const auto fire = Fire(Killstreak::Napalm, napalm, 0);
    const auto drop = Drops::MakeDrops(info, napalm, fire, Going(7000.0f, 1030.0f), Flat).front();
    CHECK(!Drops::GetShown(drop, drop.release.tick - 1));
    const auto release = Drops::GetShown(drop, drop.release.tick);
    CHECK(release.has_value());
    CHECK_NEAR(glm::distance(release->point, drop.release.point), 0.0f, 1e-3);
    CHECK_NEAR(release->rotation[0].x, 1.0f, 1e-3);

    const auto mid = drop.release.tick + 800;
    const auto later = Drops::GetShown(drop, mid);
    CHECK_NEAR(later->point.x, drop.release.point.x + 3500.0f * 0.8f, 0.05f);
    CHECK_NEAR(later->point.z, 1030.0f - 0.5f * Components::VehicleLogic::GRAVITY * 0.64f, 0.05f);
    CHECK(later->rotation[0].z < -0.1f);
    CHECK_NEAR(glm::determinant(later->rotation), 1.0f, 1e-3);

    // nothing kept: asked again, after a later tick, the same
    CHECK(!Drops::GetShown(drop, drop.goneAt));
    const auto again = Drops::GetShown(drop, mid);
    CHECK(again->point == later->point);
}

TEST("drops: the care package leaves from behind and under the chopper and stays where it lands")
{
    const auto chopper = Killstreaks::MakeStart(Killstreak::CarePackageChopper, START, 30000, 0.0f);
    const auto& info = Killstreaks::Get(Killstreak::CarePackageChopper);
    const auto fire = Fire(Killstreak::CarePackageChopper, chopper, 5000);
    const auto hovering = [](std::uint32_t) { return Types::ActorPlacement{glm::vec3(100.0f, 200.0f, 850.0f), 90.0f}; };
    const auto drops = Drops::MakeDrops(info, chopper, fire, hovering, Flat);
    CHECK(drops.size() == 1);
    const auto& drop = drops.front();
    CHECK(drop.model == "mp_supplydrop_ally");
    CHECK(drop.plays.empty());
    // facing +y: 400 behind it is -y
    CHECK_NEAR(drop.release.point.x, 100.0f, 1e-3);
    CHECK_NEAR(drop.release.point.y, 200.0f - 400.0f, 1e-3);
    CHECK_NEAR(drop.release.point.z, 650.0f, 1e-3);
    CHECK_NEAR(glm::length(drop.release.velocity), 0.0f, 1e-3);
    // down in the Drop clip's time, then there for good, lying level, turned as it left
    CHECK_NEAR(drop.landing->seconds, Seconds(Components::VehicleLogic::GetDropTicks()), 0.01f);
    const auto landed = Drops::GetShown(drop, 5000 + 2000);
    CHECK_NEAR(landed->point.z, 0.0f, 1e-3);
    CHECK_NEAR(landed->rotation[0].y, 1.0f, 1e-3);
    CHECK_NEAR(landed->rotation[2].z, 1.0f, 1e-3);
    CHECK(Drops::GetShown(drop, 500000).has_value());

    // from a chopper going, it goes on with it
    const auto flying = Drops::MakeDrops(info, chopper, fire, Going(1320.0f, 850.0f), Flat);
    CHECK_NEAR(flying[0].release.velocity.x, 1320.0f, 1.0f);
    CHECK(flying[0].landing->point.x > flying[0].release.point.x + 1600.0f);

    // the enemy chopper's is the enemy's
    auto enemy = chopper;
    enemy.models = {std::string(info.models[1])};
    CHECK(Drops::MakeDrops(info, enemy, fire, hovering, Flat).front().model == "mp_supplydrop_axis");
}

TEST("drops: a crate on a slope lies on it")
{
    const auto chopper = Killstreaks::MakeStart(Killstreak::CarePackageChopper, START, 30000, 0.0f);
    const auto slope = [](glm::vec3 from, glm::vec3 to) {
        auto hit = Flat(from, to);
        if (hit)
            hit->normal = glm::normalize(glm::vec3(0.5f, 0.0f, 1.0f));
        return hit;
    };
    const auto hovering = [](std::uint32_t) { return Types::ActorPlacement{glm::vec3(0.0f, 0.0f, 850.0f), 0.0f}; };
    const auto drop = Drops::MakeDrops(Killstreaks::Get(Killstreak::CarePackageChopper), chopper,
                                       Fire(Killstreak::CarePackageChopper, chopper, 5000), hovering, slope)
                          .front();
    const auto landed = Drops::GetShown(drop, 9000);
    CHECK_NEAR(glm::dot(landed->rotation[2], glm::normalize(glm::vec3(0.5f, 0.0f, 1.0f))), 1.0f, 1e-3);
    CHECK_NEAR(glm::determinant(landed->rotation), 1.0f, 1e-3);
    CHECK(landed->rotation[0].z < 0.0f);
}

TEST("mortar: the Mortar Team's three points about the click, across the way its shells come, and a Fire at the tick")
{
    const auto mortar = Killstreaks::MakeMortar({100.0f, 200.0f, 0.0f}, 90.0f, 4000);
    CHECK(mortar.models.empty());
    CHECK(mortar.mortar && mortar.mortar->points.size() == 3);
    // (the shells come along +y: the points go along x)
    CHECK_NEAR(mortar.mortar->points[0].x, 100.0f + Killstreaks::MORTAR_SPREAD, 0.01);
    CHECK_NEAR(mortar.mortar->points[1].x, 100.0f, 0.01);
    CHECK_NEAR(mortar.mortar->points[2].y, 200.0f, 0.01);
    CHECK(mortar.events.size() == 1 && mortar.events[0].type == Types::ActorEventType::Fire);
    CHECK(mortar.events[0].tick == 4000 && mortar.events[0].shots == 2 && mortar.events[0].interval == 1300);
}

TEST("mortar: two shells on each point, 1.3 s apart, the points 2.5 s apart, the first landing at the Fire's tick")
{
    auto mortar = Killstreaks::MakeMortar({0.0f, 0.0f, GROUND}, 90.0f, 20000);
    const auto drops = Drops::MakeMortarDrops(mortar, mortar.events[0], Flat);
    CHECK(drops.size() == 6);
    // _mortar.gsc: fired 75 degrees up from 6,000 back, seen from 30% along its arc: 5.2 s before it lands
    CHECK_NEAR(static_cast<double>(Drops::GetMortarFlight()), 5236.0, 3.0);
    const std::array<std::uint32_t, 6> lands{20000, 21300, 22500, 23800, 25000, 26300};
    for (std::size_t i = 0; i < drops.size(); ++i)
    {
        const auto& drop = drops[i];
        CHECK(drop.landing.has_value());
        if (!drop.landing)
            continue;
        const auto landTick =
            drop.release.tick + static_cast<std::uint32_t>(std::lround(drop.landing->seconds * 1000.0f));
        CHECK(std::abs(static_cast<int>(landTick) - static_cast<int>(lands[i])) <= 5);
        // on its point
        CHECK(glm::distance(drop.landing->point, mortar.mortar->points[i / 2]) < 5.0f);
        // far up and back as it is first seen, coming down the way the camera looked
        CHECK(drop.release.point.z > 4000.0f);
        CHECK(drop.release.point.y < -3000.0f);
        CHECK(drop.model == Drops::MORTAR_MODEL);
        // a blast where it lands, and gone then
        CHECK(drop.plays.size() == 1 && drop.plays[0].effect == Drops::MORTAR_BLAST && drop.plays[0].tick == landTick);
        CHECK(drop.goneAt == landTick);
        CHECK(!Drops::GetShown(drop, landTick));
        CHECK(Drops::GetShown(drop, landTick - 1000).has_value());
    }
    // (the same from the actor and its event again: a rewind and a jump get them back)
    const auto again = Drops::MakeMortarDrops(mortar, mortar.events[0], Flat);
    CHECK(again.size() == drops.size() && again[3].release.point == drops[3].release.point);
    // more shells a point, as the Shots row has it; a Fire before the film's start sees them later on their arc
    mortar.events[0].shots = 3;
    CHECK(Drops::MakeMortarDrops(mortar, mortar.events[0], Flat).size() == 9);
    mortar.events[0].tick = 1000;
    const auto early = Drops::MakeMortarDrops(mortar, mortar.events[0], Flat);
    CHECK(early.front().release.tick == 0);
    CHECK(early.front().landing && std::abs(early.front().landing->seconds - 1.0f) < 0.01f);
}

TEST("rockets: the Gunship's Fire row fires its rockets, two 0.3 s apart, or its gun again")
{
    const auto& gunship = Killstreaks::Get(Killstreak::Gunship);
    CHECK(Killstreaks::GetRockets(Killstreak::Gunship) != nullptr);
    CHECK(Killstreaks::GetRockets(Killstreak::AttackHelicopter) == nullptr);
    Types::Actor hind;
    hind.models = {std::string(gunship.models[0])};
    auto fire = Fire(Killstreak::Gunship, hind, 5000);
    const auto gunShots = fire.shots, gunInterval = fire.interval;
    Killstreaks::SetRockets(gunship, fire, true);
    CHECK(fire.rockets && fire.shots == 2 && fire.interval == 300);
    Killstreaks::SetRockets(gunship, fire, false);
    CHECK(!fire.rockets && fire.shots == gunShots && fire.interval == gunInterval);
    // (one without rockets keeps its gun)
    auto cobra = Fire(Killstreak::AttackHelicopter, hind, 5000);
    Killstreaks::SetRockets(Killstreaks::Get(Killstreak::AttackHelicopter), cobra, true);
    CHECK(!cobra.rockets);
}

TEST("mortar, rockets: the Mortar Team's points and a Fire's Rockets come back from a saved scene")
{
    ActorLogic::Scene scene;
    scene.actors.push_back(Killstreaks::MakeMortar({10.0f, 20.0f, 30.0f}, 45.0f, 3000));
    scene.actors.back().events[0].rockets = true;
    const auto read = ActorLogic::SceneFromJson(ActorLogic::SceneToJson(scene, "demo"), 60000);
    CHECK(read.actors.size() == 1 && read.actors[0].mortar);
    CHECK(read.actors[0].mortar == scene.actors[0].mortar);
    CHECK(read.actors[0].events.size() == 1 && read.actors[0].events[0].rockets);
}

TEST("props: a model on its own has a bar, and comes back from a saved scene")
{
    auto prop = ActorLogic::MakeProp("p_rus_chair_metal", glm::vec3(10.0f, 20.0f, 30.0f), 45.0f, 1000, 2000);
    prop.id = 3;
    prop.name = "Chair";
    CHECK(prop.models.empty() && prop.clips.empty());
    // at least its length
    CHECK(prop.placedProp->end == 1000 + ActorLogic::PROP_LENGTH);
    CHECK(ActorLogic::GetBar(prop) == std::pair(1000u, 1000u + ActorLogic::PROP_LENGTH));
    ActorLogic::MoveBar(prop, 500);
    CHECK(ActorLogic::GetBar(prop) == std::pair(500u, 500u + ActorLogic::PROP_LENGTH));

    prop.placedProp->pitch = 12.0f;
    prop.placedProp->roll = -7.0f;
    prop.placedProp->scale = 2.5f;
    ActorLogic::Scene scene;
    scene.actors = {prop};
    const auto saved = ActorLogic::SceneToJson(scene, "demo");
    const auto back = ActorLogic::SceneFromJson(saved, 100000).actors;
    CHECK(back.size() == 1);
    CHECK(back.front().placedProp == prop.placedProp);
    CHECK(back.front().position == prop.position && back.front().yaw == 45.0f);

    // scenes without props stay as they were
    Types::Actor person;
    scene.actors = {person};
    CHECK(ActorLogic::SceneToJson(scene, "demo").dump().find("placedProp") == std::string::npos);
}

TEST("props: a bar is a tenth of a second at least, and a scale stays sane")
{
    auto prop = ActorLogic::MakeProp("x", glm::vec3(0.0f), 0.0f, 1000, 2000);
    prop.placedProp->end = 1000;
    prop.placedProp->scale = 100.0f;
    ActorLogic::Normalize(prop);
    CHECK(prop.placedProp->end == 1100);
    CHECK(prop.placedProp->scale == ActorLogic::MAX_PROP_SCALE);
    prop.placedProp->scale = 0.0f;
    ActorLogic::Normalize(prop);
    CHECK(prop.placedProp->scale == ActorLogic::MIN_PROP_SCALE);
}

TEST("props: a prop's name is its model's without its prefixes")
{
    CHECK(ActorLogic::GetPropBaseName("p_rus_chair_metal") == "Chair metal");
    CHECK(ActorLogic::GetPropBaseName("t5_veh_jet_f4_gearup") == "Veh jet f4 gearup");
    CHECK(ActorLogic::GetPropBaseName("barrel") == "Barrel");
}
