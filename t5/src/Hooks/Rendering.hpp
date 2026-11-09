#pragma once

namespace IWXMVM::T5::Hooks::Rendering
{
    void Install();

    // renders at this size instead of the window's, or at the window's again for nullopt, holding the window at its
    // size meanwhile, and restarts the renderer; false in fullscreen, where it doesn't apply. The vid_restart is
    // queued from the main thread, as the command buffer belongs to it.
    bool SetRenderSizeOverride(std::optional<std::pair<std::int32_t, std::int32_t>> size, HWND window);

    // The size of the sun's shadow map: each of its two halves is that square (the game's 1,024).
    // The renderer reads it as it makes its render targets, so a new size comes in with the next device reset:
    // with restart, SetSunShadowMapSize queues one (vid_restart) when the map isn't that size, else it waits for one that
    // comes anyway (a render size override's). GetSunShadowMapSize is the size the map has now.
    inline constexpr std::int32_t GAME_SUN_SHADOW_MAP_SIZE = 1024;
    void SetSunShadowMapSize(std::int32_t size, bool restart);
    std::int32_t GetSunShadowMapSize();
    // called on the main thread every frame
    void RunMainThreadFrame();
    // how many frames the main thread has run (RunMainThreadFrame), which it doesn't while it loads
    std::uint32_t GetMainThreadFrames();
    // Runs an action at the start of the main thread's next frame and waits for it; false (and it doesn't run) when the
    // main thread didn't get to it in time
    bool RunOnMainThread(std::function<void()> action, std::chrono::milliseconds timeout);

#ifdef IWXMVM_T5_HARNESS
    // records the sorted indices of the materials drawn from now on, or stops
    void RecordDrawnMaterials(bool record);
    std::vector<std::uint32_t> GetDrawnMaterials();
    // 0 world, 1 player, 2 every pass, -1 by name again
    void OverrideMaterialKind(std::uint32_t sortedIndex, int kind);
#endif
}  // namespace IWXMVM::T5::Hooks::Rendering
