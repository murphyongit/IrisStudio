#include "StdInclude.hpp"
#include "Test.hpp"

#include <fstream>
#include <sstream>

#include "Components/WeaponCatalog.hpp"
#include "Setup/SetupLogic.hpp"

using namespace IWXMVM;

// The campaign's and Zombies' weapons: campaign-weapons.json, what it adds to the
// map's loadout catalog for actors, and the zones Iris Studio builds of it

namespace
{
    namespace WeaponCatalog = Components::WeaponCatalog;

    const auto RESOURCES =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / "core" / "resources";

    std::string ReadText(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        std::stringstream text;
        text << file.rdbuf();
        return text.str();
    }

    WeaponCatalog::Catalog Sample()
    {
        return WeaponCatalog::Parse(nlohmann::json::parse(R"({
            "version": 1,
            "weapons": [
                {"name": "ppsh_sp", "display": "PPSh-41", "group": "Campaign", "category": "Submachine Guns",
                 "variants": [{"name": "ppsh_sp", "attachments": [], "display": "PPSh-41", "zone": "t5w_ppsh_sp",
                               "sound": "wpn_ppsh_fire", "sounds": "t5s_spl_fullahead", "projectile": false,
                               "left": ""}]},
                {"name": "ray_gun_zm", "display": "Ray Gun", "group": "Zombies", "category": "Wonder Weapons",
                 "variants": [{"name": "ray_gun_zm", "attachments": [], "zone": "t5w_ray_gun_zm",
                               "sound": "wpn_rgun_fire_npc", "sounds": "t5s_zmb_common", "projectile": true},
                              {"name": "ray_gun_upgraded_zm", "attachments": ["pap"], "display": "Porter's X2 Ray Gun",
                               "zone": "t5w_ray_gun_zm", "sound": "wpn_rgun_fire_npc", "sounds": "t5s_zmb_common",
                               "projectile": true}]},
                {"name": "tokarevtt30_sp", "display": "Tokarev TT-33", "group": "Campaign", "category": "Pistols",
                 "variants": [{"name": "tokarevtt30_sp", "attachments": [], "zone": "t5w_tokarevtt30_sp",
                               "sound": "", "sounds": ""}]},
                {"name": "cz75_mp",
                 "variants": [{"name": "cz75_upgraded_zm", "attachments": ["pap"], "zone": "t5w_cz75_upgraded_zm",
                               "sound": "wpn_cz75_fire_npc_pap", "sounds": "t5s_zmb_common"},
                              {"name": "cz75dw_upgraded_zm", "attachments": ["pap", "dw"],
                               "zone": "t5w_cz75_upgraded_zm", "sound": "wpn_cz75_fire_npc_pap",
                               "sounds": "t5s_zmb_common",
                               "left": "cz75lh_upgraded_zm"}]},
                {"name": "l96a1_mp",
                 "variants": [{"name": "l96a1_upgraded_zm", "attachments": ["pap"], "zone": "t5w_l96a1_upgraded_zm",
                               "sound": "wpn_l96a1_fire_npc", "sounds": ""}]}
            ],
            "zones": [{"zone": "t5w_ppsh_sp", "sources": ["fullahead"], "assets": [["weapon", "ppsh_sp"]]}]
        })"));
    }

    // a map's loadout catalog: a submachine gun and the CZ75, single and dual wield
    Types::LoadoutCatalog Map()
    {
        Types::LoadoutCatalog loadouts;
        loadouts.weapons = {{"ak74u_mp", "AK-74u", "Submachine Guns", true}, {"cz75_mp", "CZ75", "Pistols", false}};
        loadouts.variants = {
            {"ak74u_mp", "ak74u_mp", {}}, {"cz75_mp", "cz75_mp", {}}, {"cz75dw_mp", "cz75_mp", {"dw"}}};
        loadouts.attachments = {{"dw", "Dual Wield"}};
        return loadouts;
    }
}  // namespace

