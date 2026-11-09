#include "StdInclude.hpp"
#include "Test.hpp"

#include "Components/MapMenuLogic.hpp"
#include "Setup/SetupLogic.hpp"

using namespace IWXMVM;

// Campaign levels and Zombies maps in the game's Change Map menu (MapMenuLogic): the tabs'
// rows and their order, the rows added to mp/mapsTable.csv, the language keys, the pictures, and the shipped catalog's
// menu entries

namespace
{
    namespace Maps = Components::CampaignMapLogic;
    namespace Menu = Components::MapMenuLogic;

    const std::filesystem::path SHIPPED_CATALOG =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / "core" / "resources" /
        "campaign-maps.json";
    const std::filesystem::path SHIPPED_KITS =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / "core" / "resources" /
        "campaign-kits.json";

    std::string ReadText(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), {});
    }

    const Maps::Catalog& Shipped()
    {
        static const auto catalog = Maps::ReadCatalog(ReadText(SHIPPED_CATALOG));
        return catalog;
    }

    std::vector<std::string> Names(const std::vector<const Maps::Level*>& levels)
    {
        std::vector<std::string> names;
        for (const auto* level : levels)
            names.push_back(level->name);
        return names;
    }
}  // namespace

TEST("maps menu: the campaign tab lists the 18 levels as the story goes, the Zombies tab the 10 maps as they came out")
{
    const auto all = [](const Maps::Level&) { return true; };
    const auto campaign = Names(Menu::ListTab(Shipped(), Menu::Tab::Campaign, all));
    CHECK(campaign.size() == 18);
    CHECK(campaign.front() == "frontend" && campaign[1] == "cuba" && campaign[2] == "vorkuta");
    CHECK(campaign[11] == "wmd_sr71" && campaign[12] == "wmd" && campaign.back() == "outro");
    const auto zombies = Names(Menu::ListTab(Shipped(), Menu::Tab::Zombies, all));
    CHECK(zombies.size() == 10);
    CHECK(zombies.front() == "zombie_theater" && zombies[5] == "zombie_moon");
    CHECK(zombies.back() == "zombie_cod5_factory");
    CHECK(Menu::GetTabLabel(Menu::Tab::Campaign) == "CAMPAIGN" && Menu::GetTabLabel(Menu::Tab::Zombies) == "ZOMBIES");
    CHECK(Menu::GetTabLabelKey(Menu::Tab::Zombies) == "@IRIS_TAB_ZOMBIES");
    CHECK(Menu::ResolveKey(Shipped(), "IRIS_TAB_CAMPAIGN", {}) == "CAMPAIGN");
}

TEST("maps menu: maps the game folder lacks are left out, and a tab can end up empty")
{
    const auto steam = [](const Maps::Level& level) {
        return level.mode == Maps::Mode::Campaign || level.name == "zombie_theater" || level.name == "zombie_pentagon";
    };
    CHECK(Names(Menu::ListTab(Shipped(), Menu::Tab::Zombies, steam)) ==
          std::vector<std::string>({"zombie_theater", "zombie_pentagon"}));
    CHECK(Menu::ListTab(Shipped(), Menu::Tab::Zombies, [](const Maps::Level&) { return false; }).empty());
}

TEST("maps menu: a level's row of mp/mapsTable.csv has the game's 16 columns, a pack the game's tabs never list")
{
    const auto& cuba = *Shipped().FindLevel("cuba");
    const auto row = Menu::MakeTableRow(cuba, 27);
    CHECK(row.size() == Menu::TABLE_COLUMNS);
    CHECK(row[0] == "cuba" && row[1] == "cubans" && row[2] == "cubans");
    CHECK(row[3] == "IRIS_MAP_CUBA" && row[4] == "menu_cuba_map_select_final" && row[5] == "27");
    CHECK(row[6] == "MENU_LEVEL_CUBA_DESC" && row[8].empty() && row[9] == "NO" && row[10] == "YES");
    CHECK(row[12] == "MPUI_REBELS_SHORT" && row[15] == "tropas");
    const auto pack = std::stoi(row[11]);
    CHECK(pack > 5 && Menu::FindTabOfPack(pack) == Menu::Tab::Campaign);
    const auto zombiesPack = std::stoi(Menu::MakeTableRow(*Shipped().FindLevel("zombie_moon"), 40)[11]);
    CHECK(zombiesPack > 5 && zombiesPack != pack && Menu::FindTabOfPack(zombiesPack) == Menu::Tab::Zombies);
    CHECK(!Menu::FindTabOfPack(0) && !Menu::FindTabOfPack(2));
}

