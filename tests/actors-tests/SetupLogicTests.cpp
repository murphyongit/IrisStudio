#include "StdInclude.hpp"
#include "Test.hpp"

#include "Setup/SetupLogic.hpp"

#include <set>

using namespace IWXMVM;

// What the launcher works out without Windows or the game (Setup/SetupLogic):
// Steam's library files, the fingerprint of the game's zones, and how sizes and counts are written

namespace
{
    // steamapps\libraryfolders.vdf as Steam writes it since 2021 (this machine's, with a second library added)
    constexpr std::string_view LIBRARY_FOLDERS = R"("libraryfolders"
{
	"0"
	{
		"path"		"C:\\Program Files (x86)\\Steam"
		"label"		""
		"contentid"		"6459475780743688243"
		"totalsize"		"0"
		"apps"
		{
			"7940"		"6988163320"
			"10180"		"12100050737"
		}
	}
	"1"
	{
		"path"		"D:\\SteamLibrary"
		"label"		"Games"
		"apps"
		{
			"42700"		"9617628587"
			"42710"		"8580096"
		}
	}
}
)";
}  // namespace

TEST("libraryfolders.vdf: every library, the one with Black Ops first")
{
    const auto libraries = Setup::SteamLibraries(LIBRARY_FOLDERS, Setup::BLACK_OPS_APP_ID);
    CHECK(libraries.size() == 2);
    CHECK(libraries[0].path == std::filesystem::path("D:\\SteamLibrary"));
    CHECK(libraries[0].hasApp);
    CHECK(libraries[1].path == std::filesystem::path("C:\\Program Files (x86)\\Steam"));
    CHECK(!libraries[1].hasApp);
}

TEST("libraryfolders.vdf: the format before 2021, with numbered paths and other keys")
{
    constexpr std::string_view old = "\xEF\xBB\xBF\"LibraryFolders\"\n{\n\t\"TimeNextStatsReport\"\t\t\"1600000000\"\n"
                                     "\t\"ContentStatsID\"\t\t\"-123\"\n\t\"1\"\t\t\"E:\\\\Steam Games\"\n}\n";
    const auto libraries = Setup::SteamLibraries(old, Setup::BLACK_OPS_APP_ID);
    CHECK(libraries.size() == 1);
    CHECK(libraries[0].path == std::filesystem::path("E:\\Steam Games"));
    CHECK(!libraries[0].hasApp);
}

TEST("KeyValues: comments, bare words, escapes and nested blocks")
{
    const auto root = Setup::ParseKeyValues("// a comment\n\"a\" { b \"quoted \\\"text\\\"\" // after\n c { d e } }");
    CHECK(root.has_value());
    const auto a = root->Find("A");
    CHECK(a && a->block);
    CHECK(a->Find("b") && a->Find("b")->value == "quoted \"text\"");
    CHECK(a->Find("c") && a->Find("c")->Find("d") && a->Find("c")->Find("d")->value == "e");
    CHECK(!a->Find("missing"));
}

TEST("KeyValues: text that isn't, gives nothing or no libraries")
{
    CHECK(!Setup::ParseKeyValues("\"a\" { \"b\" \"c\"").has_value());
    CHECK(!Setup::ParseKeyValues("\"a\" \"unterminated").has_value());
    CHECK(!Setup::ParseKeyValues("}").has_value());
    CHECK(Setup::SteamLibraries("not a vdf {", Setup::BLACK_OPS_APP_ID).empty());
    CHECK(Setup::SteamLibraries("\"other\" { \"0\" { \"path\" \"C:\\\\x\" } }", Setup::BLACK_OPS_APP_ID).empty());
}

TEST("appmanifest: the install folder")
{
    constexpr std::string_view manifest = "\"AppState\"\n{\n\t\"appid\"\t\t\"42700\"\n\t\"name\"\t\t\"Call of Duty: "
                                          "Black Ops\"\n\t\"installdir\"\t\t\"Call of Duty Black Ops\"\n}\n";
    CHECK(Setup::InstallDir(manifest) == std::optional<std::string>("Call of Duty Black Ops"));
    CHECK(!Setup::InstallDir("\"AppState\" { \"appid\" \"42700\" }").has_value());
    CHECK(!Setup::InstallDir("broken {").has_value());
}

