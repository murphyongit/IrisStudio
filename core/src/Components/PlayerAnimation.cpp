#include "StdInclude.hpp"
#include "PlayerAnimation.hpp"

#include "Configuration/Configuration.hpp"
#include "Configuration/PreferencesConfiguration.hpp"
#include "Components/Loadouts.hpp"
#include "Events.hpp"

namespace IWXMVM::Components::PlayerAnimation
{
    bool hideAllCorpses = false;
    bool attachWeaponToCorpse = false;
    std::string_view latestAnimName;
    std::vector<std::pair<std::string_view, std::uint32_t>> cachedAnims;

    // The game applies the choices on its own thread while the UI changes them on the render thread
    std::mutex mutex;
    std::string animationForAll;
    std::map<std::int32_t, std::string> playerAnimations;
    std::map<std::int32_t, std::string> teamAnimations;
    std::atomic<std::uint32_t> choicesGeneration = 0;

    // what is chosen for a client: its own, its team's, else that for all players
    const std::string& GetChosen(std::int32_t client, std::int32_t team)
    {
        if (const auto it = playerAnimations.find(client); it != playerAnimations.end())
            return it->second;
        const auto it = teamAnimations.find(team);
        return it != teamAnimations.end() ? it->second : animationForAll;
    }

    void Initialize()
    {
        // the players of another demo are other players
        Events::RegisterListener(EventType::PostDemoLoad, [] {
            std::lock_guard lock(mutex);
            playerAnimations.clear();
            teamAnimations.clear();
        });
    }

    void PopulateAnimationData(std::vector<std::pair<std::string_view, std::uint32_t>>& anims)
    {
        cachedAnims = std::move(anims);
    }

    const std::vector<std::pair<std::string_view, std::uint32_t>>& GetAnimations()
    {
        return cachedAnims;
    }

    std::string_view GetLatestAnimationName()
    {
        return latestAnimName;
    }

    std::string GetAnimationForAll()
    {
        std::lock_guard lock(mutex);
        return animationForAll;
    }

    void SetAnimationForAll(std::string_view name)
    {
        std::lock_guard lock(mutex);
        animationForAll = name;
        ++choicesGeneration;
    }

    std::string GetCatalogAnimation(std::int32_t client)
    {
        const auto team = Loadouts::GetTeam(client);
        std::lock_guard lock(mutex);
        const auto& chosen = GetChosen(client, team);
        if (chosen.empty() ||
            std::ranges::find(cachedAnims, chosen, &std::pair<std::string_view, std::uint32_t>::first) !=
                cachedAnims.end())
            return {};
        return chosen;
    }

    std::optional<std::string> GetAnimationForPlayer(std::int32_t client)
    {
        std::lock_guard lock(mutex);
        const auto it = playerAnimations.find(client);
        return it != playerAnimations.end() ? std::optional(it->second) : std::nullopt;
    }

    void SetAnimationForPlayer(std::int32_t client, std::optional<std::string_view> name)
    {
        std::lock_guard lock(mutex);
        if (name)
            playerAnimations[client] = *name;
        else
            playerAnimations.erase(client);
        ++choicesGeneration;
    }

    std::optional<std::string> GetAnimationForTeam(std::int32_t team)
    {
        std::lock_guard lock(mutex);
        const auto it = teamAnimations.find(team);
        return it != teamAnimations.end() ? std::optional(it->second) : std::nullopt;
    }

    void SetAnimationForTeam(std::int32_t team, std::optional<std::string_view> name)
    {
        std::lock_guard lock(mutex);
        if (name)
            teamAnimations[team] = *name;
        else
            teamAnimations.erase(team);
        ++choicesGeneration;
    }

    void GetChoices(Types::PlayerChoices& choices)
    {
        std::lock_guard lock(mutex);
        choices.deathForAll = animationForAll;
        choices.teamDeaths = teamAnimations;
        choices.playerDeaths = playerAnimations;
    }

    void SetChoices(const Types::PlayerChoices& choices)
    {
        std::lock_guard lock(mutex);
        animationForAll = choices.deathForAll;
        teamAnimations = choices.teamDeaths;
        playerAnimations = choices.playerDeaths;
    }

    std::uint32_t GetChoicesGeneration()
    {
        return choicesGeneration;
    }

    std::map<std::int32_t, std::string> GetPlayerAnimations()
    {
        std::lock_guard lock(mutex);
        return playerAnimations;
    }

    void ResetAnimations()
    {
        std::lock_guard lock(mutex);
        animationForAll.clear();
        playerAnimations.clear();
        teamAnimations.clear();
        ++choicesGeneration;
    }

    bool IsFavorite(std::string_view name)
    {
        const auto& favorites = PreferencesConfiguration::Get().favoriteDeathAnimations;
        return std::ranges::find(favorites, name) != favorites.end();
    }

    void SetFavorite(std::string_view name, bool favorite)
    {
        auto& favorites = PreferencesConfiguration::Get().favoriteDeathAnimations;
        const auto it = std::ranges::find(favorites, name);
        if (favorite == (it != favorites.end()))
            return;

        if (favorite)
            favorites.emplace_back(name);
        else
            favorites.erase(it);
        Configuration::Get().Write(true);
    }

    bool& HideAllCorpses()
    {
        return hideAllCorpses;
    }

    bool& AttachWeaponToCorpse()
    {
        return attachWeaponToCorpse;
    }

    void ApplyAnimation(std::int32_t client, std::string_view animName, std::uint32_t& animIndex)
    {
        std::string chosen;
        {
            const auto team = Loadouts::GetTeam(client);
            std::lock_guard lock(mutex);
            chosen = GetChosen(client, team);
        }

        latestAnimName = animName;
        if (chosen.empty())
            return;

        const auto anim = std::ranges::find(cachedAnims, chosen, &std::pair<std::string_view, std::uint32_t>::first);
        if (anim != cachedAnims.end() && (animIndex & 511) != anim->second)
            animIndex = anim->second;
    }
}  // namespace IWXMVM::Components::PlayerAnimation
