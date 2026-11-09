#pragma once

namespace IWXMVM::T5::PoolGrowth
{
    // The image and model pools twice as big, so that a campaign level's world and the
    // actors' characters fit together. They grow once a game: the growth outlives the mod, and an injection after an
    // unload finds them grown.
    //
    // Before the hooks: whether they can grow (or have). False when the game's code isn't what the tables were made
    // from (a Plutonium update), which leaves the pools as they are.
    bool Install();
    // After the hooks: grows them on a thread of its own, once no thread of the game is in the way, which a zone's load
    // at the menu is for some seconds
    void StartGrowing();
    // Before the mod unloads: stops trying, and that thread with it
    void Stop();
    // Holds the caller until the pools have grown, at most for that long: whether they have. For a level's load, on the
    // main thread, so that they grow before the first map; nothing to wait for returns at once.
    bool WaitUntilGrown(std::chrono::milliseconds limit);
    // Whether they have, or will
    bool WillGrow();
    bool IsGrown();
    // The zones' names and flags (g_zoneInfo, wherever it is now, ZONE_INFO_SIZE bytes each, the first entry unused) and
    // how many entries it has; and how many zones may be loaded (32 unless the tables moved)
    std::uintptr_t GetZoneInfo();
    std::size_t GetZoneInfoCount();
    std::size_t GetMaxZones();
    // what the game's physical memory has left, its fixed 352 MB shared by the map's zones and the sound banks; a zone
    // that doesn't fit ends the game with "Out of memory error"
    std::uint32_t GetPhysicalMemoryFree();
    // how many blocks of the zones in the mod's own memory aren't aligned as the game's zone memory is, for the harness
    std::size_t CountUnalignedZoneBlocks();
    // for the harness: grows them now, in a game started without (launch.ps1 -NoPoolGrowth); what happened
    std::string GrowNow();

    // A pointer outside the pools that the renderer's streaming code was handed as an image or model (the mod's own
    // copies of models: magazines, spine rigs, heads), which it gives a reserved index
    struct Stray
    {
        std::uintptr_t entry = 0;
        std::uintptr_t caller = 0;  // where in the streaming code, last
        std::uint32_t times = 0;
        std::string name;  // of the model or image, when it reads as one
    };
    struct PoolState
    {
        std::int32_t type = 0;
        std::uint32_t count = 0;  // entries the pool has
        std::uint32_t strays = 0;
        std::vector<Stray> strayEntries;  // of the last ones, by their address
        std::uintptr_t extension = 0, extensionEnd = 0;  // the new entries
    };
    struct State
    {
        bool grown = false;
        std::string outcome;  // what happened, for the log and the harness
        std::vector<PoolState> pools;
    };
    State GetState();

    // logs the pointers outside the pools that reached the streaming code since the last call. Main thread, each frame.
    void LogNewStrays();

    // For the harness: takes the free entries of the game's own array out of the pool's free list (or puts them back),
    // so what registers next gets new entries; how many. Main thread, while no zone loads.
    std::size_t HoldGameEntries(std::int32_t type, bool hold);
}  // namespace IWXMVM::T5::PoolGrowth