TEST("the zones' fingerprint: the same in any order and case, another when a size changes")
{
    const std::vector<Setup::ZoneFile> files = {
        {"Common\\cuba.ff", 102775840}, {"Common\\patch.ff", 4096}, {"English\\en_cuba.ff", 1234567}};
    const auto fingerprint = Setup::Fingerprint(files);
    CHECK(fingerprint.size() == 16);
    CHECK(std::all_of(fingerprint.begin(), fingerprint.end(), [](char c) { return std::isxdigit(c) != 0; }));
    CHECK(Setup::Fingerprint({files[2], files[0], files[1]}) == fingerprint);
    CHECK(Setup::Fingerprint({{"common/CUBA.ff", 102775840}, {"COMMON\\Patch.FF", 4096}, {"english/en_cuba.ff", 1234567}}) ==
          fingerprint);
    CHECK(Setup::Fingerprint({{"Common\\cuba.ff", 102775841}, files[1], files[2]}) != fingerprint);
    CHECK(Setup::Fingerprint({files[0], files[1]}) != fingerprint);
    // a name can't run into the size of the one before it
    CHECK(Setup::Fingerprint({{"a1", 23}}) != Setup::Fingerprint({{"a", 123}}));
}

TEST("counts and sizes as the launcher writes them")
{
    CHECK(Setup::FormatCount(0) == "0");
    CHECK(Setup::FormatCount(272) == "272");
    CHECK(Setup::FormatCount(7938) == "7,938");
    CHECK(Setup::FormatCount(1234567) == "1,234,567");
    CHECK(Setup::FormatSize(512) == "512 bytes");
    CHECK(Setup::FormatSize(820 * 1024) == "820 KB");
    CHECK(Setup::FormatSize(3984588) == "3.8 MB");
    CHECK(Setup::FormatSize(280408189) == "267 MB");
    CHECK(Setup::FormatSize(225467522) == "215 MB");
    CHECK(Setup::FormatSize(1288490189) == "1.2 GB");
}

TEST("the player's name: what Plutonium's -name takes")
{
    CHECK(Setup::PlayerName("Iris") == "Iris");
    CHECK(Setup::PlayerName("two words") == "twowords");
    CHECK(Setup::PlayerName("\"quoted\"\\") == "quoted");
    CHECK(Setup::PlayerName("averyveryverylongname") == "averyveryverylo");
    CHECK(Setup::PlayerName("") == "Player");
    CHECK(Setup::PlayerName("   ") == "Player");
    CHECK(Setup::PlayerName("\xC3\xA9t\xC3\xA9") == "t");
}

namespace
{
    const std::filesystem::path RESOURCES =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / "core" / "resources";

    std::string ReadText(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }

    // the game's zones as a full game folder has them, some of them
    Setup::GameZones SomeGameZones()
    {
        Setup::GameZones zones;
        zones.common = {"common.ff", "common_zombie.ff", "common_zombie_patch.ff", "cuba.ff", "patch.ff",
                        "zombie_theater.ff", "zombie_theater_patch.ff"};
        zones.languages = {{"English", {"en_zombie_theater.ff", "en_patch.ff", "en_common_zombie.ff", "en_cuba.ff",
                                        "en_common.ff", "en_frontend.ff"}}};
        return zones;
    }
}  // namespace

