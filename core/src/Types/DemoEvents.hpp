#pragma once

namespace IWXMVM::Types
{
    // How a kill was made, as far as the game tells
    enum class KillMethod : std::uint8_t
    {
        Weapon,
        Headshot,
        Melee,
        Impact,  // hit directly by a projectile, e.g. a grenade launcher round that didn't explode
        Falling,
        Crush,
        Suicide,
    };

    struct DemoKill
    {
        std::uint32_t tick;          // on the demo's timeline
        std::int32_t attacker;       // client number, -1 for the world
        std::int32_t victim;         // client number
        std::int32_t povClient;      // the client the demo followed at the time
        KillMethod method;
        std::string weapon;          // what the attacker held; empty when unknown
        float distance;              // between attacker and victim in game units, 0 when unknown
        bool attackerInAir;
        bool teamKill;
    };

    // Other moments worth jumping to
    enum class DemoMomentType : std::uint8_t
    {
        BombPlanted,
    };

    struct DemoMoment
    {
        std::uint32_t tick;
        DemoMomentType type;
    };

    struct DemoPlayer
    {
        std::string name;
        std::int32_t team;           // 0 when the game has no teams
    };

    // What happens in a demo, read from the whole file when it is loaded
    struct DemoEvents
    {
        std::vector<DemoKill> kills;  // in order
        std::vector<DemoMoment> moments;
        std::map<std::int32_t, DemoPlayer> players;  // by client number, as last seen in the demo
        std::int32_t recorder = -1;   // the client whose view the demo mostly shows

        std::string GetPlayerName(std::int32_t client) const
        {
            if (client < 0)
                return "World";
            const auto it = players.find(client);
            return it != players.end() && !it->second.name.empty() ? it->second.name
                                                                   : std::format("Player {}", client);
        }
    };
}  // namespace IWXMVM::Types