TEST("weapons: the catalog's variants, their zones and sounds")
{
    const auto catalog = Sample();
    CHECK(catalog.weapons.size() == 5);
    const auto upgraded = catalog.FindVariant("ray_gun_upgraded_zm");
    CHECK(upgraded && upgraded->displayName == "Porter's X2 Ray Gun" && upgraded->projectile);
    // attachments sorted, as the loadout catalog finds variants by them
    const auto dual = catalog.FindVariant("cz75dw_upgraded_zm");
    CHECK((dual && dual->attachments == std::vector<std::string>{"dw", "pap"} && dual->left == "cz75lh_upgraded_zm"));
    // its own zone, then the bank's
    CHECK(
        (WeaponCatalog::GetZones(catalog, "ppsh_sp") == std::vector<std::string>{"t5w_ppsh_sp", "t5s_spl_fullahead"}));
    // a sound multiplayer's banks have needs no bank zone; a weapon the catalog hasn't needs none
    CHECK((WeaponCatalog::GetZones(catalog, "l96a1_upgraded_zm") == std::vector<std::string>{"t5w_l96a1_upgraded_zm"}));
    CHECK(WeaponCatalog::GetZones(catalog, "ak74u_mp").empty());
    CHECK(catalog.FindZone("t5w_ppsh_sp") && catalog.FindZone("t5w_ppsh_sp")->sources.front() == "fullahead");
    // its left-hand guns are the catalog's too (their zone is the variant's), multiplayer's guns aren't
    CHECK(catalog.Has("cz75lh_upgraded_zm") && catalog.Has("ray_gun_zm") && !catalog.Has("cz75_mp"));
}

TEST("weapons: the groups come after multiplayer's guns, and Pack-a-Punch is an attachment (picks A1, B1, C1)")
{
    auto loadouts = Map();
    WeaponCatalog::AddTo(loadouts, Sample(), [](const std::string&) { return std::string(); });
    // the map's first, then Campaign and Zombies in the catalog's order
    std::vector<std::string> names;
    for (const auto& weapon : loadouts.weapons)
        names.push_back(weapon.name);
    CHECK((names == std::vector<std::string>{"ak74u_mp", "cz75_mp", "ppsh_sp", "ray_gun_zm", "tokarevtt30_sp"}));
    const auto ppsh = loadouts.FindWeapon("ppsh_sp");
    CHECK(ppsh && ppsh->actorsOnly && ppsh->category == "Campaign" && ppsh->subcategory == "Submachine Guns" &&
          ppsh->primary && ppsh->displayName == "PPSh-41");
    CHECK(!loadouts.FindWeapon("tokarevtt30_sp")->primary);
    CHECK(!loadouts.FindWeapon("ak74u_mp")->actorsOnly);
    // the Ray Gun upgraded, and the map's CZ75 too (single and dual wield); the L96A1 isn't on this map
    const auto rayGun = loadouts.FindVariant("ray_gun_zm", std::vector<std::string>{"pap"});
    CHECK(rayGun && rayGun->name == "ray_gun_upgraded_zm" && rayGun->actorsOnly);
    CHECK(loadouts.FindVariant("cz75_mp", std::vector<std::string>{"pap"})->name == "cz75_upgraded_zm");
    CHECK(loadouts.FindVariant("cz75_mp", std::vector<std::string>{"dw", "pap"})->name == "cz75dw_upgraded_zm");
    CHECK(!loadouts.FindVariant("cz75_mp", std::vector<std::string>{"dw"})->actorsOnly);
    CHECK(!loadouts.FindVariant("l96a1_upgraded_zm"));
    CHECK(loadouts.DescribeAttachment("pap") == "Pack-a-Punch");
}

TEST("weapons: a weapon whose zone isn't built is listed with why (pick D1)")
{
    auto loadouts = Map();
    WeaponCatalog::AddTo(loadouts, Sample(), [](const std::string& zone) {
        return zone == "t5s_zmb_common" ? std::string("Run Iris Studio to add it") : std::string();
    });
    CHECK(loadouts.FindWeapon("ppsh_sp")->unbuilt.empty());
    // its sound's bank counts as its own zone
    CHECK(loadouts.FindWeapon("ray_gun_zm")->unbuilt == "Run Iris Studio to add it");
    CHECK(loadouts.FindVariant("cz75_upgraded_zm")->unbuilt == "Run Iris Studio to add it");
}

