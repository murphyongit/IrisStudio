#include "StdInclude.hpp"
#include "MapMenu.hpp"

#include <d3dx9.h>

#include "Addresses.hpp"
#include "CampaignMaps.hpp"
#include "ContentPacksInternal.hpp"
#include "D3D9.hpp"
#include "Events.hpp"
#include "Functions.hpp"
#include "Structures.hpp"
#include "Utilities/HookManager.hpp"

namespace IWXMVM::T5::MapMenu
{
    using namespace Structures;
    namespace Logic = Components::MapMenuLogic;
    namespace Maps = Components::CampaignMapLogic;
    using Logic::Tab;

    // The game's Change Map menu (select_map, ui_mp, as patch_ui_mp has it) lists the maps
    // of mp/mapsTable.csv that the UI copied into its map list as it started (UI_LoadMapList): the MAPS tab those of
    // pack 0, DLC MAPS those of the packs owned, as the two tabs' buttons set ui_showDLCMaps. The map's name, picture,
    // description and size are looked up by its name in the table and as the material "menu_<map>_map_select_final".
    //
    // The levels are added as the game's maps are: rows of the table (a pack its tabs never list), the map list loaded
    // again by the game's own loader; two tabs made of copies of DLC MAPS' four items (frame, label as selected, label,
    // button) moved after it, whose buttons set ui_irisMapTab as well (MAPS' and DLC MAPS' set it to 0), the list's
    // three functions showing a tab's rows while it is on, and which labels and MAP SIZE show following it
    // (Item_IsVisible); the pictures made of the setup's DDS files as copies of a menu picture's material; the texts
    // multiplayer lacks given to its language lookup.

    // StringTable (mp/mapsTable.csv), as the game's lookups read it (0x5C19D0, 0x657DC0, 0x8B4180): the cells row by
    // row, and the cells' numbers sorted by their hash (Com_HashString, MapMenuLogic::HashCell)
    struct StringTableCell
    {
        const char* string;
        std::int32_t hash;
    };
    struct StringTable
    {
        const char* name;
        std::int32_t columnCount;
        std::int32_t rowCount;
        StringTableCell* values;
        std::int16_t* cellIndex;
    };
    inline constexpr std::int32_t ASSET_TYPE_STRINGTABLE = 37;
    inline constexpr const char* MAPS_TABLE = "mp/mapsTable.csv";
    inline constexpr std::int32_t MAPS_TABLE_COLUMNS = 16;
    // the cell that says how many maps there are ("maxnum_map", 26), and the column of a row's index
    inline constexpr const char* MAP_COUNT_ROW = "maxnum_map";
    inline constexpr std::int32_t INDEX_COLUMN = 5;

    // The UI's map list: entries of 0x130 bytes, the count 8 bytes before the first, at most 128
    inline constexpr std::size_t MAP_ENTRY_SIZE = 0x130;
    inline constexpr std::size_t MAP_ENTRY_NAME = 0x20;
    inline constexpr std::size_t MAP_ENTRY_PACK = 0x6C;
    inline constexpr std::int32_t MAP_LIST_MOST = 128;

    // menuDef_t and itemDef_s, of what the tabs are made of (Structures::menuDef_t for the rest of the menu)
    inline constexpr std::size_t MENU_ITEM_COUNT = 0xB0;
    inline constexpr std::size_t MENU_ITEMS = 0x188;
    inline constexpr std::size_t ITEM_SIZE = 0x110;
    inline constexpr std::size_t ITEM_RECT_X = 0x04;
    inline constexpr std::size_t ITEM_RECT_W = 0x0C;
    // the rect the item is placed by (relative to the menu's), which the game works out its rect of each frame
    inline constexpr std::size_t ITEM_RECT_CLIENT_X = 0x1C;
    // a label's text block: its rect first, then the statement of its text ({file, line, rpn count, rpn}, the text a
    // string operand of it) and a block of the statements of its rect, the x's at +0x18
    inline constexpr std::size_t ITEM_TEXT = 0xC0;
    inline constexpr std::size_t TEXT_SIZE = 0x50;
    inline constexpr std::size_t TEXT_STATEMENT = 0x3C;
    inline constexpr std::size_t TEXT_RECT_STATEMENTS = 0x40;
    inline constexpr std::size_t TEXT_RECT_STATEMENTS_SIZE = 0x38;
    inline constexpr std::size_t TEXT_RECT_X_STATEMENT = 0x18;
    inline constexpr std::size_t ITEM_PARENT = 0xC4;
    // four expression statements of the item's rect: the x's (worked out from the tabs before it, the text widths of
    // their labels) first, the width's (its own label's width) the third
    inline constexpr std::size_t ITEM_EXPRESSIONS = 0xC8;
    inline constexpr std::size_t EXPRESSION_SIZE = 0x10;
    inline constexpr std::size_t X_EXPRESSION = 0;
    inline constexpr std::size_t WIDTH_EXPRESSION = 2;
    inline constexpr std::size_t RPN_SIZE = 12;
    // the 'visible when' statement: none when its file is null
    inline constexpr std::size_t ITEM_VISIBLE_EXP = 0xCC;
    // the event handlers {name, script, next}, a script's action at +0x1C
    inline constexpr std::size_t ITEM_ON_EVENT = 0x104;
    inline constexpr std::size_t HANDLER_SIZE = 0x0C;
    inline constexpr std::size_t SCRIPT_SIZE = 0x30;
    inline constexpr std::size_t SCRIPT_ACTION = 0x1C;

