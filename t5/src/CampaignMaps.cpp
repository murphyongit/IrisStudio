#include "StdInclude.hpp"
#include "CampaignMaps.hpp"

#include <deque>
#include <unordered_map>

#include "Addresses.hpp"
#include "Functions.hpp"
#include "ContentPacksInternal.hpp"
#include "PoolGrowth.hpp"
#include "Structures.hpp"
#include "Components/CampaignMapLogic.hpp"
#include "Utilities/HookManager.hpp"
#include "Utilities/MathUtils.hpp"
#include "Utilities/Patches.hpp"

// core/resources/campaign-maps.json, which core embeds (Resources.cpp)
namespace IWXMVM::Resources
{
    extern "C" const unsigned char CAMPAIGN_MAPS_data[];
    extern "C" const unsigned int CAMPAIGN_MAPS_size;
}

namespace IWXMVM::T5::CampaignMaps
{
    using namespace Structures;
    using namespace ContentPacks;
    namespace Logic = Components::CampaignMapLogic;

    // Zombies maps load the same way: their zones are levels as the campaign's are, and
    // single player loads common_zombie before them, whose effects' materials and technique sets they refer to; it
    // loads before the level here too (Logic::Level::zones), registered world-only like the level's own zone.
    //
    // Multiplayer loads any zone of the game, but a campaign level's is the whole mission (AI, weapons, animations,
    // sounds, scripts), which overruns the pools, and multiplayer has no pool for its game_map_sp at all ("Exceeded
    // limit of 1 'game_map_sp' assets"; col_map_sp and col_map_mp share one). So of a level's zone (and its language
    // twin) only its world is registered: the gfxworld, clipmap, comworld, gameworld and map entities, under
    // multiplayer's types (GameWorldSp and GameWorldMp have one layout) and multiplayer's name (maps/mp/<level>.d3dbsp,
    // which its scripts find a map's world by), and what they draw with, by the names tools/t5-maps listed of a zone
    // OAT's Linker made of the world (campaign-maps.json); the lights' definitions and the visions too. The rest is
    // left out as a pack's is (ContentPacks: a copy of its header, its resources released after the load).
    //
    // The map's entities are rewritten for multiplayer (CampaignMapLogic: no AI, nodes, triggers; spawn points), and
    // the scripts multiplayer looks for are registered as the entities come: the map's own (the teamset of its
    // setting, its first fog and vision) and an empty traversal script for each animscript its path nodes name.
    //
    // A multiplayer map's zone also carries the players' body, head and dog scripts with its teamset's models, the
    // RC-XD, the bomb and the HQ crate; the setup builds those out of a multiplayer map of each teamset into a zone of
    // their own (t5m_kit_<teamset>), which is added to the level's zones with its flags, so
    // it loads and goes with it. Its assets that are registered already are shared, as a character zone's.
    //
    // Left-out technique sets share vertex declarations and shaders with kept ones (the loader makes one that several
    // use), and the kit's duplicates with what the kit registered, so the release spares what kept technique sets and
    // models use, as ContentPacks::ReleaseDuplicates does ("Vertex type 2 doesn't have the information used by
    // shader" on the first frame, else).

    inline constexpr std::int32_t ASSET_TYPE_PHYSPRESET = 1;
    inline constexpr std::int32_t ASSET_TYPE_CLIPMAP_SP = 11;
    inline constexpr std::int32_t ASSET_TYPE_CLIPMAP_MP = 12;
    inline constexpr std::int32_t ASSET_TYPE_COMWORLD = 13;
    inline constexpr std::int32_t ASSET_TYPE_GAMEWORLD_SP = 14;
    inline constexpr std::int32_t ASSET_TYPE_GAMEWORLD_MP = 15;
    inline constexpr std::int32_t ASSET_TYPE_MAP_ENTS = 16;
    inline constexpr std::int32_t ASSET_TYPE_LIGHT_DEF = 18;
    inline constexpr std::int32_t ASSET_TYPE_RAWFILE = 36;
    inline constexpr const char* DEFAULT_MATERIAL = "$default";

    // the types of campaign-maps.json's lists (the effects' definitions with the world)
    inline constexpr std::array<std::pair<std::string_view, std::int32_t>, 6> WORLD_TYPES{{
        {"physpreset", ASSET_TYPE_PHYSPRESET},
        {"xmodel", ASSET_TYPE_XMODEL},
        {"material", ASSET_TYPE_MATERIAL},
        {"techniqueset", ASSET_TYPE_TECHNIQUE_SET},
        {"image", ASSET_TYPE_IMAGE},
        {"fx", ASSET_TYPE_FX},
    }};

    struct RawFileHeader
    {
        const char* name;
        std::int32_t len;
        const char* buffer;
    };
    struct MapEntsHeader
    {
        const char* name;
        char* entityString;
        std::int32_t numEntityChars;
    };
    struct XZoneInfo
    {
        const char* name;
        std::int32_t allocFlags;
        std::int32_t freeFlags;
    };

    struct Map
    {
        const Logic::Level* level = nullptr;
        std::string worldName;    // as single player names it
        std::string mpWorldName;  // as it is registered
        std::array<std::unordered_set<std::string>, ASSET_TYPE_COUNT> keep;
        std::string kit;  // the kit zone it loads with, empty for none
        Logic::Rewrite rewrite;
        struct Script
        {
            std::string name;
            std::vector<std::uint8_t> buffer;
            RawFileHeader header{};
        };
        std::deque<Script> scripts;
        LoadResult result;
        std::set<std::string> zones;  // of it, that have started loading
        bool finished = false;
        std::uint64_t load = 0;  // how manyth level to finish loading it was (its effects spawn once for each)
        std::chrono::steady_clock::time_point started;
        // the copies of what it left out (see KeepHeader), from the load's first on, and the kit's duplicates (copies
        // of what it had that was registered, whose resources are released)
        std::size_t copiesBefore = 0;
        std::vector<std::unique_ptr<std::uint8_t[]>> headerCopies;
        std::vector<std::unique_ptr<std::uint8_t[]>> duplicateCopies;
        std::vector<std::pair<std::int32_t, void*>> duplicates;
        // what it registered of technique sets and models (whose shaders, declarations and surfaces the left-out may
        // share), and the pack's technique sets before it (what it registered late goes there, see RegisterCopy)
        std::vector<const MaterialTechniqueSet*> techniqueSets;
        std::vector<const XModel*> models;
        std::unordered_set<const MaterialTechniqueSet*> packTechniqueSetsBefore;
        std::vector<std::string> kitNotes;
    };

