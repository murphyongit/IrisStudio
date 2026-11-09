#include "StdInclude.hpp"
#include "Test.hpp"

#include "Components/CampaignMapLogic.hpp"
#include "Utilities/Inflate.hpp"

using namespace IWXMVM;

// Campaign levels loaded as multiplayer maps (CampaignMapLogic): the levels and their
// teamsets, the catalog, the entities rewritten for multiplayer from pentagon's and cuba's own (excerpts of their entity
// strings, in data), the spawn points, the stand-in scripts and the room a level leaves

namespace
{
    namespace Maps = Components::CampaignMapLogic;

    const std::filesystem::path DATA = std::filesystem::path(__FILE__).parent_path() / "data";
    // the catalog that comes with the mod (tools/t5-maps)
    const std::filesystem::path SHIPPED_CATALOG =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / "core" / "resources" /
        "campaign-maps.json";

    std::string ReadText(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), {});
    }

    std::vector<Maps::Entity> OfClass(const std::vector<Maps::Entity>& entities, std::string_view prefix)
    {
        std::vector<Maps::Entity> found;
        for (const auto& entity : entities)
            if (const auto classname = Maps::FindKey(entity, "classname"); classname && classname->starts_with(prefix))
                found.push_back(entity);
        return found;
    }

    bool Near(const glm::vec3& a, const glm::vec3& b)
    {
        return std::abs(a.x - b.x) < 0.01f && std::abs(a.y - b.y) < 0.01f && std::abs(a.z - b.z) < 0.01f;
    }

    constexpr std::string_view SMALL_CATALOG = R"({
        "version": 1,
        "levels": [
            {"name": "cuba", "title": "Operation 40", "teamset": "cubans", "fog": "204.799, 3014.41, 513.74", "vision": "cuba",
             "new": {"image": 1660, "material": 1748, "xmodel": 443},
             "world": {"image": ["cuba_sky", "~cuba_wall_c"], "xmodel": ["p_cub_car"], "material": ["wc/cub_wall"]},
             "closedDoors": ["fake_door", "pit_door"]},
            {"name": "wmd", "teamset": "winterspecops", "world": {}}
        ]
    })";
}  // namespace

TEST("campaign maps: teamsets are named as the game's scripts, each with its kit zone")
{
    CHECK(Maps::GetTeamsetName(Maps::Teamset::UrbanSpecOps) == "urbanspecops");
    CHECK(Maps::GetTeamsetName(Maps::Teamset::JungleMarines) == "junglemarines");
    CHECK(Maps::GetTeamsetName(Maps::Teamset::WinterSpecOps) == "winterspecops");
    CHECK(Maps::GetTeamsetName(Maps::Teamset::Cubans) == "cubans");
    CHECK(Maps::FindTeamset("cubans") == Maps::Teamset::Cubans);
    CHECK(!Maps::FindTeamset("spetsnaz"));
    CHECK(Maps::GetKitZone(Maps::Teamset::WinterSpecOps) == "t5m_kit_winterspecops");
    CHECK(Maps::GetWorldName("cuba") == "maps/cuba.d3dbsp");
    CHECK(Maps::GetMultiplayerWorldName("cuba") == "maps/mp/cuba.d3dbsp");
}

TEST("campaign maps: the catalog has the levels and their worlds")
{
    const auto catalog = Maps::ReadCatalog(SMALL_CATALOG);
    CHECK(catalog.levels.size() == 2);
    const auto cuba = catalog.FindLevel("cuba");
    CHECK(cuba && cuba->title == "Operation 40" && cuba->teamset == Maps::Teamset::Cubans);
    CHECK(cuba && cuba->fog == "204.799, 3014.41, 513.74" && cuba->vision == "cuba");
    CHECK(cuba && cuba->newImages == 1660 && cuba->newMaterials == 1748 && cuba->newModels == 443);
    CHECK(cuba && cuba->world.at("image").contains("~cuba_wall_c") && cuba->world.at("xmodel").size() == 1);
    CHECK(cuba && cuba->closedDoors.size() == 2 && cuba->closedDoors.contains("pit_door"));
    const auto wmd = catalog.FindLevel("wmd");
    CHECK(wmd && wmd->title == "wmd" && wmd->fog.empty() && wmd->world.empty() && wmd->closedDoors.empty());

    bool threw = false;
    try
    {
        Maps::ReadCatalog(R"({"version": 2, "levels": []})");
    }
    catch (const std::runtime_error&)
    {
        threw = true;
    }
    CHECK(threw);
}

TEST("campaign maps: a level's zone and its language twin are the level, other zones aren't")
{
    const auto catalog = Maps::ReadCatalog(SMALL_CATALOG);
    CHECK(Maps::FindLevelOfZone(catalog, "cuba") == catalog.FindLevel("cuba"));
    CHECK(Maps::FindLevelOfZone(catalog, "en_cuba") == catalog.FindLevel("cuba"));
    CHECK(!Maps::FindLevelOfZone(catalog, "mp_cairo"));
    CHECK(!Maps::FindLevelOfZone(catalog, "en_patch_mp"));
    CHECK(!Maps::FindLevelOfZone(catalog, "cuba_patch"));
}

