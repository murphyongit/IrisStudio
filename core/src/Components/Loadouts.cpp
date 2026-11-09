#include "StdInclude.hpp"
#include "Loadouts.hpp"

#include "Events.hpp"
#include "Mod.hpp"
#include "Resources.hpp"
#include "WeaponCatalog.hpp"

// core/resources/campaign-weapons.json (Resources.cpp)
namespace IWXMVM::Resources
{
    INCBIN_EXTERN(CAMPAIGN_WEAPONS);
}

namespace IWXMVM::Components::Loadouts
{
    // The game applies the choices on its own thread while the UI changes them on the render thread
    std::mutex mutex;
    std::shared_ptr<const Types::LoadoutCatalog> catalog;
    Types::Loadout loadoutForAll;
    std::map<std::int32_t, Types::Loadout> playerLoadouts;
    std::map<std::int32_t, Types::Loadout> teamLoadouts;
    std::optional<FacePaintPreview> facePaintPreview;
    std::map<std::int32_t, LeftOut> leftOuts;
    std::atomic<std::uint32_t> generation = 0;
    std::atomic<std::uint32_t> choicesGeneration = 0;

    void Initialize()
    {
        // the players of another demo are other players
        Events::RegisterListener(EventType::PostDemoLoad, [] {
            std::lock_guard lock(mutex);
            playerLoadouts.clear();
            teamLoadouts.clear();
            leftOuts.clear();
            ++generation;
        });
    }

    void SetCatalog(std::shared_ptr<const Types::LoadoutCatalog> newCatalog)
    {
        std::lock_guard lock(mutex);
        catalog = std::move(newCatalog);
        ++generation;
    }

    std::shared_ptr<const Types::LoadoutCatalog> GetCatalog()
    {
        std::lock_guard lock(mutex);
        return catalog;
    }

    Types::Loadout GetLoadoutForAll()
    {
        std::lock_guard lock(mutex);
        return loadoutForAll;
    }

    void SetLoadoutForAll(Types::Loadout loadout)
    {
        std::lock_guard lock(mutex);
        if (loadoutForAll == loadout)
            return;
        loadoutForAll = std::move(loadout);
        ++generation;
        ++choicesGeneration;
    }

    std::optional<Types::Loadout> GetLoadoutForPlayer(std::int32_t client)
    {
        std::lock_guard lock(mutex);
        const auto it = playerLoadouts.find(client);
        return it != playerLoadouts.end() ? std::optional(it->second) : std::nullopt;
    }

    void SetLoadoutForPlayer(std::int32_t client, std::optional<Types::Loadout> loadout)
    {
        std::lock_guard lock(mutex);
        if (loadout)
            playerLoadouts[client] = std::move(*loadout);
        else
            playerLoadouts.erase(client);
        ++generation;
        ++choicesGeneration;
    }

    std::map<std::int32_t, Types::Loadout> GetPlayerLoadouts()
    {
        std::lock_guard lock(mutex);
        return playerLoadouts;
    }

    std::optional<Types::Loadout> GetLoadoutForTeam(std::int32_t team)
    {
        std::lock_guard lock(mutex);
        const auto it = teamLoadouts.find(team);
        return it != teamLoadouts.end() ? std::optional(it->second) : std::nullopt;
    }

    void SetLoadoutForTeam(std::int32_t team, std::optional<Types::Loadout> loadout)
    {
        std::lock_guard lock(mutex);
        if (loadout)
            teamLoadouts[team] = std::move(*loadout);
        else
            teamLoadouts.erase(team);
        ++generation;
        ++choicesGeneration;
    }

    Types::Loadout GetLoadoutFor(const Target& target)
    {
        if (target.client)
            return GetLoadoutForPlayer(*target.client).value_or(Types::Loadout{});
        return target.team ? GetLoadoutForTeam(*target.team).value_or(Types::Loadout{}) : GetLoadoutForAll();
    }

    void SetLoadoutFor(const Target& target, const Types::Loadout& loadout)
    {
        const auto own = loadout.IsEmpty() ? std::nullopt : std::optional(loadout);
        if (target.client)
            SetLoadoutForPlayer(*target.client, own);
        else if (target.team)
            SetLoadoutForTeam(*target.team, own);
        else
            SetLoadoutForAll(loadout);
    }

    std::int32_t GetTeam(std::int32_t client)
    {
        const auto events = Mod::GetGameInterface()->GetDemoEvents();
        const auto player = events ? events->players.find(client) : decltype(events->players.end()){};
        return events && player != events->players.end() ? player->second.team : 0;
    }

    void GetChoices(Types::PlayerChoices& choices)
    {
        std::lock_guard lock(mutex);
        choices.loadoutForAll = loadoutForAll;
        choices.teamLoadouts = teamLoadouts;
        choices.playerLoadouts = playerLoadouts;
    }

    void SetChoices(const Types::PlayerChoices& choices)
    {
        std::lock_guard lock(mutex);
        loadoutForAll = choices.loadoutForAll;
        teamLoadouts = choices.teamLoadouts;
        playerLoadouts = choices.playerLoadouts;
        ++generation;
    }

    std::uint32_t GetChoicesGeneration()
    {
        return choicesGeneration;
    }

    void SetFacePaintPreview(std::optional<FacePaintPreview> preview)
    {
        std::lock_guard lock(mutex);
        if (facePaintPreview == preview)
            return;
        facePaintPreview = std::move(preview);
        ++generation;
    }

    void SetLeftOut(std::int32_t client, LeftOut leftOut)
    {
        std::lock_guard lock(mutex);
        if (leftOut.stowed || leftOut.worn)
            leftOuts[client] = leftOut;
        else
            leftOuts.erase(client);
    }