TEST("characters.json: a zone for each of its 307 characters, the models as zones.py lists them")
{
    const auto zones = Setup::ReadCharacterZones(ReadText(RESOURCES / "characters.json"));
    CHECK(zones.size() == 307);
    const auto tropas = std::find_if(zones.begin(), zones.end(),
                                     [](const Setup::CharacterZone& z) { return z.name == "t5c_cuba_cub_tropas_2"; });
    CHECK(tropas != zones.end());
    if (tropas == zones.end())
        return;
    CHECK(tropas->level == "cuba");
    // the parts, the gear array (its first already listed), the wounded torsos and legs, then the thrown limbs
    const std::vector<std::string> expected = {
        "c_cub_tropas_body1",           "c_cub_tropas_head2",           "c_cub_tropas_gear1",
        "c_cub_tropas_gear2",           "c_cub_tropas_gear3",           "c_cub_tropas_body1_g_upclean",
        "c_cub_tropas_body1_g_rarmoff", "c_cub_tropas_body1_g_larmoff", "c_cub_tropas_body1_g_torso",
        "c_cub_tropas_body1_g_lowclean", "c_cub_tropas_body1_g_rlegoff", "c_cub_tropas_body1_g_llegoff",
        "c_cub_tropas_body1_g_legsoff", "c_cub_tropas_body1_g_rarmspawn", "c_cub_tropas_body1_g_larmspawn",
        "c_cub_tropas_body1_g_rlegspawn", "c_cub_tropas_body1_g_llegspawn"};
    CHECK(tropas->models == expected);
    // the last level's last: a body only its scripts use (Dead Ops' moss zombie), with the head of its name
    CHECK(zones.back().name == "t5c_zombietron_usa_sog_mp_body_camo");
    CHECK((zones.back().models == std::vector<std::string>{"c_usa_sog_mp_body_camo", "c_usa_sog_mp_head_1"}));
    const auto agent = std::find_if(zones.begin(), zones.end(), [](const Setup::CharacterZone& z) {
        return z.name == "t5c_hue_city_usa_jungmar_fieldagent_fb";
    });
    CHECK(agent != zones.end() && agent->level == "hue_city" &&
          agent->models == std::vector<std::string>{"c_usa_jungmar_fieldagent_fb"});
    CHECK(Setup::ZoneDefinition("t5c_x", {"a", "b"}) == ">game,T5\n>name,t5c_x\n\nxmodel,a\nxmodel,b\n");
}

TEST("characters.json and animations.json that aren't of their version throw")
{
    bool threw = false;
    try
    {
        Setup::ReadCharacterZones("{\"version\": 1, \"characters\": []}");
    }
    catch (const std::runtime_error&)
    {
        threw = true;
    }
    CHECK(threw);
    threw = false;
    try
    {
        Setup::ReadAnimationCatalog("not json");
    }
    catch (const std::runtime_error&)
    {
        threw = true;
    }
    CHECK(threw);
}

TEST("animations.json: every animation with its hash and zones")
{
    const auto catalog = Setup::ReadAnimationCatalog(ReadText(RESOURCES / "animations.json"));
    CHECK(catalog.animations.size() == 7938);
    CHECK(!catalog.zones.empty() && catalog.zones[0] == "common_mp");
    std::size_t withoutZones = 0;
    for (const auto& animation : catalog.animations)
        withoutZones += animation.zones.empty() ? 1 : 0;
    CHECK(withoutZones == 0);
    const auto keyed = Setup::ReadAnimationCatalog(
        R"({"version": 1, "zones": [{"zone": "common_mp"}, {"zone": "cuba"}], "animations": [{"n": "pb_stand_alert", "z": [0, 1], "h": "0123456789abcdef"}]})");
    CHECK(keyed.animations.size() == 1);
    CHECK(keyed.animations[0].key == "pb_stand_alert");
    CHECK(keyed.animations[0].hash == (std::array<std::uint8_t, 8>{0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef}));
    CHECK(keyed.animations[0].zones == (std::vector<std::uint16_t>{0, 1}));
}

TEST("what the Linker loads for a level: each zone after its language twin, as zones.py")
{
    const auto zones = SomeGameZones();
    const auto cuba = Setup::LevelLoads(zones, "cuba");
    CHECK(cuba.has_value());
    if (cuba)
    {
        const std::vector<std::filesystem::path> expected = {
            "zone/English/en_patch.ff",  "zone/Common/patch.ff", "zone/English/en_common.ff",
            "zone/Common/common.ff",     "zone/English/en_cuba.ff", "zone/Common/cuba.ff"};
        CHECK(*cuba == expected);
    }
    const auto theater = Setup::LevelLoads(zones, "zombie_theater");
    CHECK(theater.has_value());
    if (theater)
    {
        const std::vector<std::filesystem::path> expected = {
            "zone/English/en_patch.ff",        "zone/Common/patch.ff",
            "zone/English/en_common_zombie.ff", "zone/Common/common_zombie.ff",
            "zone/Common/common_zombie_patch.ff", "zone/Common/zombie_theater_patch.ff",
            "zone/English/en_zombie_theater.ff", "zone/Common/zombie_theater.ff"};
        CHECK(*theater == expected);
    }
    // a level the game hasn't (a map pack not installed)
    CHECK(!Setup::LevelLoads(zones, "zombie_moon").has_value());
    // a language twin alone isn't the level
    CHECK(!Setup::LevelLoads(zones, "frontend").has_value());
    CHECK(zones.HasCommon("CUBA"));
    CHECK(!zones.HasCommon("zombie_moon"));

    // a multiplayer map, a kit's level: after multiplayer's base zones
    auto withMaps = zones;
    withMaps.common.insert(withMaps.common.end(), {"code_post_gfx_mp.ff", "patch_mp.ff", "common_mp.ff", "mp_nuked.ff"});
    withMaps.languages[0].second.insert(withMaps.languages[0].second.end(),
                                        {"en_code_post_gfx_mp.ff", "en_patch_mp.ff", "en_common_mp.ff"});
    const auto nuked = Setup::LevelLoads(withMaps, "mp_nuked");
    CHECK(nuked.has_value());
    if (nuked)
    {
        const std::vector<std::filesystem::path> expected = {
            "zone/English/en_code_post_gfx_mp.ff", "zone/Common/code_post_gfx_mp.ff", "zone/English/en_patch_mp.ff",
            "zone/Common/patch_mp.ff",             "zone/English/en_common_mp.ff",    "zone/Common/common_mp.ff",
            "zone/Common/mp_nuked.ff"};
        CHECK(*nuked == expected);
    }
    CHECK(!Setup::LevelLoads(withMaps, "mp_cairo").has_value());
}

