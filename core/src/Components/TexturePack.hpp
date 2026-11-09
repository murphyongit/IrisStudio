#pragma once

#include "RestartLogic.hpp"
#include "TextureLogic.hpp"

namespace IWXMVM::Components::TexturePack
{
    // Texture packs: DDS files named after the game's images, drawn in place of those images'
    // textures. The packs are in the mod's folder (textures\v1 built by Iris Studio, textures\imported\<name>, and
    // use.json, Iris Studio's switches). Once a demo's map is in, a worker matches the images the game has registered
    // with the packs (TextureLogic::Resolve), leaving out the groups Iris Studio turned off.
    //
    // As drawn, within a budget (AF1): the device's SetTexture (SurfaceDepth's hook) hands each texture the game sets
    // to Swap. The first time an image of the pack is drawn, the worker reads its file and the render thread makes it
    // a few a frame, in the card's memory only (D3DPOOL_DEFAULT, outside the game's 32-bit address space); until then
    // the game's own texture draws. Behind those, the rest of the map's pack is read ahead while the budget has room
    // (drawn first alone, a whole map took about 10 s to come in, the camera turning faster than that). The
    // budget is what the card had free at the map's load less a gigabyte; the least recently drawn go first to make
    // room. Before a device reset the textures are let go, and made again (drawn first, then read ahead).
    // Off (the Look tab's Textures: Game), nothing is swapped.

    // the folder of the packs (the mod's textures folder); the harness points it elsewhere
    std::filesystem::path GetFolder();
    void SetFolder(std::optional<std::filesystem::path> folder);
    // matched again at the next frame in a demo (the folder or the switches changed)
    void Reload();
    // whether a match is under way: the worker making it, or made and not yet taken by the render thread (the harness
    // waits for it to end before reading the heap, as a match reads every pack's index)
    bool IsMatching();

    // the Look tab's Textures: Pack, this PC's
    void SetOn(bool on);
    bool IsOn();

    // for the harness: a budget in place of the card's, a set of groups off in place of use.json's (none to go back)
    void SetBudgetOverride(std::optional<std::uint64_t> bytes);
    void SetGroupsOffOverride(std::optional<std::set<TextureLogic::Group>> groups);
    // for the harness, on the render thread: the images of the pack the last frame drew, only these swapped (none: all
    // of them), and the images whose texture the game shares with another image
    std::vector<std::string> GetDrawnImages();
    void SetSwappedImages(std::optional<std::set<std::string>> names);
    std::vector<std::vector<std::string>> GetSharedImages();

    // the texture to draw for one the game sets (on the render thread)
    IDirect3DBaseTexture9* Swap(IDirect3DBaseTexture9* texture);
    // on the render thread: the file an image of the map draws from (none: the game's), and a number that changes with
    // each match of the map with the packs
    std::filesystem::path FindFile(std::string_view image);
    std::uint32_t GetGeneration();
    // before the device resets: the pack's textures go, and are made again as drawn
    void OnReset();

    // listens for frames and demo loads, and starts the worker
    void Install();
    // before the mod is ejected, on the render thread: the worker stops and the textures go
    void Shutdown();

    struct Stats
    {
        std::uint32_t images = 0;     // of the pack matched on this map, its groups on
        std::uint32_t made = 0;       // on the card now
        std::uint32_t waiting = 0;    // asked of the worker, drawn or read ahead, not made yet
        std::uint32_t drawnLeft = 0;  // drawn, not made yet (of those the budget has room for)
        std::uint32_t aheadLeft = 0;  // not drawn, still to come by the read-ahead while the budget has room
        std::uint32_t failed = 0;     // whose file didn't load
        std::uint32_t refused = 0;    // that didn't fit the budget
        std::uint32_t evicted = 0;    // let go for others since the match
        std::uint32_t swaps = 0;      // textures swapped in the last frame
        std::uint32_t watched = 0;    // the game's textures of the images now, looked for
        std::uint32_t calls = 0;      // textures the game set in the last frame, swapped or not
        std::uint32_t madeLastFrame = 0;
        std::uint64_t inUse = 0;      // bytes on the card
        std::uint64_t budget = 0;
        std::uint64_t packBytes = 0;  // of the images matched, on disk
        std::uint64_t freeAtLoad = 0; // the card's free memory when matched (0 if it couldn't be read)
        float lastMakeMs = 0.0f;      // making textures in the last frame that made any
        float mostMakeMs = 0.0f;      // in a frame, since the match
        float meanWaitMs = 0.0f;      // from an image's first draw to its texture made, since the match
        float mostWaitMs = 0.0f;
        std::uint32_t drawnWaits = 0; // images drawn before their texture was made, since the match
        float matchMs = 0.0f;         // the last match, on the main thread
        std::uint32_t matches = 0;    // since the start
        std::array<std::uint32_t, 3> groups{};     // the images matched by group (groups off too)
        std::array<std::uint32_t, 3> groupsOff{};  // of those, left out
    };
    Stats GetStats();
    // what a restart of the renderer waits for, after a device reset let the textures go (RestartLogic); it doesn't
    // wait for images the budget has no room for
    RestartLogic::Load GetLoad();
    // what the Look tab's line says
    TextureLogic::Status GetStatus();
}  // namespace IWXMVM::Components::TexturePack