    // select_map's items (patch_ui_mp): MAPS' label as selected and its label, its button; DLC MAPS' four; MAP SIZE's
    // label and value. Checked by their actions before anything changes.
    inline constexpr std::size_t SELECT_MAP_ITEMS = 22;
    inline constexpr std::size_t MAPS_SELECTED = 7, MAPS_LABEL = 8, MAPS_BUTTON = 9;
    inline constexpr std::size_t DLC_FRAME = 10, DLC_SELECTED = 11, DLC_LABEL = 12, DLC_BUTTON = 13;
    inline constexpr std::size_t SIZE_LABEL = 18, SIZE_VALUE = 19;

    template <typename T>
    T& At(void* base, std::size_t offset)
    {
        return *reinterpret_cast<T*>(reinterpret_cast<std::uint8_t*>(base) + offset);
    }

    const Maps::Catalog* catalog = nullptr;
    std::filesystem::path pictures;
    std::string error;
    std::recursive_mutex mutex;  // the strings and pictures, which the hooks read on the render and loader threads too

    // the rows: the table's fields as the game had them, and what replaced them
    struct Rows
    {
        StringTable* table = nullptr;
        std::int32_t rowCount = 0;
        StringTableCell* values = nullptr;
        std::int16_t* cellIndex = nullptr;
        std::vector<StringTableCell> newValues;
        std::vector<std::int16_t> newCellIndex;
        std::deque<std::string> texts;
    };
    std::optional<Rows> rows;

    // the tabs: select_map's items as the game had them, and what was made for them
    enum class Rule
    {
        MapsSelected,  // MAPS' orange label: only while no tab of ours is on
        MapsLabel,     // MAPS' white label: also while one of ours is on
        GameOnly,      // MAP SIZE: only on the game's tabs
        Frame,         // a tab of ours: shown when it has maps
        Selected,
        Label,
    };
    struct ItemRule
    {
        Rule rule;
        Tab tab = Tab::Game;
    };
    struct Tabs
    {
        void* menu = nullptr;
        void** items = nullptr;  // the game's array
        std::int32_t count = 0;
        std::vector<void*> newItems;
        std::deque<std::vector<std::uint8_t>> blocks;
        std::deque<std::string> texts;
        std::vector<std::pair<void*, const char*>> actions;  // the game's buttons' scripts and their actions
        std::unordered_map<const void*, ItemRule> rules;
        // the copies, placed each frame after DLC MAPS as its own items are (PlaceTabs)
        struct Copy
        {
            void* item;
            Tab tab;
            bool frame;
            bool label;
        };
        std::vector<Copy> copies;
    };
    std::optional<Tabs> tabs;
    bool tabsRefused = false;

    // the pictures: a texture, an image and a material a picture
    struct Picture
    {
        IDirect3DTexture9* texture = nullptr;
        std::unique_ptr<GfxImage> image;
        std::unique_ptr<Material> finalMaterial, bigMaterial;
        std::unique_ptr<std::vector<MaterialTextureDef>> textures;
        std::string finalName, bigName, imageName;
        const Material* template_ = nullptr;
        bool failed = false;  // its file didn't read (the setup hasn't copied it)
    };
    std::unordered_map<std::string, Picture> picturesMade;  // by level
    std::uint32_t picturesMissing = 0;

    std::unordered_map<std::string, std::string> texts;  // what the language lookup gives, in the game's text

    Tab GetTab()
    {
        const auto dvar = Functions::FindDvar(Logic::TAB_DVAR);
        if (!dvar)
            return Tab::Game;
        const auto value = dvar->type == DVAR_TYPE_STRING ? (dvar->current.string ? std::atoi(dvar->current.string) : 0)
                           : dvar->type == DVAR_TYPE_INT  ? dvar->current.integer
                                                          : 0;
        return value == 1 ? Tab::Campaign : value == 2 ? Tab::Zombies : Tab::Game;
    }

    bool InGameFolder(const Maps::Level& level)
    {
        return CampaignMaps::IsInGameFolder(level);
    }

    std::uint8_t* MapList()
    {
        return reinterpret_cast<std::uint8_t*>(GetGameAddresses().ui_mapList());
    }
    std::int32_t& MapListCount()
    {
        return *reinterpret_cast<std::int32_t*>(MapList() - 8);
    }

    // the map list's entries of a tab, in its order
    std::vector<std::int32_t> TabEntries(Tab tab)
    {
        std::vector<std::int32_t> entries;
        const auto pack = Logic::GetPack(tab);
        const auto count = std::min(MapListCount(), MAP_LIST_MOST);
        for (std::int32_t i = 0; i < count; ++i)
            if (*reinterpret_cast<const std::int32_t*>(MapList() + i * MAP_ENTRY_SIZE + MAP_ENTRY_PACK) == pack)
                entries.push_back(i);
        return entries;
    }