TEST("campaign-kits.json: a kit zone for each teamset, out of a multiplayer map, with its scripts, models and effects")
{
    const auto kits = Setup::ReadKitZones(ReadText(RESOURCES / "campaign-kits.json"));
    CHECK(kits.size() == 4);
    const std::vector<std::pair<std::string, std::string>> expected = {{"t5m_kit_urbanspecops", "mp_nuked"},
                                                                       {"t5m_kit_junglemarines", "mp_havoc"},
                                                                       {"t5m_kit_winterspecops", "mp_array"},
                                                                       {"t5m_kit_cubans", "mp_cairo"}};
    for (const auto& [name, level] : expected)
    {
        const auto kit = std::find_if(kits.begin(), kits.end(), [&](const auto& z) { return z.name == name; });
        CHECK(kit != kits.end());
        if (kit == kits.end())
            continue;
        CHECK(kit->level == level);
        // 5 bodies, 5 heads and 5 arms of each side, both RC-XDs, the bomb and the HQ crate
        CHECK(kit->models.size() == 34);
        CHECK(std::count(kit->models.begin(), kit->models.end(), "t5_veh_rcbomb_allies") == 1);
        CHECK(std::count(kit->models.begin(), kit->models.end(), "mp_supplydrop_hq") == 1);
        const auto scripts = std::count_if(kit->assets.begin(), kit->assets.end(),
                                           [](const auto& a) { return a.first == "rawfile"; });
        CHECK(scripts == 12);
        const auto effects =
            std::count_if(kit->assets.begin(), kit->assets.end(), [](const auto& a) { return a.first == "fx"; });
        CHECK(effects == 26);
    }
    CHECK(Setup::ZoneDefinition("t5m_kit_x", {"a"}, {{"rawfile", "mpbody/standard_mp.gsc"}, {"fx", "b/c"}}) ==
          ">game,T5\n>name,t5m_kit_x\n\nxmodel,a\nrawfile,mpbody/standard_mp.gsc\nfx,b/c\n");
    bool threw = false;
    try
    {
        Setup::ReadKitZones(R"({"version": 1, "kits": [{"zone": "t5m_kit_x", "assets": []}]})");
    }
    catch (const std::runtime_error&)
    {
        threw = true;
    }
    CHECK(threw);
}

TEST("command line arguments are quoted as Windows reads them back")
{
    CHECK(Setup::QuoteArgument(L"t5mp") == L"t5mp");
    CHECK(Setup::QuoteArgument(L"") == L"\"\"");
    CHECK(Setup::QuoteArgument(L"C:\\Games\\Call of Duty Black Ops") == L"\"C:\\Games\\Call of Duty Black Ops\"");
    // backslashes before the closing quote are doubled, the others kept
    CHECK(Setup::QuoteArgument(L"C:\\a folder\\") == L"\"C:\\a folder\\\\\"");
    CHECK(Setup::QuoteArgument(L"say \"hi\"") == L"\"say \\\"hi\\\"\"");
    CHECK(Setup::QuoteArgument(L"a\\\\\"b c") == L"\"a\\\\\\\\\\\"b c\"");
    CHECK(Setup::QuoteArgument(L"C:\\no\\spaces\\") == L"C:\\no\\spaces\\");
}

