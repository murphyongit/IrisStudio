#include "StdInclude.hpp"
#include "Test.hpp"

#include "Components/LiveGameLogic.hpp"

using namespace IWXMVM;

// What the player of a live game can ask of it: a killstreak by its script name, a shot that
// hits the enemy nearest to where it lands, and when a body goes

namespace
{
    namespace Logic = Components::LiveGameLogic;
    using std::chrono::milliseconds;
    using std::chrono::seconds;

    constexpr std::int32_t NO_TEAM = 0;
    constexpr std::int32_t AXIS = 1;
    constexpr std::int32_t ALLIES = 2;
}  // namespace

TEST("every killstreak is found by its script name, and nothing else is")
{
    for (const auto& killstreak : Logic::KILLSTREAKS)
    {
        CHECK(Logic::FindKillstreak(killstreak.type) == &killstreak);
        CHECK(!killstreak.name.empty());
    }
    CHECK(Logic::FindKillstreak("ak47_mp") == nullptr);
    CHECK(Logic::FindKillstreak("") == nullptr);
}

TEST("the guns the owner named are among the killstreaks, under the game's names")
{
    CHECK((Logic::FindKillstreak("minigun_mp")->name == "Death Machine"));
    CHECK((Logic::FindKillstreak("m220_tow_mp")->name == "Valkyrie Rockets"));
}

TEST("only guns that fire bullets count: a launcher, a grenade and a mine don't")
{
    constexpr std::int32_t GRENADE = 1;
    constexpr std::int32_t PROJECTILE = 2;
    constexpr std::int32_t MINE = 6;
    CHECK(Logic::FiresBullets(Logic::WEAPON_TYPE_BULLET));
    CHECK(!Logic::FiresBullets(GRENADE));
    CHECK(!Logic::FiresBullets(PROJECTILE));
    CHECK(!Logic::FiresBullets(MINE));
}

TEST("a hit is a bullet of the gun's own kind, never an explosive one")
{
    CHECK(Logic::GetBulletKind(true) == Logic::MOD_RIFLE_BULLET);
    CHECK(Logic::GetBulletKind(false) == Logic::MOD_PISTOL_BULLET);
}

TEST("a bullet's damage falls along a line from its near range to its far range")
{
    const Logic::BulletDamage bullet{50, 30, 1000.0f, 2000.0f};
    CHECK(Logic::GetDamage(bullet, 0.0f) == 50);
    CHECK(Logic::GetDamage(bullet, 1000.0f) == 50);
    CHECK(Logic::GetDamage(bullet, 1500.0f) == 40);
    CHECK(Logic::GetDamage(bullet, 2000.0f) == 30);
    CHECK(Logic::GetDamage(bullet, 9000.0f) == 30);
}

TEST("a gun whose damage doesn't fall does the same at any distance")
{
    CHECK(Logic::GetDamage({70, 70, 500.0f, 1500.0f}, 1000.0f) == 70);
    CHECK(Logic::GetDamage({70, 30, 500.0f, 500.0f}, 1000.0f) == 70);
}

TEST("the other team is the enemy, and without teams everyone is")
{
    CHECK(Logic::IsEnemy(AXIS, ALLIES, NO_TEAM));
    CHECK(!Logic::IsEnemy(AXIS, AXIS, NO_TEAM));
    CHECK(Logic::IsEnemy(NO_TEAM, NO_TEAM, NO_TEAM));
}

TEST("a hit is measured from the line of a body, so feet and head count as its chest does")
{
    const glm::vec3 origin(100.0f, 0.0f, 0.0f);
    CHECK(Logic::GetDistanceToBody({130.0f, 0.0f, 40.0f}, origin) == 30.0f);
    CHECK(Logic::GetDistanceToBody({130.0f, 0.0f, 0.0f}, origin) == 30.0f);
    CHECK(Logic::GetDistanceToBody({130.0f, 0.0f, 60.0f}, origin) == 30.0f);
    CHECK(Logic::GetDistanceToBody({100.0f, 0.0f, 90.0f}, origin) == 30.0f);
    CHECK(Logic::GetDistanceToBody({100.0f, 0.0f, -30.0f}, origin) == 30.0f);
}

TEST("the bodies within the radius come back nearest first")
{
    const std::array<glm::vec3, 4> origins{glm::vec3(90.0f, 0.0f, 0.0f), glm::vec3(30.0f, 0.0f, 0.0f),
                                           glm::vec3(300.0f, 0.0f, 0.0f), glm::vec3(0.0f, 60.0f, 0.0f)};
    const auto within = Logic::FindWithin({0.0f, 0.0f, 40.0f}, 100.0f, origins);
    CHECK(within.size() == 3);
    CHECK(within[0] == 1);
    CHECK(within[1] == 3);
    CHECK(within[2] == 0);
}

TEST("a radius of nothing near finds no body")
{
    const std::array<glm::vec3, 1> origins{glm::vec3(300.0f, 0.0f, 0.0f)};
    CHECK(Logic::FindWithin({0.0f, 0.0f, 40.0f}, 100.0f, origins).empty());
    CHECK(Logic::FindWithin({0.0f, 0.0f, 40.0f}, 100.0f, {}).empty());
}

TEST("a body goes when its timer is up, and never without one")
{
    CHECK(!Logic::IsBodyDue(milliseconds(2999), 3));
    CHECK(Logic::IsBodyDue(seconds(3), 3));
    CHECK(Logic::IsBodyDue(seconds(90), 3));
    CHECK(!Logic::IsBodyDue(seconds(90), std::nullopt));
}

TEST("the body timer is its slider's seconds when ticked, and none when not")
{
    CHECK((Logic::GetBodyTimer(true, 5) == 5));
    CHECK((Logic::GetBodyTimer(true, 0) == Logic::BODY_SECONDS_LEAST));
    CHECK((Logic::GetBodyTimer(true, 90) == Logic::BODY_SECONDS_MOST));
    CHECK(!Logic::GetBodyTimer(false, 5));
    CHECK((Logic::BODY_SECONDS_LEAST == 1 && Logic::BODY_SECONDS_MOST == 30 && Logic::BODY_SECONDS_DEFAULT == 5));
}
