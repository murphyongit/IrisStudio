#pragma once

#include "RestartLogic.hpp"
#include "Types/NormalMap.hpp"

namespace IWXMVM::Components::SurfaceDepth
{
    // Surface depth and Bump strength in the game's drawing (the patch and the heights are
    // SurfaceLogic's). The device's SetPixelShader is hooked: with the setting on, a lit shader that reads a normal map
    // is swapped for its patched copy (made in the background the first time it's drawn), and the height of the normal
    // map it reads is bound to a sampler the shader leaves free. Off, the game's shaders draw as they are: its exact
    // pixels. Only the map's own normal maps (the map and its props) get the strength, and only the surfaces that take
    // depth get the depth (SurfaceLogic::TakesDepth, GetDepthShare); everything else draws by the game's own shader.
    //
    // Textures on Pack, the heights of a normal map the pack has are made from the pack's file (at most
    // MAX_PACK_HEIGHTS a side), which the worker reads: the pack's texture is on the card, where it can't be read. The
    // heights' textures are on the card too, and go before a device reset (OnReset).

    // the map's normal maps (the game's GetMapNormalMaps, read after each demo's load): another map's replace them,
    // with what was made for them
    void SetMapNormalMaps(std::vector<Types::NormalMap> normalMaps);

    // hooks the device's methods (from D3D9's hooks, with the device's table of methods), and listens for loads and
    // frames: once a frame on the render thread, the heights waiting for the device are made
    void Install(void** deviceMethods);

    // The Look tab's rows (UI::VisualsMenu sets them each frame): Surface depth On or Off; how deep the bottom of the
    // height is, in world units (an inch each), while On; the slopes of the normal maps times Bump strength (1 the
    // game's), On or Off. Off with the game's strength, the game's shaders draw.
    void SetOn(bool on);
    bool IsOn();
    void SetDepth(float depth);
    float GetDepth();
    void SetBumpStrength(float strength);
    float GetBumpStrength();

    // the flattest view the depth takes (SurfaceLogic::MIN_VIEW_SLOPE at least): flatter views shift no further
    void SetMinViewSlope(float slope);
    float GetMinViewSlope();
    // how much darker the deepest of the height is drawn (SurfaceLogic::DEFAULT_CAVITY)
    void SetCavity(float cavity);
    float GetCavity();
    // how dark the height's shadow falls (SurfaceLogic::DEFAULT_SHADOW), 0 none
    void SetShadow(float shadow);
    float GetShadow();

    // Sets a texture past the hook, for the mod's own drawing (Skybox's bake): neither swapped nor taken for one the
    // game set
    HRESULT SetTextureDirect(IDirect3DDevice9* device, DWORD stage, IDirect3DBaseTexture9* texture);

    // before the device resets, on the render thread: the heights on the card go, and come back from their files
    void OnReset(IDirect3DDevice9* device);
    // before the game makes a new device (a mod loaded with loadmod): what was made on the old one goes, as drawing
    // with it on the new one fails the game's Present. The map's normal maps come again with the next demo.
    void OnNewDevice();
    // before the mod is ejected, on the render thread: the workers stop (a thread running keeps the DLL loaded), what
    // was made goes, and the hooks only pass through
    void Shutdown(IDirect3DDevice9* device);

    struct Stats
    {
        std::uint32_t shaders = 0;        // lit shaders seen that read a normal map
        std::uint32_t patched = 0;        // of those, patched and made
        std::uint32_t withDepth = 0;      // of those, with depth
        std::uint32_t failed = 0;         // patches that didn't assemble or make
        std::uint32_t normalMaps = 0;     // the map's
        std::uint32_t heights = 0;        // of those, with their height made
        std::uint32_t unreadable = 0;     // of those, whose texture couldn't be read
        std::uint32_t streaming = 0;      // of those, waiting to be streamed in
        std::uint32_t withDepthMaps = 0;  // of those made, with a height on the card, and from the pack
        std::uint32_t fromPack = 0;
        std::uint64_t heightBytes = 0;    // on the card
        std::uint32_t patchedDraws = 0;   // shaders set patched in the last frame, and with the map's normal maps
        std::uint32_t mapDraws = 0;
    };
    Stats GetStats();
    // what a restart of the renderer waits for, after a device reset let the heights go (RestartLogic): the map's
    // heights, and the patches of the shaders drawn; none the game hasn't streamed in, which may be never
    RestartLogic::Load GetLoad();

    // for the harness, on the render thread: the map's normal maps that may take depth, and what each got
    struct HeightInfo
    {
        std::string name;
        std::int32_t width = 0;
        std::int32_t height = 0;
        float relief = 0.0f;
        float share = 0.0f;
        bool fromPack = false;
        bool made = false;
    };
    std::vector<HeightInfo> GetHeights();
}  // namespace IWXMVM::Components::SurfaceDepth
