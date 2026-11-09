#include "StdInclude.hpp"
#include "WeaponCatalog.hpp"

#include <charconv>

namespace IWXMVM::Components::WeaponCatalog
{
    namespace
    {
        std::string StringOrEmpty(const nlohmann::json& node, const char* key)
        {
            const auto it = node.find(key);
            return it != node.end() && it->is_string() ? it->get<std::string>() : std::string();
        }

        std::vector<std::string> Strings(const nlohmann::json& node, const char* key)
        {
            std::vector<std::string> strings;
            if (const auto it = node.find(key); it != node.end() && it->is_array())
                for (const auto& entry : *it)
                    if (entry.is_string())
                        strings.push_back(entry.get<std::string>());
            return strings;
        }

        // the categories of the groups secondaries are listed in, as multiplayer's Pistols and Launchers
        bool IsSecondaryCategory(std::string_view category)
        {
            return category == "Pistols" || category == "Launchers";
        }
    }  // namespace

    const Variant* Catalog::FindVariant(std::string_view name) const
    {
        for (const auto& weapon : weapons)
            for (const auto& variant : weapon.variants)
                if (variant.name == name)
                    return &variant;
        return nullptr;
    }

    bool Catalog::Has(std::string_view name) const
    {
        return std::ranges::any_of(weapons, [&](const Weapon& weapon) {
            return std::ranges::any_of(
                weapon.variants, [&](const Variant& variant) { return variant.name == name || variant.left == name; });
        });
    }

    const Zone* Catalog::FindZone(std::string_view name) const
    {
        const auto it = std::ranges::find(zones, name, &Zone::name);
        return it != zones.end() ? &*it : nullptr;
    }

    Catalog Parse(const nlohmann::json& json)
    {
        if (!json.is_object() || json.value("version", 0) != 1 || !json.contains("weapons") ||
            !json["weapons"].is_array())
            throw std::runtime_error("campaign-weapons.json isn't of version 1");
        Catalog catalog;
        for (const auto& entry : json["weapons"])
        {
            Weapon weapon{StringOrEmpty(entry, "name"),
                          StringOrEmpty(entry, "display"),
                          StringOrEmpty(entry, "group"),
                          StringOrEmpty(entry, "category"),
                          {}};
            if (const auto it = entry.find("variants"); it != entry.end() && it->is_array())
                for (const auto& node : *it)
                {
                    Variant variant{StringOrEmpty(node, "name"),     Strings(node, "attachments"),
                                    StringOrEmpty(node, "display"),  StringOrEmpty(node, "zone"),
                                    StringOrEmpty(node, "sound"),    StringOrEmpty(node, "sounds"),
                                    node.value("projectile", false), StringOrEmpty(node, "left")};
                    std::ranges::sort(variant.attachments);
                    if (!variant.name.empty() && !variant.zone.empty())
                        weapon.variants.push_back(std::move(variant));
                }
            if (weapon.name.empty() || weapon.variants.empty())
                throw std::runtime_error("campaign-weapons.json has a weapon without its name or variants");
            catalog.weapons.push_back(std::move(weapon));
        }
        if (const auto it = json.find("zones"); it != json.end() && it->is_array())
            for (const auto& node : *it)
                catalog.zones.push_back({StringOrEmpty(node, "zone"), Strings(node, "sources")});
        if (const auto it = json.find("damage"); it != json.end() && it->is_object())
            for (const auto& [name, values] : it->items())
                catalog.damage[name] = Damage{values.at(0).get<std::int32_t>(), values.at(1).get<std::int32_t>(),
                                              values.at(2).get<std::int32_t>(), values.at(3).get<std::int32_t>()};
        if (const auto it = json.find("textures"); it != json.end() && it->is_object())
        {
            catalog.texturesFile = StringOrEmpty(*it, "file");
            if (const auto characters = it->find("characters"); characters != it->end() && characters->is_object())
                for (const auto& [zone, node] : characters->items())
                    if (node.is_array())
                        for (const auto& image : node)
                            if (image.is_string())
                                catalog.characterTextures[zone].push_back(image.get<std::string>());
        }
        return catalog;
    }

    bool HasTextures(const Catalog& catalog, std::string_view zone, std::string_view archiveEnd)
    {
        const auto found = catalog.characterTextures.find(zone);
        if (found == catalog.characterTextures.end())
            return true;
        const auto lower = [](std::string_view text) {
            std::string lowered(text);
            std::ranges::transform(lowered, lowered.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return lowered;
        };
        const auto names = lower(archiveEnd);
        return std::ranges::all_of(found->second, [&](const std::string& image) {
            return names.find("images/" + lower(image) + ".iwi") != std::string::npos;
        });
    }

    std::optional<std::pair<std::int32_t, std::string>> ParseWeaponStringCommand(std::string_view command)
    {
        constexpr std::string_view SET_CONFIG_STRING = "d ";
        if (!command.starts_with(SET_CONFIG_STRING))
            return std::nullopt;
        command.remove_prefix(SET_CONFIG_STRING.size());
        std::int32_t configString = 0;
        const auto [end, error] = std::from_chars(command.data(), command.data() + command.size(), configString);
        if (error != std::errc{} || end == command.data() + command.size() || *end != ' ' ||
            configString < WEAPON_CONFIG_STRINGS ||
            configString >= WEAPON_CONFIG_STRINGS + WEAPON_CONFIG_STRING_COUNT)
            return std::nullopt;
        return std::pair(configString - WEAPON_CONFIG_STRINGS, std::string(end + 1, command.data() + command.size()));
    }

    std::vector<std::string> GetZones(const Catalog& catalog, std::string_view variant)
    {
        const auto found = catalog.FindVariant(variant);
        if (!found)
            return {};
        std::vector<std::string> zones{found->zone};
        if (!found->sounds.empty())
            zones.push_back(found->sounds);
        return zones;
    }

    void AddTo(Types::LoadoutCatalog& loadouts, const Catalog& catalog,
               const std::function<std::string(const std::string& zone)>& unbuilt)
    {
        const auto whyNot = [&](const Variant& variant) {
            for (const auto& zone : GetZones(catalog, variant.name))
                if (auto why = unbuilt(zone); !why.empty())
                    return why;
            return std::string();
        };
        bool upgraded = false;
        for (const auto& weapon : catalog.weapons)
        {
            // a multiplayer gun's upgraded variants, when the map has the gun
            if (weapon.group.empty() && !loadouts.FindWeapon(weapon.name))
                continue;
            if (!weapon.group.empty())
            {
                if (loadouts.FindWeapon(weapon.name))
                    continue;
                loadouts.weapons.push_back({weapon.name, weapon.displayName, weapon.group,
                                            !IsSecondaryCategory(weapon.category), true, weapon.category,
                                            whyNot(weapon.variants.front())});
            }
            for (const auto& variant : weapon.variants)
            {
                if (loadouts.FindVariant(variant.name))
                    continue;
                loadouts.variants.push_back({variant.name, weapon.name, variant.attachments, true, whyNot(variant)});
                upgraded |= std::ranges::find(variant.attachments, PACK_A_PUNCH) != variant.attachments.end();
            }
        }
        if (upgraded && std::ranges::none_of(loadouts.attachments, [](const Types::LoadoutCatalog::Attachment& a) {
                return a.name == PACK_A_PUNCH;
            }))
            loadouts.attachments.push_back({std::string(PACK_A_PUNCH), std::string(PACK_A_PUNCH_NAME)});
    }
}  // namespace IWXMVM::Components::WeaponCatalog
