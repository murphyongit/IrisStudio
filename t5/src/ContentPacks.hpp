#pragma once

#include <set>
#include <shared_mutex>

#include "Structures.hpp"

namespace IWXMVM::T5::ContentPacks
{
    // What loading a pack did, for the log and the UI
    struct LoadResult
    {
        bool loaded = false;
        std::string error;
        std::vector<std::string> models;           // the character models it made available
        std::array<std::uint32_t, 64> kept{};      // assets registered, by type
        std::array<std::uint32_t, 64> skipped{};   // assets left out, by type
        std::array<std::uint32_t, 64> poolFull{};  // needed but left out since their pool was full
        std::array<std::uint32_t, 64> shared{};    // needed and already registered by another zone
        std::int64_t memoryBefore = 0, memorySurveyed = 0, memoryAfter = 0;  // the process's private bytes
        std::int64_t milliseconds = 0;                                       // how long it took
        std::uint32_t invalidNames = 0;      // what the survey found pointing at something else
        std::uint32_t registeredLate = 0;    // what the survey missed and the load found in use
        std::uint32_t releasedTextures = 0;  // of the images left out
        std::uint64_t releasedTextureBytes = 0;
        std::uint32_t releasedShaders = 0;  // shaders and vertex declarations of technique sets left out
        std::uint32_t keptShaders = 0;      // of those, the ones that technique sets kept draw with too
        std::uint32_t releasedWorldBuffers = 0;
        std::uint32_t replacedTechniqueSets = 0;  // of materials, by a plain one
        std::uint32_t addedArguments = 0;         // constants and samplers the plain ones needed
        std::uint32_t removedTechniques = 0;      // which models couldn't be drawn with
        std::vector<std::string> notes;
    };

    // Loads a single player or Zombies zone for its characters: only their models, materials, techniques and
    // images are added, the rest of the zone (its world, scripts, AI and sounds) stays out. Replaces the pack
    // loaded before. With animations, single player common's animations first (once), unless the animation store
    // has the animation pack (see AnimationStore). Runs on the main thread and takes a few seconds.
    LoadResult Load(const std::string& zone, bool animations = true);

    // Loads a character zone (made by tools/t5-characters from the game's own zones: a character's models and what
    // they draw with), all of which is kept, next to the pack and the other character zones. Runs on the main thread
    // and takes a second or so; nothing should draw the actors meanwhile (IsLoading).
    // With clientStarting, also while the client starts (a film's or game's weapons are registered then, before the
    // game counts as in a demo or game: see WeaponZones). A registered weapon's zone stays until the map goes.
    LoadResult LoadCharacterZone(const std::string& zone, bool clientStarting = false, bool registeredWeapon = false);
    // Unloads a character zone, with those that use its assets and those unloaded by the same flag (see
    // ContentPacks.cpp), and returns them all. Actors made of their models have to be released first. Main thread.
    std::vector<std::string> UnloadCharacterZone(const std::string& zone);
    // As a demo is started (its command, on the main thread): the character zones are unloaded, as they use assets of
    // the map's zones, which go with it
    void UnloadCharacterZonesForDemo();
    // Loads the character zones the scene's actors need (Components::Actors::GetWantedCharacterZones), one a frame,
    // with single player common's animations before the first unless the animation store has the animation pack (see
    // AnimationStore), and unloads those no actor needs, the least recently needed first, while there are more than
    // the budget (16 zones, 512 MB of textures); nothing while capturing. Main thread, each frame.
    void RunCharacterZoneRequests();
    // the budget, for the harness
    void SetCharacterZoneBudget(std::size_t zones, std::uint64_t textureBytes);
    // the free address space below which the zones no actor needs go whatever the budget (0: the default, 512 MB and a
    // largest block of half that), for the harness
    void SetAddressSpaceFloor(std::uint64_t bytes);
    void SetWeaponRoomKept(std::uint32_t room);
    // for the harness: the zones wanted aren't loaded while held (as if each took long), to see what waits for them
    void HoldCharacterZoneLoads(bool hold);
    // why the last character zone that didn't load didn't (until the next demo), empty for none
    std::string GetCharacterZoneError();
    struct CharacterZoneInfo
    {
        std::string name;
        std::int32_t flags = 0;              // it was loaded with
        std::vector<std::string> models;     // it registered or found registered
        std::uint64_t textureBytes = 0;      // of the images it registered, as they are now (mips stream in)
        std::vector<std::string> dependsOn;  // the character zones whose assets it uses
        bool usesPack = false;               // and the pack's
    };
    std::vector<CharacterZoneInfo> GetCharacterZones();
    // the zones the game has loaded, with their allocation flags, for the harness
    std::vector<std::pair<std::string, std::int32_t>> GetLoadedZones();
    // how many more assets of a type its pool has room for, for the harness. Main thread only.
    std::uint32_t CountFreeSlots(std::int32_t type);
    // how many registered assets of a type have their entry in a range of memory (the pools' new entries, see
    // PoolGrowth), for the harness. Main thread only.
    std::uint32_t CountRegisteredIn(std::int32_t type, std::uintptr_t begin, std::uintptr_t end);
    // the entries of the asset table of a type and name, stand-ins too: their zone, whether in use, and their header,
    // for the harness. Main thread only.
    std::vector<std::string> DescribeEntries(std::int32_t type, const std::string& name);