TEST("campaign-weapons.json: every wonder weapon but the Ray Gun has its damage, the left guns and modes too (#90)")
{
    const auto catalog = WeaponCatalog::Parse(nlohmann::json::parse(ReadText(RESOURCES / "campaign-weapons.json")));
    const auto thundergun = catalog.damage.find("thundergun_zm");
    CHECK(thundergun != catalog.damage.end());
    CHECK(thundergun->second.direct == 1000);
    CHECK(thundergun->second.radius == 240);
    CHECK(!catalog.damage.contains("ray_gun_zm"));
    CHECK(catalog.damage.contains("microwavegunlh_zm"));
    CHECK(catalog.damage.contains("microwavegun_zm"));
    // each is a weapon of the catalog's zones, or a dual wield gun's or mode's
    for (const auto& weapon : catalog.weapons)
        if (weapon.category == "Wonder Weapons" && weapon.name != "ray_gun_zm")
            for (const auto& variant : weapon.variants)
                CHECK(catalog.damage.contains(variant.name));
}

TEST("weapons: a film's server command that sets a weapon's config string is read (#85)")
{
    constexpr auto FIRST = WeaponCatalog::WEAPON_CONFIG_STRINGS;
    constexpr auto COUNT = WeaponCatalog::WEAPON_CONFIG_STRING_COUNT;
    const auto gun = WeaponCatalog::ParseWeaponStringCommand("d 2952 1706 ppsh_sp");
    CHECK(gun.has_value());
    CHECK(gun->first == 116);
    CHECK(gun->second == "1706 ppsh_sp");

    const auto first = WeaponCatalog::ParseWeaponStringCommand(std::format("d {} 1 ak47_mp", FIRST));
    CHECK(first.has_value() && first->first == 0);
    const auto last = WeaponCatalog::ParseWeaponStringCommand(std::format("d {} 2 ak74u_mp", FIRST + COUNT - 1));
    CHECK(last.has_value() && last->first == COUNT - 1);

    // other config strings and commands
    CHECK(!WeaponCatalog::ParseWeaponStringCommand(std::format("d {} 1 ak47_mp", FIRST - 1)));
    CHECK(!WeaponCatalog::ParseWeaponStringCommand(std::format("d {} 1 ak47_mp", FIRST + COUNT)));
    CHECK(!WeaponCatalog::ParseWeaponStringCommand("a 1706"));
    CHECK(!WeaponCatalog::ParseWeaponStringCommand("d"));
    CHECK(!WeaponCatalog::ParseWeaponStringCommand("d 2960"));
    CHECK(!WeaponCatalog::ParseWeaponStringCommand("d x 1706 ppsh_sp"));
    CHECK(!WeaponCatalog::ParseWeaponStringCommand("data 2960 1706 ppsh_sp"));
}

TEST("weapons: a catalog that isn't one throws")
{
    bool threw = false;
    try
    {
        WeaponCatalog::Parse(nlohmann::json::parse(R"({"version": 2, "weapons": []})"));
    }
    catch (const std::runtime_error&)
    {
        threw = true;
    }
    CHECK(threw);
}