TEST("campaign maps: the shipped catalog has the 18 campaign levels with their teamsets")
{
    const auto catalog = Maps::ReadCatalog(ReadText(SHIPPED_CATALOG));
    CHECK(std::ranges::count(catalog.levels, Maps::Mode::Campaign, &Maps::Level::mode) == 18);
    const std::map<std::string, Maps::Teamset> teamsets{
        {"pentagon", Maps::Teamset::UrbanSpecOps},  {"kowloon", Maps::Teamset::UrbanSpecOps},
        {"int_escape", Maps::Teamset::UrbanSpecOps}, {"frontend", Maps::Teamset::UrbanSpecOps},
        {"outro", Maps::Teamset::UrbanSpecOps},      {"rebirth", Maps::Teamset::UrbanSpecOps},
        {"underwaterbase", Maps::Teamset::UrbanSpecOps}, {"flashpoint", Maps::Teamset::UrbanSpecOps},
        {"creek_1", Maps::Teamset::JungleMarines},   {"khe_sanh", Maps::Teamset::JungleMarines},
        {"hue_city", Maps::Teamset::JungleMarines},  {"river", Maps::Teamset::JungleMarines},
        {"pow", Maps::Teamset::JungleMarines},       {"vorkuta", Maps::Teamset::WinterSpecOps},
        {"wmd", Maps::Teamset::WinterSpecOps},       {"wmd_sr71", Maps::Teamset::WinterSpecOps},
        {"fullahead", Maps::Teamset::WinterSpecOps}, {"cuba", Maps::Teamset::Cubans}};
    for (const auto& [name, teamset] : teamsets)
    {
        const auto level = catalog.FindLevel(name);
        CHECK(level && level->teamset == teamset);
    }
    // what the research measured of the worlds, with what the ambient effects take (3)
    const auto cuba = catalog.FindLevel("cuba");
    CHECK(cuba && cuba->newImages == 1674 && cuba->newMaterials == 1766 && cuba->newModels == 443 &&
          cuba->newEffects == 49);
    CHECK(cuba && cuba->vision == "cuba" && !cuba->fog.empty());
    const auto pentagon = catalog.FindLevel("pentagon");
    CHECK(pentagon && pentagon->newImages == 1264 && pentagon->world.at("image").size() > 1200);
    // their createfx: every definition placed is registered with the world; outro has none
    CHECK(pentagon && pentagon->effects.size() == 272 && pentagon->effectKinds.size() == 19);
    CHECK(catalog.FindLevel("wmd")->effects.size() == 612 && catalog.FindLevel("outro")->effects.empty());
    for (const auto& level : catalog.levels)
        for (const auto& definition : level.effectKinds)
            CHECK(level.world.at("fx").contains(definition));
}

TEST("campaign maps: the shipped catalog has the 10 Zombies maps, read as campaign levels are")
{
    const auto catalog = Maps::ReadCatalog(ReadText(SHIPPED_CATALOG));
    CHECK(std::ranges::count(catalog.levels, Maps::Mode::Zombies, &Maps::Level::mode) == 10);
    // the teamset of their setting: snow winter spec ops, jungle and swamp jungle marines
    const std::map<std::string, std::pair<std::string, Maps::Teamset>> zombies{
        {"zombie_theater", {"Kino der Toten", Maps::Teamset::UrbanSpecOps}},
        {"zombie_pentagon", {"Five", Maps::Teamset::UrbanSpecOps}},
        {"zombie_cosmodrome", {"Ascension", Maps::Teamset::UrbanSpecOps}},
        {"zombie_coast", {"Call of the Dead", Maps::Teamset::WinterSpecOps}},
        {"zombie_temple", {"Shangri-La", Maps::Teamset::JungleMarines}},
        {"zombie_moon", {"Moon", Maps::Teamset::UrbanSpecOps}},
        {"zombie_cod5_prototype", {"Nacht der Untoten", Maps::Teamset::UrbanSpecOps}},
        {"zombie_cod5_asylum", {"Verruckt", Maps::Teamset::UrbanSpecOps}},
        {"zombie_cod5_sumpf", {"Shi No Numa", Maps::Teamset::JungleMarines}},
        {"zombie_cod5_factory", {"Der Riese", Maps::Teamset::WinterSpecOps}}};
    for (const auto& [name, look] : zombies)
    {
        const auto level = catalog.FindLevel(name);
        CHECK(level && level->mode == Maps::Mode::Zombies && level->title == look.first &&
              level->teamset == look.second && !level->effects.empty());
        CHECK(level && Maps::FindLevelOfZone(catalog, "en_" + name) == level);
    }
    CHECK(Maps::GetModeName(Maps::Mode::Zombies) == "Zombies map");
    CHECK(Maps::GetModeName(catalog.FindLevel("cuba")->mode) == "campaign level");
    // Five as the build measured it; the vision Zombies mode gives a map whose art script sets none (Ascension's), and
    // none of the commented-out calls (Verruckt's art script keeps a "cheat_bw" one)
    const auto five = catalog.FindLevel("zombie_pentagon");
    CHECK(five && five->newImages == 1416 && five->effects.size() == 372 && five->vision == "zombie_pentagon" &&
          !five->fog.empty());
    CHECK(catalog.FindLevel("zombie_cosmodrome")->vision == "zombie_cosmodrome_nopower");
    CHECK(catalog.FindLevel("zombie_cod5_asylum")->vision == "zombie_asylum");
    // Nacht's definitions are loaded with a space before the bracket ("loadfx (")
    CHECK(catalog.FindLevel("zombie_cod5_prototype")->effectKinds.size() == 19);
    // a zone that only has the level's name in it isn't the level's
    CHECK(!Maps::FindLevelOfZone(catalog, "zombie_pentagon_patch") && !Maps::FindLevelOfZone(catalog, "common_zombie"));
    // common_zombie loads before each (they take its effects' materials), no zone before a campaign level; not Dead Ops
    // Arcade, a top-down mode left out
    CHECK(five && five->zones == std::vector<std::string>{"common_zombie"});
    CHECK(!catalog.FindLevel("zombietron"));
    CHECK(std::ranges::count_if(catalog.levels, [](const Maps::Level& level) { return !level.zones.empty(); }) == 10);
    // with the patches and language twins the game loads with them (not the level's own zones)
    for (const auto zone : {"common_zombie", "en_common_zombie", "common_zombie_patch", "zombie_pentagon_patch"})
        CHECK(Maps::IsLoadedWith(*five, zone) && Maps::IsLoadedWithAny(catalog, zone));
    for (const auto zone : {"zombie_pentagon", "en_zombie_pentagon", "zombie_theater_patch", "common_mp", "patch_mp",
                            "common_zombie_patch2", "en_patch"})
        CHECK(!Maps::IsLoadedWith(*five, zone));
    CHECK(!Maps::IsLoadedWithAny(catalog, "common_mp") && !Maps::IsLoadedWithAny(catalog, "mp_nuked_patch"));
    // a film of a map the game folder doesn't have is refused in plain words, naming it as the menus do
    CHECK(Maps::GetMissingMapRefusal(*catalog.FindLevel("zombie_moon")) ==
          "It was recorded on Moon (zombie_moon), a Zombies map this game folder doesn't have. Add the map's files to "
          "the game folder to play it.");

    bool threw = false;
    try
    {
        Maps::ReadCatalog(R"({"version": 1, "levels": [{"name": "x", "mode": "survival", "teamset": "cubans",
                              "world": {}}]})");
    }
    catch (const std::runtime_error&)
    {
        threw = true;
    }
    CHECK(threw);
}