    // The list's three functions: while a tab of ours is on, its rows
    typedef std::int32_t(__cdecl* UI_MapListCount_t)();
    typedef std::int32_t(__cdecl* UI_MapListIndex_t)(std::int32_t row);
    typedef const char*(__cdecl* UI_MapListEntryName_t)(std::int32_t row);
    UI_MapListCount_t UI_MapListCount_Trampoline = nullptr;
    UI_MapListIndex_t UI_MapListIndex_Trampoline = nullptr;
    UI_MapListEntryName_t UI_MapListEntryName_Trampoline = nullptr;

    std::int32_t __cdecl UI_MapListCount_Hook()
    {
        const auto tab = GetTab();
        if (tab == Tab::Game || !rows)
            return UI_MapListCount_Trampoline();
        return static_cast<std::int32_t>(TabEntries(tab).size());
    }

    std::int32_t __cdecl UI_MapListIndex_Hook(std::int32_t row)
    {
        const auto tab = GetTab();
        if (tab == Tab::Game || !rows)
            return UI_MapListIndex_Trampoline(row);
        const auto entries = TabEntries(tab);
        return row >= 0 && row < static_cast<std::int32_t>(entries.size()) ? entries[row] : 0;
    }

    const char* __cdecl UI_MapListEntryName_Hook(std::int32_t row)
    {
        const auto tab = GetTab();
        if (tab == Tab::Game || !rows)
            return UI_MapListEntryName_Trampoline(row);
        const auto entries = TabEntries(tab);
        const auto entry = row >= 0 && row < static_cast<std::int32_t>(entries.size()) ? entries[row] : 0;
        return reinterpret_cast<const char*>(MapList() + entry * MAP_ENTRY_SIZE + MAP_ENTRY_NAME);
    }

    // Which of select_map's items show: the game's 'visible when' first, then the tab (Rule)
    typedef std::int32_t(__cdecl* Item_IsVisible_t)(void* dc, std::int32_t localClientNum, void* item);
    Item_IsVisible_t Item_IsVisible_Trampoline = nullptr;

    bool HasRows(Tab tab)
    {
        return catalog && !Logic::ListTab(*catalog, tab, InGameFolder).empty();
    }

    std::int32_t __cdecl Item_IsVisible_Hook(void* dc, std::int32_t localClientNum, void* item)
    {
        const auto visible = Item_IsVisible_Trampoline(dc, localClientNum, item);
        if (!tabs)
            return visible;
        const auto rule = tabs->rules.find(item);
        if (rule == tabs->rules.end())
            return visible;
        const auto tab = GetTab();
        switch (rule->second.rule)
        {
            case Rule::MapsSelected:
            case Rule::GameOnly:
                return visible && tab == Tab::Game;
            case Rule::MapsLabel:
                return visible || tab != Tab::Game;
            case Rule::Frame:
                return visible && HasRows(rule->second.tab);
            case Rule::Selected:
                return visible && HasRows(rule->second.tab) && tab == rule->second.tab;
            case Rule::Label:
                return visible && HasRows(rule->second.tab) && tab != rule->second.tab;
        }
        return visible;
    }

    // The language lookup: the mod's texts for its keys and for the keys multiplayer lacks
    typedef const char*(__cdecl* SEH_StringEd_GetString_t)(const char* key);
    SEH_StringEd_GetString_t SEH_StringEd_GetString_Trampoline = nullptr;

    const char* OwnText(const char* key, bool gameHasNone)
    {
        if (!catalog || !key)
            return nullptr;
        const std::string_view name(key);
        if (!name.starts_with("IRIS_") && !gameHasNone)
            return nullptr;
        {
            std::lock_guard lock(mutex);
            if (const auto known = texts.find(std::string(name)); known != texts.end())
                return known->second.c_str();
        }
        // (no lock held while the game looks a key up: it takes its own locks, which threads in the hooks may hold)
        bool fromGame = false;
        const auto text =
            Logic::ResolveKey(*catalog, name, [&fromGame](const std::string& gameKey) -> std::optional<std::string> {
                const auto found = SEH_StringEd_GetString_Trampoline(gameKey.c_str());
                if (!found || !*found)
                    return std::nullopt;
                fromGame = true;
                return std::string(found);
            });
        // a key of the mod's it has no text for is empty: the game's lookup of a key it doesn't have waits while a
        // zone loads (DB_FindXAssetHeader), and a map's load stopped there
        const bool ours = name.starts_with("IRIS_");
        if (!text && !ours)
            return nullptr;
        if (!text)
            LOG_WARN("Change Map: no text for {}", name);
        // (the game's own text is in its code page already, the catalog's is UTF-8)
        std::lock_guard lock(mutex);
        const auto [kept, _] =
            texts.try_emplace(std::string(name), !text ? std::string() : fromGame ? *text : Logic::ToGameText(*text));
        return kept->second.c_str();
    }

    const char* __cdecl SEH_StringEd_GetString_Hook(const char* key)
    {
        if (key && std::string_view(key).starts_with("IRIS_"))
            return OwnText(key, false);
        const auto text = SEH_StringEd_GetString_Trampoline(key);
        if (text)
            return text;
        if (const auto own = OwnText(key, true))
            return own;
        return text;
    }

    // The pictures: the material the menu asks for a map's picture, the mod's
    typedef void*(__cdecl* DB_FindXAssetHeader_t)(std::int32_t type, const char* name);
    DB_FindXAssetHeader_t DB_FindXAssetHeader_Trampoline = nullptr;
    inline constexpr const char* PICTURE_TEMPLATE = "menu_mp_array_map_select_final";

