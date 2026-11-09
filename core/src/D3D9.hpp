#pragma once

namespace IWXMVM::D3D9
{
    void Initialize();

    HWND FindWindowHandle();
    IDirect3DDevice9* GetDevice();
    IDirect3DTexture9* GetDepthTexture();
    bool CaptureBackBuffer(IDirect3DTexture9* texture);
    bool CreateTexture(IDirect3DTexture9*& texture, ImVec2 size);
    bool IsReshadePresent();
    // the device's resets (and creations) so far, which a restart of the renderer waits for (Components::RestartLogic)
    std::uint32_t GetResetCount();
    // where the game view is in the window's client area, for what is painted in it while the device resets
    void SetGameViewRect(const RECT& rect);
    // the window's picture, with the loading screen on it, as the game is asked to restart, painted back while the reset
    // leaves it black, and the screen's ring, painted between the frames, which stop for some seconds: where the editor
    // drew it last, in the client area (colors 0xRRGGBB). The picture says "Restarting the renderer": it's out of date
    // once the game has made its change
    void KeepWindowPicture();
    void ReleaseWindowPicture();
    void OnWindowPictureOutOfDate();
    void SetRestartRing(float x, float y, float radius, float thickness, std::uint32_t track, std::uint32_t arc);

    // File > Exit: has the render thread, which uses the device, let go of the UI and the graphics' resources at its
    // next EndScene (released from another thread, the driver could wait for it forever), after which the hooks only
    // pass through; false when it didn't in time
    bool ReleaseForUnload(std::chrono::milliseconds timeout);
}  // namespace IWXMVM::D3D9