TEST("campaign maps: entity strings are read as the game reads them")
{
    const auto entities = Maps::ParseEntities(
        "{\n\"classname\" \"worldspawn\"\n\"skyboxmodel\" \"skybox_sp_pentagon\"\n}\n"
        "{\n\"origin\" \"1 2 3\"\n\"classname\" \"script_model\"\n\"model\" \"p_pent_chair\"\n}\n");
    CHECK(entities.size() == 2);
    CHECK(entities[1].size() == 3 && *Maps::FindKey(entities[1], "model") == "p_pent_chair");
    CHECK(!Maps::FindKey(entities[0], "origin"));
    const auto pentagon = Maps::ParseEntities(ReadText(DATA / "campaign-ents-pentagon.txt"));
    CHECK(pentagon.size() == 23);
    const auto cuba = Maps::ParseEntities(ReadText(DATA / "campaign-ents-cuba.txt"));
    CHECK(cuba.size() == 25);
}

TEST("campaign maps: an entity's position that came a whole range away is back near its player")
{
    // wmd's chair: at y -29923 in the level, read as 35613, with the player in the same room
    const glm::vec3 player(11950, -29850, 38361);
    CHECK(Near(Maps::UnwrapOrigin({12074, 35613, 38352}, player), glm::vec3(12074, -29923, 38352)));
    // what's where it was sent stays, the other way round too
    CHECK(Near(Maps::UnwrapOrigin({12074, -29923, 38352}, player), glm::vec3(12074, -29923, 38352)));
    CHECK(Near(Maps::UnwrapOrigin({-40000, 10, 5}, {30000, 0, 0}), glm::vec3(25536, 10, 5)));
    // on any axis, and the fraction kept
    CHECK(Near(Maps::UnwrapOrigin({10.5f, 20.25f, 70000}, {0, 0, 5000}), glm::vec3(10.5f, 20.25f, 4464)));
    // a multiplayer map has nothing a range away
    CHECK(Near(Maps::UnwrapOrigin({2000, -2000, 100}, {-1500, 1500, 0}), glm::vec3(2000, -2000, 100)));

    // what the level placed is found however far off it stands (river shows 90 of 210 so), the network's whole units allowed for
    const std::vector<glm::vec3> origins{{40000, 0, 0}, {-3, 7, 9}, {12000, 80000, 38000}};
    const Maps::LevelOrigins placed(origins);
    CHECK(placed.Find({40000.4f, 0.6f, -0.4f}, {}).has_value() && placed.Find({-3, 7, 9}, {}).has_value());
    CHECK(!placed.Find({40000, 0, 5}, {}).has_value() && !placed.Find({0, 0, 0}, {}).has_value());
    CHECK(Near(Maps::UnwrapOrigin({40000, 0, 0}, {0, 0, 0}, placed), glm::vec3(40000, 0, 0)));
    CHECK(Near(Maps::UnwrapOrigin({-25536, 0, 0}, {0, 0, 0}, placed), glm::vec3(40000, 0, 0)));
    CHECK(Near(Maps::UnwrapOrigin({12000, 14464, -27536}, {0, 0, 0}, placed), glm::vec3(12000, 80000, 38000)));
    CHECK(Near(Maps::UnwrapOrigin({40000, 10, 0}, {0, 0, 0}, placed), glm::vec3(-25536, 10, 0)));

    // two a range apart: the one nearest the player
    const std::vector<glm::vec3> twins{{0, 100, 0}, {65536, 100, 0}};
    const Maps::LevelOrigins both(twins);
    CHECK(Near(*both.Find({0, 100, 0}, {60000, 0, 0}), glm::vec3(65536, 100, 0)));
    CHECK(Near(*both.Find({0, 100, 0}, {100, 0, 0}), glm::vec3(0, 100, 0)));

    // at the edge of the range
    const std::vector<glm::vec3> edge{{65535.5f, 0, 0}};
    CHECK(Maps::LevelOrigins(edge).Find({0.5f, 0, 0}, {}).has_value());
}

TEST("campaign maps: a Zombies door is what its trigger names, its other half and clip with it, and every door is open")
{
    const auto text =
        "{\n\"classname\" \"worldspawn\"\n}\n"
        "{\n\"classname\" \"trigger_use\"\n\"targetname\" \"zombie_door\"\n\"target\" \"foyer_door\"\n}\n"
        "{\n\"classname\" \"trigger_use_touch\"\n\"targetname\" \"zombie_debris\"\n\"target\" \"pile_1\"\n}\n"
        "{\n\"classname\" \"script_brushmodel\"\n\"targetname\" \"foyer_door\"\n\"model\" \"*1\"\n\"origin\" \"1 2 3\"\n}\n"
        "{\n\"classname\" \"script_brushmodel\"\n\"targetname\" \"foyer_door\"\n\"model\" \"*2\"\n\"script_noteworthy\" \"clip\"\n}\n"
        "{\n\"classname\" \"script_model\"\n\"targetname\" \"pile_1\"\n\"model\" \"p_rubble\"\n\"origin\" \"4 5 6\"\n}\n"
        "{\n\"classname\" \"script_model\"\n\"targetname\" \"chair\"\n\"model\" \"p_chair\"\n}\n"
        "{\n\"classname\" \"script_model\"\n\"targetname\" \"mansion_gate_left\"\n\"model\" \"p_gate\"\n}\n"
        "{\n\"classname\" \"script_model\"\n\"targetname\" \"destructible\"\n\"model\" \"dest_door\"\n}\n"
        "{\n\"classname\" \"script_model\"\n\"targetname\" \"pf12_auto3\"\n\"model\" \"p_cub_door01_wood\"\n}\n";
    const auto doors = Maps::FindDoors(text);
    CHECK(doors.size() == 4);
    CHECK(doors[0].name == "foyer_door" && doors[0].zombies && doors[0].pieces == 2 && Near(doors[0].at, glm::vec3(1, 2, 3)));
    CHECK(doors[1].name == "pile_1" && doors[1].zombies && doors[1].pieces == 1);
    // on a campaign level a name or a model tells, for a person to look at
    CHECK(doors[2].name == "mansion_gate_left" && !doors[2].zombies);
    CHECK(doors[3].name == "pf12_auto3" && !doors[3].zombies);

    // by default every door is open, all of its pieces, and what isn't a door stays
    const auto open = Maps::RewriteEntities(text);
    CHECK(open.doorPiecesLeftOut == 5 && open.doors.size() == 4);
    const auto openOut = Maps::ParseEntities(open.entities);
    CHECK(OfClass(openOut, "script_brushmodel").empty());
    const auto models = OfClass(openOut, "script_model");
    CHECK(models.size() == 2);
    CHECK(*Maps::FindKey(models[0], "targetname") == "chair" && *Maps::FindKey(models[1], "targetname") == "destructible");

    // closed as the mission has them, only for a test to compare
    const auto closed = Maps::RewriteEntities(text, {}, {.doors = Maps::Doors::Closed});
    CHECK(closed.doorPiecesLeftOut == 0 && closed.doors.size() == 4);
    const auto closedOut = Maps::ParseEntities(closed.entities);
    CHECK(OfClass(closedOut, "script_brushmodel").size() == 2 && OfClass(closedOut, "script_model").size() == 5);

    // a door that opens into nothing (the level's closedDoors) stays, unless the harness measures them all
    const auto kept = Maps::RewriteEntities(text, {}, {.keptDoors = {"foyer_door"}});
    CHECK(kept.doorPiecesLeftOut == 3 && kept.doors.size() == 4);
    CHECK(!kept.doors[0].open && kept.doors[1].open && kept.doors[3].open);
    CHECK(OfClass(Maps::ParseEntities(kept.entities), "script_brushmodel").size() == 2);
    const auto all = Maps::RewriteEntities(text, {}, {.doors = Maps::Doors::AllOpen, .keptDoors = {"foyer_door"}});
    CHECK(all.doorPiecesLeftOut == 5 && all.doors[0].open);
    CHECK(std::ranges::none_of(closed.doors, &Maps::Door::open));
}