    const Material* FindPicture(const char* name)
    {
        if (!catalog || !name || std::strncmp(name, "menu_", 5) != 0 || std::strstr(name, "_map_select_") == nullptr)
            return nullptr;
        const auto level = Logic::FindLevelOfPictureMaterial(*catalog, name);
        if (!level)
            return nullptr;
        // only while the material they are copies of is there as it was: a map's load unloads the menus' zones (their
        // assets become default stubs), and the copies share its technique set
        const auto template_ = ContentPacks::FindRegistered(ASSET_TYPE_MATERIAL, PICTURE_TEMPLATE);
        std::lock_guard lock(mutex);
        const auto made = picturesMade.find(*level);
        if (made == picturesMade.end() || !made->second.finalMaterial || made->second.template_ != template_ ||
            static_cast<const Material*>(template_)->techniqueSet != made->second.finalMaterial->techniqueSet)
            return nullptr;
        return made->second.finalName == name ? made->second.finalMaterial.get() : made->second.bigMaterial.get();
    }

    // The language entries of the mod's keys, as the menus' expressions look a key up (locString)
    inline constexpr std::int32_t ASSET_TYPE_LOCALIZE_ENTRY = 23;
    struct LocalizeEntry
    {
        const char* value;
        const char* name;
    };
    std::unordered_map<std::string, LocalizeEntry> localizeEntries;

    const LocalizeEntry* FindLocalizeEntry(const char* name)
    {
        if (!catalog || !name)
            return nullptr;
        const std::string_view key(name);
        if (!key.starts_with("IRIS_") && !catalog->strings.contains(key))
            return nullptr;
        const auto text = OwnText(name, true);
        if (!text)
            return nullptr;
        std::lock_guard lock(mutex);
        auto [entry, added] = localizeEntries.try_emplace(std::string(key));
        entry->second = {text, entry->first.c_str()};
        return &entry->second;
    }

    void* __cdecl DB_FindXAssetHeader_Hook(std::int32_t type, const char* name)
    {
        if (type == ASSET_TYPE_MATERIAL)
            if (const auto picture = FindPicture(name))
                return const_cast<Material*>(picture);
        if (type == ASSET_TYPE_LOCALIZE_ENTRY)
            if (const auto entry = FindLocalizeEntry(name))
                return const_cast<LocalizeEntry*>(entry);
        return DB_FindXAssetHeader_Trampoline(type, name);
    }

    // The pictures' textures, made on the render thread (once): the game's device isn't made for calls from other
    // threads (r_multithreaded_device 0), and textures made on the main thread as the render thread drew left the
    // driver stuck, and the game with it, when the mod was loaded as the menu showed
    std::atomic<bool> texturesWanted = false;

    void MakeTextures()
    {
        const auto device = D3D9::GetDevice();
        const auto levels = catalog;
        if (!levels || !device || !texturesWanted.exchange(false))
            return;
        for (const auto& level : levels->levels)
        {
            if (!level.menu || level.menu->picture.empty())
                continue;
            {
                std::lock_guard lock(mutex);
                const auto& picture = picturesMade[level.name];
                if (picture.texture || picture.failed)
                    continue;
            }
            // (no lock held around the device: the main thread may wait for this thread in a hook)
            const auto file = pictures / (std::string(Logic::GetPictureImage(level.menu->picture)) + ".dds");
            std::ifstream in(file, std::ios::binary);
            const std::vector<char> data((std::istreambuf_iterator<char>(in)), {});
            IDirect3DTexture9* texture = nullptr;
            const auto result =
                data.empty() ? E_FAIL
                             : D3DXCreateTextureFromFileInMemoryEx(
                                   device, data.data(), static_cast<UINT>(data.size()), D3DX_DEFAULT_NONPOW2,
                                   D3DX_DEFAULT_NONPOW2, D3DX_FROM_FILE, 0, D3DFMT_FROM_FILE, D3DPOOL_MANAGED,
                                   D3DX_FILTER_NONE, D3DX_FILTER_NONE, 0, nullptr, nullptr, &texture);
            std::lock_guard lock(mutex);
            auto& picture = picturesMade[level.name];
            if (FAILED(result))
            {
                picture.failed = true;
                LOG_WARN("Change Map: no picture of {} ({} doesn't read, {:08X})", level.name, file.string(),
                         static_cast<std::uint32_t>(result));
                continue;
            }
            picture.texture = texture;
        }
    }

