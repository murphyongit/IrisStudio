#pragma once
#include "Types/LiveActor.hpp"

namespace IWXMVM::Components::LiveActorLogic
{
    // The rules of live actors: how one joins a game, which side and input it gets, the
    // game's bot settings. No game state; tested in tests/actors-tests.

    // The game's team numbers, as a client's state has them
    inline constexpr std::int32_t TEAM_FREE = 0;
    inline constexpr std::int32_t TEAM_AXIS = 1;
    inline constexpr std::int32_t TEAM_ALLIES = 2;

    // The answers the game's menus give for a team and a class, which is all a test client needs to join and spawn:
    // the menu's name and the response, as _bot.gsc and _menus.gsc notify them
    inline constexpr std::string_view MENU_TEAM = "team_marinesopfor";
    inline constexpr std::string_view MENU_CLASS = "changeclass";
    inline constexpr std::string_view CLASS_RESPONSE = "smg_mp";  // what bot_spawn_think picks; bots get a loadout
    std::string_view GetTeamResponse(Types::LiveActorSide side, std::int32_t hostTeam);

    // How a live actor gets into the game. The game adds the client, then takes a moment before its menu thread
    // listens, so the steps come at the times _bot.gsc waits between them; a class that brought no spawn is asked
    // again.
    inline constexpr std::chrono::milliseconds TEAM_DELAY{300};
    inline constexpr std::chrono::milliseconds CLASS_DELAY{500};
    inline constexpr std::chrono::milliseconds CLASS_RETRY{3000};
    inline constexpr std::chrono::milliseconds SPAWN_SETTLE{150};  // the script's own spawn is over by then
    inline constexpr std::chrono::milliseconds BETWEEN_ADDS{250};  // as bot_spawner_think adds them
    enum class JoinStep
    {
        None,
        Team,
        Class,
    };
    struct Join
    {
        std::chrono::milliseconds sinceAdded{};
        std::optional<std::chrono::milliseconds> sinceTeam;   // since it was given a team, if it was
        std::optional<std::chrono::milliseconds> sinceClass;  // since it last chose a class, if it did
        bool spawned = false;                                 // it has been alive
    };
    JoinStep GetJoinStep(const Join& join);

    // What an actor sends the game for a frame. A bot's usercmd has these; the rest of it is left as it is.
    struct Input
    {
        std::int8_t forward = 0;
        std::int8_t right = 0;
        std::uint32_t buttons = 0;
        std::uint32_t buttons2 = 0;
    };
    // An actor that doesn't run the game's bot code this frame (Still, while alive): it holds its view and does nothing
    bool RunsBotCode(Types::LiveActorMode mode, bool alive);
    // What is left of what the bot code asked for
    Input FilterInput(Types::LiveActorMode mode, Input input);

    // The dvars of a skill, the values of _bot.gsc's bot_set_difficulty
    struct SkillDvar
    {
        std::string_view name;
        std::string_view value;
    };
    std::span<const SkillDvar> GetSkillDvars(Types::LiveActorSkill skill);
    std::string_view GetSkillName(Types::LiveActorSkill skill);

    // The most live actors a game takes: every client slot but the demo client's and the players'
    std::int32_t GetRoom(std::int32_t maxClients, std::int32_t clientsTaken);

    // What changing a live actor needs from the game: its gun given again; a new spawn at its place (it is moved); the
    // team menu again (it dies and comes back on the other team, as a player who changes team does)
    bool NeedsWeapon(const Types::LiveActor& before, const Types::LiveActor& now);
    bool NeedsPlacing(const Types::LiveActor& before, const Types::LiveActor& now);
    bool NeedsTeam(const Types::LiveActor& before, const Types::LiveActor& now);

    // A new bot: what the one given has chosen (the one selected, so ten alike take ten presses), but its place; the
    // defaults without one. Its name is the game's.
    Types::LiveActor MakeNew(const Types::LiveActor* like);
}  // namespace IWXMVM::Components::LiveActorLogic