    std::mutex mapMutex;
    // The finished level's areas for the UI, which asks each frame on the render thread: it can't wait on mapMutex, held
    // while a load waits on that thread
    std::atomic<std::shared_ptr<const Types::LevelAreas>> levelAreas;
    std::atomic<std::shared_ptr<const Logic::LevelOrigins>> levelOrigins;
    std::unique_ptr<Map> map;                     // loading or loaded
    std::uint64_t loads = 0;                      // levels that finished loading
    std::vector<std::unique_ptr<Map>> goneMaps;  // replaced by another, freed once their level has gone
    const Logic::Catalog* catalog = nullptr;
    std::filesystem::path kitsFolder;
    std::string error = "not installed";
    std::optional<Patches::Patch<5>> loadZonesCall;
    // single player's Rezurrection IWDs mounted while a map of theirs loads (see PrepareMapLoad)
    std::optional<Patches::Patch<1>> rezurrectionIwds;
    // how many names db_patchZoneNames has room for (the loader compares 0x30 bytes of pointers)
    inline constexpr std::size_t PATCH_ZONE_NAMES = 12;
    // the allocation flags of a level's patch (<level>_patch), freed with the map after the level's own (0x800) and
    // its language twin (0x1000)
    inline constexpr std::int32_t LEVEL_PATCH_ZONE_FLAGS = 0x2000;
    // free flags that free nothing (the high bit: the entry's own free flags, not those of its allocation flags)
    inline constexpr std::int32_t FREE_NOTHING = static_cast<std::int32_t>(0x80000000u);
    typedef void(__cdecl* DB_LoadXAssets_t)(XZoneInfo* zones, std::uint32_t count, std::int32_t sync);
    DB_LoadXAssets_t LoadXAssets = nullptr;

    // Starts a campaign map (with mapMutex held): the one before goes, after the level it was of; if it hadn't finished
    // loading (an error dropped the map), what it left out refers to the zone's memory, which is gone, so it is
    // forgotten, not released
    Map& StartMap(const Logic::Level& level, std::string kit)
    {
        if (map)
        {
            if (!map->finished)
            {
                unloadableCopies.clear();
                RemoveSoundCopies();
                map->duplicates.clear();
            }
            goneMaps.push_back(std::move(map));
        }
        map = std::make_unique<Map>();
        map->level = &level;
        map->worldName = Logic::GetWorldName(level.name);
        map->mpWorldName = Logic::GetMultiplayerWorldName(level.name);
        map->kit = std::move(kit);
        map->started = std::chrono::steady_clock::now();
        map->copiesBefore = headerCopies.size();
        map->packTechniqueSetsBefore = packTechniqueSets;
        for (const auto& [typeName, names] : level.world)
        {
            const auto type = std::ranges::find(WORLD_TYPES, typeName, &std::pair<std::string_view, std::int32_t>::first);
            if (type != WORLD_TYPES.end())
                map->keep[type->second].insert(names.begin(), names.end());
        }
        return *map;
    }

    // registers a script as zones have them (a rawfile), unless one of that name is
    void AddScript(Map& loading, const std::string& name, std::string_view text)
    {
        if (FindRegistered(ASSET_TYPE_RAWFILE, name.c_str()))
        {
            loading.result.notes.push_back("a script of that name is registered: " + name);
            return;
        }
        auto& script = loading.scripts.emplace_back();
        script.name = name;
        auto file = Logic::MakeScriptFile(name, text);
        script.buffer = std::move(file.buffer);
        script.header = {script.name.c_str(), file.length, reinterpret_cast<const char*>(script.buffer.data())};
        AddXAsset(ASSET_TYPE_RAWFILE, &script.header);
    }

    Logic::RewriteOptions rewriteOptions;

    // the map's entities as multiplayer spawns them, and the scripts it looks for
    void RewriteEntities(Map& loading, MapEntsHeader& header)
    {
        auto options = rewriteOptions;
        options.keptDoors = loading.level->closedDoors;
        loading.rewrite = Logic::RewriteEntities(
            std::string_view(header.entityString, header.numEntityChars),
            [&](const std::string& model) {
                // (the zones list models in lower case and 89 entities name theirs with capitals,
                // dest_rus_console_lg_A_d0: the game doesn't mind, and they were lost)
                auto lower = model;
                std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                return loading.keep[ASSET_TYPE_XMODEL].contains(model) || loading.keep[ASSET_TYPE_XMODEL].contains(lower) ||
                       FindRegistered(ASSET_TYPE_XMODEL, model.c_str()) != nullptr;
            },
            options);
        header.entityString = loading.rewrite.entities.data();
        header.numEntityChars = static_cast<std::int32_t>(loading.rewrite.entities.size() + 1);

        const auto players = catalog->playerModels.find(loading.level->teamset);
        AddScript(loading, Logic::GetMapScriptName(loading.level->name),
                  Logic::MakeMapScript(*loading.level, players != catalog->playerModels.end()
                                                           ? players->second
                                                           : std::vector<std::string>{}));
        AddScript(loading, Logic::GetClientScriptName(loading.level->name), Logic::MakeClientScript(*loading.level));
        for (const auto& traversal : loading.rewrite.traversals)
            AddScript(loading, Logic::GetTraversalScriptName(traversal), Logic::GetEmptyScript());
    }

    bool IsWorldType(std::int32_t type)
    {
        return type == ASSET_TYPE_CLIPMAP_SP || type == ASSET_TYPE_CLIPMAP_MP || type == ASSET_TYPE_COMWORLD ||
               type == ASSET_TYPE_GAMEWORLD_SP || type == ASSET_TYPE_GAMEWORLD_MP || type == ASSET_TYPE_MAP_ENTS ||
               type == ASSET_TYPE_GFXWORLD;
    }