TEST("campaign-weapons.json: Project Nova's guns, the wonder weapons upgraded, each in a zone Iris Studio builds")
{
    const auto text = ReadText(RESOURCES / "campaign-weapons.json");
    const auto catalog = WeaponCatalog::Parse(nlohmann::json::parse(text));
    for (const auto name : {"ppsh_sp", "mp40_sp", "sten_sp", "stg44_sp", "mosin_sp", "mosin_scope_sp", "tokarevtt30_sp",
                            "waltherp38_sp", "panzerschreck_sp", "ray_gun_zm", "thundergun_zm", "tesla_gun_zm"})
        CHECK(catalog.FindVariant(name));
    for (const auto name : {"ray_gun_upgraded_zm", "thundergun_upgraded_zm", "m1911_upgraded_zm"})
    {
        const auto variant = catalog.FindVariant(name);
        CHECK(variant && std::ranges::find(variant->attachments, "pap") != variant->attachments.end());
    }
    CHECK(catalog.FindVariant("m1911_upgraded_zm")->left == "m1911lh_upgraded_zm");
    CHECK(catalog.FindVariant("ray_gun_zm")->projectile && !catalog.FindVariant("ppsh_sp")->projectile);

    // every zone a variant names is one Iris Studio builds
    const auto zones = Setup::ReadWeaponZones(text);
    for (const auto& weapon : catalog.weapons)
        for (const auto& variant : weapon.variants)
            for (const auto& zone : WeaponCatalog::GetZones(catalog, variant.name))
                CHECK(std::ranges::find(zones, zone, &Setup::CharacterZone::name) != zones.end());
    // and the textures only Rezurrection's files have (World at War's guns'), for one file of Plutonium's storage
    const auto textures = Setup::ReadWeaponTextures(text);
    CHECK(textures.file == "iw_iris_weapons.iwd" && textures.images.size() > 50);
    const auto ppsh = std::ranges::find(zones, "t5w_ppsh_sp", &Setup::CharacterZone::name);
    CHECK((ppsh != zones.end() && ppsh->level == "fullahead" && ppsh->assets.size() == 1 &&
           ppsh->assets[0] == std::pair<std::string, std::string>{"weapon", "ppsh_sp"}));
}

TEST("weapon zones are linked from the first of their levels the game has")
{
    Setup::GameZones game;
    game.common = {"patch.ff", "common_zombie.ff", "common_zombie_patch.ff", "zombie_pentagon.ff",
                   "zombie_pentagon_patch.ff"};
    Setup::CharacterZone rayGun{
        "t5w_ray_gun_zm", "zombie_theater", {}, {{"weapon", "ray_gun_zm"}}, {"zombie_pentagon", "zombie_moon"}};
    CHECK(Setup::ChooseLevel(game, rayGun) == std::optional<std::string>("zombie_pentagon"));
    game.common.push_back("zombie_theater.ff");
    game.common.push_back("zombie_theater_patch.ff");
    CHECK(Setup::ChooseLevel(game, rayGun) == std::optional<std::string>("zombie_theater"));
    Setup::CharacterZone shrinkRay{"t5w_shrink_ray_zm", "zombie_temple", {}, {{"weapon", "shrink_ray_zm"}}, {}};
    CHECK(!Setup::ChooseLevel(game, shrinkRay));
}

// Character zones with images only Rezurrection's files have: the game ends on an image it can't load
TEST("weapons: a character zone's images are looked for in the textures' file")
{
    const auto catalog = WeaponCatalog::Parse(nlohmann::json::parse(R"({"version": 1, "weapons": [], "zones": [],
        "textures": {"file": "iw_iris_weapons.iwd", "images": ["char_jap_impinf_body5z_1_c"],
                     "characters": {"t5c_zombie": ["Char_Jap_Impinf_Body5z_1_c", "b"]}}})"));
    CHECK(catalog.texturesFile == "iw_iris_weapons.iwd");
    CHECK(WeaponCatalog::HasTextures(catalog, "t5c_zombie", "xximages/char_jap_impinf_body5z_1_c.iwixximages/b.iwi"));
    CHECK(!WeaponCatalog::HasTextures(catalog, "t5c_zombie", "images/b.iwi"));
    CHECK(!WeaponCatalog::HasTextures(catalog, "t5c_zombie", ""));
    // one that needs none
    CHECK(WeaponCatalog::HasTextures(catalog, "t5c_other", ""));
}

TEST("campaign-weapons.json: the 14 character zones that need Rezurrection's images, all in the textures' file")
{
    const auto text = ReadText(RESOURCES / "campaign-weapons.json");
    const auto catalog = WeaponCatalog::Parse(nlohmann::json::parse(text));
    const auto textures = Setup::ReadWeaponTextures(text);
    CHECK(catalog.characterTextures.size() == 14);
    CHECK(catalog.characterTextures.contains("t5c_zombie_cod5_sumpf_char_jap_zombie"));
    for (const auto& [zone, images] : catalog.characterTextures)
        for (const auto& image : images)
            CHECK(std::ranges::find(textures.images, image) != textures.images.end());
}
