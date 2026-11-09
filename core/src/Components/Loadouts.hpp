#pragma once
#include "Components/PlayerLookLogic.hpp"
#include "Types/Loadout.hpp"

namespace IWXMVM::Components::Loadouts
{
    // How players look in a demo: their character model and, per weapon they hold, the weapon, its attachments and
    // camo. A loadout is chosen for all players and, over it, for single players (by client number). Choices are kept
    // by name, so they carry over to another demo or game with the same weapons; the choices for single players are
    // cleared when another demo loads.

    void Initialize();

    // The game sets what it offers whenever it changes (another map), on its own thread
    void SetCatalog(std::shared_ptr<const Types::LoadoutCatalog> catalog);
    std::shared_ptr<const Types::LoadoutCatalog> GetCatalog();

    Types::Loadout GetLoadoutForAll();
    void SetLoadoutForAll(Types::Loadout loadout);

    // nullopt while the player only has the loadout for all players
    std::optional<Types::Loadout> GetLoadoutForPlayer(std::int32_t client);
    void SetLoadoutForPlayer(std::int32_t client, std::optional<Types::Loadout> loadout);
    std::map<std::int32_t, Types::Loadout> GetPlayerLoadouts();
    // the same for a team (see PlayerLookLogic::TEAM_AXIS), between those two
    std::optional<Types::Loadout> GetLoadoutForTeam(std::int32_t team);
    void SetLoadoutForTeam(std::int32_t team, std::optional<Types::Loadout> loadout);
    // Who a choice on the Players page is for: a player, a team, or all players (neither)
    struct Target
    {
        std::optional<std::int32_t> client;
        std::optional<std::int32_t> team;
        bool operator==(const Target&) const = default;
    };
    // what is chosen for them, empty for a player or team with nothing of its own; an empty one takes that off
    Types::Loadout GetLoadoutFor(const Target& target);
    void SetLoadoutFor(const Target& target, const Types::Loadout& loadout);

    // the team a client is on as the demo's scan has it, 0 for none
    std::int32_t GetTeam(std::int32_t client);

    // what the scene keeps of these, and what it brings back (all of them replaced)
    void GetChoices(Types::PlayerChoices& choices);
    void SetChoices(const Types::PlayerChoices& choices);
    // changes whenever what the scene keeps does
    std::uint32_t GetChoicesGeneration();

    // A face paint shown on a player, a team or everyone (neither given) for as long as the mouse is on it, over the
    // choices; nullopt for none
    struct FacePaintPreview
    {
        std::optional<std::int32_t> client;
        std::optional<std::int32_t> team;
        std::int32_t paint = 0;
        bool operator==(const FacePaintPreview&) const = default;
    };
    void SetFacePaintPreview(std::optional<FacePaintPreview> preview);

    // What a player's models couldn't keep for the bones the game allows (see PlayerLookLogic::Fit), which the game sets
    // as it writes them; a campaign character has many more than a multiplayer one
    struct LeftOut
    {
        bool stowed = false;
        bool worn = false;
    };
    void SetLeftOut(std::int32_t client, LeftOut leftOut);
    std::map<std::int32_t, LeftOut> GetLeftOut();

    // back to what the demo shows, for everyone
    void ResetLoadouts();

    // changes whenever a choice does, so the game can keep what it worked out until then
    std::uint32_t GetGeneration();

    // What the game draws instead of a player's weapon variant and camo; nullopt keeps them
    struct ResolvedWeapon
    {
        std::string variant;
        std::int32_t camo;
    };
    std::optional<ResolvedWeapon> ResolveWeapon(std::int32_t client, std::string_view variant, std::int32_t camo);

    // How a player looks, its choices over those for all players
    PlayerLookLogic::ResolvedCharacter ResolveCharacter(std::int32_t client);

    // Every loadout's chosen models with the character they came from, for their zones
    struct ChosenModels
    {
        std::vector<std::string> models;
        std::string character;
    };
    std::vector<ChosenModels> GetChosenModels();
    // whether a loadout has a face paint on
    bool HasFacePaint();

    // The variant of a weapon with attachments as close to the given ones as the game has, or null
    const Types::LoadoutCatalog::Variant* FindClosestVariant(const Types::LoadoutCatalog& catalog,
                                                             std::string_view weapon,
                                                             std::vector<std::string> attachments);
}  // namespace IWXMVM::Components::Loadouts
