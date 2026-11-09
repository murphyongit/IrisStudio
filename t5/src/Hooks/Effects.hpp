#pragma once

namespace IWXMVM::T5::Hooks::Effects
{
    // A rewind plays the demo again from its start, spawning its effects again (see Effects.cpp): OnRewind is called as
    // it begins, OnClientFrame once a client frame on the game's thread
    void OnRewind();
    void OnClientFrame();

    // The client's effects: MAX_EFFECTS of them, and while they are all in use FX_SpawnEffect spawns none (no blood,
    // muzzle flash or napalm plays)
    inline constexpr std::size_t MAX_EFFECTS = 1024;

    // How many of the client's effects are in use, and the effects most in use, for the harness. Main thread only.
    struct EffectCounts
    {
        std::size_t used = 0;
        std::vector<std::pair<std::string, std::size_t>> most;  // by name, most first
        std::uint32_t refused = 0;                              // as CountRefused
    };
    EffectCounts CountEffects();
    // how many of the game's effects are in use, as it keeps count (the first of CountEffects, for every shot)
    std::size_t CountEffectsInUse();

    // How many spawns were refused since the map loaded because the client's effects were all in use
    std::uint32_t CountRefused();

    // The looping effects kept track of, how many of them still play, and how many were forgotten once gone since the
    // map loaded, for the harness. Main thread only.
    struct LoopingCounts
    {
        std::size_t tracked = 0;
        std::size_t alive = 0;
        std::size_t forgotten = 0;
    };
    LoopingCounts CountLooping();
    // whether an effect playing loops (a trail, a light), till it is killed. Main thread only.
    bool IsLooping(std::int32_t handle);

#ifdef IWXMVM_T5_HARNESS
    // The arguments of the next effects spawned, for the harness's fxwatch (what the game passes FX_SpawnEffect: its
    // definition's name, and three floats where the fourth argument points, if it does)
    struct WatchedSpawn
    {
        std::array<std::uint32_t, 11> args{};
        std::string name;
        std::optional<std::array<float, 3>> pointed;
    };
    void WatchSpawns(int count);
    std::vector<WatchedSpawn> GetWatchedSpawns();
    // three floats at an address, if it can be read
    std::optional<std::array<float, 3>> Peek(std::uint32_t address);

    // How many effects whose names have a part in them spawned since the last reset, and how many didn't (with the
    // client's effects all in use, or for another reason)
    struct SpawnCounts
    {
        std::size_t spawned = 0;
        std::size_t refused = 0;
        std::size_t full = 0;  // of those refused, with the effects all in use
    };
    SpawnCounts CountSpawns(std::string_view part, bool reset);
#endif

    void Install();
}  // namespace IWXMVM::T5::Hooks::Effects