TEST("campaign maps: a door is a model of a door or a brush model named so, and a blocker or a prop by a door stays")
{
    // vorkuta's wall switch and the shotguns on the floor by a broken door, creek_1's thatch roof, a blocker's rubble
    // and an invisible blocker stay; the doors, their clips (a prefab's by the door's name) and a gate's lock go
    const auto text =
        "{\n\"classname\" \"worldspawn\"\n}\n"
        "{\n\"classname\" \"script_model\"\n\"targetname\" \"door_button_before\"\n\"model\" \"p_rus_electric_switch_stop_obj\"\n}\n"
        "{\n\"classname\" \"script_model\"\n\"targetname\" \"sergei_weapon_door_dmg\"\n\"model\" \"t5_weapon_ks23_world\"\n}\n"
        "{\n\"classname\" \"script_brushmodel\"\n\"targetname\" \"sergei_weapon_door_dmg\"\n\"model\" \"*3\"\n}\n"
        "{\n\"classname\" \"script_model\"\n\"targetname\" \"roof_door\"\n\"model\" \"anim_jun_roof_thatch\"\n}\n"
        "{\n\"classname\" \"script_model\"\n\"targetname\" \"mansion_wood_blocker\"\n\"model\" \"p_jun_rubble03\"\n}\n"
        "{\n\"classname\" \"script_brushmodel\"\n\"targetname\" \"mansion_wood_blocker\"\n\"model\" \"*4\"\n}\n"
        "{\n\"classname\" \"script_brushmodel\"\n\"targetname\" \"weaver_door_blocker\"\n\"model\" \"*5\"\n}\n"
        "{\n\"classname\" \"script_model\"\n\"targetname\" \"gate_lock\"\n\"model\" \"p_rus_gate_lock_obj\"\n}\n"
        "{\n\"classname\" \"script_model\"\n\"targetname\" \"rope_entrance\"\n\"model\" \"p_rus_door_white_plain_right\"\n}\n"
        "{\n\"classname\" \"script_model\"\n\"targetname\" \"pf1344_auto314\"\n\"model\" \"p_zom_moon_lab_airlock_door01_left_locked\"\n}\n"
        "{\n\"classname\" \"script_brushmodel\"\n\"targetname\" \"pf1344_auto314\"\n\"model\" \"*6\"\n}\n";
    const auto doors = Maps::FindDoors(text);
    CHECK(doors.size() == 5);
    if (doors.size() != 5)
        return;
    CHECK(doors[0].name == "sergei_weapon_door_dmg" && doors[0].pieces == 1);
    CHECK(doors[1].name == "weaver_door_blocker" && doors[2].name == "gate_lock" && doors[3].name == "rope_entrance");
    CHECK(doors[4].name == "pf1344_auto314" && doors[4].pieces == 2);

    const auto rewrite = Maps::RewriteEntities(text);
    CHECK(rewrite.doorPiecesLeftOut == 6);
    const auto out = Maps::ParseEntities(rewrite.entities);
    const auto models = OfClass(out, "script_model");
    CHECK(models.size() == 4);
    if (models.size() != 4)
        return;
    CHECK(*Maps::FindKey(models[0], "targetname") == "door_button_before");
    CHECK(*Maps::FindKey(models[1], "model") == "t5_weapon_ks23_world");
    CHECK(*Maps::FindKey(models[2], "targetname") == "roof_door");
    CHECK(*Maps::FindKey(models[3], "targetname") == "mansion_wood_blocker");
    const auto brushes = OfClass(out, "script_brushmodel");
    CHECK(brushes.size() == 1 && *Maps::FindKey(brushes[0], "targetname") == "mansion_wood_blocker");
}

TEST("campaign maps: flickering lights flicker, and burn steadily only for a test, other types as they are")
{
    const auto text =
        "{\n\"classname\" \"worldspawn\"\n}\n"
        "{\n\"classname\" \"light\"\n\"targetname\" \"fire_flicker\"\n\"intensity\" \"15\"\n}\n"
        "{\n\"classname\" \"light\"\n\"targetname\" \"generic_pulsing\"\n}\n"
        "{\n\"classname\" \"script_model\"\n\"targetname\" \"fire_flicker\"\n\"model\" \"p_x\"\n}\n";
    const auto mission = Maps::RewriteEntities(text);
    CHECK(mission.lightsMadeSteady == 0);
    CHECK(mission.entities.find("light_solid") == std::string::npos);

    const auto steady = Maps::RewriteEntities(text, {}, {.steadyLights = true});
    CHECK(steady.lightsMadeSteady == 1);
    const auto lights = OfClass(Maps::ParseEntities(steady.entities), "light");
    CHECK(lights.size() == 2);
    CHECK(*Maps::FindKey(lights[0], "targetname") == "light_solid" && *Maps::FindKey(lights[0], "intensity") == "15");
    CHECK(*Maps::FindKey(lights[1], "targetname") == "generic_pulsing");
    CHECK(*Maps::FindKey(OfClass(Maps::ParseEntities(steady.entities), "script_model")[0], "targetname") == "fire_flicker");
}

