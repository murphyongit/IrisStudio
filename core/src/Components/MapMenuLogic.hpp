#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "CampaignMapLogic.hpp"

namespace IWXMVM::Components::MapMenuLogic
{
    // Campaign levels and Zombies maps in the game's Change Map menu: two
    // tabs after MAPS and DLC MAPS, CAMPAIGN and ZOMBIES, whose rows are the catalog's levels added to
    // mp/mapsTable.csv as the game's own maps are, with the game's names, descriptions and pictures. No game state;
    // tested in tests/actors-tests.

    // the tab the menu shows: the game's own (MAPS or DLC MAPS, which ui_showDLCMaps picks), or one of ours
    enum class Tab : std::int32_t
    {
        Game = 0,
        Campaign = 1,
        Zombies = 2,
    };
    std::string_view GetTabLabel(Tab tab);
    // the language key the tab's label shows ("@IRIS_TAB_CAMPAIGN"), whose text is its label (ResolveKey)
    std::string GetTabLabelKey(Tab tab);
    Tab GetTab(const CampaignMapLogic::Level& level);
    // the dvar the tabs' buttons set (0 for MAPS and DLC MAPS, else the Tab)
    inline constexpr std::string_view TAB_DVAR = "ui_irisMapTab";

    // A tab's maps in the game's order, only those with a menu entry the game folder has (has)
    std::vector<const CampaignMapLogic::Level*> ListTab(const CampaignMapLogic::Catalog& catalog, Tab tab,
                                                        const std::function<bool(const CampaignMapLogic::Level&)>& has);

    // The pack number of a tab's rows in mp/mapsTable.csv (its column k10): one the game's tabs never list (they list
    // pack 0, and 1-5 for map packs owned)
    std::int32_t GetPack(Tab tab);
    std::optional<Tab> FindTabOfPack(std::int32_t pack);

    // A level's row in mp/mapsTable.csv, as the game's rows have their 16 columns: the map, its teamset twice, the
    // language key of its name (the list shows it + "_CAPS"), its picture's material, the row's index, its
    // description's key, the compass (none), the size (none: MAP SIZE is left out on these tabs), NO, YES,
    // the pack, and the teamset's two team names and two short names
    inline constexpr std::size_t TABLE_COLUMNS = 16;
    std::vector<std::string> MakeTableRow(const CampaignMapLogic::Level& level, std::uint32_t index);

    // The name keys of our rows: IRIS_MAP_CUBA, whose text is the level's title, and IRIS_MAP_CUBA_CAPS, its menu
    // name's text
    std::string GetNameKey(std::string_view level);
    // What the game's language lookup gives for a key the menu shows: the catalog's text for a key multiplayer lacks or
    // one of ours, a tab's label, a level's name for its name keys (game: the game's own lookup, for the name's key),
    // else none (the game's own text stands). UTF-8.
    std::optional<std::string> ResolveKey(const CampaignMapLogic::Catalog& catalog, std::string_view key,
                                          const std::function<std::optional<std::string>(const std::string&)>& game);

    // The materials the menu draws a map's picture with ("menu_" + map + "_map_select_final", and "_big" behind the LAN
    // setup), and the level a material name is the picture of, if any
    std::string GetPictureMaterial(std::string_view level, bool big);
    std::optional<std::string> FindLevelOfPictureMaterial(const CampaignMapLogic::Catalog& catalog,
                                                          std::string_view material);
    // the image of a picture entry ("cuba" of "frontend/cuba"), empty for none
    std::string_view GetPictureImage(std::string_view picture);

    // The hash a string table's cell is found by, as the game's lookup computes it (djb2 over the lowercase letters)
    std::int32_t HashCell(std::string_view text);

    // UTF-8 text as the game's strings are (Windows-1252: what Latin-1 has; other letters become '?')
    std::string ToGameText(std::string_view utf8);
}  // namespace IWXMVM::Components::MapMenuLogic
