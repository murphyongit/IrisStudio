#pragma once
#include "Structures.hpp"

namespace IWXMVM::T5::LiveActors
{
    // The actors of a live game: each is a real test client of this game's own server,
    // which the game's bot code moves and the game hits, kills and records as any player. Core keeps what is chosen
    // (Components::LiveActors); here that is brought about, and their look is drawn by what draws demo players.

    // A game this process hosts: only then are there actors. Nothing here can reach a server somewhere else.
    bool IsLive();
    // whether a client is a live actor
    bool IsActor(std::int32_t client);

    // the health of the player of this game (main thread)
    std::int32_t GetHostHealth();
    // Answers one of the game's menus for the player of this game, as the harness joins a game (on the server's thread)
    void JoinHost(std::string_view menu, std::string_view response);

    // The server's side of a client, for what runs on the server's thread: its player entity and state, health and team
    using ScriptValue = std::variant<std::string, std::int32_t, std::array<float, 3>>;
    std::uint8_t* GetEntity(std::int32_t number);
    Structures::playerState_s* GetPlayerState(std::int32_t number);
    std::int32_t GetHealth(std::int32_t number);
    std::int32_t GetTeam(std::int32_t number);
    bool IsInGame(std::int32_t number);
    inline constexpr std::int32_t NO_TEAM = 0;
    // Calls what a script calls on a player, by the script machine's rules
    void CallMethod(std::int32_t number, std::string_view name, std::vector<ScriptValue> values = {});

    // What a live game's actors need of the main thread: a new map's reset and their guns' zones. Each frame. (The
    // server's own thread adds them and brings them up to what is chosen.)
    void RunFrame();

    void Install();
}  // namespace IWXMVM::T5::LiveActors