TEST("campaign maps: a vehicle stays only if it stands there from the start")
{
    const auto vehicle = [](const char* keys) {
        return Maps::ParseEntities(std::format("{{\n\"classname\" \"script_vehicle\"\n{}}}\n", keys)).at(0);
    };
    // the ship's deck guns and the train on underwaterbase and vorkuta
    CHECK(Maps::IsParkedVehicle(vehicle("\"vehicletype\" \"wpn_zpu_antiair\"\n\"targetname\" \"aa_gun\"\n")));
    CHECK(Maps::IsParkedVehicle(vehicle("\"vehicletype\" \"train_car_boxcar\"\n\"spawnflags\" \"0\"\n")));
    // no one key tells them: wmd has spawn groups and no flag, cuba flags all of its, some have a path alone
    CHECK(!Maps::IsParkedVehicle(vehicle("\"vehicletype\" \"jeep\"\n\"script_vehiclespawngroup\" \"1\"\n")));
    CHECK(!Maps::IsParkedVehicle(vehicle("\"vehicletype\" \"truck\"\n\"spawnflags\" \"2\"\n")));
    CHECK(!Maps::IsParkedVehicle(vehicle("\"vehicletype\" \"truck\"\n\"spawnflags\" \"3\"\n")));
    CHECK(!Maps::IsParkedVehicle(vehicle("\"vehicletype\" \"apc\"\n\"target\" \"auto1\"\n")));
    // nothing marks hue_city's opening helicopters, at 9,000 units
    CHECK(!Maps::IsParkedVehicle(vehicle("\"vehicletype\" \"heli_huey_assault\"\n\"spawnflags\" \"1\"\n")));
    CHECK(!Maps::IsParkedVehicle(vehicle("\"vehicletype\" \"plane_phantom_gearup_lowres\"\n")));
    CHECK(!Maps::IsParkedVehicle(vehicle("\"vehicletype\" \"misc_freefall\"\n")));

    const auto rewrite = Maps::RewriteEntities(
        "{\n\"classname\" \"worldspawn\"\n}\n"
        "{\n\"classname\" \"script_vehicle\"\n\"vehicletype\" \"wpn_zpu_antiair\"\n\"model\" \"vehicle_zpu4\"\n"
        "\"origin\" \"1 2 3\"\n\"spawnflags\" \"0\"\n}\n"
        "{\n\"classname\" \"script_vehicle\"\n\"vehicletype\" \"jeep\"\n\"model\" \"t5_veh_jeep\"\n"
        "\"script_vehiclespawngroup\" \"2\"\n}\n");
    const auto out = Maps::ParseEntities(rewrite.entities);
    CHECK(rewrite.vehiclesKept == 1 && rewrite.vehiclesLeftOut == 1);
    const auto models = OfClass(out, "script_model");
    CHECK(models.size() == 1 && *Maps::FindKey(models[0], "model") == "vehicle_zpu4");
    CHECK(!Maps::FindKey(models[0], "spawnflags") && !Maps::FindKey(models[0], "vehicletype"));
    CHECK(OfClass(out, "script_vehicle").empty());
}

TEST("campaign maps: pentagon's entities lose the AI, nodes and triggers and its traffic, players spawn at its start")
{
    const auto rewrite = Maps::RewriteEntities(ReadText(DATA / "campaign-ents-pentagon.txt"));
    const auto out = Maps::ParseEntities(rewrite.entities);
    // kept: worldspawn, 2 structs, an origin, a model, a brush model, a light; the 2 cars are traffic a script drives,
    // and the security doors' 2 halves open
    CHECK(rewrite.kept == 7);
    CHECK(rewrite.dropped == 16);
    CHECK(rewrite.doorPiecesLeftOut == 2);
    CHECK(rewrite.vehiclesKept == 0 && rewrite.vehiclesLeftOut == 2);
    CHECK(OfClass(out, "worldspawn").size() == 1);
    CHECK(*Maps::FindKey(OfClass(out, "worldspawn")[0], "skyboxmodel") == "skybox_sp_pentagon");
    // a mounted gun stays as its picture, and the spare keys of what it was go
    const auto guns = Maps::RewriteEntities(
        "{\n\"classname\" \"misc_turret\"\n\"model\" \"t5_weapon_m60e3_MG\"\n\"origin\" \"1 2 3\"\n\"target\" \"x\"\n}\n");
    const auto gunEntities = Maps::ParseEntities(guns.entities);
    CHECK(OfClass(gunEntities, "misc_turret").empty() && OfClass(gunEntities, "script_model").size() == 1);
    CHECK(guns.entities.find("target") == std::string::npos);

    for (const auto prefix : {"actor_", "node_", "trigger_", "info_", "lod_info", "script_vehicle", "misc_turret"})
        CHECK(OfClass(out, prefix).empty());
    CHECK(OfClass(out, "script_model").size() == 1);
    CHECK(OfClass(out, "script_brushmodel").size() == 1 && OfClass(out, "light").size() == 1);

    // its start, and the spawn points of every game type there and at its path nodes
    CHECK(Near(rewrite.start, glm::vec3(-50162.5f, -33283.5f, 437.0f)));
    CHECK(std::abs(rewrite.startYaw - 60.2f) < 0.001f);
    CHECK(!rewrite.spawnPoints.empty() && Near(rewrite.spawnPoints.front(), rewrite.start));
    CHECK(rewrite.spawns == 19 * rewrite.spawnPoints.size());
    const auto tdm = OfClass(out, "mp_tdm_spawn_allies_start");
    CHECK(tdm.size() == rewrite.spawnPoints.size());
    CHECK(*Maps::FindKey(tdm[0], "origin") == "-50162.5 -33283.5 437");
    CHECK(*Maps::FindKey(tdm[0], "angles") == "0 60.2 0");
    CHECK(OfClass(out, "mp_dm_spawn").size() == rewrite.spawnPoints.size());
    CHECK(OfClass(out, "mp_sd_spawn_defender").size() == rewrite.spawnPoints.size());
    const auto intermission = OfClass(out, "mp_global_intermission");
    CHECK(intermission.size() == 1);
    CHECK(*Maps::FindKey(intermission[0], "origin") == "-50162.5 -33283.5 517");
    CHECK(rewrite.traversals.empty());
}