    void* AddLevelAsset(Map& loading, std::int32_t type, void* header)
    {
        auto& counts = loading.result;
        const std::string name = GetAssetName(type, header);
        if (IsWorldType(type))
        {
            if (_stricmp(name.c_str(), loading.worldName.c_str()) == 0)
                *static_cast<const char**>(header) = loading.mpWorldName.c_str();
            else
                counts.notes.push_back(std::format("a world asset of type {} named {}", type, name));
            if (type == ASSET_TYPE_MAP_ENTS)
                RewriteEntities(loading, *static_cast<MapEntsHeader*>(header));
            ++counts.kept[type];
            return AddXAsset(type == ASSET_TYPE_CLIPMAP_SP     ? ASSET_TYPE_CLIPMAP_MP
                             : type == ASSET_TYPE_GAMEWORLD_SP ? ASSET_TYPE_GAMEWORLD_MP
                                                               : type,
                             header);
        }

        // a reference to an asset of another zone (its name after a comma): the asset multiplayer has of that name, as
        // the game's registration gives (the world's static models use ",tag_origin"), else it is left out
        if (name.starts_with(','))
        {
            if (const auto registered = FindRegistered(type, name.c_str() + 1))
            {
                ++counts.shared[type];
                return registered;
            }
            // a material multiplayer hasn't (single player's common has it: Der Riese's teleporter, the eyes of Five's
            // models): the default material, as the game gives for a material it can't find; an empty copy has no
            // technique set, and drawing what used it crashed the game
            if (type == ASSET_TYPE_MATERIAL)
                if (const auto fallback = FindRegistered(type, DEFAULT_MATERIAL))
                {
                    ++counts.shared[type];
                    counts.notes.push_back("a material multiplayer hasn't, drawn with the default: " + name.substr(1));
                    return fallback;
                }
            ++counts.skipped[type];
            return KeepHeader(type, header);
        }
        const bool keep = type == ASSET_TYPE_LIGHT_DEF || (type == ASSET_TYPE_RAWFILE && name.starts_with("vision/")) ||
                          loading.keep[type].contains(name);
        if (!keep)
        {
            ++counts.skipped[type];
            return KeepHeader(type, header);
        }
        if (const auto registered = FindRegistered(type, name.c_str()))
        {
            ++counts.shared[type];
            return registered;
        }
        // DB_AllocXAssetHeader ends the game when a pool is full
        if (!PoolHasRoom(type))
        {
            ++counts.poolFull[type];
            return KeepHeader(type, header);
        }
        ++counts.kept[type];
        if (type == ASSET_TYPE_XMODEL)
            RegisterMaterials(*static_cast<XModel*>(header));
        else if (type == ASSET_TYPE_MATERIAL)
            RegisterTextures(*static_cast<Material*>(header));
        const auto added = AddXAsset(type, header);
        if (type == ASSET_TYPE_TECHNIQUE_SET)
            loading.techniqueSets.push_back(static_cast<const MaterialTechniqueSet*>(added));
        else if (type == ASSET_TYPE_XMODEL)
            loading.models.push_back(static_cast<const XModel*>(added));
        return added;
    }

    // Everything the kit has is wanted: what is registered by that name already is shared (its resources released
    // after the load), the rest registered
    void* AddKitAsset(Map& loading, std::int32_t type, void* header)
    {
        auto& counts = loading.result;
        const std::string name = GetAssetName(type, header);
        if (const auto registered = FindRegisteredEntry(type, name.c_str()))
        {
            ++counts.shared[type];
            if (HasResources(type))
            {
                loading.duplicateCopies.push_back(CopyHeader(type, header));
                loading.duplicates.emplace_back(type, loading.duplicateCopies.back().get());
            }
            return const_cast<void*>(registered->header);
        }
        if (!PoolHasRoom(type))
        {
            ++counts.poolFull[type];
            loading.kitNotes.push_back(std::format("no room for {} {}", type, name));
            return KeepHeader(type, header);
        }
        ++counts.kept[type];
        const auto added = AddXAsset(type, header);
        if (type == ASSET_TYPE_TECHNIQUE_SET)
            loading.techniqueSets.push_back(static_cast<const MaterialTechniqueSet*>(added));
        else if (type == ASSET_TYPE_XMODEL)
            loading.models.push_back(static_cast<const XModel*>(added));
        return added;
    }

    std::optional<void*> AddAsset(std::int32_t type, void* header)
    {
        if (!catalog || !loadZonesCall)
            return std::nullopt;
        const auto index = *reinterpret_cast<const std::int32_t*>(GetGameAddresses().g_zoneIndex());
        if (index <= 0 || index >= 33)
            return std::nullopt;
        const auto zone = GetZoneName(index);
        // (most zones are no campaign level's: those are told apart before taking the lock)
        const auto level = Logic::FindLevelOfZone(*catalog, zone);
        if (!level && !zone.starts_with(Logic::KIT_ZONE_PREFIX) && !Logic::IsLoadedWithAny(*catalog, zone))
            return std::nullopt;

        std::lock_guard lock(mapMutex);
        const bool kit = map && !map->kit.empty() && zone == map->kit;
        // a zone that loads with it (common_zombie, which its mode loads before it, the patches the game loads with
        // a zone): its world's assets of it, as of the level's own, so no weapons or animations of single player
        const bool shared = map && !map->finished && Logic::IsLoadedWith(*map->level, zone);
        if (!level && !kit && !shared)
            return std::nullopt;
        // a level loaded some other way than the map's zones (see LoadLevelZones), without its kit
        if (level && (!map || map->level != level || map->finished))
            StartMap(*level, {});
        map->zones.emplace(zone);
        const auto before = result;
        result = &map->result;
        const auto added = kit ? AddKitAsset(*map, type, header) : AddLevelAsset(*map, type, header);
        result = before;
        return added;
    }

    // The Direct3D objects of what the level left out and of the kit's duplicates, released as unloading them would,
    // but on the render thread and sparing the shaders, vertex declarations and surfaces that what it registered uses
    // (see ContentPacks::ReleaseDuplicates)
    void ReleaseCopies(Map& loaded)
    {
        std::unordered_set<const void*> kept;
        for (const auto techniqueSet : loaded.techniqueSets)
            ForEachPass(*techniqueSet, [&](const MaterialPass& pass) {
                kept.insert({pass.vertexDecl, pass.vertexShader, pass.pixelShader});
            });
        for (const auto model : loaded.models)
            kept.insert(model->surfs);

        typedef void(__cdecl * DB_XAssetUnloadHandler_t)(void* header);
        const auto handlers = reinterpret_cast<DB_XAssetUnloadHandler_t*>(GetGameAddresses().DB_XAssetUnloadHandlers());
        std::unordered_set<const void*> released;
        const auto release = [&](void*& object) {
            if (!object)
                return;
            ReleaseOnRenderThread(object);
            object = nullptr;
            ++loaded.result.releasedShaders;
        };
        auto copies = std::move(unloadableCopies);
        unloadableCopies.clear();
        copies.insert(copies.end(), loaded.duplicates.begin(), loaded.duplicates.end());
        for (const auto& [type, header] : copies)
        {
            if (type == ASSET_TYPE_IMAGE)
            {
                auto& image = *static_cast<GfxImage*>(header);
                if (image.texture)
                {
                    ++loaded.result.releasedTextures;
                    loaded.result.releasedTextureBytes += image.cardMemory;
                }
                // never added, so never streamed in (see ContentPacks::ReleaseCopies)
                image.streaming = false;
                if (handlers[type])
                    handlers[type](&image);
            }
            else if (type == ASSET_TYPE_TECHNIQUE_SET)
            {
                ForEachPass(*static_cast<const MaterialTechniqueSet*>(header), [&](const MaterialPass& pass) {
                    for (const auto shader : {pass.vertexShader, pass.pixelShader})
                    {
                        if (!shader || !released.insert(shader).second)
                            continue;
                        if (kept.contains(shader))
                            loaded.result.keptShaders += shader->program ? 1 : 0;
                        else
                            release(shader->program);
                    }
                    const auto declaration = pass.vertexDecl;
                    if (!declaration || !released.insert(declaration).second || !declaration->isLoaded)
                        return;
                    if (kept.contains(declaration))
                    {
                        ++loaded.result.keptShaders;
                        return;
                    }
                    declaration->isLoaded = false;
                    for (std::size_t i = 0; i < RELEASED_DECLARATION_COUNT; ++i)
                        release(declaration->declarations[i]);
                });
            }
            else if (type == ASSET_TYPE_XMODEL)
            {
                auto& model = *static_cast<XModel*>(header);
                if (!kept.contains(model.surfs))
                    ReleaseModelBuffersOnRenderThread(model);
            }
        }
        loaded.duplicates.clear();
        loaded.duplicateCopies.clear();
    }

