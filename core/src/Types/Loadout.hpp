#pragma once

namespace IWXMVM::Types
{
    // What a game offers for changing how players look in a demo, by name. Weapons are the game's weapon variants,
    // each a base weapon with a set of attachments.
    struct LoadoutCatalog
    {
        struct Weapon
        {
            std::string name;          // the base weapon, e.g. ak47_mp
            std::string displayName;
            std::string category;      // e.g. Assault Rifles; weapons are listed by category in this order
            bool primary;              // else a secondary, e.g. a pistol
            // A weapon of the campaign or Zombies (Components::WeaponCatalog), which only actors and the player of a
            // live game hold, from a zone of its own: the kind within its category (e.g. Wonder Weapons), and whether
            // its zone is built (else why not, for its tooltip)
            bool actorsOnly = false;
            std::string subcategory;
            std::string unbuilt;
            std::uint32_t fireTime = 0;  // ms between its shots, 0 for what the map's weapon files don't say
            float aimedSpread = 0.0f;    // degrees its pellets spread aimed, 0 for a gun that has none
        };

        struct Variant
        {
            std::string name;          // e.g. ak47_acog_extclip_mp
            std::string weapon;        // the base weapon's name
            std::vector<std::string> attachments;  // sorted
            // a variant from a zone of its own (a campaign or Zombies weapon, or a multiplayer gun's Pack-a-Punch
            // one), which only actors hold, and why it can't be held yet (its zone isn't built), empty when it can
            bool actorsOnly = false;
            std::string unbuilt;
        };

        struct Attachment
        {
            std::string name;          // e.g. acog
            std::string displayName;
        };

        struct Camo
        {
            std::string displayName;
            std::uint32_t color;       // for the swatch, 0xRRGGBB
        };

        std::vector<Weapon> weapons;
        std::vector<Variant> variants;
        std::vector<Attachment> attachments;  // every attachment a weapon has, in the order to list them
        std::vector<Camo> camos;              // by the game's camo number
        std::vector<std::string> bodies;      // the character models the map has loaded
        std::vector<std::string> heads;

        const Weapon* FindWeapon(std::string_view name) const
        {
            const auto it = std::ranges::find(weapons, name, &Weapon::name);
            return it != weapons.end() ? &*it : nullptr;
        }

        const Variant* FindVariant(std::string_view name) const
        {
            const auto it = std::ranges::find(variants, name, &Variant::name);
            return it != variants.end() ? &*it : nullptr;
        }

        const Variant* FindVariant(std::string_view weapon, const std::vector<std::string>& sortedAttachments) const
        {
            const auto it = std::ranges::find_if(variants, [&](const Variant& variant) {
                return variant.weapon == weapon && variant.attachments == sortedAttachments;
            });
            return it != variants.end() ? &*it : nullptr;
        }

        std::string DescribeAttachment(std::string_view name) const
        {
            const auto it = std::ranges::find(attachments, name, &Attachment::name);
            return it != attachments.end() ? it->displayName : std::string(name);
        }
    };

    // Changes what one weapon looks like. Each part that is empty keeps what the demo has.
    struct WeaponRule
    {
        static constexpr std::string_view ANY_PRIMARY = "*primary";
        static constexpr std::string_view ANY_SECONDARY = "*secondary";

        // the base weapon held in the demo, ANY_PRIMARY, ANY_SECONDARY, or empty for any weapon
        std::string from;
        std::optional<std::string> weapon;
        std::optional<std::vector<std::string>> attachments;  // sorted
        std::optional<std::int32_t> camo;

        bool operator==(const WeaponRule&) const = default;
    };

    // How a player, or all players, look. A character of the catalog sets the body, the head (empty for a full body,
    // which has its face) and what is worn, and is kept by its id, for the zone its models are in.
    struct Loadout
    {
        std::optional<std::string> body;
        std::optional<std::string> head;
        std::optional<std::vector<std::string>> worn;
        std::optional<std::string> character;
        std::optional<std::int32_t> facePaint;  // see Components::FacePaintLogic
        std::optional<std::string> stowed;      // the gun on the back, a base weapon; empty takes the demo's off
        std::vector<WeaponRule> weaponRules;

        bool IsEmpty() const
        {
            return !body && !head && !worn && !character && !facePaint && !stowed && weaponRules.empty();
        }

        bool operator==(const Loadout&) const = default;
    };

    // What is chosen for a demo's players, saved with its scene: for all of them, for a team over that, and for a player
    // over both, by team and client number; a death by its animation's name, empty for the demo's own
    struct PlayerChoices
    {
        Loadout loadoutForAll;
        std::map<std::int32_t, Loadout> teamLoadouts;
        std::map<std::int32_t, Loadout> playerLoadouts;
        std::string deathForAll;
        std::map<std::int32_t, std::string> teamDeaths;
        std::map<std::int32_t, std::string> playerDeaths;

        bool operator==(const PlayerChoices&) const = default;
    };
}  // namespace IWXMVM::Types