TEST("campaign maps: cuba's destructibles become plain models and its path nodes' traversals are listed")
{
    const auto text = ReadText(DATA / "campaign-ents-cuba.txt");
    const auto rewrite = Maps::RewriteEntities(text);
    const auto out = Maps::ParseEntities(rewrite.entities);
    for (const auto& entity : out)
        CHECK(!Maps::FindKey(entity, "destructibledef"));
    for (const auto prefix : {"rope", "weapon_", "spawn_manager", "trigger_", "actor_", "node_", "info_vehicle"})
        CHECK(OfClass(out, prefix).empty());
    // (its truck and police car are a script's, see IsParkedVehicle)
    CHECK(OfClass(out, "script_model").size() == 3);
    CHECK(rewrite.vehiclesKept == 0 && rewrite.vehiclesLeftOut == 2);
    CHECK((rewrite.traversals == std::set<std::string>{"mantle_over_36", "stairs_down_8x12"}));
    CHECK(Near(rewrite.start, glm::vec3(-16019.5f, -1475.2f, -872.0f)));
    // its start and its 6 path nodes, all more than 64 units apart, nearest first
    CHECK(rewrite.spawnPoints.size() == 7);
    CHECK(Near(rewrite.spawnPoints[1], glm::vec3(-15876.6f, -1417.9f, -920.0f)));
    CHECK(rewrite.spawns == 19 * 7);

    // a flickering light keeps its light, not the models it switches between (multiplayer's client script would set
    // them unprecached)
    const auto light = Maps::ParseEntities(Maps::RewriteEntities(
        "{\n\"classname\" \"light\"\n\"origin\" \"1 2 3\"\n\"script_light_model\" \"lab_light\"\n"
        "\"script_light_on_model\" \"p_rus_rb_lab_warning_light_01\"\n\"script_light_off_model\" \"p_off\"\n"
        "\"script_wait_min\" \"0.1\"\n}\n").entities);
    CHECK(OfClass(light, "light").size() == 1);
    CHECK(!Maps::FindKey(OfClass(light, "light")[0], "script_light_model") &&
          !Maps::FindKey(OfClass(light, "light")[0], "script_light_on_model") &&
          !Maps::FindKey(OfClass(light, "light")[0], "script_light_off_model"));
    CHECK(Maps::FindKey(OfClass(light, "light")[0], "script_wait_min"));

    // models multiplayer won't have are left out
    const auto missing = *Maps::FindKey(OfClass(out, "script_model")[0], "model");
    const auto without = Maps::RewriteEntities(text, [&](const std::string& model) { return model != missing; });
    CHECK(without.dropped > rewrite.dropped && without.kept + without.dropped == rewrite.kept + rewrite.dropped);
    CHECK(without.entities.find(missing) == std::string::npos);

    // but the world stays the first entity, without the model it names when multiplayer won't have that (Five's
    // worldspawn names p_rus_mop; the game stops at a first entity that isn't the world)
    const auto five = Maps::RewriteEntities(
        "{\n\"skyboxmodel\" \"skybox_sp_frontend\"\n\"classname\" \"worldspawn\"\n\"model\" \"p_rus_mop\"\n}\n"
        "{\n\"classname\" \"script_model\"\n\"origin\" \"1 2 3\"\n\"model\" \"p_rus_mop\"\n}\n",
        [](const std::string& model) { return model != "p_rus_mop"; });
    const auto fiveOut = Maps::ParseEntities(five.entities);
    CHECK(five.kept == 1 && five.dropped == 1 && !fiveOut.empty());
    CHECK(*Maps::FindKey(fiveOut[0], "classname") == "worldspawn" && !Maps::FindKey(fiveOut[0], "model") &&
          *Maps::FindKey(fiveOut[0], "skyboxmodel") == "skybox_sp_frontend");
}

TEST("campaign maps: spawn points are the start, then the nearest path nodes apart from those before")
{
    const glm::vec3 start(0.0f, 0.0f, 0.0f);
    // too near the start, right above another, far, and twenty in a row 100 apart
    std::vector<glm::vec3> nodes{{30.0f, 0.0f, 0.0f}, {5000.0f, 0.0f, 0.0f}, {100.0f, 0.0f, 0.0f}, {100.0f, 0.0f, 200.0f}};
    for (int i = 2; i < 22; ++i)
        nodes.emplace_back(0.0f, 100.0f * i, 0.0f);
    const auto points = Maps::ChooseSpawnPoints(start, nodes);
    CHECK(points.size() == Maps::SPAWN_POINTS);
    CHECK(Near(points[0], start));
    CHECK(Near(points[1], glm::vec3(100.0f, 0.0f, 0.0f)));
    CHECK(Near(points[2], glm::vec3(0.0f, 200.0f, 0.0f)));
    CHECK(std::ranges::none_of(points, [](const glm::vec3& p) { return p.x == 30.0f || p.z == 200.0f || p.x == 5000.0f; }));
    CHECK(Maps::ChooseSpawnPoints(start, {}).size() == 1);
}

