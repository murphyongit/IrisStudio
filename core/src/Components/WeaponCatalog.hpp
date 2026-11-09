#pragma once

#include "nlohmann/json.hpp"
#include "Types/Loadout.hpp"

namespace IWXMVM::Components::WeaponCatalog
{
    // The weapons of the campaign and Zombies multiplayer hasn't (core/resources/campaign-weapons.json, made by
    // tools/t5-weapons/weapons.py), and the Pack-a-Punch ones of multiplayer's guns. Each is
    // in a zone of its own built at setup (t5w_<weapon>), and the sound of its shots in a level's sound bank (a zone
    // t5s_<bank>), loaded as an actor holds it or the player of a live game is given it.

    // the attachment the upgraded (Pack-a-Punch) variant of a weapon has
    inline constexpr std::string_view PACK_A_PUNCH = "pap";
    inline constexpr std::string_view PACK_A_PUNCH_NAME = "Pack-a-Punch";

    struct Variant
    {
        std::string name;                      // the game's weapon, e.g. ray_gun_upgraded_zm
        std::vector<std::string> attachments;  // sorted: none, pap, or a multiplayer variant's with pap
        std::string displayName;
        std::string zone;         // t5w_...
        std::string sound;        // the alias its shots play (the first of its sounds a bank has), empty for none
        std::string sounds;       // the zone of the bank that has it, empty when multiplayer's banks have it
        bool projectile = false;  // its shot is a projectile (Ray Gun, Panzerschreck), not a bullet
        std::string left;         // a dual wield gun's left-hand weapon, empty for one held in one hand or two
    };

    struct Weapon
    {
        std::string name;         // its first variant's, or the multiplayer gun its variants upgrade
        std::string displayName;  // empty for a multiplayer gun
        std::string group;        // Campaign or Zombies, empty for a multiplayer gun
        std::string category;     // within the group: Submachine Guns, Wonder Weapons...
        std::vector<Variant> variants;
    };

    struct Zone
    {
        std::string name;
        std::vector<std::string> sources;  // the levels it is linked from
    };

    // What a wonder weapon's def holds in place of the damage Zombies' scripts do, which multiplayer doesn't run
    struct Damage
    {
        std::int32_t direct = 0;  // of a hit
        std::int32_t inner = 0;   // of its explosion, at the centre and at the edge
        std::int32_t outer = 0;
        std::int32_t radius = 0;
    };

    struct Catalog
    {
        std::vector<Weapon> weapons;
        std::vector<Zone> zones;
        std::map<std::string, Damage, std::less<>> damage;  // by the game's weapon
        // the file of the textures only single player's Rezurrection files have, and the images of the character zones
        // that use them: loading one of those without its images ends the game
        std::string texturesFile;
        std::map<std::string, std::vector<std::string>, std::less<>> characterTextures;

        const Variant* FindVariant(std::string_view name) const;
        const Zone* FindZone(std::string_view name) const;
        // whether a weapon of the game is one of the catalog's zones: a variant, or a dual wield variant's left one
        bool Has(std::string_view name) const;
    };

    // throws std::runtime_error for a file that isn't of version 1
    Catalog Parse(const nlohmann::json& json);
    // the catalog the mod embeds (empty when it doesn't read)
    const Catalog& Get();

    // Whether a zone's images are named by the end of the textures' file (where a zip lists what it holds), or it
    // needs none
    bool HasTextures(const Catalog& catalog, std::string_view zone, std::string_view archiveEnd);

    // where the config strings of the weapons start, and how many there are
    inline constexpr std::int32_t WEAPON_CONFIG_STRINGS = 0xB14;
    inline constexpr std::int32_t WEAPON_CONFIG_STRING_COUNT = 0x100;
    // The server command that sets a config string ("d <config string> <text>"), when it is one of the weapons': its
    // place among them and its text
    std::optional<std::pair<std::int32_t, std::string>> ParseWeaponStringCommand(std::string_view command);

    // the zones a weapon variant needs loaded: its own, then its sound's; none for a variant the catalog hasn't
    std::vector<std::string> GetZones(const Catalog& catalog, std::string_view variant);

    // Adds the catalog's weapons to the map's loadout catalog: its groups after multiplayer's categories,
    // the Pack-a-Punch variants as an attachment (B1), also of the multiplayer guns the map has (C1). `unbuilt` says
    // why a zone can't load (empty when it can), for those listed greyed (D1).
    void AddTo(Types::LoadoutCatalog& loadouts, const Catalog& catalog,
               const std::function<std::string(const std::string& zone)>& unbuilt);
}  // namespace IWXMVM::Components::WeaponCatalog