    // Makes the pictures' materials, as copies of a multiplayer map's picture (again when its zone was loaded again:
    // they share its technique set), once the render thread made their textures
    void MakePictures()
    {
        const auto template_ =
            static_cast<const Material*>(ContentPacks::FindRegistered(ASSET_TYPE_MATERIAL, PICTURE_TEMPLATE));
        if (!template_ || !template_->textureTable || template_->textureCount == 0)
            return;
        std::uint32_t missing = 0;
        for (const auto& level : catalog->levels)
        {
            if (!level.menu || level.menu->picture.empty())
                continue;
            // (the materials made only here, on the main thread; the lock only around what the hooks and the render
            // thread read and write)
            Picture* picture = nullptr;
            IDirect3DTexture9* texture = nullptr;
            bool failed = false;
            {
                std::lock_guard lock(mutex);
                picture = &picturesMade[level.name];
                texture = picture->texture;
                failed = picture->failed;
            }
            if (failed)
            {
                ++missing;
                continue;
            }
            if (!texture)
            {
                texturesWanted = true;
                continue;
            }
            if (picture->template_ == template_ && picture->finalMaterial &&
                picture->finalMaterial->techniqueSet == template_->techniqueSet)
                continue;
            auto image = std::make_unique<GfxImage>(*template_->textureTable[0].image);
            image->texture = texture;
            image->streaming = false;
            picture->imageName = std::string(Logic::GetPictureImage(level.menu->picture));
            image->name = picture->imageName.c_str();
            auto textures = std::make_unique<std::vector<MaterialTextureDef>>(
                template_->textureTable, template_->textureTable + template_->textureCount);
            for (auto& texture : *textures)
                texture.image = image.get();
            picture->finalName = Logic::GetPictureMaterial(level.name, false);
            picture->bigName = Logic::GetPictureMaterial(level.name, true);
            auto finalMaterial = std::make_unique<Material>(*template_);
            finalMaterial->name = picture->finalName.c_str();
            finalMaterial->textureTable = textures->data();
            auto bigMaterial = std::make_unique<Material>(*finalMaterial);
            bigMaterial->name = picture->bigName.c_str();
            std::lock_guard lock(mutex);
            // what the renderer may still have queued of the copies before is kept
            if (picture->finalMaterial)
            {
                static_cast<void>(picture->image.release());
                static_cast<void>(picture->textures.release());
                static_cast<void>(picture->finalMaterial.release());
                static_cast<void>(picture->bigMaterial.release());
            }
            picture->image = std::move(image);
            picture->textures = std::move(textures);
            picture->finalMaterial = std::move(finalMaterial);
            picture->bigMaterial = std::move(bigMaterial);
            picture->template_ = template_;
        }
        picturesMissing = missing;
    }

    // The rows: the table with the levels' rows added, and the map list loaded again from it
    void AddRows()
    {
        auto table = static_cast<StringTable*>(ContentPacks::FindRegistered(ASSET_TYPE_STRINGTABLE, MAPS_TABLE));
        if (!table)
            return;
        if (rows && rows->table == table && table->values == rows->newValues.data())
            return;
        if (table->columnCount != MAPS_TABLE_COLUMNS)
        {
            error = std::format("{} has {} columns, not {}", MAPS_TABLE, table->columnCount, MAPS_TABLE_COLUMNS);
            return;
        }
        Rows added{.table = table, .rowCount = table->rowCount, .values = table->values, .cellIndex = table->cellIndex};
        const auto columns = table->columnCount;
        added.newValues.assign(table->values, table->values + table->rowCount * columns);
        // the game's maps: the count's row says how many
        std::int32_t countRow = -1;
        for (std::int32_t row = 0; row < table->rowCount; ++row)
            if (const auto first = table->values[row * columns].string; first && std::strcmp(first, MAP_COUNT_ROW) == 0)
                countRow = row;
        if (countRow < 0)
        {
            error = std::format("{} has no {} row", MAPS_TABLE, MAP_COUNT_ROW);
            return;
        }
        const auto gameMaps = std::atoi(table->values[countRow * columns + 1].string);
        const auto cell = [&](const std::string& text) {
            const auto& kept = added.texts.emplace_back(text);
            return StringTableCell{kept.c_str(), Logic::HashCell(kept)};
        };
        std::int32_t index = gameMaps;
        for (const auto tab : {Tab::Campaign, Tab::Zombies})
            for (const auto* level : Logic::ListTab(*catalog, tab, InGameFolder))
            {
                if (index >= MAP_LIST_MOST)
                    break;
                for (const auto& text : Logic::MakeTableRow(*level, static_cast<std::uint32_t>(index)))
                    added.newValues.push_back(cell(text));
                ++index;
            }
        added.newValues[countRow * columns + 1] = cell(std::to_string(index));
        const auto cells = static_cast<std::int32_t>(added.newValues.size());
        added.newCellIndex.resize(cells);
        std::iota(added.newCellIndex.begin(), added.newCellIndex.end(), std::int16_t{0});
        std::stable_sort(added.newCellIndex.begin(), added.newCellIndex.end(), [&](std::int16_t a, std::int16_t b) {
            return added.newValues[a].hash < added.newValues[b].hash;
        });
        table->values = added.newValues.data();
        table->cellIndex = added.newCellIndex.data();
        table->rowCount = cells / columns;
        rows = std::move(added);
        reinterpret_cast<void(__cdecl*)()>(GetGameAddresses().UI_LoadMapList())();
        LOG_INFO("Change Map: {} campaign levels and Zombies maps added to {} after the game's {} (map list {})",
                 index - gameMaps, MAPS_TABLE, gameMaps, MapListCount());
    }

    void RestoreRows()
    {
        if (!rows)
            return;
        if (rows->table->values == rows->newValues.data())
        {
            rows->table->values = rows->values;
            rows->table->cellIndex = rows->cellIndex;
            rows->table->rowCount = rows->rowCount;
            reinterpret_cast<void(__cdecl*)()>(GetGameAddresses().UI_LoadMapList())();
        }
        rows.reset();
    }