TEST("campaign maps: the stand-in scripts set up the map with its teamset, fog and vision")
{
    auto cuba = *Maps::ReadCatalog(SMALL_CATALOG).FindLevel("cuba");
    CHECK(Maps::GetMapScriptName("cuba") == "maps/mp/cuba.gsc");
    CHECK(Maps::GetClientScriptName("cuba") == "clientscripts/mp/cuba.csc");
    const auto script = Maps::MakeMapScript(cuba);
    CHECK(script.starts_with("main()\r\n{\r\n\tmaps\\mp\\_load::main();\r\n"));
    CHECK(script.find("\tmaps\\mp\\gametypes\\_teamset_cubans::level_init();\r\n") != std::string::npos);
    CHECK(script.find("level_use_unified_spawning(true);") != std::string::npos);
    CHECK(script.find("\tsetVolFog(204.799, 3014.41, 513.74);\r\n") != std::string::npos);
    CHECK(script.find("\tVisionSetNaked(\"cuba\", 0);\r\n") != std::string::npos);
    CHECK(script.ends_with("}\r\n"));
    // the kit's player models precached in the level's first frame (multiplayer's setPlayerModels precaches the
    // jungle marines' on a map mp/mapsTable.csv doesn't name), not on a jungle marines' level, which has them
    const auto shipped = Maps::ReadCatalog(ReadText(SHIPPED_CATALOG));
    CHECK(shipped.playerModels.size() == 4 && shipped.playerModels.at(Maps::Teamset::Cubans).size() == 30);
    const auto& cubans = shipped.playerModels.at(Maps::Teamset::Cubans);
    CHECK(std::ranges::find(cubans, "c_cub_rebels_mp_body_standard") != cubans.end() &&
          std::ranges::find(cubans, "viewmodel_cub_rebels_standard_arms") != cubans.end());
    const auto precached = Maps::MakeMapScript(cuba, cubans);
    CHECK(precached.find("\tPrecacheModel(\"c_cub_rebels_mp_body_standard\");\r\n") != std::string::npos);
    CHECK(precached.find("level_init();") < precached.find("PrecacheModel") && precached.ends_with("}\r\n"));
    const auto& khe = *shipped.FindLevel("khe_sanh");
    CHECK(Maps::MakeMapScript(khe, shipped.playerModels.at(khe.teamset)).find("PrecacheModel") == std::string::npos);
    cuba.fog.clear();
    cuba.vision.clear();
    CHECK(Maps::MakeMapScript(cuba).find("setVolFog") == std::string::npos);
    CHECK(Maps::MakeMapScript(cuba).find("VisionSetNaked") == std::string::npos);
    const auto client = Maps::MakeClientScript(cuba);
    CHECK(client.find("clientscripts\\mp\\_teamset_cubans::level_init();") != std::string::npos);
    CHECK(client.find("clientscripts\\mp\\_load::main();") != std::string::npos);
    CHECK(Maps::GetTraversalScriptName("mantle_over_36") == "maps/mp/animscripts/traverse/mantle_over_36.gsc");
    // as the game asks for one too long for its 64 characters (Call of the Dead's zipline), cut short
    CHECK(Maps::GetTraversalScriptName("zombie_zipline_traverse_lighthouse") ==
          "maps/mp/animscripts/traverse/zombie_zipline_traverse_lighthouse");
    CHECK(Maps::GetEmptyScript() == "main()\r\n{\r\n}\r\n");
}

TEST("campaign maps: a script is packed as zones have them and inflates back")
{
    const auto text = Maps::MakeMapScript(*Maps::ReadCatalog(SMALL_CATALOG).FindLevel("cuba"));
    const auto packed = Maps::PackScript(text);
    std::uint32_t sizes[2]{};
    std::memcpy(sizes, packed.data(), 8);
    CHECK(sizes[0] == text.size() + 1);
    CHECK(sizes[1] == packed.size() - 8);
    const auto plain = Inflate::Zlib(std::span(packed).subspan(8), sizes[0]);
    CHECK(plain && plain->size() == text.size() + 1 && plain->back() == 0);
    CHECK(plain && std::string(plain->begin(), plain->end() - 1) == text);

    // as the game reads each name: a .gsc packed, a name cut short of its extension plain (Call of the Dead's zipline)
    const auto gsc = Maps::MakeScriptFile("maps/mp/cuba.gsc", text);
    CHECK(Maps::UnpackScript(gsc.buffer) == std::string(text));
    CHECK(!Maps::UnpackScript(std::span(gsc.buffer).first(6)));
    CHECK(gsc.buffer == packed && gsc.length == static_cast<std::int32_t>(packed.size()));
    const auto cut = Maps::MakeScriptFile(Maps::GetTraversalScriptName("zombie_zipline_traverse_lighthouse"), "main()");
    CHECK(cut.length == 6 && cut.buffer.size() == 7 && cut.buffer.back() == 0 &&
          std::string(cut.buffer.begin(), cut.buffer.end() - 1) == "main()");
}

TEST("campaign maps: the room a level leaves, as the research measured it")
{
    const auto cuba = *Maps::ReadCatalog(SMALL_CATALOG).FindLevel("cuba");
    // the room a map has in the Nuketown test demo: images 1788, materials 1905, models 661
    const auto now = Maps::MeasureBudget(cuba, 1788, 1905, 661);
    CHECK(now.fits && now.images == 128 && now.materials == 157 && now.models == 218);
    CHECK(now.characters == 5);
    // with the pools grown: 4608 and 1000 more
    const auto grown = Maps::MeasureBudget(cuba, 1788 + 4608, 1905, 661 + 1000);
    CHECK(grown.images == 4736 && grown.models == 1218 && grown.characters == 189);
    const auto full = Maps::MeasureBudget(cuba, 1000, 1905, 661);
    CHECK(!full.fits && full.images < 0 && full.characters == 0);
}

// The levels' ambient effects: the createfx's oneshots and loops, read from the catalog,
// listed by definition for the Look tab, and when each plays as the campaign's client script plays it
TEST("campaign maps: a level's ambient effects are read with their definitions")
{
    const auto catalog = Maps::ReadCatalog(R"({
        "version": 1,
        "levels": [
            {"name": "pentagon", "teamset": "urbanspecops", "world": {"fx": ["env/fire/fx_fire_sm"]},
             "effects": {"names": ["env/fire/fx_fire_sm", "maps/pentagon/fx_dust_motes"],
                         "placed": [[1, 10, 20, 30, 270, 0, 0, -15, 0], [0, -1.5, 2, 3, 0, 90, 5, 0.5, 1],
                                    [1, 0, 0, 0, 0, 0, 0, -15, 0]]}},
            {"name": "outro", "teamset": "urbanspecops", "world": {}}
        ]
    })");
    const auto& pentagon = *catalog.FindLevel("pentagon");
    CHECK(pentagon.effectKinds.size() == 2 && pentagon.effects.size() == 3);
    const auto& dust = pentagon.effects[0];
    CHECK(dust.kind == 1 && Near(dust.origin, {10, 20, 30}) && Near(dust.angles, {270, 0, 0}) && dust.delay == -15.0f &&
          !dust.loop);
    const auto& fire = pentagon.effects[1];
    CHECK(fire.kind == 0 && Near(fire.origin, {-1.5f, 2, 3}) && Near(fire.angles, {0, 90, 5}) && fire.delay == 0.5f &&
          fire.loop);
    CHECK(catalog.FindLevel("outro")->effects.empty());

    bool threw = false;
    try
    {
        Maps::ReadCatalog(R"({"version": 1, "levels": [{"name": "pentagon", "teamset": "urbanspecops", "world": {},
            "effects": {"names": ["a"], "placed": [[1, 0, 0, 0, 0, 0, 0, -15, 0]]}}]})");
    }
    catch (const std::runtime_error&)
    {
        threw = true;
    }
    CHECK(threw);
}

