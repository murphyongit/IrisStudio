#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

namespace IWXMVM::Components::LiveGameLogic
{
    // What the player of a live game this process hosts can ask of it: any killstreak, a shot
    // that hits the enemy nearest to where it lands, and what becomes of the bodies. No game state; tested in
    // tests/actors-tests.

    // The killstreaks a player can be given: what _hardpoints.gsc registers each as, and what the game calls it. The ones
    // only a Care Package's crate holds and the game's unused Artillery aren't listed.
    struct Killstreak
    {
        std::string_view type;
        std::string_view name;
    };
    inline constexpr std::array<Killstreak, 17> KILLSTREAKS = {{
        {"radar_mp", "Spy Plane"},
        {"rcbomb_mp", "RC-XD"},
        {"counteruav_mp", "Counter-Spy Plane"},
        {"auto_tow_mp", "SAM Turret"},
        {"supply_drop_mp", "Care Package"},
        {"napalm_mp", "Napalm Strike"},
        {"autoturret_mp", "Sentry Gun"},
        {"mortar_mp", "Mortar Team"},
        {"helicopter_comlink_mp", "Attack Helicopter"},
        {"m220_tow_mp", "Valkyrie Rockets"},
        {"radardirection_mp", "Blackbird"},
        {"airstrike_mp", "Rolling Thunder"},
        {"helicopter_gunner_mp", "Chopper Gunner"},
        {"dogs_mp", "Attack Dogs"},
        {"helicopter_player_firstperson_mp", "Gunship"},
        {"m202_flash_mp", "Grim Reaper"},
        {"minigun_mp", "Death Machine"},
    }};
    const Killstreak* FindKillstreak(std::string_view type);

    // The Explosive Bullet's radius, in the game's units, the first as mp_editingmod's
    inline constexpr float RADIUS_DEFAULT = 100.0f;
    inline constexpr float RADIUS_LEAST = 10.0f;
    inline constexpr float RADIUS_MOST = 500.0f;

    // A gun fires bullets when its weapon file says so: not a launcher, a grenade or a mine
    inline constexpr std::int32_t WEAPON_TYPE_BULLET = 0;
    bool FiresBullets(std::int32_t weaponType);

    // The game's means of death of a gun's bullets, as Bullet_Fire chooses it: what a player's death looks like and what
    // the scripts count goes by this, so the hit stays a bullet's
    inline constexpr std::int32_t MOD_PISTOL_BULLET = 1;
    inline constexpr std::int32_t MOD_RIFLE_BULLET = 2;
    std::int32_t GetBulletKind(bool rifleBullet);

    // The hit location the game's table has for a chest, which is what a hit gets: its multiplier is the weapon file's and
    // a death is chosen as from any shot there
    inline constexpr std::int32_t HIT_TORSO_UPPER = 4;

    // What a bullet of a gun does, from the weapon file: all of it up to the near range, the least from the far range on,
    // and along a line between the two, whole numbers as the game's are
    struct BulletDamage
    {
        std::int32_t most = 0;
        std::int32_t least = 0;
        float nearRange = 0.0f;
        float farRange = 0.0f;
    };
    std::int32_t GetDamage(const BulletDamage& bullet, float distance);

    // Whether the other side's players can be hit: in a game without teams, everyone is an enemy
    bool IsEnemy(std::int32_t team, std::int32_t otherTeam, std::int32_t noTeam);

    // How far a point is from a standing player at an origin: from the line of its body, feet to head, so a hit on the
    // ground at its feet and one over its head count the same as a hit beside its chest
    inline constexpr float BODY_HEIGHT = 60.0f;
    inline constexpr float CHEST_HEIGHT = 40.0f;
    float GetDistanceToBody(const glm::vec3& point, const glm::vec3& origin);

    // The bodies within a radius of a point, by their origin, the nearest first
    std::vector<std::size_t> FindWithin(const glm::vec3& point, float radius, std::span<const glm::vec3> origins);

    // A body goes once it has been there for the timer's seconds; none is kept for ever
    bool IsBodyDue(std::chrono::milliseconds age, std::optional<std::int32_t> timerSeconds);
    // The timer's seconds, as its slider has them, and the timer they make when it's on
    inline constexpr std::int32_t BODY_SECONDS_LEAST = 1;
    inline constexpr std::int32_t BODY_SECONDS_MOST = 30;
    inline constexpr std::int32_t BODY_SECONDS_DEFAULT = 5;
    std::optional<std::int32_t> GetBodyTimer(bool on, std::int32_t seconds);
}  // namespace IWXMVM::Components::LiveGameLogic
