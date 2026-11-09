#pragma once

#include "Components/MapMenuLogic.hpp"

namespace IWXMVM::T5::MapMenu
{
    // Campaign levels and Zombies maps in the game's Change Map menu (select_map;
    // Components::MapMenuLogic): their rows added to mp/mapsTable.csv and the UI's map list as the game's own
    // maps are, two tabs (CAMPAIGN, ZOMBIES) after DLC MAPS made of copies of its items, the list showing a tab's maps
    // while it is on, their pictures (the setup's DDS files in picturesFolder) as the materials the menu asks for, and
    // the texts multiplayer's language files lack. Installed after CampaignMaps (it needs the catalog).
    void Install(const std::filesystem::path& picturesFolder);

    // Main thread, each frame: adds the rows, the tabs and the pictures again when the game has made the table, the map
    // list or the menu again (its UI starting again)
    void Frame();

    // As the mod unloads: the menu, the table and the list as the game had them
    void Unload();

    // for the harness
    struct State
    {
        bool rows = false;  // the table and the map list have the levels' rows
        bool tabs = false;  // select_map has the tabs
        std::uint32_t tableRows = 0, mapListCount = 0;
        Components::MapMenuLogic::Tab tab = Components::MapMenuLogic::Tab::Game;
        std::vector<std::string> listed;  // the maps the list shows now, by name
        std::uint32_t pictures = 0, picturesMissing = 0;
        std::string error;
    };
    State GetState();
    // the map list index of a map, as the list's selection gives it to START MATCH (ui_currentNetMap), -1 for none
    std::int32_t FindMapListIndex(const std::string& map);
    // what the language lookup gives for a key, as the menu shows it (the game's text or the mod's)
    std::string Localize(const std::string& key);
    // whether the material the menu asks for a map's picture is the mod's picture of it
    bool HasPicture(const std::string& map);
    // whether select_map shows an item of a tab of ours now: its selected label (the other labels aside)
    bool IsTabSelectedShown(Components::MapMenuLogic::Tab tab);
}  // namespace IWXMVM::T5::MapMenu