    void FinishLoad()
    {
        std::lock_guard lock(mapMutex);
        // a level gone with the map: what was made for it goes too
        std::erase_if(goneMaps, [](const auto& gone) { return !IsZoneLoaded(gone->level->name); });
        if (!map)
            return;
        if (map->finished)
        {
            if (!IsZoneLoaded(map->level->name))
            {
                map.reset();
                levelAreas.store(nullptr);
                levelOrigins.store(nullptr);
            }
            return;
        }
        // (the main thread runs no frame while the server or client loads a map's zones)
        if (!IsZoneLoaded(map->level->name) || (!map->kit.empty() && !map->zones.contains(map->kit)))
            return;

        auto& loaded = *map;
        RemoveSoundCopies();
        ReleaseCopies(loaded);
        // what was made for it goes with it
        for (auto i = loaded.copiesBefore; i < headerCopies.size(); ++i)
        {
            copies.erase(headerCopies[i].get());
            loaded.headerCopies.push_back(std::move(headerCopies[i]));
        }
        headerCopies.resize(std::min(headerCopies.size(), loaded.copiesBefore));
        // the technique sets registered late, which go with the level
        std::erase_if(packTechniqueSets, [&](const auto techniqueSet) {
            return !loaded.packTechniqueSetsBefore.contains(techniqueSet);
        });
        loaded.finished = true;
        loaded.load = ++loads;
        auto areas = std::make_shared<Types::LevelAreas>();
        areas->level = loaded.level->name;
        areas->nodes = loaded.rewrite.nodes;
        for (const auto& area : loaded.rewrite.areas)
            areas->areas.push_back({area.spot, area.yaw});
        levelAreas.store(std::move(areas));
        levelOrigins.store(std::make_shared<const Logic::LevelOrigins>(loaded.rewrite.origins));
        // (mounted: the next start of the file system skips them again, unless it loads such a map)
        if (rezurrectionIwds)
            rezurrectionIwds->Revert();
        loaded.result.loaded = true;
        loaded.result.milliseconds =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - loaded.started)
                .count();
        LOG_INFO("The {} {} loaded as a multiplayer map in {} ms, with {}: {} entities kept, {} left out, "
                 "{} spawn points, {} scripts; released {} textures ({} MB), {} shaders",
                 Logic::GetModeName(loaded.level->mode), loaded.level->name, loaded.result.milliseconds,
                 loaded.kit.empty() ? "no kit" : loaded.kit,
                 loaded.rewrite.kept, loaded.rewrite.dropped, loaded.rewrite.spawns, loaded.scripts.size(),
                 loaded.result.releasedTextures, loaded.result.releasedTextureBytes >> 20, loaded.result.releasedShaders);
    }

    // how long a map's load waits for the pools: well over what a zone's load takes. A multiplayer map can go on without
    // them, and they grow under it. A level can't, and there is no way back from here: the game's Com_Error, dropped or
    // fatal, left it on the loading screen for good, its main thread asleep. So it says why and closes the game, on a
    // path no start has come near.
    inline constexpr auto MAP_POOLS_WAIT = std::chrono::seconds(60);
    inline constexpr auto LEVEL_POOLS_WAIT = std::chrono::seconds(120);

    // The map's zones load here (Com_LoadLevelZones' call of DB_LoadXAssets, as the server and the client load a
    // map): for a campaign level, its kit zone is added after it, with the level's flags, so it goes with the level
    void __cdecl LoadLevelZones(XZoneInfo* zones, std::uint32_t count, std::int32_t sync)
    {
        const auto level = count > 0 && zones[count - 1].name
                               ? Logic::FindLevelOfZone(*catalog, zones[count - 1].name)
                               : nullptr;
        // any map's zones wait for the pools, so that they grow before the first map (see PoolGrowth)
        const bool grown = PoolGrowth::WaitUntilGrown(
            std::chrono::duration_cast<std::chrono::milliseconds>(level ? LEVEL_POOLS_WAIT : MAP_POOLS_WAIT));
        if (level && !grown)
        {
            LOG_ERROR("The {} {} can't load: the pools haven't grown", Logic::GetModeName(level->mode), level->name);
            MessageBoxA(nullptr, "Campaign and Zombies maps couldn't get ready, so the game has to close. Start it again.",
                        "T5iris", MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST);
            TerminateProcess(GetCurrentProcess(), 1);
        }
        if (!level)
        {
            LoadXAssets(zones, count, sync);
            return;
        }

        // (the loader copies the names: they need to last as long as the call)
        static std::string kit;
        static std::vector<XZoneInfo> withKit;
        kit = Logic::GetKitZone(level->teamset);
        std::error_code missing;
        const bool hasKit = std::filesystem::is_regular_file(kitsFolder / (kit + ".ff"), missing);
        {
            std::lock_guard lock(mapMutex);
            StartMap(*level, hasKit ? kit : std::string());
        }
        // The zones its mode loads before it (common_zombie) load before the level's own and its language twin, as
        // single player loads them, so that what the level refers to of them is registered as it comes. Freeing goes
        // by these entries: each frees the loaded zones of its allocation flags and above (unless its free flags say
        // otherwise, with 0x80000000), in passes of the flags in the game's order (0x800, then the language twins'
        // 0x1000, then the patches' 0x2000), each pass from the last loaded down, and memory has to be freed the other
        // way round from how it was taken ("free does not match allocation" else). So common_zombie takes a level
        // patch's flags, which the level's entry frees after the level and its language twin, and frees nothing of
        // its own (with its own entry first, the map before's common_zombie was freed under its level). The loader
        // adds no language twin or patch to a zone of those flags, and finds it in zone/Common (a zone of the twins'
        // flags it looks for in zone/<language>).
        std::uint32_t first = 0;
        while (first < count && Logic::FindLevelOfZone(*catalog, zones[first].name ? zones[first].name : "") != level)
            ++first;
        withKit.assign(zones, zones + first);
        for (const auto& shared : level->zones)
            if (FindZoneFile(shared))
                withKit.push_back({shared.c_str(), LEVEL_PATCH_ZONE_FLAGS, FREE_NOTHING});
            else
                LOG_ERROR("The {} {} loads without {}, which the game folder doesn't have: what it takes of it is left "
                          "out", Logic::GetModeName(level->mode), level->name, shared);
        withKit.insert(withKit.end(), zones + first, zones + count);
        if (!hasKit)
            LOG_ERROR("The {} {} loads without the players' kit, {}: Iris Studio's setup builds it "
                      "(players are invisible, and the game stops for want of their body scripts)",
                      Logic::GetModeName(level->mode), level->name, kit);
        else
            withKit.push_back({kit.c_str(), zones[count - 1].allocFlags, 0});
        LOG_INFO("The {} {} loads as a multiplayer map, with {}", Logic::GetModeName(level->mode), level->name,
                 hasKit ? kit : "no kit");

        // The game loads a zone's patch after it (<name>_patch, allocation flags 0x2000) when the zone has one, as
        // the Zombies maps do (multiplayer's maps have none), where it was freed after the level it sits on ("free does
        // not match allocation"). They patch the Zombies mode's scripts and weapons, not the world, so they don't load:
        // while the level's zones are asked for, the patch table's names of them read as none.
        const auto patchNames = reinterpret_cast<const char**>(GetGameAddresses().db_patchZoneNames());
        std::array<const char*, PATCH_ZONE_NAMES> patchNamesBefore{};
        static constexpr const char* NO_PATCH = "";
        const auto listed = [&](const char* name) {
            const auto named = [&](const XZoneInfo& zone) { return zone.name && _stricmp(zone.name, name) == 0; };
            return std::ranges::any_of(withKit, named);
        };
        for (std::size_t i = 0; i < PATCH_ZONE_NAMES; ++i)
        {
            patchNamesBefore[i] = patchNames[i];
            if (patchNames[i] && listed(patchNames[i]))
                patchNames[i] = NO_PATCH;
        }
        LoadXAssets(withKit.data(), static_cast<std::uint32_t>(withKit.size()), sync);
        std::ranges::copy(patchNamesBefore, patchNames);
    }

    // The map packs the game owns (Content_IsOwned, by the DLC bit of a map in the game's table of maps): Plutonium
    // has the multiplayer packs owned (bits 8, 0x10 and 0x20, which bring Ascension, Call of the Dead and Shangri-La
    // too), not the two with only Zombies maps, the World at War remakes (bit 4: Nacht der Untoten, Verruckt, Shi No
    // Numa, Der Riese) and Moon's (0x40), so devmap refused those ("You do not have this map") with their zones in the
    // game folder. Such a pack counts as owned when the folder has a Zombies map's zone of it; a pack with a
    // multiplayer map stays as the game has it.
    struct MapTableEntry
    {
        const char* name;
        std::int32_t mode;  // 1 multiplayer, 2 campaign, 6 Zombies
        std::int32_t pack;  // its DLC bit, 2 the game's own
        const char* presence;
    };
    inline constexpr std::int32_t MAP_MODE_MULTIPLAYER = 1;
    inline constexpr std::int32_t MAP_MODE_ZOMBIES = 6;
    inline constexpr std::size_t MAP_TABLE_MOST = 256;
    std::set<std::int32_t> ownedZombiesPacks;

    typedef bool(__cdecl* Content_IsOwned_t)(std::int32_t pack);
    Content_IsOwned_t Content_IsOwned_Trampoline = nullptr;
    bool __cdecl Content_IsOwned_Hook(std::int32_t pack)
    {
        return Content_IsOwned_Trampoline(pack) || ownedZombiesPacks.contains(pack);
    }

    // Moon's and the remakes' images streamed from files are in single player's Rezurrection IWDs (main/iw_36.iwd to
    // iw_41.iwd), which multiplayer's file system skips ("Skip DLC5 SP only"): Moon stopped with "Couldn't load image
    // '~~-gp_zom_moon_jump_pad_s-rgb~e73eca3e'". Mounted for all maps they would replace files multiplayer has (247 of
    // theirs are in other IWDs: sounds, textures), so they are mounted only while a map of such a pack loads: the file
    // system starts again as each map or film loads, and the skip's first number reads 42 (none skipped) from the map
    // command or a film's PLAY of such a map until it has loaded.
    std::set<std::string> ownedPackMaps;  // the Zombies maps of the packs counted as owned
    inline constexpr std::uint8_t NO_IWD_SKIPPED = 42;

    void PrepareMapLoad(const std::string& map)
    {
        if (!rezurrectionIwds)
            return;
        if (!ownedPackMaps.contains(map))
        {
            rezurrectionIwds->Revert();
            return;
        }
        if (!rezurrectionIwds->IsApplied())
            LOG_INFO("Single player's Rezurrection files (main/iw_36.iwd to iw_41.iwd) are mounted for {}", map);
        rezurrectionIwds->Apply();
    }

    // The map command of a campaign level or Zombies map runs as devmap (cheats on), as they have loaded so far: the
    // Change Map menu's START MATCH runs "map <map>"
    typedef void(__cdecl* SV_Map_f_t)();
    SV_Map_f_t SV_Map_f_Trampoline = nullptr;
    std::uintptr_t svCmdNesting = 0, svCmdArgc = 0, svCmdArgv = 0;
    void __cdecl SV_Map_f_Hook()
    {
        static const char DEVMAP[] = "devmap";
        const auto nesting = *reinterpret_cast<const std::int32_t*>(svCmdNesting);
        if (nesting >= 0 && nesting < 8 && reinterpret_cast<const std::int32_t*>(svCmdArgc)[nesting] > 1)
        {
            const auto argv = reinterpret_cast<const char** const*>(svCmdArgv)[nesting];
            PrepareMapLoad(argv[1]);
            if (catalog && catalog->FindLevel(argv[1]) && _stricmp(argv[0], "map") == 0)
            {
                LOG_INFO("{} {} runs as devmap", argv[0], argv[1]);
                argv[0] = DEVMAP;
            }
        }
        SV_Map_f_Trampoline();
    }

    void HookMapCommand()
    {
        const auto svMap = GetGameAddresses().SV_Map_f();
        // (mov eax, [nesting]; mov ecx, [eax*4 + argc]; ...; mov edx, [eax*4 + argv])
        svCmdNesting = *reinterpret_cast<const std::uint32_t*>(svMap + 1);
        svCmdArgc = *reinterpret_cast<const std::uint32_t*>(svMap + 8);
        svCmdArgv = *reinterpret_cast<const std::uint32_t*>(svMap + 0x1B);
        HookManager::CreateHook(svMap, (std::uintptr_t)SV_Map_f_Hook, (std::uintptr_t*)&SV_Map_f_Trampoline);
    }

    void OwnZombiesPacks()
    {
        const auto table = reinterpret_cast<const MapTableEntry*>(GetGameAddresses().com_mapTable());
        std::set<std::int32_t> multiplayer;
        std::map<std::int32_t, std::vector<std::string>> zombies;
        for (std::size_t i = 0; i < MAP_TABLE_MOST && table[i].name && *table[i].name; ++i)
        {
            const auto& entry = table[i];
            if (entry.mode == MAP_MODE_MULTIPLAYER)
                multiplayer.insert(entry.pack);
            const auto level = catalog->FindLevel(entry.name);
            if (entry.mode == MAP_MODE_ZOMBIES && level && level->mode == Logic::Mode::Zombies &&
                FindZoneFile(entry.name))
                zombies[entry.pack].push_back(entry.name);
        }
        Content_IsOwned_t isOwned = reinterpret_cast<Content_IsOwned_t>(GetGameAddresses().Content_IsOwned());
        for (const auto& [pack, maps] : zombies)
        {
            if (multiplayer.contains(pack) || isOwned(pack))
                continue;
            ownedZombiesPacks.insert(pack);
            ownedPackMaps.insert(maps.begin(), maps.end());
            LOG_INFO("Zombies maps of a pack the game doesn't have owned load, as the game folder has them: {}",
                     std::accumulate(std::next(maps.begin()), maps.end(), maps.front(),
                                     [](std::string all, const std::string& map) { return all + ", " + map; }));
        }
        if (ownedZombiesPacks.empty())
            return;
        HookManager::CreateHook(GetGameAddresses().Content_IsOwned(), (std::uintptr_t)Content_IsOwned_Hook,
                                (std::uintptr_t*)&Content_IsOwned_Trampoline);

        const auto skippedFirst = GetGameAddresses().FS_SkippedIwdsFirst();
        if (*reinterpret_cast<const std::uint8_t*>(skippedFirst) != 36)
        {
            LOG_ERROR("The file system's skip of single player's IWDs isn't the code expected: Moon and the remakes "
                      "miss their streamed images");
            return;
        }
        rezurrectionIwds.emplace(skippedFirst, std::array{NO_IWD_SKIPPED}, Patches::PatchApplySetting::Deferred);
    }

    void Install(const std::filesystem::path& zonesFolder)
    {
        kitsFolder = zonesFolder;
        if (!PoolGrowth::WillGrow())
        {
            error = "the pools couldn't grow (see the log)";
            LOG_ERROR("Campaign maps don't load: {}", error);
            return;
        }
        try
        {
            static const auto read = Logic::ReadCatalog(std::string_view(
                reinterpret_cast<const char*>(Resources::CAMPAIGN_MAPS_data), Resources::CAMPAIGN_MAPS_size));
            catalog = &read;
        }
        catch (const std::exception& e)
        {
            error = std::string("the catalog doesn't read: ") + e.what();
            LOG_ERROR("Campaign maps don't load: {}", error);
            return;
        }

        const auto callAddress = GetGameAddresses().LoadLevelZones_LoadXAssets();
        const auto target = callAddress + 5 + *reinterpret_cast<const std::int32_t*>(callAddress + 1);
        if (*reinterpret_cast<const std::uint8_t*>(callAddress) != 0xE8 || target != GetGameAddresses().DB_LoadXAssets())
        {
            error = "the map's zones aren't loaded with the call expected";
            LOG_ERROR("Campaign maps don't load: {}", error);
            return;
        }
        LoadXAssets = reinterpret_cast<DB_LoadXAssets_t>(target);
        std::array<std::uint8_t, 5> call{0xE8};
        const auto hookOffset =
            static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(LoadLevelZones) - (callAddress + 5));
        std::memcpy(&call[1], &hookOffset, sizeof(hookOffset));
        // reverted when the mod is unloaded
        loadZonesCall.emplace(callAddress, call, Patches::PatchApplySetting::Immediately);
        error.clear();
        LOG_INFO("Campaign maps load with devmap: {} levels", catalog->levels.size());
        OwnZombiesPacks();
        HookMapCommand();
    }

    const Logic::Catalog* GetCatalog()
    {
        return catalog;
    }

    bool IsInGameFolder(const Logic::Level& level)
    {
        return FindZoneFile(level.name).has_value();
    }

    void Unload()
    {
        std::lock_guard lock(mapMutex);
        // left to the process: the level's zone points to them until it goes
        const auto leave = [](Map& left) {
            for (auto& copy : left.headerCopies)
                static_cast<void>(copy.release());
            for (auto& copy : left.duplicateCopies)
                static_cast<void>(copy.release());
        };
        if (map)
        {
            leave(*map);
            static_cast<void>(map.release());
        }
        for (auto& gone : goneMaps)
        {
            leave(*gone);
            static_cast<void>(gone.release());
        }
        goneMaps.clear();
        catalog = nullptr;
        rezurrectionIwds.reset();
    }

    // The level's ambient effects. A campaign level's client script plays its createfx's
    // oneshots once as the map starts, most 15 s into their lives, and its loops again each delay once a play has
    // ended (clientscripts\_fx.csc); multiplayer's own maps do the same in their scripts, which campaign levels don't
    // have here. So the mod does, as the client's map starts (the level's load after the one before): definitions that
    // loop (FxEffectDef +0x10: looping elements) kept, to hide them when asked, the rest let go to end by themselves.
    // Like a multiplayer map's, they stay through rewinds and jumps (Actors::StopEffects ends only what actors started,
    // Hooks::Effects only what the demo spawns again), as the campaign's do. Once all that were kept are gone, the
    // client's effects were cleared (a map started again without a load): they spawn again.
    inline constexpr std::size_t FX_LOOPING_ELEMS = 0x10;
    inline constexpr std::chrono::milliseconds MAP_EFFECTS_CHECK{1000};
    struct PlayedEffect
    {
        std::int32_t handle = 0;
        std::int32_t spawnedAt = 0;  // the client's time
        bool kept = false;
        bool spawned = false;  // since the map started (or it was shown again)
    };
    struct MapEffectsPlay
    {
        const Logic::Level* level = nullptr;
        std::uint64_t load = 0;
        std::int32_t startedAt = 0;
        std::vector<void*> definitions;  // by kind, null for one the map hasn't
        std::vector<PlayedEffect> played;  // by effect
        std::vector<std::uint32_t> alive;  // by kind, as last checked
        std::chrono::steady_clock::time_point checkedAt;
    };
    std::mutex mapEffectsMutex;
    MapEffectsPlay mapEffects;
    std::map<std::string, std::set<std::string>, std::less<>> hiddenMapEffects;  // by level, by definition

    bool IsMapEffectShown(const MapEffectsPlay& play, const Logic::MapEffect& effect)
    {
        const auto hidden = hiddenMapEffects.find(play.level->name);
        return play.definitions[effect.kind] &&
               (hidden == hiddenMapEffects.end() || !hidden->second.contains(play.level->effectKinds[effect.kind]));
    }

    bool IsEffectAlive(std::int32_t handle)
    {
        typedef void*(__cdecl * FX_EffectFromHandle_t)(void* system, std::int32_t handle);
        const auto system = *reinterpret_cast<void**>(GetGameAddresses().fx_system());
        return handle && system &&
               reinterpret_cast<FX_EffectFromHandle_t>(GetGameAddresses().FX_EffectFromHandle())(system, handle);
    }

    void SpawnMapEffect(MapEffectsPlay& play, std::size_t index, std::int32_t msecBegin, std::int32_t now)
    {
        const auto& effect = play.level->effects[index];
        auto& played = play.played[index];
        const auto definition = play.definitions[effect.kind];
        const auto axes = MathUtils::AnglesToAxis(effect.angles);
        float axis[3][3];
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                axis[i][j] = axes[i][j];
        const float origin[3] = {effect.origin.x, effect.origin.y, effect.origin.z};
        played.kept = *reinterpret_cast<const std::int32_t*>(static_cast<const std::uint8_t*>(definition) +
                                                             FX_LOOPING_ELEMS) > 0;
        played.handle = Functions::FX_SpawnWorldEffect(definition, origin, axis, msecBegin, played.kept);
        played.spawnedAt = now;
        played.spawned = true;
    }

    // (with mapEffectsMutex held)
    void EndMapEffects(MapEffectsPlay& play)
    {
        for (const auto& played : play.played)
            if (played.handle)
                Functions::FX_KillEffect(0, played.handle);
        play = {};
    }

    void FixClientScript(std::int32_t type, void* header)
    {
        if (type != ASSET_TYPE_RAWFILE)
            return;
        auto& file = *static_cast<RawFileHeader*>(header);
        if (!file.name || !Logic::IsFixedClientScript(file.name))
            return;
        const auto text = Logic::UnpackScript(
            {reinterpret_cast<const std::uint8_t*>(file.buffer), static_cast<std::size_t>(file.len)});
        const auto fixed = text ? Logic::FixClientScript(*text) : std::nullopt;
        if (!fixed)
        {
            LOG_ERROR("{} isn't the script expected: it stays as it is", file.name);
            return;
        }
        // never freed: the zone's asset points at it while the game runs, which may outlive the mod
        const auto made = new Logic::ScriptFile(Logic::MakeScriptFile(file.name, *fixed));
        file.buffer = reinterpret_cast<const char*>(made->buffer.data());
        file.len = made->length;
    }

    std::vector<Logic::Area> GetAreas()
    {
        std::lock_guard lock(mapMutex);
        return map && map->finished ? map->rewrite.areas : std::vector<Logic::Area>{};
    }

    std::shared_ptr<const Types::LevelAreas> GetLevelAreas()
    {
        return levelAreas.load();
    }

    void NotifyClientScriptsOfRewind()
    {
        // what the game does with a clientNotify: clientscripts/mp/_callbacks::level_notify in the client scripts'
        // instance, its handle in .bss where the client's load keeps it (0 until then)
        constexpr std::int32_t CLIENT_SCRIPTS = 1;
        const auto levelNotify = *reinterpret_cast<const std::int32_t*>(0xD54F54);
        if (!levelNotify)
            return;
        const auto& addresses = GetGameAddresses();
        reinterpret_cast<void(__cdecl*)(const char*, std::int32_t)>(addresses.Scr_AddString())(Logic::CLIENT_REWIND_NOTIFY,
                                                                                              CLIENT_SCRIPTS);
        const auto thread =
            reinterpret_cast<std::uint16_t(__cdecl*)(std::int32_t, std::int32_t, std::uint32_t)>(0x69BB00)(CLIENT_SCRIPTS,
                                                                                                          levelNotify, 1);
        reinterpret_cast<void(__cdecl*)(std::uint32_t, std::int32_t)>(0x5C17C0)(thread, CLIENT_SCRIPTS);
    }

    void PlayMapEffects()
    {
        const Logic::Level* level = nullptr;
        std::uint64_t load = 0;
        {
            std::lock_guard lock(mapMutex);
            if (map && map->finished)
            {
                level = map->level;
                load = map->load;
            }
        }
        const auto cg = GetClientGlobals();
        std::lock_guard lock(mapEffectsMutex);
        // (another map: the client's effects went with the one before)
        if (!level || level->effects.empty())
        {
            mapEffects = {};
            return;
        }
        if (!cg || cg->time <= 0 || !*reinterpret_cast<void**>(GetGameAddresses().fx_system()))
            return;
        const auto now = cg->time;
        auto& play = mapEffects;

        bool start = play.level != level || play.load != load;
        const auto checking = std::chrono::steady_clock::now() - play.checkedAt >= MAP_EFFECTS_CHECK;
        if (!start && checking)
        {
            // what plays now, by kind; all that were kept gone: the client's effects were cleared
            play.checkedAt = std::chrono::steady_clock::now();
            std::ranges::fill(play.alive, 0u);
            std::size_t kept = 0, keptAlive = 0;
            for (std::size_t i = 0; i < play.played.size(); ++i)
            {
                const bool alive = IsEffectAlive(play.played[i].handle);
                play.alive[level->effects[i].kind] += alive ? 1 : 0;
                kept += play.played[i].kept ? 1 : 0;
                keptAlive += play.played[i].kept && alive ? 1 : 0;
            }
            if (kept > 0 && keptAlive == 0)
            {
                LOG_INFO("The effects of {} were all gone (the client's were cleared): they spawn again", level->name);
                start = true;
            }
        }
        if (start)
        {
            play = {};
            play.level = level;
            play.load = load;
            play.startedAt = now;
            play.played.resize(level->effects.size());
            play.alive.resize(level->effectKinds.size());
            for (const auto& definition : level->effectKinds)
                play.definitions.push_back(FindRegistered(ASSET_TYPE_FX, definition.c_str()));
        }

        for (std::size_t i = 0; i < level->effects.size(); ++i)
        {
            const auto& effect = level->effects[i];
            auto& played = play.played[i];
            if (!IsMapEffectShown(play, effect))
            {
                // hidden: what it plays ends at once, and it starts over when shown again
                if (played.handle)
                    Functions::FX_KillEffect(0, played.handle);
                played = {};
                continue;
            }
            if (!played.spawned)
            {
                // as the map starts, or as it is shown again: as if the map had started then
                SpawnMapEffect(play, i, Logic::GetEffectBegin(effect, now), now);
                continue;
            }
            if (!effect.loop)
                continue;
            // (a rewind to before its last play: counted from now)
            if (now < played.spawnedAt)
                played.spawnedAt = now;
            if (checking || now - played.spawnedAt < 250)
                if (Logic::PlaysAgain(effect, now, played.spawnedAt, IsEffectAlive(played.handle)))
                    SpawnMapEffect(play, i, now, now);
        }
        if (start)
        {
            const auto missing = std::ranges::count(play.definitions, nullptr);
            LOG_INFO("The effects of {}: {} placed of {} definitions, {} spawned ({} kept){}", level->name,
                     level->effects.size(), level->effectKinds.size(),
                     std::ranges::count(play.played, true, &PlayedEffect::spawned),
                     std::ranges::count(play.played, true, &PlayedEffect::kept),
                     missing ? std::format(", {} definitions not registered", missing) : "");
        }
    }

    std::optional<Types::MapEffects> GetMapEffects()
    {
        std::lock_guard lock(mapEffectsMutex);
        const auto level = mapEffects.level;
        if (!level)
            return std::nullopt;
        Types::MapEffects effects{level->name};
        const auto hidden = hiddenMapEffects.find(level->name);
        for (const auto& row : Logic::ListEffectKinds(*level))
        {
            const auto& definition = level->effectKinds[row.kind];
            if (!mapEffects.definitions[row.kind])
                continue;
            effects.kinds.push_back({definition, std::string(row.name), row.placed, mapEffects.alive[row.kind],
                                     hidden == hiddenMapEffects.end() || !hidden->second.contains(definition)});
        }
        return effects;
    }

    void SetHiddenMapEffects(const std::string& level, const std::set<std::string>& definitions)
    {
        std::lock_guard lock(mapEffectsMutex);
        hiddenMapEffects[level] = definitions;
        // (counted again at the next frame)
        mapEffects.checkedAt = {};
    }

    std::string GetMissingMapRefusal(const std::string& map)
    {
        const auto level = catalog ? catalog->FindLevel(map) : nullptr;
        if (!level || FindZoneFile(level->name))
            return {};
        return Logic::GetMissingMapRefusal(*level);
    }

    std::optional<MapEffectsState> GetMapEffectsState()
    {
        std::lock_guard lock(mapEffectsMutex);
        if (!mapEffects.level)
            return std::nullopt;
        MapEffectsState state{mapEffects.level->name, static_cast<std::uint32_t>(mapEffects.level->effects.size())};
        state.startedAt = mapEffects.startedAt;
        for (std::size_t i = 0; i < mapEffects.played.size(); ++i)
        {
            const auto& played = mapEffects.played[i];
            state.spawned += played.spawned ? 1 : 0;
            state.kept += played.kept ? 1 : 0;
            state.alive += IsEffectAlive(played.handle) ? 1 : 0;
            state.missing += mapEffects.definitions[mapEffects.level->effects[i].kind] ? 0 : 1;
        }
        return state;
    }

    std::atomic<std::uint32_t> snapshotEntitiesMoved = 0;  // the most in a snapshot, for the harness

    std::uint32_t TakeSnapshotEntitiesMoved()
    {
        return snapshotEntitiesMoved.exchange(0);
    }

    void FixSnapshot(Structures::snapshot_t& snapshot)
    {
        const auto placed = levelOrigins.load();
        if (!placed)
            return;
        const glm::vec3 viewer(snapshot.ps.origin[0], snapshot.ps.origin[1], snapshot.ps.origin[2]);
        const auto count = std::min<std::size_t>(std::max(snapshot.numEntities, 0), std::size(snapshot.entities));
        std::uint32_t moved = 0;
        for (std::size_t i = 0; i < count; ++i)
        {
            auto& base = snapshot.entities[i].pos.trBase;
            const auto fixed = Logic::UnwrapOrigin(glm::vec3(base[0], base[1], base[2]), viewer, *placed);
            moved += fixed.x != base[0] || fixed.y != base[1] || fixed.z != base[2] ? 1 : 0;
            std::ranges::copy(std::array{fixed.x, fixed.y, fixed.z}, base);
        }
        snapshotEntitiesMoved = std::max(snapshotEntitiesMoved.load(), moved);
    }

    void SetRewriteOptions(Logic::RewriteOptions options)
    {
        std::lock_guard lock(mapMutex);
        rewriteOptions = std::move(options);
    }

    std::optional<State> GetState()
    {
        std::lock_guard lock(mapMutex);
        if (!map)
            return std::nullopt;
        return State{map->level->name,
                     map->finished,
                     map->kit,
                     map->result,
                     map->rewrite.kept,
                     map->rewrite.dropped,
                     map->rewrite.spawns,
                     map->rewrite.vehiclesKept,
                     map->rewrite.vehiclesLeftOut,
                     map->rewrite.lightsMadeSteady,
                     map->rewrite.doorPiecesLeftOut,
                     map->rewrite.doors,
                     static_cast<std::uint32_t>(map->scripts.size()),
                     map->rewrite.entities,
                     map->rewrite.spawnPoints,
                     map->kitNotes};
    }

    std::string GetError()
    {
        return error;
    }
}  // namespace IWXMVM::T5::CampaignMaps