TEST("maps menu: names come from the game's keys, the catalog's texts where multiplayer has none, ours for three")
{
    const auto game = [](const std::string& key) -> std::optional<std::string> {
        if (key == "MENU_LEVEL_CUBA_CAPS")
            return "OPERATION 40";
        return std::nullopt;
    };
    CHECK(Menu::ResolveKey(Shipped(), "IRIS_MAP_CUBA_CAPS", game) == "OPERATION 40");
    CHECK(Menu::ResolveKey(Shipped(), "IRIS_MAP_CUBA", game) == "Operation 40");
    CHECK(Menu::ResolveKey(Shipped(), "IRIS_MAP_FRONTEND_CAPS", game) == "INTERROGATION");
    CHECK(Menu::ResolveKey(Shipped(), "IRIS_MAP_WMD_SR71_CAPS", game) == "SR-71");
    CHECK(Menu::ResolveKey(Shipped(), "IRIS_MAP_OUTRO_CAPS", game) == "EPILOGUE");
    CHECK(Menu::ResolveKey(Shipped(), "IRIS_MAP_OUTRO_DESC", game) == "The epilogue's stage.");
    // Moon and the remakes are named only in single player's language files: the catalog has their texts
    CHECK(Menu::ResolveKey(Shipped(), "IRIS_MAP_ZOMBIE_MOON_CAPS", game) == "MOON");
    CHECK(Menu::ResolveKey(Shipped(), "IRIS_MAP_ZOMBIE_COD5_ASYLUM_CAPS", game) == "VERR\xC3\x9C" "CKT");
    CHECK(Menu::ResolveKey(Shipped(), "WAW_MENU_ZOMBIE_FACTORY_DESC", game)->starts_with("The Giant is rising."));
    // the game's own keys stay the game's
    CHECK(!Menu::ResolveKey(Shipped(), "MENU_LEVEL_CUBA_DESC", game));
    CHECK(!Menu::ResolveKey(Shipped(), "MPUI_ARRAY_CAPS", game));
    CHECK(!Menu::ResolveKey(Shipped(), "IRIS_MAP_MP_NUKED_CAPS", game));
    CHECK(Menu::ToGameText("VERR\xC3\x9C" "CKT") == "VERR\xDC" "CKT");
    CHECK(Menu::ToGameText("a\xE2\x80\x94" "b") == "a?b");
}

TEST("maps menu: pictures are the materials the menu asks for by the map's name, from the setup's images")
{
    CHECK(Menu::GetPictureMaterial("cuba", false) == "menu_cuba_map_select_final");
    CHECK(Menu::GetPictureMaterial("cuba", true) == "menu_cuba_map_select_big");
    CHECK(Menu::FindLevelOfPictureMaterial(Shipped(), "menu_cuba_map_select_final") == "cuba");
    CHECK(Menu::FindLevelOfPictureMaterial(Shipped(), "menu_zombie_cod5_sumpf_map_select_big") == "zombie_cod5_sumpf");
    CHECK(!Menu::FindLevelOfPictureMaterial(Shipped(), "menu_mp_array_map_select_final"));
    CHECK(!Menu::FindLevelOfPictureMaterial(Shipped(), "menu_cuba_map_select"));
    CHECK(Shipped().FindLevel("frontend")->menu->picture == "frontend/int_escape");
    CHECK(Shipped().FindLevel("wmd_sr71")->menu->picture == "frontend/wmd");
    CHECK(Shipped().FindLevel("outro")->menu->picture.empty());
    CHECK(Shipped().FindLevel("zombie_cod5_prototype")->menu->picture == "patch/loadscreen_zombie_prototype_mini");
    CHECK(Menu::GetPictureImage("patch/zombie_moon") == "zombie_moon");
    // the setup copies each picture once (campaign-kits.json)
    const auto kits = ReadText(SHIPPED_KITS);
    CHECK(kits.find("\"image\": \"zombie_moon\"") != std::string::npos);
    CHECK(kits.find("\"image\": \"cuba\"") != std::string::npos);
}

TEST("maps menu: a table cell's hash is the game's (djb2 over lowercase letters)")
{
    CHECK(Menu::HashCell("") == 5381);
    CHECK(Menu::HashCell("a") == 5381 * 33 + 'a');
    CHECK(Menu::HashCell("MP_Array") == Menu::HashCell("mp_array"));
}

TEST("maps menu: the setup copies the 25 pictures of campaign-kits.json, each from its zone")
{
    const auto pictures = Setup::ReadMenuPictures(ReadText(SHIPPED_KITS));
    CHECK(pictures.size() == 25);
    const auto has = [&](std::string_view zone, std::string_view image) {
        return std::ranges::any_of(pictures, [&](const auto& p) { return p.zone == zone && p.image == image; });
    };
    CHECK(has("frontend", "cuba") && has("frontend", "zombie_theater") && has("patch", "zombie_moon"));
    CHECK(has("patch", "loadscreen_zombie_factory_mini") && !has("frontend", "outro"));
    // every catalog entry's picture is one of them
    for (const auto& level : Shipped().levels)
        if (level.menu && !level.menu->picture.empty())
        {
            const auto slash = level.menu->picture.find('/');
            CHECK(has(level.menu->picture.substr(0, slash), level.menu->picture.substr(slash + 1)));
        }
    CHECK(Setup::ReadMenuPictures(R"({"version":1,"kits":[]})").empty());
}
