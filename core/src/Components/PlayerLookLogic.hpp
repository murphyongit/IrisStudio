#pragma once
#include "Types/Character.hpp"
#include "Types/Loadout.hpp"

#include "nlohmann/json.hpp"

namespace IWXMVM::Components::PlayerLookLogic
{
    // How a player of a demo looks, by character: what is chosen for it and for all players,
    // the models the game then builds it of, and the face paints. No game state; tested in tests/actors-tests.

    // The face paints of multiplayer: the patterns of mp/weaponoptions.csv by the number the game draws them by, 0 for
    // none, with the names its menus give them
    inline constexpr std::array<std::string_view, 25> FACE_PAINTS = {
        "None",    "Stalker", "Banshee", "Highlander", "Sidewinder", "Mantis",      "Militia", "Apache", "Sandman",
        "Zulu",    "Blitz",   "Commando", "Tundra",    "Animal",     "Dutch",       "Ranger",  "Smoke",  "Black Widow",
        "Death",   "Reaper",  "Jester",  "Dragon",     "Lion",       "Demon",       "Spider",
    };
    // the name of a paint, empty for a number the game has none of
    std::string_view GetFacePaintName(std::int32_t number);

    // What a player gets: nullopt keeps the demo's; an empty head takes the demo's off (a full body has its face)
    struct ResolvedCharacter
    {
        std::optional<std::string> body;
        std::optional<std::string> head;
        std::optional<std::vector<std::string>> worn;
        std::optional<std::int32_t> facePaint;
        std::optional<std::string> stowed;  // the gun on the back, a base weapon; empty takes the demo's off

        bool HasModels() const
        {
            return body || head || worn || stowed;
        }
        bool operator==(const ResolvedCharacter&) const = default;
    };
    // The choices for a player over those for its team (null for none) over those for all players
    ResolvedCharacter Resolve(const Types::Loadout* player, const Types::Loadout* team, const Types::Loadout& all);

    // The numbers the game gives the two teams, and a team's name by the map's multiplayer bodies: the allies are the
    // Americans or the rebels, the axis the others
    inline constexpr std::int32_t TEAM_AXIS = 1;
    inline constexpr std::int32_t TEAM_ALLIES = 2;
    std::string GetTeamName(std::int32_t team, const std::vector<std::string>& bodies);

    // the choices as a scene keeps them
    nlohmann::json ChoicesToJson(const Types::PlayerChoices& choices);
    Types::PlayerChoices ChoicesFromJson(const nlohmann::json& node);

    // A character's models as a loadout has them: the body, the head (empty for none) and what is worn
    void SetCharacter(Types::Loadout& loadout, const Types::Character& character);
    // The same for models as an actor has them, the body first: the first that isHead says is the head, the rest worn
    void SetModels(Types::Loadout& loadout, const std::vector<std::string>& models, std::string_view character,
                   const std::function<bool(const std::string&)>& isHead);
    // the models a loadout chooses, the body first
    std::vector<std::string> GetModels(const Types::Loadout& loadout);

    // A model on a player: what the game lists after the body, the head first and the stowed weapon on its tag
    struct Attached
    {
        std::string name;
        std::string tag;  // empty on the body's bones
        bool operator==(const Attached&) const = default;
    };
    using Models = std::vector<Attached>;  // the body first

    // the bone the game puts the stowed weapon on
    inline constexpr std::string_view STOWED_TAG = "tag_stowed_back";

    // The models the game holds for a player, from what the demo gives it (original) and the choices: the choices, the
    // demo's for what isn't chosen, and the weapon it has stowed, or stowedModel's for a stowed gun chosen (the model of
    // chosen.stowed, empty for none). At most mostModels: the game lists six after the body.
    Models GetWanted(const Models& original, const ResolvedCharacter& chosen, std::size_t mostModels,
                     const std::optional<std::string>& stowedModel = std::nullopt);

    // Which of the models fit in a DObj given the bones of each and those of what the game adds (the gun in its hands,
    // a campaign body's spine rig): the body, its head and those come first, then what is worn, then the stowed weapon
    Models Fit(const Models& models, std::span<const std::uint32_t> bones, std::uint32_t addedBones,
               std::uint32_t maxBones);
}  // namespace IWXMVM::Components::PlayerLookLogic