TEST("the update check: the latest release's tag and page, and whether it is newer")
{
    const auto release = Setup::ReadLatestRelease(
        R"({"tag_name": "v0.2.0", "html_url": "https://github.com/yerbisonline/IrisStudio/releases/tag/v0.2.0",
            "draft": false, "prerelease": false, "assets": []})");
    CHECK(release.has_value());
    if (release)
    {
        CHECK(release->tag == "v0.2.0");
        CHECK(release->url == "https://github.com/yerbisonline/IrisStudio/releases/tag/v0.2.0");
    }
    CHECK(!Setup::ReadLatestRelease(R"({"tag_name": "v0.3.0", "html_url": "https://github.com/x", "prerelease": true})"));
    CHECK(!Setup::ReadLatestRelease(R"({"message": "Not Found"})"));
    CHECK(!Setup::ReadLatestRelease(R"({"tag_name": "nightly", "html_url": "https://github.com/x"})"));
    CHECK(!Setup::ReadLatestRelease(R"({"tag_name": "v1.0.0", "html_url": "https://evil.example/x"})"));
    CHECK(!Setup::ReadLatestRelease("<html>"));

    CHECK(Setup::IsNewerVersion("v0.2.0", "0.1.0"));
    CHECK(Setup::IsNewerVersion("0.1.10", "0.1.9"));
    CHECK(Setup::IsNewerVersion("1.0", "0.9.9"));
    CHECK(!Setup::IsNewerVersion("v0.1.0", "0.1.0"));
    CHECK(!Setup::IsNewerVersion("0.1", "0.1.0"));
    CHECK(!Setup::IsNewerVersion("0.0.9", "0.1.0"));
    CHECK(!Setup::IsNewerVersion("v0.2.0-beta", "0.1.0"));
    CHECK(!Setup::IsNewerVersion("", "0.1.0"));
}

TEST("the setup's summary: downloads, builds, the space needed and the time, as measured")
{
    const auto all = Setup::Estimate({true, true, 272, 7938});
    CHECK(Setup::FormatSize(all.downloadBytes) == "113 MB");
    CHECK(Setup::FormatSize(all.buildBytes) == "481 MB");
    // the downloads unpacked, the zones, and the pack twice while it is put together
    CHECK(Setup::FormatSize(all.spaceBytes) == "804 MB");
    CHECK(Setup::Duration(all.seconds) == "Takes about 3 minutes.");

    // what the user has already isn't counted
    const auto packOnly = Setup::Estimate({false, false, 0, 7938});
    CHECK(packOnly.downloadBytes == 0);
    CHECK(Setup::FormatSize(packOnly.buildBytes) == "213 MB");
    CHECK(Setup::Duration(packOnly.seconds) == "Takes about a minute.");
    const auto nothing = Setup::Estimate({});
    CHECK(nothing.spaceBytes == 0 && nothing.seconds == 0.0);
    CHECK(Setup::Duration(0.0) == "Takes about a minute.");
    CHECK(Setup::Duration(121.0) == "Takes about 2 minutes.");
    CHECK(Setup::Duration(160.0) == "Takes about 3 minutes.");
}

TEST("the palette in the mod's config.json: read, and set with the rest of the file kept")
{
    const std::string config = "{\n    \"keybinds\": {\n        \"F1\": \"toggle\"\n    },\n    \"preferences\": {\n"
                               "        \"palette\": \"steel\",\n        \"uiScale\": 1.25\n    }\n}";
    CHECK(Setup::ModPalette(config) == "steel");
    CHECK(Setup::ModPalette("{\"preferences\": {\"uiScale\": 1}}").empty());
    CHECK(Setup::ModPalette("{\"preferences\": {\"palette\": 3}}").empty());
    CHECK(Setup::ModPalette("not json").empty());

    const auto written = Setup::WithModPalette(config, "mono");
    CHECK(written.has_value());
    if (written)
    {
        CHECK(Setup::ModPalette(*written) == "mono");
        // the rest stays, and the file is written as the mod writes it
        CHECK(written->find("\"F1\": \"toggle\"") != std::string::npos);
        CHECK(written->find("\"uiScale\": 1.25") != std::string::npos);
        CHECK(*written == *Setup::WithModPalette(*written, "mono"));
    }
    CHECK(Setup::ModPalette(*Setup::WithModPalette("{}", "sage")) == "sage");
    CHECK(!Setup::WithModPalette("[1, 2]", "mono").has_value());
}

TEST("zones built from a game are out of date only when a zone they were built from changed size")
{
    const std::vector<Setup::ZoneFile> built = {
        {"Common\\cuba.ff", 100}, {"Common\\patch.ff", 50}, {"English\\en_cuba.ff", 20}};
    // the same zones in another folder, named in another case
    CHECK(!Setup::ZonesChanged(built, {{"common/CUBA.ff", 100}, {"Common\\patch.ff", 50}, {"english\\en_cuba.ff", 20}}));
    // a copy with more map packs, or fewer
    CHECK(!Setup::ZonesChanged(built, {{"Common\\cuba.ff", 100}, {"Common\\patch.ff", 50}, {"English\\en_cuba.ff", 20},
                                       {"Common\\zombie_moon.ff", 900}}));
    CHECK(!Setup::ZonesChanged(built, {{"Common\\cuba.ff", 100}}));
    // a game update
    CHECK(Setup::ZonesChanged(built, {{"Common\\cuba.ff", 101}, {"Common\\patch.ff", 50}, {"English\\en_cuba.ff", 20}}));
    CHECK(!Setup::ZonesChanged({}, built));

    // why: the game was updated, or only the mod's characters are new (the launcher's line says which)
    using Setup::ZonesStale;
    const std::vector<Setup::ZoneFile> updated{{"Common\\cuba.ff", 101}};
    CHECK(Setup::GetZonesStale(built, "", built, "", "catalog 1", "catalog 1") == ZonesStale::No);
    CHECK(Setup::GetZonesStale(built, "", built, "", "catalog 1", "catalog 2") == ZonesStale::NewCharacters);
    CHECK(Setup::GetZonesStale(built, "", updated, "", "catalog 1", "catalog 1") == ZonesStale::GameUpdated);
    CHECK(Setup::GetZonesStale(built, "", updated, "", "catalog 1", "catalog 2") == ZonesStale::GameUpdated);
    // a setup.json of an older launcher: the fingerprint of the game's zones
    CHECK(Setup::GetZonesStale({}, "f1", built, "f1", "catalog 1", "catalog 2") == ZonesStale::NewCharacters);
    CHECK(Setup::GetZonesStale({}, "f1", built, "f2", "catalog 1", "catalog 1") == ZonesStale::GameUpdated);
}

TEST("the zone files character zones are built from: what the Linker loads for their levels, no others")
{
    Setup::GameZones zones;
    zones.common = {"common.ff", "cuba.ff", "patch.ff", "patch_mp.ff", "mp_nuked.ff"};
    zones.languages = {{"English", {"en_common.ff", "en_cuba.ff", "en_patch.ff", "en_patch_mp.ff"}}};
    const std::vector<Setup::ZoneFile> files = {
        {"Common\\common.ff", 1}, {"Common\\cuba.ff", 2},        {"Common\\patch.ff", 3},
        {"Common\\patch_mp.ff", 4}, {"Common\\mp_nuked.ff", 5},  {"English\\en_common.ff", 6},
        {"English\\en_cuba.ff", 7}, {"English\\en_patch.ff", 8}, {"English\\en_patch_mp.ff", 9}};
    const auto used = Setup::FilesBuiltFrom(zones, {"cuba"}, files);
    std::set<std::string> paths;
    for (const auto& file : used)
        paths.insert(file.path);
    CHECK(paths == (std::set<std::string>{"Common\\common.ff", "Common\\cuba.ff", "Common\\patch.ff",
                                          "English\\en_common.ff", "English\\en_cuba.ff", "English\\en_patch.ff"}));
    // so another copy whose multiplayer patch differs (as Steam's and the user's do) has the same zones
    auto other = files;
    other.back().size = 99;
    CHECK(!Setup::ZonesChanged(used, other));
    CHECK(Setup::FilesBuiltFrom(zones, {"zombie_moon"}, files).empty());
}

TEST("setup: an older textures file hasn't all the catalog's images")
{
    const std::string end = "..images/Hk21_c.iwi..images/b.iwi..";
    CHECK(Setup::NamesAllImages(end, {"hk21_c", "b"}));
    CHECK(!Setup::NamesAllImages(end, {"hk21_c", "b", "char_jap_impinf_body5z_1_c"}));
    CHECK(Setup::NamesAllImages("", {}));
}
