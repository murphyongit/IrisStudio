#pragma once
#include "Structures.hpp"

namespace IWXMVM::T5::AnimationStore
{
    // The animation store (step 6): any animation of the catalog, loaded from the
    // animation pack (storage/t5/iris/animations.pack, which tools/t5-characters/animations.py writes) into memory of
    // the mod's own as an actor plays it. Each is laid out as the game's loader lays out an XAnimParts
    // (AnimationPackLogic), its bone and notetrack names made the game's script strings, and actors' trees take it as
    // they take the game's own (see Actors.cpp), so any animation plays on any actor it fits without the game's
    // XAnimParts pool, its zone slots or single player common. Each actor that plays an animation holds a reference to
    // it, which keeps it loaded; the others go, the least recently used first, once all of them take more than the
    // budget, except those the scene's clips name and anything while capturing; everything goes as a demo loads. What
    // goes is freed a few frames later, when no thread of the game can be working out a skeleton with it anymore.

    struct Loaded;

    // Keeps an animation of the store loaded while it is held; empty for none
    class Reference
    {
       public:
        Reference() = default;
        explicit Reference(Loaded* loaded);
        Reference(Reference&& other) noexcept;
        Reference& operator=(Reference&& other) noexcept;
        Reference(const Reference&) = delete;
        Reference& operator=(const Reference&) = delete;
        ~Reference();

        const Structures::XAnimParts* Parts() const;
        // the catalog's name of the animation
        std::string_view Key() const;
        explicit operator bool() const
        {
            return loaded != nullptr;
        }

       private:
        void Drop();
        Loaded* loaded = nullptr;
    };

    // Opens the pack and reads its index (again when its file changed), checking each animation is the catalog's (its
    // hash): as the mod starts and as each demo loads. Main thread.
    void Open();
    // whether the pack can be loaded from, and whether it has an animation the catalog lists (by the catalog's name)
    bool IsReady();
    bool Has(std::string_view key);
    // why the pack can't be loaded from, empty when it can
    std::string GetError();
    // changes when the store becomes ready or not, so the game's catalog of animations is made again
    std::uint32_t GetGeneration();

    // The animation an actor's tree takes by the catalog's name: the game's own as it has it registered (no reference),
    // else the store's, loaded if need be, with a reference; a copy that differs from the game's ("name (zone)") is
    // always the store's. Null parts for one neither has. Main thread.
    struct Found
    {
        const Structures::XAnimParts* parts = nullptr;
        Reference stored;  // the store's, if it is
    };
    Found Find(const std::string& name);
    // The same from any thread without loading (for decoding: the caller holds ContentPacks::TryReadAssets): the
    // store's only if it has it loaded, and if it hasn't, it is loaded in the main thread's next frame
    Found FindLoaded(const std::string& name);
    // The store's copy alone, loaded if need be (main thread; for the harness)
    Reference Acquire(const std::string& key, std::string* why = nullptr);

    // Each frame of a demo, on the main thread: loads the animations the scene's clips name (a few milliseconds' worth
    // a frame, all of them while capturing) and those asked for, unloads the least recently used past the budget (not
    // while capturing) and frees those unloaded a few frames ago
    void RunFrame(const std::vector<std::string>& wanted, bool capturing);
    // As a demo loads (the main thread, after the actors are released): every animation goes
    void Clear();
    // As the mod unloads: the game's trees may still have the animations, which are left to the process
    void Unload();

    // for the harness
    struct Stats
    {
        bool ready = false;
        std::string error;
        std::size_t packEntries = 0;
        std::size_t loaded = 0;
        std::uint64_t loadedBytes = 0;
        std::size_t referenced = 0;
        std::size_t wanted = 0;
        std::size_t retired = 0;  // unloaded, to be freed
        std::uint64_t retiredBytes = 0;
        std::uint64_t budget = 0;
        std::uint64_t loads = 0;
        double loadMilliseconds = 0.0;  // all of them together
        std::uint64_t unloads = 0;
        std::uint64_t frees = 0;
        std::uint64_t failures = 0;
        std::size_t scriptStrings = 0;
    };
    Stats GetStats();
    struct LoadedInfo
    {
        std::string key;
        std::uint32_t bytes = 0;
        std::uint32_t references = 0;
        bool wanted = false;
    };
    std::vector<LoadedInfo> GetLoaded();
    void SetBudget(std::uint64_t bytes);
    // the store's copy even of an animation the game has registered (to compare the two)
    void SetPreferStore(bool prefer);
    // as if there were no pack, from the next frame on
    void SetIgnored(bool ignore);
    // an entry of the pack, inflated; none when it isn't there or doesn't inflate
    std::optional<std::vector<std::uint8_t>> ReadEntry(const std::string& key);
    // the pack's names
    std::vector<std::string> GetKeys();
}  // namespace IWXMVM::T5::AnimationStore
