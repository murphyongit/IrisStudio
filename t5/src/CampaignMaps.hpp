#pragma once

#include "Components/CampaignMapLogic.hpp"
#include "ContentPacks.hpp"
#include "Structures.hpp"
#include "Types/LevelAreas.hpp"
#include "Types/MapEffects.hpp"

namespace IWXMVM::T5::CampaignMaps
{
    // Campaign levels and Zombies maps load with devmap as multiplayer maps, canvases for actors
    // (Components::CampaignMapLogic): of a level's zone (and of common_zombie before a
    // Zombies map) only its world is registered, as multiplayer's, with its entities rewritten and stand-in scripts,
    // and the players' kit zone of its teamset loads with it. Only with the pools grown (PoolGrowth), else the game's
    // own errors stand. zonesFolder is where the setup built the kits.
    void Install(const std::filesystem::path& zonesFolder);

    // Asked first by ContentPacks' hook of DB_AddXAsset, for an asset of a zone that is no pack's: what to give the
    // loader instead of registering it as the game would, when the zone is a level's, its kit's or common_zombie. On
    // the loader's thread.
    std::optional<void*> AddAsset(std::int32_t type, void* header);

    // the catalog of the levels (null while campaign maps don't load), and whether the game folder has a level's zone
    const Components::CampaignMapLogic::Catalog* GetCatalog();
    bool IsInGameFolder(const Components::CampaignMapLogic::Level& level);

    // Before a map or film of that map loads (the server's map command, a film's PLAY): what the file system mounts
    // for it as it starts again (single player's Rezurrection IWDs for Moon and the remakes)
    void PrepareMapLoad(const std::string& map);

    // Why a film recorded on that map won't play, in words, when the map is a campaign level or Zombies map whose zone
    // the game folder doesn't have (a DLC map it lacks): the game stops as it loads a zone it can't find. Empty else.
    std::string GetMissingMapRefusal(const std::string& map);

    // Once the level has loaded: releases what it loaded and left out (as ContentPacks does a pack's) and logs it;
    // forgets a level that has gone. Main thread, each frame.
    void FinishLoad();

    // As the mod unloads (ContentPacks::Unload): what was made for the level is left to the process, as its zone points
    // to it until the map changes. Main thread.
    void Unload();

    // the campaign map loading or loaded, for the harness
    struct State
    {
        std::string level;
        bool finished = false;
        std::string kit;  // the kit zone it loaded with, empty for none
        ContentPacks::LoadResult result;
        std::uint32_t entitiesKept = 0, entitiesDropped = 0, spawns = 0;
        std::uint32_t vehiclesKept = 0, vehiclesLeftOut = 0, lightsMadeSteady = 0, doorPiecesLeftOut = 0;
        std::vector<Components::CampaignMapLogic::Door> doors;
        std::uint32_t scripts = 0;  // stand-in scripts it registered
        std::string entities;       // what multiplayer got
        std::vector<glm::vec3> spawnPoints;
        std::vector<std::string> kitNotes;
    };
    std::optional<State> GetState();
    // Puts an incoming snapshot's entities back where they are on a level bigger than positions go over the network
    // (CampaignMapLogic::UnwrapOrigin). Before the client takes them in, in films too.
    void FixSnapshot(Structures::snapshot_t& snapshot);
    std::uint32_t TakeSnapshotEntitiesMoved();  // for the harness: the most it moved in a snapshot since last asked
    // what the levels' rewrites do otherwise from the next load on, for the harness
    void SetRewriteOptions(Components::CampaignMapLogic::RewriteOptions options);
    // why campaign maps don't load, empty when they do
    std::string GetError();

    // The level's ambient effects: its createfx's oneshots and loops, spawned as the
    // client's map starts and played on as the campaign's client script plays them, those of the definitions hidden
    // left out. Main thread, each client frame; the rest any thread.
    void PlayMapEffects();

    // A multiplayer map's client script as the mod changes it (Logic::FixClientScript), as its zone registers it
    void FixClientScript(std::int32_t type, void* header);
    // Tells the client scripts of a rewind, before it starts. Main thread.
    void NotifyClientScriptsOfRewind();

    // The parts of the level loaded, in the order a walk from its start meets them; none on
    // a multiplayer map
    std::vector<Components::CampaignMapLogic::Area> GetAreas();
    std::shared_ptr<const Types::LevelAreas> GetLevelAreas();
    std::optional<Types::MapEffects> GetMapEffects();
    void SetHiddenMapEffects(const std::string& level, const std::set<std::string>& definitions);
    // what the client plays of them now, for the harness: spawned since the map started, alive
    struct MapEffectsState
    {
        std::string level;
        std::uint32_t placed = 0, spawned = 0, alive = 0, kept = 0, missing = 0;
        std::int32_t startedAt = 0;  // the client's time
    };
    std::optional<MapEffectsState> GetMapEffectsState();
}  // namespace IWXMVM::T5::CampaignMaps