    // select_map, as the UI's context has it
    void* FindMenu(const char* name)
    {
        const auto dc = reinterpret_cast<std::uint8_t*>(GetGameAddresses().uiContext());
        const auto count = At<std::int32_t>(dc, 0x14FC);
        for (std::int32_t i = 0; i < count && i < 1024; ++i)
        {
            const auto menu = At<void*>(dc, 0x23C + i * 8 + 4);
            if (menu && At<const char*>(menu, 0) && std::strcmp(At<const char*>(menu, 0), name) == 0)
                return menu;
        }
        return nullptr;
    }

    void* EventScript(void* item)
    {
        const auto handler = At<void*>(item, ITEM_ON_EVENT);
        return handler ? At<void*>(handler, 4) : nullptr;
    }
    const char* Action(void* item)
    {
        const auto script = EventScript(item);
        return script ? At<const char*>(script, SCRIPT_ACTION) : nullptr;
    }

    std::uint8_t* CopyBlock(Tabs& made, const std::uint8_t* source, std::size_t size)
    {
        return made.blocks.emplace_back(source, source + size).data();
    }

    // a statement's own copy of its rpn, the string operands of oldText newText
    void CopyStatement(Tabs& made, std::uint8_t* statement, const char* oldText, const char* newText)
    {
        const auto count = At<std::int32_t>(statement, 8);
        const auto rpn = At<std::uint8_t*>(statement, 12);
        if (!rpn || count <= 0 || count >= 64)
            return;
        const auto copy = CopyBlock(made, rpn, count * RPN_SIZE);
        for (std::size_t offset = 0; offset + 4 <= count * RPN_SIZE; offset += 4)
            if (At<const char*>(copy, offset) == oldText)
                At<const char*>(copy, offset) = newText;
        At<std::uint8_t*>(statement, 12) = copy;
    }

    // no statement: the value stays as it is set (PlaceTabs)
    void DropStatement(std::uint8_t* statement)
    {
        std::memset(statement, 0, EXPRESSION_SIZE);
    }

    // A copy of an item for a tab of ours, with its own label text and action, its x set each frame (label: the item
    // is a label, whose +0xC0 is its text block; a button's is another block)
    void* CopyItem(Tabs& made, void* source, bool label, const char* oldText, const char* newText, const char* action)
    {
        void* item = CopyBlock(made, reinterpret_cast<std::uint8_t*>(source), ITEM_SIZE);
        // no 'visible when' of DLC MAPS' (Item_IsVisible_Hook decides)
        At<const char*>(item, ITEM_VISIBLE_EXP) = nullptr;
        if (const auto text = label ? At<std::uint8_t*>(item, ITEM_TEXT) : nullptr)
        {
            const auto copy = CopyBlock(made, text, TEXT_SIZE);
            At<std::uint8_t*>(item, ITEM_TEXT) = copy;
            if (const auto statement = At<std::uint8_t*>(copy, TEXT_STATEMENT))
            {
                At<std::uint8_t*>(copy, TEXT_STATEMENT) = CopyBlock(made, statement, EXPRESSION_SIZE);
                CopyStatement(made, At<std::uint8_t*>(copy, TEXT_STATEMENT), oldText, newText);
            }
            if (const auto statements = At<std::uint8_t*>(copy, TEXT_RECT_STATEMENTS))
            {
                At<std::uint8_t*>(copy, TEXT_RECT_STATEMENTS) =
                    CopyBlock(made, statements, TEXT_RECT_STATEMENTS_SIZE);
                DropStatement(At<std::uint8_t*>(copy, TEXT_RECT_STATEMENTS) + TEXT_RECT_X_STATEMENT);
            }
        }
        if (const auto expressions = At<std::uint8_t*>(item, ITEM_EXPRESSIONS))
        {
            const auto copy = CopyBlock(made, expressions, 4 * EXPRESSION_SIZE);
            At<std::uint8_t*>(item, ITEM_EXPRESSIONS) = copy;
            DropStatement(copy + X_EXPRESSION * EXPRESSION_SIZE);
            CopyStatement(made, copy + WIDTH_EXPRESSION * EXPRESSION_SIZE, oldText, newText);
        }
        if (action)
            if (auto handler = At<std::uint8_t*>(item, ITEM_ON_EVENT); handler && At<std::uint8_t*>(handler, 4))
            {
                auto& handlerCopy = made.blocks.emplace_back(handler, handler + HANDLER_SIZE);
                const auto script = At<std::uint8_t*>(handler, 4);
                auto& scriptCopy = made.blocks.emplace_back(script, script + SCRIPT_SIZE);
                At<const char*>(scriptCopy.data(), SCRIPT_ACTION) = action;
                At<std::uint8_t*>(handlerCopy.data(), 4) = scriptCopy.data();
                At<std::uint8_t*>(item, ITEM_ON_EVENT) = handlerCopy.data();
            }
        return item;
    }

