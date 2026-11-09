#pragma once
#include "Types/Loadout.hpp"

namespace IWXMVM::Components::PlayerAnimation
{
    // The death animations a game offers (name, the game's animation number), and which one each corpse plays. An
    // animation is chosen for all players and, overriding that, for single players (by client number). Choices are
    // kept by name, so they carry over to another demo or game with the same animations. An empty name keeps the
    // animation the game gave.

    void Initialize();

    // Called by the game for a corpse playing a death animation; replaces animIndex with the chosen animation
    void ApplyAnimation(std::int32_t client, std::string_view animName, std::uint32_t& animIndex);

    void PopulateAnimationData(std::vector<std::pair<std::string_view, std::uint32_t>>& anims);
    const std::vector<std::pair<std::string_view, std::uint32_t>>& GetAnimations();
    std::string_view GetLatestAnimationName();

    std::string GetAnimationForAll();
    void SetAnimationForAll(std::string_view name);

    // The animation chosen for a player when it isn't one of the deaths the game lists (any animation of the catalog,
    // which the game plays from a tree of the corpse's own); empty otherwise
    std::string GetCatalogAnimation(std::int32_t client);

    // nullopt while the player follows the animation for all players
    std::optional<std::string> GetAnimationForPlayer(std::int32_t client);
    void SetAnimationForPlayer(std::int32_t client, std::optional<std::string_view> name);
    std::map<std::int32_t, std::string> GetPlayerAnimations();
    // the same for a team, between those two
    std::optional<std::string> GetAnimationForTeam(std::int32_t team);
    void SetAnimationForTeam(std::int32_t team, std::optional<std::string_view> name);

    // what the scene keeps of these, and what it brings back (all of them replaced)
    void GetChoices(Types::PlayerChoices& choices);
    void SetChoices(const Types::PlayerChoices& choices);
    // changes whenever what the scene keeps does
    std::uint32_t GetChoicesGeneration();

    // back to the animations the game gave, for everyone
    void ResetAnimations();

    bool IsFavorite(std::string_view name);
    void SetFavorite(std::string_view name, bool favorite);

    bool& HideAllCorpses();
    bool& AttachWeaponToCorpse();
}  // namespace IWXMVM::Components::PlayerAnimation