    // the asset of that type and name a loaded zone registered, or null
    void* FindAsset(std::int32_t type, const char* name);
    // whether the game can paint a face on a model, on this map (see AddFacePaint)
    bool CanHaveFacePaint(const Structures::XModel& model);
    // whether the game finds a zone: one of the mod's folder (character, weapon and sound zones), else zone/Common's
    bool HasGameZone(const std::string& zone);
    // whether a character zone (or a weapon's) didn't load, until the next demo (main thread)
    bool HasZoneFailed(const std::string& zone);
    // whether a character zone (or a weapon's) is loaded; any thread
    bool IsCharacterZoneLoaded(const std::string& zone);
    // the weapons (WeaponVariantDef) a weapon zone registered; main thread
    std::vector<void*> GetZoneWeapons(const std::string& zone);

    // The packs the game's files have: single player levels and Zombies maps
    struct PackInfo
    {
        std::string name;  // the zone
        std::string displayName;
        std::string group;
    };
    const std::vector<PackInfo>& GetAvailablePacks();

    // What the actors UI asked for: the pack is loaded at the start of the next main thread frame (RunRequestedLoad)
    // in place of the one before, unless it is loaded already
    void RequestLoad(const std::string& zone);
    void RunRequestedLoad();
    // As the mod unloads (T5Interface::PrepareUnload): no pack loads anymore, and the copies of the pack's headers and
    // the tables made for its materials are left in memory, as its zone points to them until the map changes (which
    // unloads it) or another pack loads. Main thread only.
    void Unload();

    // While a zone loads the client still draws frames (from inside the loader), with the asset table being changed
    // and the pack's models coming and going, so nothing should look them up or draw them then
    bool IsLoading();
    // What other threads than the game's hold while they read assets (the core's reload profiles, previews): owned
    // unless a pack is loading, and a pack doesn't start loading while it is held
    std::shared_lock<std::shared_mutex> TryReadAssets();

    struct PackState
    {
        std::string loaded;   // the pack, empty for none
        std::string loading;  // the pack asked for, until it has loaded
        std::string error;
        std::int32_t memoryMegabytes = 0;  // what the game uses after loading it
        std::vector<std::string> models;   // of the loaded pack
        std::uint32_t generation = 0;      // changes whenever the loaded assets do
    };
    PackState GetPackState();
    // what the last load did, for the harness
    LoadResult GetLastLoadResult();
    // the registered materials that lack a constant or sampler their technique set's shaders read, or whose tables
    // aren't in the order of their name hashes, which crashes the renderer, with what they lack; for the harness
    // (main thread)
    std::vector<std::string> FindIncompleteMaterials();
#ifdef IWXMVM_T5_HARNESS
    // zones the mod's lookup of the game's zone files says it hasn't (as a game folder without a DLC map), for the
    // harness's zonehide
    void HideZoneFiles(std::set<std::string> zones);
#endif

    void Install();
}  // namespace IWXMVM::T5::ContentPacks