TEST("campaign maps: the Look tab lists the effects by name with how many the level places")
{
    Maps::Level level;
    level.effectKinds = {"maps/pentagon/fx_light_beam", "env/fire/fx_fire_sm", "env/smoke/fx_unused"};
    for (const auto kind : {0u, 1u, 0u, 0u})
        level.effects.push_back({kind});
    const auto rows = Maps::ListEffectKinds(level);
    CHECK(rows.size() == 2);
    CHECK(rows[0].name == "fx_fire_sm" && rows[0].kind == 1 && rows[0].placed == 1);
    CHECK(rows[1].name == "fx_light_beam" && rows[1].kind == 0 && rows[1].placed == 3);
    CHECK(Maps::GetEffectKindName("fx_plain") == "fx_plain");
}

TEST("campaign maps: map effects start and loop as the campaign's client script plays them")
{
    // a oneshot: already 15 s into its life as the map starts; a positive delay waited, then given again
    CHECK(Maps::GetEffectBegin({.delay = -15.0f}, 100000) == 85000);
    CHECK(Maps::GetEffectBegin({.delay = 2.0f}, 100000) == 104000);
    // a loop: at once with its delay, then again each delay once the play before has ended
    const Maps::MapEffect loop{.delay = 0.5f, .loop = true};
    CHECK(Maps::GetEffectBegin(loop, 100000) == 100500);
    CHECK(!Maps::PlaysAgain(loop, 100400, 100000, false));
    CHECK(Maps::PlaysAgain(loop, 100500, 100000, false));
    CHECK(!Maps::PlaysAgain(loop, 103000, 100000, true));
    // the demo rewound to before that play
    CHECK(!Maps::PlaysAgain(loop, 90000, 100000, false));
    // a oneshot never plays again
    CHECK(!Maps::PlaysAgain({.delay = -15.0f}, 200000, 100000, false));
}

// Multiplayer WMD's train duplicated on rewinds
TEST("campaign maps: the train's client script gets what ends it with a rewind")
{
    const std::string script =
        "runTrain( localClientNum, moveTime, numOfCarts )\r\n{\r\n\tlevel endon (\"game_ended\");\r\n\t\r\n\t"
        "originalRatio = ( moveTime / 80 );\r\n}\r\n"
        "spawnTrainEngine()\r\n{\r\n\ttrainEngine SetModel( engineType );\r\n}\r\n"
        "spawnTrainCar()\r\n{\r\n\ttrainCar SetModel( carType );\r\n}\r\n"
        "play_trainCar_horn()\r\n{\r\n\tself endon( \"movedone\" );\r\n}\r\n";
    CHECK(Maps::IsFixedClientScript("clientscripts/mp/mp_russianbase.csc"));
    CHECK(!Maps::IsFixedClientScript("clientscripts/mp/mp_nuked.csc"));
    const auto fixed = Maps::FixClientScript(script);
    CHECK(fixed.has_value());
    if (!fixed)
        return;
    CHECK(fixed->find("level endon( \"iris_rewind\" );\r\n\toriginalRatio") != std::string::npos);
    CHECK(fixed->find("trainEngine SetModel( engineType );\r\n\ttrainEngine thread deleteOnRewind();") != std::string::npos);
    CHECK(fixed->find("trainCar SetModel( carType );\r\n\ttrainCar thread deleteOnRewind();") != std::string::npos);
    CHECK(fixed->find("self endon( \"entityshutdown\" );\r\n}\r\n\r\ndeleteOnRewind()") != std::string::npos);
    CHECK(fixed->find("level waittill( \"iris_rewind\" );") != std::string::npos);
    CHECK(fixed->find(Maps::CLIENT_REWIND_NOTIFY) != std::string::npos);
}

TEST("campaign maps: a script that isn't the one expected is left alone")
{
    CHECK(!Maps::FixClientScript("main()\r\n{\r\n}\r\n"));
}

// A level's parts, to go to
namespace
{
    std::vector<glm::vec3> Line(glm::vec3 from, glm::vec3 step, int count)
    {
        std::vector<glm::vec3> nodes;
        for (int i = 0; i < count; ++i)
            nodes.push_back(from + step * static_cast<float>(i));
        return nodes;
    }
}  // namespace

TEST("campaign maps: path nodes far apart make a part each, in the order a walk from the start meets them")
{
    auto nodes = Line({20000, 0, 0}, {100, 0, 0}, 30);
    const auto first = Line({0, 0, 0}, {100, 0, 0}, 30);
    nodes.insert(nodes.end(), first.begin(), first.end());
    const auto third = Line({40000, 0, 0}, {100, 0, 0}, 30);
    nodes.insert(nodes.end(), third.begin(), third.end());
    const auto areas = Maps::FindAreas(nodes, {0, 0, 0});
    CHECK(areas.size() == 3);
    if (areas.size() != 3)
        return;
    CHECK(areas[0].maxs.x < 5000.0f);
    CHECK(areas[1].mins.x >= 20000.0f && areas[1].maxs.x < 25000.0f);
    CHECK(areas[2].mins.x >= 40000.0f);
    CHECK(areas[0].nodes == 30);
    // a spot of the part itself, near its middle, facing along it
    CHECK(std::abs(areas[0].spot.x - 1450.0f) <= 100.0f);
    CHECK(std::abs(std::abs(areas[0].yaw) - (areas[0].spot.x > 1450.0f ? 180.0f : 0.0f)) < 0.01f);
}

TEST("campaign maps: a few nodes on their own aren't a place to go")
{
    auto nodes = Line({0, 0, 0}, {100, 0, 0}, 30);
    const auto corner = Line({20000, 0, 0}, {100, 0, 0}, Maps::AREA_LEAST_NODES - 1);
    nodes.insert(nodes.end(), corner.begin(), corner.end());
    CHECK(Maps::FindAreas(nodes, {0, 0, 0}).size() == 1);
    CHECK(Maps::FindAreas({}, {0, 0, 0}).empty());
}

TEST("campaign maps: one long walk is cut into parts no wider than the most")
{
    const auto nodes = Line({0, 0, 0}, {200, 0, 0}, 200);
    const auto areas = Maps::FindAreas(nodes, {0, 0, 0});
    CHECK(areas.size() >= 7);
    std::uint32_t total = 0;
    for (const auto& area : areas)
    {
        CHECK(area.maxs.x - area.mins.x <= Maps::AREA_MOST_SPAN);
        total += area.nodes;
    }
    CHECK(total == 200);
    // along the walk
    for (std::size_t i = 1; i < areas.size(); ++i)
        CHECK(areas[i].spot.x > areas[i - 1].spot.x);
}
