#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace IWXMVM::Types
{
    // A bot of a live game: a real test client of the local server, with the gun chosen
    // for it, so the game moves it, hits it, kills it and records it as any player. It has the game's bot name and
    // look.

    // What it does with what the game's bot code wants: stands as placed; stands but aims and shoots; plays as a bot
    enum class LiveActorMode
    {
        Still,
        Guard,
        Roam,
    };

    enum class LiveActorSide
    {
        Enemy,
        Friend,
    };

    // The game's bot settings are one set for all bots (the sv_bot dvars), with the names of Combat Training
    enum class LiveActorSkill
    {
        Easy,
        Normal,
        Hard,
        Veteran,
    };

    struct LiveActor
    {
        std::uint32_t id = 0;
        LiveActorMode mode = LiveActorMode::Still;
        LiveActorSide side = LiveActorSide::Enemy;
        // the gun as the Weapon row has it, empty for Auto (what the game gives a bot), and the variant given
        std::string gun;
        std::vector<std::string> attachments;
        std::string weapon;
        std::int32_t camo = 0;  // the game's camo number
        // where it stands and faces, for as long as it isn't Roam; nullopt for where the game spawns it
        std::optional<std::array<float, 3>> origin;
        float yaw = 0.0f;

        bool operator==(const LiveActor&) const = default;
    };

    // What the game reports of a live actor
    struct LiveActorState
    {
        std::int32_t client = -1;  // its client and entity number, -1 until it has joined
        std::string name;
        bool alive = false;
        std::int32_t health = 0;
        std::array<float, 3> origin{};
        std::string weapon;   // what it holds
        std::string problem;  // why it isn't there or isn't as chosen, empty when all is well

        bool operator==(const LiveActorState&) const = default;
    };
}  // namespace IWXMVM::Types
