#pragma once
#include "Components/LiveGameLogic.hpp"

namespace IWXMVM::Components::LiveGame
{
    // What is chosen for the player of a live game this process hosts, and for its bodies, and
    // what is asked of the game once. All of it works only in a game this process hosts: the binding drops what is asked
    // anywhere else, and nothing here reaches a server somewhere else.

    struct Settings
    {
        bool canswap = false;          // a gun's first raise plays each time it comes out
        bool explosiveBullet = false;  // a shot also hits the enemy nearest to where it lands
        float radius = LiveGameLogic::RADIUS_DEFAULT;
        bool bulletKills = false;  // that hit kills, whatever the gun does
        bool fromAim = true;       // the radius is around where the view aims, not where the bullet landed
        bool bodyTimer = false;    // bodies go after bodySeconds
        std::int32_t bodySeconds = LiveGameLogic::BODY_SECONDS_DEFAULT;

        bool operator==(const Settings&) const = default;
    };
    Settings GetSettings();
    void SetSettings(const Settings& settings);

    // A gun for the player: the variant, its camo and whether it takes the place of the gun in hand
    struct GunRequest
    {
        std::string weapon;
        std::int32_t camo = 0;
        bool replace = true;

        bool operator==(const GunRequest&) const = default;
    };
    void RequestGun(GunRequest gun);
    std::vector<GunRequest> TakeGunRequests();

    // A killstreak of LiveGameLogic::KILLSTREAKS for the player
    void RequestKillstreak(std::string type);
    std::vector<std::string> TakeKillstreakRequests();

    // Every body goes now
    void RequestClearBodies();
    bool TakeClearBodies();

    // Why the last thing asked didn't happen, empty when all is well (what the panel shows)
    void SetProblem(std::string problem);
    std::string GetProblem();

    // what was asked and not done goes with the game
    void DropRequests();
}  // namespace IWXMVM::Components::LiveGame