    // The tabs: select_map with copies of DLC MAPS' items for each tab of ours
    void AddTabs()
    {
        const auto menu = FindMenu("select_map");
        if (!menu)
            return;
        if (tabs && tabs->menu == menu && At<void**>(menu, MENU_ITEMS) == tabs->newItems.data())
            return;
        if (tabsRefused)
            return;
        const auto count = At<std::int32_t>(menu, MENU_ITEM_COUNT);
        const auto items = At<void**>(menu, MENU_ITEMS);
        const auto contains = [](const char* text, std::string_view part) {
            return text && std::string_view(text).find(part) != std::string_view::npos;
        };
        if (count != SELECT_MAP_ITEMS || !items || !contains(Action(items[MAPS_BUTTON]), "ui_showDLCMaps 0") ||
            !contains(Action(items[DLC_BUTTON]), "ui_showDLCMaps 1") ||
            At<void*>(items[DLC_FRAME], ITEM_PARENT) != menu)
        {
            tabsRefused = true;
            error = "select_map isn't the menu expected: no tabs";
            LOG_ERROR("Change Map: {} ({} items)", error, count);
            return;
        }

        Tabs made{.menu = menu, .items = items, .count = count};
        made.newItems.assign(items, items + count);
        made.rules[items[MAPS_SELECTED]] = {Rule::MapsSelected};
        made.rules[items[MAPS_LABEL]] = {Rule::MapsLabel};
        made.rules[items[SIZE_LABEL]] = {Rule::GameOnly};
        made.rules[items[SIZE_VALUE]] = {Rule::GameOnly};
        // MAPS and DLC MAPS put the game's tabs back
        for (const auto button : {MAPS_BUTTON, DLC_BUTTON})
        {
            const auto script = EventScript(items[button]);
            const auto& action = made.texts.emplace_back(std::format(
                "setDvar {} 0 ; {}", Logic::TAB_DVAR, At<const char*>(script, SCRIPT_ACTION)));
            made.actions.emplace_back(script, At<const char*>(script, SCRIPT_ACTION));
            At<const char*>(script, SCRIPT_ACTION) = action.c_str();
        }
        // DLC MAPS' label text (its text statement's string), which the copies' texts replace
        const auto textStatement = At<std::uint8_t*>(At<std::uint8_t*>(items[DLC_LABEL], ITEM_TEXT), TEXT_STATEMENT);
        const auto oldText = At<const char*>(At<std::uint8_t*>(textStatement, 12), 8);
        for (const auto tab : {Tab::Campaign, Tab::Zombies})
        {
            const auto& text = made.texts.emplace_back(Logic::GetTabLabelKey(tab));
            const auto& action = made.texts.emplace_back(std::format(
                "play \"uin_navigation_click\" ; setDvar ui_showDLCMaps 0 ; setDvar {} {} ; execNow movefeeder 0 "
                "map_selection ; ;",
                Logic::TAB_DVAR, static_cast<std::int32_t>(tab)));
            const std::array<std::pair<std::size_t, Rule>, 4> parts{
                {{DLC_FRAME, Rule::Frame}, {DLC_SELECTED, Rule::Selected}, {DLC_LABEL, Rule::Label},
                 {DLC_BUTTON, Rule::Frame}}};
            for (const auto& [source, rule] : parts)
            {
                const bool label = source == DLC_SELECTED || source == DLC_LABEL;
                const auto item = CopyItem(made, items[source], label, oldText, text.c_str(),
                                           source == DLC_BUTTON ? action.c_str() : nullptr);
                made.newItems.push_back(item);
                made.rules[item] = {rule, tab};
                made.copies.push_back({item, tab, source == DLC_FRAME, label});
            }
        }
        At<void**>(menu, MENU_ITEMS) = made.newItems.data();
        At<std::int32_t>(menu, MENU_ITEM_COUNT) = static_cast<std::int32_t>(made.newItems.size());
        tabs = std::move(made);
        LOG_INFO("Change Map: the CAMPAIGN and ZOMBIES tabs added to select_map");
    }

    // The copies after DLC MAPS as its items are after MAPS: each tab where the one before it ends, its label's text as
    // far in as DLC MAPS' (the game works out each tab's width from its label's)
    void PlaceTabs()
    {
        if (!tabs || At<void**>(tabs->menu, MENU_ITEMS) != tabs->newItems.data())
            return;
        const auto items = tabs->items;
        const auto label = items[DLC_LABEL];
        const float textInset = At<std::uint8_t*>(label, ITEM_TEXT)
                                    ? At<float>(At<std::uint8_t*>(label, ITEM_TEXT), 0) - At<float>(label, ITEM_RECT_X)
                                    : 0.0f;
        float x = At<float>(items[DLC_FRAME], ITEM_RECT_X) + At<float>(items[DLC_FRAME], ITEM_RECT_W);
        for (const auto tab : {Tab::Campaign, Tab::Zombies})
        {
            float width = At<float>(items[DLC_FRAME], ITEM_RECT_W);
            for (const auto& copy : tabs->copies)
            {
                if (copy.tab != tab)
                    continue;
                At<float>(copy.item, ITEM_RECT_X) = x;
                At<float>(copy.item, ITEM_RECT_CLIENT_X) = x;
                if (const auto text = copy.label ? At<std::uint8_t*>(copy.item, ITEM_TEXT) : nullptr)
                    At<float>(text, 0) = x + textInset;
                if (copy.frame && At<float>(copy.item, ITEM_RECT_W) > 0.0f)
                    width = At<float>(copy.item, ITEM_RECT_W);
            }
            if (HasRows(tab))
                x += width;
        }
    }