    std::map<std::int32_t, LeftOut> GetLeftOut()
    {
        std::lock_guard lock(mutex);
        return leftOuts;
    }

    void ResetLoadouts()
    {
        std::lock_guard lock(mutex);
        loadoutForAll = {};
        playerLoadouts.clear();
        teamLoadouts.clear();
        ++generation;
        ++choicesGeneration;
    }

    std::uint32_t GetGeneration()
    {
        return generation;
    }

    const Types::LoadoutCatalog::Variant* FindClosestVariant(const Types::LoadoutCatalog& catalog,
                                                             std::string_view weapon,
                                                             std::vector<std::string> attachments)
    {
        // drops the attachments the weapon can't have, then others from the back until a variant exists
        std::erase_if(attachments, [&](const std::string& attachment) {
            return std::ranges::none_of(catalog.variants, [&](const Types::LoadoutCatalog::Variant& variant) {
                return variant.weapon == weapon && std::ranges::find(variant.attachments, attachment) !=
                                                       variant.attachments.end();
            });
        });
        std::ranges::sort(attachments);

        while (true)
        {
            if (const auto variant = catalog.FindVariant(weapon, attachments))
                return variant;
            if (attachments.empty())
                return nullptr;
            attachments.pop_back();
        }
    }

    // The rule for a weapon: the player's before those for all players, and a rule for the weapon itself before one
    // for its slot (primary or secondary), before one for any weapon
    const Types::WeaponRule* FindRule(const Types::Loadout* player, const Types::Loadout* team,
                                      const Types::Loadout& all, const Types::LoadoutCatalog::Weapon& weapon)
    {
        const auto slot = weapon.primary ? Types::WeaponRule::ANY_PRIMARY : Types::WeaponRule::ANY_SECONDARY;
        for (const auto loadout : {player, team, &all})
        {
            if (!loadout)
                continue;
            for (const std::string_view from : {std::string_view(weapon.name), slot, std::string_view{}})
            {
                const auto it = std::ranges::find(loadout->weaponRules, from, &Types::WeaponRule::from);
                if (it != loadout->weaponRules.end())
                    return &*it;
            }
        }
        return nullptr;
    }

    std::optional<ResolvedWeapon> ResolveWeapon(std::int32_t client, std::string_view variantName, std::int32_t camo)
    {
        const auto teamNumber = GetTeam(client);
        std::lock_guard lock(mutex);
        if (!catalog)
            return std::nullopt;

        const auto original = catalog->FindVariant(variantName);
        const auto originalWeapon = original ? catalog->FindWeapon(original->weapon) : nullptr;
        if (!originalWeapon)
            return std::nullopt;

        const auto player = playerLoadouts.find(client);
        const auto team = teamLoadouts.find(teamNumber);
        const auto rule = FindRule(player != playerLoadouts.end() ? &player->second : nullptr,
                                   team != teamLoadouts.end() ? &team->second : nullptr, loadoutForAll, *originalWeapon);
        if (!rule)
            return std::nullopt;

        const auto& weapon = rule->weapon.value_or(original->weapon);
        const auto variant =
            FindClosestVariant(*catalog, weapon, rule->attachments.value_or(original->attachments));
        if (!variant)
            return std::nullopt;

        return ResolvedWeapon{variant->name, rule->camo.value_or(camo)};
    }

    PlayerLookLogic::ResolvedCharacter ResolveCharacter(std::int32_t client)
    {
        const auto teamNumber = GetTeam(client);
        std::lock_guard lock(mutex);
        const auto player = playerLoadouts.find(client);
        const auto team = teamLoadouts.find(teamNumber);
        auto resolved = PlayerLookLogic::Resolve(player != playerLoadouts.end() ? &player->second : nullptr,
                                                 team != teamLoadouts.end() ? &team->second : nullptr, loadoutForAll);
        if (facePaintPreview && (!facePaintPreview->client || facePaintPreview->client == client) &&
            (!facePaintPreview->team || facePaintPreview->team == teamNumber))
            resolved.facePaint = facePaintPreview->paint;
        return resolved;
    }

    bool HasFacePaint()
    {
        std::lock_guard lock(mutex);
        const auto painted = [](const Types::Loadout& loadout) { return loadout.facePaint.value_or(0) > 0; };
        const auto anyPainted = [&](const auto& loadouts) {
            return std::ranges::any_of(loadouts, [&](const auto& entry) { return painted(entry.second); });
        };
        return facePaintPreview || painted(loadoutForAll) || anyPainted(teamLoadouts) || anyPainted(playerLoadouts);
    }

    std::vector<ChosenModels> GetChosenModels()
    {
        std::lock_guard lock(mutex);
        std::vector<ChosenModels> chosen;
        const auto add = [&](const Types::Loadout& loadout) {
            if (auto models = PlayerLookLogic::GetModels(loadout); !models.empty())
                chosen.push_back({std::move(models), loadout.character.value_or("")});
        };
        add(loadoutForAll);
        for (const auto& [team, loadout] : teamLoadouts)
            add(loadout);
        for (const auto& [client, loadout] : playerLoadouts)
            add(loadout);
        return chosen;
    }
}  // namespace IWXMVM::Components::Loadouts

namespace IWXMVM::Components::WeaponCatalog
{
    const Catalog& Get()
    {
        static const Catalog catalog = [] {
            try
            {
                const std::string_view text(reinterpret_cast<const char*>(Resources::CAMPAIGN_WEAPONS_data),
                                            Resources::CAMPAIGN_WEAPONS_size);
                return Parse(nlohmann::json::parse(text));
            }
            catch (const std::exception& e)
            {
                LOG_ERROR("The weapon catalog doesn't read: {}", e.what());
                return Catalog{};
            }
        }();
        return catalog;
    }
}  // namespace IWXMVM::Components::WeaponCatalog