    void RestoreTabs()
    {
        if (!tabs)
            return;
        for (const auto& [script, action] : tabs->actions)
            At<const char*>(script, SCRIPT_ACTION) = action;
        if (At<void**>(tabs->menu, MENU_ITEMS) == tabs->newItems.data())
        {
            At<void**>(tabs->menu, MENU_ITEMS) = tabs->items;
            At<std::int32_t>(tabs->menu, MENU_ITEM_COUNT) = tabs->count;
        }
        tabs.reset();
    }

    void Install(const std::filesystem::path& picturesFolder)
    {
        catalog = CampaignMaps::GetCatalog();
        if (!catalog)
            return;
        pictures = picturesFolder;
        static bool listening = false;
        if (!std::exchange(listening, true))
            Events::RegisterListener(EventType::OnFrame, MakeTextures);
        const auto& addresses = GetGameAddresses();
        HookManager::CreateHook(addresses.UI_MapListCount(), (std::uintptr_t)UI_MapListCount_Hook,
                                (std::uintptr_t*)&UI_MapListCount_Trampoline);
        HookManager::CreateHook(addresses.UI_MapListIndex(), (std::uintptr_t)UI_MapListIndex_Hook,
                                (std::uintptr_t*)&UI_MapListIndex_Trampoline);
        HookManager::CreateHook(addresses.UI_MapListEntryName(), (std::uintptr_t)UI_MapListEntryName_Hook,
                                (std::uintptr_t*)&UI_MapListEntryName_Trampoline);
        HookManager::CreateHook(addresses.Item_IsVisible(), (std::uintptr_t)Item_IsVisible_Hook,
                                (std::uintptr_t*)&Item_IsVisible_Trampoline);
        HookManager::CreateHook(addresses.SEH_StringEd_GetString(), (std::uintptr_t)SEH_StringEd_GetString_Hook,
                                (std::uintptr_t*)&SEH_StringEd_GetString_Trampoline);
        HookManager::CreateHook(addresses.DB_FindXAssetHeader(), (std::uintptr_t)DB_FindXAssetHeader_Hook,
                                (std::uintptr_t*)&DB_FindXAssetHeader_Trampoline);
        LOG_INFO("Change Map: campaign levels and Zombies maps, pictures from {}", pictures.string());
    }

    void Frame()
    {
        if (!catalog)
            return;
        AddRows();
        AddTabs();
        PlaceTabs();
        MakePictures();
    }

    void Unload()
    {
        RestoreTabs();
        RestoreRows();
        catalog = nullptr;
        // the pictures' textures and materials are left to the process: the renderer may still have them queued
        for (auto& [_, picture] : picturesMade)
        {
            static_cast<void>(picture.image.release());
            static_cast<void>(picture.textures.release());
            static_cast<void>(picture.finalMaterial.release());
            static_cast<void>(picture.bigMaterial.release());
        }
    }

    State GetState()
    {
        State state;
        state.error = error;
        // (not installed: campaign maps don't load, and the hooks' trampolines are none)
        if (!UI_MapListCount_Trampoline)
        {
            state.error = "not installed";
            return state;
        }
        state.rows = rows.has_value();
        state.tabs = tabs.has_value();
        if (rows)
            state.tableRows = static_cast<std::uint32_t>(rows->table->rowCount);
        state.mapListCount = static_cast<std::uint32_t>(MapListCount());
        state.tab = GetTab();
        const auto count = reinterpret_cast<UI_MapListCount_t>(UI_MapListCount_Hook)();
        for (std::int32_t row = 0; row < count; ++row)
            state.listed.emplace_back(UI_MapListEntryName_Hook(row));
        std::lock_guard lock(mutex);
        for (const auto& [_, picture] : picturesMade)
            state.pictures += picture.finalMaterial ? 1 : 0;
        state.picturesMissing = picturesMissing;
        return state;
    }

    std::int32_t FindMapListIndex(const std::string& map)
    {
        const auto count = std::min(MapListCount(), MAP_LIST_MOST);
        for (std::int32_t i = 0; i < count; ++i)
            if (map == reinterpret_cast<const char*>(MapList() + i * MAP_ENTRY_SIZE + MAP_ENTRY_NAME))
                return i;
        return -1;
    }

    std::string Localize(const std::string& key)
    {
        if (!SEH_StringEd_GetString_Trampoline)
            return "";
        const auto text = SEH_StringEd_GetString_Hook(key.c_str());
        return text ? text : "";
    }

    bool HasPicture(const std::string& map)
    {
        if (!DB_FindXAssetHeader_Trampoline)
            return false;
        const auto material = Logic::GetPictureMaterial(map, false);
        const auto found = static_cast<const Material*>(
            reinterpret_cast<DB_FindXAssetHeader_t>(DB_FindXAssetHeader_Hook)(ASSET_TYPE_MATERIAL, material.c_str()));
        return found && found->name && material == found->name && found == FindPicture(material.c_str());
    }

    bool IsTabSelectedShown(Tab tab)
    {
        if (!tabs)
            return false;
        for (const auto& [item, rule] : tabs->rules)
            if (rule.rule == Rule::Selected && rule.tab == tab)
                return Item_IsVisible_Hook(reinterpret_cast<void*>(GetGameAddresses().uiContext()), 0,
                                           const_cast<void*>(item)) != 0;
        return false;
    }
}  // namespace IWXMVM::T5::MapMenu
